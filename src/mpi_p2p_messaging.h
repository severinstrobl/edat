#ifndef SRC_MPI_P2P_MESSAGING_H_
#define SRC_MPI_P2P_MESSAGING_H_

#include <map>
#include "mpi.h"
#include "messaging.h"
#include <mutex>

class MPI_P2P_Messaging : public Messaging {
  bool protectMPI, mpiInitHere;
  int my_rank, total_ranks;
  std::map<MPI_Request, char*> outstandingSendRequests;
  std::mutex outstandingSendRequests_mutex;
  void initMPI();
  void checkSendRequestsForProgress();
public:
  MPI_P2P_Messaging(Scheduler & a_scheduler) : Messaging(a_scheduler) { initMPI(); }
  virtual void runPollForEvents();
  virtual void finalise();
  virtual void fireEvent(void *, int, int, int, const char *);
  virtual int getRank();
  virtual int getNumRanks();
};
#endif
