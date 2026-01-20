#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

// Fix conflict with Boost.Beast
#ifdef off_t
#undef off_t
#endif

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <string>

class Engine;

namespace http_server {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class http_server {
public:
  http_server(Engine &db, std::string address, unsigned short port,
              int threads = 1);
  void run();
  void stop();

private:
  void do_accept();
  void on_accept(beast::error_code ec, tcp::socket socket);

  std::string address_;
  unsigned short port_;
  net::io_context ioc_;
  net::signal_set signals_;
  tcp::acceptor acceptor_;
  Engine &db_;
  int threads_;
};

} // namespace http_server
