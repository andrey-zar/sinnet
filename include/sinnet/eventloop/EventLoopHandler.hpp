#pragma once

#include <cstdint>

namespace sinnet {

class EventLoopHandler {
public:
    EventLoopHandler() = default;
    virtual ~EventLoopHandler() = default;

    virtual void onEvent(uint32_t event_mask) = 0;

protected:
    static constexpr uint32_t kEpollIn = 0x001;
    static constexpr uint32_t kEpollOut = 0x004;
};

}  // namespace sinnet