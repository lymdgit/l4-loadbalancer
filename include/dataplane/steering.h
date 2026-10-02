/**
 * @file steering.h
 * @brief 多核分发：决定一个包归哪个 worker 处理（"会话 owner"）
 *
 * FULLNAT 下同一条连接两个方向的五元组完全不同：
 *   正向 Client:cport -> VIP:vport      回程 RS:rport -> LIP:nat_port
 * 会话表是 per-worker 的，必须保证两个方向都由创建会话的 worker 处理。
 *
 * 两种模式（启动时根据网卡能力自动选择）：
 *
 * HW（网卡支持 RSS）：
 *   owner = RETA[toeplitz(key, 五元组) % reta_size]，与网卡分队列的算法一致。
 *   分配 SNAT 端口时只选 "回程五元组的 toeplitz 结果落在本 worker 队列" 的端口，
 *   于是网卡天然把回程包送回本核（方案 A，无需 rte_flow）。
 *   若网卡对某协议只按 IP 做 RSS（如 vmxnet3 的 UDP），端口不影响 hash，
 *   回程 owner 改为 nat_port % N。
 *
 * SW（网卡不支持 RSS，如 af_packet）：
 *   正向 owner = hash(五元组) % N，回程 owner = nat_port % N。
 *
 * 两种模式下，收到包的 worker 都会重新计算 owner，不是自己就通过 ring 转交
 * （方案 B）。所以即使软件 Toeplitz 与网卡不一致，结果也只是多一次转交，
 * 不会丢会话；ST_REDIRECT_OUT / ST_RSS_MISMATCH 计数可以用来确认方案 A 是否生效。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_STEERING_H
#define L4LB_DATAPLANE_STEERING_H

#include "common/types.h"
#include <array>
#include <cstdint>

namespace l4lb {

class Steering {
public:
  static constexpr uint16_t kMaxReta = 512;
  static constexpr size_t kRssKeyLen = 40;

  /// 默认 RSS key（Intel/Microsoft 标准 key，vmxnet3 等驱动的默认值）
  static const uint8_t kDefaultRssKey[kRssKeyLen];

  /// SW 模式初始化
  void init_sw(uint16_t num_workers);

  /**
   * @brief HW 模式初始化
   * @param reta RETA 表（队列号），长度 reta_size
   * @param tcp_l4 / udp_l4 网卡是否对 TCP / UDP 按端口做 RSS
   */
  void init_hw(uint16_t num_workers, const uint8_t *rss_key,
               const uint16_t *reta, uint16_t reta_size, bool tcp_l4,
               bool udp_l4);

  bool hw() const { return hw_; }
  uint16_t workers() const { return n_; }

  /// 正向（Client -> VIP）五元组的 owner
  uint16_t fwd_owner(const FiveTuple &t) const;

  /// 回程（RS -> LIP）五元组的 owner
  uint16_t ret_owner(const FiveTuple &t) const;

  /**
   * @brief 软件计算的 RSS hash（与网卡算法一致）
   * @param l4 是否把端口算进去
   */
  uint32_t rss_hash(const FiveTuple &t, bool l4) const;

  /// 某协议的回程 owner 是否由端口经 RSS 决定（否则是 nat_port % N）
  bool ret_by_rss(uint8_t proto) const {
    return hw_ && (proto == 6 ? tcp_l4_ : udp_l4_);
  }
  bool l4_hashed(uint8_t proto) const { return proto == 6 ? tcp_l4_ : udp_l4_; }

  /**
   * @brief SNAT 端口候选序列：worker w 依次尝试的端口
   *
   * 回程 owner 由 RSS 决定时，所有端口都可能落到本 worker，步长为 1；
   * 否则只有 port % N == w 的端口属于本 worker，步长为 N。
   */
  uint16_t port_step(uint8_t proto) const {
    return ret_by_rss(proto) ? 1 : n_;
  }

private:
  bool hw_ = false;
  bool tcp_l4_ = false, udp_l4_ = false;
  uint16_t n_ = 1;
  uint16_t reta_size_ = 0;
  std::array<uint16_t, kMaxReta> reta_owner_{}; ///< RETA 下标 -> worker
  alignas(8) uint8_t key_be_[kRssKeyLen] = {};  ///< rte_softrss_be 使用的 key
};

} // namespace l4lb

#endif // L4LB_DATAPLANE_STEERING_H
