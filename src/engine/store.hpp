#pragma once
#include "clock.hpp"
#include "merkle.hpp"
#include "replication_log.hpp"
#include "wal.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string> // Replaced string_view
#include <unordered_map>
#include <vector>

#include "buffer.hpp"
#include "json.hpp"

namespace l3kv {

// A wrapper around a single Lite3 buffer using PMR
class Blob {
public:
  lite3cpp::Buffer buf_;

public:
  Blob(std::pmr::memory_resource *mr, size_t cap = 1024) : buf_(cap) {
    buf_.init_object();
  }

  void overwrite(const std::string &data) {
    bool is_json = false;
    if (!data.empty()) {
      char first = data[0];
      if (first == '{' || first == '[') {
        is_json = true;
      }
    }

    if (is_json) {
      try {
        lite3cpp::Buffer new_buf = lite3cpp::lite3_json::from_json_string(data);
        buf_ = std::move(new_buf);
        return;
      } catch (...) {
        // Fallback to binary if JSON parsing fails?
        // Or assume it was meant to be JSON and failed.
        // For now, let's assume it might be binary if it fails?
        // No, if it starts with { it's likely JSON.
        // But maybe binary data starts with {.
      }
    }

    // Treat as binary
    std::vector<uint8_t> vec(data.begin(), data.end());
    buf_ = lite3cpp::Buffer(std::move(vec));
  }

  bool set_int(const std::string &key, int64_t val) {
    buf_.set_i64(0, key, val);
    return true;
  }

  bool set_str(const std::string &key, const std::string &val) {
    buf_.set_str(0, key, val);
    return true;
  }

  std::span<const uint8_t> view() const { return {buf_.data(), buf_.size()}; }
};

class Engine {
  static constexpr size_t SHARDS = 64;
  struct Shard {
    std::shared_mutex mx;
    std::pmr::unsynchronized_pool_resource pool;
    std::unordered_map<std::string, std::unique_ptr<Blob>> map;
    Shard() : pool(std::pmr::new_delete_resource()) {}
  };

  std::vector<std::unique_ptr<Shard>> shards_;
  std::unique_ptr<WriteAheadLog> wal_;
  HybridLogicalClock clock_;
  MerkleTree merkle_;

  Shard &get_shard(const std::string &key) {
    size_t h = std::hash<std::string>{}(key);
    return *shards_[h % SHARDS];
  }

  // ... (Move methods from bottom to top) ...

private:
  Timestamp get_local_timestamp_internal(const std::string &key) {
    return {0, 0, 0};
  }

  uint64_t hash_blob(const std::unique_ptr<Blob> &blob) {
    if (!blob)
      return 0;
    auto v = blob->view();
    return fnv1a_64(v.data(), v.size());
  }

  void apply_put(const std::string &key, const std::string &json_body) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);

    uint64_t old_h = 0;
    if (s.map.contains(key)) {
      old_h = hash_blob(s.map[key]);
    } else {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }

    s.map[key]->overwrite(json_body);
    uint64_t new_h = hash_blob(s.map[key]);
    lock.unlock();
    merkle_.apply_delta(key, old_h ^ new_h);
  }

  void apply_patch_int(const std::string &key, const std::string &field,
                       int64_t val) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key))
      s.map[key] = std::make_unique<Blob>(&s.pool);

    uint64_t old_h = hash_blob(s.map[key]);
    s.map[key]->set_int(field, val);
    uint64_t new_h = hash_blob(s.map[key]);
    lock.unlock();
    merkle_.apply_delta(key, old_h ^ new_h);
  }

  void apply_patch_str(const std::string &key, const std::string &field,
                       const std::string &val) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key))
      s.map[key] = std::make_unique<Blob>(&s.pool);

    uint64_t old_h = hash_blob(s.map[key]);
    s.map[key]->set_str(field, val);
    uint64_t new_h = hash_blob(s.map[key]);
    lock.unlock();
    merkle_.apply_delta(key, old_h ^ new_h);
  }

  bool apply_del(const std::string &key) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);

    // Tombstone logic: Don't erase. Set to empty.
    if (!s.map.contains(key)) {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }

    uint64_t old_h = hash_blob(s.map[key]);
    s.map[key]->overwrite(""); // Set to empty (Tombstone)
    uint64_t new_h = hash_blob(s.map[key]);

    lock.unlock();
    merkle_.apply_delta(key, old_h ^ new_h);
    return true; // Always "succeeded" in setting tombstone
  }

public:
  Engine(std::string wal_path, uint32_t node_id = 1) : clock_(node_id) {
    wal_ = std::make_unique<WriteAheadLog>(wal_path);
    for (size_t i = 0; i < SHARDS; ++i)
      shards_.push_back(std::make_unique<Shard>());

    wal_->recover(
        [this](WalOp op, std::string_view key, std::string_view payload) {
          try {
            if (op == WalOp::PUT) {
              apply_put(std::string(key), std::string(payload));
            } else if (op == WalOp::PATCH_I64) {
              std::string p(payload);
              size_t colon = p.find(':');
              if (colon != std::string::npos) {
                std::string field = p.substr(0, colon);
                int64_t val = std::stoll(p.substr(colon + 1));
                apply_patch_int(std::string(key), field, val);
              }
            } else if (op == WalOp::PATCH_STR) {
              std::string p(payload);
              size_t colon = p.find(':');
              if (colon != std::string::npos) {
                std::string field = p.substr(0, colon);
                std::string val = p.substr(colon + 1);
                apply_patch_str(std::string(key), field, val);
              }
            } else if (op == WalOp::DELETE_) {
              apply_del(std::string(key));
            }
          } catch (const std::exception &e) {
            std::cerr << "WAL Recovery Skip: " << e.what() << "\n";
          }
        });
  }

  lite3cpp::Buffer get(const std::string &key) {
    auto &s = get_shard(key);
    std::shared_lock lock(s.mx);
    if (auto it = s.map.find(key); it != s.map.end()) {
      return it->second->buf_;
    }
    return lite3cpp::Buffer();
  }

  void put(std::string key, const std::string &json_body) {
    auto now = clock_.now();
    std::string meta_key = key + ":meta";
    std::string meta_val = "{\"ts\":" + std::to_string(now.wall_time) +
                           ",\"l\":" + std::to_string(now.logical) +
                           ",\"n\":" + std::to_string(now.node_id) + "}";

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::PUT, key, json_body});
    batch.push_back({WalOp::PUT, meta_key, meta_val});

    wal_->append_batch(batch);

    apply_put(key, json_body);
    apply_put(meta_key, meta_val);
  }

  void patch_int(std::string key, std::string field, int64_t val) {
    auto now = clock_.now();
    std::string meta_key = key + ":meta";
    std::string ts_str = std::to_string(now.wall_time) + ":" +
                         std::to_string(now.logical) + ":" +
                         std::to_string(now.node_id);

    std::string log_payload_int = field + ":" + std::to_string(val);
    std::string log_payload_str = field + ":" + ts_str;

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::PATCH_I64, key, log_payload_int});
    batch.push_back({WalOp::PATCH_STR, meta_key, log_payload_str});

    wal_->append_batch(batch);

    apply_patch_int(key, field, val);
    apply_patch_str(meta_key, field, ts_str);
  }

  bool del(const std::string &key) {
    auto now = clock_.now();
    std::string meta_key = key + ":meta";
    std::string meta_val = "{\"ts\":" + std::to_string(now.wall_time) +
                           ",\"l\":" + std::to_string(now.logical) +
                           ",\"n\":" + std::to_string(now.node_id) +
                           ",\"tombstone\":true}";

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::DELETE_, key, ""});
    batch.push_back({WalOp::PUT, meta_key, meta_val});

    wal_->append_batch(batch);

    bool existed = apply_del(key);
    apply_put(meta_key, meta_val);
    return existed;
  }

  // ... apply_mutation remains mostly same but uses modified apply_del ...

  inline void apply_mutation(const Mutation &m) {
    // ... (TS checks same as before) ...
    // 1. Get Local TS (Inlined)
    std::string meta_key_lookup = m.key + ":meta";
    auto buf = get(meta_key_lookup);
    Timestamp local_ts{0, 0, 0};
    if (buf.size() > 0) {
      auto type = buf.get_type(0, "ts");
      if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) {
        int64_t w = buf.get_i64(0, "ts");
        uint32_t l = (uint32_t)buf.get_i64(0, "l");
        uint32_t n = (uint32_t)buf.get_i64(0, "n");
        local_ts = {w, l, n};
      }
    }

    if (m.timestamp <= local_ts) {
      std::cerr << "[Store] Rejecting mutation for " << m.key
                << " (Stale). Inc: " << m.timestamp.wall_time
                << " Local: " << local_ts.wall_time << "\n";
      return;
    }
    // std::cerr << "[Store] Applying mutation for " << m.key << "\n";

    std::string meta_key = m.key + ":meta";
    std::string meta_val = "{\"ts\":" + std::to_string(m.timestamp.wall_time) +
                           ",\"l\":" + std::to_string(m.timestamp.logical) +
                           ",\"n\":" + std::to_string(m.timestamp.node_id) +
                           (m.is_delete ? ",\"tombstone\":true" : "") + "}";

    std::vector<BatchOp> wal_batch;
    if (m.is_delete) {
      wal_batch.push_back({WalOp::DELETE_, m.key, ""});
    } else {
      std::string val_str(m.value.begin(), m.value.end());
      wal_batch.push_back({WalOp::PUT, m.key, val_str});
    }
    wal_batch.push_back({WalOp::PUT, meta_key, meta_val});

    wal_->append_batch(wal_batch);

    if (m.is_delete) {
      apply_del(m.key);
    } else {
      std::string val_str(m.value.begin(), m.value.end());
      apply_put(m.key, val_str);
    }
    apply_put(meta_key, meta_val);
  }

  void flush() { wal_->flush(); }
  auto get_wal_stats() { return wal_->stats(); }
  uint64_t get_merkle_root_hash() { return merkle_.get_root_hash(); }
  uint64_t get_merkle_node(int level, int index) {
    return merkle_.get_node_hash(level, index);
  }

  std::vector<std::pair<std::string, uint64_t>>
  get_bucket_keys(int bucket_idx) {
    std::vector<std::pair<std::string, uint64_t>> result;
    for (auto &shard : shards_) {
      std::shared_lock lock(shard->mx);
      for (auto &[k, v] : shard->map) {
        uint64_t kh = fnv1a_64(k);
        uint32_t b = (kh >> 48) & 0xFFFF;
        if (b == (uint32_t)bucket_idx) {
          if (v->view().size() > 0) // Only send non-tombstones? No, we MUST
                                    // send tombstones for sync!
            result.push_back({k, hash_blob(v)});
          else {
            // It's a tombstone. Existing sync logic relies on hash mismatch.
            // If we send hash of empty blob, and they have value, hashes
            // differ. So we include it.
            result.push_back({k, hash_blob(v)});
          }
        }
      }
    }
    return result;
  }
};

} // namespace l3kv
