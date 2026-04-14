/**
 * @file config.h
 * @brief 配置管理模块
 *
 * 负责解析和管理负载均衡器的配置信息，包括：
 * - INI 格式配置文件解析
 * - VIP 和 Real Server 配置
 * - 运行时参数配置
 *
 * INI 文件格式示例：
 * [section]
 * key = value
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_CONFIG_H
#define L4LB_COMMON_CONFIG_H

#include "logger.h"
#include "types.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>


namespace l4lb {

/**
 * @brief Real Server 配置信息
 */
struct RealServerConfig {
  std::string ip;  ///< IP 地址字符串
  uint16_t port;   ///< 端口
  uint32_t weight; ///< 权重
  std::string mac; ///< MAC 地址字符串
};

/**
 * @brief 配置管理类
 *
 * 单例模式实现，提供全局配置访问
 *
 * 使用方式：
 * @code
 * auto& config = Config::instance();
 * config.load("lb.conf");
 * std::string vip = config.get("vip", "ip");
 * @endcode
 */
class Config {
public:
  /**
   * @brief 获取单例实例
   */
  static Config &instance() {
    static Config config;
    return config;
  }

  /**
   * @brief 从文件加载配置
   *
   * @param filename 配置文件路径
   * @return true 加载成功
   */
  bool load(const std::string &filename) {
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

  /**
   * @brief 获取配置项（字符串）
   *
   * @param section section 名称
   * @param key 键名
   * @param default_val 默认值
   * @return 配置值
   */
  std::string get(const std::string &section, const std::string &key,
                  const std::string &default_val = "") const {
    std::string full_key = section + "." + key;
    auto it = config_map_.find(full_key);
    return it != config_map_.end() ? it->second : default_val;
  }

  /**
   * @brief 获取整数配置项
   */
  int get_int(const std::string &section, const std::string &key,
              int default_val = 0) const {
    std::string val = get(section, key);
    if (val.empty())
      return default_val;

    try {
      return std::stoi(val);
    } catch (...) {
      return default_val;
    }
  }

  /**
   * @brief 获取布尔配置项
   */
  bool get_bool(const std::string &section, const std::string &key,
                bool default_val = false) const {
    std::string val = get(section, key);
    if (val.empty())
      return default_val;

    // true/false, yes/no, 1/0
    val = to_lower(val);
    return val == "true" || val == "yes" || val == "1" || val == "on";
  }

  /**
   * @brief 获取转发模式
   */
  ForwardMode get_forward_mode() const {
    std::string mode = get("global", "mode", "nat");
    return (to_lower(mode) == "dr") ? ForwardMode::DR : ForwardMode::NAT;
  }

  /**
   * @brief 获取 VIP 地址
   */
  IPv4Addr get_vip() const {
    return ip_from_string(get("vip", "ip", "0.0.0.0"));
  }

  /**
   * @brief 获取 VIP MAC 地址
   */
  MacAddr get_vip_mac() const {
    return mac_from_string(get("vip", "mac", "00:00:00:00:00:00"));
  }

  /**
   * @brief 获取监听端口列表
   */
  std::vector<uint16_t> get_listen_ports() const {
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

  /**
   * @brief 获取 Real Server 配置列表
   */
  const std::vector<RealServerConfig> &get_real_servers() const {
    return real_servers_;
  }

  /**
   * @brief 获取网关 IP
   */
  IPv4Addr get_gateway() const {
    return ip_from_string(get("network", "gateway", "0.0.0.0"));
  }

  /**
   * @brief 获取会话超时时间
   */
  uint32_t get_session_timeout() const {
    return static_cast<uint32_t>(get_int("global", "session_timeout", 300));
  }

  /**
   * @brief 获取虚拟节点数量
   */
  uint32_t get_virtual_nodes() const {
    return static_cast<uint32_t>(get_int("global", "virtual_nodes", 150));
  }

  /**
   * @brief 打印配置信息
   */
  void dump() const {
    LOG_INFO("========== Configuration ==========");
    LOG_INFO("Forward Mode: %s",
             get_forward_mode() == ForwardMode::NAT ? "NAT" : "DR");
    LOG_INFO("VIP: %s", get("vip", "ip").c_str());
    LOG_INFO("VIP MAC: %s", get("vip", "mac").c_str());
    LOG_INFO("Gateway: %s", get("network", "gateway").c_str());
    LOG_INFO("Session Timeout: %d seconds", get_session_timeout());
    LOG_INFO("Virtual Nodes: %d", get_virtual_nodes());
    LOG_INFO("Real Servers: %zu", real_servers_.size());

    for (size_t i = 0; i < real_servers_.size(); ++i) {
      const auto &rs = real_servers_[i];
      LOG_INFO("  [%zu] %s:%d weight=%d mac=%s", i, rs.ip.c_str(), rs.port,
               rs.weight, rs.mac.c_str());
    }
    LOG_INFO("====================================");
  }

private:
  Config() = default;

  // 禁止拷贝
  Config(const Config &) = delete;
  Config &operator=(const Config &) = delete;

  /**
   * @brief 解析 Real Server 配置
   *
   * 配置格式: server1 = ip:port:weight:mac
   */
  void parse_real_servers() {
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
      LOG_INFO("Parsed Real Server: %s:%d weight=%d mac=%s", rs.ip.c_str(),
               rs.port, rs.weight, rs.mac.c_str());
    }
  }

  /**
   * @brief 去除字符串首尾空白
   */
  static std::string trim(const std::string &str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) {
      return std::isspace(c);
    });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) {
                 return std::isspace(c);
               }).base();
    return (start < end) ? std::string(start, end) : std::string();
  }

  /**
   * @brief 转换为小写
   */
  static std::string to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return str;
  }

  std::unordered_map<std::string, std::string> config_map_; ///< 配置存储
  std::vector<RealServerConfig> real_servers_; ///< Real Server 列表
};

} // namespace l4lb

#endif // L4LB_COMMON_CONFIG_H
