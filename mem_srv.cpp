#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <logging.h>
#include <sstream>
using namespace std;
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

static void accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *ctx)
{
  /* We got a new connection! Set up a bufferevent for it. */
  struct event_base *base = evconnlistener_get_base(listener);
  struct bufferevent *bev = bufferevent_socket_new(
      base, fd, BEV_OPT_CLOSE_ON_FREE);
  set_tcp_no_delay(fd);

  bufferevent_setcb(bev, echo_read_cb, NULL, echo_event_cb, NULL);

  bufferevent_enable(bev, EV_READ|EV_WRITE);
}

int main(int argc, char **argv)
{
  int port = 9876;

  if (argc > 1) {
    port = atoi(argv[1]);
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
