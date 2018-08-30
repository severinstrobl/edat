#ifndef SRC_THREADPOOL_H_
#define SRC_THREADPOOL_H_

#include <thread>
#include <condition_variable>
#include <mutex>
#include <queue>
#include "configuration.h"
#include "threadpackage.h"

class Messaging;
struct PausedTaskDescriptor;

class ThreadPoolCommand {
  void (*callFunction)(void *);
  void *data;
 public:
  void setCallFunction(void (*callFunction)(void *)) { this->callFunction = callFunction; }
  void issueFunctionCall() { this->callFunction(data); }
  void (*getCallFunction())(void *) { return callFunction; }
  void setData(void *data) { this->data = data; }
  void *getData() { return this->data; }
};

struct PendingThreadContainer {
  void (*callFunction)(void *);
  void *args;
};

struct WorkerThread {
  ThreadPackage * activeThread;
  std::map<PausedTaskDescriptor*, ThreadPackage*> pausedThreads;
  std::queue<ThreadPackage*> waitingThreads, idleThreads;
  std::mutex pausedAndWaitingMutex;
  int core_id=-1;
  ThreadPoolCommand threadCommand;
};

class ThreadPool {
  Configuration & configuration;
  int number_of_workers, pollingProgressThread;
  bool main_thread_is_worker, restartAnotherPoller;
  ThreadPackage * mainThreadPackage;
  PausedTaskDescriptor* pausedMainThreadDescriptor=NULL;
  WorkerThread * workers;
  std::mutex thread_start_mutex, progressMutex, pollingProgressThreadMutex, pausedTasksToWorkersMutex;
  std::queue<PendingThreadContainer> threadQueue;
  std::map<PausedTaskDescriptor*, int> pausedTasksToWorkers;

  bool *threadBusy, progressPollIdleThread;
  int next_suggested_idle_thread;
  Messaging * messaging=NULL;

  void threadEntryProcedure(int);
  int get_index_of_idle_thread();
  void mapThreadsToCores(bool);
  void launchThreadToPollForProgressIfPossible();
  int findIndexFromThreadId(std::thread::id);
  static void threadReportCoreIdFunction(void *);
 public:
  ThreadPool(Configuration&);
  void lockMutexForFinalisationTest();
  void unlockMutexForFinalisationTest();
  void startThread(void (*)(void *), void *);
  bool isThreadPoolFinished();
  void setMessaging(Messaging*);
  void notifyMainThreadIsSleeping();
  void pauseThread(PausedTaskDescriptor*, std::unique_lock<std::mutex>*);
  void markThreadResume(PausedTaskDescriptor*);
  void resetPolling();
  int getNumberOfWorkers() { return number_of_workers; }
  int getCurrentWorkerId();
  int getNumberActiveWorkers();
};

#endif /* SRC_THREADPOOL_H_ */
