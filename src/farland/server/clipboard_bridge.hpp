// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/cliprdr_server.hpp>
#include <farland/channels/svc.hpp>
#include <farland/platform/clipboard.hpp>
#include <farland/server/clipboard_files.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace farland::server {

struct ClipboardOptions {
    /// The largest data of one format, either way, and so the largest
    /// cliprdr message the client may send.
    std::size_t max_data_size = std::size_t{64} * 1024 * 1024;
    /// Copy files too (FileGroupDescriptorW and file contents).
    bool files = true;
    /// What one paste of the client's files may stage.
    std::uint64_t max_staged_bytes = std::uint64_t{1} << 30U;
    std::size_t max_staged_files = 4096;
    /// Where the client's files are staged; empty for
    /// clipboard_files::default_staging_root().
    std::filesystem::path staging_root;
    /// FILECONTENTS_RANGE size for the client's files.
    std::uint32_t range_size = std::uint32_t{1} << 20U;
    /// The most file data one of the client's requests gets at once.
    std::size_t max_read_size = std::size_t{4} * 1024 * 1024;
    channels::cliprdr::ServerConfig protocol;
};

/// Connects the client's clipboard (the cliprdr channel) with the desktop's
/// (a platform::Clipboard), both ways and delay-rendered: nothing is copied
/// until someone pastes. Formats map as follows:
///
/// | Desktop (MIME)                                  | Client ([MS-RDPECLIP])                   |
/// |-------------------------------------------------|------------------------------------------|
/// | text/plain;charset=utf-8, text/plain, UTF8_STRING | CF_UNICODETEXT (CF_TEXT from the client) |
/// | text/html                                       | "HTML Format"                            |
/// | image/png                                       | CF_DIB, CF_DIBV5, "PNG"                  |
/// | image/bmp                                       | CF_DIB, CF_DIBV5                         |
/// | text/uri-list, x-special/gnome-copied-files     | FileGroupDescriptorW and file contents   |
///
/// Files the client copied are fetched when the desktop pastes them, into a
/// private staging directory (clipboard_files::StagingDirectory; the last
/// two pastes are kept for the session), and handed over as file URIs.
/// Files the desktop copied are listed and read with
/// clipboard_files::LocalFileList.
class ClipboardBridge {
public:
    using Clock = std::chrono::steady_clock;
    /// Sends one Virtual Channel PDU (CHANNEL_PDU_HEADER and data) on "cliprdr".
    using Send = std::function<void(std::span<const std::byte> chunk)>;

    /// `desktop` must outlive the bridge.
    ClipboardBridge(platform::Clipboard& desktop, Send send, ClipboardOptions options = {});
    ClipboardBridge(const ClipboardBridge&) = delete;
    ClipboardBridge& operator=(const ClipboardBridge&) = delete;
    ClipboardBridge(ClipboardBridge&&) = delete;
    ClipboardBridge& operator=(ClipboardBridge&&) = delete;
    /// Fails the desktop's pastes still waiting, removes the staged files.
    ~ClipboardBridge();

    /// Starts the channel: capabilities and Monitor Ready go out.
    void start();
    /// One Virtual Channel PDU from the client. An error ends the channel
    /// (not the connection); the bridge then refuses every paste.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> chunk, Clock::time_point now);
    /// Dispatches the desktop clipboard and handles its events, and times
    /// out requests. Call on every loop iteration.
    void service(Clock::time_point now);

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    struct Transfer {
        std::uint32_t serial = 0;
        std::string mime_type;
    };
    struct ActiveTransfer {
        Transfer transfer;
        std::uint32_t format_id = 0;
    };
    struct PendingRead {
        std::uint64_t request = 0;
        std::uint32_t format_id = 0;
        std::string mime_type;
    };
    struct Staged {
        std::uint64_t epoch = 0;
        std::vector<std::filesystem::path> items;
    };
    struct Download;

    void process();
    void flush();
    void handle(channels::cliprdr::ServerEvent& event);
    void handle(platform::ClipboardEvent& event);
    void on_remote_formats(std::vector<channels::cliprdr::Format> list);
    void announce_desktop(const std::vector<std::string>& mime_types);
    void on_data_requested(std::uint64_t id, std::uint32_t format_id);
    void on_file_contents_requested(std::uint64_t id, const channels::cliprdr::FileContentsRequest& request);
    void on_read_finished(std::uint64_t id, std::optional<std::vector<std::byte>> data);
    [[nodiscard]] Result<std::vector<std::byte>> for_client(const PendingRead& read, std::span<const std::byte> data);
    void pump_transfers();
    [[nodiscard]] std::optional<std::uint32_t> source_format(const std::string& mime_type) const;
    void on_data_received(std::optional<std::vector<std::byte>> data);
    [[nodiscard]] static Result<std::vector<std::byte>> for_desktop(const ActiveTransfer& active,
                                                                    std::span<const std::byte> data);
    void start_download(const Transfer& transfer, std::span<const std::byte> file_list);
    void advance_download();
    void on_file_contents_received(std::uint32_t stream_id, std::optional<std::vector<std::byte>> data);
    void finish_download(bool ok);
    [[nodiscard]] std::vector<std::byte> staged_uris(const std::string& mime_type) const;
    [[nodiscard]] std::optional<std::uint32_t> remote_id(std::string_view name) const;
    [[nodiscard]] bool remote_has(std::uint32_t id) const;
    void fail_transfers();

    platform::Clipboard& desktop_;
    Send send_;
    ClipboardOptions options_;
    channels::svc::Reassembler reassembler_;
    channels::cliprdr::ClipboardServer server_;
    Clock::time_point now_;
    bool failed_ = false;

    // The client's clipboard, pasted on the desktop.
    std::vector<channels::cliprdr::Format> remote_formats_;
    std::uint64_t remote_epoch_ = 0;
    bool first_remote_list_ = true;
    std::deque<Transfer> transfers_;
    std::optional<ActiveTransfer> active_;
    std::vector<Transfer> waiting_for_files_;
    std::unique_ptr<Download> download_;
    std::optional<Staged> staged_;
    std::deque<clipboard_files::StagingDirectory> staging_dirs_;
    std::uint32_t next_clip_data_id_ = 1;

    // The desktop's clipboard, pasted on the client.
    std::optional<std::vector<std::string>> desktop_mimes_;
    std::map<std::uint64_t, PendingRead> reads_;
    std::shared_ptr<const clipboard_files::LocalFileList> files_;
    std::map<std::uint32_t, std::shared_ptr<const clipboard_files::LocalFileList>> locked_;
};

}  // namespace farland::server
