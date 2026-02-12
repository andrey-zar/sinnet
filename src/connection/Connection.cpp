#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/eventloop/EventLoop.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <system_error>
#include <memory>
#include <unistd.h>

namespace sinnet::connection {

namespace {

int createNonBlockingSocket(int domain, int type, int protocol) {
    const int fd = ::socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    return fd;
}

void setSocketOptionInt(int fd,
                        int level,
                        int option_name,
                        int value,
                        const char* option_label,
                        bool required) {
    if (::setsockopt(fd, level, option_name, &value, sizeof(value)) == 0) {
        return;
    }

    if (required) {
        throw std::system_error(errno, std::generic_category(), option_label);
    }
}

void applyLowLatencySocketOptions(int fd, int socktype, int protocol) {
    static constexpr int kSocketBufferBytes = 512 * 1024;
    setSocketOptionInt(fd,
                       SOL_SOCKET,
                       SO_SNDBUF,
                       kSocketBufferBytes,
                       "setsockopt(SO_SNDBUF)",
                       true);
    setSocketOptionInt(fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       kSocketBufferBytes,
                       "setsockopt(SO_RCVBUF)",
                       true);

    // Reduce small-packet coalescing delay for stream sockets.
    if (socktype == SOCK_STREAM || protocol == IPPROTO_TCP) {
        setSocketOptionInt(fd, IPPROTO_TCP, TCP_NODELAY, 1, "setsockopt(TCP_NODELAY)", true);
    }

    // Ask the IP stack to prefer low-delay routing/queuing.
    setSocketOptionInt(fd, IPPROTO_IP, IP_TOS, IPTOS_LOWDELAY, "setsockopt(IP_TOS)", false);
}

void waitForConnectCompletion(int fd) {
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    const int poll_result = ::poll(&pfd, 1, 2'000);
    if (poll_result == 0) {
        throw std::runtime_error("connect timeout");
    }
    if (poll_result < 0) {
        throw std::system_error(errno, std::generic_category(), "poll");
    }

    int socket_error = 0;
    socklen_t option_len = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &option_len) != 0) {
        throw std::system_error(errno, std::generic_category(), "getsockopt");
    }
    if (socket_error != 0) {
        throw std::system_error(socket_error, std::generic_category(), "connect");
    }
}

using AddrInfoPtr = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>;

}  // namespace

void ConnectionHandler::onClosed(Connection&) {}

Connection::Connection(sinnet::EventLoop& loop,
                       ConnectionHandler& handler,
                       int domain,
                       int type,
                       int protocol)
    : loop_(loop),
      handler_(handler),
      fd_(createNonBlockingSocket(domain, type, protocol)) {
    applyLowLatencySocketOptions(fd_, type, protocol);
}

Connection::~Connection() {
    close();
}

bool Connection::isOpen() const noexcept {
    return fd_ >= 0;
}

void Connection::close() noexcept {
    if (fd_ < 0) {
        return;
    }

    if (is_registered_) {
        try {
            loop_.unregisterFd(fd_);
        } catch (...) {
            // close() is noexcept; swallow unregister failures during shutdown path.
        }
        is_registered_ = false;
        registered_events_ = 0;
        fd_ = -1;
        resetSendState();
        return;
    }

    ::close(fd_);
    fd_ = -1;
    resetSendState();
}

void Connection::onEvent(uint32_t event_mask) {
    if ((event_mask & (EPOLLERR | EPOLLHUP)) != 0U) {
        close();
        handler_.onClosed(*this);
        return;
    }

    if ((event_mask & EPOLLOUT) != 0U) {
        flushSendBuffer();
    }

    if ((event_mask & EPOLLIN) == 0U || !isOpen()) {
        return;
    }

    handleReadableEvent();
}

void Connection::connectToRemote(std::string_view host,
                                 std::string_view port,
                                 int family,
                                 int socktype,
                                 int protocol,
                                 bool wait_for_nonblocking_connect) {
    if (!isOpen()) {
        throw std::logic_error("connection is not open");
    }

    struct addrinfo hints {};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;

    std::string host_str(host);
    std::string port_str(port);

    struct addrinfo* result_raw = nullptr;
    const int gai_result = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result_raw);
    if (gai_result != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + ::gai_strerror(gai_result));
    }
    AddrInfoPtr result(result_raw, ::freeaddrinfo);

    bool connected = false;
    for (addrinfo* current = result.get(); current != nullptr; current = current->ai_next) {
        if (::connect(fd_, current->ai_addr, current->ai_addrlen) == 0) {
            connected = true;
            break;
        }

        if (errno == EINPROGRESS && wait_for_nonblocking_connect) {
            waitForConnectCompletion(fd_);
            connected = true;
            break;
        }
    }

    if (!connected) {
        throw std::runtime_error("unable to connect using provided host and port");
    }

    registerIfNeeded();
}

ssize_t Connection::enqueueSendData(std::span<const std::byte> data,
                                    int flags,
                                    const char* closed_error_message,
                                    bool reserve_large_first_chunk) {
    if (!isOpen()) {
        throw std::logic_error(closed_error_message);
    }
    if (flags != 0) {
        throw std::invalid_argument("buffered send supports flags == 0 only");
    }
    if (data.empty()) {
        return 0;
    }
    if (pending_send_bytes_ + data.size() > kHardSendBufferBytes) {
        throw std::length_error("send buffer hard cap exceeded");
    }

    PendingChunk chunk;
    if (reserve_large_first_chunk && send_queue_.empty()) {
        chunk.data.reserve((data.size() > kInitialSendBufferBytes) ? data.size() : kInitialSendBufferBytes);
    } else {
        chunk.data.reserve(data.size());
    }
    chunk.data.resize(data.size());
    std::memcpy(chunk.data.data(), data.data(), data.size());
    send_queue_.push_back(std::move(chunk));
    pending_send_bytes_ += data.size();

    updateRegistrationEvents();
    return static_cast<ssize_t>(data.size());
}

int Connection::socketFd() const noexcept {
    return fd_;
}

ConnectionHandler& Connection::handler() noexcept {
    return handler_;
}

const ConnectionHandler& Connection::handler() const noexcept {
    return handler_;
}

void Connection::registerIfNeeded() {
    if (is_registered_ || !isOpen()) {
        return;
    }

    registered_events_ = EPOLLIN | EPOLLERR | EPOLLHUP;
    loop_.registerFd(fd_, this, registered_events_);
    is_registered_ = true;
}

void Connection::resetSendState() noexcept {
    send_queue_.clear();
    pending_send_bytes_ = 0;
}

void Connection::updateRegistrationEvents() {
    if (!is_registered_ || !isOpen()) {
        return;
    }

    uint32_t events = EPOLLIN | EPOLLERR | EPOLLHUP;
    if (pending_send_bytes_ > 0) {
        events |= EPOLLOUT;
    }

    if (events == registered_events_) {
        return;
    }

    loop_.modifyFdEvents(fd_, events);
    registered_events_ = events;
}

}  // namespace sinnet::connection
