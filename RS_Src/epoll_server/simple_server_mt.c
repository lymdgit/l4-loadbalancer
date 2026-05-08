#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 80
#define MAX_EVENTS 1024
#define NUM_THREADS 4 // 根据 CPU 核心数调整

static volatile unsigned long long total_requests = 0;

void *stats_thread(void *arg) {
  (void)arg;
  unsigned long long last_requests = 0;
  while (1) {
    sleep(1);
    unsigned long long current_requests = total_requests;
    printf("[STATS] Total Requests: %llu, QPS: %llu\n", current_requests,
           current_requests - last_requests);
    last_requests = current_requests;
  }
  return NULL;
}

const char *response = "HTTP/1.1 200 OK\r\n"
                       "Content-Length: 2\r\n"
                       "Connection: keep-alive\r\n"
                       "\r\n"
                       "OK";
const int response_len = 64; // 正确长度

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void *worker_thread(void *arg) {
  int thread_id = *(int *)arg;

  // 每个线程创建自己的 socket (SO_REUSEPORT)
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    return NULL;
  }
  listen(server_fd, 4096);
  set_nonblock(server_fd);

  // 每个线程有自己的 epoll
  int epoll_fd = epoll_create1(0);
  struct epoll_event ev, events[MAX_EVENTS];
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

  printf("Worker %d started on port %d\n", thread_id, PORT);

  while (1) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;

      if (fd == server_fd) {
        // Accept all pending connections
        while (1) {
          int client_fd = accept(server_fd, NULL, NULL);
          if (client_fd < 0)
            break;
          set_nonblock(client_fd);
          // LT 模式：安全可靠，不需要 drain 循环
          ev.events = EPOLLIN;
          ev.data.fd = client_fd;
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        }
      } else {
        // 使用 LT 模式，recv 一次即可，未读完的数据下次 epoll 会再通知
        char buffer[4096];
        int len = recv(fd, buffer, sizeof(buffer), 0);
        if (len > 0) {
          // wrk 使用 keep-alive 管线化，一次 recv 可能读到多个 HTTP 请求
          // 必须数清楚有几个请求（按 \r\n\r\n 结尾计数），发对应数量的响应
          // 否则请求/响应管线错位 → 连接永久卡死
          int num_requests = 0;
          for (int j = 0; j <= len - 4; j++) {
            if (buffer[j] == '\r' && buffer[j + 1] == '\n' &&
                buffer[j + 2] == '\r' && buffer[j + 3] == '\n') {
              num_requests++;
            }
          }
          if (num_requests == 0)
            num_requests = 1;

          for (int r = 0; r < num_requests; r++) {
            int sent = send(fd, response, response_len, MSG_NOSIGNAL);
            if (sent <= 0) {
              // send buffer 满或连接断开，关闭连接避免僵死
              close(fd);
              goto next_event;
            }
          }
          __sync_fetch_and_add(&total_requests, num_requests);
        } else if (len == 0) {
          close(fd);
        } else if (errno != EAGAIN) {
          close(fd);
        }
      }
      next_event:;
    }
  }
  return NULL;
}

int main() {
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
