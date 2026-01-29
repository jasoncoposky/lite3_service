#include "../engine/wal.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace l3kv;

void test_simple_append_recover() {
  std::string path = "test_simple.wal";
  std::filesystem::remove(path);

  {
    WriteAheadLog wal(path);
    wal.recover([](WalOp, std::string_view, std::string_view) {});
    wal.append(WalOp::PUT, "key1", "val1");
    wal.append(WalOp::DELETE_, "key2", "");
    wal.flush();
  }

  {
    WriteAheadLog wal(path);
    std::vector<std::string> ops;
    wal.recover([&](WalOp op, std::string_view key, std::string_view val) {
      ops.push_back(std::string(key) + ":" + std::string(val));
    });

    assert(ops.size() == 2);
    assert(ops[0] == "key1:val1");
    assert(ops[1] == "key2:");
    std::cout << "[PASS] Simple Append/Recover" << std::endl;
  }
  std::filesystem::remove(path);
}

void test_batch_append_recover() {
  std::cout << "TEST: batch_append_recover start" << std::endl;
  std::string path = "test_batch.wal";
  std::filesystem::remove(path);

  {
    WriteAheadLog wal(path);
    wal.recover([](WalOp, std::string_view, std::string_view) {});

    std::vector<BatchOp> batch;
    batch.push_back({WalOp::PUT, "bkey1", "bval1"});
    batch.push_back({WalOp::PUT, "bkey2", "bval2"});

    wal.append_batch(batch);
    wal.flush();
  }

  {
    WriteAheadLog wal(path);
    std::vector<std::string> ops;
    wal.recover([&](WalOp op, std::string_view key, std::string_view val) {
      ops.push_back(std::string(key) + ":" + std::string(val));
    });

    assert(ops.size() == 2);
    assert(ops[0] == "bkey1:bval1");
    assert(ops[1] == "bkey2:bval2");
    std::cout << "[PASS] Batch Append/Recover" << std::endl;
  }
  std::filesystem::remove(path);
}

int main() {
  std::cout << "DEBUG: Starting test_wal..." << std::endl;
  try {
    test_simple_append_recover();
    // batch test will fail to compile until we add the method
    test_batch_append_recover();
    std::cout << "All WAL Tests Passed!" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Test Failed: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
