#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 80
#define QUEUE_DEPTH 4096
#define NUM_THREADS 4
#define NUM_ACCEPTS 64 // 同时挂多个 accept，加速建连

// Operation types
enum {
  OP_ACCEPT,
  OP_READ,
  OP_WRITE,
};

// Request context（每个活跃连接一个）
struct request {
  int fd;
  int type;
  int num_responses; // 待发送的响应数
  char buffer[1024];
};

// per-thread 计数器，避免 false sharing
typedef struct {
  unsigned long long count;
  char _pad[56];
} __attribute__((aligned(64))) per_thread_counter_t;

static per_thread_counter_t thread_counters[16];

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

// 用 sizeof 自动计算，彻底避免手算错误
static const char response[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Length: 2\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "OK";
static const int response_len = sizeof(response) - 1;

// ===== 提交 Accept 请求 =====
// addr 传 NULL：我们不需要客户端地址，避免共享内存竞态
static void add_accept_request(struct io_uring *ring, int server_fd,
                               struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe)
    return;
  io_uring_prep_accept(sqe, server_fd, NULL, NULL, 0);
  req->type = OP_ACCEPT;
  req->fd = server_fd;
  io_uring_sqe_set_data(sqe, req);
}

// ===== 提交 Recv 请求 =====
static void add_read_request(struct io_uring *ring, struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    close(req->fd);
    free(req);
    return;
  }
  req->type = OP_READ;
  io_uring_prep_recv(sqe, req->fd, req->buffer, sizeof(req->buffer), 0);
  io_uring_sqe_set_data(sqe, req);
}

// ===== 提交 Send 请求 =====
static void add_write_request(struct io_uring *ring, struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    close(req->fd);
    free(req);
    return;
  }
  req->type = OP_WRITE;
  io_uring_prep_send(sqe, req->fd, response, response_len, MSG_NOSIGNAL);
  io_uring_sqe_set_data(sqe, req);
}

// 统计 buffer 中 \r\n\r\n 的数量（完整 HTTP 请求数）
static int count_requests(const char *buf, int len) {
  int count = 0;
  for (int i = 0; i <= len - 4; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      count++;
    }
  }
  return count;
}

void *worker_thread(void *arg) {
  int thread_id = *(int *)arg;

  // CPU 亲和性绑定
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(thread_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  // 创建 io_uring
  struct io_uring ring;
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  // 注意：SQPOLL 需要 root 权限或 CAP_SYS_NICE
  // 如果不想要求权限，去掉这一行即可
  // params.flags = IORING_SETUP_SQPOLL;
  // params.sq_thread_idle = 1000; // 空闲 1ms 后休眠

  int ret = io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params);
  if (ret < 0) {
    fprintf(stderr, "Worker %d: queue_init failed: %s\n", thread_id,
            strerror(-ret));
    return NULL;
  }

  // 创建监听 socket，SO_REUSEPORT
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  // TCP_DEFER_ACCEPT：内核收到数据后才唤醒 accept
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

  printf("Worker %d started on port %d (CPU %d)\n", thread_id, PORT,
         thread_id % (int)sysconf(_SC_NPROCESSORS_ONLN));

  // 预分配 accept 请求（不释放，循环复用）
  struct request *accept_reqs[NUM_ACCEPTS];
  for (int i = 0; i < NUM_ACCEPTS; i++) {
    accept_reqs[i] = malloc(sizeof(struct request));
    add_accept_request(&ring, server_fd, accept_reqs[i]);
  }
  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;

  while (1) {
    ret = io_uring_submit_and_wait(&ring, 1);
    if (ret < 0) {
      if (ret == -EINTR)
        continue;
      fprintf(stderr, "submit_and_wait: %s\n", strerror(-ret));
      continue;
    }

    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring, head, cqe) {
      count++;
      struct request *req = (struct request *)io_uring_cqe_get_data(cqe);
      int res = cqe->res;

      switch (req->type) {
      case OP_ACCEPT: {
        if (res >= 0) {
          int client_fd = res;

          // 禁用 Nagle
          int nodelay = 1;
          setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     sizeof(nodelay));

          // 为新连接分配 request 并提交 recv
          struct request *read_req = malloc(sizeof(struct request));
          if (read_req) {
            read_req->fd = client_fd;
            add_read_request(&ring, read_req);
          } else {
            close(client_fd);
          }
        }
        // 重新挂上 accept（复用同一个 req，不 malloc/free）
        add_accept_request(&ring, server_fd, req);
        break;
      }

      case OP_READ: {
        if (res > 0) {
          // 统计本次 recv 包含多少个完整 HTTP 请求
          int num = count_requests(req->buffer, res);
          if (num > 0) {
            req->num_responses = num;
            // 先发第一个响应，后续响应在 WRITE 完成后链式发送
            req->num_responses--;
            add_write_request(&ring, req);
            thread_counters[thread_id].count += 1;
          } else {
            // partial request，继续读
            add_read_request(&ring, req);
          }
        } else {
          // 连接关闭或错误
          close(req->fd);
          free(req);
        }
        break;
      }

      case OP_WRITE: {
        if (res > 0) {
          if (req->num_responses > 0) {
            // 还有剩余响应要发
            req->num_responses--;
            add_write_request(&ring, req);
            thread_counters[thread_id].count += 1;
          } else {
            // 所有响应发完，继续读下一个请求（keep-alive）
            add_read_request(&ring, req);
          }
        } else {
          // send 失败，关闭连接
          close(req->fd);
          free(req);
        }
        break;
      }
      }
    }

    io_uring_cq_advance(&ring, count);
  }

  io_uring_queue_exit(&ring);
  return NULL;
}

int main() {
  printf("io_uring server | response_len=%d bytes\n", response_len);

  pthread_t threads[NUM_THREADS];
  pthread_t stats_tid;
  int thread_ids[NUM_THREADS];

  pthread_create(&stats_tid, NULL, stats_thread, NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    thread_ids[i] = i;
    pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]);
  }

  printf("io_uring server running with %d workers on port %d\n", NUM_THREADS,
         PORT);

  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}
