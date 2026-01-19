#pragma once
#include "libconveyor/conveyor_modern.hpp"
#include "wal_storage.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
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
    // Initialize temporary Reader Conveyor for buffered recovery
    libconveyor::v2::Config read_cfg;
    read_cfg.handle = (storage_handle_t)file_.h;
    read_cfg.ops = wal::WindowsStorage::get_ops();
    read_cfg.write_capacity =
        64 * 1024; // Small write buffer, we are only reading
    read_cfg.read_capacity = 10 * 1024 * 1024; // 10MB Read Buffer for speed

    auto read_create_res = libconveyor::v2::Conveyor::create(read_cfg);
    if (!read_create_res) {
      std::cerr << "WAL Recovery: Failed to create reader conveyor. Falling "
                   "back to unbuffered.\n";
    }

    auto &reader = read_create_res.value();
    off_t offset = 0;

    struct Buffered {
      libconveyor::v2::Conveyor &r;
      off_t &off;
      std::vector<uint8_t> &buf;

      bool read(void *dest, size_t len, const char *ctx) {
        buf.resize(len);
        auto res = r.read(buf);
        if (!res) {
          std::cerr << "WAL Recovery [" << ctx
                    << "]: Read error: " << res.error().message() << "\n";
          return false;
        }
        if (res.value() != len) {
          if (res.value() > 0) {
            std::cerr << "WAL Recovery [" << ctx << "]: Partial read ("
                      << res.value() << "/" << len << ")\n";
          }
          return false;
        }
        std::memcpy(dest, buf.data(), len);
        off += (off_t)len;
        return true;
      }
    };

    std::vector<uint8_t> read_buf;
    Buffered b{reader, offset, read_buf};

    while (true) {
      LogHeader h;
      if (!b.read(&h, sizeof(h), "HEADER"))
        break;

      std::string key(h.key_len, '\0');
      std::string payload(h.payload_len, '\0');

      if (h.key_len > 0) {
        if (!b.read(key.data(), h.key_len, "KEY")) {
          std::cerr << "WAL Recovery: Truncated key at offset " << offset
                    << "\n";
          break;
        }
      }

      if (h.payload_len > 0) {
        if (!b.read(payload.data(), h.payload_len, "PAYLOAD")) {
          std::cerr << "WAL Recovery: Truncated payload at offset " << offset
                    << "\n";
          break;
        }
      }

      uint32_t computed = compute_crc(h.op, key, payload);
      if (computed != h.crc) {
        if (h.crc == 0 && computed != 0) {
          std::cerr << "WAL WARNING: Zero CRC allowed for legacy.\n";
        } else {
          std::cerr << "WAL ERROR: CRC Mismatch at offset "
                    << offset - (off_t)sizeof(h) - (off_t)h.key_len -
                           (off_t)h.payload_len
                    << ". Transaction may be partial or corrupt.\n";
          break;
        }
      }
      callback((WalOp)h.op, key, payload);
    }
    std::cerr << "WAL Recovery: Completed at offset " << offset << "\n";

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