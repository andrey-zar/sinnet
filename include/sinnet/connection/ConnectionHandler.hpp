#pragma once

#include <cstddef>
#include <span>

namespace sinnet::connection {

class Connection;

// Application-facing callback interface for connection events.
//
// Contract:
// - `onData(...)` is required and receives raw protocol payload bytes.
// - `onClosed(...)` is optional and called when the connection is closed.
class ConnectionHandler {
public:
    ConnectionHandler() = default;
    virtual ~ConnectionHandler() = default;

    // Called when asynchronous connect completes successfully.
    virtual void onConnected(Connection& connection) noexcept;

    // Called when asynchronous connect fails. Uses errno-compatible error code.
    virtual void onConnectError(Connection& connection, int error_code) noexcept;

    // Called when new payload bytes are available.
    virtual void onData(Connection& connection, std::span<const std::byte> data) noexcept = 0;

    // Called when the connection transitions to closed state.
    virtual void onClosed(Connection& connection) noexcept;
};

}  // namespace sinnet::connection
