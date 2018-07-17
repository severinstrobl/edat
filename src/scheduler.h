#ifndef SRC_SCHEDULER_H_
#define SRC_SCHEDULER_H_

#include "edat.h"
#include "threadpool.h"
#include "configuration.h"
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
    this->persistent= source.persistent;
  }

  SpecificEvent(std::istream&, std::streampos);

  char* getData() const { return data; }
  void setData(char* data) { this->data = data; }
  int getSourcePid() const { return source_pid; }
  void setSourcePid(int sourcePid) { source_pid = sourcePid; }
  std::string getEventId() { return this->event_id; }
  int getMessageLength() { return this->message_length; }
  int getMessageType() { return this->message_type; }
  int getRawDataLength() { return this->raw_data_length; }
  bool isPersistent() { return this->persistent; }
  bool isAContext() { return this->aContext; }
  void serialize(std::ostream&, std::streampos) const;
};

struct HeldEvent {
  int target;
  const char * event_id;
  SpecificEvent * spec_evt;
};

class DependencyKey {
  std::string s;
  int i;
public:
  DependencyKey(std::string s, int i) {
    this->s = s;
    this->i = i;
  }

  bool operator<(const DependencyKey& k) const {
    int s_cmp = this->s.compare(k.s);
    if(s_cmp == 0) {
      if (this->i == EDAT_ANY || k.i == EDAT_ANY) return false;
      return this->i < k.i;
    }
    return s_cmp < 0;
  }

  void display() {
    printf("Key: %s from %d\n", s.c_str(), i);
  }
};

enum TaskDescriptorType { PENDING, PAUSED, ACTIVE };

struct TaskDescriptor {
  std::map<DependencyKey, int*> outstandingDependencies;
  std::map<DependencyKey, std::queue<SpecificEvent*>> arrivedEvents;
  std::vector<DependencyKey> taskDependencyOrder;
  int numArrivedEvents;
  taskID_t task_id;
  TaskDescriptor(void) { generateTaskID(); }
  void generateTaskID(void);
  virtual TaskDescriptorType getDescriptorType() = 0;
  virtual ~TaskDescriptor() = default;
};

struct PendingTaskDescriptor : TaskDescriptor {
  std::map<DependencyKey, int*> originalDependencies;
  bool freeData, persistent, resilient;
  std::string task_name;
  void (*task_fn)(EDAT_Event*, int);
  void deepCopy(PendingTaskDescriptor&);
  virtual TaskDescriptorType getDescriptorType() {return PENDING;}
  virtual ~PendingTaskDescriptor() = default;
};

struct ActiveTaskDescriptor : PendingTaskDescriptor {
  std::queue<HeldEvent> firedEvents;
  ActiveTaskDescriptor(PendingTaskDescriptor&);
  virtual ~ActiveTaskDescriptor();
  virtual TaskDescriptorType getDescriptorType() {return ACTIVE;}
  PendingTaskDescriptor* generatePendingTask();
};

struct PausedTaskDescriptor : TaskDescriptor {
  virtual TaskDescriptorType getDescriptorType() {return PAUSED;}
};

class Scheduler {
    int outstandingEventsToHandle; // This tracks the non-persistent events for termination checking
    std::vector<PendingTaskDescriptor*> registeredTasks;
    std::vector<PausedTaskDescriptor*> pausedTasks;
    std::map<DependencyKey, std::queue<SpecificEvent*>> outstandingEvents;
    ThreadPool & threadPool;
    Configuration & configuration;
    std::mutex taskAndEvent_mutex;
    static void threadBootstrapperFunction(void*);
    std::pair<TaskDescriptor*, int> findTaskMatchingEventAndUpdate(SpecificEvent*);
    void consumeEventsByPersistentTasks();
    bool checkProgressPersistentTasks();
    std::vector<PendingTaskDescriptor*>::iterator locatePendingTaskFromName(std::string);
    static EDAT_Event * generateEventsPayload(TaskDescriptor*, std::set<int>*);
    void updateMatchingEventInTaskDescriptor(TaskDescriptor*, DependencyKey, std::map<DependencyKey, int*>::iterator, SpecificEvent*);
public:
    Scheduler(ThreadPool & tp, Configuration & aconfig) : threadPool(tp), configuration(aconfig) { outstandingEventsToHandle = 0; }
    void registerTask(void (*)(EDAT_Event*, int), std::string, std::vector<std::pair<int, std::string>>, bool);
    EDAT_Event* pauseTask(std::vector<std::pair<int, std::string>>);
    void registerEvent(SpecificEvent*);
    bool isFinished();
    void readyToRunTask(PendingTaskDescriptor*);
    bool isTaskScheduled(std::string);
    bool descheduleTask(std::string);
};

#endif
