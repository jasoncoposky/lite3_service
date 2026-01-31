#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include "engine/store.hpp"
#include "kalman_filter.hpp"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
// Fix conflict with Boost.Beast - Ensure these are UNDEF'd right before beast!
#ifdef off_t
#undef off_t
#endif
#ifdef DELETE
#undef DELETE
#endif

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/config.hpp>
#include <lite3/ring.hpp>
#include <map>
#include <thread>
#include <vector>

namespace l3kv {
class Engine;
}

namespace http_server {

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

// session class is internal to http_server.cpp

class http_server {
public:
  http_server(l3kv::Engine &db, std::string address, unsigned short port,
              int min_threads = 4, int max_threads = 16,
              std::shared_ptr<lite3::ConsistentHash> ring = nullptr,
              uint32_t node_id = 0,
              std::map<uint32_t, std::pair<std::string, int>> peers = {});
  void run();
  void stop();

private:
  void do_accept();
  void on_accept(beast::error_code ec, tcp::socket socket);

  // Dynamic Thread Pool
  void start_manager();
  void manager_loop();
  void adjust_pool_size(int target);

  std::string address_;
  unsigned short port_;
  net::io_context ioc_;
  net::signal_set signals_;
  tcp::acceptor acceptor_;
  l3kv::Engine &db_;

  int min_threads_;
  int max_threads_;
  int n_threads_{0}; // Active thread count (including main)
  std::vector<std::thread> thread_pool_;
  net::steady_timer manager_timer_;

  std::shared_ptr<lite3::ConsistentHash> ring_;
  uint32_t self_node_id_;
  // Map NodeID -> {Host, Port}
  std::map<uint32_t, std::pair<std::string, int>> peers_;

  KalmanFilter kf_;
  std::chrono::steady_clock::time_point last_tick_;
};

} // namespace http_server
