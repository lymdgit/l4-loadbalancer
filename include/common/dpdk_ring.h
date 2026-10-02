/**
 * @file dpdk_ring.h
 * @brief 在 C++ 中使用 rte_ring
 *
 * DPDK 24.11 的 rte_ring_elem.h 中，rte_ring_create_elem() 声明在
 * extern "C" 保护之外，C++ 调用会按 C++ 名字修饰导致链接失败；
 * 也不能整体包一层 extern "C"（rte_bitops.h 在 C++ 下使用了函数重载）。
 * 因此用一个 C 文件（src/common/dpdk_ring.c）转调。
 */

#ifndef L4LB_COMMON_DPDK_RING_H
#define L4LB_COMMON_DPDK_RING_H

#include <rte_ring.h>

extern "C" struct rte_ring *l4lb_ring_create_elem(const char *name,
                                                  unsigned int esize,
                                                  unsigned int count,
                                                  int socket_id,
                                                  unsigned int flags);

#endif // L4LB_COMMON_DPDK_RING_H
