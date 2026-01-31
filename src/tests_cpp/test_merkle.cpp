#include "../engine/merkle.hpp"
#include <cassert>
#include <iostream>

using namespace l3kv;

void test_merkle_xor() {
  std::cout << "TEST: Merkle Tree XOR Logic..." << std::endl;
  MerkleTree t;

  // Initial root should be non-zero (hash of empty tree)
  uint64_t h_empty = t.get_root_hash();
  assert(h_empty != 0);

  // Update k1 with 0xAA
  t.apply_delta("k1", 0xAA);
  uint64_t r1 = t.get_root_hash();
  assert(r1 != h_empty);

  // Update k1 with 0xAA again (XOR out)
  t.apply_delta("k1", 0xAA);
  assert(t.get_root_hash() == h_empty);
  std::cout << "[PASS] XOR Cancellation. Empty Hash: " << h_empty << std::endl;

  // Add k1
  t.apply_delta("k1", 0xAA);
  assert(t.get_root_hash() == r1);

  // Add k2 with 0xBB
  t.apply_delta("k2", 0xBB);
  uint64_t r2 = t.get_root_hash();
  assert(r2 != r1);
  assert(r2 != 0);

  std::cout << "[PASS] Multiple Keys impacting Root" << std::endl;
}

int main() {
  test_merkle_xor();
  return 0;
}
