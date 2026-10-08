/**
 * @file logger.cpp
 * @brief 日志系统实现
 */

#include "common/logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>

#include <rte_log.h>

namespace l4lb {

namespace {

/// SIGHUP 置位，写线程处理；必须 lock-free 才能在信号处理函数里写
std::atomic<bool> g_reopen{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "g_reopen is written from a signal handler");

/// 积攒到这么多条时提前唤醒写线程，否则最多 1 秒写一次
constexpr size_t kWakeBatch = 256;

std::string format_line(const char *level_str, const char *where,
                        const char *msg, size_t len) {
  // 时间戳在调用时取，不受写线程延迟影响
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_buf;
  localtime_r(&ts.tv_sec, &tm_buf);
  char time_str[40];
  size_t tl = strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
  snprintf(time_str + tl, sizeof(time_str) - tl, ".%03ld",
           ts.tv_nsec / 1000000);

  std::string out;
  out.reserve(len + 96);
  out.append("[").append(time_str).append("] [").append(level_str)
      .append("] [").append(where).append("] ").append(msg, len)
      .push_back('\n');
  return out;
}

/// mkdir -p
bool make_dirs(const std::string &dir) {
  std::string cur;
  size_t pos = 0;
  while (pos != std::string::npos) {
    pos = dir.find('/', pos + 1);
    cur = dir.substr(0, pos);
    if (cur.empty())
      continue;
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
      return false;
  }
  struct stat st;
  return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// DPDK 日志级别（RTE_LOG_EMERG=1 ... RTE_LOG_DEBUG=8）映射到本模块级别
LogLevel from_rte_level(int lv) {
  if (lv <= 0)
    return LogLevel::INFO; // 未经 rte_log 的直接写入
  if (lv <= static_cast<int>(RTE_LOG_ERR))
    return LogLevel::ERROR;
  if (lv == static_cast<int>(RTE_LOG_WARNING))
    return LogLevel::WARN;
  if (lv == static_cast<int>(RTE_LOG_DEBUG))
    return LogLevel::DEBUG;
  return LogLevel::INFO;
}

} // namespace

// ============================================================================
// 异步写：调用方 push 入队，写线程批量取出写文件
// ============================================================================

struct Logger::Sink {
  // ---- 调用方与写线程共享，mu 保护 ----
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> queue;
  size_t cap = 0;
  bool urgent = false; ///< 有 ERROR 以上的日志，立即写并刷盘
  bool stop = false;
  std::atomic<bool> active{false}; ///< 写线程在运行，日志走队列
  std::atomic<uint64_t> dropped{0};      ///< 累计丢弃
  std::atomic<uint64_t> drop_unreported{0};

  // ---- 只由写线程访问（open/shutdown 时线程未运行）----
  std::thread thread;
  LogFileOptions opts;
  std::string path;
  FILE *file = nullptr;
  uint64_t size = 0;

  // ---- DPDK 适配：cookie 回调在 FILE 锁内调用，已串行 ----
  FILE *dpdk = nullptr;
  std::string dpdk_partial;

  /// @return false 异步写未开启，调用方自己写 stderr
  bool push(std::string &&line, bool is_urgent) {
    if (!active.load(std::memory_order_acquire))
      return false;
    std::lock_guard<std::mutex> lk(mu);
    if (!active.load(std::memory_order_relaxed))
      return false;
    if (queue.size() >= cap) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      drop_unreported.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    queue.push_back(std::move(line));
    // 只在需要时唤醒，平时由写线程 1 秒超时自己醒来
    if (is_urgent || queue.size() == kWakeBatch) {
      urgent = urgent || is_urgent;
      cv.notify_one();
    }
    return true;
  }

  bool open_current() {
    file = fopen(path.c_str(), "a");
    if (!file)
      return false;
    fseek(file, 0, SEEK_END);
    long pos = ftell(file);
    size = pos > 0 ? static_cast<uint64_t>(pos) : 0;
    return true;
  }

  /// 写线程内部的提示信息（文件打不开等），只能写 stderr 或文件
  void notice(LogLevel level, const char *fmt, ...)
      __attribute__((format(printf, 3, 4))) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    size_t len = n < 0 ? 0 : std::min(static_cast<size_t>(n), sizeof(msg) - 1);
    write_line(format_line(Logger::get_level_str(level), "logger", msg,
                           len));
  }

  /// l4lb.log -> .1 -> .2 ... 最旧的删除，再新建 l4lb.log
  void rotate() {
    if (file)
      fclose(file);
    file = nullptr;
    if (opts.max_files == 0) {
      remove(path.c_str());
    } else {
      std::string oldest = path + "." + std::to_string(opts.max_files);
      remove(oldest.c_str());
      for (uint32_t i = opts.max_files; i > 1; --i) {
        std::string from = path + "." + std::to_string(i - 1);
        std::string to = path + "." + std::to_string(i);
        rename(from.c_str(), to.c_str()); // 不存在时失败，忽略
      }
      rename(path.c_str(), (path + ".1").c_str());
    }
    if (!open_current())
      notice(LogLevel::ERROR, "cannot open %s after rotation: %s", path.c_str(),
             strerror(errno));
  }

  void write_line(const std::string &line) {
    // 文件暂时打不开时也写 stderr，日志不丢
    if (opts.also_stderr || !file)
      fwrite(line.data(), 1, line.size(), stderr);
    if (!file)
      return;
    fwrite(line.data(), 1, line.size(), file);
    size += line.size();
    if (opts.max_size && size >= opts.max_size)
      rotate();
  }

  void flush() {
    if (file)
      fflush(file);
    fflush(stderr);
  }

  void run() {
    std::vector<std::string> batch;
    for (;;) {
      bool stopping;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(1), [this] {
          return stop || urgent || queue.size() >= kWakeBatch;
        });
        batch.swap(queue);
        urgent = false;
        stopping = stop;
      }
      if (g_reopen.exchange(false, std::memory_order_relaxed)) {
        if (file)
          fclose(file);
        if (open_current())
          notice(LogLevel::INFO, "log file reopened: %s", path.c_str());
        else
          notice(LogLevel::ERROR, "cannot reopen %s: %s", path.c_str(),
                 strerror(errno));
      }
      if (!file && !path.empty() && open_current()) // 之前打开失败，重试
        notice(LogLevel::INFO, "log file reopened: %s", path.c_str());
      uint64_t d = drop_unreported.exchange(0, std::memory_order_relaxed);
      if (d)
        notice(LogLevel::WARN, "%lu log messages dropped (queue full)", d);
      for (const auto &line : batch)
        write_line(line);
      batch.clear();
      flush();
      // stop 置位后不会再有新日志入队（active 已清零），本批即最后一批
      if (stopping)
        break;
    }
  }

  // ---- DPDK cookie FILE 回调：按行拆分后送入日志队列 ----
  static ssize_t cookie_write(void *cookie, const char *buf, size_t n) {
    auto *s = static_cast<Sink *>(cookie);
    s->dpdk_partial.append(buf, n);
    // rte_vlog 在同一线程写完后 fflush，此时取到的就是这条消息的级别
    LogLevel level = from_rte_level(rte_log_cur_msg_loglevel());
    size_t start = 0, nl;
    while ((nl = s->dpdk_partial.find('\n', start)) != std::string::npos) {
      if (nl > start)
        Logger::instance().write_raw(level, "dpdk",
                                     s->dpdk_partial.data() + start, nl - start);
      start = nl + 1;
    }
    s->dpdk_partial.erase(0, start);
    return static_cast<ssize_t>(n);
  }

  static int cookie_close(void *cookie) {
    auto *s = static_cast<Sink *>(cookie);
    if (!s->dpdk_partial.empty())
      Logger::instance().write_raw(LogLevel::INFO, "dpdk",
                                   s->dpdk_partial.data(),
                                   s->dpdk_partial.size());
    s->dpdk_partial.clear();
    s->dpdk = nullptr;
    return 0;
  }
};

Logger::Logger() : sink_(new Sink), level_(LogLevel::INFO) {}

Logger::~Logger() { shutdown(); }

bool Logger::open_file(const LogFileOptions &opts) {
  shutdown();
  Sink &s = *sink_;
  if (!make_dirs(opts.dir)) {
    LOG_WARN("cannot create log dir %s: %s; logging to stderr only",
             opts.dir.c_str(), strerror(errno));
    return false;
  }
  s.opts = opts;
  s.path = opts.dir;
  if (s.path.back() != '/')
    s.path.push_back('/');
  s.path += opts.name;
  if (!s.open_current()) {
    LOG_WARN("cannot open log file %s: %s; logging to stderr only",
             s.path.c_str(), strerror(errno));
    s.path.clear();
    return false;
  }
  s.cap = opts.queue_cap ? opts.queue_cap : 1;
  s.queue.reserve(std::min(s.cap, kWakeBatch * 2));
  s.stop = false;
  s.urgent = false;
  g_reopen.store(false, std::memory_order_relaxed);
  s.thread = std::thread([&s] { s.run(); });
  pthread_setname_np(s.thread.native_handle(), "l4lb-log");
  s.active.store(true, std::memory_order_release);
  return true;
}

void Logger::shutdown() {
  Sink &s = *sink_;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.active.load(std::memory_order_relaxed))
      return;
    // 之后的日志同步写 stderr；队列里已有的由写线程写完
    s.active.store(false, std::memory_order_release);
    s.stop = true;
    s.cv.notify_one();
  }
  s.thread.join();
  if (s.file)
    fclose(s.file);
  s.file = nullptr;
  s.path.clear();
}

void Logger::request_reopen() { g_reopen.store(true, std::memory_order_relaxed); }

uint64_t Logger::dropped() const {
  return sink_->dropped.load(std::memory_order_relaxed);
}

FILE *Logger::dpdk_stream() {
  Sink &s = *sink_;
  if (!s.active.load(std::memory_order_acquire))
    return nullptr;
  if (!s.dpdk) {
    cookie_io_functions_t fns{};
    fns.write = Sink::cookie_write;
    fns.close = Sink::cookie_close;
    s.dpdk = fopencookie(&s, "w", fns);
  }
  return s.dpdk;
}

void Logger::set_level(const std::string &level_str) {
  if (level_str == "debug")
    set_level(LogLevel::DEBUG);
  else if (level_str == "info")
    set_level(LogLevel::INFO);
  else if (level_str == "warn")
    set_level(LogLevel::WARN);
  else if (level_str == "error")
    set_level(LogLevel::ERROR);
  else if (level_str == "fatal")
    set_level(LogLevel::FATAL);
  else if (level_str == "off")
    set_level(LogLevel::OFF);
}

void Logger::log(LogLevel level, const char *file, int line, const char *func,
                 const char *fmt, ...) {
  // 级别过滤
  if (!is_enabled(level)) {
    return;
  }

  // 从文件路径中提取文件名
  const char *filename = file;
  const char *p = file;
  while (*p) {
    if (*p == '/' || *p == '\\') {
      filename = p + 1;
    }
    ++p;
  }
  char where[256];
  snprintf(where, sizeof(where), "%s:%d %s", filename, line, func);

  // 格式化用户消息
  char msg_buf[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);
  if (n < 0)
    n = 0;
  size_t len = std::min(static_cast<size_t>(n), sizeof(msg_buf) - 1);

  emit(level, where, msg_buf, len);
}

void Logger::write_raw(LogLevel level, const char *tag, const char *msg,
                       size_t len) {
  emit(level, tag, msg, len);
}

void Logger::emit(LogLevel level, const char *where, const char *msg,
                  size_t len) {
  std::string out = format_line(get_level_str(level), where, msg, len);
  // push 只在入队成功时才移走 out
  if (sink_->push(std::move(out), level >= LogLevel::ERROR))
    return;
  // 异步写未开启：同步写 stderr（启动早期、shutdown 之后）
  std::lock_guard<std::mutex> lock(mutex_);
  fwrite(out.data(), 1, out.size(), stderr);
  fflush(stderr);
}

const char *Logger::get_level_str(LogLevel level) {
  switch (level) {
  case LogLevel::DEBUG:
    return "DEBUG";
  case LogLevel::INFO:
    return "INFO ";
  case LogLevel::WARN:
    return "WARN ";
  case LogLevel::ERROR:
    return "ERROR";
  case LogLevel::FATAL:
    return "FATAL";
  default:
    return "?????";
  }
}

bool log_ratelimit_pass(std::atomic<int64_t> &last, int64_t interval_sec) {
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  int64_t prev = last.load(std::memory_order_relaxed);
  if (prev != 0 && now - prev < interval_sec)
    return false;
  // 多核同时触发时只放行一个
  return last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

} // namespace l4lb
