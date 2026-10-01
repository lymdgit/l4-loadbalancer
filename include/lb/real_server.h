/**
 * @file real_server.h
 * @brief Real Server 管理
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_REAL_SERVER_H
#define L4LB_LB_REAL_SERVER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "lb/consistent_hash.h"

namespace l4lb {

/**
 * @brief Real Server 管理器（实现见 src/lb/real_server.cpp）
 *
 * 优化：servers_array_ 提供 O(1) 无锁读路径，get_server 高频调用不再加锁
 */
class RealServerManager {
public:
    static constexpr size_t kMaxServers = 64;

    static RealServerManager& instance() {
        static RealServerManager mgr;
        return mgr;
    }

    /// 从 Config 加载服务器并构建哈希环
    bool load_from_config();

    /// 添加服务器
    void add_server(const RealServer& rs);

    /// 移除服务器
    void remove_server(uint32_t id);

    /// 设置服务器状态
    void set_status(uint32_t id, ServerStatus status);

    /// 按五元组选择服务器，无可用服务器返回 nullptr
    RealServer* select_server(const FiveTuple& tuple);

    /// 获取服务器 - 无锁快速路径（id < kMaxServers 时）；不可用返回 nullptr
    RealServer* get_server(uint32_t id);

    /// 获取所有服务器（拷贝）
    std::vector<RealServer> get_all_servers() const;

    /// 获取服务器数量
    size_t count() const;

private:
    RealServerManager() : hash_ring_(150) {}

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, RealServer> servers_;
    std::array<RealServer, kMaxServers> servers_array_{};  // 无锁快速路径
    ConsistentHashRing hash_ring_;
};

} // namespace l4lb

#endif // L4LB_LB_REAL_SERVER_H
