#pragma once

#include "sinnet/connection/Connection.hpp"
#include "sinnet/connection/ConnectionHandler.hpp"

#include <cstddef>
#include <span>

namespace sinnet {
class EventLoop;
}

namespace sinnet::connection {

// UDP datagram connection with mmsg-based batch send/receive paths.
class UDPConnection : public Connection {
public:
    UDPConnection(sinnet::EventLoop& loop, ConnectionHandler& handler);

    // Starts asynchronous UDP connect to a resolved endpoint.
    void connect(const Endpoint& endpoint) override;

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
