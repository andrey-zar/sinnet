#include "sinnet/connection/TCPConnection.hpp"

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>

namespace sinnet::connection {

TCPConnection::TCPConnection(sinnet::EventLoop& loop, ConnectionHandler& handler)
    : Connection(loop, handler, SOCK_STREAM, IPPROTO_TCP) {}

void TCPConnection::connect(const Endpoint& endpoint) {
    connectToRemote(endpoint);
}

ssize_t TCPConnection::send(std::span<const std::byte> data, int flags) {
    return enqueueSendData(data, flags, "tcp connection is not open", true);
}

void TCPConnection::handleReadableEvent() {
    if (!isOpen()) {
        return;
    }

    char buffer[4096];
    for (;;) {
        const ssize_t n = ::recv(socketFd(), buffer, sizeof(buffer), 0);
        if (n > 0) {
            const auto* data_ptr = reinterpret_cast<const std::byte*>(buffer);
            handler().onData(*this, std::span<const std::byte>(data_ptr, static_cast<size_t>(n)));
            continue;
        }

        if (n == 0) {
            close();
            handler().onClosed(*this);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        throw std::system_error(errno, std::generic_category(), "recv");
    }
}

void TCPConnection::flushSendBuffer() {
    while (!send_queue_.empty()) {
        struct iovec iovecs[kMaxIovecBatch];
        int iov_count = 0;
        for (const PendingChunk& chunk : send_queue_) {
            if (iov_count >= static_cast<int>(kMaxIovecBatch)) {
                break;
            }

            const size_t remaining = chunk.data.size() - chunk.offset;
            if (remaining == 0) {
                continue;
            }

            iovecs[iov_count].iov_base =
                const_cast<char*>(chunk.data.data() + chunk.offset);
            iovecs[iov_count].iov_len = remaining;
            ++iov_count;
        }
        if (iov_count == 0) {
            break;
        }

        struct msghdr msg {};
        msg.msg_iov = iovecs;
        msg.msg_iovlen = static_cast<size_t>(iov_count);

        const ssize_t sent = ::sendmsg(socketFd(), &msg, MSG_NOSIGNAL);
        if (sent > 0) {
            size_t consumed = static_cast<size_t>(sent);
            pending_send_bytes_ -= consumed;

            while (consumed > 0 && !send_queue_.empty()) {
                PendingChunk& front = send_queue_.front();
                const size_t remaining = front.data.size() - front.offset;
                if (consumed < remaining) {
                    front.offset += consumed;
                    consumed = 0;
                    break;
                }

                consumed -= remaining;
                send_queue_.pop_front();
            }
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        throw std::system_error(errno, std::generic_category(), "sendmsg");
    }

    if (send_queue_.empty()) {
        pending_send_bytes_ = 0;
    }
    updateRegistrationEvents();
}

}  // namespace sinnet::connection
