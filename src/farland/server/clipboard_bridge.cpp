// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/channels/clipboard_formats.hpp>
#include <farland/codec/dib.hpp>
#include <farland/codec/png.hpp>
#include <farland/server/clipboard_bridge.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>
#include <utility>

namespace farland::server {

namespace {

namespace cliprdr = channels::cliprdr;
namespace formats = channels::clipboard;
namespace ev = cliprdr::server_event;
using namespace std::string_view_literals;

constexpr std::string_view log_component = "server.clipboard";
/// IDs of the named formats farland announces (the client maps them by name).
constexpr std::uint32_t html_id = 0xD010;
constexpr std::uint32_t png_id = 0xD011;
constexpr std::uint32_t files_id = 0xD012;

/// Desktop text types, the preferred first.
constexpr std::array text_mimes = {"text/plain;charset=utf-8"sv, "UTF8_STRING"sv, "text/plain"sv,
                                   "text/plain;charset=UTF-8"sv, "STRING"sv,      "TEXT"sv};
constexpr std::array bmp_mimes = {"image/bmp"sv, "image/x-bmp"sv, "image/x-MS-bmp"sv};
constexpr std::string_view png_mime = "image/png";
constexpr std::string_view html_mime = "text/html";
constexpr std::string_view uri_list_mime = "text/uri-list";
constexpr std::string_view gnome_files_mime = "x-special/gnome-copied-files";
/// What the desktop gets offered for each kind of client data.
constexpr std::array offered_text = {"text/plain;charset=utf-8"sv, "text/plain"sv};
constexpr std::array offered_files = {uri_list_mime, gnome_files_mime};

enum class Kind : std::uint8_t { text, html, png, bmp, files };

std::optional<Kind> kind_of(std::string_view mime)
{
    if (std::ranges::find(text_mimes, mime) != text_mimes.end()) {
        return Kind::text;
    }
    if (mime == html_mime) {
        return Kind::html;
    }
    if (mime == png_mime) {
        return Kind::png;
    }
    if (std::ranges::find(bmp_mimes, mime) != bmp_mimes.end()) {
        return Kind::bmp;
    }
    if (mime == uri_list_mime || mime == gnome_files_mime) {
        return Kind::files;
    }
    return std::nullopt;
}

bool contains(const std::vector<std::string>& mimes, std::string_view mime)
{
    return std::ranges::find(mimes, mime) != mimes.end();
}

template <std::size_t N>
std::optional<std::string> first_of(const std::vector<std::string>& mimes,
                                    const std::array<std::string_view, N>& wanted)
{
    for (const auto mime : wanted) {
        if (contains(mimes, mime)) {
            return std::string(mime);
        }
    }
    return std::nullopt;
}

std::vector<std::byte> to_bytes(std::string_view text)
{
    const auto bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}

bool is_png(std::span<const std::byte> data)
{
    constexpr std::array<std::uint8_t, 8> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data.size() >= signature.size() &&
           std::ranges::equal(data.first(signature.size()), std::as_bytes(std::span(signature)));
}

std::string describe(const std::vector<cliprdr::Format>& list)
{
    std::string text;
    for (const auto& format : list) {
        text += text.empty() ? "" : ", ";
        text += format.name.empty() ? std::format("{}", format.id) : format.name;
    }
    return text.empty() ? "nothing" : text;
}

std::string describe(const std::vector<std::string>& mimes)
{
    std::string text;
    for (const auto& mime : mimes) {
        text += text.empty() ? "" : ", ";
        text += mime;
    }
    return text.empty() ? "nothing" : text;
}

}  // namespace

/// Fetching the files of a FileGroupDescriptorW into a staging directory.
struct ClipboardBridge::Download {
    Download(std::uint64_t epoch_, std::vector<cliprdr::FileDescriptor> files_,
             std::vector<std::vector<std::string>> components_, clipboard_files::StagingDirectory directory_)
        : epoch(epoch_), files(std::move(files_)), components(std::move(components_)), directory(std::move(directory_))
    {
    }

    std::uint64_t epoch = 0;
    std::vector<cliprdr::FileDescriptor> files;
    std::vector<std::vector<std::string>> components;
    clipboard_files::StagingDirectory directory;
    std::optional<std::uint32_t> clip_data_id;
    std::size_t index = 0;
    bool file_open = false;
    std::optional<std::uint64_t> size;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    std::optional<std::uint32_t> stream;
    bool size_request = false;
};

ClipboardBridge::ClipboardBridge(platform::Clipboard& desktop, Send send, ClipboardOptions options)
    : desktop_(desktop), send_(std::move(send)), options_(std::move(options)),
      reassembler_(options_.max_data_size + 1024), server_(options_.protocol)
{
    if (!options_.files) {
        options_.protocol.general_flags &=
            ~(cliprdr::general_flag::stream_fileclip_enabled | cliprdr::general_flag::can_lock_clipdata);
        server_ = cliprdr::ClipboardServer(options_.protocol);
    }
}

ClipboardBridge::~ClipboardBridge()
{
    fail_transfers();
}

void ClipboardBridge::start()
{
    desktop_mimes_ = desktop_.mime_types();
    server_.start();
    flush();
}

void ClipboardBridge::flush()
{
    for (const auto& message : server_.take_output()) {
        for (const auto& chunk : channels::svc::encode_chunks(message)) {
            send_(chunk);
        }
    }
}

Result<void> ClipboardBridge::receive(std::span<const std::byte> chunk, Clock::time_point now)
{
    if (failed_) {
        return {};
    }
    now_ = now;
    auto message = reassembler_.add(chunk);
    Result<void> result;
    if (!message) {
        result = std::unexpected(message.error());
    } else if (*message) {
        result = server_.receive(**message, now);
    }
    if (!result) {
        failed_ = true;
        fail_transfers();
        return result;
    }
    process();
    return {};
}

void ClipboardBridge::service(Clock::time_point now)
{
    now_ = now;
    desktop_.dispatch();
    while (auto event = desktop_.poll_event()) {
        handle(*event);
    }
    if (failed_) {
        fail_transfers();
        return;
    }
    server_.tick(now);
    process();
}

void ClipboardBridge::process()
{
    while (auto event = server_.poll_event()) {
        handle(*event);
    }
    pump_transfers();
    advance_download();
    flush();
}

void ClipboardBridge::handle(cliprdr::ServerEvent& event)
{
    std::visit(
        [this](auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ev::Ready>) {
                log::info(log_component, "clipboard ready (client flags {:#x})", e.flags);
            } else if constexpr (std::is_same_v<T, ev::RemoteFormatList>) {
                on_remote_formats(std::move(e.formats));
            } else if constexpr (std::is_same_v<T, ev::FormatListAnswered>) {
                if (!e.ok) {
                    log::info(log_component, "the client refused the desktop's clipboard");
                }
            } else if constexpr (std::is_same_v<T, ev::DataRequested>) {
                on_data_requested(e.id, e.format_id);
            } else if constexpr (std::is_same_v<T, ev::FileContentsRequested>) {
                on_file_contents_requested(e.id, e.request);
            } else if constexpr (std::is_same_v<T, ev::DataReceived>) {
                on_data_received(std::move(e.data));
            } else if constexpr (std::is_same_v<T, ev::FileContentsReceived>) {
                on_file_contents_received(e.stream_id, std::move(e.data));
            } else if constexpr (std::is_same_v<T, ev::Locked>) {
                if (files_) {
                    locked_[e.clip_data_id] = files_;
                }
            } else if constexpr (std::is_same_v<T, ev::Unlocked>) {
                locked_.erase(e.clip_data_id);
            }
        },
        event);
}

void ClipboardBridge::handle(platform::ClipboardEvent& event)
{
    namespace pev = platform::clipboard_event;
    if (auto* owner = std::get_if<pev::OwnerChanged>(&event)) {
        log::info(log_component, "the desktop copied {}", describe(owner->mime_types));
        desktop_mimes_ = std::move(owner->mime_types);
        if (!failed_ && server_.ready() && !first_remote_list_) {
            announce_desktop(*desktop_mimes_);
        }
    } else if (auto* transfer = std::get_if<pev::TransferRequested>(&event)) {
        transfers_.push_back(Transfer{transfer->serial, std::move(transfer->mime_type)});
    } else if (auto* read = std::get_if<pev::ReadFinished>(&event)) {
        on_read_finished(read->id, std::move(read->data));
    }
}

// --- The desktop's clipboard, pasted on the client

void ClipboardBridge::announce_desktop(const std::vector<std::string>& mime_types)
{
    std::vector<cliprdr::Format> list;
    if (first_of(mime_types, text_mimes)) {
        list.push_back({cliprdr::cf::unicode_text, ""});
    }
    if (contains(mime_types, html_mime)) {
        list.push_back({html_id, cliprdr::html_format});
    }
    const bool png = codec::png_supported() && contains(mime_types, png_mime);
    if (png || first_of(mime_types, bmp_mimes)) {
        list.push_back({cliprdr::cf::dib, ""});
        list.push_back({cliprdr::cf::dibv5, ""});
    }
    if (contains(mime_types, png_mime)) {
        list.push_back({png_id, cliprdr::png_format});
    }
    if (options_.files && server_.has(cliprdr::general_flag::stream_fileclip_enabled) &&
        first_of(mime_types, offered_files)) {
        list.push_back({files_id, cliprdr::file_group_descriptor_w});
    }
    files_.reset();
    server_.announce(std::move(list));
}

void ClipboardBridge::on_data_requested(std::uint64_t id, std::uint32_t format_id)
{
    const std::vector<std::string> none;
    const auto& mimes = desktop_mimes_ ? *desktop_mimes_ : none;
    std::optional<std::string> mime;
    if (format_id == cliprdr::cf::unicode_text) {
        mime = first_of(mimes, text_mimes);
    } else if (format_id == html_id && contains(mimes, html_mime)) {
        mime = std::string(html_mime);
    } else if (format_id == cliprdr::cf::dib || format_id == cliprdr::cf::dibv5) {
        mime = codec::png_supported() && contains(mimes, png_mime) ? std::optional<std::string>(png_mime)
                                                                   : first_of(mimes, bmp_mimes);
    } else if (format_id == png_id && contains(mimes, png_mime)) {
        mime = std::string(png_mime);
    } else if (format_id == files_id && options_.files) {
        mime = first_of(mimes, offered_files);
    }
    if (!mime) {
        log::debug(log_component, "the client pastes format {:#x}, which the desktop does not have", format_id);
        server_.answer(id, std::nullopt);
        return;
    }
    log::debug(log_component, "the client pastes format {:#x}: reading {}", format_id, *mime);
    const auto read = desktop_.read(*mime);
    reads_[read] = PendingRead{id, format_id, std::move(*mime)};
}

void ClipboardBridge::on_read_finished(std::uint64_t id, std::optional<std::vector<std::byte>> data)
{
    const auto found = reads_.find(id);
    if (found == reads_.end()) {
        return;
    }
    const auto read = std::move(found->second);
    reads_.erase(found);
    if (!data) {
        log::info(log_component, "the desktop did not give its {}", read.mime_type);
        server_.answer(read.request, std::nullopt);
        return;
    }
    auto converted = for_client(read, *data);
    if (!converted || converted->size() > options_.max_data_size) {
        log::info(log_component, "cannot hand {} to the client: {}", read.mime_type,
                  converted ? "too large" : converted.error().message());
        server_.answer(read.request, std::nullopt);
        return;
    }
    log::debug(log_component, "the client gets {} bytes of format {:#x}", converted->size(), read.format_id);
    server_.answer(read.request, *converted);
}

Result<std::vector<std::byte>> ClipboardBridge::for_client(const PendingRead& read, std::span<const std::byte> data)
{
    const std::uint64_t max_pixels = options_.max_data_size / 4;
    switch (read.format_id) {
    case cliprdr::cf::unicode_text:
        return formats::utf8_to_unicode_text(formats::local_text_to_utf8(data));
    case html_id:
        return formats::html_to_cf_html(data);
    case cliprdr::cf::dib:
    case cliprdr::cf::dibv5: {
        const auto header = read.format_id == cliprdr::cf::dibv5 ? codec::DibHeader::v5 : codec::DibHeader::info;
        if (read.mime_type == png_mime) {
            FARLAND_TRY(const auto image, codec::decode_png(data, max_pixels));
            return codec::encode_dib(image, header);
        }
        FARLAND_TRY(auto dib, codec::bmp_to_dib(data));
        if (header == codec::DibHeader::info) {
            return dib;
        }
        FARLAND_TRY(const auto image, codec::decode_dib(dib));
        return codec::encode_dib(image, header);
    }
    case png_id:
        return std::vector<std::byte>(data.begin(), data.end());
    case files_id: {
        const auto paths = clipboard_files::parse_uri_list(formats::local_text_to_utf8(data));
        FARLAND_TRY(auto list, clipboard_files::LocalFileList::from_paths(paths));
        if (list.descriptors().empty()) {
            return fail(Errc::invalid_value, "no files the client can take");
        }
        log::info(log_component, "offering {} files and directories to the client", list.descriptors().size());
        files_ = std::make_shared<const clipboard_files::LocalFileList>(std::move(list));
        return cliprdr::encode_file_list(files_->descriptors());
    }
    default:
        return fail(Errc::invalid_value, "unknown clipboard format");
    }
}

void ClipboardBridge::on_file_contents_requested(std::uint64_t id, const cliprdr::FileContentsRequest& request)
{
    auto list = files_;
    if (request.clip_data_id) {
        if (const auto locked = locked_.find(*request.clip_data_id); locked != locked_.end()) {
            list = locked->second;
        }
    }
    log::debug(log_component, "the client asks for file {} ({} at {}, {} bytes)", request.index,
               request.flags == cliprdr::file_contents::size ? "size" : "range", request.position, request.requested);
    if (!list) {
        server_.answer(id, std::nullopt);
        return;
    }
    if (request.flags == cliprdr::file_contents::size) {
        const auto size = list->size(request.index);
        if (!size) {
            server_.answer(id, std::nullopt);
            return;
        }
        Writer w(8);
        w.u64le(*size);
        server_.answer(id, w.view());
        return;
    }
    const auto data =
        list->read(request.index, request.position, std::min<std::size_t>(request.requested, options_.max_read_size));
    if (!data) {
        log::info(log_component, "cannot read file {} for the client: {}", request.index, data.error().message());
        server_.answer(id, std::nullopt);
        return;
    }
    server_.answer(id, *data);
}

// --- The client's clipboard, pasted on the desktop

std::optional<std::uint32_t> ClipboardBridge::remote_id(std::string_view name) const
{
    const auto found = std::ranges::find(remote_formats_, name, &cliprdr::Format::name);
    if (found == remote_formats_.end()) {
        return std::nullopt;
    }
    return found->id;
}

bool ClipboardBridge::remote_has(std::uint32_t id) const
{
    return std::ranges::any_of(remote_formats_, [id](const auto& f) { return f.id == id && f.name.empty(); });
}

void ClipboardBridge::on_remote_formats(std::vector<cliprdr::Format> list)
{
    log::info(log_component, "the client copied {}", describe(list));
    remote_formats_ = std::move(list);
    ++remote_epoch_;
    const bool first = std::exchange(first_remote_list_, false);
    if (first && remote_formats_.empty()) {
        // The client's clipboard is empty: offer the desktop's.
        if (desktop_mimes_) {
            announce_desktop(*desktop_mimes_);
        }
        return;
    }
    std::vector<std::string> mimes;
    if (remote_has(cliprdr::cf::unicode_text) || remote_has(cliprdr::cf::text) || remote_has(cliprdr::cf::oem_text)) {
        mimes.insert(mimes.end(), offered_text.begin(), offered_text.end());
    }
    if (remote_id(cliprdr::html_format)) {
        mimes.emplace_back(html_mime);
    }
    const bool dib = remote_has(cliprdr::cf::dib) || remote_has(cliprdr::cf::dibv5);
    if (remote_id(cliprdr::png_format) || remote_id(png_mime) || (dib && codec::png_supported())) {
        mimes.emplace_back(png_mime);
    }
    if (dib) {
        mimes.emplace_back(bmp_mimes[0]);
    }
    if (options_.files && server_.has(cliprdr::general_flag::stream_fileclip_enabled) &&
        remote_id(cliprdr::file_group_descriptor_w)) {
        mimes.insert(mimes.end(), offered_files.begin(), offered_files.end());
    }
    if (mimes.empty()) {
        return;  // nothing the desktop could take; it keeps what it has
    }
    files_.reset();
    desktop_mimes_.reset();
    desktop_.set_selection(mimes);
}

std::optional<std::uint32_t> ClipboardBridge::source_format(const std::string& mime_type) const
{
    const auto kind = kind_of(mime_type);
    if (!kind) {
        return std::nullopt;
    }
    const auto standard = [this](std::initializer_list<std::uint32_t> ids) -> std::optional<std::uint32_t> {
        for (const auto id : ids) {
            if (remote_has(id)) {
                return id;
            }
        }
        return std::nullopt;
    };
    switch (*kind) {
    case Kind::text:
        return standard({cliprdr::cf::unicode_text, cliprdr::cf::text, cliprdr::cf::oem_text});
    case Kind::html:
        return remote_id(cliprdr::html_format);
    case Kind::png:
        if (auto id = remote_id(cliprdr::png_format)) {
            return id;
        }
        if (auto id = remote_id(png_mime)) {
            return id;
        }
        return codec::png_supported() ? standard({cliprdr::cf::dibv5, cliprdr::cf::dib}) : std::nullopt;
    case Kind::bmp:
        return standard({cliprdr::cf::dib, cliprdr::cf::dibv5});
    case Kind::files:
        return options_.files ? remote_id(cliprdr::file_group_descriptor_w) : std::nullopt;
    }
    return std::nullopt;
}

void ClipboardBridge::pump_transfers()
{
    while (!active_ && !transfers_.empty()) {
        const auto transfer = transfers_.front();
        if (!server_.ready()) {
            transfers_.pop_front();
            desktop_.write(transfer.serial, std::nullopt);
            continue;
        }
        const auto format = source_format(transfer.mime_type);
        if (!format) {
            transfers_.pop_front();
            desktop_.write(transfer.serial, std::nullopt);
            continue;
        }
        if (kind_of(transfer.mime_type) == Kind::files) {
            if (staged_ && staged_->epoch == remote_epoch_) {
                transfers_.pop_front();
                desktop_.write(transfer.serial, staged_uris(transfer.mime_type));
                continue;
            }
            if (download_) {
                transfers_.pop_front();
                waiting_for_files_.push_back(transfer);
                continue;
            }
        }
        if (!server_.request_data(*format, now_)) {
            return;  // a request is outstanding; try again when it is done
        }
        transfers_.pop_front();
        log::debug(log_component, "the desktop pastes {}: asking the client for format {:#x}", transfer.mime_type,
                   *format);
        active_ = ActiveTransfer{transfer, *format};
    }
}

void ClipboardBridge::on_data_received(std::optional<std::vector<std::byte>> data)
{
    if (!active_) {
        return;
    }
    const auto active = std::move(*active_);
    active_.reset();
    if (!data) {
        log::info(log_component, "the client did not give its clipboard data for {}", active.transfer.mime_type);
        desktop_.write(active.transfer.serial, std::nullopt);
        return;
    }
    if (kind_of(active.transfer.mime_type) == Kind::files) {
        start_download(active.transfer, *data);
        return;
    }
    auto converted = for_desktop(active, *data);
    if (!converted) {
        log::info(log_component, "cannot hand the client's data to the desktop as {}: {}", active.transfer.mime_type,
                  converted.error().message());
        desktop_.write(active.transfer.serial, std::nullopt);
        return;
    }
    log::debug(log_component, "the desktop gets {} bytes of {}", converted->size(), active.transfer.mime_type);
    desktop_.write(active.transfer.serial, std::move(*converted));
}

Result<std::vector<std::byte>> ClipboardBridge::for_desktop(const ActiveTransfer& active,
                                                            std::span<const std::byte> data)
{
    switch (kind_of(active.transfer.mime_type).value_or(Kind::text)) {
    case Kind::text:
        return to_bytes(active.format_id == cliprdr::cf::unicode_text ? formats::unicode_text_to_utf8(data)
                                                                      : formats::ansi_text_to_utf8(data));
    case Kind::html: {
        FARLAND_TRY(const auto html, formats::cf_html_to_html(data));
        return to_bytes(html);
    }
    case Kind::png: {
        if (active.format_id != cliprdr::cf::dib && active.format_id != cliprdr::cf::dibv5) {
            if (!is_png(data)) {
                return fail(Errc::invalid_value, "the client's PNG is not one");
            }
            return std::vector<std::byte>(data.begin(), data.end());
        }
        FARLAND_TRY(const auto image, codec::decode_dib(data));
        return codec::encode_png(image);
    }
    case Kind::bmp:
        return codec::dib_to_bmp(data);
    case Kind::files:
        break;
    }
    return fail(Errc::invalid_value, "unexpected clipboard data");
}

void ClipboardBridge::start_download(const Transfer& transfer, std::span<const std::byte> file_list)
{
    auto files = cliprdr::decode_file_list(file_list);
    auto root =
        options_.staging_root.empty() ? clipboard_files::default_staging_root() : std::optional(options_.staging_root);
    const auto refuse = [&](std::string_view why) {
        log::info(log_component, "not pasting the client's files: {}", why);
        desktop_.write(transfer.serial, std::nullopt);
    };
    if (!files) {
        refuse(files.error().message());
        return;
    }
    if (files->empty() || files->size() > options_.max_staged_files) {
        refuse(files->empty() ? "no files" : "too many files");
        return;
    }
    if (!root) {
        refuse("XDG_RUNTIME_DIR is not set");
        return;
    }
    std::vector<std::vector<std::string>> components;
    std::uint64_t declared = 0;
    for (const auto& file : *files) {
        auto split = clipboard_files::split_client_name(file.name);
        if (!split) {
            refuse(split.error().message());
            return;
        }
        components.push_back(std::move(*split));
        if ((file.flags & cliprdr::fd_flag::file_size) != 0) {
            declared += file.size;
        }
    }
    if (declared > options_.max_staged_bytes) {
        refuse("the files are larger than the staging limit");
        return;
    }
    auto directory = clipboard_files::StagingDirectory::create(*root);
    if (!directory) {
        refuse(directory.error().message());
        return;
    }
    auto download =
        std::make_unique<Download>(remote_epoch_, std::move(*files), std::move(components), std::move(*directory));
    if (server_.has(cliprdr::general_flag::can_lock_clipdata)) {
        download->clip_data_id = next_clip_data_id_++;
        server_.lock(*download->clip_data_id);
    }
    log::info(log_component, "fetching {} files and directories from the client into {}", download->files.size(),
              download->directory.path().string());
    download_ = std::move(download);
    waiting_for_files_.push_back(transfer);
    advance_download();
}

void ClipboardBridge::advance_download()
{
    if (!download_ || download_->stream) {
        return;
    }
    auto& d = *download_;
    while (d.index < d.files.size()) {
        const auto& file = d.files[d.index];
        const auto& components = d.components[d.index];
        const auto index = static_cast<std::int32_t>(d.index);
        if ((file.attributes & cliprdr::file_attribute::directory) != 0) {
            if (!d.directory.make_directory(components)) {
                finish_download(false);
                return;
            }
            ++d.index;
            continue;
        }
        if (!d.file_open) {
            if (!d.directory.create_file(components)) {
                finish_download(false);
                return;
            }
            d.file_open = true;
            d.offset = 0;
            d.size.reset();
            if ((file.flags & cliprdr::fd_flag::file_size) != 0) {
                d.size = file.size;
            }
        }
        if (!d.size) {
            d.stream = server_.request_file_contents(index, cliprdr::file_contents::size, 0, 0, d.clip_data_id, now_);
            d.size_request = true;
            return;  // nullopt: a request is outstanding; the next call asks again
        }
        if (d.offset >= *d.size) {
            d.directory.close_file();
            d.file_open = false;
            ++d.index;
            continue;
        }
        const auto length =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(options_.range_size, *d.size - d.offset));
        if (d.offset + length > 0xFFFFFFFFULL && !server_.has(cliprdr::general_flag::huge_file_support_enabled)) {
            finish_download(false);  // the client cannot serve beyond 4 GiB
            return;
        }
        d.stream =
            server_.request_file_contents(index, cliprdr::file_contents::range, d.offset, length, d.clip_data_id, now_);
        d.size_request = false;
        return;
    }
    finish_download(true);
}

void ClipboardBridge::on_file_contents_received(std::uint32_t stream_id, std::optional<std::vector<std::byte>> data)
{
    if (!download_ || download_->stream != stream_id) {
        return;
    }
    auto& d = *download_;
    d.stream.reset();
    if (!data) {
        log::info(log_component, "the client did not give file {}", d.files[d.index].name);
        finish_download(false);
        return;
    }
    if (d.size_request) {
        Reader r(*data);
        d.size = r.u64le().value_or(0);
    } else {
        if (data->empty() || !d.size || d.offset + data->size() > *d.size) {
            finish_download(false);
            return;
        }
        if (!d.directory.append(*data)) {
            finish_download(false);
            return;
        }
        d.offset += data->size();
        d.total += data->size();
    }
    if (d.total + (*d.size - std::min(*d.size, d.offset)) > options_.max_staged_bytes) {
        log::info(log_component, "not pasting the client's files: larger than the staging limit");
        finish_download(false);
        return;
    }
    advance_download();
}

void ClipboardBridge::finish_download(bool ok)
{
    auto download = std::move(download_);
    if (download->clip_data_id) {
        server_.unlock(*download->clip_data_id);
    }
    download->directory.close_file();
    const auto waiting = std::exchange(waiting_for_files_, {});
    if (!ok) {
        for (const auto& transfer : waiting) {
            desktop_.write(transfer.serial, std::nullopt);
        }
        return;
    }
    Staged staged{download->epoch, {}};
    for (const auto& components : download->components) {
        auto item = download->directory.path() / components.front();
        if (std::ranges::find(staged.items, item) == staged.items.end()) {
            staged.items.push_back(std::move(item));
        }
    }
    log::info(log_component, "the client's files are staged in {} ({} bytes)", download->directory.path().string(),
              download->total);
    staged_ = std::move(staged);
    staging_dirs_.push_back(std::move(download->directory));
    while (staging_dirs_.size() > 2) {
        staging_dirs_.pop_front();  // removes that paste's files
    }
    for (const auto& transfer : waiting) {
        desktop_.write(transfer.serial, staged_uris(transfer.mime_type));
    }
}

std::vector<std::byte> ClipboardBridge::staged_uris(const std::string& mime_type) const
{
    if (!staged_) {
        return {};
    }
    std::string text;
    if (mime_type == gnome_files_mime) {
        text = "copy";
        for (const auto& item : staged_->items) {
            text += "\n" + clipboard_files::file_uri(item);
        }
    } else {
        for (const auto& item : staged_->items) {
            text += clipboard_files::file_uri(item) + "\r\n";
        }
    }
    return to_bytes(text);
}

void ClipboardBridge::fail_transfers()
{
    if (active_) {
        desktop_.write(active_->transfer.serial, std::nullopt);
        active_.reset();
    }
    for (const auto& transfer : std::exchange(transfers_, {})) {
        desktop_.write(transfer.serial, std::nullopt);
    }
    for (const auto& transfer : std::exchange(waiting_for_files_, {})) {
        desktop_.write(transfer.serial, std::nullopt);
    }
    download_.reset();
}

}  // namespace farland::server
