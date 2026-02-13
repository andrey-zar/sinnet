#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/connection/TCPConnection.hpp"
#include "sinnet/connection/UDPConnection.hpp"
#include "sinnet/eventloop/EventLoop.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <gtest/gtest.h>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

int createTcpServerSocket(uint16_t* out_port) {
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("socket failed");
    }

    int reuse = 1;
    (void)::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (::bind(server_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(server_fd);
        throw std::runtime_error("bind failed");
    }
    if (::listen(server_fd, 1) != 0) {
        ::close(server_fd);
        throw std::runtime_error("listen failed");
    }

    sockaddr_in bound_addr {};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(server_fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
        ::close(server_fd);
        throw std::runtime_error("getsockname failed");
    }

    *out_port = ntohs(bound_addr.sin_port);
    return server_fd;
}

int createUdpServerSocket(uint16_t* out_port) {
    const int server_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("socket failed");
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (::bind(server_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(server_fd);
        throw std::runtime_error("bind failed");
    }

    sockaddr_in bound_addr {};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(server_fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
        ::close(server_fd);
        throw std::runtime_error("getsockname failed");
    }

    *out_port = ntohs(bound_addr.sin_port);
    return server_fd;
}

uint16_t reserveUnusedTcpPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed");
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("bind failed");
    }

    sockaddr_in bound_addr {};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname failed");
    }

    ::close(fd);
    return ntohs(bound_addr.sin_port);
}

sinnet::connection::Connection::Endpoint makeIpv4Endpoint(uint16_t port) {
    sinnet::connection::Connection::Endpoint endpoint;
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::memcpy(&endpoint.address, &addr, sizeof(addr));
    endpoint.address_length = static_cast<socklen_t>(sizeof(addr));
    return endpoint;
}

class StopOnDataHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit StopOnDataHandler(sinnet::EventLoop* loop,
                               std::string* storage,
                               std::mutex* mutex,
                               std::condition_variable* cv,
                               bool* ready,
                               std::string expected_payload)
        : loop_(loop),
          storage_(storage),
          mutex_(mutex),
          cv_(cv),
          ready_(ready),
          expected_payload_(std::move(expected_payload)) {}

    void onData(sinnet::connection::Connection&, std::span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            storage_->append(reinterpret_cast<const char*>(data.data()), data.size());
            *ready_ = storage_->find(expected_payload_) != std::string::npos;
        }
        cv_->notify_one();
        if (*ready_) {
            loop_->stop();
        }
    }

private:
    sinnet::EventLoop* loop_ = nullptr;
    std::string* storage_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
    bool* ready_ = nullptr;
    std::string expected_payload_;
};

class StopOnByteCountHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit StopOnByteCountHandler(sinnet::EventLoop* loop,
                                    std::string* storage,
                                    std::mutex* mutex,
                                    std::condition_variable* cv,
                                    bool* ready,
                                    size_t expected_bytes)
        : loop_(loop),
          storage_(storage),
          mutex_(mutex),
          cv_(cv),
          ready_(ready),
          expected_bytes_(expected_bytes) {}

    void onData(sinnet::connection::Connection&, std::span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            storage_->append(reinterpret_cast<const char*>(data.data()), data.size());
            *ready_ = storage_->size() >= expected_bytes_;
        }
        cv_->notify_one();
        if (*ready_) {
            loop_->stop();
        }
    }

private:
    sinnet::EventLoop* loop_ = nullptr;
    std::string* storage_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
    bool* ready_ = nullptr;
    size_t expected_bytes_ = 0;
};

class ConnectSignalHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit ConnectSignalHandler(sinnet::EventLoop* loop,
                                  std::mutex* mutex,
                                  std::condition_variable* cv)
        : loop_(loop), mutex_(mutex), cv_(cv) {}

    void onConnected(sinnet::connection::Connection&) noexcept override {
        connected_.store(true, std::memory_order_release);
        cv_->notify_one();
        loop_->stop();
    }

    void onConnectError(sinnet::connection::Connection&, int error_code) noexcept override {
        connect_error_.store(error_code, std::memory_order_release);
        cv_->notify_one();
        loop_->stop();
    }

    void onData(sinnet::connection::Connection&, std::span<const std::byte>) noexcept override {}

    bool connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    int connectError() const noexcept {
        return connect_error_.load(std::memory_order_acquire);
    }

private:
    sinnet::EventLoop* loop_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<int> connect_error_{0};
};

class NoopConnectionHandler : public sinnet::connection::ConnectionHandler {
public:
    void onData(sinnet::connection::Connection&, std::span<const std::byte>) noexcept override {}
};

class InspectableTCPConnection : public sinnet::connection::TCPConnection {
public:
    using sinnet::connection::TCPConnection::TCPConnection;

    size_t reusableHeapBufferCount() const noexcept {
        return debugReusableHeapBufferCount();
    }

    size_t reusableHeapBufferTakeHits() const noexcept {
        return debugReusableHeapBufferTakeHits();
    }
};

class InspectableUDPConnection : public sinnet::connection::UDPConnection {
public:
    using sinnet::connection::UDPConnection::UDPConnection;

    const void* recvMessagesPtr() const noexcept {
        return debugRecvMessagesPtr();
    }

    const void* recvBuffersPtr() const noexcept {
        return debugRecvBuffersPtr();
    }
};

TEST(ConnectionTests, TcpConnectionCanSendAndReceive) {
    sinnet::EventLoop loop;
    std::string received_payload;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    StopOnDataHandler handler(&loop, &received_payload, &mutex, &cv, &ready, "pong");

    uint16_t port = 0;
    const int server_fd = createTcpServerSocket(&port);

    std::thread server_thread([&]() {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        ASSERT_GE(client_fd, 0);

        char in_buf[4] {};
        const ssize_t n = ::recv(client_fd, in_buf, sizeof(in_buf), 0);
        ASSERT_EQ(n, 4);
        ASSERT_EQ(std::string(in_buf, in_buf + 4), "ping");

        const char out_buf[] = "pong";
        ASSERT_EQ(::send(client_fd, out_buf, 4, 0), 4);

        ::close(client_fd);
        ::close(server_fd);
    });

    sinnet::connection::TCPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    const char request[] = "ping";
    ASSERT_EQ(connection.send(std::as_bytes(std::span(request, 4)), 0), 4);

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (...) {
            loop_error = std::current_exception();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return ready; }));
    }

    connection.close();
    loop_thread.join();

    server_thread.join();
    ASSERT_EQ(loop_error, nullptr);
    EXPECT_EQ(received_payload, "pong");
}

TEST(ConnectionTests, UdpConnectionCanSendAndReceive) {
    sinnet::EventLoop loop;
    std::string received_payload;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    StopOnDataHandler handler(&loop, &received_payload, &mutex, &cv, &ready, "pong");

    uint16_t port = 0;
    const int server_fd = createUdpServerSocket(&port);

    std::thread server_thread([&]() {
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);

        char in_buf[4] {};
        const ssize_t n = ::recvfrom(server_fd,
                                     in_buf,
                                     sizeof(in_buf),
                                     0,
                                     reinterpret_cast<sockaddr*>(&peer_addr),
                                     &peer_len);
        ASSERT_EQ(n, 4);
        ASSERT_EQ(std::string(in_buf, in_buf + 4), "ping");

        const char out_buf[] = "pong";
        ASSERT_EQ(::sendto(server_fd,
                           out_buf,
                           4,
                           0,
                           reinterpret_cast<const sockaddr*>(&peer_addr),
                           peer_len),
                  4);

        ::close(server_fd);
    });

    sinnet::connection::UDPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    const char request[] = "ping";
    ASSERT_EQ(connection.send(std::as_bytes(std::span(request, 4)), 0), 4);

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (...) {
            loop_error = std::current_exception();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return ready; }));
    }

    connection.close();
    loop_thread.join();

    server_thread.join();
    ASSERT_EQ(loop_error, nullptr);
    EXPECT_EQ(received_payload, "pong");
}

TEST(ConnectionTests, ConnectionInvokesHandlerOnData) {
    sinnet::EventLoop loop;
    std::string received_data;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    StopOnDataHandler handler(&loop, &received_data, &mutex, &cv, &ready, "data");

    uint16_t port = 0;
    const int server_fd = createTcpServerSocket(&port);

    std::thread server_thread([&]() {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        ASSERT_GE(client_fd, 0);

        const char out_buf[] = "data";
        ASSERT_EQ(::send(client_fd, out_buf, 4, 0), 4);

        ::close(client_fd);
        ::close(server_fd);
    });

    sinnet::connection::TCPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (...) {
            loop_error = std::current_exception();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return ready; }));
    }

    connection.close();
    loop_thread.join();
    server_thread.join();

    ASSERT_EQ(loop_error, nullptr);
    EXPECT_EQ(received_data, "data");
}

TEST(ConnectionTests, UdpConnectionBatchesMultipleSmallMessages) {
    sinnet::EventLoop loop;
    std::string received_data;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;

    static constexpr size_t kMessageCount = 128;
    static constexpr std::string_view kPayload = "ok";
    const size_t expected_bytes = kMessageCount * kPayload.size();
    StopOnByteCountHandler handler(&loop, &received_data, &mutex, &cv, &ready, expected_bytes);

    uint16_t port = 0;
    const int server_fd = createUdpServerSocket(&port);

    std::thread server_thread([&]() {
        sockaddr_in peer_addr {};
        socklen_t peer_len = sizeof(peer_addr);

        for (size_t i = 0; i < kMessageCount; ++i) {
            char in_buf[16] {};
            const ssize_t n = ::recvfrom(server_fd,
                                         in_buf,
                                         sizeof(in_buf),
                                         0,
                                         reinterpret_cast<sockaddr*>(&peer_addr),
                                         &peer_len);
            ASSERT_EQ(n, static_cast<ssize_t>(kPayload.size()));
            ASSERT_EQ(std::string_view(in_buf, static_cast<size_t>(n)), kPayload);
        }

        for (size_t i = 0; i < kMessageCount; ++i) {
            ASSERT_EQ(::sendto(server_fd,
                               kPayload.data(),
                               kPayload.size(),
                               0,
                               reinterpret_cast<const sockaddr*>(&peer_addr),
                               peer_len),
                      static_cast<ssize_t>(kPayload.size()));
        }

        ::close(server_fd);
    });

    sinnet::connection::UDPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    for (size_t i = 0; i < kMessageCount; ++i) {
        const auto payload_bytes = std::span(
            reinterpret_cast<const std::byte*>(kPayload.data()), kPayload.size());
        ASSERT_EQ(connection.send(payload_bytes, 0),
                  static_cast<ssize_t>(kPayload.size()));
    }

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (...) {
            loop_error = std::current_exception();
        }
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return ready; }));
    }

    connection.close();
    loop_thread.join();
    server_thread.join();

    ASSERT_EQ(loop_error, nullptr);
    ASSERT_EQ(received_data.size(), expected_bytes);
}

TEST(ConnectionTests, ConnectSuccessInvokesOnConnected) {
    sinnet::EventLoop loop;
    std::mutex mutex;
    std::condition_variable cv;
    ConnectSignalHandler handler(&loop, &mutex, &cv);

    uint16_t port = 0;
    const int server_fd = createTcpServerSocket(&port);
    std::thread server_thread([&]() {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        ASSERT_GE(client_fd, 0);
        ::close(client_fd);
        ::close(server_fd);
    });

    sinnet::connection::TCPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    if (!handler.connected()) {
        std::exception_ptr loop_error;
        std::thread loop_thread([&]() {
            try {
                loop.run();
            } catch (...) {
                loop_error = std::current_exception();
            }
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return handler.connected(); }));
        }
        loop_thread.join();
        ASSERT_EQ(loop_error, nullptr);
    }

    server_thread.join();
    EXPECT_TRUE(handler.connected());
    EXPECT_EQ(handler.connectError(), 0);
}

TEST(ConnectionTests, ConnectFailureInvokesOnConnectError) {
    sinnet::EventLoop loop;
    std::mutex mutex;
    std::condition_variable cv;
    ConnectSignalHandler handler(&loop, &mutex, &cv);

    const uint16_t unused_port = reserveUnusedTcpPort();
    sinnet::connection::TCPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(unused_port));

    if (handler.connectError() == 0) {
        std::exception_ptr loop_error;
        std::thread loop_thread([&]() {
            try {
                loop.run();
            } catch (...) {
                loop_error = std::current_exception();
            }
        });
        {
            std::unique_lock<std::mutex> lock(mutex);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() {
                return handler.connectError() != 0;
            }));
        }
        loop_thread.join();
        ASSERT_EQ(loop_error, nullptr);
    }

    EXPECT_FALSE(handler.connected());
    EXPECT_NE(handler.connectError(), 0);
}

TEST(ConnectionTests, LargeSendBuffersAreReusedAcrossFlushes) {
    sinnet::EventLoop loop;
    NoopConnectionHandler handler;

    uint16_t port = 0;
    const int server_fd = createTcpServerSocket(&port);
    std::atomic<size_t> received_total{0};

    std::thread server_thread([&]() {
        const int client_fd = ::accept(server_fd, nullptr, nullptr);
        ASSERT_GE(client_fd, 0);
        std::array<char, 4096> buffer {};
        for (;;) {
            const ssize_t n = ::recv(client_fd, buffer.data(), buffer.size(), 0);
            if (n > 0) {
                received_total.fetch_add(static_cast<size_t>(n), std::memory_order_relaxed);
                continue;
            }
            break;
        }
        ::close(client_fd);
        ::close(server_fd);
    });

    InspectableTCPConnection connection(loop, handler);
    connection.connect(makeIpv4Endpoint(port));

    static constexpr size_t kPayloadSize = 4096;
    std::array<std::byte, kPayloadSize> payload {};
    std::fill(payload.begin(), payload.end(), std::byte{'A'});

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (...) {
            loop_error = std::current_exception();
        }
    });

    ASSERT_EQ(connection.send(payload, 0), static_cast<ssize_t>(kPayloadSize));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (connection.reusableHeapBufferCount() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GT(connection.reusableHeapBufferCount(), 0U);
    ASSERT_EQ(connection.send(payload, 0), static_cast<ssize_t>(kPayloadSize));

    while (connection.reusableHeapBufferTakeHits() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GT(connection.reusableHeapBufferTakeHits(), 0U);
    while (received_total.load(std::memory_order_relaxed) < (kPayloadSize * 2) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    connection.close();
    loop.stop();
    loop_thread.join();
    server_thread.join();

    ASSERT_EQ(loop_error, nullptr);
    EXPECT_GT(connection.reusableHeapBufferTakeHits(), 0U);
    EXPECT_GE(received_total.load(std::memory_order_relaxed), kPayloadSize * 2);
}

TEST(ConnectionTests, UdpReceiveScratchBuffersAreStable) {
    sinnet::EventLoop loop;
    NoopConnectionHandler handler;
    InspectableUDPConnection connection(loop, handler);

    const void* messages_ptr_before = connection.recvMessagesPtr();
    const void* buffers_ptr_before = connection.recvBuffersPtr();

    uint16_t port = 0;
    const int server_fd = createUdpServerSocket(&port);
    connection.connect(makeIpv4Endpoint(port));

    const void* messages_ptr_after = connection.recvMessagesPtr();
    const void* buffers_ptr_after = connection.recvBuffersPtr();

    EXPECT_EQ(messages_ptr_before, messages_ptr_after);
    EXPECT_EQ(buffers_ptr_before, buffers_ptr_after);

    connection.close();
    ::close(server_fd);
}

}  // namespace
