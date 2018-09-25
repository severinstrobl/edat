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

#ifndef SRC_SCHEDULER_H_
#define SRC_SCHEDULER_H_

#include "edat.h"
#include "threadpool.h"
#include "configuration.h"
#include "concurrency_ctrl.h"
#include "misc.h"
#include <map>
#include <string>
#include <mutex>
#include <queue>
#include <utility>
#include <set>
#include <fstream>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long int taskID_t;

class SpecificEvent {
  int source_pid, message_length, raw_data_length, message_type;
  char* data;
  std::string event_id;
  bool persistent, aContext;
  std::streampos file_pos;

 public:
  SpecificEvent(int sourcePid, int message_length, int raw_data_length, int message_type, bool persistent, bool aContext, std::string event_id, char* data) {
    this->source_pid = sourcePid;
    this->message_type = message_type;
    this->raw_data_length = raw_data_length;
    this->event_id = event_id;
    this->message_length = message_length;
    this->data = data;
    this->persistent=persistent;
    this->aContext=aContext;
  }

  SpecificEvent(const SpecificEvent& source) {
    // Copy constructor needed as we free the data from event to event, hence take a copy of this
    this->source_pid = source.source_pid;
    this->message_type = source.message_type;
    this->event_id =  source.event_id;
    this->message_length = source.message_length;
    this->raw_data_length=source.raw_data_length;
    this->aContext=source.aContext;
    if (source.data != NULL) {
      this->data = (char*) malloc(this->raw_data_length);
      memcpy(this->data, source.data, this->raw_data_length);
    } else {
      this->data = source.data;
    }
    this->persistent = source.persistent;
    this->file_pos = source.file_pos;
  }

  SpecificEvent(const SpecificEvent& source, bool deep) {
    // Copy constructor which just takes a pointer to data if boolean flag is false
    this->source_pid = source.source_pid;
    this->message_type = source.message_type;
    this->event_id =  source.event_id;
    this->message_length = source.message_length;
    this->raw_data_length=source.raw_data_length;
    this->aContext=source.aContext;
    if (source.data != NULL && deep) {
      this->data = (char*) malloc(this->raw_data_length);
      memcpy(this->data, source.data, this->raw_data_length);
    } else {
      this->data = source.data;
    }
    this->persistent = source.persistent;
    this->file_pos = source.file_pos;
  }

  SpecificEvent(std::istream&, const std::streampos);

  char* getData() const { return data; }
  void setData(char* data) { this->data = data; }
  int getSourcePid() const { return source_pid; }
  void setSourcePid(int sourcePid) { source_pid = sourcePid; }
  std::string getEventId() const { return this->event_id; }
  int getMessageLength() { return this->message_length; }
  int getMessageType() { return this->message_type; }
  int getRawDataLength() { return this->raw_data_length; }
  bool isPersistent() { return this->persistent; }
  bool isAContext() { return this->aContext; }
  std::streampos getFilePos() const { return this->file_pos; }
  void setFilePos(std::streampos bookmark) { this->file_pos = bookmark; }
  void serialize(std::ostream&, const std::streampos) const;
  void serialize(std::ostream&) const;
};

enum HeldEventState { HELD, CONFIRMED };

struct HeldEvent {
  std::streampos file_pos;
  HeldEventState state = HELD;
  int target;
  SpecificEvent * spec_evt;
  HeldEvent() = default;
  HeldEvent(const HeldEvent&, const int target);
  HeldEvent(std::istream&, const std::streampos);
  void serialize(std::ostream&, const std::streampos);
  void serialize(std::ostream&);
  void fire(Messaging&);
  bool matchEventId(const std::string);
};

class DependencyKey {
  std::string s;
  int i;
public:
  DependencyKey(std::string s, int i) {
    this->s = s;
    this->i = i;
  }

  DependencyKey(std::istream& file, const std::streampos object_begin) {
    const char eoo[4] = {'E', 'O', 'O', '\0'};
    int str_len;
    char marker_buf[4], byte;
    bool end_of_string;
    std::streampos bookmark;

    file.seekg(object_begin);

    file.read(reinterpret_cast<char*>(&(this->i)), sizeof(int));

    bookmark = file.tellg();
    str_len = 0;
    end_of_string = false;
    while(!end_of_string) {
      file.get(byte);
      if (byte == '\0') {
        str_len++;
        end_of_string = true;
      } else {
        str_len++;
      }
    }

    file.seekg(bookmark);
    char * memblock = new char[str_len];
    file.read(memblock, str_len);
    this->s = std::string(memblock);
    delete[] memblock;

    file.read(marker_buf, 4);
    if (strcmp(marker_buf, eoo)) raiseError("DependencyKey deserialization error, EOO not found");
  }

  bool operator<(const DependencyKey& k) const {
    int s_cmp = this->s.compare(k.s);
    if(s_cmp == 0) {
      if (this->i == EDAT_ANY || k.i == EDAT_ANY) return false;
      return this->i < k.i;
    }
    return s_cmp < 0;
  }

  bool operator==(const DependencyKey& k) const {
    if (this->s.compare(k.s) == 0) {
      if (this->i == EDAT_ANY || k.i == EDAT_ANY) return true;
      return this->i == k.i;
    }
    return false;
  }

  void display() {
    printf("Key: %s from %d\n", s.c_str(), i);
  }

  void serialize(std::ostream& file, const std::streampos object_begin) const {
    // serialization schema:
    // int i, string s (as a char[]), EOO\0
    const char eoo[4] = {'E', 'O', 'O', '\0'};

    file.seekp(object_begin);
    file.write(reinterpret_cast<const char *>(&i), sizeof(i));
    file.write(s.c_str(), s.size()+1);
    file.write(eoo, sizeof(eoo));

    return;
  }

  void serialize(std::ostream& file) const {
    // serialization schema:
    // int i, string s (as a char[]), EOO\0
    const char eoo[4] = {'E', 'O', 'O', '\0'};

    file.write(reinterpret_cast<const char *>(&i), sizeof(i));
    file.write(s.c_str(), s.size()+1);
    file.write(eoo, sizeof(eoo));

    return;
  }

};

enum TaskDescriptorType { PENDING, PAUSED, ACTIVE };

struct TaskDescriptor {
  std::map<DependencyKey, int*> outstandingDependencies;
  std::map<DependencyKey, std::queue<SpecificEvent*>> arrivedEvents;
  std::vector<DependencyKey> taskDependencyOrder;
  int numArrivedEvents=0;
  taskID_t task_id;
  TaskDescriptor() { generateTaskID(); }
  void generateTaskID(void);
  static void resetTaskID(taskID_t);
  bool greedyConsumerOfEvents=false;
  virtual TaskDescriptorType getDescriptorType() = 0;
  virtual ~TaskDescriptor() = default;
};

struct PendingTaskDescriptor : TaskDescriptor {
  std::map<DependencyKey, int*> originalDependencies;
  bool freeData=true, persistent=false;
  int func_id = -1, resilient = 0;
  std::string task_name;
  void (*task_fn)(EDAT_Event*, int);
  PendingTaskDescriptor() = default;
  PendingTaskDescriptor(std::istream&, const std::streampos);
  void deepCopy(PendingTaskDescriptor&);
  void serialize(std::ostream&, const std::streampos);
  void serialize(std::ostream&);
  virtual TaskDescriptorType getDescriptorType() {return PENDING;}
  virtual ~PendingTaskDescriptor() = default;
};

struct ActiveTaskDescriptor : PendingTaskDescriptor {
  std::queue<HeldEvent*> firedEvents;
  ActiveTaskDescriptor(PendingTaskDescriptor&);
  virtual ~ActiveTaskDescriptor();
  virtual TaskDescriptorType getDescriptorType() {return ACTIVE;}
  PendingTaskDescriptor* generatePendingTask();
private:
  void serialize(std::ostream&, const std::streampos) const { return; };
};

struct PausedTaskDescriptor : TaskDescriptor {
  virtual TaskDescriptorType getDescriptorType() {return PAUSED;}
};

// This TaskExecutionContext is provided to the bootstrapper method, that is static (called from the thread)
// and hence we can pop in here more context to use before and after task execution.
struct TaskExecutionContext {
  PendingTaskDescriptor * taskDescriptor;
  ConcurrencyControl * concurrencyControl;
public:
  TaskExecutionContext(PendingTaskDescriptor * td, ConcurrencyControl * cc) : taskDescriptor(td), concurrencyControl(cc) { }
};

class Scheduler {
    int outstandingEventsToHandle; // This tracks the non-persistent events for termination checking
    int resilienceLevel;
    std::vector<PendingTaskDescriptor*> registeredTasks;
    std::vector<PausedTaskDescriptor*> pausedTasks;
    std::map<DependencyKey, std::queue<SpecificEvent*>> outstandingEvents;
    ThreadPool & threadPool;
    Configuration & configuration;
    ConcurrencyControl & concurrencyControl;
    std::mutex taskAndEvent_mutex;
    static void threadBootstrapperFunction(void*);
    std::pair<TaskDescriptor*, int> findTaskMatchingEventAndUpdate(SpecificEvent*);
    void consumeEventsByPersistentTasks();
    bool checkProgressPersistentTasks();
    std::vector<PendingTaskDescriptor*>::iterator locatePendingTaskFromName(std::string);
    static EDAT_Event * generateEventsPayload(TaskDescriptor*, std::set<int>*);
    static void generateEventPayload(SpecificEvent*, EDAT_Event*);
    void updateMatchingEventInTaskDescriptor(TaskDescriptor*, DependencyKey, std::map<DependencyKey, int*>::iterator, SpecificEvent*);
public:
    Scheduler(ThreadPool & tp, Configuration & aconfig, ConcurrencyControl & cc) : threadPool(tp), configuration(aconfig),
      concurrencyControl(cc) {
        outstandingEventsToHandle = 0;
        resilienceLevel = aconfig.get("EDAT_RESILIENCE", 0);
      }
    void registerTask(void (*)(EDAT_Event*, int), std::string, std::vector<std::pair<int, std::string>>, bool, bool);
    void registerTask(PendingTaskDescriptor *);
    EDAT_Event* pauseTask(std::vector<std::pair<int, std::string>>);
    void registerEvent(SpecificEvent*);
    void registerEvents(std::vector<SpecificEvent*>);
    void registerEvent(std::pair<DependencyKey,std::queue<SpecificEvent*>>);
    bool isFinished();
    void lockMutexForFinalisationTest();
    void unlockMutexForFinalisationTest();
    void readyToRunTask(PendingTaskDescriptor*);
    bool edatIsTaskSubmitted(std::string);
    bool removeTask(std::string);
    std::pair<int, EDAT_Event*> retrieveAnyMatchingEvents(std::vector<std::pair<int, std::string>>);
    void reset();
};

#endif
