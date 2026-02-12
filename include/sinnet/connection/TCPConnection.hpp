#pragma once

#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace sinnet {
class EventLoop;
}

namespace sinnet::connection {

// TCP stream connection with buffered send and vectored flush path.
class TCPConnection : public Connection {
public:
    TCPConnection(sinnet::EventLoop& loop, ConnectionHandler& handler);

    // Establishes non-blocking TCP connection.
    void connect(std::string_view host, std::string_view port) override;

    // Enqueues outgoing stream bytes for async flush on EPOLLOUT.
    ssize_t send(std::span<const std::byte> data, int flags = 0);

protected:
    // Reads stream bytes until EAGAIN and dispatches payload chunks to handler.
    void handleReadableEvent() override;

    // Drains queued stream bytes in batched sendmsg+iovec syscalls.
    void flushSendBuffer() override;

private:
    static constexpr size_t kMaxIovecBatch = 64;
};

}  // namespace sinnet::connection
