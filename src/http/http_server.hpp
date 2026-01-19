#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <string>

namespace http_server {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class http_server {
public:
    http_server(std::string address, unsigned short port);
    void run();

private:
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);

    std::string address_;
    unsigned short port_;
    net::io_context ioc_;
    tcp::acceptor acceptor_;
};

} // namespace http_server
