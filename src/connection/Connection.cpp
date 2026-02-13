#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/eventloop/EventLoop.hpp"

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <system_error>
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

}  // namespace

void ConnectionHandler::onConnected(Connection&) noexcept {}
void ConnectionHandler::onConnectError(Connection&, int) noexcept {}
void ConnectionHandler::onClosed(Connection&) noexcept {}

const char* Connection::PendingChunk::data() const noexcept {
    return heap_data ? heap_data.get() : inline_data.data();
}

char* Connection::PendingChunk::data() noexcept {
    return heap_data ? heap_data.get() : inline_data.data();
}

Connection::Connection(sinnet::EventLoop& loop,
                       ConnectionHandler& handler,
                       int type,
                       int protocol)
    : loop_(loop),
      handler_(handler),
      socket_type_(type),
      socket_protocol_(protocol) {
    free_heap_buffers_.reserve(kMaxReusableHeapBuffers);
}

Connection::~Connection() {
    close();
}

bool Connection::isOpen() const noexcept {
    return fd_ >= 0;
}

bool Connection::isConnected() const noexcept {
    return state_ == State::Connected;
}

Connection::State Connection::state() const noexcept {
    return state_;
}

void Connection::close() noexcept {
    if (fd_ < 0) {
        if (state_ == State::Connecting || state_ == State::Connected || state_ == State::Idle) {
            state_ = State::Closed;
        }
        return;
    }

    if (registration_.has_value()) {
        try {
            registration_->reset();
        } catch (...) {
            // close() is noexcept; swallow unregister failures during shutdown path.
        }
        registration_.reset();
        registered_events_ = 0;
        fd_ = -1;
        resetSendState();
        if (state_ != State::Failed) {
            state_ = State::Closed;
        }
        return;
    }

    ::close(fd_);
    fd_ = -1;
    resetSendState();
    if (state_ != State::Failed) {
        state_ = State::Closed;
    }
}

void Connection::onEvent(uint32_t event_mask) noexcept {
    if (state_ == State::Connecting) {
        if ((event_mask & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
            handleConnectEvent();
        }
        return;
    }

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

void Connection::connectToRemote(const Endpoint& endpoint) {
    if (state_ == State::Connecting) {
        throw std::logic_error("connection is already connecting");
    }
    if (state_ == State::Connected) {
        throw std::logic_error("connection is already connected");
    }
    if (endpoint.address_length == 0) {
        throw std::invalid_argument("endpoint address_length must be non-zero");
    }

    const int family = endpoint.address.ss_family;
    if (family != AF_INET && family != AF_INET6) {
        throw std::invalid_argument("endpoint family must be AF_INET or AF_INET6");
    }

    const socklen_t min_length =
        static_cast<socklen_t>(family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
    if (endpoint.address_length < min_length ||
        endpoint.address_length > static_cast<socklen_t>(sizeof(sockaddr_storage))) {
        throw std::invalid_argument("endpoint address_length is invalid for address family");
    }

    ensureSocketForFamily(family);

    const int connect_result = ::connect(fd_,
                                         reinterpret_cast<const sockaddr*>(&endpoint.address),
                                         endpoint.address_length);
    if (connect_result == 0) {
        completeConnect();
        return;
    }

    if (errno == EINPROGRESS) {
        state_ = State::Connecting;
        registerIfNeeded();
        updateRegistrationEvents();
        return;
    }

    failConnect(errno);
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

    (void)reserve_large_first_chunk;
    PendingChunk chunk;
    chunk.size = data.size();
    chunk.offset = 0;
    if (chunk.size > PendingChunk::kInlineBytes) {
        size_t capacity = 0;
        chunk.heap_data = takeReusableHeapBuffer(chunk.size, capacity);
        if (!chunk.heap_data) {
            capacity = chunk.size;
            chunk.heap_data = std::make_unique<char[]>(capacity);
        }
        chunk.heap_capacity = capacity;
    }
    std::memcpy(chunk.data(), data.data(), chunk.size);
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
    if (registration_.has_value() || !isOpen()) {
        return;
    }

    registered_events_ = EPOLLERR | EPOLLHUP;
    registration_.emplace(loop_.registerFdScoped(fd_, this, registered_events_));
}

void Connection::resetSendState() noexcept {
    for (auto& chunk : send_queue_) {
        recycleChunkStorage(chunk);
    }
    send_queue_.clear();
    pending_send_bytes_ = 0;
}

std::unique_ptr<char[]> Connection::takeReusableHeapBuffer(size_t min_capacity,
                                                           size_t& out_capacity) noexcept {
    for (size_t i = 0; i < free_heap_buffers_.size(); ++i) {
        ReusableHeapBuffer& candidate = free_heap_buffers_[i];
        if (candidate.capacity < min_capacity) {
            continue;
        }
        out_capacity = candidate.capacity;
        std::unique_ptr<char[]> buffer = std::move(candidate.data);
        free_heap_buffers_[i] = std::move(free_heap_buffers_.back());
        free_heap_buffers_.pop_back();
        ++reusable_heap_take_hits_;
        return buffer;
    }
    out_capacity = 0;
    return nullptr;
}

void Connection::recycleChunkStorage(PendingChunk& chunk) noexcept {
    if (chunk.heap_data) {
        if (free_heap_buffers_.size() < kMaxReusableHeapBuffers) {
            free_heap_buffers_.push_back(
                ReusableHeapBuffer{std::move(chunk.heap_data), chunk.heap_capacity});
        } else {
            chunk.heap_data.reset();
        }
    }
    chunk.heap_capacity = 0;
    chunk.size = 0;
    chunk.offset = 0;
}

size_t Connection::debugReusableHeapBufferCount() const noexcept {
    return free_heap_buffers_.size();
}

size_t Connection::debugReusableHeapBufferTakeHits() const noexcept {
    return reusable_heap_take_hits_;
}

void Connection::updateRegistrationEvents() {
    if (!registration_.has_value() || !isOpen()) {
        return;
    }

    uint32_t events = EPOLLERR | EPOLLHUP;
    if (state_ == State::Connecting) {
        events |= EPOLLOUT;
    } else if (state_ == State::Connected) {
        events |= EPOLLIN;
        if (pending_send_bytes_ > 0) {
            events |= EPOLLOUT;
        }
    }

    if (events == registered_events_) {
        return;
    }

    loop_.modifyFdEvents(fd_, events);
    registered_events_ = events;
}

void Connection::ensureSocketForFamily(int family) {
    if (fd_ >= 0) {
        return;
    }

    fd_ = createNonBlockingSocket(family, socket_type_, socket_protocol_);
    applyLowLatencySocketOptions(fd_, socket_type_, socket_protocol_);
    if (state_ != State::Connected) {
        state_ = State::Idle;
    }
}

void Connection::handleConnectEvent() noexcept {
    if (!isOpen() || state_ != State::Connecting) {
        return;
    }

    int socket_error = 0;
    socklen_t option_len = sizeof(socket_error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &option_len) != 0) {
        failConnect(errno);
        return;
    }

    if (socket_error != 0) {
        failConnect(socket_error);
        return;
    }

    completeConnect();
}

void Connection::completeConnect() noexcept {
    state_ = State::Connected;
    try {
        registerIfNeeded();
        updateRegistrationEvents();
    } catch (...) {
        failConnect(EIO);
        return;
    }
    handler_.onConnected(*this);
    if (pending_send_bytes_ > 0) {
        flushSendBuffer();
    }
}

void Connection::failConnect(int error_code) noexcept {
    state_ = State::Failed;

    if (fd_ >= 0) {
        if (registration_.has_value()) {
            try {
                registration_->reset();
            } catch (...) {
                ::close(fd_);
            }
            registration_.reset();
        } else {
            ::close(fd_);
        }
        fd_ = -1;
        registered_events_ = 0;
    }
    resetSendState();
    handler_.onConnectError(*this, error_code);
}

}  // namespace sinnet::connection
