#pragma once
#include "wal.hpp"
#include <lite3.h>
#include <memory_resource>
#include <unordered_map>
#include <shared_mutex>
#include <vector>
#include <optional>
#include <cstring>

// A wrapper around a single Lite3 buffer using PMR
class Blob {
    std::pmr::vector<uint8_t> buf_;
    size_t cursor_ = 0;
public:
    Blob(std::pmr::memory_resource* mr, size_t cap = 1024) 
        : buf_(cap, mr) {
        lite3_init_obj(buf_.data(), &cursor_, buf_.capacity());
    }

    // Raw write (PUT)
    void overwrite(std::string_view data) {
        if (data.size() > buf_.capacity()) {
            size_t new_cap = std::max(buf_.capacity() * 2, data.size());
            buf_.resize(new_cap);
        }
        std::memcpy(buf_.data(), data.data(), data.size());
    }

    // Zero-Copy Mutation (PATCH)
    bool set_int(std::string_view key, int64_t val) {
        int ret = lite3_set_i64(buf_.data(), &cursor_, 0, buf_.capacity(), key.data(), val);
        if (ret != LITE3_OK) {
            buf_.resize(buf_.capacity() * 2);
            return lite3_set_i64(buf_.data(), &cursor_, 0, buf_.capacity(), key.data(), val) == LITE3_OK;
        }
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

    Shard& get_shard(std::string_view key) {
        size_t h = std::hash<std::string_view>{}(key);
        return *shards_[h % SHARDS];
    }

public:
    Engine(std::string wal_path) {
        wal_ = std::make_unique<WriteAheadLog>(wal_path);
        for(size_t i=0; i<SHARDS; ++i) shards_.push_back(std::make_unique<Shard>());
    }

    std::vector<uint8_t> get(std::string_view key) {
        auto& s = get_shard(key);
        std::shared_lock lock(s.mx);
        if (auto it = s.map.find(std::string(key)); it != s.map.end()) {
            auto v = it->second->view();
            return {v.begin(), v.end()};
        }
        return {};
    }

    void put(std::string key, std::string_view body) {
        wal_->append(WalOp::PUT, key, body);
        auto& s = get_shard(key);
        std::unique_lock lock(s.mx);
        
        if (!s.map.contains(key)) {
            s.map[key] = std::make_unique<Blob>(&s.pool, body.size());
        }
        s.map[key]->overwrite(body);
    }

    void patch_int(std::string key, std::string field, int64_t val) {
        std::string log_payload = field + ":" + std::to_string(val);
        wal_->append(WalOp::PATCH_I64, key, log_payload);

        auto& s = get_shard(key);
        std::unique_lock lock(s.mx);
        
        if (!s.map.contains(key)) {
            s.map[key] = std::make_unique<Blob>(&s.pool);
        }
        s.map[key]->set_int(field, val);
    }
};
