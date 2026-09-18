// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/data_control_clipboard.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ext-data-control-v1-client-protocol.h>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <unistd.h>
#include <wayland-client.h>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.clipboard";
constexpr std::size_t read_chunk = std::size_t{64} * 1024;

void set_nonblocking(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL);    // NOLINT(cppcoreguidelines-pro-type-vararg)
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // NOLINT(cppcoreguidelines-pro-type-vararg)
}

}  // namespace

struct DataControlClipboard::Listeners {
    static DataControlClipboard& self(void* data) noexcept { return *static_cast<DataControlClipboard*>(data); }

    static constexpr ext_data_control_offer_v1_listener offer{
        .offer =
            [](void* data, ext_data_control_offer_v1* proxy, const char* mime_type) {
                if (auto* o = self(data).find_offer(proxy); o != nullptr && mime_type != nullptr) {
                    o->mime_types.emplace_back(mime_type);
                }
            },
    };

    static constexpr ext_data_control_device_v1_listener device{
        .data_offer = [](void* data, ext_data_control_device_v1* /*device*/,
                         ext_data_control_offer_v1* proxy) { self(data).offer_added(proxy); },
        .selection = [](void* data, ext_data_control_device_v1* /*device*/,
                        ext_data_control_offer_v1* proxy) { self(data).selection(proxy); },
        .finished = [](void* data, ext_data_control_device_v1* /*device*/) { self(data).device_finished(); },
        .primary_selection = [](void* data, ext_data_control_device_v1* /*device*/,
                                ext_data_control_offer_v1* proxy) { self(data).primary_selection(proxy); },
    };

    static constexpr ext_data_control_source_v1_listener source{
        .send = [](void* data, ext_data_control_source_v1* /*source*/, const char* mime_type,
                   std::int32_t fd) { self(data).source_send(mime_type, fd); },
        .cancelled = [](void* data, ext_data_control_source_v1* /*source*/) { self(data).source_cancelled(); },
    };
};

Result<std::unique_ptr<DataControlClipboard>> DataControlClipboard::create(WaylandConnection& connection,
                                                                           DataControlClipboardOptions options)
{
    const auto* global = connection.find_global(ext_data_control_manager_v1_interface.name);
    if (global == nullptr) {
        return fail(Errc::unsupported, "the compositor offers no ext_data_control_manager_v1");
    }
    if (connection.seat() == nullptr) {
        return fail(Errc::unsupported, "the compositor has no seat");
    }
    std::unique_ptr<DataControlClipboard> clipboard(new DataControlClipboard(connection, options));
    clipboard->manager_ =
        static_cast<ext_data_control_manager_v1*>(connection.bind(*global, &ext_data_control_manager_v1_interface, 1));
    clipboard->device_ = ext_data_control_manager_v1_get_data_device(clipboard->manager_, connection.seat());
    ext_data_control_device_v1_add_listener(clipboard->device_, &Listeners::device, clipboard.get());
    // The first selection event follows at once.
    if (!connection.roundtrip(std::chrono::seconds(5))) {
        return fail(Errc::io, "the compositor did not announce its clipboard");
    }
    return clipboard;
}

DataControlClipboard::~DataControlClipboard()
{
    for (auto& offer : offers_) {
        ext_data_control_offer_v1_destroy(offer->proxy);
    }
    if (source_ != nullptr) {
        ext_data_control_source_v1_destroy(source_);
    }
    if (device_ != nullptr) {
        ext_data_control_device_v1_destroy(device_);
    }
    if (manager_ != nullptr) {
        ext_data_control_manager_v1_destroy(manager_);
    }
    connection_.flush();
}

DataControlClipboard::Offer* DataControlClipboard::find_offer(ext_data_control_offer_v1* proxy)
{
    const auto it = std::ranges::find_if(offers_, [proxy](const auto& o) { return o->proxy == proxy; });
    return it == offers_.end() ? nullptr : it->get();
}

void DataControlClipboard::destroy_offer(ext_data_control_offer_v1* proxy)
{
    std::erase_if(offers_, [proxy](const auto& o) {
        if (o->proxy != proxy) {
            return false;
        }
        ext_data_control_offer_v1_destroy(proxy);
        return true;
    });
}

void DataControlClipboard::offer_added(ext_data_control_offer_v1* proxy)
{
    auto offer = std::make_unique<Offer>();
    offer->proxy = proxy;
    ext_data_control_offer_v1_add_listener(proxy, &Listeners::offer, this);
    offers_.push_back(std::move(offer));
}

void DataControlClipboard::selection(ext_data_control_offer_v1* proxy)
{
    if (selection_ != nullptr && selection_ != proxy) {
        destroy_offer(selection_);
    }
    selection_ = proxy;
    if (proxy == nullptr) {
        // Nothing on the clipboard any more.
        if (source_ == nullptr) {
            const bool had_types = mime_types_ && !mime_types_->empty();
            mime_types_ = std::vector<std::string>{};
            if (had_types) {
                events_.emplace_back(clipboard_event::OwnerChanged{});
            }
        }
        return;
    }
    const auto* offer = find_offer(proxy);
    if (offer == nullptr) {
        return;
    }
    // The compositor announces the session's own selection too: while our
    // source lives and the types match, the offer is ours.
    auto types = offer->mime_types;
    auto ours = source_types_;
    std::ranges::sort(types);
    std::ranges::sort(ours);
    if (source_ != nullptr && types == ours) {
        mime_types_.reset();
        return;
    }
    mime_types_ = offer->mime_types;
    events_.emplace_back(clipboard_event::OwnerChanged{offer->mime_types});
}

void DataControlClipboard::primary_selection(ext_data_control_offer_v1* proxy)
{
    if (proxy != nullptr && proxy != selection_) {
        destroy_offer(proxy);
    }
}

void DataControlClipboard::device_finished()
{
    log::warn(log_component, "the compositor ended the clipboard device");
    ext_data_control_device_v1_destroy(device_);
    device_ = nullptr;
}

void DataControlClipboard::set_selection(const std::vector<std::string>& mime_types)
{
    if (device_ == nullptr) {
        return;
    }
    if (source_ != nullptr) {
        ext_data_control_source_v1_destroy(source_);
    }
    source_ = ext_data_control_manager_v1_create_data_source(manager_);
    ext_data_control_source_v1_add_listener(source_, &Listeners::source, this);
    for (const auto& mime : mime_types) {
        ext_data_control_source_v1_offer(source_, mime.c_str());
    }
    source_types_ = mime_types;
    ext_data_control_device_v1_set_selection(device_, source_);
    mime_types_.reset();
    connection_.flush();
}

void DataControlClipboard::source_send(const char* mime_type, int fd)
{
    UniqueFd owned(fd);
    if (source_ == nullptr || mime_type == nullptr) {
        return;
    }
    set_nonblocking(owned.get());
    const auto serial = next_serial_++;
    requested_.emplace_back(serial, std::move(owned));
    events_.emplace_back(clipboard_event::TransferRequested{serial, mime_type});
}

void DataControlClipboard::source_cancelled()
{
    // Someone else copied; their selection event follows.
    if (source_ != nullptr) {
        ext_data_control_source_v1_destroy(source_);
        source_ = nullptr;
    }
    source_types_.clear();
}

void DataControlClipboard::write(std::uint32_t serial, std::optional<std::vector<std::byte>> data)
{
    const auto it = std::ranges::find(requested_, serial, &std::pair<std::uint32_t, UniqueFd>::first);
    if (it == requested_.end()) {
        return;
    }
    UniqueFd fd = std::move(it->second);
    requested_.erase(it);
    if (!data) {
        return;  // closing the pipe refuses the paste
    }
    writes_.push_back(Write{serial, std::move(fd), std::move(*data), 0, Clock::now() + options_.write_timeout});
    if (pump_write(writes_.back())) {
        writes_.pop_back();
    }
}

std::uint64_t DataControlClipboard::read(const std::string& mime_type)
{
    const std::uint64_t id = next_read_++;
    std::array<int, 2> pipe_fds{-1, -1};
    if (selection_ == nullptr || ::pipe2(pipe_fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
        return id;
    }
    UniqueFd read_end(pipe_fds[0]);
    UniqueFd write_end(pipe_fds[1]);
    ext_data_control_offer_v1_receive(selection_, mime_type.c_str(), write_end.get());
    connection_.flush();
    write_end.reset();  // the compositor has its copy
    reads_.push_back(Read{id, std::move(read_end), {}, Clock::now() + options_.read_timeout});
    return id;
}

std::optional<ClipboardEvent> DataControlClipboard::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::vector<PollFd> DataControlClipboard::poll_fds() const
{
    std::vector<PollFd> fds;
    fds.reserve(reads_.size() + writes_.size() + 1);
    fds.push_back(PollFd{connection_.fd(), POLLIN});
    for (const auto& read : reads_) {
        fds.push_back(PollFd{read.fd.get(), POLLIN});
    }
    for (const auto& write : writes_) {
        fds.push_back(PollFd{write.fd.get(), POLLOUT});
    }
    return fds;
}

bool DataControlClipboard::pump(Read& read)
{
    std::array<std::byte, read_chunk> buffer{};
    for (;;) {
        const auto n = ::read(read.fd.get(), buffer.data(), buffer.size());
        if (n > 0) {
            if (read.data.size() + static_cast<std::size_t>(n) > options_.max_read_size) {
                log::warn(log_component, "the desktop's clipboard data is larger than {} bytes",
                          options_.max_read_size);
                events_.emplace_back(clipboard_event::ReadFinished{read.id, std::nullopt});
                return true;
            }
            const auto got = std::span(buffer).first(static_cast<std::size_t>(n));
            read.data.insert(read.data.end(), got.begin(), got.end());
            continue;
        }
        if (n == 0) {
            events_.emplace_back(clipboard_event::ReadFinished{read.id, std::move(read.data)});
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {  // EWOULDBLOCK on Linux
            if (Clock::now() < read.deadline) {
                return false;
            }
            log::warn(log_component, "the desktop did not hand over its clipboard data in time");
        }
        events_.emplace_back(clipboard_event::ReadFinished{read.id, std::nullopt});
        return true;
    }
}

bool DataControlClipboard::pump_write(Write& write)
{
    while (write.offset < write.data.size()) {
        const auto rest = std::span(write.data).subspan(write.offset);
        const auto n = ::write(write.fd.get(), rest.data(), rest.size());
        if (n > 0) {
            write.offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN && Clock::now() < write.deadline) {
            return false;
        }
        log::debug(log_component, "a desktop application stopped taking clipboard data");
        return true;
    }
    return true;  // closing the pipe ends the paste
}

void DataControlClipboard::dispatch()
{
    connection_.dispatch();
    std::erase_if(reads_, [this](Read& read) { return pump(read); });
    std::erase_if(writes_, [](Write& write) { return pump_write(write); });
}

}  // namespace farland::platform::kwin
