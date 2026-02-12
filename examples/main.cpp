#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/eventloop/EventLoop.hpp"
#include "sinnet/connection/TCPConnection.hpp"

#include <cerrno>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

class StopLoopException : public std::exception {
public:
    const char* what() const noexcept override {
        return "stop event loop";
    }
};

class HttpHeadHandler : public sinnet::connection::ConnectionHandler {
public:
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
        throw StopLoopException();
    }
    std::string response_;
};

}  // namespace

int main() {
    try {
        sinnet::EventLoop event_loop;
        HttpHeadHandler handler;
        sinnet::connection::TCPConnection connection(event_loop, handler);
        connection.connect("example.com", "80");
        static constexpr std::string_view kRequest =
            "HEAD / HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Connection: close\r\n"
            "\r\n";
        connection.send(std::as_bytes(std::span(kRequest.data(), kRequest.size())), 0);
        event_loop.run();
    } catch (const StopLoopException&) {
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Example failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
