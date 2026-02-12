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

// UDP datagram connection with mmsg-based batch send/receive paths.
class UDPConnection : public Connection {
public:
    UDPConnection(sinnet::EventLoop& loop, ConnectionHandler& handler);

    // Establishes connected UDP socket (peer address is fixed after connect).
    void connect(std::string_view host, std::string_view port) override;

    // Enqueues one datagram for async flush on EPOLLOUT.
    ssize_t send(std::span<const std::byte> data, int flags = 0);

protected:
    // Drains queued datagrams in batched sendmmsg syscalls.
    void flushSendBuffer() override;

    // Reads datagrams in batches via recvmmsg and dispatches to handler.
    void handleReadableEvent() override;

private:
    static constexpr size_t kMaxBatchMessages = 32;
    static constexpr size_t kReceiveBufferBytes = 2048;
};

}  // namespace sinnet::connection
