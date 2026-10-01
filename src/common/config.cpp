/**
 * @file config.cpp
 * @brief 配置管理模块实现
 */

#include "common/config.h"

#include "common/logger.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace l4lb {

bool Config::load(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    LOG_ERROR("Failed to open config file: %s", filename.c_str());
    return false;
  }

  std::string line;
  std::string current_section;
  int line_num = 0;

  while (std::getline(file, line)) {
    ++line_num;

    // 去除首尾空白
    line = trim(line);

    // 跳过空行和注释
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }

    // 解析 section [section_name]
    if (line[0] == '[') {
      auto end = line.find(']');
      if (end == std::string::npos) {
        LOG_WARN("Invalid section at line %d: %s", line_num, line.c_str());
        continue;
      }
      current_section = line.substr(1, end - 1);
      current_section = trim(current_section);
      continue;
    }

    // 解析 key = value
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos) {
      LOG_WARN("Invalid config at line %d: %s", line_num, line.c_str());
      continue;
    }

    std::string key = trim(line.substr(0, eq_pos));
    std::string value = trim(line.substr(eq_pos + 1));

    // 存储配置项
    std::string full_key =
        current_section.empty() ? key : current_section + "." + key;
    config_map_[full_key] = value;
  }

  LOG_INFO("Loaded %zu configuration items from %s", config_map_.size(),
           filename.c_str());

  // 解析特殊配置
  parse_real_servers();

  return true;
}

std::string Config::get(const std::string &section, const std::string &key,
                        const std::string &default_val) const {
  std::string full_key = section + "." + key;
  auto it = config_map_.find(full_key);
  return it != config_map_.end() ? it->second : default_val;
}

int Config::get_int(const std::string &section, const std::string &key,
                    int default_val) const {
  std::string val = get(section, key);
  if (val.empty())
    return default_val;

  try {
    return std::stoi(val);
  } catch (...) {
    return default_val;
  }
}

bool Config::get_bool(const std::string &section, const std::string &key,
                      bool default_val) const {
  std::string val = get(section, key);
  if (val.empty())
    return default_val;

  val = to_lower(val);
  return val == "true" || val == "yes" || val == "1" || val == "on";
}

ForwardMode Config::get_forward_mode() const {
  std::string mode = get("global", "mode", "nat");
  return (to_lower(mode) == "dr") ? ForwardMode::DR : ForwardMode::NAT;
}

IPv4Addr Config::get_vip() const {
  return ip_from_string(get("vip", "ip", "0.0.0.0"));
}

MacAddr Config::get_vip_mac() const {
  return mac_from_string(get("vip", "mac", "00:00:00:00:00:00"));
}

std::vector<uint16_t> Config::get_listen_ports() const {
  std::vector<uint16_t> ports;
  std::string ports_str = get("vip", "ports", "80");

  std::stringstream ss(ports_str);
  std::string port_str;
  while (std::getline(ss, port_str, ',')) {
    port_str = trim(port_str);
    if (!port_str.empty()) {
      try {
        ports.push_back(static_cast<uint16_t>(std::stoi(port_str)));
      } catch (...) {
        LOG_WARN("Invalid port: %s", port_str.c_str());
      }
    }
  }

  return ports;
}

IPv4Addr Config::get_gateway() const {
  return ip_from_string(get("network", "gateway", "0.0.0.0"));
}

uint32_t Config::get_session_timeout() const {
  return static_cast<uint32_t>(get_int("global", "session_timeout", 300));
}

uint32_t Config::get_virtual_nodes() const {
  return static_cast<uint32_t>(get_int("global", "virtual_nodes", 150));
}

void Config::dump() const {
  LOG_INFO("========== Configuration ==========");
  LOG_INFO("Forward Mode: %s",
           get_forward_mode() == ForwardMode::NAT ? "NAT" : "DR");
  LOG_INFO("VIP: %s", get("vip", "ip").c_str());
  LOG_INFO("VIP MAC: %s", get("vip", "mac").c_str());
  LOG_INFO("Gateway: %s", get("network", "gateway").c_str());
  LOG_INFO("Session Timeout: %u seconds", get_session_timeout());
  LOG_INFO("Virtual Nodes: %u", get_virtual_nodes());
  LOG_INFO("Real Servers: %zu", real_servers_.size());

  for (size_t i = 0; i < real_servers_.size(); ++i) {
    const auto &rs = real_servers_[i];
    LOG_INFO("  [%zu] %s:%u weight=%u mac=%s", i, rs.ip.c_str(), rs.port,
             rs.weight, rs.mac.c_str());
  }
  LOG_INFO("====================================");
}

void Config::parse_real_servers() {
  real_servers_.clear();

  int count = get_int("realserver", "count", 0);
  for (int i = 1; i <= count; ++i) {
    std::string key = "server" + std::to_string(i);
    std::string value = get("realserver", key);

    if (value.empty())
      continue;

    // 解析 ip:port:weight:mac (MAC 是 6 段用冒号分隔)
    // 例如: 192.168.72.149:80:100:00:0c:29:bd:b3:a4
    // parts[0]=ip, [1]=port, [2]=weight, [3-8]=MAC 6段
    RealServerConfig rs;

    std::vector<std::string> parts;
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, ':')) {
      parts.push_back(trim(part));
    }

    if (parts.size() >= 1)
      rs.ip = parts[0];
    if (parts.size() >= 2)
      rs.port = static_cast<uint16_t>(std::stoi(parts[1]));
    if (parts.size() >= 3)
      rs.weight = static_cast<uint32_t>(std::stoi(parts[2]));

    // MAC 需要 9 个 parts: ip + port + weight + 6 MAC octets
    if (parts.size() >= 9) {
      rs.mac = parts[3] + ":" + parts[4] + ":" + parts[5] + ":" + parts[6] +
               ":" + parts[7] + ":" + parts[8];
    }

    real_servers_.push_back(rs);
    LOG_INFO("Parsed Real Server: %s:%u weight=%u mac=%s", rs.ip.c_str(),
             rs.port, rs.weight, rs.mac.c_str());
  }
}

std::string Config::trim(const std::string &str) {
  auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) {
    return std::isspace(c);
  });
  auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) {
               return std::isspace(c);
             }).base();
  return (start < end) ? std::string(start, end) : std::string();
}

std::string Config::to_lower(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return str;
}

} // namespace l4lb
