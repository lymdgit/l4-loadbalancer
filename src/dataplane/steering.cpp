/**
 * @file steering.cpp
 * @brief 多核分发实现
 */

#include "dataplane/steering.h"

#include <cstring>

#include <rte_byteorder.h>
#include <rte_jhash.h>
#include <rte_thash.h>

namespace l4lb {

const uint8_t Steering::kDefaultRssKey[kRssKeyLen] = {
    0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2, 0x41, 0x67,
    0x25, 0x3D, 0x43, 0xA3, 0x8F, 0xB0, 0xD0, 0xCA, 0x2B, 0xCB,
    0xAE, 0x7B, 0x30, 0xB4, 0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30,
    0xF2, 0x0C, 0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA,
};

void Steering::init_sw(uint16_t num_workers) {
  hw_ = false;
  tcp_l4_ = udp_l4_ = false;
  n_ = num_workers ? num_workers : 1;
}

void Steering::init_hw(uint16_t num_workers, const uint8_t *rss_key,
                       const uint16_t *reta, uint16_t reta_size, bool tcp_l4,
                       bool udp_l4) {
  hw_ = true;
  n_ = num_workers;
  tcp_l4_ = tcp_l4;
  udp_l4_ = udp_l4;
  reta_size_ = reta_size;
  // worker 下标 == 队列号（main.cpp 按此分配），RETA 存的就是 owner
  for (uint16_t i = 0; i < reta_size && i < kMaxReta; ++i)
    reta_owner_[i] = static_cast<uint16_t>(reta[i] % n_);
  // rte_softrss_be 要求 key 预先按 32 位字转换字节序
  rte_convert_rss_key(reinterpret_cast<const uint32_t *>(rss_key),
                      reinterpret_cast<uint32_t *>(key_be_), kRssKeyLen);
}

uint32_t Steering::rss_hash(const FiveTuple &t, bool l4) const {
  // Toeplitz 输入：src_ip, dst_ip, (src_port << 16 | dst_port)，均为主机字节序
  uint32_t in[3];
  in[0] = rte_be_to_cpu_32(t.src_ip);
  in[1] = rte_be_to_cpu_32(t.dst_ip);
  in[2] = (static_cast<uint32_t>(rte_be_to_cpu_16(t.src_port)) << 16) |
          rte_be_to_cpu_16(t.dst_port);
  return rte_softrss_be(in, l4 ? 3 : 2, key_be_);
}

uint16_t Steering::fwd_owner(const FiveTuple &t) const {
  if (n_ == 1)
    return 0;
  if (!hw_) {
    uint32_t w[3];
    t.words(w);
    return static_cast<uint16_t>(rte_jhash_3words(w[0], w[1], w[2], 0x1234567) %
                                 n_);
  }
  uint32_t h = rss_hash(t, l4_hashed(t.protocol));
  return reta_size_ ? reta_owner_[h % reta_size_]
                    : static_cast<uint16_t>(h % n_);
}

uint16_t Steering::ret_owner(const FiveTuple &t) const {
  if (n_ == 1)
    return 0;
  if (!ret_by_rss(t.protocol))
    return static_cast<uint16_t>(rte_be_to_cpu_16(t.dst_port) % n_);
  uint32_t h = rss_hash(t, true);
  return reta_size_ ? reta_owner_[h % reta_size_]
                    : static_cast<uint16_t>(h % n_);
}

} // namespace l4lb
