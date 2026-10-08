/**
 * @file config.h
 * @brief 配置管理模块
 *
 * INI 格式：
 *
 *   [global]       mode / dataplane / log_level / log_dir / max_sessions / 各状态超时 / toa ...
 *   [network]      vip_mac / netmask / gateway / local_ips / hc_src
 *   [healthcheck]  enabled / interval / timeout / failure_threshold / ...
 *   [control]      socket
 *   [service.N]    vip / port / proto / scheduler / server1 = ip:port:weight[:mac] ...
 *
 * 兼容旧格式：[vip] ip/ports/mac + [realserver] count/serverN，
 * 会被转换成每个端口一个 service（TCP 和 UDP 各一个）。
 *
 * 加载时做完整校验，出错时报告具体的 key 并返回 false。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_CONFIG_H
#define L4LB_COMMON_CONFIG_H

#include "common/types.h"
#include <map>
#include <string>
#include <vector>

namespace l4lb {

/// SNAT 端口段（FULLNAT 的 LIP 源端口）
constexpr uint16_t kNatPortMin = 10000;
constexpr uint16_t kNatPortMax = 60000;
/// 健康检查探测使用的源端口段
constexpr uint16_t kHcPortMin = 61000;
constexpr uint16_t kHcPortMax = 61999;

/// 容量上限（控制面校验用）
constexpr size_t kMaxServices = 64;
constexpr size_t kMaxRsPerService = 256;
constexpr size_t kMaxLocalIps = 64;

/// 调度算法
enum class SchedulerType { WRR, MAGLEV };

/// 数据面模式（docs/pipeline改造.md）
enum class DataplaneMode {
  RTC,      ///< 每个 lcore 收包 + 处理（网卡有 RSS 时用）
  PIPELINE, ///< main lcore 收包分发，其余 lcore 处理（网卡没有 RSS 时用）
};

/// Real Server 配置
struct RsConf {
  IPv4Addr ip = 0;     ///< 网络字节序
  uint16_t port = 0;   ///< 主机字节序
  uint32_t weight = 1; ///< 0 表示不接新连接
  MacAddr mac{};       ///< 全 0 表示通过 ARP 解析
};

/// 一个虚拟服务：VIP:port/proto -> 一组 RS
struct ServiceConf {
  std::string name;
  IPv4Addr vip = 0;       ///< 网络字节序
  uint16_t port = 0;      ///< 主机字节序
  uint8_t proto = 6;      ///< 6=TCP 17=UDP
  SchedulerType sched = SchedulerType::WRR;
  std::vector<RsConf> rs;
};

/// TCP/UDP 各状态超时（秒）
struct TimeoutConf {
  uint32_t tcp_syn = 10;
  uint32_t tcp_established = 900;
  uint32_t tcp_fin = 10;
  uint32_t tcp_timewait = 10;
  uint32_t tcp_close = 5;
  uint32_t udp = 60;
};

struct HealthConf {
  bool enabled = false;
  uint32_t interval_ms = 5000;
  uint32_t timeout_ms = 3000;
  uint32_t fall = 3; ///< 连续失败多少次判定 DOWN
  uint32_t rise = 2; ///< 连续成功多少次判定 UP
};

/// 完整的负载均衡配置
struct LbConfig {
  ForwardMode mode = ForwardMode::NAT;
  std::string log_level = "info";
  std::string log_dir;             ///< 日志目录，空表示只写 stderr
  uint32_t log_max_size_mb = 100;  ///< 单个日志文件上限，超过后轮转
  uint32_t log_max_files = 5;      ///< 保留的旧日志文件个数
  bool log_stderr = true;          ///< 写文件的同时输出到 stderr
  uint32_t stats_interval = 10;    ///< 周期统计（含速率、忙碌率）的日志间隔（秒）
  uint32_t max_sessions = 1 << 20; ///< 所有 worker 合计
  TimeoutConf timeouts;
  bool toa = false;                ///< FULLNAT 下插入 TOA 选项透传客户端地址
  bool strip_tcp_timestamp = true; ///< FULLNAT 下去掉 SYN 中的 TCP timestamp
  bool force_sw_steering = false;  ///< steering = sw：不用网卡 RSS（调试用）
  DataplaneMode dataplane = DataplaneMode::RTC;

  MacAddr vip_mac{};               ///< 全 0 表示使用网卡 MAC
  IPv4Addr netmask = 0;            ///< 0 表示所有地址都按直连处理
  IPv4Addr gateway = 0;
  std::vector<IPv4Addr> local_ips; ///< FULLNAT SNAT 源地址（LIP）
  IPv4Addr hc_src = 0;             ///< 健康检查源地址

  HealthConf health;
  std::string control_socket = "/run/l4lb.sock";

  std::vector<ServiceConf> services;
};

/**
 * @brief 配置管理类（只在启动和控制面使用，数据面不访问）
 */
class Config {
public:
  static Config &instance() {
    static Config config;
    return config;
  }

  /// 加载并校验配置文件；失败时已打印错误原因
  bool load(const std::string &filename);

  /// 解析得到的配置
  const LbConfig &lb() const { return lb_; }

  /// 原始 key（section.key）查询
  std::string get(const std::string &section, const std::string &key,
                  const std::string &default_val = "") const;

  /// 打印配置信息
  void dump() const;

private:
  Config() = default;
  Config(const Config &) = delete;
  Config &operator=(const Config &) = delete;

  bool parse_file(const std::string &filename);
  bool build();
  bool parse_services();
  bool parse_legacy_services();
  bool parse_rs(const std::string &key, const std::string &value, RsConf &rs);
  bool validate();

  bool get_u32(const std::string &section, const std::string &key,
               uint32_t &out, uint32_t min_val, uint32_t max_val);
  bool get_bool(const std::string &section, const std::string &key, bool &out);
  bool get_ip(const std::string &section, const std::string &key,
              IPv4Addr &out);

  static std::string trim(const std::string &str);
  static std::string to_lower(std::string str);
  static std::vector<std::string> split(const std::string &s, char sep);

  /// section -> (key -> value)；section 名可以包含 '.'（如 service.web）
  std::map<std::string, std::map<std::string, std::string>> sections_;
  LbConfig lb_;
};

} // namespace l4lb

#endif // L4LB_COMMON_CONFIG_H
