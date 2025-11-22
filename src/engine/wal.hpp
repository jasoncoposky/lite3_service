#pragma once
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <vector>
#include <span>

enum class WalOp : uint8_t { PUT = 1, PATCH_I64 = 2 };

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

public:
    explicit WriteAheadLog(std::string path) : path_(std::move(path)) {
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

private:
    void loop() {
        std::vector<Entry> batch;
        batch.reserve(1024);

        while (running_) {
            {
                std::unique_lock lock(mx_);
                cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
                if (!running_ && queue_.empty()) return;
                std::swap(batch, queue_);
            }

            for (const auto& e : batch) {
                LogHeader h{0, (uint8_t)e.op, (uint16_t)e.key.size(), (uint32_t)e.payload.size()};
                file_.write((char*)&h, sizeof(h));
                file_.write(e.key.data(), e.key.size());
                file_.write(e.payload.data(), e.payload.size());
            }
            
            file_.flush(); // In prod: fsync()
            batch.clear();
        }
    }
};
