#include "sinnet/eventloop/EventLoop.hpp"
#include "sinnet/eventloop/EventLoopHandler.hpp"

#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>

namespace sinnet {

namespace {

constexpr uint32_t kDefaultEpollEvents = EPOLLIN | EPOLLOUT;
constexpr int kEpollMaxEvents = 64;
constexpr uint32_t kDefaultInitialSlotsCapacity = 1024;
constexpr uint32_t kFdTableHardCap = 8192;

uint32_t computeFdTableMax() {
    struct rlimit rl {};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return kFdTableHardCap;
    }

    const rlim_t limit = rl.rlim_cur;
    if (limit == RLIM_INFINITY) {
        return kFdTableHardCap;
    }

    const uint64_t bounded =
        std::min<uint64_t>(static_cast<uint64_t>(limit), static_cast<uint64_t>(kFdTableHardCap));
    return static_cast<uint32_t>(bounded);
}

}  // namespace

// -----------------------------------------------------------------------------
// Token encoding: u64 = (generation << 32) | slot_id
// -----------------------------------------------------------------------------

uint64_t EventLoop::makeToken(uint32_t slot_id, uint32_t generation) {
    return (uint64_t{generation} << 32) | uint64_t{slot_id};
}

// -----------------------------------------------------------------------------
// Slot allocation (O(1) free list)
// -----------------------------------------------------------------------------

uint32_t EventLoop::allocSlot() {
    if (free_head_ != kInvalidSlot) {
        uint32_t slot_id = free_head_;
        free_head_ = slots_[slot_id].next_free;
        slots_[slot_id].next_free = kInvalidSlot;
        return slot_id;
    }
    throw std::runtime_error("slot pool exhausted; increase initial_slots_capacity");
}

void EventLoop::freeSlot(uint32_t slot_id) {
    slots_[slot_id].ptr = nullptr;
    slots_[slot_id].generation++;
    slots_[slot_id].fd = -1;
    slots_[slot_id].next_free = free_head_;
    free_head_ = slot_id;
}

// -----------------------------------------------------------------------------
// Constructor / destructor
// -----------------------------------------------------------------------------

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));
    }

    // Pre-size direct fd index table once at startup.
    const uint32_t max_fd = computeFdTableMax();
    fd_to_slot_.assign(static_cast<size_t>(max_fd) + 1, kInvalidSlot);

    // Preallocate contiguous slot pool and build O(1) free list.
    slots_.resize(kDefaultInitialSlotsCapacity);
    for (uint32_t i = 0; i < kDefaultInitialSlotsCapacity; ++i) {
        slots_[i].next_free = (i + 1 < kDefaultInitialSlotsCapacity) ? (i + 1) : kInvalidSlot;
    }
    free_head_ = kDefaultInitialSlotsCapacity > 0 ? 0 : kInvalidSlot;
}

EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        for (size_t fd = 0; fd < fd_to_slot_.size(); ++fd) {
            if (fd_to_slot_[fd] == kInvalidSlot) {
                continue;
            }
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, static_cast<int>(fd), nullptr);
            close(static_cast<int>(fd));
        }
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

// -----------------------------------------------------------------------------
// RegisterFD: alloc slot, store token in epoll_event.data.u64
// -----------------------------------------------------------------------------

void EventLoop::registerFd(int fd, EventLoopHandler* handler, uint32_t events) {
    if (fd < 0 || static_cast<size_t>(fd) >= fd_to_slot_.size()) {
        throw std::out_of_range("fd is outside preallocated fd_to_slot_ table");
    }
    if (fd_to_slot_[fd] != kInvalidSlot) {
        throw std::runtime_error("fd is already registered");
    }
    if (handler == nullptr) {
        throw std::invalid_argument("handler must not be null");
    }

    if (events == 0) {
        events = kDefaultEpollEvents;
    }

    uint32_t slot_id = allocSlot();
    Slot& slot = slots_[slot_id];
    slot.ptr = handler;
    slot.fd = fd;

    uint64_t token = makeToken(slot_id, slot.generation);

    struct epoll_event ev {};
    ev.events = events;
    ev.data.u64 = token;

    int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        freeSlot(slot_id);
        throw std::runtime_error(std::string("epoll_ctl ADD: ") + std::strerror(errno));
    }

    fd_to_slot_[fd] = slot_id;
}

// -----------------------------------------------------------------------------
// UnregisterFD: epoll_ctl DEL, close, invalidate slot, push to free list
// -----------------------------------------------------------------------------

void EventLoop::unregisterFd(int fd) {
    if (fd < 0 || static_cast<size_t>(fd) >= fd_to_slot_.size()) {
        return;
    }

    const uint32_t slot_id = fd_to_slot_[fd];
    if (slot_id == kInvalidSlot || slot_id >= slots_.size()) {
        return;
    }

    // Defensive check: ignore inconsistent mapping instead of touching wrong slot.
    if (slots_[slot_id].fd != fd) {
        return;
    }

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);

    fd_to_slot_[fd] = kInvalidSlot;
    freeSlot(slot_id);
}

// -----------------------------------------------------------------------------
// run(): main epoll loop with token validation
// -----------------------------------------------------------------------------

void EventLoop::run() {
    struct epoll_event events[kEpollMaxEvents];

    for (;;) {
        const int n = epoll_wait(epoll_fd_, events, kEpollMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
        }

        for (int i = 0; i < n; ++i) {
            const uint64_t token = events[i].data.u64;
            const uint32_t slot_id = static_cast<uint32_t>(token);
            const uint32_t gen = static_cast<uint32_t>(token >> 32);

            if (slot_id >= slots_.size()) {
                continue;
            }

            Slot& slot = slots_[slot_id];
            if (slot.generation == gen) {
                EventLoopHandler* const handler = slot.ptr;
                if (handler != nullptr) {
                    handler->onEvent(events[i].events);
                }
            }
            // else: stale event (slot reused or deregistered), ignore
        }
    }
}

// -----------------------------------------------------------------------------
// CPU pinning (Linux)
// -----------------------------------------------------------------------------

void EventLoop::pinToCpu(int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        throw std::runtime_error(std::string("pthread_setaffinity_np: ") + std::strerror(errno));
    }
#else
    (void)cpu_id;
#endif
}

}  // namespace sinnet
