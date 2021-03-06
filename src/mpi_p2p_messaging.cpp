/*
* Copyright (c) 2018, EPCC, The University of Edinburgh
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice, this
*    list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its
*    contributors may be used to endorse or promote products derived from
*    this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "mpi_p2p_messaging.h"
#include "misc.h"
#include "scheduler.h"
#include "metrics.h"
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <mutex>
#include <cstdlib>
#include <ctime>

#ifndef DO_METRICS
#define DO_METRICS false
#endif

#define MPI_TAG 16384
#define MPI_TERMINATION_TAG 16385
#define MPI_TERMINATION_CONFIRM_TAG 16386
#define SEND_PROGRESS_PERIOD 10
#define MAX_TERMINATION_COUNT 100

/**
* Initialises MPI if it has not already been initialised at serialised mode. If it has been initialised then checks which mode it is in to
* ensure compatability with what we are doing here
*/
MPI_P2P_Messaging::MPI_P2P_Messaging(Scheduler & a_scheduler, ThreadPool & a_threadPool, ContextManager& a_contextManager,
                                     Configuration & aconfig) : Messaging(a_scheduler, a_threadPool, a_contextManager, aconfig) {
  initialise(MPI_COMM_WORLD);
}

MPI_P2P_Messaging::MPI_P2P_Messaging(Scheduler & a_scheduler, ThreadPool & a_threadPool, ContextManager& a_contextManager,
                                     Configuration & aconfig, int mpi_communicator) : Messaging(a_scheduler, a_threadPool, a_contextManager, aconfig) {
  initialise(MPI_Comm_f2c(mpi_communicator));
}

/**
* Initialises the MPI transport layer with a specific communicator that all EDAT processes belong to. If MPI is already initialised this is fine,
* we just go with that if it is in serialised or thread multiple mode, otherwise we initialise MPI here too.
*/
void MPI_P2P_Messaging::initialise(MPI_Comm comm) {
  int is_mpi_init, provided;
  MPI_Initialized(&is_mpi_init);
  if (is_mpi_init) {
    mpiInitHere = false;
    MPI_Query_thread(&provided);
    if (provided != MPI_THREAD_MULTIPLE && provided != MPI_THREAD_SERIALIZED) {
      raiseError("You must initialise MPI in thread serialised or multiple, or let EDAT do this for you");
    }
    protectMPI = provided == MPI_THREAD_SERIALIZED;
  } else {
    mpiInitHere = true;
    MPI_Init_thread(NULL, NULL, MPI_THREAD_SERIALIZED, &provided);
    protectMPI = true;
  }
  communicator=comm;
  if (protectMPI) mpi_mutex.lock();
  MPI_Comm_rank(communicator, &my_rank);
  MPI_Comm_size(communicator, &total_ranks);
  std::srand(std::time(nullptr));
  if (protectMPI) mpi_mutex.unlock();
  if (my_rank == 0) {
    termination_codes=new int[total_ranks];
    pingback_termination_codes=new int[total_ranks];
    for (int i=0;i<total_ranks;i++) termination_codes[i]=-1;
  }
  terminated=false;
  eligable_for_termination=false;
  batchEvents=configuration.get("EDAT_BATCH_EVENTS", false);
  max_batched_events=configuration.get("EDAT_MAX_BATCHED_EVENTS", 1000);
  batch_timeout=configuration.get("EDAT_BATCHING_EVENTS_TIMEOUT", 0.1);
  enableBridge=configuration.get("EDAT_ENABLE_BRIDGE", false);
  if (doesProgressThreadExist()) startProgressThread();
}

/**
* Fires an event, either remote or local event. Also handles when we are sending to all targets rather than just
* one specific process
*/
void MPI_P2P_Messaging::fireEvent(void * data, int data_count, int data_type, int target, bool persistent, const char * event_id) {
  if (target == my_rank || target == EDAT_ALL) {
    int data_size=getTypeSize(data_type) * data_count;
    char * buffer_data=(char*) malloc(data_size);
    if (contextManager.isTypeAContext(data_type)) {
      // If its a context then pass the pointer to the context data rather than the data itself
      memcpy(buffer_data, &data, data_size);
    } else {
      memcpy(buffer_data, data, data_size);
    }
    SpecificEvent* event=new SpecificEvent(my_rank, data_count, data_count * getTypeSize(data_type), data_type, persistent,
                                           contextManager.isTypeAContext(data_type), std::string(event_id), (char*) buffer_data);
    scheduler.registerEvent(event);
  }
  if (target != my_rank) {
    if (target != EDAT_ALL) {
      sendSingleEvent(data, data_count, data_type, target, persistent, event_id);
    } else {
      for (int i=0;i<total_ranks;i++) {
        if (i != my_rank) {
          sendSingleEvent(data, data_count, data_type, i, persistent, event_id);
        }
      }
    }
  }
}

void MPI_P2P_Messaging::resetPolling() {
  mode=0;
  terminated_id=0;
  terminated=false;
  eligable_for_termination=false;
  if (my_rank == 0) {
    for (int i=0;i<total_ranks;i++) {
      termination_codes[i]=-1;
      pingback_termination_codes[i]=-1;
    }
  }
  Messaging::resetPolling();
}



/**
* Sends a single event to a specific target by packaging the data into a buffer and sending it over. We use a non-blocking synchronous send as we want acknowledgement
* from the target that the message has started to be received (for termination correctness.)
*/
void MPI_P2P_Messaging::sendSingleEvent(void * data, int data_count, int data_type, int target, bool persistent,
                                        const char * event_id) {
  int event_id_len=strlen(event_id);
  int type_element_size=getTypeSize(data_type);
  int packet_size=(type_element_size * data_count) + (sizeof(int) * 3) + event_id_len + 1 + sizeof(char);
  char * buffer = (char*) malloc(packet_size);
  memcpy(buffer, &data_type, sizeof(int));
  memcpy(&buffer[4], &my_rank, sizeof(int));
  memcpy(&buffer[8], &event_id_len, sizeof(int));
  char pVal=persistent ? 1 : 0;
  memcpy(&buffer[12], &pVal, sizeof(char));
  memcpy(&buffer[13], event_id, sizeof(char) * (event_id_len + 1));
  if (data != NULL) memcpy(&buffer[(13 + event_id_len + 1)], data, type_element_size * data_count);
  MPI_Request request;
  if (protectMPI) mpi_mutex.lock();
  MPI_Issend(buffer, packet_size, MPI_BYTE, target, MPI_TAG, communicator, &request);
  if (protectMPI) mpi_mutex.unlock();
  {
    std::lock_guard<std::mutex> out_sendReq_lock(outstandingSendRequests_mutex);
    outstandingSendRequests.insert(std::pair<MPI_Request, char*>(request, buffer));
  }
}

/**
* Locks the mutexes for testing for finalisation, this ensures whilst the finalisation test is going on there is no state change
*/
void MPI_P2P_Messaging::lockMutexForFinalisationTest() {
  dataArrival_mutex.lock();
  outstandingSendRequests_mutex.lock();
}

/**
* Unlocks the mutexes for finalisation testing
*/
void MPI_P2P_Messaging::unlockMutexForFinalisationTest() {
  dataArrival_mutex.unlock();
  outstandingSendRequests_mutex.unlock();
}

/**
* Determines whether the messaging is finished or not locally
*/
bool MPI_P2P_Messaging::isFinished() {
  int pending_message, global_pending_message;
  if (protectMPI) mpi_mutex.lock();
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_TAG, communicator, &pending_message, MPI_STATUS_IGNORE);
  if (enableBridge) {
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_TAG, MPI_COMM_WORLD, &global_pending_message, MPI_STATUS_IGNORE);
  } else {
    global_pending_message=0;
  }
  if (protectMPI) mpi_mutex.unlock();
  if (pending_message || global_pending_message) return false;
  return outstandingSendRequests.empty() && eventShortTermStore.empty();
}

/**
* Finalises the messaging - will force quit out of the polling loop (most likely this has already terminated as it has driven completion.) If MPI was initialised
* here then will finalise it
*/
void MPI_P2P_Messaging::finalise() {
  continue_polling=false;
  Messaging::finalise();
  if (mpiInitHere) MPI_Finalize();
}

/**
* Retrieves my rank
*/
int MPI_P2P_Messaging::getRank() {
  return my_rank;
}

/**
* Retrieves the overall number of ranks
*/
int MPI_P2P_Messaging::getNumRanks() {
  return total_ranks;
}

/**
* Checks the outstanding send requests for progress and will free the buffers of any that have been sent, this is just a clean up routine
*/
void MPI_P2P_Messaging::checkSendRequestsForProgress() {
  std::vector<MPI_Request> allreqHandles, storedReqHandles;
  std::lock_guard<std::mutex> out_sendReq_lock(outstandingSendRequests_mutex);
  for(auto imap : outstandingSendRequests) {
    allreqHandles.push_back(imap.first);
    // Needed as the MPI call resets the request back to null, hence we rely on the index and grab the origonal request based on this
    storedReqHandles.push_back(imap.first);
  }

  if (!allreqHandles.empty()) {
    MPI_Request * req_handles=(MPI_Request *) allreqHandles.data();
    int * returnIndicies=new int[allreqHandles.size()];
    int out_count;
    if (protectMPI) mpi_mutex.lock();
    MPI_Testsome(allreqHandles.size(), req_handles, &out_count, returnIndicies, MPI_STATUSES_IGNORE);
    if (protectMPI) mpi_mutex.unlock();
    if (out_count > 0) {
      for (int i=0;i<out_count;i++) {
        auto it = outstandingSendRequests.find(storedReqHandles[returnIndicies[i]]);
        if (it != outstandingSendRequests.end()) {
          free(it->second);
          outstandingSendRequests.erase(it);
        }
      }
    }
    delete[] returnIndicies;
  }
}

/**
* Locks MPI communications, this is needed if MPI is running in serialised mode (rather than multiple) which can be selected
* for performance as the implementation of MPI thread multiple is often poor
*/
void MPI_P2P_Messaging::lockComms() {
  mpi_mutex.lock();
}

/**
* Unlocks MPI communications
*/
void MPI_P2P_Messaging::unlockComms() {
  mpi_mutex.unlock();
}

/**
* Handles the arrival of some message (driven by the MPI status which we likely got from a probe and the appropriate
* communicator)
*/
void MPI_P2P_Messaging::handleRemoteMessageArrival(MPI_Status message_status, MPI_Comm comm_to_use) {
  char* buffer, *data_buffer;
  int message_size;
  #if DO_METRICS
    unsigned long int timer_key_pm = metrics::METRICS->timerStart("pending_message");
  #endif
  terminated=false;
  if (protectMPI) mpi_mutex.lock();
  MPI_Get_count(&message_status, MPI_BYTE, &message_size);
  buffer = (char*)malloc(message_size);
  MPI_Recv(buffer, message_size, MPI_BYTE, message_status.MPI_SOURCE, MPI_TAG, comm_to_use, MPI_STATUS_IGNORE);
  if (protectMPI) mpi_mutex.unlock();
  int data_type = *((int*)buffer);
  int source_pid = *((int*)&buffer[4]);
  char persistent=*((char*)&buffer[12]);
  int event_id_length = strlen(&buffer[13]);
  int data_size = message_size - (13 + event_id_length + 1);
  if (data_size > 0) {
    data_buffer = (char*)malloc(data_size);
    memcpy(data_buffer, &buffer[13 + event_id_length + 1], data_size);
  } else {
    data_buffer = NULL;
  }
  SpecificEvent* event=new SpecificEvent(source_pid, data_size > 0 ? data_size / getTypeSize(data_type) : 0, data_size, data_type,
                                          persistent == 1 ? true : false, contextManager.isTypeAContext(data_type), std::string(&buffer[13]), data_buffer);
  if (batchEvents) {
    last_event_arrival=MPI_Wtime();
    eventShortTermStore.push_back(event);
    if (eventShortTermStore.size() >= max_batched_events) {
      scheduler.registerEvents(eventShortTermStore);
      eventShortTermStore.clear();
    }
  } else {
    scheduler.registerEvent(event);
  }
  free(buffer);
  #if DO_METRICS
    metrics::METRICS->timerStop("pending_message", timer_key_pm);
  #endif
}

/**
* The main polling functionality, this will go through and fire a local event, then every so often will check for sending of event progress (request handles.) It then
* will check for any messages pending, if there is one then this is received and marshalled/decoded into an event before being registered with the scheduler.
* If there are no outstanding messages, then we might be in a local termination criteria - check if so and handle. Regardless progress termination protocol.
* This only performs one "tick" through, so is called repeatedly by a progress thread of an idle thread if there is none
*/
bool MPI_P2P_Messaging::performSinglePoll(int * iteration_counter) {
  #if DO_METRICS
    unsigned long int timer_key_psp = metrics::METRICS->timerStart("performSinglePoll");
  #endif
  int pending_message, global_pending_message;
  MPI_Status message_status, message_status_global;

  fireASingleLocalEvent();
  if (*iteration_counter == SEND_PROGRESS_PERIOD) {
    checkSendRequestsForProgress();
    *iteration_counter=0;
  } else {
    (*iteration_counter)++;
  }
  std::unique_lock<std::mutex> dataArrivalLock(dataArrival_mutex);
  if (protectMPI) mpi_mutex.lock();
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_TAG, communicator, &pending_message, &message_status);
  if (enableBridge) {
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_TAG, MPI_COMM_WORLD, &global_pending_message, &message_status_global);
  } else {
    global_pending_message=0;
  }
  if (protectMPI) mpi_mutex.unlock();
  if (pending_message) handleRemoteMessageArrival(message_status, communicator);
  if (global_pending_message) handleRemoteMessageArrival(message_status_global, MPI_COMM_WORLD);
  dataArrivalLock.unlock();

  if (!pending_message && !global_pending_message) {
    if (batchEvents && !eventShortTermStore.empty() && MPI_Wtime() - last_event_arrival > batch_timeout) {
      scheduler.registerEvents(eventShortTermStore);
      eventShortTermStore.clear();
    }
    bool current_terminated=checkForLocalTermination();
    if (current_terminated && !terminated) {
      terminated_id=std::rand();
      if (my_rank != 0) {
        if (protectMPI) mpi_mutex.lock();
        int sendTerminationIdFlag=(terminate_send_req == MPI_REQUEST_NULL);
        if (!sendTerminationIdFlag) MPI_Test(&terminate_send_req, &sendTerminationIdFlag, MPI_STATUS_IGNORE);
        if (sendTerminationIdFlag) MPI_Isend(&terminated_id, 1, MPI_INT, 0, MPI_TERMINATION_TAG, communicator, &terminate_send_req);
        if (termination_pingback_request == MPI_REQUEST_NULL) {
          MPI_Irecv(NULL, 0, MPI_INT, 0, MPI_TERMINATION_TAG, communicator, &termination_pingback_request);
        }
        if (protectMPI) mpi_mutex.unlock();
      }
    }
    terminated=current_terminated;
  }
  if (my_rank == 0) termination_codes[0]=terminated ? terminated_id : -1;
  #if DO_METRICS
    metrics::METRICS->timerStop("performSinglePoll", timer_key_psp);
  #endif
  return eligable_for_termination ? handleTerminationProtocol() : true;
}

/**
* Runs the poll for events from within a progress thread (the thread calls this procedure.) It will loop round and call the polling function
* whilst there is progress to be made.
*/
void MPI_P2P_Messaging::runPollForEvents() {
  int iteration_counter=0;
  while (continue_polling) {
    continue_polling=performSinglePoll(&iteration_counter);
  }
}

/**
* Handles the termination protocol, we have a process (0) as the master and others as workers. This is needed as we can tell if I am
* idle myself (terminated), but that doesn't mean that I won't reactivate with another event. Hence we need to only terminate when all
* ranks are completed. To check this, when ranks individually terminate they generate a random ID (which changes each time they reactivate
* and re-terminate.) These are then sent to the master, which stores them and when ids have been received from each rank then it sends a command
* to ping-back the latest termination ID (or -1 if it is currently active again.) If all these latest IDs match the previous IDs for each worker
* then we assume the system is in a steady state and hence should terminate. If the ids are different then you go back to the first stage of gathering
* termination IDs and pinging back.
*/
bool MPI_P2P_Messaging::handleTerminationProtocol() {
  #if DO_METRICS
    unsigned long int timer_key = metrics::METRICS->timerStart("handleTerminationProtocol");
  #endif
  if (my_rank == 0) {
    bool rt=true;
    if (mode == 0) trackTentativeTerminationCodes();
    if (mode == 1) rt=confirmTerminationCodes();
    #if DO_METRICS
      metrics::METRICS->timerStop("handleTerminationProtocol", timer_key);
    #endif
    return rt;
  } else {
    #if DO_METRICS
      metrics::METRICS->timerStop("handleTerminationProtocol", timer_key);
    #endif
    return handleTerminationProtocolMessagesAsWorker();
  }
}

/**
* Will handle the termination protocol as a worker (non-rank 0.) This involves two aspects, the first is waiting a ping back from
* the master to send the current termination id (or -1 if I am now active.) This ping back will only be requested when the master
* has been told from each rank that it is terminated, although obviously those ranks might now have progressed and be active again!
* The second is checking for a termination command from the master, this is after the ping back and the master will determine if
* all ranks are indeed completed or if it needs to crack on again
*/
bool MPI_P2P_Messaging::handleTerminationProtocolMessagesAsWorker() {
  if (termination_pingback_request != MPI_REQUEST_NULL) {
    int completed;
    if (protectMPI) mpi_mutex.lock();
    MPI_Test(&termination_pingback_request, &completed, MPI_STATUS_IGNORE);
    if (completed) {
      int not_completed=-1;
      if (terminate_send_pingback != MPI_REQUEST_NULL) {
        MPI_Cancel(&terminate_send_pingback);
        MPI_Wait(&terminate_send_pingback, MPI_STATUS_IGNORE);
      }
      // Send the master either my termination id or that I am active
      MPI_Isend(terminated ? &terminated_id : &not_completed, 1, MPI_INT, 0, MPI_TERMINATION_CONFIRM_TAG, communicator, &terminate_send_pingback);
      // Irrespective register the reply recieve which tells the worker whether it should terminate or not
      MPI_Irecv(&reply_from_master, 1, MPI_INT, 0, MPI_TERMINATION_CONFIRM_TAG, communicator, &termination_completed_request);
    }
    if (protectMPI) mpi_mutex.unlock();
  }
  if (termination_completed_request != MPI_REQUEST_NULL) {
    int completed;
    if (protectMPI) mpi_mutex.lock();
    MPI_Test(&termination_completed_request, &completed, MPI_STATUS_IGNORE);
    if (protectMPI) mpi_mutex.unlock();
    if (completed) {
      if (reply_from_master == 1) {
        // All finished
        return false;
      } else {
        // Not finished, need to restart
        if (termination_pingback_request == MPI_REQUEST_NULL) {
          // Reregister the ping-back recv if required
          if (protectMPI) mpi_mutex.lock();
          MPI_Irecv(NULL, 0, MPI_INT, 0, MPI_TERMINATION_TAG, communicator, &termination_pingback_request);
          if (protectMPI) mpi_mutex.unlock();
        }
      }
    }
  }
  return true;
}

/**
* On the master we confirm the termination codes, this is grabbing back the codes from each worker and then comparing them against the previous code
* if they all match then the system is in a steady state & terminate. Otherwise need to restart termination. Will tell each worker whether it should
* terminate or not depending on the values that the master receives.
*/
bool MPI_P2P_Messaging::confirmTerminationCodes() {
  int msg_pending, termination_command=0;
  bool updated=false;
  MPI_Status status;
  if (protectMPI) mpi_mutex.lock();
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_TERMINATION_CONFIRM_TAG, communicator, &msg_pending, &status);
  if (protectMPI) mpi_mutex.unlock();
  while (msg_pending) {
    updated=true;
    if (protectMPI) mpi_mutex.lock();
    MPI_Recv(&pingback_termination_codes[status.MPI_SOURCE], 1, MPI_INT, status.MPI_SOURCE, MPI_TERMINATION_CONFIRM_TAG, communicator, MPI_STATUS_IGNORE);
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_TERMINATION_CONFIRM_TAG, communicator, &msg_pending, &status);
    if (protectMPI) mpi_mutex.unlock();
  }
  if (!checkForCodeInList(pingback_termination_codes, -2)) {
    // All responses are in, now process
    if (!checkForCodeInList(pingback_termination_codes, -1)) {
      // All still termination
      if (compareTerminationRanks()) {
        // Terminated, all finished!
        termination_command=1;
      } else {
        // Not terminated as code is different, therefore go back and re-request ping back (update termination codes too!)
        termination_command=0;
        mode=0;
      }
    } else {
      // There is an active process
      termination_command=0;
      mode=0;
    }
    if (mode == 0) termination_codes[0]=pingback_termination_codes[0];
    if (protectMPI) mpi_mutex.lock();
    for (int i=1;i<total_ranks;i++) {
      MPI_Send(&termination_command, 1, MPI_INT, i, MPI_TERMINATION_CONFIRM_TAG, communicator);
      if (mode == 0) termination_codes[i]=pingback_termination_codes[i];
    }
    if (protectMPI) mpi_mutex.unlock();
  }
  return termination_command==0;
}

/**
* Tracks termination codes being sent to the master from other, worker processes. It will loop through and grab all the messages it can greedily. If this has then terminated
* it will check that all have terminated and if so will send the ping back command to all workers (to tell them to send their id or -1 if been reactivated.) We progress
* through greedily to grab these ids (irrespective if this master process is still active) in order to avoid lots of messages backing up if workers go from active to
* terminated continuously.
*/
void MPI_P2P_Messaging::trackTentativeTerminationCodes() {
  int msg_pending;
  bool updated=false;
  MPI_Status status;
  if (protectMPI) mpi_mutex.lock();
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_TERMINATION_TAG, communicator, &msg_pending, &status);
  if (protectMPI) mpi_mutex.unlock();
  while (msg_pending) {
    updated=true;
    if (protectMPI) mpi_mutex.lock();
    MPI_Recv(&termination_codes[status.MPI_SOURCE], 1, MPI_INT, status.MPI_SOURCE, MPI_TERMINATION_TAG, communicator, MPI_STATUS_IGNORE);
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_TERMINATION_TAG, communicator, &msg_pending, &status);
    if (protectMPI) mpi_mutex.unlock();
  }
  if (terminated && !checkForCodeInList(termination_codes, -1)) {
    mode=1;
    pingback_termination_codes[0]=terminated_id;
    for (int i=1;i<total_ranks;i++) pingback_termination_codes[i]=-2;
    if (protectMPI) mpi_mutex.lock();
    for (int i=1;i<total_ranks;i++) {
      MPI_Send(NULL, 0, MPI_INT, i, MPI_TERMINATION_TAG, communicator);
    }
    if (protectMPI) mpi_mutex.unlock();
  }
}

/**
* Compares the two termination lists, the ones we get as the code runs and the other the ping-back.
*/
bool MPI_P2P_Messaging::compareTerminationRanks() {
  for (int i=0;i<total_ranks;i++) {
    if (termination_codes[i] != pingback_termination_codes[i]) return false;
  }
  return true;
}

/**
* Checks whether a specific code is in a list of integers or not. This is useful for checking for
* termination sentinel or awaiting value
*/
bool MPI_P2P_Messaging::checkForCodeInList(int * codes_to_check, int failure_code) {
  for (int i=0;i<total_ranks;i++) {
    if (codes_to_check[i] == failure_code) return true;
  }
  return false;
}
