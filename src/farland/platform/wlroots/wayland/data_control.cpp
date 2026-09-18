// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>
#include <farland/platform/wlroots/wayland/data_control.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <ext-data-control-v1-client-protocol.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wlr-data-control-unstable-v1-client-protocol.h>

namespace farland::platform::wayland {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "platform.wayland.clipboard";
constexpr std::size_t read_chunk = std::size_t{64} * 1024;

/// ext-data-control-v1 (wayland-protocols staging).
struct ExtDataControl {
    using Manager = ext_data_control_manager_v1;
    using Device = ext_data_control_device_v1;
    using Source = ext_data_control_source_v1;
    using Offer = ext_data_control_offer_v1;
    using DeviceListener = ext_data_control_device_v1_listener;
    using SourceListener = ext_data_control_source_v1_listener;
    using OfferListener = ext_data_control_offer_v1_listener;
    static constexpr std::string_view name = "ext_data_control_manager_v1";

    static const wl_interface* interface() { return &ext_data_control_manager_v1_interface; }
    static Device* get_device(Manager* manager, wl_seat* seat)
    {
        return ext_data_control_manager_v1_get_data_device(manager, seat);
    }
    static Source* create_source(Manager* manager) { return ext_data_control_manager_v1_create_data_source(manager); }
    static void listen(Device* device, const DeviceListener* listener, void* data)
    {
        ext_data_control_device_v1_add_listener(device, listener, data);
    }
    static void listen(Source* source, const SourceListener* listener, void* data)
    {
        ext_data_control_source_v1_add_listener(source, listener, data);
    }
    static void listen(Offer* offer, const OfferListener* listener, void* data)
    {
        ext_data_control_offer_v1_add_listener(offer, listener, data);
    }
    static void set_selection(Device* device, Source* source)
    {
        ext_data_control_device_v1_set_selection(device, source);
    }
    static void offer(Source* source, const std::string& mime)
    {
        ext_data_control_source_v1_offer(source, mime.c_str());
    }
    static void receive(Offer* offer, const std::string& mime, int fd)
    {
        ext_data_control_offer_v1_receive(offer, mime.c_str(), fd);
    }
    static void destroy(Manager* manager) { ext_data_control_manager_v1_destroy(manager); }
    static void destroy(Device* device) { ext_data_control_device_v1_destroy(device); }
    static void destroy(Source* source) { ext_data_control_source_v1_destroy(source); }
    static void destroy(Offer* offer) { ext_data_control_offer_v1_destroy(offer); }
};

/// zwlr-data-control-unstable-v1, version 1 (no primary selection).
struct WlrDataControl {
    using Manager = zwlr_data_control_manager_v1;
    using Device = zwlr_data_control_device_v1;
    using Source = zwlr_data_control_source_v1;
    using Offer = zwlr_data_control_offer_v1;
    using DeviceListener = zwlr_data_control_device_v1_listener;
    using SourceListener = zwlr_data_control_source_v1_listener;
    using OfferListener = zwlr_data_control_offer_v1_listener;
    static constexpr std::string_view name = "zwlr_data_control_manager_v1";

    static const wl_interface* interface() { return &zwlr_data_control_manager_v1_interface; }
    static Device* get_device(Manager* manager, wl_seat* seat)
    {
        return zwlr_data_control_manager_v1_get_data_device(manager, seat);
    }
    static Source* create_source(Manager* manager) { return zwlr_data_control_manager_v1_create_data_source(manager); }
    static void listen(Device* device, const DeviceListener* listener, void* data)
    {
        zwlr_data_control_device_v1_add_listener(device, listener, data);
    }
    static void listen(Source* source, const SourceListener* listener, void* data)
    {
        zwlr_data_control_source_v1_add_listener(source, listener, data);
    }
    static void listen(Offer* offer, const OfferListener* listener, void* data)
    {
        zwlr_data_control_offer_v1_add_listener(offer, listener, data);
    }
    static void set_selection(Device* device, Source* source)
    {
        zwlr_data_control_device_v1_set_selection(device, source);
    }
    static void offer(Source* source, const std::string& mime)
    {
        zwlr_data_control_source_v1_offer(source, mime.c_str());
    }
    static void receive(Offer* offer, const std::string& mime, int fd)
    {
        zwlr_data_control_offer_v1_receive(offer, mime.c_str(), fd);
    }
    static void destroy(Manager* manager) { zwlr_data_control_manager_v1_destroy(manager); }
    static void destroy(Device* device) { zwlr_data_control_device_v1_destroy(device); }
    static void destroy(Source* source) { zwlr_data_control_source_v1_destroy(source); }
    static void destroy(Offer* offer) { zwlr_data_control_offer_v1_destroy(offer); }
};

void set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
}

template <class P>
class DataControlClipboard final : public Clipboard {
public:
    DataControlClipboard(Connection& connection, typename P::Manager* manager, wl_seat* seat,
                         const DataControlOptions& options)
        : connection_(&connection), manager_(manager), device_(P::get_device(manager, seat)), options_(options)
    {
        static constexpr typename P::DeviceListener device_listener{
            .data_offer =
                [](void* data, typename P::Device* /*device*/, typename P::Offer* offer) {
                    auto* self = static_cast<DataControlClipboard*>(data);
                    self->offers_.emplace(offer, std::vector<std::string>{});
                    listen_offer(offer, self);
                },
            .selection =
                [](void* data, typename P::Device* /*device*/, typename P::Offer* offer) {
                    static_cast<DataControlClipboard*>(data)->on_selection(offer);
                },
            .finished = [](void* data,
                           typename P::Device* /*device*/) { static_cast<DataControlClipboard*>(data)->on_finished(); },
            .primary_selection =
                [](void* data, typename P::Device* /*device*/, typename P::Offer* offer) {
                    // Not shared: only the regular clipboard maps onto cliprdr.
                    static_cast<DataControlClipboard*>(data)->drop_offer(offer);
                },
        };
        P::listen(device_, &device_listener, this);
        connection_->flush();
    }
    DataControlClipboard(const DataControlClipboard&) = delete;
    DataControlClipboard& operator=(const DataControlClipboard&) = delete;
    DataControlClipboard(DataControlClipboard&&) = delete;
    DataControlClipboard& operator=(DataControlClipboard&&) = delete;
    ~DataControlClipboard() override
    {
        for (auto& [offer, types] : offers_) {
            P::destroy(offer);
        }
        if (source_ != nullptr) {
            P::destroy(source_);
        }
        if (device_ != nullptr) {
            P::destroy(device_);
        }
        P::destroy(manager_);
        connection_->flush();
    }

    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return mime_types_; }

    void set_selection(const std::vector<std::string>& mime_types) override
    {
        if (device_ == nullptr) {
            return;
        }
        if (source_ != nullptr) {
            P::destroy(source_);
            source_ = nullptr;
        }
        if (!mime_types.empty()) {
            source_ = P::create_source(manager_);
            static constexpr typename P::SourceListener source_listener{
                .send = [](void* data, typename P::Source* /*source*/, const char* mime,
                           std::int32_t fd) { static_cast<DataControlClipboard*>(data)->on_send(mime, UniqueFd(fd)); },
                .cancelled =
                    [](void* data, typename P::Source* source) {
                        static_cast<DataControlClipboard*>(data)->on_cancelled(source);
                    },
            };
            P::listen(source_, &source_listener, this);
            for (const auto& mime : mime_types) {
                P::offer(source_, mime);
            }
        }
        // A null source clears the selection.
        P::set_selection(device_, source_);
        mime_types_.reset();
        connection_->flush();
    }

    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override
    {
        const auto it = sends_.find(serial);
        if (it == sends_.end()) {
            return;
        }
        UniqueFd fd = std::move(it->second);
        sends_.erase(it);
        if (!data || data->empty()) {
            return;  // closing the pipe ends (or refuses) the paste
        }
        set_nonblocking(fd.get());
        writes_.push_back(Write{std::move(fd), std::move(*data), 0, Clock::now() + options_.write_timeout});
    }

    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override
    {
        const std::uint64_t id = next_read_++;
        const auto selected = offers_.find(selection_);
        std::array<int, 2> pipe_fds{-1, -1};
        if (selection_ == nullptr || selected == offers_.end() ||
            ::pipe2(pipe_fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
            events_.emplace_back(clipboard_event::ReadFinished{id, std::nullopt});
            return id;
        }
        UniqueFd read_end(pipe_fds[0]);
        const UniqueFd write_end(pipe_fds[1]);
        // libwayland duplicates the descriptor as it queues the request, so
        // ours closes at once and the pipe ends when the owner closes its copy.
        P::receive(selection_, mime_type, write_end.get());
        connection_->flush();
        reads_.push_back(Read{id, std::move(read_end), {}, Clock::now() + options_.read_timeout});
        return id;
    }

    [[nodiscard]] std::optional<ClipboardEvent> poll_event() override
    {
        if (events_.empty()) {
            return std::nullopt;
        }
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

    [[nodiscard]] std::vector<PollFd> poll_fds() const override
    {
        std::vector<PollFd> fds;
        fds.reserve(reads_.size() + writes_.size());
        for (const auto& r : reads_) {
            fds.push_back(PollFd{r.fd.get(), POLLIN});
        }
        for (const auto& w : writes_) {
            fds.push_back(PollFd{w.fd.get(), POLLOUT});
        }
        return fds;
    }

    void dispatch() override
    {
        const auto now = Clock::now();
        std::erase_if(reads_, [this, now](Read& r) {
            if (pump(r)) {
                return true;
            }
            if (now > r.deadline) {
                log::warn(log_component, "the desktop's clipboard owner did not hand over its data in time");
                events_.emplace_back(clipboard_event::ReadFinished{r.id, std::nullopt});
                return true;
            }
            return false;
        });
        std::erase_if(writes_, [now](Write& w) {
            if (pump(w)) {
                return true;
            }
            if (now > w.deadline) {
                log::warn(log_component, "a desktop application did not take the pasted data in time");
                return true;
            }
            return false;
        });
        connection_->flush();
    }

private:
    struct Read {
        std::uint64_t id = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        Clock::time_point deadline;
    };
    struct Write {
        UniqueFd fd;
        std::vector<std::byte> data;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };

    static void listen_offer(typename P::Offer* new_offer, DataControlClipboard* clipboard)
    {
        static constexpr typename P::OfferListener offer_listener{
            .offer =
                [](void* data, typename P::Offer* offer, const char* mime) {
                    auto* self = static_cast<DataControlClipboard*>(data);
                    if (const auto it = self->offers_.find(offer); it != self->offers_.end()) {
                        it->second.emplace_back(mime);
                    }
                },
        };
        P::listen(new_offer, &offer_listener, clipboard);
    }

    void drop_offer(typename P::Offer* offer)
    {
        if (offer == nullptr || offer == selection_) {
            return;
        }
        if (const auto it = offers_.find(offer); it != offers_.end()) {
            P::destroy(offer);
            offers_.erase(it);
        }
    }

    void on_selection(typename P::Offer* offer)
    {
        auto* const previous = selection_;
        selection_ = offer;
        drop_offer(previous);
        if (source_ != nullptr) {
            // Our own set_selection() coming back: the session owns the
            // clipboard, so it announces nothing.
            mime_types_.reset();
            return;
        }
        std::vector<std::string> types;
        if (const auto it = offers_.find(offer); offer != nullptr && it != offers_.end()) {
            types = it->second;
        }
        if (mime_types_ == types) {
            return;
        }
        mime_types_ = types;
        events_.emplace_back(clipboard_event::OwnerChanged{std::move(types)});
    }

    void on_send(const char* mime, UniqueFd fd)
    {
        const std::uint32_t serial = next_serial_++;
        sends_.emplace(serial, std::move(fd));
        events_.emplace_back(clipboard_event::TransferRequested{serial, mime});
    }

    void on_cancelled(typename P::Source* source)
    {
        // Someone else took the selection; their offer follows.
        P::destroy(source);
        if (source == source_) {
            source_ = nullptr;
        }
    }

    void on_finished()
    {
        log::warn(log_component, "the compositor withdrew the data-control device: no clipboard from now on");
        P::destroy(device_);
        device_ = nullptr;
        mime_types_.reset();
    }

    /// Reads what the pipe has; true when the transfer is over.
    [[nodiscard]] bool pump(Read& r)
    {
        std::array<std::byte, read_chunk> chunk{};
        while (true) {
            const auto got = ::read(r.fd.get(), chunk.data(), chunk.size());
            if (got > 0) {
                if (r.data.size() + static_cast<std::size_t>(got) > options_.max_read_size) {
                    log::warn(log_component, "the desktop's clipboard data is larger than {} bytes",
                              options_.max_read_size);
                    events_.emplace_back(clipboard_event::ReadFinished{r.id, std::nullopt});
                    return true;
                }
                const auto part = std::span(chunk).first(static_cast<std::size_t>(got));
                r.data.insert(r.data.end(), part.begin(), part.end());
                continue;
            }
            if (got == 0) {
                events_.emplace_back(clipboard_event::ReadFinished{r.id, std::move(r.data)});
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            events_.emplace_back(clipboard_event::ReadFinished{r.id, std::nullopt});
            return true;
        }
    }

    /// Writes what the pipe takes; true when the transfer is over.
    [[nodiscard]] static bool pump(Write& w)
    {
        while (w.offset < w.data.size()) {
            const auto left = std::span(w.data).subspan(w.offset);
            const auto put = ::write(w.fd.get(), left.data(), left.size());
            if (put > 0) {
                w.offset += static_cast<std::size_t>(put);
                continue;
            }
            if (put < 0 && errno == EINTR) {
                continue;
            }
            // EAGAIN waits for POLLOUT; anything else (EPIPE: the reader
            // gave up) ends the transfer.
            return put < 0 && errno != EAGAIN;
        }
        return true;
    }

    Connection* connection_;
    typename P::Manager* manager_;
    typename P::Device* device_;
    DataControlOptions options_;
    /// Our data source while it holds the selection.
    typename P::Source* source_ = nullptr;
    /// Offers the compositor introduced, with their types so far.
    std::map<typename P::Offer*, std::vector<std::string>> offers_;
    /// The offer of the current selection; null when empty.
    typename P::Offer* selection_ = nullptr;
    std::optional<std::vector<std::string>> mime_types_;
    std::map<std::uint32_t, UniqueFd> sends_;
    std::deque<ClipboardEvent> events_;
    std::vector<Read> reads_;
    std::vector<Write> writes_;
    std::uint32_t next_serial_ = 1;
    std::uint64_t next_read_ = 1;
};

template <class P>
std::unique_ptr<Clipboard> make_clipboard(Connection& connection, wl_seat* seat, const DataControlOptions& options)
{
    const Global* global = connection.find(P::name);
    if (global == nullptr) {
        return nullptr;
    }
    auto* manager = connection.bind<typename P::Manager>(*global, P::interface(), 1);
    log::info(log_component, "sharing the clipboard through {}", P::name);
    return std::make_unique<DataControlClipboard<P>>(connection, manager, seat, options);
}

}  // namespace

std::unique_ptr<Clipboard> create_data_control_clipboard(Connection& connection, wl_seat* seat,
                                                         const DataControlOptions& options)
{
    if (auto clipboard = make_clipboard<ExtDataControl>(connection, seat, options)) {
        return clipboard;
    }
    return make_clipboard<WlrDataControl>(connection, seat, options);
}

}  // namespace farland::platform::wayland
