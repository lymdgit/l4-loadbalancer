/**
 * @file session.cpp
 * @brief per-worker 会话表实现
 */

#include "lb/session.h"

#include "common/logger.h"

#include <cstring>

#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_malloc.h>

namespace l4lb {

namespace {
/// tick 是 1 秒粒度、且只在整秒推进：+1 保证会话至少存活 timeout 秒
inline uint64_t deadline(uint64_t now_tick, uint32_t timeout) {
  return now_tick + (timeout ? timeout : 1) + 1;
}
} // namespace

SessionTable::~SessionTable() { destroy(); }

bool SessionTable::init(const char *name, uint32_t capacity, int socket_id,
                        uint64_t now_tick) {
  capacity_ = capacity;
  cur_tick_ = now_tick;

  // 每个会话最多两个 key；rte_hash 负载过高时插入会失败，留出余量
  struct rte_hash_parameters params = {};
  params.name = name;
  params.entries = capacity * 2 + capacity / 2;
  params.key_len = sizeof(FiveTuple);
  params.hash_func = rte_hash_crc;
  params.hash_func_init_val = 0;
  params.socket_id = socket_id;
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE; // 桶满时用扩展桶，不失败
  hash_ = rte_hash_create(&params);
  if (!hash_) {
    LOG_ERROR("session table %s: rte_hash_create failed: %s", name,
              rte_strerror(rte_errno));
    return false;
  }

  sessions_ = static_cast<Session *>(rte_zmalloc_socket(
      "sessions", sizeof(Session) * capacity, RTE_CACHE_LINE_SIZE, socket_id));
  free_stack_ = static_cast<uint32_t *>(rte_malloc_socket(
      "session_free", sizeof(uint32_t) * capacity, 0, socket_id));
  slots_ = static_cast<uint32_t *>(rte_malloc_socket(
      "session_wheel", sizeof(uint32_t) * kWheelSlots, 0, socket_id));
  if (!sessions_ || !free_stack_ || !slots_) {
    LOG_ERROR("session table %s: out of hugepage memory (%u sessions)", name,
              capacity);
    destroy();
    return false;
  }

  // 空闲栈：栈顶在高位，先分配下标小的会话
  for (uint32_t i = 0; i < capacity; ++i) {
    free_stack_[i] = capacity - 1 - i;
    sessions_[i].wheel_prev = kFree;
  }
  free_top_ = capacity;
  for (uint32_t i = 0; i < kWheelSlots; ++i)
    slots_[i] = kNil;
  pending_ = kNil;
  return true;
}

void SessionTable::destroy() {
  if (hash_)
    rte_hash_free(hash_);
  rte_free(sessions_);
  rte_free(free_stack_);
  rte_free(slots_);
  hash_ = nullptr;
  sessions_ = nullptr;
  free_stack_ = nullptr;
  slots_ = nullptr;
}

Session *SessionTable::lookup(const FiveTuple &key) const {
  void *data = nullptr;
  if (rte_hash_lookup_data(hash_, &key, &data) < 0)
    return nullptr;
  return &sessions_[reinterpret_cast<uintptr_t>(data)];
}

Session *SessionTable::create(const FiveTuple &client, const FiveTuple *server,
                              uint64_t now_tick, uint32_t timeout) {
  if (free_top_ == 0)
    return nullptr;
  uint32_t idx = free_stack_[free_top_ - 1];
  void *data = reinterpret_cast<void *>(static_cast<uintptr_t>(idx));

  if (rte_hash_add_key_data(hash_, &client, data) < 0)
    return nullptr;
  if (server && rte_hash_add_key_data(hash_, server, data) < 0) {
    rte_hash_del_key(hash_, &client);
    return nullptr;
  }
  --free_top_;

  Session *s = &sessions_[idx];
  *s = Session{};
  s->client = client;
  if (server) {
    s->server = *server;
    s->flags |= SF_FULLNAT;
  }
  s->created_tick = now_tick;
  s->expire_tick = deadline(now_tick, timeout);
  wheel_link(idx, s->expire_tick);
  return s;
}

void SessionTable::remove(Session *s) {
  uint32_t idx = index_of(s);
  rte_hash_del_key(hash_, &s->client);
  if (s->flags & SF_FULLNAT)
    rte_hash_del_key(hash_, &s->server);
  wheel_unlink(idx);
  s->wheel_prev = kFree;
  free_stack_[free_top_++] = idx;
}

void SessionTable::touch(Session *s, uint64_t now_tick, uint32_t timeout) {
  uint64_t exp = deadline(now_tick, timeout);
  // 超时变长：只改 expire_tick，时间轮扫到旧槽位时再重新挂
  // 超时变短：必须挪到更早的槽位，否则会晚于预期才过期
  if (exp < s->slot_tick && !s->in_pending) {
    uint32_t idx = index_of(s);
    wheel_unlink(idx);
    wheel_link(idx, exp);
  }
  s->expire_tick = exp;
}

size_t SessionTable::expire(uint64_t now_tick) {
  size_t removed = 0, budget = kExpireBudget;
  while (budget) {
    if (pending_ == kNil) {
      if (cur_tick_ > now_tick)
        break;
      // 把当前槽位整条链表取下来逐个处理（处理中重新挂回的会话不会被重复访问）
      uint32_t slot = cur_tick_ & (kWheelSlots - 1);
      uint32_t head = slots_[slot];
      slots_[slot] = kNil;
      while (head != kNil) {
        uint32_t next = sessions_[head].wheel_next;
        pending_push(head);
        head = next;
      }
      ++cur_tick_;
      continue;
    }

    uint32_t idx = pending_pop();
    Session &s = sessions_[idx];
    --budget;
    if (s.expire_tick <= now_tick) {
      remove(&s);
      ++removed;
    } else {
      wheel_link(idx, s.expire_tick);
    }
  }
  return removed;
}

// ---------------------------------------------------------------------------
// 时间轮链表
// ---------------------------------------------------------------------------

void SessionTable::wheel_link(uint32_t idx, uint64_t tick) {
  // 不能挂到已经处理过的槽位上
  if (tick < cur_tick_)
    tick = cur_tick_;
  Session &s = sessions_[idx];
  uint32_t slot = tick & (kWheelSlots - 1);
  s.slot_tick = tick;
  s.in_pending = false;
  s.wheel_prev = kNil;
  s.wheel_next = slots_[slot];
  if (s.wheel_next != kNil)
    sessions_[s.wheel_next].wheel_prev = idx;
  slots_[slot] = idx;
}

void SessionTable::wheel_unlink(uint32_t idx) {
  Session &s = sessions_[idx];
  uint32_t &head =
      s.in_pending ? pending_ : slots_[s.slot_tick & (kWheelSlots - 1)];
  if (s.wheel_prev != kNil)
    sessions_[s.wheel_prev].wheel_next = s.wheel_next;
  else
    head = s.wheel_next;
  if (s.wheel_next != kNil)
    sessions_[s.wheel_next].wheel_prev = s.wheel_prev;
  s.wheel_prev = s.wheel_next = kNil;
}

void SessionTable::pending_push(uint32_t idx) {
  Session &s = sessions_[idx];
  s.in_pending = true;
  s.wheel_prev = kNil;
  s.wheel_next = pending_;
  if (pending_ != kNil)
    sessions_[pending_].wheel_prev = idx;
  pending_ = idx;
}

uint32_t SessionTable::pending_pop() {
  uint32_t idx = pending_;
  wheel_unlink(idx);
  sessions_[idx].in_pending = false;
  return idx;
}

} // namespace l4lb
