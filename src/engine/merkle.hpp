#ifndef L3KV_ENGINE_MERKLE_HPP
#define L3KV_ENGINE_MERKLE_HPP

#include <array>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace l3kv {

// Simple stable hash (FNV-1a 64-bit)
inline uint64_t fnv1a_64(const void *data, size_t len) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= ptr[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

inline uint64_t fnv1a_64(std::string_view s) {
  return fnv1a_64(s.data(), s.size());
}

// 4-Level Hex Tree
// Level 0: 1 Node (Root)
// Level 1: 16 Nodes
// Level 2: 256 Nodes
// Level 3: 4096 Nodes
// Level 4: 65536 Buckets (Leaves)

class MerkleTree {
  // We represent the tree as a flat array or sparse map?
  // Flat array for 65k leaves + parents is roughly 70k uint64_t ~= 560KB. Very
  // cheap. Structure: L4 (Leaves): Offset 4681 (approx) -> 65536 entries?
  // Actually, standard heap layout:
  // Node i has children 16*i + 1 ... 16*i + 16.
  // But 16-ary tree logic is a bit manual.
  //
  // Let's use Layered Arrays for simplicity.
  // L4: 65536 hashes
  // L3: 4096 hashes
  // L2: 256 hashes
  // L1: 16 hashes
  // L0: 1 hash

  std::array<std::vector<uint64_t>, 5> layers_;
  std::array<std::vector<bool>, 5> dirty_; // Dirty bits for lazy calc
  mutable std::mutex mx_;

public:
  MerkleTree() {
    // Initialize layers
    layers_[4].resize(65536, 0);
    layers_[3].resize(4096, 0);
    layers_[2].resize(256, 0);
    layers_[1].resize(16, 0);
    layers_[0].resize(1, 0);

    // Initial dirty state: clean (all zero)
    dirty_[4].resize(65536, false);
    dirty_[3].resize(4096, false);
    dirty_[2].resize(256, false);
    dirty_[1].resize(16, false);
    dirty_[0].resize(1, false);
  }

  // Update a key.
  // val_hash could be hash of (Timestamp + Value) to capture all state.
  void update(std::string_view key, uint64_t val_hash) {
    std::lock_guard<std::mutex> lock(mx_);

    // 1. Map Key to Bucket (0-65535)
    // We use the top 16 bits of the key's hash
    uint64_t k_hash = fnv1a_64(key);
    uint32_t bucket_idx = (k_hash >> 48) & 0xFFFF; // Top 16 bits

    // 2. XOR into Leaf Bucket
    // Merkle bucket = XOR sum of all items in it. Order independent.
    // If update is "New Value replacing Old Value", we need XOR(Old) ^
    // XOR(New). PROBLEM: We don't know "Old" value hash here unless we store it
    // or caller provides it. Standard Merkle usually recomputes bucket from
    // scratch or maintains Count/XorSum. BUT: L3KV `update` is called on Put.
    // We might not know previous state easily without read. OPTION A: Caller
    // provides (OldHash, NewHash). OPTION B: We only support "Rebuild Bucket"
    // or Assume caller handles the XOR delta logic?
    //
    // Simplify: The "Value" of the bucket is the Hash of its content.
    // In Dynamo/Cassandra, bucket = Hash(Keys in range).
    // If we use XOR-Filter style: BucketValue ^= Hash(Key, Val).
    // On Delete: BucketValue ^= Hash(Key, Val). (XOR removes it).
    // On Overwrite: BucketValue ^= Hash(Key, OldVal) ^ Hash(Key, NewVal).
    //
    // So we need `update(key, old_val_hash, new_val_hash)`.
    // If insert: old=0.
    // If delete: new=0.

    // Let's change signature to support Delta.
    // But wait, Store apply_put doesn't know old val hash easily without
    // deserializing old blob.
    //
    // Alternative: "Dirty Bucket" just marks bucket as "Needs Rescan".
    // Then a background job scans the Table for keys in that bucket range and
    // recomputes. That's more robust but slower.
    //
    // Given the prompt "High-Granularity... Dirty Bucket Merkle", usually
    // implies: Write -> Mark Bucket Dirty. Read Root -> If Dirty, Recompute
    // affected branches. But "Recompute" requires source data. If we don't
    // store the tree state per key, we must scan table. Scanning 1/65536th of
    // table is fast. BUT our Engine doesn't support Range Scans yet easily
    // (Hash Map Sharded). Sharded Map makes range scan hard (keys scattered).
    //
    // Workaround: We will use the XOR approach for O(1) updates.
    // Restriction: Store MUST provide old hash.
    // This assumes we can calculate hash(val) cheaply.

    // Let's implement `xor_update(bucket, hash_delta)`.
    // Caller is responsible for xor-ing old and new.

    layers_[4][bucket_idx] ^= val_hash;

    // Mark path dirty up to root
    mark_dirty(4, bucket_idx);
  }

  // Helper for XOR update logic
  // delta = Hash(Key, NewVal) ^ Hash(Key, OldVal)
  void apply_delta(std::string_view key, uint64_t hash_delta) {
    std::lock_guard<std::mutex> lock(mx_);
    uint64_t k_hash = fnv1a_64(key);
    uint32_t bucket_idx = (k_hash >> 48) & 0xFFFF;

    layers_[4][bucket_idx] ^= hash_delta;
    mark_dirty(4, bucket_idx);
  }

  uint64_t get_root_hash() {
    std::lock_guard<std::mutex> lock(mx_);
    recompute_dirty();
    return layers_[0][0];
  }

  uint64_t get_node_hash(int level, size_t index) {
    std::lock_guard<std::mutex> lock(mx_);
    recompute_dirty(); // Ensure clean before reading any node? Or just specific
                       // path?
    // Simple: recompute all dirty.
    if (level < 0 || level > 4)
      return 0;
    if (index >= layers_[level].size())
      return 0;
    return layers_[level][index];
  }

private:
  void mark_dirty(int level, size_t idx) {
    // Mark this node dirty?
    // Actually, if we update L4, we need L3 parent to know it's stale.
    // L3 parent idx = idx / 16.
    if (level > 0) {
      size_t parent = idx / 16;
      if (!dirty_[level - 1][parent]) {
        dirty_[level - 1][parent] = true;
        mark_dirty(level - 1, parent);
      }
    }
  }

  void recompute_dirty() {
    // Recompute from L4 up to L0?
    // Actually, we marked path dirty from L3 up to L0.
    // We iterate levels 3 down to 0 ? No, we need 4->3, 3->2, etc.
    // Wait, modifying L4 is instant. We need to propagate changes up.
    // Strategy: To get root, we need clean L0. L0 depends on L1...
    // We should iterate Levels 3 to 0. (L4 is always "source of truth").

    // But we only want to recompute *dirty* nodes.
    // Iterating all 4096 L3 nodes to check dirty is fast.

    // Level 3 (Parents of Leaves)
    for (size_t i = 0; i < layers_[3].size(); ++i) {
      if (dirty_[3][i]) {
        uint64_t sum = 0; // Using XOR sum or Hash of children?
        // Merkle usually Hash(Concat Children).
        // XOR sum of children is also valid for homomorphic properties but
        // weaker collision. Let's use Hash(Children). Combine 16 children.
        // Optimization: Just XOR them?
        // "Dirty Bucket Merkle" often implies precise hashing.
        // Let's do Hash of Children hashes.
        uint64_t combined = 0;
        for (int k = 0; k < 16; ++k) {
          // Mix child hash
          uint64_t child_h = layers_[4][i * 16 + k];
          // Simple mix: Rotate and XOR?
          // Or just re-hash the block of 16 uint64s.
          // Re-hashing is better.
          // We'll accumulate in a buffer if needed, or incremental fnv?
          // Incremental FNV over 16 * 8 bytes.
          // Simpler: combined = fnv1a( &child_h, 8 ) mixed.
        }
        // Implementation: Hash the array slice.
        const auto *data = &layers_[4][i * 16];
        layers_[3][i] = fnv1a_64(data, 16 * sizeof(uint64_t));
        dirty_[3][i] = false;
      }
    }

    // Level 2
    for (size_t i = 0; i < layers_[2].size(); ++i) {
      if (dirty_[2][i]) {
        const auto *data = &layers_[3][i * 16];
        layers_[2][i] = fnv1a_64(data, 16 * sizeof(uint64_t));
        dirty_[2][i] = false;
      }
    }

    // Level 1
    for (size_t i = 0; i < layers_[1].size(); ++i) {
      if (dirty_[1][i]) {
        const auto *data = &layers_[2][i * 16];
        layers_[1][i] = fnv1a_64(data, 16 * sizeof(uint64_t));
        dirty_[1][i] = false;
      }
    }

    // Level 0
    if (dirty_[0][0]) {
      const auto *data = &layers_[1][0];
      layers_[0][0] = fnv1a_64(data, 16 * sizeof(uint64_t));
      dirty_[0][0] = false;
    }
  }
};

} // namespace l3kv

#endif
