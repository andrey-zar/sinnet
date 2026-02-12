// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Andrei Zaretckii
//
// Single-threaded epoll-based event loop with slot+generation handle scheme.
// Avoids shared_ptr/weak_ptr in hot path; prevents use-after-free via generation
// validation. Designed for low-latency: no allocations in hot path, cache-friendly.
//
// ABA PREVENTION:
// Without generation: epoll returns event for slot_id X. Between epoll_wait and
// dispatch, fd is unregistered, slot X freed and reused for a new fd. We would
// dispatch to the NEW handler using the OLD event (different fd). With generation:
// token = (gen << 32) | slot_id. On free we increment generation. Old events
// carry old gen; slot.generation != gen => stale, ignore. Same slot_id, different
// gen => slot was reused.
//
// LOW-LATENCY: single-threaded, no allocs in run() (preallocate/reserve),
// contiguous slots, stack-allocated epoll_event buffer. Deferred cleanup optional.
//
// PSEUDOCODE:
//   RegisterFD(fd, handler):
//     slot_id = free_head ? pop_free() : grow_slots()
//     slot.ptr=handler, slot.fd=fd
//     token = (slot.generation << 32) | slot_id
//     epoll_ctl(ADD, fd, {events, data.u64=token})
//     fd_to_slot[fd] = slot_id
//
//   UnregisterFD(fd):
//     slot_id = fd_to_slot[fd]; fd_to_slot[fd] = kInvalidSlot
//     epoll_ctl(DEL, fd); close(fd)
//     slot.ptr=null, slot.generation++; push_free(slot_id)
//
//   main loop:
//     events = epoll_wait(...)
//     for e in events:
//       slot_id, gen = decode(e.data.u64)
//       if slot.ptr && slot.generation==gen: slot.ptr->onEvent(e.events)

#pragma once

#include <cstdint>
#include <vector>

struct epoll_event;

namespace sinnet {

class EventLoopHandler;

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Register fd with handler. Uses epoll_event.data.u64 = (generation << 32) | slot_id.
    void registerFd(int fd, EventLoopHandler* handler, uint32_t events = 0x001 | 0x004);

    // Unregister fd: epoll_ctl(DEL), close(fd), invalidate slot, push to free list.
    void unregisterFd(int fd);

    // Main loop: epoll_wait -> dispatch by token -> validate slot.ptr && slot.generation.
    void run();

    // Optional: pin thread to isolated CPU core (call before run()). Linux-only.
    void pinToCpu(int cpu_id);

private:
    static constexpr uint32_t kInvalidSlot = UINT32_MAX;

    struct Slot {
        EventLoopHandler* ptr = nullptr;
        uint32_t generation = 0;
        int fd = -1;
        uint32_t next_free = kInvalidSlot;  // free-list linkage
    };

    // Token encoding: u64 = (gen << 32) | slot_id
    static uint64_t makeToken(uint32_t slot_id, uint32_t generation);

    uint32_t allocSlot();
    void freeSlot(uint32_t slot_id);

    int epoll_fd_ = -1;
    std::vector<Slot> slots_;
    uint32_t free_head_ = kInvalidSlot;
    std::vector<uint32_t> fd_to_slot_;  // fd -> slot_id, fixed direct index table
};

}  // namespace sinnet
