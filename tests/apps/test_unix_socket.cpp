// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "unix_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <utility>

using farland::Errc;
using farland::UniqueFd;
using farland::app::peer_credentials;
using farland::app::receive_message;
using farland::app::send_message;

namespace {

std::pair<UniqueFd, UniqueFd> socket_pair()
{
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

std::pair<UniqueFd, UniqueFd> pipe_pair()
{
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::pipe(fds.data()) == 0);
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

std::vector<std::byte> frame_of(std::span<const std::byte> body)
{
    std::vector<std::byte> out(4 + body.size());
    const auto size = static_cast<std::uint32_t>(body.size());
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>(size >> (8 * i));
    }
    std::ranges::copy(body, out.begin() + 4);
    return out;
}

std::vector<std::byte> frame_of(std::string_view body)
{
    return frame_of(std::as_bytes(std::span(body)));
}

void write_raw(int fd, std::span<const std::byte> bytes)
{
    REQUIRE(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
}

}  // namespace

TEST_CASE("A message and its descriptor cross a Unix socket")
{
    auto [a, b] = socket_pair();
    auto [read_end, write_end] = pipe_pair();
    REQUIRE(send_message(a.get(), frame_of("hello"), write_end.get()).has_value());
    write_end.reset();  // the received duplicate is now the only write end

    auto received = receive_message(b.get(), 64).value();
    REQUIRE(received.has_value());
    CHECK(received->frame == frame_of("hello"));
    REQUIRE(received->fd.valid());
    CHECK((::fcntl(received->fd.get(), F_GETFD) & FD_CLOEXEC) != 0);

    REQUIRE(::write(received->fd.get(), "x", 1) == 1);
    char c = 0;
    REQUIRE(::read(read_end.get(), &c, 1) == 1);
    CHECK(c == 'x');
}

TEST_CASE("A descriptor belongs to the message it was sent with")
{
    auto [a, b] = socket_pair();
    auto [read_end, write_end] = pipe_pair();
    // Both messages are queued before the receiver reads anything.
    REQUIRE(send_message(a.get(), frame_of("first")).has_value());
    REQUIRE(send_message(a.get(), frame_of("second"), write_end.get()).has_value());
    REQUIRE(send_message(a.get(), frame_of("third")).has_value());

    auto first = receive_message(b.get(), 64).value();
    REQUIRE(first.has_value());
    CHECK(first->frame == frame_of("first"));
    CHECK_FALSE(first->fd.valid());

    auto second = receive_message(b.get(), 64).value();
    REQUIRE(second.has_value());
    CHECK(second->frame == frame_of("second"));
    CHECK(second->fd.valid());

    auto third = receive_message(b.get(), 64).value();
    REQUIRE(third.has_value());
    CHECK_FALSE(third->fd.valid());
}

TEST_CASE("End of file between messages is not an error, inside one it is")
{
    {
        auto [a, b] = socket_pair();
        a.reset();
        const auto received = receive_message(b.get(), 64);
        REQUIRE(received.has_value());
        CHECK_FALSE(received->has_value());
    }
    {
        auto [a, b] = socket_pair();
        const auto frame = frame_of("abcdef");
        write_raw(a.get(), std::span(frame).first(6));
        a.reset();
        const auto received = receive_message(b.get(), 64);
        REQUIRE_FALSE(received.has_value());
        CHECK(received.error().code == Errc::truncated);
    }
    {
        auto [a, b] = socket_pair();
        const auto frame = frame_of("abcdef");
        write_raw(a.get(), std::span(frame).first(2));  // inside the length prefix
        a.reset();
        CHECK(receive_message(b.get(), 64).error().code == Errc::truncated);
    }
}

TEST_CASE("Oversized and empty messages are refused")
{
    auto [a, b] = socket_pair();
    const std::array<std::byte, 8> big{};
    write_raw(a.get(), frame_of(big));
    CHECK(receive_message(b.get(), 7).error().code == Errc::limit_exceeded);

    auto [c, d] = socket_pair();
    const std::array<std::byte, 4> zero{};
    write_raw(c.get(), zero);
    CHECK(receive_message(d.get(), 64).error().code == Errc::limit_exceeded);
}

TEST_CASE("Two descriptors with one message are refused and closed")
{
    auto [a, b] = socket_pair();
    auto [read_end, write_end] = pipe_pair();
    const auto frame = frame_of("two");
    std::array<int, 2> fds{write_end.get(), write_end.get()};
    iovec iov{const_cast<std::byte*>(frame.data()), frame.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(fds))> control{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = static_cast<decltype(msg.msg_controllen)>(control.size());
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = static_cast<decltype(c->cmsg_len)>(CMSG_LEN(sizeof(fds)));
    std::memcpy(CMSG_DATA(c), fds.data(), sizeof(fds));
    REQUIRE(::sendmsg(a.get(), &msg, 0) == static_cast<ssize_t>(frame.size()));
    write_end.reset();

    const auto received = receive_message(b.get(), 64);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == Errc::invalid_value);

    // Every write end is closed again: the pipe reads end of file, not EAGAIN.
    REQUIRE(::fcntl(read_end.get(), F_SETFL, O_NONBLOCK) == 0);
    char byte = 0;
    CHECK(::read(read_end.get(), &byte, 1) == 0);
}

TEST_CASE("Receiving times out")
{
    auto [a, b] = socket_pair();
    const auto received = receive_message(b.get(), 64, 50);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == Errc::io);
}

TEST_CASE("Large messages cross a non-blocking socket in pieces")
{
    auto [a, b] = socket_pair();
    REQUIRE(::fcntl(a.get(), F_SETFL, O_NONBLOCK) == 0);
    REQUIRE(::fcntl(b.get(), F_SETFL, O_NONBLOCK) == 0);
    auto [read_end, write_end] = pipe_pair();
    std::vector<std::byte> body(std::size_t{1} << 20);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::byte>(i * 7);
    }
    const auto frame = frame_of(body);

    bool sent = false;
    std::thread sender([&] { sent = send_message(a.get(), frame, write_end.get()).has_value(); });
    auto received = receive_message(b.get(), body.size(), 10'000);
    sender.join();
    CHECK(sent);
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    CHECK((*received)->frame == frame);
    CHECK((*received)->fd.valid());
}

TEST_CASE("The peer's credentials come from the kernel")
{
    auto [a, b] = socket_pair();
    const auto peer = peer_credentials(b.get()).value();
    CHECK(peer.uid == ::getuid());
    CHECK(peer.gid == ::getgid());
#ifdef __linux__
    REQUIRE(peer.pid.has_value());
#endif
    if (peer.pid) {
        CHECK(*peer.pid == ::getpid());
    }
    CHECK_FALSE(peer_credentials(-1).has_value());
}
