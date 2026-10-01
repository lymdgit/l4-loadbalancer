/**
 * @file real_server.cpp
 * @brief Real Server 管理实现
 */

#include "lb/real_server.h"

#include "common/config.h"

namespace l4lb {

bool RealServerManager::load_from_config() {
  auto &cfg = Config::instance();
  auto servers = cfg.get_real_servers();

  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < servers.size(); ++i) {
    RealServer rs;
    rs.id = static_cast<uint32_t>(i + 1);
    rs.ip = ip_from_string(servers[i].ip);
    rs.port = servers[i].port;
    rs.mac = mac_from_string(servers[i].mac);
    rs.weight = servers[i].weight;
    rs.status = ServerStatus::UP;

    servers_[rs.id] = rs;
    if (rs.id < kMaxServers) {
      servers_array_[rs.id] = rs;
    }
    hash_ring_.add_node(rs.id, rs.weight);
  }
  return true;
}

void RealServerManager::add_server(const RealServer &rs) {
  std::lock_guard<std::mutex> lock(mutex_);
  servers_[rs.id] = rs;
  if (rs.id < kMaxServers) {
    servers_array_[rs.id] = rs;
  }
  hash_ring_.add_node(rs.id, rs.weight);
}

void RealServerManager::remove_server(uint32_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  servers_.erase(id);
  if (id < kMaxServers) {
    servers_array_[id] = RealServer{};
  }
  hash_ring_.remove_node(id);
}

void RealServerManager::set_status(uint32_t id, ServerStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = servers_.find(id);
  if (it != servers_.end()) {
    it->second.status = status;
    if (id < kMaxServers) {
      servers_array_[id].status = status;
    }
  }
}

RealServer *RealServerManager::select_server(const FiveTuple &tuple) {
  uint32_t server_id;
  if (!hash_ring_.get_server(tuple, server_id)) {
    return nullptr;
  }

  return get_server(server_id);
}

RealServer *RealServerManager::get_server(uint32_t id) {
  if (id < kMaxServers && id > 0 && servers_array_[id].id == id) {
    return servers_array_[id].is_available() ? &servers_array_[id] : nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = servers_.find(id);
  return it != servers_.end() && it->second.is_available() ? &it->second
                                                           : nullptr;
}

std::vector<RealServer> RealServerManager::get_all_servers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RealServer> result;
  result.reserve(servers_.size());
  for (const auto &[id, rs] : servers_) {
    result.push_back(rs);
  }
  return result;
}

size_t RealServerManager::count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return servers_.size();
}

} // namespace l4lb
