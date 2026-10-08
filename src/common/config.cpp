/**
 * @file config.cpp
 * @brief 配置管理模块实现
 */

#include "common/config.h"

#include "common/logger.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <set>
#include <tuple>
#include <sstream>

namespace l4lb {

namespace {

const char *proto_name(uint8_t proto) { return proto == 17 ? "udp" : "tcp"; }

const char *sched_name(SchedulerType s) {
  return s == SchedulerType::MAGLEV ? "maglev" : "wrr";
}

bool in_port_range(uint32_t p, uint32_t lo, uint32_t hi) {
  return p >= lo && p <= hi;
}

} // namespace

// ============================================================================
// 读取文件
// ============================================================================

bool Config::load(const std::string &filename) {
  sections_.clear();
  lb_ = LbConfig{};
  if (!parse_file(filename))
    return false;
  if (!build())
    return false;
  return validate();
}

bool Config::parse_file(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    LOG_ERROR("Failed to open config file: %s", filename.c_str());
    return false;
  }

  std::string line;
  std::string current_section;
  int line_num = 0;
  size_t items = 0;

  while (std::getline(file, line)) {
    ++line_num;
    line = trim(line);

    // 跳过空行和注释
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;

    // 解析 section [section_name]
    if (line[0] == '[') {
      auto end = line.find(']');
      if (end == std::string::npos) {
        LOG_ERROR("%s:%d: invalid section: %s", filename.c_str(), line_num,
                  line.c_str());
        return false;
      }
      current_section = to_lower(trim(line.substr(1, end - 1)));
      sections_[current_section];
      continue;
    }

    // 解析 key = value（行尾 # 之后视为注释）
    auto eq_pos = line.find('=');
    if (eq_pos == std::string::npos || current_section.empty()) {
      LOG_ERROR("%s:%d: expected 'key = value' inside a section: %s",
                filename.c_str(), line_num, line.c_str());
      return false;
    }
    std::string key = to_lower(trim(line.substr(0, eq_pos)));
    std::string value = line.substr(eq_pos + 1);
    auto hash = value.find('#');
    if (hash != std::string::npos)
      value = value.substr(0, hash);
    sections_[current_section][key] = trim(value);
    ++items;
  }

  LOG_INFO("Loaded %zu configuration items from %s", items, filename.c_str());
  return true;
}

std::string Config::get(const std::string &section, const std::string &key,
                        const std::string &default_val) const {
  auto s = sections_.find(section);
  if (s == sections_.end())
    return default_val;
  auto it = s->second.find(key);
  return it != s->second.end() ? it->second : default_val;
}

// ============================================================================
// 类型化读取：出错时打印 section.key 并返回 false；key 不存在时保持默认值
// ============================================================================

bool Config::get_u32(const std::string &section, const std::string &key,
                     uint32_t &out, uint32_t min_val, uint32_t max_val) {
  std::string v = get(section, key);
  if (v.empty())
    return true;
  char *end = nullptr;
  errno = 0;
  unsigned long n = strtoul(v.c_str(), &end, 10);
  if (errno || end == v.c_str() || *end != '\0' || n < min_val || n > max_val) {
    LOG_ERROR("config [%s] %s = '%s': expected integer in [%u, %u]",
              section.c_str(), key.c_str(), v.c_str(), min_val, max_val);
    return false;
  }
  out = static_cast<uint32_t>(n);
  return true;
}

bool Config::get_bool(const std::string &section, const std::string &key,
                      bool &out) {
  std::string v = to_lower(get(section, key));
  if (v.empty())
    return true;
  if (v == "true" || v == "yes" || v == "1" || v == "on") {
    out = true;
  } else if (v == "false" || v == "no" || v == "0" || v == "off") {
    out = false;
  } else {
    LOG_ERROR("config [%s] %s = '%s': expected true/false", section.c_str(),
              key.c_str(), v.c_str());
    return false;
  }
  return true;
}

bool Config::get_ip(const std::string &section, const std::string &key,
                    IPv4Addr &out) {
  std::string v = get(section, key);
  if (v.empty())
    return true;
  if (!parse_ipv4(v, out)) {
    LOG_ERROR("config [%s] %s = '%s': invalid IPv4 address", section.c_str(),
              key.c_str(), v.c_str());
    return false;
  }
  return true;
}

// ============================================================================
// 构建 LbConfig
// ============================================================================

bool Config::build() {
  auto &c = lb_;

  // ---- [global] ----
  std::string mode = to_lower(get("global", "mode", "nat"));
  if (mode == "nat" || mode == "fullnat") {
    c.mode = ForwardMode::NAT;
  } else if (mode == "dr") {
    c.mode = ForwardMode::DR;
  } else {
    LOG_ERROR("config [global] mode = '%s': expected nat or dr", mode.c_str());
    return false;
  }
  c.log_level = to_lower(get("global", "log_level", c.log_level));
  c.log_dir = get("global", "log_dir", c.log_dir);
  std::string steering = to_lower(get("global", "steering", "auto"));
  if (steering != "auto" && steering != "sw") {
    LOG_ERROR("config [global] steering = '%s': expected auto or sw",
              steering.c_str());
    return false;
  }
  c.force_sw_steering = steering == "sw";
  std::string dp = to_lower(get("global", "dataplane", "rtc"));
  if (dp == "rtc") {
    c.dataplane = DataplaneMode::RTC;
  } else if (dp == "pipeline") {
    c.dataplane = DataplaneMode::PIPELINE;
  } else {
    LOG_ERROR("config [global] dataplane = '%s': expected rtc or pipeline",
              dp.c_str());
    return false;
  }

  auto &t = c.timeouts;
  bool ok = get_u32("global", "max_sessions", c.max_sessions, 1024, 1u << 26) &&
            get_bool("global", "toa", c.toa) &&
            get_bool("global", "strip_tcp_timestamp", c.strip_tcp_timestamp) &&
            get_u32("global", "log_max_size_mb", c.log_max_size_mb, 1, 10240) &&
            get_u32("global", "log_max_files", c.log_max_files, 1, 100) &&
            get_bool("global", "log_stderr", c.log_stderr) &&
            get_u32("global", "stats_interval", c.stats_interval, 1, 3600);
  // session_timeout：旧配置项，作为 ESTABLISHED 和 UDP 的超时
  uint32_t legacy_timeout = 0;
  ok = ok && get_u32("global", "session_timeout", legacy_timeout, 1, 86400);
  if (legacy_timeout) {
    t.tcp_established = legacy_timeout;
    t.udp = legacy_timeout;
  }
  ok = ok && get_u32("global", "tcp_syn_timeout", t.tcp_syn, 1, 86400) &&
       get_u32("global", "tcp_established_timeout", t.tcp_established, 1,
               86400) &&
       get_u32("global", "tcp_fin_timeout", t.tcp_fin, 1, 86400) &&
       get_u32("global", "tcp_timewait_timeout", t.tcp_timewait, 1, 86400) &&
       get_u32("global", "tcp_close_timeout", t.tcp_close, 1, 86400) &&
       get_u32("global", "udp_timeout", t.udp, 1, 86400);
  if (!ok)
    return false;
  // 握手/挥手阶段的超时不超过 ESTABLISHED 超时（session_timeout 调小时一起缩短）
  t.tcp_syn = std::min(t.tcp_syn, t.tcp_established);
  t.tcp_fin = std::min(t.tcp_fin, t.tcp_established);
  t.tcp_timewait = std::min(t.tcp_timewait, t.tcp_established);
  t.tcp_close = std::min(t.tcp_close, t.tcp_established);

  // ---- [network]（vip_mac 兼容旧的 [vip] mac）----
  std::string mac = get("network", "vip_mac", get("vip", "mac"));
  if (!mac.empty() && !parse_mac(mac, c.vip_mac)) {
    LOG_ERROR("config vip_mac = '%s': invalid MAC address", mac.c_str());
    return false;
  }
  if (!get_ip("network", "netmask", c.netmask) ||
      !get_ip("network", "gateway", c.gateway) ||
      !get_ip("network", "hc_src", c.hc_src))
    return false;
  for (const auto &s : split(get("network", "local_ips"), ',')) {
    IPv4Addr ip;
    if (!parse_ipv4(s, ip)) {
      LOG_ERROR("config [network] local_ips: invalid address '%s'", s.c_str());
      return false;
    }
    c.local_ips.push_back(ip);
  }

  // ---- [healthcheck] ----
  auto &h = c.health;
  if (!get_bool("healthcheck", "enabled", h.enabled) ||
      !get_u32("healthcheck", "interval", h.interval_ms, 1, 3600) ||
      !get_u32("healthcheck", "timeout", h.timeout_ms, 10, 60000) ||
      !get_u32("healthcheck", "failure_threshold", h.fall, 1, 100) ||
      !get_u32("healthcheck", "success_threshold", h.rise, 1, 100))
    return false;
  // interval 以秒为单位配置（与旧配置一致），timeout 以毫秒为单位
  if (!get("healthcheck", "interval").empty())
    h.interval_ms *= 1000;

  // ---- [control] ----
  c.control_socket = get("control", "socket", c.control_socket);

  // ---- services ----
  bool has_new = false;
  for (const auto &sec : sections_)
    if (sec.first.compare(0, 8, "service.") == 0)
      has_new = true;
  return has_new ? parse_services() : parse_legacy_services();
}

bool Config::parse_rs(const std::string &key, const std::string &value,
                      RsConf &rs) {
  // ip:port[:weight[:mac]]，mac 本身也是 6 段冒号分隔
  auto parts = split(value, ':');
  if (parts.size() != 2 && parts.size() != 3 && parts.size() != 9) {
    LOG_ERROR("config %s = '%s': expected ip:port[:weight[:mac]]", key.c_str(),
              value.c_str());
    return false;
  }
  if (!parse_ipv4(parts[0], rs.ip)) {
    LOG_ERROR("config %s: invalid RS address '%s'", key.c_str(),
              parts[0].c_str());
    return false;
  }
  char *end = nullptr;
  unsigned long port = strtoul(parts[1].c_str(), &end, 10);
  if (*end || port == 0 || port > 65535) {
    LOG_ERROR("config %s: invalid RS port '%s'", key.c_str(), parts[1].c_str());
    return false;
  }
  rs.port = static_cast<uint16_t>(port);
  if (parts.size() >= 3) {
    unsigned long w = strtoul(parts[2].c_str(), &end, 10);
    if (*end || w > 65535) {
      LOG_ERROR("config %s: invalid weight '%s'", key.c_str(),
                parts[2].c_str());
      return false;
    }
    rs.weight = static_cast<uint32_t>(w);
  }
  if (parts.size() == 9) {
    std::string mac = parts[3];
    for (int i = 4; i < 9; ++i)
      mac += ":" + parts[i];
    if (!parse_mac(mac, rs.mac)) {
      LOG_ERROR("config %s: invalid MAC '%s'", key.c_str(), mac.c_str());
      return false;
    }
  }
  return true;
}

bool Config::parse_services() {
  for (const auto &sec : sections_) {
    if (sec.first.compare(0, 8, "service.") != 0)
      continue;
    const std::string &name = sec.first;
    ServiceConf svc;
    svc.name = name.substr(8);

    if (!parse_ipv4(get(name, "vip"), svc.vip)) {
      LOG_ERROR("config [%s] vip: missing or invalid", name.c_str());
      return false;
    }
    uint32_t port = 0;
    if (!get_u32(name, "port", port, 1, 65535) || port == 0) {
      LOG_ERROR("config [%s] port: missing or invalid", name.c_str());
      return false;
    }
    svc.port = static_cast<uint16_t>(port);

    std::string proto = to_lower(get(name, "proto", "tcp"));
    if (proto == "tcp") {
      svc.proto = 6;
    } else if (proto == "udp") {
      svc.proto = 17;
    } else {
      LOG_ERROR("config [%s] proto = '%s': expected tcp or udp", name.c_str(),
                proto.c_str());
      return false;
    }

    std::string sched = to_lower(get(name, "scheduler", "wrr"));
    if (sched == "wrr" || sched == "rr") {
      svc.sched = SchedulerType::WRR;
    } else if (sched == "maglev" || sched == "consistent_hash") {
      svc.sched = SchedulerType::MAGLEV;
    } else {
      LOG_ERROR("config [%s] scheduler = '%s': expected wrr or maglev",
                name.c_str(), sched.c_str());
      return false;
    }

    // serverN / rsN，按 N 排序，保证 RS 顺序稳定
    std::map<unsigned long, std::string> servers;
    for (const auto &kv : sec.second) {
      for (const char *prefix : {"server", "rs"}) {
        size_t plen = strlen(prefix);
        if (kv.first.compare(0, plen, prefix) == 0 && kv.first.size() > plen &&
            isdigit(static_cast<unsigned char>(kv.first[plen]))) {
          servers[strtoul(kv.first.c_str() + plen, nullptr, 10)] = kv.second;
        }
      }
    }
    for (const auto &kv : servers) {
      RsConf rs;
      if (!parse_rs("[" + name + "] server" + std::to_string(kv.first),
                    kv.second, rs))
        return false;
      svc.rs.push_back(rs);
    }
    lb_.services.push_back(svc);
  }
  return true;
}

bool Config::parse_legacy_services() {
  // 旧格式：[vip] ip/ports + [realserver] count/serverN；每个端口 TCP、UDP 各一个服务
  IPv4Addr vip = 0;
  if (!parse_ipv4(get("vip", "ip"), vip)) {
    LOG_ERROR("config: no [service.*] section and [vip] ip is missing/invalid");
    return false;
  }
  std::vector<RsConf> rs_list;
  uint32_t count = 0;
  if (!get_u32("realserver", "count", count, 0, kMaxRsPerService))
    return false;
  for (uint32_t i = 1; i <= count; ++i) {
    std::string key = "server" + std::to_string(i);
    std::string value = get("realserver", key);
    if (value.empty())
      continue;
    RsConf rs;
    if (!parse_rs("[realserver] " + key, value, rs))
      return false;
    rs_list.push_back(rs);
  }

  std::string ports = get("vip", "ports", "80");
  for (const auto &p : split(ports, ',')) {
    char *end = nullptr;
    unsigned long port = strtoul(p.c_str(), &end, 10);
    if (*end || port == 0 || port > 65535) {
      LOG_ERROR("config [vip] ports: invalid port '%s'", p.c_str());
      return false;
    }
    for (uint8_t proto : {uint8_t(6), uint8_t(17)}) {
      ServiceConf svc;
      svc.name = std::string(proto_name(proto)) + "-" + p;
      svc.vip = vip;
      svc.port = static_cast<uint16_t>(port);
      svc.proto = proto;
      svc.rs = rs_list;
      lb_.services.push_back(svc);
    }
  }
  return true;
}

// ============================================================================
// 校验
// ============================================================================

bool Config::validate() {
  auto &c = lb_;
  if (c.services.empty()) {
    LOG_ERROR("config: no service defined");
    return false;
  }
  if (c.services.size() > kMaxServices) {
    LOG_ERROR("config: too many services (%zu > %zu)", c.services.size(),
              kMaxServices);
    return false;
  }

  std::set<IPv4Addr> vips;
  std::set<std::tuple<IPv4Addr, uint16_t, uint8_t>> keys;
  for (const auto &svc : c.services) {
    if (svc.rs.empty()) {
      LOG_ERROR("config: service %s has no real server", svc.name.c_str());
      return false;
    }
    if (svc.rs.size() > kMaxRsPerService) {
      LOG_ERROR("config: service %s has too many real servers",
                svc.name.c_str());
      return false;
    }
    if (!keys.insert({svc.vip, svc.port, svc.proto}).second) {
      LOG_ERROR("config: duplicate service %s:%u/%s",
                ip_to_string(svc.vip).c_str(), svc.port, proto_name(svc.proto));
      return false;
    }
    vips.insert(svc.vip);
  }

  // FULLNAT：未配置 LIP 时使用第一个 VIP
  if (c.mode == ForwardMode::NAT && c.local_ips.empty())
    c.local_ips.push_back(c.services[0].vip);
  if (c.local_ips.size() > kMaxLocalIps) {
    LOG_ERROR("config: too many local_ips (%zu > %zu)", c.local_ips.size(),
              kMaxLocalIps);
    return false;
  }
  if (c.hc_src == 0 && !c.local_ips.empty())
    c.hc_src = c.local_ips[0];
  if (c.health.enabled && c.hc_src == 0) {
    LOG_ERROR("config: healthcheck needs [network] hc_src in DR mode "
              "(an address on the LB that is not a VIP)");
    return false;
  }
  if (c.mode == ForwardMode::DR && vips.count(c.hc_src)) {
    LOG_ERROR("config: hc_src must not be a VIP in DR mode (RS has VIP on lo)");
    return false;
  }

  // LIP / hc_src 上的端口段留给 SNAT 和健康检查，服务端口不能与之重叠
  std::set<IPv4Addr> locals(c.local_ips.begin(), c.local_ips.end());
  if (c.hc_src)
    locals.insert(c.hc_src);
  for (const auto &svc : c.services) {
    if (locals.count(svc.vip) &&
        in_port_range(svc.port, kNatPortMin, kHcPortMax)) {
      LOG_ERROR("config: service port %u on %s overlaps the reserved "
                "SNAT/health-check port range %u-%u (VIP is also a local IP)",
                svc.port, ip_to_string(svc.vip).c_str(), kNatPortMin,
                kHcPortMax);
      return false;
    }
  }

  if (c.gateway && c.netmask &&
      (c.gateway & c.netmask) != (c.local_ips.empty()
                                      ? (c.services[0].vip & c.netmask)
                                      : (c.local_ips[0] & c.netmask))) {
    LOG_WARN("config: gateway %s is not in the local subnet",
             ip_to_string(c.gateway).c_str());
  }
  return true;
}

// ============================================================================
// 打印
// ============================================================================

void Config::dump() const {
  const auto &c = lb_;
  LOG_INFO("========== Configuration ==========");
  LOG_INFO("Forward Mode: %s, dataplane: %s",
           c.mode == ForwardMode::NAT ? "FULLNAT" : "DR",
           c.dataplane == DataplaneMode::PIPELINE ? "pipeline" : "rtc");
  if (c.log_dir.empty())
    LOG_INFO("Log: level=%s, stderr only", c.log_level.c_str());
  else
    LOG_INFO("Log: level=%s dir=%s max_size=%uMB max_files=%u stderr=%s",
             c.log_level.c_str(), c.log_dir.c_str(), c.log_max_size_mb,
             c.log_max_files, c.log_stderr ? "on" : "off");
  LOG_INFO("Stats interval: %us", c.stats_interval);
  LOG_INFO("Max sessions: %u, TOA: %s, strip TCP timestamp: %s",
           c.max_sessions, c.toa ? "on" : "off",
           c.strip_tcp_timestamp ? "on" : "off");
  LOG_INFO("Timeouts: syn=%u est=%u fin=%u timewait=%u close=%u udp=%u",
           c.timeouts.tcp_syn, c.timeouts.tcp_established, c.timeouts.tcp_fin,
           c.timeouts.tcp_timewait, c.timeouts.tcp_close, c.timeouts.udp);
  LOG_INFO("Network: netmask=%s gateway=%s hc_src=%s",
           ip_to_string(c.netmask).c_str(), ip_to_string(c.gateway).c_str(),
           ip_to_string(c.hc_src).c_str());
  for (auto ip : c.local_ips)
    LOG_INFO("  Local IP: %s", ip_to_string(ip).c_str());
  LOG_INFO("Health check: %s interval=%ums timeout=%ums fall=%u rise=%u",
           c.health.enabled ? "on" : "off", c.health.interval_ms,
           c.health.timeout_ms, c.health.fall, c.health.rise);
  for (const auto &svc : c.services) {
    LOG_INFO("Service %s: %s:%u/%s sched=%s, %zu RS", svc.name.c_str(),
             ip_to_string(svc.vip).c_str(), svc.port, proto_name(svc.proto),
             sched_name(svc.sched), svc.rs.size());
    for (const auto &rs : svc.rs)
      LOG_INFO("    RS %s:%u weight=%u mac=%s", ip_to_string(rs.ip).c_str(),
               rs.port, rs.weight,
               mac_is_zero(rs.mac) ? "(arp)" : mac_to_string(rs.mac).c_str());
  }
  LOG_INFO("====================================");
}

// ============================================================================
// 字符串工具
// ============================================================================

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

std::vector<std::string> Config::split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, sep)) {
    part = trim(part);
    if (!part.empty())
      out.push_back(part);
  }
  return out;
}

} // namespace l4lb
