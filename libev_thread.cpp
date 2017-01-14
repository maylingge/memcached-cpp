#include "libev_thread.h"
#include "logging.h"
#include <pthread.h>
#include <queue>

static EVThread* evthread;
static int nthreads = 1;
static int init_count = 0;

static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&((EVThread*)arg)->thread_id, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}


static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}



static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    /* Force worker threads to pile up if someone wants us to */
}

static void thread_libevent_process(int fd, short which, void *arg) {
    EVThread *me = arg;
    char buf[1];
    unsigned int timeout_fd;

    if (read(fd, buf, 1) != 1) {
        log_err("Can't read from libevent pipe\n");
        return;
    }

    switch (buf[0]) {
    case 'c':
      CQ_ITEM item = me->cq.front();
      cq.pop();
      struct bufferevent *bev = bufferevent_socket_new(me->base, item.fd, BEV_OPT_CLOSE_ON_FREE);
      set_tcp_no_delay(fd);

      bufferevent_setcb(bev, echo_read_cb, NULL, echo_event_cb, NULL);

      bufferevent_enable(bev, EV_READ|EV_WRITE);     
      break;
    }
}

static void setup_thread(EVThread *me) {
    me->base = event_init();
    if (! me->base) {
        log_err("Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        log_err("Can't monitor libevent notify pipe\n");
        exit(1);
    }
}
/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    EVThread *me = arg;

    /* Any per-thread setup can happen here; memcached_thread_init() will block until
     * all threads have finished initializing.
     */
    register_thread_initialized();
    event_base_loop(me->base, 0);
    return NULL;
}


void thread_init() {
  pthread_mutex_init(&init_lock, NULL);
  pthread_cond_init(&init_cond, NULL);

  int fds[2];
  if (pipe(fds)) {
      log_err("Can't create notify pipe");
      exit(1);
  }
  
  evthread.notify_receive_fd = fds[0];
  evthread.notify_send_fd = fds[1];
  
  setup_thread(evthread);

  create_worker(worker_libevent, evthread); 


  pthread_mutex_lock(&init_lock);
  wait_for_thread_registration(nthreads);
  pthread_mutex_unlock(&init_lock);


}
