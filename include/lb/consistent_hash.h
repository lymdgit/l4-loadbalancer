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

#include <cstdint>
#include <map>
#include <vector>
#include <string>
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
    static uint32_t hash(const void* key, size_t len, uint32_t seed = 0) {
        const uint8_t* data = static_cast<const uint8_t*>(key);
        const int nblocks = len / 4;
        
        uint32_t h1 = seed;
        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;
        
        // Body
        const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data);
        for (int i = 0; i < nblocks; ++i) {
            uint32_t k1 = blocks[i];
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            
            h1 ^= k1;
            h1 = rotl32(h1, 13);
            h1 = h1 * 5 + 0xe6546b64;
        }
        
        // Tail
        const uint8_t* tail = data + nblocks * 4;
        uint32_t k1 = 0;
        switch (len & 3) {
            case 3: k1 ^= tail[2] << 16; [[fallthrough]];
            case 2: k1 ^= tail[1] << 8;  [[fallthrough]];
            case 1: k1 ^= tail[0];
                    k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
        }
        
        // Finalization
        h1 ^= len;
        h1 = fmix32(h1);
        return h1;
    }
    
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
 * @brief 一致性哈希环
 * 
 * 实现负载均衡的核心数据结构
 */
class ConsistentHashRing {
public:
    ConsistentHashRing(uint32_t virtual_nodes = 150) 
        : virtual_nodes_(virtual_nodes) {}
    
    /**
     * @brief 添加节点
     */
    void add_node(uint32_t server_id, uint32_t weight = 100) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        uint32_t replicas = (virtual_nodes_ * weight) / 100;
        if (replicas < 1) replicas = 1;
        
        for (uint32_t i = 0; i < replicas; ++i) {
            std::string key = std::to_string(server_id) + "#" + std::to_string(i);
            uint32_t hash = MurmurHash3::hash(key.data(), key.size());
            ring_[hash] = server_id;
        }
    }
    
    /**
     * @brief 移除节点
     */
    void remove_node(uint32_t server_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto it = ring_.begin(); it != ring_.end(); ) {
            if (it->second == server_id) {
                it = ring_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    /**
     * @brief 根据五元组选择服务器
     */
    bool get_server(const FiveTuple& tuple, uint32_t& server_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (ring_.empty()) return false;
        
        uint32_t hash = MurmurHash3::hash_tuple(tuple);
        auto it = ring_.lower_bound(hash);
        
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        
        server_id = it->second;
        return true;
    }
    
    /**
     * @brief 获取节点数量
     */
    size_t node_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ring_.size();
    }
    
    /**
     * @brief 清空哈希环
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.clear();
    }
    
private:
    uint32_t virtual_nodes_;
    mutable std::mutex mutex_;
    std::map<uint32_t, uint32_t> ring_;  // hash -> server_id
};

} // namespace l4lb

#endif // L4LB_LB_CONSISTENT_HASH_H
