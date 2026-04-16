/**
 * @file consistent_hash.h
 * @brief 一致性哈希实现
 *
 * 无锁化优化：
 * 1. std::map(红黑树) → sorted std::vector：二分查找 + 连续内存，cache 友好
 * 2. std::mutex → std::shared_mutex：多个 lcore 并发读 get_server() 不再互相阻塞
 *    - 读路径（get_server）：shared_lock，N 个 lcore 同时进入
 *    - 写路径（add/remove，控制面）：unique_lock，独占
 */

#ifndef L4LB_LB_CONSISTENT_HASH_H
#define L4LB_LB_CONSISTENT_HASH_H

#include <algorithm>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"

namespace l4lb {

/**
 * @brief MurmurHash3 32位实现
 */
class MurmurHash3 {
public:
    static uint32_t hash(const void* key, size_t len, uint32_t seed = 0) {
        const uint8_t* data = static_cast<const uint8_t*>(key);
        const int nblocks = len / 4;

        uint32_t h1 = seed;
        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;

        const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data);
        for (int i = 0; i < nblocks; ++i) {
            uint32_t k1 = blocks[i];
            k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
            h1 ^= k1; h1 = rotl32(h1, 13);
            h1 = h1 * 5 + 0xe6546b64;
        }

        const uint8_t* tail = data + nblocks * 4;
        uint32_t k1 = 0;
        switch (len & 3) {
            case 3: k1 ^= tail[2] << 16; [[fallthrough]];
            case 2: k1 ^= tail[1] << 8;  [[fallthrough]];
            case 1: k1 ^= tail[0];
                    k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
        }

        h1 ^= len;
        h1 = fmix32(h1);
        return h1;
    }

    static uint32_t hash_tuple(const FiveTuple& tuple) {
        return hash(&tuple, sizeof(tuple));
    }

private:
    static uint32_t rotl32(uint32_t x, int8_t r) { return (x << r) | (x >> (32 - r)); }
    static uint32_t fmix32(uint32_t h) {
        h ^= h >> 16; h *= 0x85ebca6b;
        h ^= h >> 13; h *= 0xc2b2ae35;
        h ^= h >> 16; return h;
    }
};

/**
 * @brief 一致性哈希环（无锁读路径版本）
 *
 * 内部用 sorted vector 代替 std::map：
 *   - std::map  : 红黑树，O(log n) 指针追逐，cache 不友好
 *   - sorted vector: O(log n) 二分查找，连续内存，cache line 友好
 *
 * 并发模型：
 *   - get_server (每个数据包新连接调用): shared_lock，多核并发，互不阻塞
 *   - add_node / remove_node (控制面，启动时调用一次): unique_lock，独占
 */
class ConsistentHashRing {
public:
    explicit ConsistentHashRing(uint32_t virtual_nodes = 150)
        : virtual_nodes_(virtual_nodes) {}

    /**
     * @brief 添加节点（控制面，启动时调用，unique_lock）
     */
    void add_node(uint32_t server_id, uint32_t weight = 100) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        uint32_t replicas = (virtual_nodes_ * weight) / 100;
        if (replicas < 1) replicas = 1;

        ring_.reserve(ring_.size() + replicas);
        for (uint32_t i = 0; i < replicas; ++i) {
            std::string key = std::to_string(server_id) + "#" + std::to_string(i);
            uint32_t h = MurmurHash3::hash(key.data(), key.size());
            // 有序插入：找到插入位置，保持 ring_ 按 hash 升序
            auto pos = std::lower_bound(ring_.begin(), ring_.end(),
                                        std::make_pair(h, 0u));
            ring_.insert(pos, {h, server_id});
        }
    }

    /**
     * @brief 移除节点（控制面，unique_lock）
     */
    void remove_node(uint32_t server_id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        ring_.erase(
            std::remove_if(ring_.begin(), ring_.end(),
                           [server_id](const std::pair<uint32_t,uint32_t>& e){
                               return e.second == server_id;
                           }),
            ring_.end());
    }

    /**
     * @brief 根据五元组选择服务器（数据面热路径，shared_lock）
     *
     * shared_lock 允许所有 lcore 同时进入此函数，互不阻塞。
     * 只要没有并发的 add_node/remove_node，shared_lock 几乎是零开销。
     */
    bool get_server(const FiveTuple& tuple, uint32_t& server_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);

        if (ring_.empty()) return false;

        uint32_t h = MurmurHash3::hash_tuple(tuple);

        // lower_bound 在有序 vector 上做二分查找，O(log n) 且 cache 友好
        auto it = std::lower_bound(ring_.begin(), ring_.end(),
                                   std::make_pair(h, 0u));
        if (it == ring_.end()) it = ring_.begin(); // 环形回绕

        server_id = it->second;
        return true;
    }

    size_t node_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return ring_.size();
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        ring_.clear();
    }

private:
    uint32_t virtual_nodes_;
    mutable std::shared_mutex mutex_;  // 读写分离锁（替代 std::mutex）
    // sorted vector: {hash, server_id}，按 hash 升序
    // 相比 std::map 节省一半以上内存，二分查找 cache 命中率更高
    std::vector<std::pair<uint32_t, uint32_t>> ring_;
};

} // namespace l4lb

#endif // L4LB_LB_CONSISTENT_HASH_H
