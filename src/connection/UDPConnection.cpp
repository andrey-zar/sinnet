#include "sinnet/connection/UDPConnection.hpp"

#include <array>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

namespace sinnet::connection {

UDPConnection::UDPConnection(sinnet::EventLoop& loop, ConnectionHandler& handler)
    : Connection(loop, handler, SOCK_DGRAM, IPPROTO_UDP) {}

void UDPConnection::connect(const Endpoint& endpoint) {
    connectToRemote(endpoint);
}

ssize_t UDPConnection::send(std::span<const std::byte> data, int flags) {
    return enqueueSendData(data, flags, "udp connection is not open", false);
}

void UDPConnection::flushSendBuffer() noexcept {
    while (!send_queue_.empty()) {
        std::array<mmsghdr, kMaxBatchMessages> messages {};
        std::array<iovec, kMaxBatchMessages> iovecs {};

        size_t batch_size = 0;
        for (size_t i = 0; i < send_queue_.size() && batch_size < kMaxBatchMessages; ++i) {
            PendingChunk& chunk = send_queue_[i];
            const size_t remaining = chunk.size - chunk.offset;
            if (remaining == 0) {
                continue;
            }

            iovecs[batch_size].iov_base =
                const_cast<char*>(chunk.data() + chunk.offset);
            iovecs[batch_size].iov_len = remaining;

            messages[batch_size].msg_hdr.msg_iov = &iovecs[batch_size];
            messages[batch_size].msg_hdr.msg_iovlen = 1;
            ++batch_size;
        }

        if (batch_size == 0) {
            break;
        }

        const int sent_count =
            ::sendmmsg(socketFd(), messages.data(), static_cast<unsigned int>(batch_size), MSG_NOSIGNAL);
        if (sent_count > 0) {
            for (int i = 0; i < sent_count && !send_queue_.empty(); ++i) {
                PendingChunk& front = send_queue_.front();
                const size_t remaining = front.size - front.offset;
                const size_t sent_bytes = static_cast<size_t>(messages[static_cast<size_t>(i)].msg_len);
                const size_t consumed = (sent_bytes < remaining) ? sent_bytes : remaining;
                pending_send_bytes_ -= consumed;
                front.offset += consumed;

                if (front.offset >= front.size) {
                    recycleChunkStorage(front);
                    send_queue_.pop_front();
                }
            }
            continue;
        }

        if (sent_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        close();
        handler().onClosed(*this);
        return;
    }

    if (send_queue_.empty()) {
        pending_send_bytes_ = 0;
    }
    try {
        updateRegistrationEvents();
    } catch (...) {
        close();
        handler().onClosed(*this);
    }
}

void UDPConnection::handleReadableEvent() noexcept {
    for (;;) {
        for (size_t i = 0; i < kMaxBatchMessages; ++i) {
            recv_iovecs_[i].iov_base = recv_buffers_[i].data();
            recv_iovecs_[i].iov_len = recv_buffers_[i].size();
            recv_messages_[i].msg_hdr.msg_iov = &recv_iovecs_[i];
            recv_messages_[i].msg_hdr.msg_iovlen = 1;
            recv_messages_[i].msg_len = 0;
        }

        const int received =
            ::recvmmsg(socketFd(),
                       recv_messages_.data(),
                       static_cast<unsigned int>(kMaxBatchMessages),
                       MSG_DONTWAIT,
                       nullptr);

        if (received > 0) {
            for (int i = 0; i < received; ++i) {
                const size_t len = static_cast<size_t>(recv_messages_[static_cast<size_t>(i)].msg_len);
                if (len == 0) {
                    continue;
                }
                const auto* data_ptr =
                    reinterpret_cast<const std::byte*>(recv_buffers_[static_cast<size_t>(i)].data());
                handler().onData(*this, std::span<const std::byte>(data_ptr, len));
            }
            continue;
        }

        if (received == 0) {
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close();
        handler().onClosed(*this);
        return;
    }
}

}  // namespace sinnet::connection
