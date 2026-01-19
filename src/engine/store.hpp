#pragma once
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

  Shard &get_shard(std::string_view key) {
    size_t h = std::hash<std::string_view>{}(key);
    return *shards_[h % SHARDS];
  }

public:
  Engine(std::string wal_path) {
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
    wal_->append(WalOp::PUT, key, json_body);
    apply_put(std::move(key), json_body);
  }

  void patch_int(std::string key, std::string field, int64_t val) {
    std::string log_payload = field + ":" + std::to_string(val);
    wal_->append(WalOp::PATCH_I64, key, log_payload);
    apply_patch_int(std::move(key), field, val);
  }

  bool del(std::string_view key) {
    wal_->append(WalOp::DELETE_, key, "");
    return apply_del(key);
  }

  void flush() { wal_->flush(); }

  auto get_wal_stats() { return wal_->stats(); }

private:
  void apply_put(std::string key, std::string_view json_body) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key)) {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }
    s.map[key]->overwrite(json_body);
  }

  void apply_patch_int(std::string key, std::string field, int64_t val) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    if (!s.map.contains(key)) {
      s.map[key] = std::make_unique<Blob>(&s.pool);
    }
    s.map[key]->set_int(field, val);
  }

  bool apply_del(std::string_view key) {
    auto &s = get_shard(key);
    std::unique_lock lock(s.mx);
    return s.map.erase(std::string(key)) > 0;
  }
};