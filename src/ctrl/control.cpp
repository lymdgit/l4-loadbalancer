/**
 * @file control.cpp
 * @brief 控制面实现
 */

#include "ctrl/control.h"

#include "common/logger.h"
#include "ctrl/snapshot.h"
#include "dataplane/context.h"
#include "dataplane/worker.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/dpdk_ring.h"

namespace l4lb {

namespace {

bool parse_u32(const std::string &s, uint32_t &out) {
  if (s.empty())
    return false;
  char *end = nullptr;
  unsigned long v = strtoul(s.c_str(), &end, 10);
  if (*end || v > UINT32_MAX)
    return false;
  out = static_cast<uint32_t>(v);
  return true;
}

/// ip:port[:weight]
bool parse_rs_spec(const std::string &spec, RsConf &rs) {
  std::vector<std::string> parts;
  std::stringstream ss(spec);
  for (std::string p; std::getline(ss, p, ':');)
    parts.push_back(p);
  uint32_t port = 0;
  if (parts.size() < 2 || parts.size() > 3 || !parse_ipv4(parts[0], rs.ip) ||
      !parse_u32(parts[1], port) || port == 0 || port > 65535)
    return false;
  rs.port = static_cast<uint16_t>(port);
  if (parts.size() == 3 && (!parse_u32(parts[2], rs.weight) || rs.weight > 65535))
    return false;
  return true;
}

const char *kHelp =
    "commands:\n"
    "  stats [-v|-r]                  summary; -v per worker; -r per service/RS\n"
    "  counters                       all raw counters (key value), for delta\n"
    "  rate                           last periodic rates and busy %\n"
    "  services                       services and real servers\n"
    "  weight <rs_id> <weight>        0 = drain (no new connections)\n"
    "  enable <rs_id> | disable <rs_id>\n"
    "  add <service_idx> <ip:port[:weight]>\n"
    "  del <rs_id>\n"
    "  log <debug|info|warn|error>\n"
    "  quit                           stop l4lb\n";

} // namespace

std::string ControlServer::execute(const std::string &line) {
  std::istringstream is(line);
  std::vector<std::string> a;
  for (std::string t; is >> t;)
    a.push_back(t);
  if (a.empty() || a[0] == "help")
    return kHelp;

  auto &mgr = g_dp.snapshots;
  const std::string &cmd = a[0];
  std::string err;
  uint32_t id = 0, w = 0;

  if (cmd == "stats" && a.size() > 1 && a[1] == "-r")
    return format_rs_stats();
  if (cmd == "stats")
    return format_stats(a.size() > 1 && a[1] == "-v");
  if (cmd == "counters")
    return format_counters();
  if (cmd == "rate")
    return perf_report_last();
  if (cmd == "services")
    return mgr.describe();
  if (cmd == "weight" && a.size() == 3 && parse_u32(a[1], id) &&
      parse_u32(a[2], w)) {
    err = mgr.set_weight(id, w);
  } else if ((cmd == "enable" || cmd == "disable") && a.size() == 2 &&
             parse_u32(a[1], id)) {
    err = mgr.set_enabled(id, cmd == "enable");
  } else if (cmd == "add" && a.size() == 3 && parse_u32(a[1], id)) {
    RsConf rs;
    if (!parse_rs_spec(a[2], rs))
      return "error: expected ip:port[:weight]\n";
    uint32_t new_id = 0;
    err = mgr.add_rs(static_cast<uint16_t>(id), rs, new_id);
    if (err.empty()) {
      mgr.publish();
      return "ok rs " + std::to_string(new_id) + "\n";
    }
  } else if (cmd == "del" && a.size() == 2 && parse_u32(a[1], id)) {
    err = mgr.del_rs(id);
  } else if (cmd == "log" && a.size() == 2) {
    Logger::instance().set_level(a[1]);
    return "ok\n";
  } else if (cmd == "quit") {
    g_running.store(false, std::memory_order_relaxed);
    return "ok, shutting down\n";
  } else {
    return "error: bad command\n" + std::string(kHelp);
  }
  if (!err.empty())
    return "error: " + err + "\n";
  mgr.publish();
  return "ok\n";
}

bool ControlServer::start(const std::string &path) {
  path_ = path;
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    LOG_ERROR("control socket path too long: %s", path.c_str());
    return false;
  }
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("control socket: %s", strerror(errno));
    return false;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  unlink(path.c_str()); // 上次异常退出残留的 socket 文件
  mode_t old = umask(0177);  // 0600：只有 root 能连接
  int ret = bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  umask(old);
  if (ret < 0 || listen(listen_fd_, 8) < 0) {
    LOG_ERROR("control socket %s: %s", path.c_str(), strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  // 控制线程：DPDK 把它绑到 -l 之外的 CPU，不与 receiver / worker 抢核
  if (rte_thread_create_control(&thread_, "l4lb-ctl", thread_main, this) != 0) {
    LOG_ERROR("control socket: failed to create thread");
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  started_ = true;
  LOG_INFO("Control socket: %s", path.c_str());
  return true;
}

uint32_t ControlServer::thread_main(void *self) {
  static_cast<ControlServer *>(self)->run();
  return 0;
}

void ControlServer::stop() {
  stop_.store(true);
  if (started_)
    rte_thread_join(thread_, nullptr);
  started_ = false;
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    unlink(path_.c_str());
    listen_fd_ = -1;
  }
}

void ControlServer::drain_health() {
  HealthEvent evs[64];
  unsigned n = rte_ring_sc_dequeue_burst_elem(g_dp.health_ring, evs,
                                              sizeof(HealthEvent), 64, nullptr);
  if (n == 0)
    return;
  for (unsigned i = 0; i < n; ++i)
    g_dp.snapshots.set_health(evs[i].rs_id, evs[i].healthy);
  g_dp.snapshots.publish();
}

void ControlServer::run() {
  while (!stop_.load()) {
    drain_health();
    struct pollfd pfd = {listen_fd_, POLLIN, 0};
    if (poll(&pfd, 1, 100) <= 0)
      continue;
    int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0)
      continue;
    // 读一行命令（最多 1KB，等待 1 秒）
    std::string line;
    char buf[256];
    struct pollfd cfd = {fd, POLLIN, 0};
    while (line.find('\n') == std::string::npos && line.size() < 1024 &&
           poll(&cfd, 1, 1000) > 0) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0)
        break;
      line.append(buf, n);
    }
    std::string reply = execute(line.substr(0, line.find('\n')));
    for (size_t off = 0; off < reply.size();) {
      ssize_t n = write(fd, reply.data() + off, reply.size() - off);
      if (n <= 0)
        break;
      off += n;
    }
    close(fd);
  }
}

} // namespace l4lb
