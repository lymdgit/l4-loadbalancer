/*
 * loadgen.c - 简易 HTTP 压测工具（wrk 不可用时使用）
 *
 * 用法：loadgen -h <ip> -p <port> [-t 线程] [-c 每线程连接数] [-d 秒] [-s]
 *   -s   短连接：每个请求新建 TCP 连接（Connection: close），测 CPS
 *        默认长连接（keep-alive），测 QPS
 *
 * 输出：总请求数、QPS、延迟（平均 / p50 / p99 / p999）、错误数，
 *       以及按响应 body "rs=<name>" 统计的后端分布。
 *
 * 编译：gcc -O2 -pthread -o loadgen loadgen.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HIST_BUCKETS 64 /* 按 2^(i/4) 微秒分桶，覆盖 1us ~ 数十秒 */
#define MAX_RS 16

static struct sockaddr_in g_addr;
static int g_short, g_conns = 50, g_duration = 10;
static char g_req[256];
static int g_req_len;
static volatile int g_stop;

struct stats {
  uint64_t ok, err, connect_err, timeout;
  uint64_t lat_sum_us;
  uint64_t hist[HIST_BUCKETS];
  char rs_name[MAX_RS][16];
  uint64_t rs_cnt[MAX_RS];
};

struct conn {
  int fd;
  int connecting;
  uint64_t start_us;
  int len;
  char buf[2048];
};

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int bucket(uint64_t us) {
  int b = 0;
  double v = 1.0;
  while (b < HIST_BUCKETS - 1 && v * 1.189207 <= (double)us) { /* 2^(1/4) */
    v *= 1.189207;
    ++b;
  }
  return b;
}

static double bucket_upper(int b) {
  double v = 1.0;
  for (int i = 0; i <= b; ++i)
    v *= 1.189207;
  return v;
}

static void count_rs(struct stats *s, const char *body) {
  const char *p = strstr(body, "rs=");
  if (!p)
    return;
  p += 3;
  char name[16] = {0};
  int i = 0;
  while (i < 15 && p[i] && p[i] != '\n' && p[i] != '\r') {
    name[i] = p[i];
    ++i;
  }
  for (int k = 0; k < MAX_RS; ++k) {
    if (s->rs_name[k][0] == 0) {
      strcpy(s->rs_name[k], name);
      s->rs_cnt[k] = 1;
      return;
    }
    if (strcmp(s->rs_name[k], name) == 0) {
      ++s->rs_cnt[k];
      return;
    }
  }
}

static void conn_open(int ep, struct conn *c, struct stats *s) {
  c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int one = 1;
  setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  c->len = 0;
  c->start_us = now_us();
  int r = connect(c->fd, (struct sockaddr *)&g_addr, sizeof(g_addr));
  if (r < 0 && errno != EINPROGRESS) {
    ++s->connect_err;
    close(c->fd);
    c->fd = -1;
    return;
  }
  c->connecting = 1;
  struct epoll_event ev = {.events = EPOLLOUT | EPOLLIN, .data.ptr = c};
  epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
}

static void conn_close(int ep, struct conn *c) {
  if (c->fd >= 0) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
  }
  c->fd = -1;
}

static int send_req(int ep, struct conn *c) {
  c->connecting = 0;
  struct epoll_event ev = {.events = EPOLLIN, .data.ptr = c};
  epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
  if (!g_short)
    c->start_us = now_us(); /* 长连接：延迟从发请求开始算 */
  return send(c->fd, g_req, g_req_len, MSG_NOSIGNAL) == g_req_len;
}

/* 读到一个完整响应返回 1，需要继续读返回 0，出错返回 -1 */
static int read_resp(struct conn *c, struct stats *s) {
  for (;;) {
    ssize_t r = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);
    if (r == 0)
      return c->len ? -1 : -1;
    if (r < 0)
      return errno == EAGAIN ? 0 : -1;
    c->len += (int)r;
    c->buf[c->len] = 0;
    char *hdr_end = strstr(c->buf, "\r\n\r\n");
    if (!hdr_end)
      continue;
    char *cl = strcasestr(c->buf, "Content-Length:");
    int body_len = cl ? atoi(cl + 15) : 0;
    char *body = hdr_end + 4;
    if (c->buf + c->len - body < body_len)
      continue;
    uint64_t lat = now_us() - c->start_us;
    ++s->ok;
    s->lat_sum_us += lat;
    ++s->hist[bucket(lat)];
    count_rs(s, body);
    c->len = 0;
    return 1;
  }
}

static void *worker(void *arg) {
  struct stats *s = arg;
  int ep = epoll_create1(0);
  struct conn *conns = calloc(g_conns, sizeof(struct conn));
  for (int i = 0; i < g_conns; ++i) {
    conns[i].fd = -1;
    conn_open(ep, &conns[i], s);
  }
  struct epoll_event evs[1024];
  uint64_t last_check = now_us();
  while (!g_stop) {
    int n = epoll_wait(ep, evs, 1024, 50);
    for (int i = 0; i < n; ++i) {
      struct conn *c = evs[i].data.ptr;
      if (c->fd < 0)
        continue;
      if (evs[i].events & (EPOLLERR | EPOLLHUP) && c->connecting) {
        ++s->connect_err;
        conn_close(ep, c);
        conn_open(ep, c, s);
        continue;
      }
      if (c->connecting) {
        if (!send_req(ep, c)) {
          ++s->err;
          conn_close(ep, c);
          conn_open(ep, c, s);
        }
        continue;
      }
      int r = read_resp(c, s);
      if (r == 0)
        continue;
      if (r < 0 || g_short) {
        if (r < 0)
          ++s->err;
        conn_close(ep, c);
        conn_open(ep, c, s);
        continue;
      }
      if (!send_req(ep, c)) { /* 长连接：继续发下一个请求 */
        ++s->err;
        conn_close(ep, c);
        conn_open(ep, c, s);
      }
    }
    /* 超过 3 秒没有响应的连接视为超时，重建 */
    uint64_t now = now_us();
    if (now - last_check > 500000) {
      for (int i = 0; i < g_conns; ++i)
        if (conns[i].fd < 0 || now - conns[i].start_us > 3000000) {
          if (conns[i].fd >= 0)
            ++s->timeout;
          conn_close(ep, &conns[i]);
          conn_open(ep, &conns[i], s);
        }
      last_check = now;
    }
  }
  for (int i = 0; i < g_conns; ++i)
    conn_close(ep, &conns[i]);
  free(conns);
  close(ep);
  return NULL;
}

int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  int port = 80, threads = 2, opt;
  while ((opt = getopt(argc, argv, "h:p:t:c:d:s")) != -1) {
    switch (opt) {
    case 'h': host = optarg; break;
    case 'p': port = atoi(optarg); break;
    case 't': threads = atoi(optarg); break;
    case 'c': g_conns = atoi(optarg); break;
    case 'd': g_duration = atoi(optarg); break;
    case 's': g_short = 1; break;
    default:
      fprintf(stderr, "usage: %s -h ip -p port [-t threads] [-c conns/thread] "
                      "[-d seconds] [-s]\n", argv[0]);
      return 1;
    }
  }
  g_addr.sin_family = AF_INET;
  g_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &g_addr.sin_addr) != 1) {
    fprintf(stderr, "bad host %s\n", host);
    return 1;
  }
  g_req_len = snprintf(g_req, sizeof(g_req),
                       "GET / HTTP/1.1\r\nHost: %s\r\nConnection: %s\r\n\r\n",
                       host, g_short ? "close" : "keep-alive");

  struct stats *st = calloc(threads, sizeof(struct stats));
  pthread_t *tid = calloc(threads, sizeof(pthread_t));
  printf("loadgen: %s:%d %s, %d threads x %d conns, %ds\n", host, port,
         g_short ? "short connections (CPS)" : "keep-alive (QPS)", threads,
         g_conns, g_duration);
  uint64_t t0 = now_us();
  for (int i = 0; i < threads; ++i)
    pthread_create(&tid[i], NULL, worker, &st[i]);
  sleep(g_duration);
  g_stop = 1;
  for (int i = 0; i < threads; ++i)
    pthread_join(tid[i], NULL);
  double secs = (now_us() - t0) / 1e6;

  struct stats t = {0};
  for (int i = 0; i < threads; ++i) {
    t.ok += st[i].ok;
    t.err += st[i].err;
    t.connect_err += st[i].connect_err;
    t.timeout += st[i].timeout;
    t.lat_sum_us += st[i].lat_sum_us;
    for (int b = 0; b < HIST_BUCKETS; ++b)
      t.hist[b] += st[i].hist[b];
    for (int k = 0; k < MAX_RS && st[i].rs_name[k][0]; ++k) {
      int j = 0;
      while (j < MAX_RS && t.rs_name[j][0] && strcmp(t.rs_name[j], st[i].rs_name[k]))
        ++j;
      if (j < MAX_RS) {
        strcpy(t.rs_name[j], st[i].rs_name[k]);
        t.rs_cnt[j] += st[i].rs_cnt[k];
      }
    }
  }
  double pct[3] = {0.5, 0.99, 0.999}, val[3] = {0};
  uint64_t acc = 0;
  int pi = 0;
  for (int b = 0; b < HIST_BUCKETS && pi < 3; ++b) {
    acc += t.hist[b];
    while (pi < 3 && acc >= pct[pi] * t.ok && t.ok)
      val[pi++] = bucket_upper(b) / 1000.0;
  }
  printf("requests %lu in %.1fs: %.0f %s\n", t.ok, secs, t.ok / secs,
         g_short ? "conn/s (CPS)" : "req/s (QPS)");
  printf("latency avg %.3fms  p50 <%.3fms  p99 <%.3fms  p999 <%.3fms\n",
         t.ok ? t.lat_sum_us / 1000.0 / t.ok : 0, val[0], val[1], val[2]);
  printf("errors: read/write %lu  connect %lu  timeout %lu\n", t.err,
         t.connect_err, t.timeout);
  printf("backends:");
  for (int k = 0; k < MAX_RS && t.rs_name[k][0]; ++k)
    printf("  %s=%lu (%.1f%%)", t.rs_name[k], t.rs_cnt[k],
           100.0 * t.rs_cnt[k] / (t.ok ? t.ok : 1));
  printf("\n");
  return 0;
}
