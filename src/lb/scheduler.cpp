/**
 * @file scheduler.cpp
 * @brief WRR 与 Maglev 调度实现
 */

#include "lb/scheduler.h"

#include <algorithm>
#include <numeric>

#include <rte_hash_crc.h>
#include <rte_jhash.h>

namespace l4lb {

void Scheduler::build(SchedulerType type,
                      const std::vector<SchedBackend> &backends) {
  type_ = type;
  table_.clear();
  bool any = false;
  for (const auto &b : backends)
    any |= b.weight > 0;
  if (!any)
    return;
  if (type == SchedulerType::MAGLEV)
    build_maglev(backends);
  else
    build_wrr(backends);
  // 各 worker 从序列的不同位置开始，避免同时启动时都打到同一个 RS
  for (size_t i = 0; i < cursor_.size(); ++i)
    cursor_[i].v = static_cast<uint32_t>(i * 7919u);
}

int Scheduler::pick(const FiveTuple &tuple, unsigned worker) const {
  if (table_.empty())
    return -1;
  if (type_ == SchedulerType::MAGLEV) {
    uint32_t w[3];
    tuple.words(w);
    uint32_t h = rte_jhash_3words(w[0], w[1], w[2], 0x9e3779b9);
    return table_[h % table_.size()];
  }
  uint32_t &c = cursor_[worker % cursor_.size()].v;
  return table_[c++ % table_.size()];
}

std::vector<uint32_t> Scheduler::distribution(size_t n_backends) const {
  std::vector<uint32_t> d(n_backends, 0);
  for (auto i : table_)
    if (i < n_backends)
      ++d[i];
  return d;
}

// ---------------------------------------------------------------------------
// 平滑加权轮询（nginx 算法）：每轮所有后端 current += weight，选 current 最大者，
// 被选中者 current -= total。展开成序列后，相同权重的后端交替出现，不会连续命中。
// ---------------------------------------------------------------------------
void Scheduler::build_wrr(const std::vector<SchedBackend> &b) {
  std::vector<uint64_t> w(b.size());
  uint64_t total = 0, g = 0;
  for (size_t i = 0; i < b.size(); ++i) {
    w[i] = b[i].weight;
    total += w[i];
    g = std::gcd(g, w[i]);
  }
  for (auto &x : w)
    x /= g;
  total /= g;
  // 序列太长时按比例缩小权重（至少保留 1）
  if (total > kWrrMaxSeq) {
    uint64_t t2 = 0;
    for (auto &x : w) {
      if (x)
        x = std::max<uint64_t>(1, x * kWrrMaxSeq / total);
      t2 += x;
    }
    total = t2;
  }

  std::vector<int64_t> cur(b.size(), 0);
  table_.reserve(total);
  for (uint64_t n = 0; n < total; ++n) {
    size_t best = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      cur[i] += static_cast<int64_t>(w[i]);
      if (w[i] && (w[best] == 0 || cur[i] > cur[best]))
        best = i;
    }
    cur[best] -= static_cast<int64_t>(total);
    table_.push_back(static_cast<uint16_t>(best));
  }
}

// ---------------------------------------------------------------------------
// Maglev（Google, NSDI'16）：每个后端由 (ip, port) 生成一个排列
//   offset = h1 % M, skip = h2 % (M - 1) + 1, perm[j] = (offset + j * skip) % M
// 各后端轮流按自己的排列抢占第一个空槽，直到 M 个槽位填满。
// 加权：每轮每个后端累加 weight / max_weight 的额度，额度满 1 才抢一个槽。
// ---------------------------------------------------------------------------
void Scheduler::build_maglev(const std::vector<SchedBackend> &b) {
  const uint32_t M = kMaglevSize;
  const size_t n = b.size();
  std::vector<uint32_t> offset(n), skip(n), next(n, 0);
  std::vector<double> credit(n, 0.0);
  uint32_t max_w = 0;
  for (size_t i = 0; i < n; ++i) {
    uint32_t key[2] = {b[i].ip, b[i].port};
    offset[i] = rte_jhash_32b(key, 2, 0x2545F491) % M;
    skip[i] = rte_jhash_32b(key, 2, 0x7A3B1C5D) % (M - 1) + 1;
    max_w = std::max(max_w, b[i].weight);
  }

  const uint16_t kEmpty = UINT16_MAX;
  table_.assign(M, kEmpty);
  uint32_t filled = 0;
  while (filled < M) {
    for (size_t i = 0; i < n && filled < M; ++i) {
      if (b[i].weight == 0)
        continue;
      credit[i] += static_cast<double>(b[i].weight) / max_w;
      if (credit[i] < 1.0)
        continue;
      credit[i] -= 1.0;
      // 沿排列找到第一个空槽
      uint32_t c;
      do {
        c = static_cast<uint32_t>(
            (offset[i] + static_cast<uint64_t>(next[i]) * skip[i]) % M);
        ++next[i];
      } while (table_[c] != kEmpty);
      table_[c] = static_cast<uint16_t>(i);
      ++filled;
    }
  }
}

} // namespace l4lb
