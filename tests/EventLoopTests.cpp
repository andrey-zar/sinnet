#include "sinnet/eventloop/EventLoop.hpp"
#include "sinnet/eventloop/EventLoopHandler.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <sched.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>

namespace {

class NoopHandler : public sinnet::EventLoopHandler {
public:
    void onEvent(uint32_t) noexcept override {}
};

class RecordingHandler : public sinnet::EventLoopHandler {
public:
    explicit RecordingHandler(std::function<void(uint32_t)> callback)
        : callback_(std::move(callback)) {}

    void onEvent(uint32_t event_mask) noexcept override {
        callback_(event_mask);
    }

private:
    std::function<void(uint32_t)> callback_;
};

TEST(EventLoopTests, RegisterFdRejectsNullHandler) {
    sinnet::EventLoop loop;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    EXPECT_THROW(loop.registerFdScoped(fds[0], nullptr), std::invalid_argument);

    close(fds[0]);
    close(fds[1]);
}

TEST(EventLoopTests, RegisterFdRejectsNegativeFd) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    EXPECT_THROW(loop.registerFdScoped(-1, &handler), std::out_of_range);
    EXPECT_THROW(loop.registerFdScoped(1'000'000, &handler), std::out_of_range);
}

TEST(EventLoopTests, DuplicateFdRegistrationThrows) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    auto registration = loop.registerFdScoped(fds[0], &handler);
    EXPECT_THROW(loop.registerFdScoped(fds[0], &handler), std::runtime_error);

    ASSERT_NO_THROW(registration.reset());
    close(fds[1]);
}

TEST(EventLoopTests, UnregisterOutOfRangeFdDoesNotThrow) {
    sinnet::EventLoop loop;
    EXPECT_NO_THROW(loop.unregisterFd(-1));
    EXPECT_NO_THROW(loop.unregisterFd(1'000'000));
}

TEST(EventLoopTests, UnregisterIsIdempotentForClosedMapping) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    auto registration = loop.registerFdScoped(fds[0], &handler);
    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
    EXPECT_NO_THROW(loop.unregisterFd(fds[0]));
    registration.reset();
    close(fds[1]);
}

TEST(EventLoopTests, UnregisterClosesFd) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    auto registration = loop.registerFdScoped(fds[0], &handler);
    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
    registration.reset();

    errno = 0;
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    close(fds[1]);
}

TEST(EventLoopTests, DestructorClosesRegisteredFd) {
    NoopHandler handler;
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    {
        sinnet::EventLoop loop;
        auto registration = loop.registerFdScoped(fds[0], &handler);
    }

    errno = 0;
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    close(fds[1]);
}

TEST(EventLoopTests, RunDispatchesReadableEvent) {
    sinnet::EventLoop loop;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    std::mutex mu;
    std::condition_variable cv;
    bool event_seen = false;
    uint32_t seen_mask = 0;

    RecordingHandler handler([&](uint32_t event_mask) {
        {
            std::lock_guard<std::mutex> lock(mu);
            event_seen = true;
            seen_mask = event_mask;
        }
        cv.notify_one();
        loop.stop();
    });

    auto registration = loop.registerFdScoped(fds[0], &handler, EPOLLIN);

    std::exception_ptr thread_error;
    std::thread runner([&]() {
        try {
            loop.run();
        } catch (...) {
            thread_error = std::current_exception();
        }
    });

    const char byte = 'x';
    ASSERT_EQ(write(fds[1], &byte, 1), 1);

    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return event_seen; }));
    }

    runner.join();
    ASSERT_EQ(thread_error, nullptr);
    EXPECT_NE((seen_mask & EPOLLIN), 0U);

    registration.reset();
    close(fds[1]);
}

TEST(EventLoopTests, RunWithDefaultEventsDispatchesReadableEvent) {
    sinnet::EventLoop loop;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    std::mutex mu;
    std::condition_variable cv;
    bool event_seen = false;
    uint32_t seen_mask = 0;

    RecordingHandler handler([&](uint32_t event_mask) {
        {
            std::lock_guard<std::mutex> lock(mu);
            event_seen = true;
            seen_mask = event_mask;
        }
        cv.notify_one();
        loop.stop();
    });

    auto registration = loop.registerFdScoped(fds[0], &handler, 0);

    std::exception_ptr thread_error;
    std::thread runner([&]() {
        try {
            loop.run();
        } catch (...) {
            thread_error = std::current_exception();
        }
    });

    const char byte = 'y';
    ASSERT_EQ(write(fds[1], &byte, 1), 1);

    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return event_seen; }));
    }

    runner.join();
    ASSERT_EQ(thread_error, nullptr);
    EXPECT_NE((seen_mask & EPOLLIN), 0U);

    registration.reset();
    close(fds[1]);
}

TEST(EventLoopTests, PinToCpuDoesNotThrowForCurrentCpu) {
    sinnet::EventLoop loop;
    const int cpu_id = sched_getcpu();
    ASSERT_GE(cpu_id, 0);
    EXPECT_NO_THROW(loop.pinToCpu(cpu_id));
}

TEST(EventLoopTests, ScopedRegistrationClosesFdOnScopeExit) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    {
        auto registration = loop.registerFdScoped(fds[0], &handler, EPOLLIN);
        ASSERT_TRUE(static_cast<bool>(registration));
        EXPECT_EQ(registration.fd(), fds[0]);
    }

    errno = 0;
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    close(fds[1]);
}

TEST(EventLoopTests, ScopedRegistrationMoveTransfersOwnership) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    {
        auto registration_a = loop.registerFdScoped(fds[0], &handler, EPOLLIN);
        auto registration_b = std::move(registration_a);
        EXPECT_FALSE(static_cast<bool>(registration_a));
        EXPECT_TRUE(static_cast<bool>(registration_b));
        EXPECT_EQ(registration_b.fd(), fds[0]);
    }

    errno = 0;
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    close(fds[1]);
}

TEST(EventLoopTests, UnregisterDuringDispatchDefersCloseUntilBatchEnd) {
    sinnet::EventLoop loop;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    std::mutex mu;
    std::condition_variable cv;
    bool callback_seen = false;
    bool fd_open_inside_callback = false;

    class DeferredCloseProbeHandler : public sinnet::EventLoopHandler {
    public:
        DeferredCloseProbeHandler(sinnet::EventLoop& loop,
                                  int fd,
                                  std::mutex& mu,
                                  std::condition_variable& cv,
                                  bool& callback_seen,
                                  bool& fd_open_inside_callback)
            : loop_(loop),
              fd_(fd),
              mu_(mu),
              cv_(cv),
              callback_seen_(callback_seen),
              fd_open_inside_callback_(fd_open_inside_callback) {}

        void onEvent(uint32_t) noexcept override {
            loop_.unregisterFd(fd_);
            errno = 0;
            fd_open_inside_callback_ = (fcntl(fd_, F_GETFD) != -1);
            {
                std::lock_guard<std::mutex> lock(mu_);
                callback_seen_ = true;
            }
            cv_.notify_one();
            loop_.stop();
        }

    private:
        sinnet::EventLoop& loop_;
        int fd_ = -1;
        std::mutex& mu_;
        std::condition_variable& cv_;
        bool& callback_seen_;
        bool& fd_open_inside_callback_;
    };

    DeferredCloseProbeHandler handler(
        loop, fds[0], mu, cv, callback_seen, fd_open_inside_callback);

    auto registration = loop.registerFdScoped(fds[0], &handler, EPOLLIN);

    std::exception_ptr thread_error;
    std::thread runner([&]() {
        try {
            loop.run();
        } catch (...) {
            thread_error = std::current_exception();
        }
    });

    const char byte = 'z';
    ASSERT_EQ(write(fds[1], &byte, 1), 1);

    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return callback_seen; }));
    }

    runner.join();
    ASSERT_EQ(thread_error, nullptr);
    EXPECT_TRUE(fd_open_inside_callback);

    errno = 0;
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    close(fds[1]);
}

}  // namespace
