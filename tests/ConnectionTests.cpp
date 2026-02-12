#include "sinnet/connection/ConnectionHandler.hpp"
#include "sinnet/connection/TCPConnection.hpp"
#include "sinnet/connection/UDPConnection.hpp"
#include "sinnet/eventloop/EventLoop.hpp"

#include <arpa/inet.h>
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

std::string portToString(uint16_t port) {
    return std::to_string(static_cast<unsigned>(port));
}

class StopLoopException : public std::exception {
public:
    const char* what() const noexcept override {
        return "stop event loop";
    }
};

class StopOnDataHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit StopOnDataHandler(std::string* storage,
                               std::mutex* mutex,
                               std::condition_variable* cv,
                               bool* ready,
                               std::string expected_payload)
        : storage_(storage),
          mutex_(mutex),
          cv_(cv),
          ready_(ready),
          expected_payload_(std::move(expected_payload)) {}

    void onData(sinnet::connection::Connection&, std::span<const std::byte> data) override {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            storage_->append(reinterpret_cast<const char*>(data.data()), data.size());
            *ready_ = storage_->find(expected_payload_) != std::string::npos;
        }
        cv_->notify_one();
        if (*ready_) {
            throw StopLoopException();
        }
    }

private:
    std::string* storage_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
    bool* ready_ = nullptr;
    std::string expected_payload_;
};

class StopOnByteCountHandler : public sinnet::connection::ConnectionHandler {
public:
    explicit StopOnByteCountHandler(std::string* storage,
                                    std::mutex* mutex,
                                    std::condition_variable* cv,
                                    bool* ready,
                                    size_t expected_bytes)
        : storage_(storage),
          mutex_(mutex),
          cv_(cv),
          ready_(ready),
          expected_bytes_(expected_bytes) {}

    void onData(sinnet::connection::Connection&, std::span<const std::byte> data) override {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            storage_->append(reinterpret_cast<const char*>(data.data()), data.size());
            *ready_ = storage_->size() >= expected_bytes_;
        }
        cv_->notify_one();
        if (*ready_) {
            throw StopLoopException();
        }
    }

private:
    std::string* storage_ = nullptr;
    std::mutex* mutex_ = nullptr;
    std::condition_variable* cv_ = nullptr;
    bool* ready_ = nullptr;
    size_t expected_bytes_ = 0;
};

TEST(ConnectionTests, TcpConnectionCanSendAndReceive) {
    sinnet::EventLoop loop;
    std::string received_payload;
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    StopOnDataHandler handler(&received_payload, &mutex, &cv, &ready, "pong");

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
    connection.connect("127.0.0.1", portToString(port));

    const char request[] = "ping";
    ASSERT_EQ(connection.send(std::as_bytes(std::span(request, 4)), 0), 4);

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (const StopLoopException&) {
            // Expected control-flow exception in this test.
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
    StopOnDataHandler handler(&received_payload, &mutex, &cv, &ready, "pong");

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
    connection.connect("127.0.0.1", portToString(port));

    const char request[] = "ping";
    ASSERT_EQ(connection.send(std::as_bytes(std::span(request, 4)), 0), 4);

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (const StopLoopException&) {
            // Expected control-flow exception in this test.
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
    StopOnDataHandler handler(&received_data, &mutex, &cv, &ready, "data");

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
    connection.connect("127.0.0.1", portToString(port));

    std::exception_ptr loop_error;
    std::thread loop_thread([&]() {
        try {
            loop.run();
        } catch (const StopLoopException&) {
            // Expected control-flow exception in this test.
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
    StopOnByteCountHandler handler(&received_data, &mutex, &cv, &ready, expected_bytes);

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
    connection.connect("127.0.0.1", portToString(port));

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
        } catch (const StopLoopException&) {
            // Expected control-flow exception in this test.
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

}  // namespace
