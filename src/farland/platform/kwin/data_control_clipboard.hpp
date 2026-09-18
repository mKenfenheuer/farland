// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/clipboard.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ext_data_control_device_v1;
struct ext_data_control_manager_v1;
struct ext_data_control_offer_v1;
struct ext_data_control_source_v1;

namespace farland::platform::kwin {

struct DataControlClipboardOptions {
    /// Local data larger than this fails a read.
    std::size_t max_read_size = std::size_t{64} * 1024 * 1024;
    /// How long a desktop application may take to hand over its data, and
    /// to take ours.
    std::chrono::milliseconds read_timeout{15'000};
    std::chrono::milliseconds write_timeout{15'000};
};

/// The desktop clipboard through ext-data-control-v1 (wayland-protocols
/// staging; KWin, wlroots compositors and Mutter 48+ offer it): the device's
/// selection events announce the desktop's types and offers read them; a
/// data source of the client's types answers pastes. Transfers go through
/// pipes that dispatch() serves without blocking. Only the regular
/// selection; the primary selection is left alone.
///
/// Nothing in it is specific to KWin. Threading: as WaylandConnection, whose
/// dispatch() delivers the events; the clipboard's own dispatch() pumps the
/// pipes. The process must ignore SIGPIPE.
class DataControlClipboard final : public Clipboard {
public:
    /// Fails when the compositor offers no ext_data_control_manager_v1 or no
    /// seat.
    [[nodiscard]] static Result<std::unique_ptr<DataControlClipboard>> create(WaylandConnection& connection,
                                                                              DataControlClipboardOptions options = {});

    DataControlClipboard(const DataControlClipboard&) = delete;
    DataControlClipboard& operator=(const DataControlClipboard&) = delete;
    DataControlClipboard(DataControlClipboard&&) = delete;
    DataControlClipboard& operator=(DataControlClipboard&&) = delete;
    /// Refuses the pastes still being written.
    ~DataControlClipboard() override;

    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return mime_types_; }
    void set_selection(const std::vector<std::string>& mime_types) override;
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override;
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override;
    [[nodiscard]] std::optional<ClipboardEvent> poll_event() override;
    [[nodiscard]] std::vector<PollFd> poll_fds() const override;
    void dispatch() override;

    /// The compositor ended the data device (the seat went away).
    [[nodiscard]] bool finished() const noexcept { return device_ == nullptr; }

    struct Listeners;

private:
    using Clock = std::chrono::steady_clock;

    struct Offer {
        ext_data_control_offer_v1* proxy = nullptr;
        std::vector<std::string> mime_types;
    };
    struct Read {
        std::uint64_t id = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        Clock::time_point deadline;
    };
    struct Write {
        std::uint32_t serial = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };

    DataControlClipboard(WaylandConnection& connection, DataControlClipboardOptions options) noexcept
        : connection_(connection), options_(options)
    {
    }
    void offer_added(ext_data_control_offer_v1* proxy);
    void selection(ext_data_control_offer_v1* proxy);
    void primary_selection(ext_data_control_offer_v1* proxy);
    void source_send(const char* mime_type, int fd);
    void source_cancelled();
    void device_finished();
    [[nodiscard]] Offer* find_offer(ext_data_control_offer_v1* proxy);
    void destroy_offer(ext_data_control_offer_v1* proxy);
    [[nodiscard]] bool pump(Read& read);
    [[nodiscard]] static bool pump_write(Write& write);

    WaylandConnection& connection_;
    DataControlClipboardOptions options_;
    ext_data_control_manager_v1* manager_ = nullptr;
    ext_data_control_device_v1* device_ = nullptr;
    /// Offers announced but not yet made the selection, and the selection.
    std::vector<std::unique_ptr<Offer>> offers_;
    ext_data_control_offer_v1* selection_ = nullptr;
    /// The client's types while the session owns the clipboard.
    ext_data_control_source_v1* source_ = nullptr;
    std::vector<std::string> source_types_;
    std::optional<std::vector<std::string>> mime_types_;
    std::deque<ClipboardEvent> events_;
    /// Pastes waiting for write().
    std::vector<std::pair<std::uint32_t, UniqueFd>> requested_;
    std::uint32_t next_serial_ = 1;
    std::vector<Read> reads_;
    std::vector<Write> writes_;
    std::uint64_t next_read_ = 1;
};

}  // namespace farland::platform::kwin
