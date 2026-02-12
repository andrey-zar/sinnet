#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/eventloop/EventLoop.hpp"
#include "sinnet/connection/TCPConnection.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <span>
#include <string>
#include <string_view>

namespace {

using AddrInfoPtr = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>;

[[nodiscard]] sinnet::connection::Connection::Endpoint resolveEndpoint(std::string_view host,
                                                                       std::string_view port) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string host_string(host);
    const std::string port_string(port);
    addrinfo* result_raw = nullptr;
    const int rc = ::getaddrinfo(host_string.c_str(), port_string.c_str(), &hints, &result_raw);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + ::gai_strerror(rc));
    }
    AddrInfoPtr result(result_raw, ::freeaddrinfo);
    if (result == nullptr) {
        throw std::runtime_error("getaddrinfo returned no addresses");
    }

    sinnet::connection::Connection::Endpoint endpoint;
    std::memcpy(&endpoint.address, result->ai_addr, static_cast<size_t>(result->ai_addrlen));
    endpoint.address_length = result->ai_addrlen;
    return endpoint;
}

class HttpHeadHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit HttpHeadHandler(sinnet::EventLoop& event_loop) : event_loop_(event_loop) {}

    void onConnected(sinnet::connection::Connection&) override {
        std::cout << "Connected, waiting for response..." << '\n';
    }

    void onData(sinnet::connection::Connection& connection, std::span<const std::byte> data) override {
        response_.append(reinterpret_cast<const char*>(data.data()), data.size());
        printStatusLineIfReady(connection);
    }

private:
    void printStatusLineIfReady(sinnet::connection::Connection& connection) {
        const size_t line_end = response_.find("\r\n");
        if (line_end == std::string::npos) {
            return;
        }

        std::cout << "HTTP status: " << response_.substr(0, line_end) << '\n';
        connection.close();
        event_loop_.stop();
    }
    sinnet::EventLoop& event_loop_;
    std::string response_;
};

}  // namespace

int main() {
    try {
        sinnet::EventLoop event_loop;
        HttpHeadHandler handler(event_loop);
        sinnet::connection::TCPConnection connection(event_loop, handler);
        const auto endpoint = resolveEndpoint("example.com", "80");
        connection.connect(endpoint);
        static constexpr std::string_view kRequest =
            "HEAD / HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Connection: close\r\n"
            "\r\n";
        connection.send(std::as_bytes(std::span(kRequest.data(), kRequest.size())), 0);
        event_loop.run();
    } catch (const std::exception& ex) {
        std::cerr << "Example failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
