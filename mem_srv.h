#ifndef _EVTHREAD_H_
#define _EVTHREAD_H_

#include <pthread.h>
#include <event2/event.h>
#include <queue>

struct CQ_ITEM {
  int fd;

};

struct CQ {
  std::queue<CQ_ITEM> cq;
  pthread_mutex_t lock;
};

struct EVThread {
  pthread_t thread_id;
  struct event_base* base;

  struct event notify_event;  /* listen event for notify pipe */
  int notify_receive_fd;      /* receiving end of notify pipe */
  int notify_send_fd;         /* sending end of notify pipe */

  CQ new_conn;
};



#endif
