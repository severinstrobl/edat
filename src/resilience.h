#ifndef SRC_RESILIENCE_H
#define SRC_RESILIENCE_H

#include "configuration.h"
#include "messaging.h"
#include <thread>
#include <map>
#include <queue>

void resilienceInit(Configuration&, Messaging*);

struct LoadedEvent {
  void * data;
  int data_count;
  int data_type;
  int target;
  bool persistent;
  const char * event_id;
};

class EDAT_Ledger {
private:
  Configuration & configuration;
  Messaging * messaging;
  std::map<std::thread::id,std::queue<LoadedEvent>> event_battery;
public:
  EDAT_Ledger(Configuration&);
  void setMessaging(Messaging*);
  void loadEvent(std::thread::id, void*, int, int, int, bool, const char *);
  void fireCannon(std::thread::id);
  void finalise(void);
};

namespace resilience {
  extern EDAT_Ledger * process_ledger;
}

#endif
