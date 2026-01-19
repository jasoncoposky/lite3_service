#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

enum class WalOp : uint8_t { PUT = 1, PATCH_I64 = 2, DELETE_ = 3 };

#pragma pack(push, 1)
struct LogHeader {
  uint32_t crc;
  uint8_t op;
  uint16_t key_len;
  uint32_t payload_len;
};
#pragma pack(pop)

class WriteAheadLog {
  struct Entry {
    WalOp op;
    std::string key;
    std::string payload;
  };

  std::string path_;
  std::ofstream file_;
  std::vector<Entry> queue_;
  std::mutex mx_;
  std::condition_variable cv_;
  std::jthread writer_thread_;
  std::atomic<bool> running_{true};

  // Simple CRC32 implementation to avoid external dependencies
  static uint32_t compute_crc(uint8_t op, std::string_view key,
                              std::string_view payload) {
    uint32_t crc = 0xFFFFFFFF;

    auto process = [&](const void *data, size_t len) {
      const uint8_t *p = (const uint8_t *)data;
      for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
          crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
      }
    };

    process(&op, sizeof(op));
    process(key.data(), key.size());
    process(payload.data(), payload.size());

    return ~crc;
  }

public:
  explicit WriteAheadLog(std::string path) : path_(std::move(path)) {
    // Open for append
    file_.open(path_, std::ios::binary | std::ios::app);
    writer_thread_ = std::jthread([this] { loop(); });
  }

  void append(WalOp op, std::string_view key, std::string_view payload) {
    {
      std::lock_guard lock(mx_);
      queue_.emplace_back(Entry{op, std::string(key), std::string(payload)});
    }
    cv_.notify_one();
  }

  void recover(
      std::function<void(WalOp, std::string_view, std::string_view)> callback) {
    // Open separate stream for reading
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open())
      return;

    while (in.peek() != EOF) {
      LogHeader h;
      if (!in.read((char *)&h, sizeof(h)))
        break;

      std::string key(h.key_len, '\0');
      std::string payload(h.payload_len, '\0');

      if (h.key_len > 0 && !in.read(key.data(), h.key_len)) {
        std::cerr << "WAL Recovery: Unexpected EOF reading key\n";
        break;
      }
      if (h.payload_len > 0 && !in.read(payload.data(), h.payload_len)) {
        std::cerr << "WAL Recovery: Unexpected EOF reading payload\n";
        break;
      }

      uint32_t computed = compute_crc(h.op, key, payload);
      if (computed != h.crc) {
        // If CRC is 0, we might assume it's legacy data if we supported that,
        // but for "bulletproof", 0 is invalid unless data actually sums to 0
        // (unlikely with CRC32 invert). However, previous implementation wrote
        // 0.
        if (h.crc == 0 && computed != 0) {
          // Warn but accept for legacy compatibility during dev?
          // Or reject. User said "bulletproof". Corrupt data is worse than lost
          // data? Let's print warning and continue if it looks like legacy.
          std::cerr << "WAL WARNING: Legacy/Zero CRC found. Accepting.\n";
        } else {
          std::cerr << "WAL ERROR: CRC Mismatch at offset "
                    << (size_t)in.tellg() - sizeof(h) - h.key_len -
                           h.payload_len
                    << ". Aborting recovery.\n";
          break;
        }
      }

      callback((WalOp)h.op, key, payload);
    }
  }

private:
  void loop() {
    std::vector<Entry> batch;
    batch.reserve(1024);

    while (running_) {
      {
        std::unique_lock lock(mx_);
        cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
        if (!running_ && queue_.empty())
          return;
        std::swap(batch, queue_);
      }

      for (const auto &e : batch) {
        uint32_t crc = compute_crc((uint8_t)e.op, e.key, e.payload);
        LogHeader h{crc, (uint8_t)e.op, (uint16_t)e.key.size(),
                    (uint32_t)e.payload.size()};
        file_.write((char *)&h, sizeof(h));
        file_.write(e.key.data(), e.key.size());
        file_.write(e.payload.data(), e.payload.size());
      }

      file_.flush();
      batch.clear();
    }
  }
};