/*
 * rs_server.c - 测试用后端 HTTP 服务器（epoll + SO_REUSEPORT 多线程）
 *
 * 用法：rs_server <port> <threads> <name>
 * 每个响应的 body 是 "rs=<name>\n"，压测工具据此统计流量分布。
 * 请求带 "Connection: close" 时服务器先关闭连接（TIME_WAIT 留在服务器侧，
 * 客户端端口可以立即复用，短连接压测不会耗尽客户端端口）。
 * 每秒打印一次 QPS，Ctrl+C 退出时打印总数。
 *
 * 编译：gcc -O2 -pthread -o rs_server rs_server.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define BUF_SIZE 8192

static int g_port;
static char g_name[32];
static char g_resp_ka[256], g_resp_close[256];
static int g_resp_ka_len, g_resp_close_len;
static atomic_ulong g_requests, g_conns;
static volatile sig_atomic_t g_stop;

static void on_signal(int s) { (void)s; g_stop = 1; }

static int make_listener(void) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(g_port),
                          .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4096) < 0) {
    perror("bind/listen");
    exit(1);
  }
  return fd;
}

/* 每个连接的接收缓冲：处理一次 recv 中有多个 / 半个请求的情况 */
struct conn {
  int fd;
  int len;
  char buf[BUF_SIZE];
};

static void close_conn(int ep, struct conn *c) {
  epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
  close(c->fd);
  free(c);
}

static void *worker(void *arg) {
  (void)arg;
  int lfd = make_listener();
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
  epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
  struct epoll_event evs[MAX_EVENTS];

  while (!g_stop) {
    int n = epoll_wait(ep, evs, MAX_EVENTS, 200);
    for (int i = 0; i < n; ++i) {
      if (evs[i].data.ptr == NULL) { /* 新连接 */
        for (;;) {
          int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
          if (fd < 0)
            break;
          int one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          struct conn *c = calloc(1, sizeof(*c));
          c->fd = fd;
          struct epoll_event cev = {.events = EPOLLIN | EPOLLRDHUP, .data.ptr = c};
          epoll_ctl(ep, EPOLL_CTL_ADD, fd, &cev);
          atomic_fetch_add_explicit(&g_conns, 1, memory_order_relaxed);
        }
        continue;
      }
      struct conn *c = evs[i].data.ptr;
      int closed = 0;
      for (;;) {
        ssize_t r = recv(c->fd, c->buf + c->len, BUF_SIZE - 1 - c->len, 0);
        if (r == 0) { closed = 1; break; }
        if (r < 0) { if (errno != EAGAIN) closed = 1; break; }
        c->len += (int)r;
        c->buf[c->len] = 0;
        /* 逐个处理完整的请求（以空行结束） */
        char *start = c->buf, *end;
        while (!closed && (end = strstr(start, "\r\n\r\n")) != NULL) {
          *end = 0;
          int want_close = strcasestr(start, "Connection: close") != NULL;
          const char *resp = want_close ? g_resp_close : g_resp_ka;
          int rlen = want_close ? g_resp_close_len : g_resp_ka_len;
          if (send(c->fd, resp, rlen, MSG_NOSIGNAL) != rlen)
            closed = 1;
          atomic_fetch_add_explicit(&g_requests, 1, memory_order_relaxed);
          if (want_close)
            closed = 1;
          start = end + 4;
        }
        c->len -= (int)(start - c->buf);
        memmove(c->buf, start, c->len);
        if (c->len >= BUF_SIZE - 1)
          closed = 1; /* 请求头过大 */
        if (closed)
          break;
      }
      if (closed)
        close_conn(ep, c);
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <port> <threads> <name>\n", argv[0]);
    return 1;
  }
  g_port = atoi(argv[1]);
  int threads = atoi(argv[2]);
  snprintf(g_name, sizeof(g_name), "%s", argv[3]);
  char body[64];
  int blen = snprintf(body, sizeof(body), "rs=%s\n", g_name);
  g_resp_ka_len = snprintf(g_resp_ka, sizeof(g_resp_ka),
                           "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                           "Connection: keep-alive\r\n\r\n%s", blen, body);
  g_resp_close_len = snprintf(g_resp_close, sizeof(g_resp_close),
                              "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                              "Connection: close\r\n\r\n%s", blen, body);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  pthread_t tid[64];
  if (threads < 1 || threads > 64)
    threads = 1;
  for (int i = 0; i < threads; ++i)
    pthread_create(&tid[i], NULL, worker, NULL);
  printf("rs_server %s listening on :%d with %d threads\n", g_name, g_port, threads);
  fflush(stdout);

  unsigned long last = 0;
  while (!g_stop) {
    sleep(1);
    unsigned long now = atomic_load(&g_requests);
    if (now != last) {
      printf("[%s] %lu req/s  total %lu  conns %lu\n", g_name, now - last, now,
             atomic_load(&g_conns));
      fflush(stdout);
    }
    last = now;
  }
  for (int i = 0; i < threads; ++i)
    pthread_join(tid[i], NULL);
  printf("[%s] stopped, total requests %lu, connections %lu\n", g_name,
         atomic_load(&g_requests), atomic_load(&g_conns));
  return 0;
}
