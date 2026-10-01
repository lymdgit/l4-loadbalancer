/**
 * @file consistent_hash.cpp
 * @brief MurmurHash3 与一致性哈希环实现
 */

#include "lb/consistent_hash.h"

#include <string>

namespace l4lb {

// ============================================================================
// MurmurHash3
// ============================================================================

uint32_t MurmurHash3::hash(const void *key, size_t len, uint32_t seed) {
  const uint8_t *data = static_cast<const uint8_t *>(key);
  const int nblocks = len / 4;

  uint32_t h1 = seed;
  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;

  // Body
  const uint32_t *blocks = reinterpret_cast<const uint32_t *>(data);
  for (int i = 0; i < nblocks; ++i) {
    uint32_t k1 = blocks[i];
    k1 *= c1;
    k1 = rotl32(k1, 15);
    k1 *= c2;

    h1 ^= k1;
    h1 = rotl32(h1, 13);
    h1 = h1 * 5 + 0xe6546b64;
  }

  // Tail
  const uint8_t *tail = data + nblocks * 4;
  uint32_t k1 = 0;
  switch (len & 3) {
  case 3:
    k1 ^= tail[2] << 16;
    [[fallthrough]];
  case 2:
    k1 ^= tail[1] << 8;
    [[fallthrough]];
  case 1:
    k1 ^= tail[0];
    k1 *= c1;
    k1 = rotl32(k1, 15);
    k1 *= c2;
    h1 ^= k1;
  }

  // Finalization
  h1 ^= len;
  h1 = fmix32(h1);
  return h1;
}

// ============================================================================
// ConsistentHashRing
// ============================================================================

void ConsistentHashRing::add_node(uint32_t server_id, uint32_t weight) {
  std::lock_guard<std::mutex> lock(mutex_);

  uint32_t replicas = (virtual_nodes_ * weight) / 100;
  if (replicas < 1)
    replicas = 1;

  for (uint32_t i = 0; i < replicas; ++i) {
    std::string key = std::to_string(server_id) + "#" + std::to_string(i);
    uint32_t hash = MurmurHash3::hash(key.data(), key.size());
    ring_[hash] = server_id;
  }
}

void ConsistentHashRing::remove_node(uint32_t server_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = ring_.begin(); it != ring_.end();) {
    if (it->second == server_id) {
      it = ring_.erase(it);
    } else {
      ++it;
    }
  }
}

bool ConsistentHashRing::get_server(const FiveTuple &tuple,
                                    uint32_t &server_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (ring_.empty())
    return false;

  uint32_t hash = MurmurHash3::hash_tuple(tuple);
  auto it = ring_.lower_bound(hash);

  if (it == ring_.end()) {
    it = ring_.begin();
  }

  server_id = it->second;
  return true;
}

size_t ConsistentHashRing::node_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ring_.size();
}

void ConsistentHashRing::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  ring_.clear();
}

} // namespace l4lb
