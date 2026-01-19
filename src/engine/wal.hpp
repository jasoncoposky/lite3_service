#pragma once
#include "wal_storage.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
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
  struct FileHandle {
    HANDLE h;
    FileHandle(const std::string &p) {
      h = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                      NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to open WAL file: " + p);
    }
    ~FileHandle() {
      if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    }
  };

  std::string path_;
  FileHandle file_; // Destroyed LAST (after wal_)
  std::unique_ptr<libconveyor::v2::Conveyor>
      wal_; // Destroyed FIRST (flushes to file_)

  std::mutex mx_;
  std::vector<uint8_t> scratch_;

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
  explicit WriteAheadLog(std::string path)
      : path_(std::move(path)), file_(path_) {
    // Conveyor is initialized in recover() to avoid contention with read loop
  }

  void append(WalOp op, std::string_view key, std::string_view payload) {
    std::lock_guard lock(mx_);
    uint32_t crc = compute_crc((uint8_t)op, key, payload);

    LogHeader h{crc, (uint8_t)op, (uint16_t)key.size(),
                (uint32_t)payload.size()};
    size_t total_len = sizeof(h) + key.size() + payload.size();

    if (scratch_.capacity() < total_len)
      scratch_.reserve(total_len * 2);
    scratch_.clear();

    const uint8_t *p = (const uint8_t *)&h;
    scratch_.insert(scratch_.end(), p, p + sizeof(h));
    scratch_.insert(scratch_.end(), key.begin(), key.end());
    scratch_.insert(scratch_.end(), payload.begin(), payload.end());

    auto res = wal_->write(scratch_);
    if (!res)
      std::cerr << "WAL Write Error: " << res.error().message() << "\n";
  }

  using RecoverCallback =
      std::function<void(WalOp, std::string_view, std::string_view)>;
  void recover(RecoverCallback callback) {
    // Reset position to 0 for recovery read
    off_t offset = 0;

    while (true) {
      LogHeader h;

      // Direct Read bypassing Conveyor to avoid EOF hang
      ssize_t bytes =
          wal::WindowsStorage::pread_impl(file_.h, &h, sizeof(h), offset);
      if (bytes <= 0)
        break; // EOF or Error

      if (bytes < sizeof(h)) {
        std::cerr << "WAL Recovery: Truncated header\n";
        break;
      }

      offset += sizeof(h);

      std::string key(h.key_len, '\0');
      std::string payload(h.payload_len, '\0');

      if (h.key_len > 0) {
        bytes = wal::WindowsStorage::pread_impl(file_.h, key.data(), h.key_len,
                                                offset);
        if (bytes != h.key_len) {
          std::cerr << "WAL Recovery: Truncated key\n";
          break;
        }
        offset += h.key_len;
      }
      if (h.payload_len > 0) {
        bytes = wal::WindowsStorage::pread_impl(file_.h, payload.data(),
                                                h.payload_len, offset);
        if (bytes != h.payload_len) {
          std::cerr << "WAL Recovery: Truncated payload\n";
          break;
        }
        offset += h.payload_len;
      }

      uint32_t computed = compute_crc(h.op, key, payload);
      if (computed != h.crc) {
        if (h.crc == 0 && computed != 0) {
          std::cerr << "WAL WARNING: Zero CRC allowed for legacy.\n";
        } else {
          std::cerr << "WAL ERROR: CRC Mismatch at offset "
                    << offset - sizeof(h) - h.key_len - h.payload_len
                    << ". Transaction may be partial or corrupt.\n";
          break;
        }
      }
      callback((WalOp)h.op, key, payload);
    }

    // Initialize Conveyor now that we are done reading
    libconveyor::v2::Config cfg;
    cfg.handle = (storage_handle_t)file_.h;
    cfg.ops = wal::WindowsStorage::get_ops();
    cfg.write_capacity = 20 * 1024 * 1024; // 20MB
    cfg.read_capacity = 5 * 1024 * 1024;

    auto create_res = libconveyor::v2::Conveyor::create(cfg);
    if (!create_res)
      throw std::system_error(create_res.error());
    wal_ = std::make_unique<libconveyor::v2::Conveyor>(
        std::move(create_res.value()));

    // Seek to end to prepare for appending
    auto seek_res = wal_->seek(0, SEEK_END);
    if (!seek_res) {
      std::cerr << "WAL: Seeding failure: " << seek_res.error().message()
                << "\n";
    }
  }

  void flush() {
    std::lock_guard lock(mx_);
    if (wal_) {
      auto res = wal_->flush();
      if (!res) {
        std::cerr << "WAL Flush Error: " << res.error().message() << "\n";
      }
    }
  }

  auto stats() {
    if (!wal_)
      return libconveyor::v2::Conveyor::Stats{};
    return wal_->stats();
  }
};