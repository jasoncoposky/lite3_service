#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

// Fix conflict with Boost.Beast
#ifdef off_t
#undef off_t
#endif

#include "kalman_filter.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>
#include <thread>
#include <vector>

namespace l3kv {
class Engine;
}

namespace http_server {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class http_server {
public:
  http_server(l3kv::Engine &db, std::string address, unsigned short port,
              int min_threads = 4, int max_threads = 16);
  void run();
  void stop();

private:
  void do_accept();
  void on_accept(beast::error_code ec, tcp::socket socket);

  // Dynamic Thread Pool
  void start_manager();
  void manager_loop();
  void adjust_pool_size(int target);

  KalmanFilter kf_;
  std::chrono::steady_clock::time_point last_tick_;

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
};

} // namespace http_server
