#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 80
#define MAX_EVENTS 4096
#define NUM_THREADS 4 // 根据 CPU 核心数调整

// 每个线程一个 cache line（64字节对齐），避免 false sharing
typedef struct {
  unsigned long long count;
  char _pad[56]; // 64 - sizeof(unsigned long long)
} __attribute__((aligned(64))) per_thread_counter_t;

static per_thread_counter_t thread_counters[16]; // 最多支持 16 线程

void *stats_thread(void *arg) {
  (void)arg;
  unsigned long long last_total = 0;
  while (1) {
    sleep(1);
    unsigned long long total = 0;
    for (int i = 0; i < NUM_THREADS; i++)
      total += thread_counters[i].count;
    printf("[STATS] Total Requests: %llu, QPS: %llu\n", total,
           total - last_total);
    last_total = total;
  }
  return NULL;
}

// 用 sizeof-1 自动计算长度，避免手动算错
static const char response[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: 2\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "OK";
static const int response_len = sizeof(response) - 1; // 自动计算，不含 '\0'

// 安全关闭连接：先从 epoll 移除，再 close
static inline void close_conn(int epoll_fd, int fd) {
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  close(fd);
}

void *worker_thread(void *arg) {
  int thread_id = *(int *)arg;

  // ===== CPU 亲和性绑定：减少线程迁移和 cache miss =====
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(thread_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  // 每个线程创建自己的 socket (SO_REUSEPORT)
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  // TCP_DEFER_ACCEPT: 内核只在收到第一个数据包后才唤醒 accept
  // 这样 accept 返回的 fd 已经有数据可读，省掉一次空的 epoll 唤醒
  int defer = 1;
  setsockopt(server_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof(defer));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    return NULL;
  }
  listen(server_fd, 65535);

  // server_fd 设置非阻塞
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

  // 每个线程有自己的 epoll
  int epoll_fd = epoll_create1(0);
  struct epoll_event ev, events[MAX_EVENTS];

  // server_fd 使用 LT 模式 + EPOLLIN
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

  printf("Worker %d started on port %d (bound to CPU %d)\n", thread_id, PORT,
         thread_id % (int)sysconf(_SC_NPROCESSORS_ONLN));

  while (1) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;

      if (fd == server_fd) {
        // 批量 accept：一次 epoll 唤醒处理所有等待的连接
        while (1) {
          int client_fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK);
          if (client_fd < 0)
            break;

          // 禁用 Nagle 算法
          int nodelay = 1;
          setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

          // 客户端 fd 使用 LT 模式
          // LT 模式优势：wrk 一问一答模式下，每个事件 recv 一次即可
          // 比 ET 模式少一次 recv(EAGAIN) 系统调用 = 每请求少 33% 的 syscall
          ev.events = EPOLLIN;
          ev.data.fd = client_fd;
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        }
      } else {
        // LT 模式：有数据就通知，recv 一次即可
        // wrk 一问一答模式下，每次事件恰好对应一个完整请求
        char buffer[1024]; // wrk 请求通常 < 200 字节，不需要 4096
        int len = recv(fd, buffer, sizeof(buffer), 0);
        if (len > 0) {
          // 统计本次 recv 中包含的完整请求数
          int num_requests = 0;
          for (int j = 0; j <= len - 4; j++) {
            if (buffer[j] == '\r' && buffer[j + 1] == '\n' &&
                buffer[j + 2] == '\r' && buffer[j + 3] == '\n') {
              num_requests++;;
            }
          }

          if (num_requests == 0)
            continue; // partial request, 等 LT 下次通知

          // 为每个完整请求发送响应
          for (int r = 0; r < num_requests; r++) {
            send(fd, response, response_len, MSG_NOSIGNAL);
          }
          thread_counters[thread_id].count += num_requests;
        } else if (len == 0) {
          // 对端关闭连接
          close_conn(epoll_fd, fd);
        } else {
          // len < 0
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close_conn(epoll_fd, fd);
          }
          // EAGAIN 在 LT 模式下不应该发生，但安全起见忽略
        }
      }
    }
  }
  return NULL;
}

int main() {
  printf("Response length: %d bytes\n", response_len);

  pthread_t threads[NUM_THREADS];
  pthread_t stats_tid;
  int thread_ids[NUM_THREADS];

  // 启动统计线程
  pthread_create(&stats_tid, NULL, stats_thread, NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    thread_ids[i] = i;
    pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]);
  }

  printf("Multi-threaded server running with %d workers on port %d\n",
         NUM_THREADS, PORT);

  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}
