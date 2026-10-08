/**
 * @file stats.cpp
 * @brief 计数器名字表
 */

#include "common/stats.h"

namespace l4lb {

const char *stat_name(Stat id) {
  static const char *const names[ST_COUNT] = {
      "rx",
      "tx",
      "tx_full",
      "arp",
      "icmp",
      "tcp",
      "udp",
      "fwd_in",
      "fwd_out",
      "icmp_err_fwd",
      "drop_malformed",
      "drop_fragment",
      "drop_cksum",
      "drop_not_local",
      "drop_no_service",
      "drop_no_session",
      "drop_no_rs",
      "drop_rs_down",
      "drop_table_full",
      "drop_no_port",
      "drop_no_neigh",
      "drop_ttl",
      "drop_redirect",
      "drop_other",
      "sess_new",
      "sess_expired",
      "sess_closed",
      "redirect_out",
      "redirect_in",
      "ring_in",
      "drop_rx_ring",
      "rss_mismatch",
      "rss_no_hash",
      "toa_added",
      "toa_no_room",
      "ts_stripped",
      "hc_resp",
  };
  return id < ST_COUNT ? names[id] : "?";
}

uint64_t StatsTotal::drops() const {
  uint64_t n = c[ST_TX_FULL] + c[ST_DROP_RX_RING];
  for (unsigned i = ST_DROP_MALFORMED; i <= ST_DROP_OTHER; ++i)
    n += c[i];
  return n;
}

} // namespace l4lb
