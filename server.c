#include "server.h"

// background process processing
static int daemonlize()
{
	pid_t pid;

	if (daemon(0, 0) == -1) return -1;

	pid = fork();

	if (pid == -1) return 1;
	else if (pid > 0) _exit(0);

	return 0;
}

// logfile output callback
static void logfile_output_callback(const zf_log_message* msg, void* arg)
{
	log_info_t* log = (log_info_t*)arg;

	// generate log file if it does not exist
	if (access(log->path, F_OK) < 0)
	{
		if (log->fp != NULL) fclose(log->fp);

		if ((log->fp = fopen(log->path, "a")) == NULL)
		{
			ZF_LOGW("failed to open log file %s", log->path);
			return;
		}
	}

	*msg->p = '\n';
	fwrite(msg->buf, msg->p - msg->buf + 1, 1, log->fp);
	fflush(log->fp);
}

// logfile close
static void logfile_output_close(int status, void* arg)
{
	log_info_t* log = (log_info_t*)arg;

	fclose(log->fp);
	free(log);
}

// logfile open
void logfile_output_open(const char* const log_path)
{
	log_info_t* log;

	log = calloc(1, sizeof(*log));

	snprintf(log->path, sizeof(log->path), "%s", log_path);

	if ((log->fp = fopen(log->path, "a")) == NULL)
	{
		ZF_LOGW("failed to open log file %s", log->path);
		return;
	}
	on_exit(logfile_output_close, log);
	zf_log_set_output_v(ZF_LOG_PUT_STD, log, logfile_output_callback);
}

// signal handler setting
static void signal_register(int signum, void* handler)
{
	struct sigaction act;

	bzero(&act, sizeof(act));
	// set interrupted signal restart flag (SA_RESTART)
	act.sa_flags = SA_SIGINFO | SA_RESTART;
	act.sa_handler = (void*)handler;

	if (sigaction(signum, &act, NULL) < 0)
		ZF_LOGW("sigaction(%d) failed", signum);
}

// signal handler
static void signal_handler(int signo, siginfo_t* info, void* arg)
{
	int status;
	int spid;

	sigset_t sigset, oldset;

	// block all signals received during handler processing
	sigfillset(&sigset);
	if (sigprocmask(SIG_BLOCK, &sigset, &oldset) < 0)
		ZF_LOGW("sigprocmask(%d) error", signo);

	switch (signo)
	{
	case SIGTERM:
		exit(0);
	case SIGCHLD:
		spid = wait(&status);
		ZF_LOGD("child process exit (%d)", spid);
		break;
	case SIGALRM:
		ZF_LOGD("timeout");
		break;
	default:
		break;
	}
}

// create transaction ID (for log search)
static void get_tran_id(char* transaction_id)
{
	uint32_t time_low;
	uint16_t time_mid;
	uint16_t time_hi_and_version;
	pid_t pid;

	struct timeval tp;
	uint64_t timestamp;

	gettimeofday(&tp, (struct timezone*)0);

	timestamp = ((uint64_t)tp.tv_sec * 10000000) + ((uint64_t)tp.tv_usec * 10) + 0x01B21DD213814000LL;

	time_low = (unsigned long)(timestamp & 0xFFFFFFFF);
	time_mid = (unsigned short)((timestamp >> 32) & 0xFFFF);
	time_hi_and_version = (unsigned short)((timestamp >> 48) & 0x0FFF);
	time_hi_and_version |= (1 << 12);
	pid = getpid();

	snprintf(transaction_id, 24, "%8.8x-%4.4x-%4.4x-%4.4x", time_low, time_mid, time_hi_and_version, pid);
}

// thread callback
static void* net_thread_callback(void* arg)
{
	client_info_t* client = (client_info_t*)arg;

	event_base_dispatch(client->base);

	ZF_LOGD("disconnected");

	evutil_closesocket(bufferevent_getfd(client->bev));
	bufferevent_free(client->bev);
	event_base_free(client->base);
	free(client);

	return NULL;
}

// bufferevent read callback
static void net_read_callback(struct bufferevent* bev, void* arg)
{
	client_info_t* client = (client_info_t*)arg;
	struct evbuffer* input = bufferevent_get_input(bev);
	struct timeval tv = { 5, 0 };

	// reset timeout
	bufferevent_set_timeouts(bev, NULL, NULL);

	// TODO: data receive processing
	// TODO: message processing

	// timeout setting
	bufferevent_set_timeouts(bev, &tv, &tv);
}

// bufferevent event callback (error, timeout, connected, eof ...)
static void net_event_callback(struct bufferevent* bev, short events, void* arg)
{
	client_info_t* client = (client_info_t*)arg;
	struct evbuffer* input = bufferevent_get_input(bev);

	if (events & BEV_EVENT_CONNECTED)
		ZF_LOGD("connected");

	if (events & BEV_EVENT_READING)
		ZF_LOGD("reading");

	if (events & BEV_EVENT_WRITING)
		ZF_LOGD("writing");

	// timeout event handling
	if (events & BEV_EVENT_TIMEOUT)
	{
		ZF_LOGD("timeout");
		if (events & BEV_EVENT_READING)
		{
			// TODO: when data is read to read buffer
		}
	}

	// connection termination event processing
	if (events & BEV_EVENT_EOF)
	{
		ZF_LOGD("disconnected");
	}

	// error event handling
	if (events & BEV_EVENT_ERROR)
	{
		ZF_LOGD("%s", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
	}
}

// set an error callback on a listener
static void net_server_error_callback(struct evconnlistener* listener, void* arg)
{
	struct event_base* base = evconnlistener_get_base(listener);
	int err = EVUTIL_SOCKET_ERROR();

	ZF_LOGW("got an error %d (%s) on the listener. shutting down.", err, evutil_socket_error_to_string(err));
	event_base_loopexit(base, NULL);
}

// socket accept event callback
static void
net_server_callback(struct evconnlistener* lev, evutil_socket_t fd, struct sockaddr* sa, int socklen, void* arg)
{
	server_info_t* server = (server_info_t*)arg;
	client_info_t *client;

	struct sockaddr_in *client_addr = (struct sockaddr_in *)sa;
	struct timeval tv = {2, 0};

	ZF_LOGD("connected");

#ifdef _THREAD
	struct event_config *cfg;

	client = calloc(1, sizeof(*client));
	if (client == NULL)
	{
		ZF_LOGW("calloc() failed");
		return;
	}

	// libevent setting
	cfg = event_config_new();
	// event_config_set_flag(cfg, EVENT_BASE_FLAG_NO_CACHE_TIME);
	// event_config_avoid_method(cfg, "epoll");
	// event_config_avoid_method(cfg, "poll");

	client->base = event_base_new_with_config(cfg);
	event_config_free(cfg);

	if (!client->base)
	{
		ZF_LOGW("event_base_new() failed");
		return;
	}

	client->bev = bufferevent_socket_new(client->base, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!client->bev)
	{
		ZF_LOGW("bufferevent_socket_new() failed");
		return;
	}

	// timeout setting (2 secs)
	bufferevent_set_timeouts(client->bev, &tv, &tv);

	evutil_inet_ntop(AF_INET, &client_addr->sin_addr, client->ip, sizeof(client->ip));
	get_tran_id(client->tran_id);
	zf_log_set_tag_prefix(client->tran_id);

	ZF_LOGD("client ip : %s", client->ip);

	bufferevent_setcb(client->bev, net_read_callback, NULL, net_event_callback, client);
	bufferevent_enable(client->bev, EV_READ | EV_WRITE);

	// add job to thread pool queue
	thr_pool_queue(server->thread, net_thread_callback, client);
#else
	pid_t pid;

	// prevent child processes from listening after fork()
	evconnlistener_disable(server->lev);

	pid = fork();

	switch (pid)
	{
	case 0: // child process
		event_reinit(server->base);

		client = calloc(1, sizeof(*client));
		if (client == NULL)
		{
			ZF_LOGW("calloc() failed");
			return;
		}

		client->bev = bufferevent_socket_new(server->base, fd, BEV_OPT_CLOSE_ON_FREE);
		if (!client->bev)
		{
			ZF_LOGW("bufferevent_socket_new() failed");
			return;
		}

		// timeout setting (2 secs)
		bufferevent_set_timeouts(client->bev, &tv, &tv);

		evutil_inet_ntop(AF_INET, &client_addr->sin_addr, client->ip, sizeof(client->ip));
		get_tran_id(client->tran_id);
		zf_log_set_tag_prefix(client->tran_id);

		ZF_LOGD("client ip : %s", client->ip);

		bufferevent_setcb(client->bev, net_read_callback, NULL, net_event_callback, client);
		bufferevent_enable(client->bev, EV_READ | EV_WRITE);
		break;
	case -1: // fork fail
		ZF_LOGW("fork() failed");
		evconnlistener_enable(server->lev);
		break;
	default: // parent process
		ZF_LOGD("child process fork (%d)", pid);
		evutil_closesocket(fd);
		evconnlistener_enable(server->lev);
	}
#endif
}

// network server shutdown processing
static void net_server_exit(int status, void* arg)
{
	server_info_t* server = (server_info_t*)arg;

	if (server->pid == getpid())
	{
		evconnlistener_free(server->lev);
		event_base_free(server->base);
#ifdef _THREAD
		thr_pool_wait(server->thread);
		thr_pool_destroy(server->thread);
#endif
		ZF_LOGD("server shutdown");
	}
	else
	{
		ZF_LOGD("disconnected");
	}
}

// network server setting
int net_server()
{
	struct event_config* cfg;
	struct sockaddr_in server_addr;
	server_info_t server;
	unsigned int flags = 0;

	// daemonlize();

	server.pid = getpid();

	// libevent initialize
	event_init();

#ifdef _THREAD
	evthread_use_pthreads();
#endif

	cfg = event_config_new();
	// event_config_set_flag(cfg, EVENT_BASE_FLAG_NO_CACHE_TIME);
	// event_config_avoid_method(cfg, "epoll");
	// event_config_avoid_method(cfg, "poll");

	server.base = event_base_new_with_config(cfg);
	if (!server.base)
	{
		ZF_LOGW("event_base_new() failed");
		return -1;
	}

	event_config_free(cfg);

#ifdef _THREAD
	server.thread = thr_pool_create(5, 5, 0, NULL);
#endif

	bzero(&server_addr, sizeof(server_addr));

	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(10000);

#ifdef _THREAD
	flags = LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE | LEV_OPT_THREADSAFE;
#else
	flags = LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE;
#endif

	server.lev = evconnlistener_new_bind(server.base, net_server_callback, &server, flags, -1,
		(struct sockaddr*)&server_addr, sizeof(server_addr));

	if (!server.lev)
	{
		ZF_LOGW("evconnlistener_new_bind() failed");
		return -1;
	}

	evconnlistener_set_error_cb(server.lev, net_server_error_callback);
	on_exit(net_server_exit, &server);

	ZF_LOGI("server start");

	// signal setting
	signal_register(SIGTERM, signal_handler);
	signal_register(SIGCHLD, signal_handler);
	signal_register(SIGALRM, signal_handler);

	event_base_dispatch(server.base);
	return 0;
}

int main(int argc, char** argv)
{
	// logfile_output_open("server.log");
	net_server();
}