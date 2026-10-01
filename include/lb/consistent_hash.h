/**
 * @file consistent_hash.h
 * @brief 一致性哈希实现
 *
 * 一致性哈希是负载均衡的核心算法，特点：
 * 1. 节点增减时只影响相邻节点的流量
 * 2. 使用虚拟节点提高负载均衡性
 * 3. 基于五元组哈希保持会话亲和性
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_CONSISTENT_HASH_H
#define L4LB_LB_CONSISTENT_HASH_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include "common/types.h"

namespace l4lb {

/**
 * @brief MurmurHash3 32位实现
 *
 * 高质量、快速的非加密哈希函数
 */
class MurmurHash3 {
public:
    static uint32_t hash(const void* key, size_t len, uint32_t seed = 0);

    static uint32_t hash_tuple(const FiveTuple& tuple) {
        return hash(&tuple, sizeof(tuple));
    }

private:
    static uint32_t rotl32(uint32_t x, int8_t r) {
        return (x << r) | (x >> (32 - r));
    }

    static uint32_t fmix32(uint32_t h) {
        h ^= h >> 16;
        h *= 0x85ebca6b;
        h ^= h >> 13;
        h *= 0xc2b2ae35;
        h ^= h >> 16;
        return h;
    }
};

/**
 * @brief 一致性哈希环（实现见 src/lb/consistent_hash.cpp）
 *
 * 实现负载均衡的核心数据结构
 */
class ConsistentHashRing {
public:
    explicit ConsistentHashRing(uint32_t virtual_nodes = 150)
        : virtual_nodes_(virtual_nodes) {}

    /// 添加节点，虚拟节点数 = virtual_nodes * weight / 100
    void add_node(uint32_t server_id, uint32_t weight = 100);

    /// 移除节点的全部虚拟节点
    void remove_node(uint32_t server_id);

    /// 根据五元组选择服务器，环为空时返回 false
    bool get_server(const FiveTuple& tuple, uint32_t& server_id) const;

    /// 获取虚拟节点数量
    size_t node_count() const;

    /// 清空哈希环
    void clear();

private:
    uint32_t virtual_nodes_;
    mutable std::mutex mutex_;
    std::map<uint32_t, uint32_t> ring_;  // hash -> server_id
};

} // namespace l4lb

#endif // L4LB_LB_CONSISTENT_HASH_H
