# sinnet

`sinnet` is a C++23 library for Linux focused on low-latency, single-threaded
network I/O using `epoll`.

The project currently provides:

- `EventLoop` for fd registration, event dispatch, and token-based stale-event protection
- connection layer with `Connection` base class
- `TCPConnection` and `UDPConnection` implementations
- handler interfaces (`EventLoopHandler`, `ConnectionHandler`) for callback-driven logic

## Key Characteristics

- Linux-only runtime (uses `epoll`, socket options, and Linux batching syscalls)
- non-blocking sockets by default (`SOCK_NONBLOCK | SOCK_CLOEXEC`)
- buffered async send pipeline
  - TCP flush via batched `sendmsg` + `iovec`
  - UDP flush via `sendmmsg`
- UDP read batching via `recvmmsg`
- low-latency socket tuning at socket setup
  - `SO_SNDBUF` / `SO_RCVBUF` set to 512 KB
  - `TCP_NODELAY` for TCP
- explicit loop lifecycle
  - `run()` blocks in `epoll_wait`
  - `stop()` wakes the loop immediately via internal `eventfd`

## Build

```bash
cmake -S . -B build
cmake --build build
```

This builds:

- library target: `sinnet`
- example binary: `example` (target `sinnet_example`)

## Run Example

```bash
./build/example
```

The example resolves `example.com` with `getaddrinfo`, connects using the
resolved endpoint, and prints the HTTP status line.

## Quick Usage

### 1) Implement a handler

Create a handler derived from `sinnet::connection::ConnectionHandler`:

```cpp
class MyHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit MyHandler(sinnet::EventLoop& loop) : loop_(loop) {}

    void onConnected(sinnet::connection::Connection&) override {
        // Connect completed; now writes can be flushed.
    }

    void onConnectError(sinnet::connection::Connection&, int error_code) override {
        // Connection failed (errno-compatible value).
        loop_.stop();
    }

    void onData(sinnet::connection::Connection&, std::span<const std::byte> data) override {
        // Process incoming raw payload bytes.
        // When your stop condition is met:
        loop_.stop();
    }

private:
    sinnet::EventLoop& loop_;
};
```

### 2) Create loop and connection

```cpp
sinnet::EventLoop loop;
MyHandler handler(loop);
sinnet::connection::TCPConnection connection(loop, handler);
sinnet::connection::Connection::Endpoint endpoint = /* result from your resolver */;
connection.connect(endpoint);
```

### 3) Queue data and run loop

```cpp
connection.send(std::as_bytes(std::span(payload)), 0);
loop.run();
```

`send(...)` enqueues data; actual socket writes are performed asynchronously on
`EPOLLOUT`.

## Connect Model

- `connect(endpoint)` is asynchronous and non-blocking.
- The library accepts one pre-resolved endpoint (`sockaddr_storage` + length).
- Hostname resolution is intentionally delegated to the library user.
- `ConnectionHandler::onConnected(...)` is called on successful connect.
- `ConnectionHandler::onConnectError(...)` is called on connect failure.

## TCP vs UDP Behavior

- `TCPConnection`
  - stream semantics
  - queued bytes flushed in batches with vectored `sendmsg`
  - read path consumes available stream data and dispatches payload chunks

- `UDPConnection`
  - datagram semantics
  - queued packets flushed in batches with `sendmmsg`
  - receive path pulls multiple datagrams per syscall via `recvmmsg`

## Tests

This project uses GoogleTest for unit tests.

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake first tries `find_package(GTest CONFIG QUIET)`. If unavailable, GoogleTest
can be fetched automatically with `SINNET_FETCH_GTEST=ON` (default).

## Project Structure

- `include/sinnet/eventloop/` - event loop public headers
- `include/sinnet/connection/` - connection API and handlers
- `src/` - implementation
- `examples/` - runnable example
- `tests/` - unit tests

## License

This project is licensed under the MIT License. See `LICENSE`.

GoogleTest is a test-only dependency under BSD 3-Clause. See
`THIRD_PARTY_NOTICES.md`.
