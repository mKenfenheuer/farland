// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <format>
#include <poll.h>

namespace farland::platform::portal::detail {

namespace {

constexpr std::string_view log_component = "platform.portal";

bool is_one_of(std::string_view name, std::initializer_list<std::string_view> names)
{
    return std::ranges::find(names, name) != names.end();
}

std::uint64_t monotonic_usec() noexcept
{
    timespec now{};
    static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
    return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000U) + (static_cast<std::uint64_t>(now.tv_nsec) / 1'000U);
}

/// Milliseconds until `deadline`, rounded up, at most INT_MAX.
int poll_timeout(Clock::time_point deadline, std::uint64_t bus_timeout_usec)
{
    using namespace std::chrono;
    auto wait = ceil<milliseconds>(deadline - Clock::now());
    if (bus_timeout_usec != UINT64_MAX) {
        const std::uint64_t now = monotonic_usec();
        const auto bus_wait = milliseconds(bus_timeout_usec > now ? ((bus_timeout_usec - now) + 999U) / 1000U : 0U);
        wait = std::min(wait, bus_wait);
    }
    return static_cast<int>(std::clamp<milliseconds::rep>(wait.count(), 0, INT_MAX));
}

}  // namespace

PortalError make_error(PortalErrc code, std::string message)
{
    return PortalError{code, std::move(message)};
}

std::unexpected<PortalError> fail(PortalErrc code, std::string message)
{
    return std::unexpected(make_error(code, std::move(message)));
}

std::unexpected<PortalError> fail_call(std::string_view what, const sd_bus_error* error, int r)
{
    if (error != nullptr && error->name != nullptr) {
        const std::string_view name = error->name;
        const std::string message =
            std::format("{}: {} ({})", what, error->message != nullptr ? error->message : "no message", name);
        if (is_one_of(name,
                      {"org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner",
                       "org.freedesktop.DBus.Error.UnknownInterface", "org.freedesktop.DBus.Error.UnknownMethod",
                       "org.freedesktop.DBus.Error.UnknownObject", "org.freedesktop.DBus.Error.UnknownProperty"})) {
            return fail(PortalErrc::unavailable, message);
        }
        if (is_one_of(name, {"org.freedesktop.DBus.Error.NoReply", "org.freedesktop.DBus.Error.Timeout",
                             "org.freedesktop.DBus.Error.TimedOut"})) {
            return fail(PortalErrc::timed_out, message);
        }
        return fail(PortalErrc::failed, message);
    }
    return fail(PortalErrc::protocol, std::format("{}: {}", what, std::strerror(r < 0 ? -r : EIO)));
}

// --- MessageWriter

void MessageWriter::append(char type, const void* value)
{
    if (status_ >= 0) {
        status_ = std::min(sd_bus_message_append_basic(message_, type, value), 0);
    }
}

MessageWriter& MessageWriter::object_path(const std::string& value)
{
    append('o', value.c_str());
    return *this;
}

MessageWriter& MessageWriter::string(const std::string& value)
{
    append('s', value.c_str());
    return *this;
}

MessageWriter& MessageWriter::u32(std::uint32_t value)
{
    append('u', &value);
    return *this;
}

MessageWriter& MessageWriter::i32(std::int32_t value)
{
    append('i', &value);
    return *this;
}

MessageWriter& MessageWriter::f64(double value)
{
    append('d', &value);
    return *this;
}

MessageWriter& MessageWriter::options(const Options& options)
{
    const auto open = [this](char type, const char* contents) {
        if (status_ >= 0) {
            status_ = std::min(sd_bus_message_open_container(message_, type, contents), 0);
        }
    };
    const auto close = [this] {
        if (status_ >= 0) {
            status_ = std::min(sd_bus_message_close_container(message_), 0);
        }
    };
    open('a', "{sv}");
    for (const auto& option : options) {
        open('e', "sv");
        string(option.key);
        if (const auto* text = std::get_if<std::string>(&option.value)) {
            open('v', "s");
            string(*text);
        } else if (const auto* number = std::get_if<std::uint32_t>(&option.value)) {
            open('v', "u");
            u32(*number);
        } else {
            open('v', "b");
            const int flag = std::get<bool>(option.value) ? 1 : 0;  // sd-bus booleans are ints
            append('b', &flag);
        }
        close();
        close();
    }
    close();
    return *this;
}

// --- MessageReader

bool MessageReader::u32(std::uint32_t& value)
{
    return sd_bus_message_read_basic(message_, 'u', &value) > 0;
}

bool MessageReader::boolean(bool& value)
{
    int flag = 0;
    if (sd_bus_message_read_basic(message_, 'b', &flag) <= 0) {
        return false;
    }
    value = flag != 0;
    return true;
}

bool MessageReader::string(std::string& value)
{
    char type = 0;
    if (sd_bus_message_peek_type(message_, &type, nullptr) <= 0 || (type != 's' && type != 'o')) {
        return false;
    }
    const char* text = nullptr;
    if (sd_bus_message_read_basic(message_, type, static_cast<void*>(&text)) <= 0 || text == nullptr) {
        return false;
    }
    value = text;
    return true;
}

bool MessageReader::int_pair(std::pair<std::int32_t, std::int32_t>& value)
{
    return enter('r', "ii") && sd_bus_message_read_basic(message_, 'i', &value.first) > 0 &&
           sd_bus_message_read_basic(message_, 'i', &value.second) > 0 && exit();
}

bool MessageReader::fd(int& value)
{
    return sd_bus_message_read_basic(message_, 'h', &value) > 0;
}

bool MessageReader::enter(char type, const char* contents)
{
    return sd_bus_message_enter_container(message_, type, contents) > 0;
}

bool MessageReader::exit()
{
    return sd_bus_message_exit_container(message_) >= 0;
}

bool MessageReader::vardict(
    const std::function<bool(std::string_view key, std::string_view signature, bool& ok)>& visit)
{
    if (!enter('a', "{sv}")) {
        return false;
    }
    for (;;) {
        const int r = sd_bus_message_enter_container(message_, 'e', "sv");
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            break;
        }
        const char* key = nullptr;
        char type = 0;
        const char* contents = nullptr;
        if (sd_bus_message_read_basic(message_, 's', static_cast<void*>(&key)) <= 0 || key == nullptr ||
            sd_bus_message_peek_type(message_, &type, &contents) <= 0 || type != 'v' || contents == nullptr ||
            !enter('v', contents)) {
            return false;
        }
        bool ok = true;
        const bool consumed = visit(key, contents, ok);
        if (!ok || (!consumed && sd_bus_message_skip(message_, contents) < 0) || !exit() || !exit()) {
            return false;
        }
    }
    return exit();
}

// --- Bus

PortalResult<std::unique_ptr<Bus>> Bus::open(const std::string& address)
{
    std::unique_ptr<Bus> bus(new Bus());
    sd_bus* raw = nullptr;
    int r = 0;
    if (address.empty()) {
        r = sd_bus_open_user(&raw);
        bus->bus_.reset(raw);
    } else {
        r = sd_bus_new(&raw);
        bus->bus_.reset(raw);
        if (r >= 0) {
            r = sd_bus_set_address(raw, address.c_str());
        }
        if (r >= 0) {
            r = sd_bus_set_bus_client(raw, 1);
        }
        if (r >= 0) {
            r = sd_bus_start(raw);
        }
    }
    if (r < 0) {
        return fail(PortalErrc::unavailable, std::format("cannot connect to the session bus{}{}: {}",
                                                         address.empty() ? "" : " at ", address, std::strerror(-r)));
    }
    const char* unique = nullptr;
    r = sd_bus_get_unique_name(raw, &unique);
    if (r < 0 || unique == nullptr) {
        return fail(PortalErrc::unavailable, std::format("no unique name on the session bus: {}", std::strerror(-r)));
    }
    bus->unique_name_ = unique;
    return bus;
}

PortalResult<void> Bus::run_until(const std::function<bool()>& done, Clock::time_point deadline, int cancel_fd)
{
    for (;;) {
        for (;;) {
            if (done()) {
                return {};
            }
            const int r = sd_bus_process(bus_.get(), nullptr);
            if (r < 0) {
                return fail(PortalErrc::protocol, std::format("session bus connection lost: {}", std::strerror(-r)));
            }
            if (r == 0) {
                break;
            }
        }
        if (done()) {
            return {};
        }
        if (Clock::now() >= deadline) {
            return fail(PortalErrc::timed_out, "the portal did not answer in time");
        }
        const int events = sd_bus_get_events(bus_.get());
        std::uint64_t bus_timeout = UINT64_MAX;
        if (events < 0 || sd_bus_get_timeout(bus_.get(), &bus_timeout) < 0) {
            return fail(PortalErrc::protocol, "session bus connection lost");
        }
        std::array<pollfd, 2> fds{{
            {.fd = sd_bus_get_fd(bus_.get()), .events = static_cast<short>(events), .revents = 0},
            {.fd = cancel_fd, .events = POLLIN, .revents = 0},
        }};
        const nfds_t count = cancel_fd >= 0 ? 2 : 1;
        if (::poll(fds.data(), count, poll_timeout(deadline, bus_timeout)) < 0 && errno != EINTR) {
            return fail(PortalErrc::protocol, std::format("poll: {}", std::strerror(errno)));
        }
        if (cancel_fd >= 0 && (fds[1].revents & POLLIN) != 0) {
            return fail(PortalErrc::aborted, "cancelled");
        }
    }
}

PortalResult<void> Bus::process_pending()
{
    for (;;) {
        const int r = sd_bus_process(bus_.get(), nullptr);
        if (r < 0) {
            return fail(PortalErrc::protocol, std::format("session bus connection lost: {}", std::strerror(-r)));
        }
        if (r == 0) {
            return {};
        }
    }
}

PortalResult<MessagePtr> Bus::new_call(const char* interface, const char* member, const char* path,
                                       const char* destination)
{
    sd_bus_message* raw = nullptr;
    const int r = sd_bus_message_new_method_call(bus_.get(), &raw, destination, path, interface, member);
    if (r < 0) {
        return fail(PortalErrc::protocol, std::format("cannot create a {} call: {}", member, std::strerror(-r)));
    }
    return MessagePtr(raw);
}

PortalResult<MessagePtr> Bus::call(sd_bus_message* message, std::string_view what, Clock::time_point deadline,
                                   int cancel_fd)
{
    MessagePtr reply;
    const auto on_reply = [](sd_bus_message* m, void* userdata, sd_bus_error* /*error*/) -> int {
        static_cast<MessagePtr*>(userdata)->reset(sd_bus_message_ref(m));
        return 0;
    };
    const auto remaining = std::chrono::ceil<std::chrono::microseconds>(deadline - Clock::now()).count();
    sd_bus_slot* raw_slot = nullptr;
    const int r = sd_bus_call_async(bus_.get(), &raw_slot, message, on_reply, &reply,
                                    static_cast<std::uint64_t>(std::max<std::int64_t>(remaining, 1)));
    const SlotPtr slot(raw_slot);
    if (r < 0) {
        return fail_call(what, nullptr, r);
    }
    auto waited = run_until([&reply] { return reply != nullptr; }, deadline, cancel_fd);
    if (!waited) {
        auto error = std::move(waited).error();
        error.message = std::format("{}: {}", what, error.message);
        return std::unexpected(std::move(error));
    }
    if (sd_bus_message_is_method_error(reply.get(), nullptr) > 0) {
        return fail_call(what, sd_bus_message_get_error(reply.get()), -EIO);
    }
    return reply;
}

void Bus::send(sd_bus_message* message)
{
    // A floating slot: sd-bus owns it and drops it after the reply.
    const int r = sd_bus_call_async(bus_.get(), nullptr, message, &Bus::on_send_reply, this, 0);
    if (r < 0) {
        on_send_reply(nullptr, this, nullptr);
    }
}

int Bus::on_send_reply(sd_bus_message* reply, void* userdata, sd_bus_error* /*error*/)
{
    if (reply != nullptr && sd_bus_message_is_method_error(reply, nullptr) <= 0) {
        return 0;
    }
    auto* self = static_cast<Bus*>(userdata);
    const sd_bus_error* error = reply != nullptr ? sd_bus_message_get_error(reply) : nullptr;
    const char* text = error != nullptr && error->message != nullptr ? error->message : "not sent";
    // Input arrives many times a second; log the first failure, then every 1000th.
    if (self->failed_sends_++ % 1000 == 0) {
        log::warn(log_component, "portal call failed ({} so far): {}", self->failed_sends_, text);
    }
    return 0;
}

}  // namespace farland::platform::portal::detail
