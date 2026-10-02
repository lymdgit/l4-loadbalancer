/**
 * @file dpdk_ring.c
 * @brief rte_ring_create_elem 的 C 链接转调（见 include/common/dpdk_ring.h）
 */

#include <rte_ring.h>

struct rte_ring *l4lb_ring_create_elem(const char *name, unsigned int esize,
                                       unsigned int count, int socket_id,
                                       unsigned int flags) {
  return rte_ring_create_elem(name, esize, count, socket_id, flags);
}
