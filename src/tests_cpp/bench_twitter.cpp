#include <chrono>
#include <iostream>
#include <lite3/client.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>


using json = nlohmann::json;

void run_rmw_benchmark(lite3::Client &client, int iterations) {
  std::string key = "twitter_profile_rmw";

  // Initial setup
  json initial = {{"id", 12345},
                  {"handle", "antigravity_ai"},
                  {"followers", 1000},
                  {"following", 500},
                  {"tweets", 42}};
  client.put(key, initial.dump());

  auto start = std::chrono::high_resolution_clock::now();
  auto last_log = start;
  int log_interval = 1000;

  std::cout << "RMW Benchmark Started (" << iterations << " iterations)...\n";

  for (int i = 0; i < iterations; ++i) {
    // 1. GET
    auto res = client.get(key);
    if (!res) {
      std::cerr << "GET failed: " << res.error().message << "\n";
      continue;
    }

    // 2. Parse & Modify
    try {
      json j = json::parse(res.value());
      j["followers"] = j["followers"].get<int>() + 1;

      // 3. PUT
      client.put(key, j.dump());
    } catch (const std::exception &e) {
      std::cerr << "JSON Error: " << e.what() << "\n";
    }

    if ((i + 1) % log_interval == 0) {
      auto now = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> diff = now - last_log;
      std::cout << "RMW Iter " << (i + 1) << ": " << log_interval << " ops in "
                << diff.count() << "s (" << log_interval / diff.count()
                << " ops/sec)\n";
      last_log = now;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> diff = end - start;

  std::cout << "RMW Benchmark: " << iterations << " iterations in "
            << diff.count() << "s (" << iterations / diff.count()
            << " ops/sec)\n";
}

void run_zeroparse_benchmark(lite3::Client &client, int iterations) {
  std::string key = "twitter_profile_zp";

  // Initial setup
  json initial = {{"id", 12345},
                  {"handle", "antigravity_ai"},
                  {"followers", 1000},
                  {"following", 500},
                  {"tweets", 42}};
  client.put(key, initial.dump());

  auto start = std::chrono::high_resolution_clock::now();
  auto last_log = start;
  int log_interval = 1000;

  std::cout << "ZP Benchmark Started (" << iterations << " iterations)...\n";

  for (int i = 0; i < iterations; ++i) {
    // Zero-Parse Mutation
    auto res = client.patch_int(key, "followers", 1001 + i);
    if (!res) {
      std::cerr << "PATCH failed: " << res.error().message << "\n";
    }

    if ((i + 1) % log_interval == 0) {
      auto now = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> diff = now - last_log;
      std::cout << "ZP Iter " << (i + 1) << ": " << log_interval << " ops in "
                << diff.count() << "s (" << log_interval / diff.count()
                << " ops/sec)\n";
      last_log = now;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> diff = end - start;

  std::cout << "Zero-Parse Benchmark: " << iterations << " iterations in "
            << diff.count() << "s (" << iterations / diff.count()
            << " ops/sec)\n";
}

int main(int argc, char **argv) {
  int iterations = 10000;
  if (argc > 1) {
    iterations = std::stoi(argv[1]);
  }

  try {
    lite3::Client client("localhost", 8080);

    run_rmw_benchmark(client, iterations);
    run_zeroparse_benchmark(client, iterations);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
