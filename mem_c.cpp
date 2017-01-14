#include <event2/event.h>
#include <iostream>
#include "logging.h"
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <stdlib.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <string.h>
using namespace std;



static void set_tcp_no_delay(evutil_socket_t fd)
{
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static void eventcb(struct bufferevent *bev, short events, void *ptr)
{
  if (events & BEV_EVENT_CONNECTED) 
  {
    evutil_socket_t fd = bufferevent_getfd(bev);
    set_tcp_no_delay(fd);
  } else if(events & BEV_EVENT_ERROR) 
  {
    log_err("Not connected.");
  }
}
static void readcb(struct bufferevent* bev, void* ctx)
{
  struct evbuffer* input = bufferevent_get_input(bev);
  void* message = malloc(evbuffer_get_length(input));
  evbuffer_copyout(input, message, evbuffer_get_length(input));
  
  log_info((char*)message);
  free(message);
  bufferevent_free(bev);
}
int main(int argc, char **argv)
{
  struct event_base* eb = event_base_new();
  if(!eb) {
    log_err("Couldnt open event base.");
  } 
  log_info("Event base created.");
  
  struct sockaddr_in sin;
  int port = 1234;
  if(argc >=2) 
  {
    port = atoi(argv[1]);
  }
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(0x7f000001);
  sin.sin_port = htons(port);
  
  struct bufferevent* bev = bufferevent_socket_new(eb, -1, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(bev, readcb, NULL, eventcb, NULL);
  bufferevent_enable(bev, EV_READ|EV_WRITE);  

  string message = "set apple 1\r\nred\r\n";
  evbuffer_add(bufferevent_get_output(bev), message.c_str(), sizeof(message));
  
  if (bufferevent_socket_connect(bev, (struct sockaddr*)&sin, sizeof(sin)) < 0 ) 
  {
    bufferevent_free(bev);
    log_err("error connect");
    return -1;
  }
  log_info("event dispatch.");
  event_base_dispatch(eb);

  // bufferevent_free(bev);
  //event_base_free(eb);
  
  return 0;
}
