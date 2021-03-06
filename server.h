#ifndef SERVER_H
#define SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/thread.h>
#include "zf_log.h"

#ifdef _THREAD
#include "thr_pool.h"
#endif

#define MSG_SIZE    8192

/* logfile info */
typedef struct
{
    char path[1024];
    FILE* fp;
} log_info_t;

// server info
typedef struct
{
	pid_t pid;
    struct event_base* base;
    struct evconnlistener* lev;
#ifdef _THREAD
    thr_pool_t *thread;
#endif
} server_info_t;

// client connection info
typedef struct
{
    char ip[20];
    char tran_id[24];
    struct event_base* base;
    struct bufferevent* bev;
} client_info_t;

#ifdef __cplusplus
}
#endif

#endif /* SERVER_H */
