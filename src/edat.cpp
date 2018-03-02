#include "edat.h"
#include "threadpool.h"
#include "scheduler.h"
#include "messaging.h"
#include "mpi_p2p_messaging.h"
#include <stddef.h>
#include <stdarg.h>
#include <string>
#include <utility>

static ThreadPool * threadPool;
static Scheduler * scheduler;
static Messaging * messaging;

int edatInit(int* argc, char*** argv) {
  threadPool=new ThreadPool();
  scheduler=new Scheduler(*threadPool);
  messaging=new MPI_P2P_Messaging(*scheduler);
  messaging->pollForEvents();
  return 0;
}

int edatFinalise(void) {
  while (!messaging->isFinished());
  while (!threadPool->isThreadPoolFinished());
  while (!scheduler->isFinished());
  messaging->finalise();
  return 0;
}

int edatGetRank() {
  return messaging->getRank();
}

int edatGetNumRanks() {
  return messaging->getNumRanks();
}

int edatScheduleTask(void (*task_fn)(EDAT_Event*, int), char* uniqueID) {
  return edatScheduleMultiTask(task_fn, 1, EDAT_ANY, uniqueID);
}

int edatScheduleMultiTask(void (*task_fn)(EDAT_Event*, int), int num_dependencies, ...) {
  std::vector<std::pair<int, std::string>> dependencies;
  va_list valist;
  va_start(valist, num_dependencies);
  for (int i=0; i<num_dependencies; i++) {
    int src=va_arg(valist, int);
    char * uuid=va_arg(valist, char*);
    if (src == EDAT_ALL) {
      for (int j=0;j<messaging->getNumRanks();j++) {
        dependencies.push_back(std::pair<int, std::string>(j, std::string(uuid)));
      }
    } else {
      dependencies.push_back(std::pair<int, std::string>(src, std::string(uuid)));
    }
  }
  va_end(valist);
  scheduler->registerTask(task_fn, dependencies);
  return 0;
}

int edatFireEvent(void* data, int data_type, int data_count, int target, const char * uniqueID) {
  messaging->fireEvent(data, data_count, data_type, target, uniqueID);
}

int edatFireEventWithReflux(void* data, int data_type, int data_count, int target, const char * uniqueID,
                            void (*reflux_task_fn)(EDAT_Event*, int)) {
  messaging->fireEvent(data, data_count, data_type, target, uniqueID, reflux_task_fn);
}
