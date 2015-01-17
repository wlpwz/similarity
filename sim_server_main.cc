#include "ub.h"
#include "sim_server.h"
#include "log.h"

using namespace sm;

comcfg::Configure conf;

static
int check(void *){
  SimServerDataManager::getInstance()->checkVersion ();
  
  return 0;
}

int
main(int argc, char **argv) {
  int ret = ub_load_comlog ("./conf", "comlog.conf");
  if (ret != 0) {
    SM_LOG_FATAL ("open configure failed");
    return -1;
  }

  if (0 != SimServerDataManager::getInstance()->init(argv[1])) {
    SM_LOG_FATAL ("init sim server manager error");
    return -1;
  }

  if (0 != conf.load ("./conf", "sim_server.conf")) {
    SM_LOG_FATAL ("open configure failed");
    return -1;
  }

  ub::NetReactor *reactor = new ub::NetReactor;
  if (reactor->load (conf["reactor"]) != 0) {
    SM_LOG_FATAL ("load reactor failed");
    return -1;
  }
  reactor->run();

  SimServer *simserver = new SimServer (reactor);
  if (simserver->load (conf["server"]["sim"]) != 0) {
    SM_LOG_WARNING ("load aserver failed");
    delete simserver;
    return -1;
  }

  SimServerDataManager::getInstance()->registerSimServer (simserver);
  ub_timer_task_t *timer = ub_create_timer_task();
  ub_add_timer_task(timer, check, NULL, 3000);
  ub_run_timer_task(timer);
  ub_join_timer_task(timer);
  ub_destroy_timer_task(timer);
  reactor->join();
  return 0;
}