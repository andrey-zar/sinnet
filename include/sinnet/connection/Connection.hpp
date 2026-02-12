#pragma once

#include "sinnet/eventloop/EventLoopHandler.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

namespace sinnet::connection {

class ConnectionHandler;
}

namespace sinnet {
class EventLoop;
}

namespace sinnet::connection {

// Base class for event-loop-driven socket connections.
//
// Responsibilities:
// - Own and manage a non-blocking socket lifecycle.
// - Integrate with EventLoop via EventLoopHandler callbacks.
// - Provide protocol-agnostic connect helpers and event dispatch flow.
//
// Extension points for derived classes:
// - `connect(...)` for protocol-specific connect behavior.
// - `flushSendBuffer()` for protocol-specific send batching path.
// - `handleReadableEvent()` for protocol-specific receive path.
class Connection : public sinnet::EventLoopHandler {
public:
    struct Endpoint {
        sockaddr_storage address {};
        socklen_t address_length = 0;
    };

    enum class State : uint8_t {
        Idle,
        Connecting,
        Connected,
        Failed,
        Closed,
    };

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) = delete;
    Connection& operator=(Connection&& other) = delete;
    virtual ~Connection();

    virtual void connect(const Endpoint& endpoint) = 0;

    // True when socket file descriptor is valid and owned by this object.
    bool isOpen() const noexcept;
    bool isConnected() const noexcept;
    State state() const noexcept;

    // Closes the socket and unregisters from EventLoop when needed.
    void close() noexcept;

    // EventLoop callback entry point. Dispatches to write/read handlers.
    void onEvent(uint32_t event_mask) noexcept override;

protected:
    // Stores references and socket parameters; socket is created on connect().
    Connection(sinnet::EventLoop& loop,
               ConnectionHandler& handler,
               int type,
               int protocol);

    // Shared async connect helper used by protocol-specific derived connections.
    void connectToRemote(const Endpoint& endpoint);
    ssize_t enqueueSendData(std::span<const std::byte> data,
                            int flags,
                            const char* closed_error_message,
                            bool reserve_large_first_chunk);

    int socketFd() const noexcept;
    ConnectionHandler& handler() noexcept;
    const ConnectionHandler& handler() const noexcept;

    // Overridable I/O hooks for protocol-specific behavior.
    virtual void handleReadableEvent() noexcept = 0;
    virtual void flushSendBuffer() noexcept = 0;

    // Generic pending-send chunk used by connection implementations.
    struct PendingChunk {
        std::vector<char> data;
        size_t offset = 0;
    };

    // Recomputes epoll interest set according to connection state/queues.
    void updateRegistrationEvents();

    // Shared queue/state for protocol send pipelines.
    std::deque<PendingChunk> send_queue_;
    size_t pending_send_bytes_ = 0;

    // Buffer policy defaults used by derived send implementations.
    static constexpr size_t kInitialSendBufferBytes = 512 * 1024;
    static constexpr size_t kHardSendBufferBytes = 8 * 1024 * 1024;

private:
    void registerIfNeeded();
    void resetSendState() noexcept;
    void ensureSocketForFamily(int family);
    void handleConnectEvent() noexcept;
    void completeConnect() noexcept;
    void failConnect(int error_code) noexcept;

    // Core ownership and registration state.
    sinnet::EventLoop& loop_;
    ConnectionHandler& handler_;
    int socket_type_ = 0;
    int socket_protocol_ = 0;
    int fd_ = -1;
    bool is_registered_ = false;
    uint32_t registered_events_ = 0;
    State state_ = State::Idle;

};

}  // namespace sinnet::connection
