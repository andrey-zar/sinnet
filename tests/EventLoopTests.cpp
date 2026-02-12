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
    void onEvent(uint32_t) override {}
};

class RecordingHandler : public sinnet::EventLoopHandler {
public:
    explicit RecordingHandler(std::function<void(uint32_t)> callback)
        : callback_(std::move(callback)) {}

    void onEvent(uint32_t event_mask) override {
        callback_(event_mask);
    }

private:
    std::function<void(uint32_t)> callback_;
};

class StopLoopException : public std::exception {
public:
    const char* what() const noexcept override {
        return "stop event loop";
    }
};

TEST(EventLoopTests, RegisterFdRejectsNullHandler) {
    sinnet::EventLoop loop;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    EXPECT_THROW(loop.registerFd(fds[0], nullptr), std::invalid_argument);

    close(fds[0]);
    close(fds[1]);
}

TEST(EventLoopTests, RegisterFdRejectsNegativeFd) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    EXPECT_THROW(loop.registerFd(-1, &handler), std::out_of_range);
    EXPECT_THROW(loop.registerFd(1'000'000, &handler), std::out_of_range);
}

TEST(EventLoopTests, DuplicateFdRegistrationThrows) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    ASSERT_NO_THROW(loop.registerFd(fds[0], &handler));
    EXPECT_THROW(loop.registerFd(fds[0], &handler), std::runtime_error);

    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
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
    ASSERT_NO_THROW(loop.registerFd(fds[0], &handler));
    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
    EXPECT_NO_THROW(loop.unregisterFd(fds[0]));
    close(fds[1]);
}

TEST(EventLoopTests, UnregisterClosesFd) {
    sinnet::EventLoop loop;
    NoopHandler handler;

    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_NO_THROW(loop.registerFd(fds[0], &handler));
    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));

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
        ASSERT_NO_THROW(loop.registerFd(fds[0], &handler));
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
        throw StopLoopException();
    });

    ASSERT_NO_THROW(loop.registerFd(fds[0], &handler, EPOLLIN));

    std::exception_ptr thread_error;
    std::thread runner([&]() {
        try {
            loop.run();
        } catch (const StopLoopException&) {
            // Expected control-flow exception to stop the infinite loop in tests.
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

    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
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
        throw StopLoopException();
    });

    ASSERT_NO_THROW(loop.registerFd(fds[0], &handler, 0));

    std::exception_ptr thread_error;
    std::thread runner([&]() {
        try {
            loop.run();
        } catch (const StopLoopException&) {
            // Expected control-flow exception to stop the infinite loop in tests.
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

    ASSERT_NO_THROW(loop.unregisterFd(fds[0]));
    close(fds[1]);
}

TEST(EventLoopTests, PinToCpuDoesNotThrowForCurrentCpu) {
    sinnet::EventLoop loop;
    const int cpu_id = sched_getcpu();
    ASSERT_GE(cpu_id, 0);
    EXPECT_NO_THROW(loop.pinToCpu(cpu_id));
}

}  // namespace
