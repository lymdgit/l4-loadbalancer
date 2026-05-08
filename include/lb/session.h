/**
 * @file session.h
 * @brief Session manager backed by DPDK hash tables.
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include "common/logger.h"
#include "common/types.h"

#include <array>
#include <cstdio>
#include <cstring>

#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

namespace l4lb {

struct SessionDebugStats {
  uint64_t lookup_hit = 0;
  uint64_t lookup_miss = 0;
  uint64_t reverse_hit = 0;
  uint64_t reverse_miss = 0;
  uint64_t create = 0;
  uint64_t update_miss = 0;
  uint64_t cleanup_removed = 0;
};

class SessionManager {
public:
  static SessionManager &instance() {
    static SessionManager mgr;
    return mgr;
  }

  bool init() {
    if (!init_reverse_table()) {
      return false;
    }

    unsigned ordinal = 0;
    unsigned lcore_id = 0;
    RTE_LCORE_FOREACH(lcore_id) {
      if (!init_forward_table(lcore_id, ordinal++)) {
        cleanup();
        return false;
      }
    }

    LOG_INFO("Forward session hash tables initialized: per_lcore_capacity=%u",
             kForwardCapacity);
    return true;
  }

  void cleanup() {
    for (auto &tbl : tables_) {
      if (tbl.hash) {
        rte_hash_free(tbl.hash);
        tbl.hash = nullptr;
      }
      if (tbl.entries) {
        rte_free(tbl.entries);
        tbl.entries = nullptr;
      }
      tbl.last_cleanup_tsc = 0;
      tbl.active_sessions = 0;
      tbl.total_sessions = 0;
      tbl.next_nat_port = 0;
      tbl.debug = {};
    }

    if (reverse_hash_) {
      rte_hash_free(reverse_hash_);
      reverse_hash_ = nullptr;
    }
    if (reverse_data_) {
      rte_free(reverse_data_);
      reverse_data_ = nullptr;
    }
  }

  void set_timeout(uint32_t seconds) {
    timeout_sec_ = seconds;
    timeout_tsc_ = rte_get_tsc_hz() * seconds;
    touch_tsc_ = timeout_tsc_ / 4;
    cleanup_interval_tsc_ = rte_get_tsc_hz();
  }

  Session *lookup_ptr(const FiveTuple &tuple, uint64_t now_tsc) {
    Table &tbl = local_table();
    if (unlikely(tbl.hash == nullptr || tbl.entries == nullptr)) {
      tbl.debug.lookup_miss++;
      return nullptr;
    }

    int32_t idx = rte_hash_lookup(tbl.hash, &tuple);
    if (idx < 0) {
      tbl.debug.lookup_miss++;
      return nullptr;
    }

    ForwardEntry &entry = tbl.entries[idx];
    if (unlikely(!entry.active)) {
      tbl.debug.lookup_miss++;
      return nullptr;
    }

    tbl.debug.lookup_hit++;
    if (now_tsc - entry.session.last_active > touch_tsc_) {
      entry.session.last_active = now_tsc;
    }
    return &entry.session;
  }

  bool lookup(const FiveTuple &tuple, Session &session, uint64_t now_tsc) {
    Session *found = lookup_ptr(tuple, now_tsc);
    if (!found) {
      return false;
    }
    session = *found;
    return true;
  }

  bool lookup_reverse(const FiveTuple &reverse_tuple, Session &session) {
    Table &tbl = local_table();
    int32_t idx = rte_hash_lookup(reverse_hash_, &reverse_tuple);
    if (idx < 0) {
      tbl.debug.reverse_miss++;
      return false;
    }

    const ReverseEntry &entry = reverse_data_[idx];
    session.client_tuple = entry.client_tuple;
    session.real_server_id = entry.real_server_id;
    session.nat_src_port = entry.nat_src_port;
    session.server_tuple = reverse_tuple;
    tbl.debug.reverse_hit++;
    return true;
  }

  Port create(const FiveTuple &client_tuple, uint32_t server_id,
              uint64_t now_tsc, IPv4Addr rs_ip = 0, Port rs_port = 0) {
    Table &tbl = local_table();
    if (unlikely(tbl.hash == nullptr || tbl.entries == nullptr)) {
      tbl.debug.update_miss++;
      return 0;
    }

    Session session;
    session.client_tuple = client_tuple;
    session.real_server_id = server_id;
    session.nat_src_port = 0;
    session.create_time = now_tsc;
    session.last_active = now_tsc;
    session.packets = 0;
    session.bytes = 0;

    int32_t idx = rte_hash_lookup(tbl.hash, &client_tuple);
    bool is_new = false;
    if (idx < 0) {
      idx = rte_hash_add_key(tbl.hash, &client_tuple);
      if (idx < 0) {
        LOG_WARN("forward rte_hash_add_key failed: %s", rte_strerror(-idx));
        tbl.debug.update_miss++;
        return session.nat_src_port;
      }
      is_new = true;
    }

    if (rs_ip != 0) {
      session.nat_src_port =
          allocate_nat_src_port(tbl, rs_ip, rs_port, client_tuple, server_id);
      session.server_tuple =
          FiveTuple(rs_ip, client_tuple.dst_ip, rs_port, session.nat_src_port,
                    client_tuple.protocol);
    }

    tbl.entries[idx].session = session;
    tbl.entries[idx].active = true;

    tbl.debug.create++;
    tbl.total_sessions++;
    if (is_new) {
      tbl.active_sessions++;
    }
    return session.nat_src_port;
  }

  void touch_session(Session *session, uint64_t bytes, uint64_t now_tsc) {
    if (unlikely(session == nullptr)) {
      local_table().debug.update_miss++;
      return;
    }
    if (now_tsc - session->last_active > touch_tsc_) {
      session->last_active = now_tsc;
    }
    session->packets++;
    session->bytes += bytes;
  }

  void update_stats(const FiveTuple &tuple, uint64_t bytes, uint64_t now_tsc) {
    Session *session = lookup_ptr(tuple, now_tsc);
    if (session) {
      touch_session(session, bytes, now_tsc);
    } else {
      local_table().debug.update_miss++;
    }
  }

  size_t cleanup_local(uint64_t now_tsc) {
    Table &tbl = local_table();
    if (tbl.entries == nullptr || now_tsc - tbl.last_cleanup_tsc <
                                      cleanup_interval_tsc_) {
      return 0;
    }

    size_t removed_count = 0;
    for (uint32_t idx = 0; idx < kForwardCapacity; ++idx) {
      ForwardEntry &entry = tbl.entries[idx];
      if (!entry.active ||
          !entry.session.is_expired(now_tsc, timeout_tsc_)) {
        continue;
      }

      if (entry.session.server_tuple.src_ip != 0) {
        rte_hash_del_key(reverse_hash_, &entry.session.server_tuple);
      }
      rte_hash_del_key(tbl.hash, &entry.session.client_tuple);
      entry.active = false;
      entry.session = Session{};
      removed_count++;
    }

    if (removed_count > tbl.active_sessions) {
      tbl.active_sessions = 0;
    } else {
      tbl.active_sessions -= removed_count;
    }
    tbl.last_cleanup_tsc = now_tsc;
    tbl.debug.cleanup_removed += removed_count;
    return removed_count;
  }

  Statistics get_stats() const {
    Statistics s{};
    for (const auto &tbl : tables_) {
      s.active_sessions += tbl.active_sessions;
      s.total_sessions += tbl.total_sessions;
    }
    return s;
  }

  SessionDebugStats get_debug_stats() const {
    SessionDebugStats s{};
    for (const auto &tbl : tables_) {
      s.lookup_hit += tbl.debug.lookup_hit;
      s.lookup_miss += tbl.debug.lookup_miss;
      s.reverse_hit += tbl.debug.reverse_hit;
      s.reverse_miss += tbl.debug.reverse_miss;
      s.create += tbl.debug.create;
      s.update_miss += tbl.debug.update_miss;
      s.cleanup_removed += tbl.debug.cleanup_removed;
    }
    return s;
  }

private:
  SessionManager() : timeout_sec_(300), timeout_tsc_(0) {}

  struct ReverseEntry {
    FiveTuple client_tuple;
    uint32_t real_server_id;
    Port nat_src_port;
  };

  struct ForwardEntry {
    Session session;
    bool active;
  };

  struct Table {
    struct rte_hash *hash = nullptr;
    ForwardEntry *entries = nullptr;
    uint64_t last_cleanup_tsc = 0;
    uint64_t active_sessions = 0;
    uint64_t total_sessions = 0;
    uint32_t port_offset = 0;
    uint32_t port_stride = 1;
    uint32_t next_nat_port = 0;
    SessionDebugStats debug{};
  } __rte_cache_aligned;

  bool init_reverse_table() {
    struct rte_hash_parameters params = {};
    params.name = "reverse_session_hash";
    params.entries = kReverseCapacity;
    params.key_len = sizeof(FiveTuple);
    params.hash_func = rte_jhash;
    params.hash_func_init_val = 0;
    params.socket_id = rte_socket_id();
    params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;

    reverse_hash_ = rte_hash_create(&params);
    if (!reverse_hash_) {
      LOG_ERROR("Failed to create reverse rte_hash: %s",
                rte_strerror(rte_errno));
      return false;
    }

    reverse_data_ = static_cast<ReverseEntry *>(rte_zmalloc(
        "reverse_data", sizeof(ReverseEntry) * kReverseCapacity,
        RTE_CACHE_LINE_SIZE));
    if (!reverse_data_) {
      LOG_ERROR("Failed to allocate reverse data array");
      return false;
    }

    LOG_INFO("Reverse session hash table initialized: capacity=%u",
             kReverseCapacity);
    return true;
  }

  bool init_forward_table(unsigned lcore_id, unsigned ordinal) {
    Table &tbl = tables_[lcore_id];
    char name[RTE_HASH_NAMESIZE];
    std::snprintf(name, sizeof(name), "forward_session_%u", lcore_id);

    struct rte_hash_parameters params = {};
    params.name = name;
    params.entries = kForwardCapacity;
    params.key_len = sizeof(FiveTuple);
    params.hash_func = rte_jhash;
    params.hash_func_init_val = 0;
    params.socket_id = rte_socket_id();
    params.extra_flag = 0;

    tbl.hash = rte_hash_create(&params);
    if (!tbl.hash) {
      LOG_ERROR("Failed to create forward rte_hash for lcore %u: %s",
                lcore_id, rte_strerror(rte_errno));
      return false;
    }

    std::snprintf(name, sizeof(name), "forward_data_%u", lcore_id);
    tbl.entries = static_cast<ForwardEntry *>(rte_zmalloc(
        name, sizeof(ForwardEntry) * kForwardCapacity, RTE_CACHE_LINE_SIZE));
    if (!tbl.entries) {
      LOG_ERROR("Failed to allocate forward data for lcore %u", lcore_id);
      return false;
    }

    tbl.port_offset = ordinal;
    tbl.port_stride = rte_lcore_count() ? rte_lcore_count() : 1;
    return true;
  }

  Table &local_table() {
    unsigned lcore = rte_lcore_id();
    if (unlikely(lcore >= RTE_MAX_LCORE)) {
      lcore = rte_get_main_lcore();
    }
    return tables_[lcore];
  }

  Port allocate_nat_src_port(Table &tbl, IPv4Addr rs_ip, Port rs_port,
                             const FiveTuple &client_tuple,
                             uint32_t server_id) {
    if (!rs_ip || !rs_port) {
      return 0;
    }

    static const uint16_t kPortMin = 10000;
    static const uint16_t kPortMax = 60000;
    static const uint32_t kPortRange = kPortMax - kPortMin + 1;

    for (uint32_t i = 0; i < kPortRange; ++i) {
      uint32_t next = tbl.next_nat_port++;
      uint32_t slot =
          (tbl.port_offset + next * tbl.port_stride) % kPortRange;
      uint16_t host_port = static_cast<uint16_t>(kPortMin + slot);
      Port nat_port = rte_cpu_to_be_16(host_port);

      FiveTuple reverse_tuple(rs_ip, client_tuple.dst_ip, rs_port, nat_port,
                              client_tuple.protocol);

      if (rte_hash_lookup(reverse_hash_, &reverse_tuple) >= 0) {
        continue;
      }

      int32_t idx = rte_hash_add_key(reverse_hash_, &reverse_tuple);
      if (idx < 0) {
        LOG_WARN("reverse rte_hash_add_key failed: %s", rte_strerror(-idx));
        continue;
      }

      reverse_data_[idx].client_tuple = client_tuple;
      reverse_data_[idx].real_server_id = server_id;
      reverse_data_[idx].nat_src_port = nat_port;
      return nat_port;
    }

    LOG_WARN("NAT port allocation exhausted, fallback to client src_port");
    return client_tuple.src_port;
  }

  uint32_t timeout_sec_;
  uint64_t timeout_tsc_;
  uint64_t touch_tsc_{0};
  uint64_t cleanup_interval_tsc_{0};

  static const uint32_t kForwardCapacity = 65536;
  static const uint32_t kReverseCapacity = 131072;

  std::array<Table, RTE_MAX_LCORE> tables_{};
  struct rte_hash *reverse_hash_ = nullptr;
  ReverseEntry *reverse_data_ = nullptr;

  SessionManager(const SessionManager &) = delete;
  SessionManager &operator=(const SessionManager &) = delete;
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
