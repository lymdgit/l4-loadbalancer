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

#include "common/types.h"
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
  /// 获取单例实例
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
  bool load(const std::string &filename);

  /**
   * @brief 获取配置项（字符串）
   *
   * @param section section 名称
   * @param key 键名
   * @param default_val 默认值
   * @return 配置值
   */
  std::string get(const std::string &section, const std::string &key,
                  const std::string &default_val = "") const;

  /// 获取整数配置项
  int get_int(const std::string &section, const std::string &key,
              int default_val = 0) const;

  /// 获取布尔配置项（true/false, yes/no, 1/0, on）
  bool get_bool(const std::string &section, const std::string &key,
                bool default_val = false) const;

  /// 获取转发模式
  ForwardMode get_forward_mode() const;

  /// 获取 VIP 地址
  IPv4Addr get_vip() const;

  /// 获取 VIP MAC 地址
  MacAddr get_vip_mac() const;

  /// 获取监听端口列表
  std::vector<uint16_t> get_listen_ports() const;

  /// 获取 Real Server 配置列表
  const std::vector<RealServerConfig> &get_real_servers() const {
    return real_servers_;
  }

  /// 获取网关 IP
  IPv4Addr get_gateway() const;

  /// 获取会话超时时间（秒）
  uint32_t get_session_timeout() const;

  /// 获取虚拟节点数量
  uint32_t get_virtual_nodes() const;

  /// 打印配置信息
  void dump() const;

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
  void parse_real_servers();

  /// 去除字符串首尾空白
  static std::string trim(const std::string &str);

  /// 转换为小写
  static std::string to_lower(std::string str);

  std::unordered_map<std::string, std::string> config_map_; ///< 配置存储
  std::vector<RealServerConfig> real_servers_; ///< Real Server 列表
};

} // namespace l4lb

#endif // L4LB_COMMON_CONFIG_H
