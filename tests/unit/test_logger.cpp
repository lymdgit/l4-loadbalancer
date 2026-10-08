// 日志单元测试：异步写文件、并发、队列满丢弃、轮转、reopen、DPDK 流
#include "common/logger.h"
#include "test.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace l4lb;

static std::string make_dir() {
  char tmpl[] = "/tmp/l4lb_ut_log.XXXXXX";
  return mkdtemp(tmpl);
}

static void remove_dir(const std::string &dir) {
  std::string cmd = "rm -rf '" + dir + "'";
  CHECK(system(cmd.c_str()) == 0);
}

static std::vector<std::string> read_lines(const std::string &path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  for (std::string line; std::getline(f, line);)
    out.push_back(line);
  return out;
}

static size_t count_with(const std::vector<std::string> &lines,
                         const std::string &needle) {
  size_t n = 0;
  for (const auto &l : lines)
    n += l.find(needle) != std::string::npos;
  return n;
}

static bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

/// "... rotate 0123" -> 123
static int seq(const std::string &line) {
  size_t p = line.rfind("rotate ");
  return p == std::string::npos ? -1 : atoi(line.c_str() + p + 7);
}

static LogFileOptions opts(const std::string &dir) {
  LogFileOptions o;
  o.dir = dir;
  o.also_stderr = false;
  o.max_size = 0;
  return o;
}

TEST(concurrent_writes_complete_lines) {
  std::string dir = make_dir();
  CHECK(Logger::instance().open_file(opts(dir + "/sub/l4")));  // mkdir -p
  constexpr int kThreads = 8, kPer = 500;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([t] {
      for (int i = 0; i < kPer; ++i)
        LOG_INFO("worker %d msg %d end", t, i);
    });
  for (auto &t : ts)
    t.join();
  Logger::instance().shutdown();
  auto lines = read_lines(dir + "/sub/l4/l4lb.log");
  CHECK_EQ(count_with(lines, "end"), static_cast<size_t>(kThreads * kPer));
  for (const auto &l : lines)
    CHECK(l.compare(0, 1, "[") == 0 && l.find("[INFO ]") != std::string::npos);
  CHECK_EQ(Logger::instance().dropped(), 0u);
  remove_dir(dir);
}

TEST(queue_full_drops_and_reports) {
  std::string dir = make_dir();
  LogFileOptions o = opts(dir);
  o.queue_cap = 16;
  CHECK(Logger::instance().open_file(o));
  uint64_t before = Logger::instance().dropped();
  constexpr int kTotal = 20000;
  for (int i = 0; i < kTotal; ++i)
    LOG_INFO("flood %d", i);
  Logger::instance().shutdown();
  uint64_t dropped = Logger::instance().dropped() - before;
  auto lines = read_lines(dir + "/l4lb.log");
  CHECK(dropped > 0);
  CHECK_EQ(count_with(lines, "flood ") + dropped, static_cast<uint64_t>(kTotal));
  CHECK(count_with(lines, "dropped (queue full)") >= 1);
  remove_dir(dir);
}

TEST(rotation_keeps_max_files) {
  std::string dir = make_dir();
  LogFileOptions o = opts(dir);
  o.max_size = 4096;
  o.max_files = 3;
  CHECK(Logger::instance().open_file(o));
  constexpr int kTotal = 1000;  // 每行约 90 字节，共约 90KB，轮转 20 多次
  for (int i = 0; i < kTotal; ++i) {
    LOG_INFO("rotate %04d", i);
    if (i % 100 == 0)
      usleep(2000);  // 避免压满队列
  }
  Logger::instance().shutdown();
  std::string base = dir + "/l4lb.log";
  CHECK(exists(base) && exists(base + ".1") && exists(base + ".2") &&
        exists(base + ".3"));
  CHECK(!exists(base + ".4"));
  struct stat st;
  for (const char *suf : {"", ".1", ".2", ".3"})
    if (stat((base + suf).c_str(), &st) == 0)
      CHECK(st.st_size <= 4096 + 256);
  // 最新的日志在 l4lb.log，次新的在 .1，并且首尾相接
  auto cur = read_lines(base);
  auto prev = read_lines(base + ".1");
  CHECK(!cur.empty() && cur.back().find("rotate 0999") != std::string::npos);
  CHECK(!prev.empty() && seq(prev.back()) + 1 == seq(cur.front()));
  CHECK(count_with(read_lines(base + ".3"), "rotate 0000") == 0);
  remove_dir(dir);
}

TEST(reopen_after_rename) {
  std::string dir = make_dir();
  CHECK(Logger::instance().open_file(opts(dir)));
  LOG_INFO("before rename");
  std::string base = dir + "/l4lb.log";
  usleep(1500 * 1000);  // 写线程 1 秒内写出
  CHECK(rename(base.c_str(), (base + ".old").c_str()) == 0);
  Logger::request_reopen();
  usleep(1500 * 1000);  // 写线程最多 1 秒后处理 reopen
  LOG_INFO("after rename");
  Logger::instance().shutdown();
  CHECK_EQ(count_with(read_lines(base + ".old"), "before rename"), 1u);
  auto now = read_lines(base);
  CHECK_EQ(count_with(now, "after rename"), 1u);
  CHECK_EQ(count_with(now, "log file reopened"), 1u);
  remove_dir(dir);
}

TEST(dpdk_stream_lines) {
  std::string dir = make_dir();
  CHECK(Logger::instance().open_file(opts(dir)));
  FILE *f = Logger::instance().dpdk_stream();
  CHECK(f != nullptr);
  if (f) {
    fprintf(f, "EAL: first line\nEAL: part");
    fflush(f);
    fprintf(f, "ial line\n");
    fflush(f);
    fclose(f);  // rte_eal_cleanup 的行为
  }
  Logger::instance().shutdown();
  auto lines = read_lines(dir + "/l4lb.log");
  CHECK_EQ(count_with(lines, "[dpdk] EAL: first line"), 1u);
  CHECK_EQ(count_with(lines, "[dpdk] EAL: partial line"), 1u);
  remove_dir(dir);
}

TEST(bad_dir_falls_back_to_stderr) {
  LogFileOptions o = opts("/proc/l4lb_no_such_dir");
  CHECK(!Logger::instance().open_file(o));
  CHECK(Logger::instance().dpdk_stream() == nullptr);
  LOG_INFO("still logging to stderr");
}

int main() { return ut::run_all(); }
