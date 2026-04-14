#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 80
#define QUEUE_DEPTH 4096 // io_uring queue depth
#define NUM_THREADS 4    // Match simple_server_mt.c

// Operation types for user_data
enum {
  OP_ACCEPT,
  OP_READ,
  OP_WRITE,
};

// Request context to track state across async operations
struct request {
  int fd;
  int type;
  char buffer[1024];
  struct iovec iov[1];
  struct msghdr msg; // For recvmsg/sendmsg if needed, but we use read/write
};

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
const int response_len = 64;

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Helper to get a fresh request structure
struct request *get_request(int fd, int type) {
  struct request *req = malloc(sizeof(struct request));
  if (!req)
    return NULL;
  req->fd = fd;
  req->type = type;
  return req;
}

// Add Accept request to ring
void add_accept_request(struct io_uring *ring, int server_fd,
                        struct sockaddr_in *client_addr, socklen_t *client_len,
                        struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE (Accept)\n");
    return;
  }
  io_uring_prep_accept(sqe, server_fd, (struct sockaddr *)client_addr,
                       client_len, 0);
  req->type = OP_ACCEPT;
  req->fd = server_fd; // Keep server fd here
  io_uring_sqe_set_data(sqe, req);
}

// Add Read request
void add_read_request(struct io_uring *ring, int client_fd) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE (Read)\n");
    return;
  }
  struct request *req = get_request(client_fd, OP_READ);
  if (!req)
    return;

  io_uring_prep_recv(sqe, client_fd, req->buffer, sizeof(req->buffer), 0);
  io_uring_sqe_set_data(sqe, req);
}

// Add Write request
void add_write_request(struct io_uring *ring, int client_fd,
                       struct request *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    fprintf(stderr, "Could not get SQE (Write)\n");
    return;
  }

  req->type = OP_WRITE;
  io_uring_prep_send(sqe, client_fd, response, response_len, 0);
  io_uring_sqe_set_data(sqe, req);
}

void *worker_thread(void *arg) {
  int thread_id = *(int *)arg;
  struct io_uring ring;
  int ret;

  // Create io_uring
  ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret < 0) {
    fprintf(stderr, "queue_init: %s\n", strerror(-ret));
    return NULL;
  }

  // Create Listen Socket with SO_REUSEPORT
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
  listen(server_fd, 1024);

  printf("Worker %d started on port %d\n", thread_id, PORT);

  // Initial Accept Submission
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  struct request *accept_req =
      malloc(sizeof(struct request)); // Permanent accept req
  add_accept_request(&ring, server_fd, &client_addr, &client_len, accept_req);

  // Initial submit not strictly necessary if we use submit_and_wait, but good
  // for starting
  io_uring_submit(&ring);

  struct io_uring_cqe *cqe;
  while (1) {
    // Wait for at least 1 event, and assume we submit whatever is in SQ
    ret = io_uring_submit_and_wait(&ring, 1);
    if (ret < 0) {
      fprintf(stderr, "submit_and_wait: %s\n", strerror(-ret));
      continue;
    }

    unsigned head;
    unsigned count = 0;

    // Process all available completions without extra syscalls
    io_uring_for_each_cqe(&ring, head, cqe) {
      count++;
      struct request *req = (struct request *)io_uring_cqe_get_data(cqe);
      int res = cqe->res;

      if (req->type == OP_ACCEPT) {
        if (res >= 0) {
          add_read_request(&ring, res); // res is client_fd
        } else {
          fprintf(stderr, "Accept failed: %d\n", res);
        }
        // Always re-arm accept to keep server alive
        add_accept_request(&ring, server_fd, &client_addr, &client_len, req);

      } else if (req->type == OP_READ) {
        if (res > 0) {
          add_write_request(&ring, req->fd, req);
        } else {
          // 0 = closed, < 0 = error
          close(req->fd);
          free(req);
        }

      } else if (req->type == OP_WRITE) {
        if (res > 0) {
          __sync_fetch_and_add(&total_requests, 1);
          // Keep-alive: read again
          req->type = OP_READ;
          struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
          if (sqe) {
            io_uring_prep_recv(sqe, req->fd, req->buffer, sizeof(req->buffer),
                               0);
            io_uring_sqe_set_data(sqe, req);
          } else {
            close(req->fd);
            free(req);
          }
        } else {
          close(req->fd);
          free(req);
        }
      }
    }

    // Advance CQ ring
    io_uring_cq_advance(&ring, count);
  }

  io_uring_queue_exit(&ring);
  return NULL;
}

int main() {
  pthread_t threads[NUM_THREADS];
  pthread_t stats_tid;
  int thread_ids[NUM_THREADS];

  // stats
  pthread_create(&stats_tid, NULL, stats_thread, NULL);

  for (int i = 0; i < NUM_THREADS; i++) {
    thread_ids[i] = i;
    pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]);
  }

  printf("IO_Uring server running with %d workers on port %d\n", NUM_THREADS,
         PORT);

  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
  return 0;
}
