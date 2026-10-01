/**
 * @file logger.cpp
 * @brief 日志系统实现
 */

#include "common/logger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace l4lb {

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

  // 获取当前时间
  time_t now = time(nullptr);
  struct tm tm_buf;
  localtime_r(&now, &tm_buf);

  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

  // 获取级别字符串
  const char *level_str = get_level_str(level);

  // 从文件路径中提取文件名
  const char *filename = file;
  const char *p = file;
  while (*p) {
    if (*p == '/' || *p == '\\') {
      filename = p + 1;
    }
    ++p;
  }

  // 格式化用户消息
  char msg_buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  // 加锁输出
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fprintf(stderr, "[%s] [%s] [%s:%d %s] %s\n", time_str, level_str, filename,
            line, func, msg_buf);
    fflush(stderr);
  }
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
