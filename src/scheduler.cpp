#include "scheduler.h"
#include "edat.h"
#include "threadpool.h"
#include "misc.h"
#include "metrics.h"
#include "resilience.h"
#include <map>
#include <string>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <queue>
#include <utility>
#include <set>
#include <fstream>

#ifndef DO_METRICS
#define DO_METRICS false
#endif

// static initilization for new_task_id
// TaskDescriptor::task_id = 0 is used to signify no task
static std::mutex task_id_mutex;
static taskID_t new_task_id = 1;

// serialization markers
static const char eod[4] = {'E', 'O', 'D', '\0'};
static const char eoo[4] = {'E', 'O', 'O', '\0'};
static const char eom[4] = {'E', 'O', 'M', '\0'};
static const char eov[4] = {'E', 'O', 'V', '\0'};
static const size_t marker_size = 4 * sizeof(char);

/**
* Deserialize constructor. Instantiates a SpecificEvent from an istream (an open binary file)
* and a streamposition (a valid file pointer to the start of the object).
*/
SpecificEvent::SpecificEvent(std::istream& file, const std::streampos object_begin) {
  char byte;
  int int_data[6];
  size_t id_length;
  std::streampos bookmark;
  bool end_of_string = false;

  file.seekg(object_begin);
  file.read(reinterpret_cast<char*>(int_data), sizeof(int_data));

  this->source_pid = int_data[0];
  this->message_length = int_data[1];
  this->raw_data_length = int_data[2];
  this->message_type = int_data[3];
  this->persistent = int_data[4] ? true : false;
  this->aContext = int_data[5] ? true : false;
  this->data = (char *) malloc(this->raw_data_length);
  file.read(data, this->raw_data_length);

  id_length = 0;
  bookmark = file.tellg();
  while (!end_of_string) {
    file.get(byte);
    if (byte == '\0') {
      id_length++;
      end_of_string = true;
    } else {
      id_length++;
    }
  }

  file.seekg(bookmark);
  char * memblock = new char[id_length];
  file.read(memblock, id_length);
  this->event_id = std::string(memblock);
  delete[] memblock;
}

/**
* Serializes a SpecificEvent to the supplied ostream at the supplied stream position.
* All the member ints and bools are serialized first as an int[], then the data as
* a char[], the end of the data is marked by EOD\0, then the event_id is serialised, and
* the end of the SpecificEvent marked by EOO\0
*/
void SpecificEvent::serialize(std::ostream& file, const std::streampos object_begin) const {
  int int_data[6] = {source_pid, message_length, raw_data_length, message_type, 0, 0};
  if (persistent) int_data[4] = 1;
  if (aContext) int_data[5] = 1;

  file.seekp(object_begin);

  file.write(reinterpret_cast<const char *>(int_data), sizeof(int_data));
  file.write(data, raw_data_length);
  file.write(event_id.c_str(), event_id.size()+1);

  return;
}

/**
* Serializes a SpecificEvent to the supplied ostream at the supplied stream position.
* All the member ints and bools are serialized first as an int[], then the data as
* a char[], the end of the data is marked by EOD\0, then the event_id is serialised, and
* the end of the SpecificEvent marked by EOO\0
*/
void SpecificEvent::serialize(std::ostream& file) const {
  int int_data[6] = {source_pid, message_length, raw_data_length, message_type, 0, 0};
  if (persistent) int_data[4] = 1;
  if (aContext) int_data[5] = 1;

  file.write(reinterpret_cast<const char *>(int_data), sizeof(int_data));
  file.write(data, raw_data_length);
  file.write(event_id.c_str(), event_id.size()+1);

  return;
}

HeldEvent::HeldEvent(const HeldEvent& src, int new_target) {
  this->file_pos = -1;
  this->target = new_target;
  this->state = HELD;
  this->spec_evt = new SpecificEvent(*(src.spec_evt), true);
}

HeldEvent::HeldEvent(std::istream& file, const std::streampos object_begin) {
  char marker_buf[4];

  this->file_pos = object_begin;
  file.seekg(object_begin);

  file.read(reinterpret_cast<char *>(&(this->state)), sizeof(HeldEventState));
  file.read(reinterpret_cast<char *>(&(this->target)), sizeof(int));

  this->spec_evt = new SpecificEvent(file, file.tellg());

  file.read(marker_buf, marker_size);
  if(strcmp(marker_buf, eoo)) raiseError("HeldEvent deserialization error, EOO not found");
}

void HeldEvent::serialize(std::ostream& file, const std::streampos object_begin) {
  file.seekp(object_begin);
  file_pos = object_begin;

  file.write(reinterpret_cast<const char *>(&state), sizeof(HeldEventState));
  file.write(reinterpret_cast<const char *>(&target), sizeof(int));
  spec_evt->serialize(file);
  file.write(eoo, marker_size);

  return;
}

void HeldEvent::serialize(std::ostream& file) {
  file_pos = file.tellp();

  file.write(reinterpret_cast<const char *>(&state), sizeof(HeldEventState));
  file.write(reinterpret_cast<const char *>(&target), sizeof(int));
  spec_evt->serialize(file);
  file.write(eoo, marker_size);

  return;
}

void HeldEvent::fire(Messaging& messaging) {
  messaging.fireEvent(spec_evt->getData(), spec_evt->getMessageLength(), spec_evt->getMessageType(), target, spec_evt->isPersistent(), spec_evt->getEventId().c_str());
  return;
}

bool HeldEvent::matchEventId(const std::string search_id) {
  if (!spec_evt->getEventId().compare(search_id)) {
    return true;
  } else {
    return false;
  }
}

/**
* Generates a unique identifier for each task, used by resilience to track
* which tasks are active, and store data for restart
*/
void TaskDescriptor::generateTaskID(void) {
  // we statically initialise task_id to 1, and use 0 for no task
  std::lock_guard<std::mutex> lock(task_id_mutex);
  task_id = new_task_id++;

  return;
}

void TaskDescriptor::resetTaskID(taskID_t old_task_id) {
  std::lock_guard<std::mutex> lock(task_id_mutex);
  new_task_id = ++old_task_id;
  return;
}

/**
* Deserialize constructor. Instantiates a PendingTaskDescriptor from an istream
* (an open binary file) and a streamposition (a valid file pointer to the start
* of the object).
*/
PendingTaskDescriptor::PendingTaskDescriptor(std::istream& file, const std::streampos object_begin) {
  char marker_buf[4], byte;
  int int_data[5], od_int;
  size_t task_name_length;
  std::streampos bookmark;
  bool end_of_string, found_eom, found_eov;

  file.seekg(object_begin);

  file.read(reinterpret_cast<char*>(&(this->task_id)), sizeof(taskID_t));
  file.read(reinterpret_cast<char*>(int_data), sizeof(int_data));

  this->func_id = int_data[0];
  this->numArrivedEvents = int_data[1];
  if(int_data[2]) this->freeData = true;
  if(int_data[3]) this->persistent = true;
  this->resilient = int_data[4];

  task_name_length = 0;
  bookmark = file.tellg();
  end_of_string = false;
  while(!end_of_string) {
    file.get(byte);
    if(byte == '\0') {
      task_name_length++;
      end_of_string = true;
    } else {
      task_name_length++;
    }
  }

  file.seekg(bookmark);
  char * memblock = new char[task_name_length];
  file.read(memblock, task_name_length);
  this->task_name = std::string(memblock);
  delete[] memblock;

  found_eom = false;
  while(!found_eom) {
    bookmark = file.tellg();
    file.read(marker_buf, marker_size);
    if(!strcmp(marker_buf, eom)) {
      found_eom = true;
    } else {
      DependencyKey depkey = DependencyKey(file, bookmark);

      file.read(reinterpret_cast<char *>(&od_int), sizeof(int));

      if (od_int > 0) this->outstandingDependencies.emplace(depkey, new int(od_int));
    }
  }

  found_eov = false;
  while(!found_eov) {
    bookmark = file.tellg();
    file.read(marker_buf, marker_size);
    if(!strcmp(marker_buf, eov)) {
      found_eov = true;
    } else {
      DependencyKey depkey = DependencyKey(file, bookmark);
      this->taskDependencyOrder.push_back(depkey);
    }
  }

  found_eom = false;
  while(!found_eom) {
    bookmark = file.tellg();
    file.read(marker_buf, marker_size);
    if(!strcmp(marker_buf, eom)) {
      found_eom = true;
    } else {
      DependencyKey depkey = DependencyKey(file, bookmark);

      file.read(reinterpret_cast<char *>(&od_int), sizeof(int));

      this->originalDependencies.emplace(depkey, new int(od_int));
    }
  }

  file.read(marker_buf, marker_size);
  if(strcmp(marker_buf, eoo)) raiseError("PendingTaskDescriptor deserialization error, EOO not found");
}

/**
* takes a deep copy of the supplied PendingTaskDescriptor and stores in this
*/
void PendingTaskDescriptor::deepCopy(PendingTaskDescriptor& src) {
  std::map<DependencyKey,int*>::const_iterator oDiter;
  std::map<DependencyKey,std::queue<SpecificEvent*>>::iterator aEiter;
  std::queue<SpecificEvent*> event_queue;
  SpecificEvent * spec_evt;
  unsigned int queue_size, i;

  for (oDiter = src.outstandingDependencies.begin(); oDiter != src.outstandingDependencies.end(); ++oDiter) {
    outstandingDependencies[oDiter->first] = new int(*(oDiter->second));
  }
  for (aEiter = src.arrivedEvents.begin(); aEiter != src.arrivedEvents.end(); ++aEiter) {
    queue_size = aEiter->second.size();
      if (queue_size > 1) {
        // queues aren't really meant to be iterated through, so this is a bit
        // messy...
        SpecificEvent* temp_queue[queue_size];
        i = 0;
        while (!aEiter->second.empty()) {
          // create copies of each specific event and push them to the new queue
          // also take note of the original
          spec_evt = new SpecificEvent(*(aEiter->second.front()), false);
          event_queue.push(spec_evt);
          temp_queue[i] = aEiter->second.front();
          aEiter->second.pop();
          i++;
        }
        for (i=0; i<queue_size; i++) {
          // now restore the original queue
          aEiter->second.push(temp_queue[i]);
        }
        arrivedEvents.emplace(aEiter->first,event_queue);
      } else {
        spec_evt = new SpecificEvent(*(aEiter->second.front()), false);
        arrivedEvents[aEiter->first].push(spec_evt);
      }
      while (!event_queue.empty()) event_queue.pop();
  }

  taskDependencyOrder = src.taskDependencyOrder;
  numArrivedEvents = src.numArrivedEvents;
  task_id = src.task_id;

  for (oDiter = src.originalDependencies.begin(); oDiter != src.originalDependencies.end(); ++oDiter) {
    originalDependencies[oDiter->first] = new int(*(oDiter->second));
  }

  freeData = src.freeData;
  persistent = src.persistent;
  func_id = src.func_id;
  task_name = src.task_name;
  task_fn = src.task_fn;
}

/*
* Serialization function. Writes PTD to file at given streampos, and leaves put
* pointer at end of object.
*/
void PendingTaskDescriptor::serialize(std::ostream& file, const std::streampos object_begin) {
  // serialization schema:
  // taskID_t task_id, int[5] {func_id, numArrivedEvents, freeData, persistent,
  // resilient}, char[] task_name : \0,
  // map<DependencyKey, int> outstandingDependencies : EOM,
  // vector<DependencyKey> taskDependencyOrder : EOV,
  // map<DependencyKey, int> originalDependencies : EOM, EOO
  std::map<DependencyKey, int*>::const_iterator od_iter;
  std::vector<DependencyKey>::const_iterator tdo_iter;

  int int_data[5] = {func_id, numArrivedEvents, 0, 0, resilient};
  if(freeData) int_data[2] = 1;
  if(persistent) int_data[3] = 1;

  file.seekp(object_begin);

  // int func_id, numArrivedEvents, freeData, persistent, resilient
  file.write(reinterpret_cast<const char *>(&task_id), sizeof(taskID_t));
  file.write(reinterpret_cast<const char *>(int_data), sizeof(int_data));
  file.write(task_name.c_str(), task_name.size()+1);

  // map<DependencyKey, int> outstandingDependencies
  for (od_iter = outstandingDependencies.begin(); od_iter != outstandingDependencies.end(); ++od_iter) {
    od_iter->first.serialize(file);
    file.write(reinterpret_cast<const char *>(od_iter->second), sizeof(int));
  }
  file.write(eom, marker_size);

  // vector<DependencyKey> taskDependencyOrder
  for(tdo_iter = taskDependencyOrder.begin(); tdo_iter != taskDependencyOrder.end(); ++tdo_iter) {
    tdo_iter->serialize(file);
  }
  file.write(eov, marker_size);

  // map<DependencyKey, int> originalDependencies
  for(od_iter = originalDependencies.begin(); od_iter != originalDependencies.end(); ++od_iter) {
    od_iter->first.serialize(file);
    file.write(reinterpret_cast<const char *>(od_iter->second), sizeof(int));
  }
  file.write(eom, marker_size);

  file.write(eoo, marker_size);

  return;
}

/*
* Serialization function. Writes PTD to file at given streampos, and leaves put
* pointer at end of object.
*/
void PendingTaskDescriptor::serialize(std::ostream& file) {
  // serialization schema:
  // taskID_t task_id, int[5] {func_id, numArrivedEvents, freeData, persistent,
  // resilient}, char[] task_name : \0,
  // map<DependencyKey, int> outstandingDependencies : EOM,
  // vector<DependencyKey> taskDependencyOrder : EOV,
  // map<DependencyKey, int> originalDependencies : EOM, EOO
  std::map<DependencyKey, int*>::const_iterator od_iter;
  std::vector<DependencyKey>::const_iterator tdo_iter;

  int int_data[5] = {func_id, numArrivedEvents, 0, 0, resilient};
  if(freeData) int_data[2] = 1;
  if(persistent) int_data[3] = 1;

  // int func_id, numArrivedEvents, freeData, persistent, resilient
  file.write(reinterpret_cast<const char *>(&task_id), sizeof(taskID_t));
  file.write(reinterpret_cast<const char *>(int_data), sizeof(int_data));
  file.write(task_name.c_str(), task_name.size()+1);

  // map<DependencyKey, int> outstandingDependencies
  for (od_iter = outstandingDependencies.begin(); od_iter != outstandingDependencies.end(); ++od_iter) {
    od_iter->first.serialize(file);
    file.write(reinterpret_cast<const char *>(od_iter->second), sizeof(int));
  }
  file.write(eom, marker_size);

  // vector<DependencyKey> taskDependencyOrder
  for(tdo_iter = taskDependencyOrder.begin(); tdo_iter != taskDependencyOrder.end(); ++tdo_iter) {
    tdo_iter->serialize(file);
  }
  file.write(eov, marker_size);

  // map<DependencyKey, int> originalDependencies
  for(od_iter = originalDependencies.begin(); od_iter != originalDependencies.end(); ++od_iter) {
    od_iter->first.serialize(file);
    file.write(reinterpret_cast<const char *>(od_iter->second), sizeof(int));
  }
  file.write(eom, marker_size);

  file.write(eoo, marker_size);

  return;
}

/**
* Constructs an ActiveTaskDescriptor by taking a deep copy of a
* PendingTaskDescriptor. ATDs are created immediately before a task is handed
* off to the threadpool.
*/
ActiveTaskDescriptor::ActiveTaskDescriptor(PendingTaskDescriptor& ptd) {
  deepCopy(ptd);
}

/**
* Destructor for ActiveTaskDescriptor. For every new a delete.
*/
ActiveTaskDescriptor::~ActiveTaskDescriptor() {
  std::map<DependencyKey,int*>::iterator oDiter;
  std::map<DependencyKey,std::queue<SpecificEvent*>>::iterator aEiter;

  for (oDiter = outstandingDependencies.begin(); oDiter != outstandingDependencies.end(); ++oDiter) {
    delete oDiter->second;
  }
  for (aEiter = arrivedEvents.begin(); aEiter != arrivedEvents.end(); ++aEiter) {
    while (!aEiter->second.empty()) {
      delete aEiter->second.front();
      aEiter->second.pop();
    }
  }
  for (oDiter = originalDependencies.begin(); oDiter != originalDependencies.end(); ++oDiter) {
    delete oDiter->second;
  }
}

/**
* Generates a PendingTaskDescriptor from the ATD for resubmission to the
* scheduler
*/
PendingTaskDescriptor* ActiveTaskDescriptor::generatePendingTask() {
  std::map<DependencyKey,int*>::const_iterator oDiter;
  std::map<DependencyKey,std::queue<SpecificEvent*>>::iterator aEiter;
  std::queue<SpecificEvent*> event_queue;
  SpecificEvent * spec_evt;
  unsigned int queue_size, i;
  PendingTaskDescriptor * ptd = new PendingTaskDescriptor();

  for (aEiter = arrivedEvents.begin(); aEiter != arrivedEvents.end(); ++aEiter) {
    queue_size = aEiter->second.size();
      if (queue_size > 1) {
        // queues aren't really meant to be iterated through, so this is a bit
        // messy...
        SpecificEvent* temp_queue[queue_size];
        i = 0;
        while (!aEiter->second.empty()) {
          // create copies of each specific event and push them to the new queue
          // also take note of the original
          spec_evt = new SpecificEvent(*(aEiter->second.front()));
          event_queue.push(spec_evt);
          temp_queue[i] = aEiter->second.front();
          aEiter->second.pop();
          i++;
        }
        for (i=0; i<queue_size; i++) {
          // now restore the original queue
          aEiter->second.push(temp_queue[i]);
        }
        ptd->arrivedEvents.emplace(aEiter->first,event_queue);
      } else {
        spec_evt = new SpecificEvent(*(aEiter->second.front()));
        ptd->arrivedEvents[aEiter->first].push(spec_evt);
      }
      while (!event_queue.empty()) event_queue.pop();
  }
  ptd->taskDependencyOrder = taskDependencyOrder;
  ptd->numArrivedEvents = numArrivedEvents;
  for (oDiter = originalDependencies.begin(); oDiter != originalDependencies.end(); ++oDiter) {
    ptd->originalDependencies[oDiter->first] = new int(*(oDiter->second));
  }
  ptd->freeData = freeData;
  ptd->persistent = persistent;
  ptd->resilient = resilient;
  ptd->task_name = task_name;
  ptd->task_fn = task_fn;

  return ptd;
}

/**
* Registers a task with EDAT, this will determine (and consume) outstanding events & then if applicable will mark ready for execution. Otherwise
* it will store the task in a scheduled state. Persistent tasks are duplicated if they are executed and the duplicate run to separate it from
* the stored version which will be updated by other events arriving.
*/
void Scheduler::registerTask(void (*task_fn)(EDAT_Event*, int), std::string task_name, std::vector<std::pair<int, std::string>> dependencies, bool persistent) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  PendingTaskDescriptor * pendingTask=new PendingTaskDescriptor();
  pendingTask->task_fn=task_fn;
  pendingTask->numArrivedEvents=0;
  pendingTask->freeData=true;
  pendingTask->persistent=persistent;
  pendingTask->task_name=task_name;
  for (std::pair<int, std::string> dependency : dependencies) {
    DependencyKey depKey = DependencyKey(dependency.second, dependency.first);
    pendingTask->taskDependencyOrder.push_back(depKey);
    std::map<DependencyKey, int*>::iterator oDit=pendingTask->originalDependencies.find(depKey);
    if (oDit != pendingTask->originalDependencies.end()) {
      (*(oDit->second))++;
    } else {
      pendingTask->originalDependencies.insert(std::pair<DependencyKey, int*>(depKey, new int(1)));
    }
    std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator it=outstandingEvents.find(depKey);
    if (it != outstandingEvents.end() && !it->second.empty()) {
      pendingTask->numArrivedEvents++;
      SpecificEvent * specificEVTToAdd;
      if (it->second.front()->isPersistent()) {
        // If its persistent event then copy the event
        specificEVTToAdd=new SpecificEvent(*(it->second.front()));
      } else {
        specificEVTToAdd=it->second.front();
        // If not persistent then remove from outstanding events
        outstandingEventsToHandle--;
        it->second.pop();
        if (it->second.empty()) outstandingEvents.erase(it);
      }

      std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator arrivedEventsIT = pendingTask->arrivedEvents.find(depKey);
      if (arrivedEventsIT == pendingTask->arrivedEvents.end()) {
        std::queue<SpecificEvent*> eventQueue;
        eventQueue.push(specificEVTToAdd);
        pendingTask->arrivedEvents.insert(std::pair<DependencyKey, std::queue<SpecificEvent*>>(depKey, eventQueue));
      } else {
        arrivedEventsIT->second.push(specificEVTToAdd);
      }
    } else {
      oDit=pendingTask->outstandingDependencies.find(depKey);
      if (oDit != pendingTask->outstandingDependencies.end()) {
        (*(oDit->second))++;
      } else {
        pendingTask->outstandingDependencies.insert(std::pair<DependencyKey, int*>(depKey, new int(1)));
      }
    }
  }

  if (resilienceLevel == 2) resilienceTaskScheduled(*pendingTask);

  if (pendingTask->outstandingDependencies.empty()) {
    PendingTaskDescriptor* exec_Task;
    if (persistent) {
      exec_Task=new PendingTaskDescriptor(*pendingTask);
      for (std::pair<DependencyKey, int*> dependency : pendingTask->originalDependencies) {
        pendingTask->outstandingDependencies.insert(std::pair<DependencyKey, int*>(dependency.first, new int(*(dependency.second))));
      }
      pendingTask->arrivedEvents.clear();
      pendingTask->numArrivedEvents=0;
      pendingTask->generateTaskID();
      registeredTasks.push_back(pendingTask);
      if (resilienceLevel == 2) resilienceTaskScheduled(*pendingTask);
    } else {
      exec_Task=pendingTask;
    }
    outstandTaskEvt_lock.unlock();
    readyToRunTask(exec_Task);
    consumeEventsByPersistentTasks();
  } else {
    registeredTasks.push_back(pendingTask);
  }
}

void Scheduler::registerTask(PendingTaskDescriptor * pending_task) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  registeredTasks.push_back(pending_task);
  return;
}

/**
* Pauses a specific task to be reactivated when the dependencies arrive. Will check to find whether any (all?) event dependencies have already arrived and if so then
* is a simple call back with these. Otherwise will call into the thread pool to pause the thread.
*/
EDAT_Event* Scheduler::pauseTask(std::vector<std::pair<int, std::string>> dependencies) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  PausedTaskDescriptor * pausedTask=new PausedTaskDescriptor();
  pausedTask->numArrivedEvents=0;
  for (std::pair<int, std::string> dependency : dependencies) {
    DependencyKey depKey = DependencyKey(dependency.second, dependency.first);
    pausedTask->taskDependencyOrder.push_back(depKey);

    std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator it=outstandingEvents.find(depKey);
    if (it != outstandingEvents.end() && !it->second.empty()) {
      pausedTask->numArrivedEvents++;
      SpecificEvent * specificEVTToAdd;
      if (it->second.front()->isPersistent()) {
        // If its persistent event then copy the event
        specificEVTToAdd=new SpecificEvent(*(it->second.front()));
      } else {
        specificEVTToAdd=it->second.front();
        // If not persistent then remove from outstanding events
        outstandingEventsToHandle--;
        it->second.pop();
        if (it->second.empty()) outstandingEvents.erase(it);
      }

      std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator arrivedEventsIT = pausedTask->arrivedEvents.find(depKey);
      if (arrivedEventsIT == pausedTask->arrivedEvents.end()) {
        std::queue<SpecificEvent*> eventQueue;
        eventQueue.push(specificEVTToAdd);
        pausedTask->arrivedEvents.insert(std::pair<DependencyKey, std::queue<SpecificEvent*>>(depKey, eventQueue));
      } else {
        arrivedEventsIT->second.push(specificEVTToAdd);
      }
    } else {
      std::map<DependencyKey, int*>::iterator oDit=pausedTask->outstandingDependencies.find(depKey);
      if (oDit != pausedTask->outstandingDependencies.end()) {
        (*(oDit->second))++;
      } else {
        pausedTask->outstandingDependencies.insert(std::pair<DependencyKey, int*>(depKey, new int(1)));
      }
    }
  }

  if (pausedTask->outstandingDependencies.empty()) {
    return generateEventsPayload(pausedTask, NULL);
  } else {
    pausedTasks.push_back(pausedTask);
    threadPool.pauseThread(pausedTask, &outstandTaskEvt_lock);
    return generateEventsPayload(pausedTask, NULL);
  }
}

/**
* Retrieves any events that match the provided dependencies, this allows picking off specific dependencies by a task without it having
* to endure the overhead of task restarting
*/
std::pair<int, EDAT_Event*> Scheduler::retrieveAnyMatchingEvents(std::vector<std::pair<int, std::string>> dependencies) {
  std::queue<SpecificEvent*> foundEvents;
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  for (std::pair<int, std::string> dependency : dependencies) {
    DependencyKey depKey = DependencyKey(dependency.second, dependency.first);
    std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator it=outstandingEvents.find(depKey);
    if (it != outstandingEvents.end() && !it->second.empty()) {
      if (it->second.front()->isPersistent()) {
        // If its persistent event then copy the event
        foundEvents.push(new SpecificEvent(*(it->second.front())));
      } else {
        foundEvents.push(it->second.front());
        // If not persistent then remove from outstanding events
        outstandingEventsToHandle--;
        it->second.pop();
        if (it->second.empty()) outstandingEvents.erase(it);
      }
    }
  }
  if (!foundEvents.empty()) {
    int num_found_events=foundEvents.size();
    EDAT_Event * events_payload = new EDAT_Event[num_found_events];
    for (int i=0;i<num_found_events;i++) {
      SpecificEvent * specEvent=foundEvents.front();
      foundEvents.pop();
      // Using a queue and iterating from the start guarantees event ordering
      generateEventPayload(specEvent, &events_payload[i]);
      delete specEvent;
    }
    return std::pair<int, EDAT_Event*>(num_found_events, events_payload);
  } else {
    return std::pair<int, EDAT_Event*>(0, NULL);
  }
}

/**
* Consumes events by persistent tasks, this is needed as lots of events can be stored and then when we register a persistent task we then want
* to consume all of these. But as we don't want to duplicate tasks internally (especially with lots of dependencies) then handle as tasks are queued for execution
* only. Hence we need to call this when a task is registered (might consume multiple outstanding events) or an event arrives (might fire a task which then
* unlocks consumption of other events.)
*/
void Scheduler::consumeEventsByPersistentTasks() {
  #if DO_METRICS
    unsigned long int timer_key = metrics::METRICS->timerStart("consumeEventsByPersistentTasks");
  #endif
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  bool consumingEvents=checkProgressPersistentTasks();
  while (consumingEvents) consumingEvents=checkProgressPersistentTasks();
  #if DO_METRICS
    metrics::METRICS->timerStop("consumeEventsByPersistentTasks", timer_key);
  #endif
}

/**
* Deschedules a task (removes it from the task list) based upon its name
*/
bool Scheduler::descheduleTask(std::string taskName) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  std::vector<PendingTaskDescriptor*>::iterator task_iterator=locatePendingTaskFromName(taskName);
  if (task_iterator != registeredTasks.end()) {
    registeredTasks.erase(task_iterator);
    return true;
  } else {
    return false;
  }
}

/**
* Determines whether a task is scheduled or not (based upon its name)
*/
bool Scheduler::isTaskScheduled(std::string taskName) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  std::vector<PendingTaskDescriptor*>::iterator task_iterator=locatePendingTaskFromName(taskName);
  return task_iterator != registeredTasks.end();
}

/**
* Returns an iterator to a specific task based on its name or the end of the vector if none is found
*/
std::vector<PendingTaskDescriptor*>::iterator Scheduler::locatePendingTaskFromName(std::string taskName) {
  std::vector<PendingTaskDescriptor*>::iterator it;
  for (it = registeredTasks.begin(); it < registeredTasks.end(); it++) {
    if (!(*it)->task_name.empty() && taskName == (*it)->task_name) return it;
  }
  return it;
}

/**
* Checks all persistent tasks for whether they can consume events, if so will do consumption and event better if we can execute some then this will do. Note that
* for each persistent task will only execute once (i.e. this might directly unlock the next iteration of that task which can comsume more and hence run itself.)
* If any tasks run then returns true, this means it is worth calling again to potentially execute further tasks.
*/
bool Scheduler::checkProgressPersistentTasks() {
  bool progress=false;
  for (PendingTaskDescriptor * pendingTask : registeredTasks) {
    if (pendingTask->persistent) {
      std::vector<DependencyKey> dependenciesToRemove;
      for (std::pair<DependencyKey, int*> dependency : pendingTask->outstandingDependencies) {
        std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator it=outstandingEvents.find(dependency.first);
        if (it != outstandingEvents.end() && !it->second.empty()) {
          pendingTask->numArrivedEvents++;
          SpecificEvent * specificEVTToAdd;
          if (it->second.front()->isPersistent()) {
            // If its persistent event then copy the event
            specificEVTToAdd=new SpecificEvent(*(it->second.front()));
          } else {
            specificEVTToAdd=it->second.front();
            // If not persistent then remove from outstanding events
            outstandingEventsToHandle--;
            it->second.pop();
            if (it->second.empty()) outstandingEvents.erase(it);
          }

          std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator arrivedEventsIT = pendingTask->arrivedEvents.find(dependency.first);
          if (arrivedEventsIT == pendingTask->arrivedEvents.end()) {
            std::queue<SpecificEvent*> eventQueue;
            eventQueue.push(specificEVTToAdd);
            pendingTask->arrivedEvents.insert(std::pair<DependencyKey, std::queue<SpecificEvent*>>(dependency.first, eventQueue));
          } else {
            arrivedEventsIT->second.push(specificEVTToAdd);
          }
          (*(dependency.second))--;
          if (*(dependency.second) <= 0) {
            dependenciesToRemove.push_back(dependency.first);
          }
        }
      }
      if (!dependenciesToRemove.empty()) {
        for (DependencyKey k : dependenciesToRemove) pendingTask->outstandingDependencies.erase(k);
      }
      if (pendingTask->outstandingDependencies.empty()) {
        PendingTaskDescriptor* exec_Task=new PendingTaskDescriptor(*pendingTask);
        for (std::pair<DependencyKey, int*> dependency : pendingTask->originalDependencies) {
          pendingTask->outstandingDependencies.insert(std::pair<DependencyKey, int*>(dependency.first, new int(*(dependency.second))));
        }
        pendingTask->arrivedEvents.clear();
        pendingTask->numArrivedEvents=0;
        pendingTask->generateTaskID();
        if (resilienceLevel == 2) resilienceTaskScheduled(*pendingTask);
        readyToRunTask(exec_Task);
        progress=true;
      }
    }
  }
  return progress;
}

/**
* Registers an event and will search through the registered and paused tasks to figure out if this can be consumed directly (which might then cause the
* task to execute/resume) or whether it needs to be stored as there is no scheduled task that can consume it currently.
*/
void Scheduler::registerEvent(SpecificEvent * event) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  if (resilienceLevel == 2) {
    if (!resilienceAddEvent(*event)) return;
  }
  std::pair<TaskDescriptor*, int> pendingEntry=findTaskMatchingEventAndUpdate(event);
  bool firstIt=true;

  while (pendingEntry.first != NULL && (event->isPersistent() || firstIt)) {
    if (pendingEntry.first->getDescriptorType() == PENDING) {
      PendingTaskDescriptor * pendingTask = (PendingTaskDescriptor*) pendingEntry.first;
      if (pendingTask->outstandingDependencies.empty()) {
        PendingTaskDescriptor* exec_Task;
        if (!pendingTask->persistent) {
          registeredTasks.erase(registeredTasks.begin() + pendingEntry.second);
          exec_Task=pendingTask;
        } else {
          exec_Task=new PendingTaskDescriptor(*pendingTask);
          for (std::pair<DependencyKey, int*> dependency : pendingTask->originalDependencies) {
            pendingTask->outstandingDependencies.insert(std::pair<DependencyKey, int*>(dependency.first, new int(*(dependency.second))));
          }
          pendingTask->arrivedEvents.clear();
          pendingTask->numArrivedEvents=0;
          pendingTask->generateTaskID();
          if (resilienceLevel == 2) resilienceTaskScheduled(*pendingTask);
        }
        outstandTaskEvt_lock.unlock();
        readyToRunTask(exec_Task);
        consumeEventsByPersistentTasks();
      }
    } else if (pendingEntry.first->getDescriptorType() == PAUSED) {
      PausedTaskDescriptor * pausedTask = (PausedTaskDescriptor*) pendingEntry.first;
      if (pausedTask->outstandingDependencies.empty()) {
        pausedTasks.erase(pausedTasks.begin() + pendingEntry.second);
        outstandTaskEvt_lock.unlock();
        threadPool.markThreadResume(pausedTask);
      }
    } else {
      raiseError("Task descriptor was not a pending or paused task");
    }
    if (event->isPersistent()) {
      if (!outstandTaskEvt_lock.owns_lock()) outstandTaskEvt_lock.lock();
      // If this is a persistent event keep trying to consume tasks to match against as many as possible
      pendingEntry=findTaskMatchingEventAndUpdate(event);
    } else {
      // If not a persistent task then the event has been consumed and don't do another iteration
      firstIt=false;
    }
  }

  if (pendingEntry.first == NULL) {
    // Will always hit here if the event is persistent as it consumes in the above loop until there are no more pending, matching tasks
    DependencyKey dK=DependencyKey(event->getEventId(), event->getSourcePid());
    std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator it = outstandingEvents.find(dK);
    if (it == outstandingEvents.end()) {
      std::queue<SpecificEvent*> eventQueue;
      eventQueue.push(event);
      outstandingEvents.insert(std::pair<DependencyKey, std::queue<SpecificEvent*>>(dK, eventQueue));
    } else {
      it->second.push(event);
    }

    if (!event->isPersistent()) outstandingEventsToHandle++;
  }
}

void Scheduler::registerEvent(std::pair<DependencyKey,std::queue<SpecificEvent*>> oe_entry) {
  std::unique_lock<std::mutex> outstandTaskEvt_lock(taskAndEvent_mutex);
  outstandingEvents.insert(oe_entry);
  if (!oe_entry.second.front()->isPersistent()) outstandingEventsToHandle += oe_entry.second.size();
  return;
}

/**
* Finds a task that depends on a specific event and updates the outstanding dependencies of that task to no longer be waiting for this
* and place this event in the arrived dependencies of that task. IT will return either the task itself (and index, as the task might be
* runnable hence we need to remove it) or NULL and -1 if no task was found. There is a priority given to scheduled tasks and then after this
* tasks that are paused and waiting for dependencies to resume.
*/
std::pair<TaskDescriptor*, int> Scheduler::findTaskMatchingEventAndUpdate(SpecificEvent * event) {
  DependencyKey eventDep = DependencyKey(event->getEventId(), event->getSourcePid());
  int i=0;
  std::map<DependencyKey, int*>::iterator it;
  for (PendingTaskDescriptor * pendingTask : registeredTasks) {
    it = pendingTask->outstandingDependencies.find(eventDep);
    if (it != pendingTask->outstandingDependencies.end()) {
      updateMatchingEventInTaskDescriptor(pendingTask, eventDep, it, event);
      return std::pair<TaskDescriptor*, int>(pendingTask, i);
    }
    i++;
  }

  i=0;
  for (PausedTaskDescriptor * pausedTask : pausedTasks) {
    it = pausedTask->outstandingDependencies.find(eventDep);
    if (it != pausedTask->outstandingDependencies.end()) {
      updateMatchingEventInTaskDescriptor(pausedTask, eventDep, it, event);
      return std::pair<TaskDescriptor*, int>(pausedTask, i);
    }
    i++;
  }
  return std::pair<TaskDescriptor*, int>(NULL, -1);
}

/**
* Updates the (found) matching event in the descriptor of the task to go from outstanding to arrived. If the event is persistent then this is a copy of the
* event, otherwise the event directly.
*/
void Scheduler::updateMatchingEventInTaskDescriptor(TaskDescriptor * taskDescriptor, DependencyKey eventDep,
                                                    std::map<DependencyKey, int*>::iterator it, SpecificEvent * event) {
  taskDescriptor->numArrivedEvents++;
  (*(it->second))--;
  if (*(it->second) <= 0) {
    taskDescriptor->outstandingDependencies.erase(it);
  }

  SpecificEvent * specificEVTToAdd;
  if (event->isPersistent()) {
    // If its persistent event then copy the event
    specificEVTToAdd=new SpecificEvent(*event);
    if (resilienceLevel == 2) resilienceAddEvent(*specificEVTToAdd);
  } else {
    specificEVTToAdd=event;
  }
  std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator arrivedEventsIT = taskDescriptor->arrivedEvents.find(eventDep);
  if (arrivedEventsIT == taskDescriptor->arrivedEvents.end()) {
    std::queue<SpecificEvent*> eventQueue;
    eventQueue.push(specificEVTToAdd);
    taskDescriptor->arrivedEvents.insert(std::pair<DependencyKey, std::queue<SpecificEvent*>>(eventDep, eventQueue));
  } else {
    arrivedEventsIT->second.push(specificEVTToAdd);
  }
  if (resilienceLevel == 2) resilienceMoveEventToTask(eventDep, taskDescriptor->task_id);
}

/**
* Marks that a specific task is ready to run. It will pass this onto the thread pool which will try and map this to a free thread if it can, otherwise if there are no idle threads
* then the thread pool will queue it up for execution when a thread becomes available.
*/
void Scheduler::readyToRunTask(PendingTaskDescriptor * taskDescriptor) {
  taskDescriptor->resilient = resilienceLevel;
  threadPool.startThread(threadBootstrapperFunction, taskDescriptor, taskDescriptor->task_id);
}

EDAT_Event * Scheduler::generateEventsPayload(TaskDescriptor * taskContainer, std::set<int> * eventsThatAreContexts) {
  EDAT_Event * events_payload = new EDAT_Event[taskContainer->numArrivedEvents];
  int i=0;
  for (DependencyKey dependencyKey : taskContainer->taskDependencyOrder) {
    // Pick them off this way to ensure ordering of dependencies wrt task definition
    std::map<DependencyKey, std::queue<SpecificEvent*>>::iterator arrivedEventsIT = taskContainer->arrivedEvents.find(dependencyKey);
    if (arrivedEventsIT == taskContainer->arrivedEvents.end()) {
      raiseError("Can not find the corresponding event dependency key when mapping the task onto a thread\n");
    }
    if (arrivedEventsIT->second.size() <=0) {
      raiseError("Too few events with a corresponding EID for when mapping the task onto a thread\n");
    }
    SpecificEvent * specEvent=arrivedEventsIT->second.front();
    arrivedEventsIT->second.pop();
    generateEventPayload(specEvent, &events_payload[i]);
    if (specEvent->isAContext() && eventsThatAreContexts != NULL) eventsThatAreContexts->emplace(i);
    i++;
  }
  return events_payload;
}

void Scheduler::generateEventPayload(SpecificEvent * specEvent, EDAT_Event * event) {
  if (specEvent->isAContext()) {
    // If its a context then de-reference the pointer to point to the memory directly and don't free the pointer (as would free the context!)
    event->data=*((char**) specEvent->getData());
  } else {
    event->data=specEvent->getData();
  }
  event->metadata.data_type=specEvent->getMessageType();
  if (event->metadata.data_type == EDAT_NOTYPE) {
    event->metadata.number_elements=0;
  } else {
    event->metadata.number_elements=specEvent->getMessageLength();
  }
  event->metadata.source=specEvent->getSourcePid();
  int event_id_len=specEvent->getEventId().size();
  char * event_id=(char*) malloc(event_id_len + 1);
  memcpy(event_id, specEvent->getEventId().c_str(), event_id_len+1);
  event->metadata.event_id=event_id;
}

/**
* This is the entry point for the thread to execute a task which is provided. In addition to the marshalling required to then call into the task
* with the correct arguments, it also frees the data at the end and will check the task queue. If there are outstanding tasks in the queue then these
* in tern will also be executed by this thread
*/
void Scheduler::threadBootstrapperFunction(void * pthreadRawData) {
  PendingTaskDescriptor * taskContainer=(PendingTaskDescriptor *) pthreadRawData;
  std::set<int> eventsThatAreContexts;
  const std::thread::id thread_id = std::this_thread::get_id();
  const int resilient = taskContainer->resilient;

  if (resilient) {
    resilienceTaskRunning(thread_id, *taskContainer, resilient);
  }

  EDAT_Event * events_payload = generateEventsPayload(taskContainer, &eventsThatAreContexts);
  taskContainer->task_fn(events_payload, taskContainer->numArrivedEvents);
  for (int j=0;j<taskContainer->numArrivedEvents;j++) {
    free(events_payload[j].metadata.event_id);
    if (taskContainer->freeData && events_payload[j].data != NULL && eventsThatAreContexts.count(j) == 0) free(events_payload[j].data);
  }

  if (resilient) {
    resilienceTaskCompleted(thread_id, taskContainer->task_id, resilient);
  }

  delete[] events_payload;
  delete taskContainer;
}

/**
* Determines whether the scheduler is finished or not
*/
bool Scheduler::isFinished() {
  std::lock_guard<std::mutex> lock(taskAndEvent_mutex);
  for (PendingTaskDescriptor * pendingTask : registeredTasks) {
    if (!pendingTask->persistent) return false;
  }
  return (outstandingEventsToHandle==0 && resilienceIsFinished(resilienceLevel));
}

void Scheduler::reset() {
  std::map<DependencyKey,std::queue<SpecificEvent*>>::iterator oe_iter;
  SpecificEvent * event;

  std::lock_guard<std::mutex> lock(taskAndEvent_mutex);

  while (!registeredTasks.empty()) {
    delete registeredTasks.back();
    registeredTasks.pop_back();
  }

  while (!pausedTasks.empty()) {
    delete pausedTasks.back();
    pausedTasks.pop_back();
  }

  while(!outstandingEvents.empty()) {
    oe_iter = outstandingEvents.begin();
    while (!oe_iter->second.empty()) {
      event = oe_iter->second.front();
      oe_iter->second.pop();
      delete event;
    }
    outstandingEvents.erase(oe_iter);
  }

  outstandingEventsToHandle = 0;

  return;
}
