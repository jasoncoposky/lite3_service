#pragma once
#include "clock.hpp"
#include "wal.hpp"

#include <cstring>
#include <iostream> // For debugging
#include <memory_resource>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "buffer.hpp"
#include "json.hpp" // For lite3::lite3_json

namespace l3kv {

// A wrapper around a single Lite3 buffer using PMR
class Blob {
public: // Made buf_ public for easier access for now, consider making private
        // with accessors later
  lite3cpp::Buffer buf_;

public:
  Blob(std::pmr::memory_resource *mr, size_t cap = 1024) : buf_(cap) {
    // pmr::memory_resource not directly used by lite3::Buffer constructor,
    // but could be passed to internal allocations if Buffer supported it.
    // For now, it's a placeholder.
    buf_.init_object(); // Initialize as an empty object by default
  }

  // Overwrite the internal buffer with new JSON string data
  void overwrite(std::string_view json_data_str) {
    // Convert the JSON string to a lite3::Buffer
    lite3cpp::Buffer new_buf =
        lite3cpp::lite3_json::from_json_string(std::string(json_data_str));
    // Replace the internal buffer's data
    buf_ = std::move(new_buf);
  }

  // Zero-Copy Mutation (PATCH)
  bool set_int(std::string_view key, int64_t val) {
    buf_.set_i64(0, key, val);
    return true;
  }

  bool set_str(std::string_view key, std::string_view val) {
    buf_.set_str(0, key, val);
    return true;
  }

  // Get a view of the internal buffer's raw data
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

  Shard &get_shard(std::string_view key) {
    size_t h = std::hash<std::string_view>{}(key);
    return *shards_[h % SHARDS];
  }

public:
  Engine(std::string wal_path, uint32_t node_id = 1) : clock_(node_id) {
    wal_ = std::make_unique<WriteAheadLog>(wal_path);
    for (size_t i = 0; i < SHARDS; ++i)
      shards_.push_back(std::make_unique<Shard>());

    // Recover from WAL
    wal_->recover(
        [this](WalOp op, std::string_view key, std::string_view payload) {
          try {
            if (op == WalOp::PUT) {
              apply_put(std::string(key), payload);
            } else if (op == WalOp::PATCH_I64) {
              // payload format: "field:val"
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
              apply_del(key);
            }
          } catch (const std::exception &e) {
            // Silently skip or log to stderr only on failure
            std::cerr << "WAL Recovery Skip: " << e.what() << "\n";
          }
        });
  }

  lite3cpp::Buffer get(std::string_view key) {
    auto &s = get_shard(key);
    std::shared_lock lock(s.mx);
    if (auto it = s.map.find(std::string(key)); it != s.map.end()) {
      return it->second->buf_;
    }
    return lite3cpp::Buffer();
  }

  void put(std::string key, std::string_view json_body) {
    auto now = clock_.now();

    // Create sidecar metadata
    // Format: {"ts": <wall_time>, "l": <logical>, "n": <node>}
    // For now simplistic string
    std::string meta_key = key + ":meta";
    std::string meta_val = "{\"ts\":" + std::to_string(now.wall_time) +
                           ",\"l\":" + std::to_string(now.logical) +
                           ",\"n\":" + std::to_string(now.node_id) + "}";

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::PUT, key, std::string(json_body)});
    batch.push_back({WalOp::PUT, meta_key, meta_val});

    wal_->append_batch(batch);

    // Apply locally
    apply_put(key, json_body);
    // TODO: Apply meta to sidecar blob (not implemented in in-memory map yet?)
    // Actually, we should probably store meta in the map too if we want to use
    // it. For now, Engine::get() ignores it, but it sits in the WAL. To support
    // replication we need to query it. Let's also apply it to in-memory map so
    // we can verify it.
    apply_put(meta_key, meta_val);
  }

  void patch_int(std::string key, std::string field, int64_t val) {
    auto now = clock_.now();
    std::string meta_key = key + ":meta";
    // TODO: Hash the field path. For now assume field is simple.
    // Sidecar Format: { "field_path": "wall:logical:node" }
    std::string ts_str = std::to_string(now.wall_time) + ":" +
                         std::to_string(now.logical) + ":" +
                         std::to_string(now.node_id);

    std::string log_payload_int = field + ":" + std::to_string(val);
    std::string log_payload_str =
        field + ":" + ts_str; // Reusing "field" as key in meta

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::PATCH_I64, key, log_payload_int});
    batch.push_back({WalOp::PATCH_STR, meta_key, log_payload_str});

    wal_->append_batch(batch);

    apply_patch_int(key, field, val);
    apply_patch_str(meta_key, field, ts_str);
  }

  bool del(std::string_view key) {
    wal_->append(WalOp::DELETE_, key, "");
    return apply_del(key);
  }

  void flush() { wal_->flush(); }

  auto get_wal_stats() { return wal_->stats(); }

private:
  void apply_put(std::string key, std::string_view json_body) {
    std::cout << "[Engine] Applying PUT: " << key << " (" << json_body.size()
              << " bytes)\n";
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key)) {
      std::cout << "[Engine] New key. Creating Blob.\n";
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }
    s.map[key]->overwrite(json_body);
    std::cout << "[Engine] Blob overwritten. Size: " << s.map[key]->buf_.size()
              << "\n";
  }

  void apply_patch_int(std::string key, std::string field, int64_t val) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key)) {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }
    s.map[key]->set_int(field, val);
  }

  void apply_patch_str(std::string key, std::string field, std::string val) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key)) {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }
    s.map[key]->set_str(field, val);
  }

  bool apply_del(std::string_view key) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    return s.map.erase(std::string(key)) > 0;
  }
};

} // namespace l3kv