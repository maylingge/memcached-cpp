#include <event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sstream>
#include <queue>

#include "mem_srv.h"
#include "logging.h"
#include <pthread.h>
#include <unistd.h>

using namespace std;

static std::vector<EVThread*> evthreads;
static int nthreads = 1;
static int init_count = 0;
static int cur_thread = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

static void cq_push(CQ& connq, CQ_ITEM& item) {
  pthread_mutex_lock(&connq.lock);
  connq.cq.push(item);
  cout<<"Queue: " << connq.cq.size()<<endl;
  pthread_mutex_unlock(&connq.lock);
}

static CQ_ITEM cq_pop(CQ& connq) {
    if (!connq.cq.empty()) {
      pthread_mutex_lock(&connq.lock);
      CQ_ITEM item = connq.cq.front();
      connq.cq.pop();
      pthread_mutex_unlock(&connq.lock);
      return item;
    }
}

static void set_tcp_no_delay(evutil_socket_t fd)
{
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
      &one, sizeof one);
}

static void signal_cb(evutil_socket_t fd, short what, void *arg)
{
  struct event_base *base = (struct event_base*)arg;
  log_info("stop\n");

  event_base_loopexit(base, NULL);
}

static void echo_read_cb(struct bufferevent *bev, void *ctx)
{
  /* This callback is invoked when there is data to read on bev. */
  struct evbuffer *input = bufferevent_get_input(bev);
  struct evbuffer *output = bufferevent_get_output(bev);
  void* message = malloc(evbuffer_get_length(input));
  evbuffer_copyout(input, message, evbuffer_get_length(input));
  ostringstream oss;
  oss<<"Recieved bytes: "<<evbuffer_get_length(input);
  log_info(oss.str());

  free(message);

  /* Copy all the data from the input buffer to the output buffer. */
  evbuffer_add_buffer(output, input);
}

static void echo_event_cb(struct bufferevent *bev, short events, void *ctx)
{
  struct evbuffer *output = bufferevent_get_output(bev);
  size_t remain = evbuffer_get_length(output);
  if (events & BEV_EVENT_ERROR) {
    log_err("Error from bufferevent");
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    log_info("closing");
    bufferevent_free(bev);
  }
}

/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    log_info("Create_worker");
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&((EVThread*)arg)->thread_id, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
    stringstream ss;
    ss << "Thread is:" << ((EVThread*)arg)->thread_id;
    log_info(ss.str());
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
    EVThread *me = (EVThread*)arg;
    char buf[1];
    unsigned int timeout_fd;

    if (read(fd, buf, 1) != 1) {
        log_err("Can't read from libevent pipe\n");
        return;
    }

    switch (buf[0]) {
    case 'c':
      CQ_ITEM item = cq_pop(me->new_conn);
      cout<<"Current thread: " << me->thread_id<<endl;
      
      struct bufferevent *bev = bufferevent_socket_new(me->base, item.fd, BEV_OPT_CLOSE_ON_FREE);
      set_tcp_no_delay(fd);

      bufferevent_setcb(bev, echo_read_cb, NULL, echo_event_cb, NULL);

      bufferevent_enable(bev, EV_READ|EV_WRITE);     
      break;
    }
}

static void setup_thread(EVThread *me) {
    log_info("Setup_thread");
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
    EVThread *me = (EVThread*)arg;

    /* Any per-thread setup can happen here; memcached_thread_init() will block until
     * all threads have finished initializing.
     */
    register_thread_initialized();
    event_base_loop(me->base, 0);
    return NULL;
}

void thread_init() {
  log_info("Thread_init");
  
  pthread_mutex_init(&init_lock, NULL);
  pthread_cond_init(&init_cond, NULL);

  for (int i=0;i<nthreads;i++) {
    EVThread* evthread = new EVThread;
    evthreads.push_back(evthread);

    int fds[2];
    if (pipe(fds)) {
        log_err("Can't create notify pipe");
        exit(1);
    }
  
    evthread->notify_receive_fd = fds[0];
    evthread->notify_send_fd = fds[1];
    
    setup_thread(evthread);

    create_worker(worker_libevent, evthread); 
  }

  pthread_mutex_lock(&init_lock);
  wait_for_thread_registration(nthreads);
  pthread_mutex_unlock(&init_lock);


}

static void accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *ctx)
{
  /* We got a new connection! Set up a bufferevent for it. */
  log_info("Accept");
  int next = (cur_thread++) % evthreads.size();
  cout<<"Thread: " << next<<endl;
  CQ_ITEM item;
  item.fd = fd;
  cq_push(evthreads[next]->new_conn, item);
  char buf[1];
  buf[0] = 'c';

  if (write(evthreads[next]->notify_send_fd, buf, 1) != 1) {
    log_err("Failed to write to thread notify pipe");
  }
}

int main(int argc, char **argv)
{
  int port = 9876;
  if (argc > 1) {
    port = atoi(argv[1]);
  }

  if (argc > 2) {
    nthreads = atoi(argv[2]);
  }
  
  struct event_base* base = event_base_new();
  
  if (!base) {
  	log_err("Couldnt open event base.");
	return 1;
  } 
  log_info("event base created.");
  struct event * evstop = evsignal_new(base, SIGHUP, signal_cb, base);

  evsignal_add(evstop, NULL);

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(0);
  sin.sin_port = htons(port);

  thread_init(); 

  struct evconnlistener *listener = evconnlistener_new_bind(base, accept_conn_cb, NULL,LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE, -1,(struct sockaddr*)&sin, sizeof(sin));

  if (!listener) {
  	log_err("Couldnt create listener.");
	return 1;
  }
  
  event_base_dispatch(base);
  evconnlistener_free(listener);
  event_free(evstop);
  event_base_free(base);
  return 0;
 
}
