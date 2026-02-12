#include "sinnet/eventloop/EventLoop.hpp"
#include "sinnet/eventloop/EventLoopHandler.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class StopLoopException : public std::exception {
public:
    const char* what() const noexcept override {
        return "stop event loop";
    }
};

class HttpHeadHandler : public sinnet::EventLoopHandler {
public:
    HttpHeadHandler(sinnet::EventLoop& loop, int fd)
        : loop_(loop), fd_(fd) {}

    void onEvent(uint32_t event_mask) override {
        if ((event_mask & (EPOLLERR | EPOLLHUP)) != 0U) {
            failWithSocketError("epoll reported error/hangup");
        }

        if (!connected_ && (event_mask & EPOLLOUT) != 0U) {
            completeConnection();
        }

        if (connected_ && !request_sent_ && (event_mask & EPOLLOUT) != 0U) {
            sendRequest();
        }

        if (request_sent_ && (event_mask & EPOLLIN) != 0U) {
            readResponse();
        }
    }

private:
    void completeConnection() {
        int so_error = 0;
        socklen_t option_len = sizeof(so_error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &option_len) != 0) {
            throw std::runtime_error(std::string("getsockopt(SO_ERROR) failed: ") + std::strerror(errno));
        }
        if (so_error != 0) {
            throw std::runtime_error(std::string("connect failed: ") + std::strerror(so_error));
        }
        connected_ = true;
    }

    void sendRequest() {
        while (sent_bytes_ < kRequest.size()) {
            const size_t remaining = kRequest.size() - sent_bytes_;
            const ssize_t written =
                send(fd_, kRequest.data() + sent_bytes_, remaining, MSG_NOSIGNAL);
            if (written > 0) {
                sent_bytes_ += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            failWithSocketError("send failed");
        }
        request_sent_ = true;
    }

    void readResponse() {
        char buffer[1024];
        for (;;) {
            const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
            if (n > 0) {
                response_.append(buffer, static_cast<size_t>(n));
                continue;
            }

            if (n == 0) {
                printStatusLine();
                loop_.unregisterFd(fd_);
                throw StopLoopException();
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            failWithSocketError("recv failed");
        }
    }

    void printStatusLine() const {
        const size_t line_end = response_.find("\r\n");
        if (line_end == std::string::npos) {
            std::cout << "Response received, but no HTTP status line found.\n";
            return;
        }
        std::cout << "HTTP status: " << response_.substr(0, line_end) << '\n';
    }

    [[noreturn]] void failWithSocketError(const char* message) {
        loop_.unregisterFd(fd_);
        throw std::runtime_error(std::string(message) + ": " + std::strerror(errno));
    }

    sinnet::EventLoop& loop_;
    int fd_ = -1;
    bool connected_ = false;
    bool request_sent_ = false;
    size_t sent_bytes_ = 0;
    std::string response_;

    static constexpr std::string_view kRequest =
        "HEAD / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
};

int createAndConnectSocket(const char* host, const char* port) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    const int gai_result = getaddrinfo(host, port, &hints, &result);
    if (gai_result != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(gai_result));
    }

    int fd = -1;
    for (struct addrinfo* current = result; current != nullptr; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype | SOCK_NONBLOCK, current->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
            freeaddrinfo(result);
            return fd;
        }
        if (errno == EINPROGRESS) {
            freeaddrinfo(result);
            return fd;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    throw std::runtime_error(std::string("unable to connect to ") + host + ":" + port);
}

}  // namespace

int main() {
    try {
        sinnet::EventLoop event_loop;
        const int socket_fd = createAndConnectSocket("example.com", "80");
        HttpHeadHandler handler(event_loop, socket_fd);

        event_loop.registerFd(socket_fd, &handler, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
        event_loop.run();
    } catch (const StopLoopException&) {
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Example failed: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
