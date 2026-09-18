// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/dmabuf_reader.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>
#include <farland/platform/portal/pipewire_util.hpp>
#include <farland/platform/portal/pixel_formats.hpp>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <limits>
#include <linux/dma-buf.h>
#include <mutex>
#include <set>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace farland::platform::portal {

namespace {

constexpr std::string_view log_component = "platform.pipewire";

/// RDP large pointers go up to 384 x 384 (MS-RDPBCGR 2.2.7.2.11), so larger
/// cursor bitmaps are of no use.
constexpr std::uint32_t max_cursor_size = 384;
/// Room for the EnumFormat params: every format twice with a requested size.
constexpr std::size_t format_pod_capacity = std::size_t{64} * 1024;
/// Damage rectangles the producer may send per buffer.
constexpr std::int32_t damage_regions_default = 16;
constexpr std::int32_t damage_regions_max = 64;
/// How long create() waits for the PipeWire daemon to answer.
constexpr int connect_timeout_seconds = 5;
/// Dmabufs are passed on only when the stream has at least this many
/// buffers: the capture keeps up to two dequeued (the one the session holds
/// and the newest pending), and the producer needs the rest to go on.
constexpr std::size_t min_buffers_to_hold = 4;

constexpr std::int32_t cursor_meta_size(std::uint32_t width, std::uint32_t height)
{
    return static_cast<std::int32_t>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) +
                                     (std::size_t{width} * height * 4U));
}

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        std::swap(fd_, other.fd_);
        return *this;
    }
    ~UniqueFd()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

void wake(const UniqueFd& fd) noexcept
{
    static_cast<void>(::eventfd_write(fd.get(), 1));
}

void drain(const UniqueFd& fd) noexcept
{
    eventfd_t value = 0;
    static_cast<void>(::eventfd_read(fd.get(), &value));
}

/// The SPA metadata of `type` as bytes; empty if the buffer has none.
std::span<const std::byte> find_meta(const spa_buffer* buffer, std::uint32_t type) noexcept
{
    const spa_meta* meta = spa_buffer_find_meta(buffer, type);
    if (meta == nullptr || meta->data == nullptr) {
        return {};
    }
    return {static_cast<const std::byte*>(meta->data), meta->size};
}

/// Copies a T out of `bytes` at `offset`, if it fits.
template <class T>
std::optional<T> read_struct(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return std::nullopt;
    }
    T value{};
    std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
    return value;
}

std::int32_t clamp_to_int32(std::uint32_t value) noexcept
{
    return static_cast<std::int32_t>(std::min<std::uint32_t>(value, std::numeric_limits<std::int32_t>::max()));
}

/// A buffer's planes, from spa_buffer::datas.
std::span<spa_data> planes_of(const spa_buffer* buffer) noexcept
{
    if (buffer->datas == nullptr) {
        return {};
    }
    return {buffer->datas, buffer->n_datas};
}

bool has_frame_data(const spa_buffer* buffer) noexcept
{
    const auto planes = planes_of(buffer);
    if (planes.empty() || planes[0].chunk == nullptr) {
        return false;
    }
    const spa_chunk& chunk = *planes[0].chunk;
    return chunk.size > 0 && (static_cast<std::uint32_t>(chunk.flags) & SPA_CHUNK_FLAG_CORRUPTED) == 0;
}

/// Our own CPU mapping of a LINEAR dmabuf, made on first use.
struct BufferSlot {
    void* map = nullptr;
    std::size_t map_size = 0;

    BufferSlot() = default;
    BufferSlot(const BufferSlot&) = delete;
    BufferSlot& operator=(const BufferSlot&) = delete;
    BufferSlot(BufferSlot&&) = delete;
    BufferSlot& operator=(BufferSlot&&) = delete;
    ~BufferSlot()
    {
        if (map != nullptr) {
            ::munmap(map, map_size);
        }
    }
};

/// Brackets CPU reads of a dmabuf (DMA_BUF_IOCTL_SYNC), so caches are coherent.
class DmabufReadAccess {
public:
    explicit DmabufReadAccess(int fd) noexcept : fd_(fd) { sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ); }
    ~DmabufReadAccess() { sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ); }
    DmabufReadAccess(const DmabufReadAccess&) = delete;
    DmabufReadAccess& operator=(const DmabufReadAccess&) = delete;
    DmabufReadAccess(DmabufReadAccess&&) = delete;
    DmabufReadAccess& operator=(DmabufReadAccess&&) = delete;

private:
    void sync(std::uint64_t flags) const noexcept
    {
        dma_buf_sync sync{};
        sync.flags = flags;
        int result = 0;
        do {
            result = ::ioctl(fd_, DMA_BUF_IOCTL_SYNC, &sync);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        } while (result == -1 && (errno == EINTR || errno == EAGAIN));
    }

    int fd_;
};

/// One pixel layout the capture offers, with the dmabuf modifiers it takes
/// for it (none: shared memory only).
struct FormatOffer {
    PixelLayout layout = PixelLayout::bgrx;
    std::vector<std::uint64_t> modifiers;
};

/// A dmabuf frame the capture keeps dequeued instead of reading it, with its
/// own duplicates of the descriptors: those stay valid even when PipeWire
/// removes the buffer meanwhile.
struct HeldBuffer {
    /// To requeue; null when there is none or PipeWire removed it.
    pw_buffer* buffer = nullptr;
    Dmabuf dmabuf;
    std::array<UniqueFd, 4> fds;
    Rect view;
};

}  // namespace

std::string_view to_string(CaptureState state) noexcept
{
    switch (state) {
    case CaptureState::connecting:
        return "connecting";
    case CaptureState::paused:
        return "paused";
    case CaptureState::streaming:
        return "streaming";
    case CaptureState::closed:
        return "closed";
    }
    return "unknown";
}

// One per capture, so field order follows meaning, not padding.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct PipeWireCapture::Impl {
    class Frames final : public FrameSource {
    public:
        explicit Frames(Impl& impl) noexcept : impl_(impl) {}
        [[nodiscard]] int wake_fd() const noexcept override { return impl_.frame_wake.get(); }
        [[nodiscard]] std::optional<Frame> take_frame() override { return impl_.take_frame(); }
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const override
        {
            const std::lock_guard lock(impl_.mutex);
            return impl_.size;
        }
        void set_access(FrameAccess wanted) override
        {
            const std::lock_guard lock(impl_.mutex);
            impl_.wanted_access = wanted;
        }
        [[nodiscard]] std::optional<codec::ImageView> map_frame() override { return impl_.map_frame(); }
        void release_frame() override { impl_.release_frame(); }

    private:
        Impl& impl_;
    };

    class Cursor final : public CursorSource {
    public:
        explicit Cursor(Impl& impl) noexcept : impl_(impl) {}
        [[nodiscard]] int wake_fd() const noexcept override { return impl_.cursor_wake.get(); }
        [[nodiscard]] std::optional<CursorUpdate> take_cursor() override { return impl_.take_cursor(); }

    private:
        Impl& impl_;
    };

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl();

    PipeWireCaptureOptions options;
    std::uint32_t target_node = SPA_ID_INVALID;
    Frames frames{*this};
    Cursor cursor{*this};
    UniqueFd frame_wake;
    UniqueFd cursor_wake;
    bool pw_initialized = false;

    // PipeWire objects: used on the loop thread or with the loop lock held.
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    pw_stream* stream = nullptr;
    spa_source* renegotiate_event = nullptr;
    /// Requeues the buffers in `returned`; signalled by the session thread.
    spa_source* release_event = nullptr;
    spa_hook core_listener{};
    spa_hook registry_listener{};
    spa_hook stream_listener{};
    int sync_seq = -1;
    bool sync_done = false;

    // Loop thread state.
    std::unique_ptr<DmabufReader> dmabuf;
    std::vector<FormatOffer> offers;
    /// request_size(); offered first, as a fixed size.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> requested_size;
    std::optional<spa_video_info_raw> format;
    PixelLayout layout = PixelLayout::bgrx;
    std::unordered_map<pw_buffer*, std::unique_ptr<BufferSlot>> slots;
    std::set<std::uint64_t> failed_modifiers;
    std::vector<std::byte> work;
    std::optional<Rect> published_view;
    std::uint64_t frames_received = 0;
    std::atomic<std::uint64_t> buffers_received{0};
    bool cursor_on_stream = false;
    bool cursor_has_shape = false;
    bool cursor_shape_empty = false;
    bool cursor_visible = false;
    std::optional<std::pair<std::int32_t, std::int32_t>> cursor_position;
    bool warned_bad_frame = false;
    bool warned_bad_cursor = false;
    bool warned_few_buffers = false;
    bool logged_held = false;

    // Shared between the loop thread and the session thread. Lock order: the
    // loop lock (held in every callback) before `mutex`.
    mutable std::mutex mutex;
    FrameAccess wanted_access = FrameAccess::cpu;
    std::vector<std::byte> pending;
    std::uint32_t pending_width = 0;
    std::uint32_t pending_height = 0;
    std::uint64_t pending_sequence = 0;
    bool frame_pending = false;
    /// The pending frame is `pending_held`, not the pixels in `pending`.
    bool pending_is_dmabuf = false;
    HeldBuffer pending_held;
    /// The frame the session took last, while it is a dmabuf.
    HeldBuffer front_held;
    /// Buffers the session gave back, for the loop thread to requeue.
    std::vector<pw_buffer*> returned;
    /// Bumped whenever PipeWire removes a buffer (Dmabuf::generation).
    std::uint64_t buffer_generation = 0;
    DamageAccumulator pending_damage;
    CursorUpdate pending_cursor;
    bool cursor_pending = false;
    CaptureState state = CaptureState::connecting;
    std::string error;
    std::pair<std::uint32_t, std::uint32_t> size{0, 0};

    // Session thread only.
    std::vector<std::byte> front;
    std::uint32_t front_width = 0;
    std::uint32_t front_height = 0;
    /// `front` holds the pixels of the frame taken last.
    bool front_has_pixels = false;
    /// The frame taken last is `front_held` and was not released.
    bool front_is_dmabuf = false;

    // --- Setup -------------------------------------------------------------

    void init_offers()
    {
        offers.clear();
        for (const PixelLayout offer_layout : all_pixel_layouts) {
            FormatOffer offer{offer_layout, {}};
            if (options.dmabuf) {
                offer.modifiers.push_back(drm_format_mod_linear);
                if (dmabuf != nullptr) {
                    for (const std::uint64_t modifier : dmabuf->modifiers(drm_fourcc(offer_layout))) {
                        if (std::ranges::find(offer.modifiers, modifier) == offer.modifiers.end()) {
                            offer.modifiers.push_back(modifier);
                        }
                    }
                }
            }
            offers.push_back(std::move(offer));
        }
    }

    /// A fixed size, or any size (1920x1080 preferred: what a virtual
    /// monitor gets unless a size was requested).
    void add_size_and_rate(pw::PodBuilder& b,
                           const std::optional<std::pair<std::uint32_t, std::uint32_t>>& fixed_size) const
    {
        const std::uint32_t max_fps = std::max<std::uint32_t>(options.max_framerate, 1);
        spa_pod_frame choice{};
        b.prop(SPA_FORMAT_VIDEO_size);
        if (fixed_size) {
            b.rectangle(fixed_size->first, fixed_size->second);
        } else {
            b.push_choice(&choice, SPA_CHOICE_Range);
            b.rectangle(1920, 1080);
            b.rectangle(1, 1);
            b.rectangle(16384, 16384);
            static_cast<void>(b.pop(&choice));
        }
        // Screen casts run at a variable rate (0/1), up to maxFramerate.
        b.prop(SPA_FORMAT_VIDEO_framerate);
        b.push_choice(&choice, SPA_CHOICE_Range);
        b.fraction(0, 1);
        b.fraction(0, 1);
        b.fraction(max_fps, 1);
        static_cast<void>(b.pop(&choice));
        b.prop(SPA_FORMAT_VIDEO_maxFramerate);
        b.push_choice(&choice, SPA_CHOICE_Range);
        b.fraction(max_fps, 1);
        b.fraction(1, 1);
        b.fraction(max_fps, 1);
        static_cast<void>(b.pop(&choice));
    }

    /// EnumFormat params: with a requested size, every format at that size
    /// first (PipeWire takes the consumer's formats in order), then every
    /// format at any size.
    [[nodiscard]] std::vector<const spa_pod*> build_formats(pw::PodBuilder& b) const
    {
        std::vector<const spa_pod*> params;
        if (requested_size) {
            add_formats(b, params, requested_size);
        }
        add_formats(b, params, std::nullopt);
        return params;
    }

    /// One param per layout with its dmabuf modifiers (preferred), then one
    /// for shared memory with every layout.
    void add_formats(pw::PodBuilder& b, std::vector<const spa_pod*>& params,
                     const std::optional<std::pair<std::uint32_t, std::uint32_t>>& fixed_size) const
    {
        for (const FormatOffer& offer : offers) {
            if (offer.modifiers.empty()) {
                continue;
            }
            spa_pod_frame object{};
            spa_pod_frame choice{};
            b.push_object(&object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            b.prop(SPA_FORMAT_mediaType);
            b.id(SPA_MEDIA_TYPE_video);
            b.prop(SPA_FORMAT_mediaSubtype);
            b.id(SPA_MEDIA_SUBTYPE_raw);
            b.prop(SPA_FORMAT_VIDEO_format);
            b.id(to_spa_format(offer.layout));
            // The producer picks the modifier (DONT_FIXATE), as the PipeWire
            // dmabuf negotiation describes.
            b.prop(SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            b.push_choice(&choice, SPA_CHOICE_Enum);
            b.long_(static_cast<std::int64_t>(offer.modifiers.front()));
            for (const std::uint64_t modifier : offer.modifiers) {
                b.long_(static_cast<std::int64_t>(modifier));
            }
            static_cast<void>(b.pop(&choice));
            add_size_and_rate(b, fixed_size);
            params.push_back(b.pop(&object));
        }
        spa_pod_frame object{};
        spa_pod_frame choice{};
        b.push_object(&object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        b.prop(SPA_FORMAT_mediaType);
        b.id(SPA_MEDIA_TYPE_video);
        b.prop(SPA_FORMAT_mediaSubtype);
        b.id(SPA_MEDIA_SUBTYPE_raw);
        b.prop(SPA_FORMAT_VIDEO_format);
        b.push_choice(&choice, SPA_CHOICE_Enum);
        b.id(to_spa_format(all_pixel_layouts.front()));
        for (const PixelLayout offer_layout : all_pixel_layouts) {
            b.id(to_spa_format(offer_layout));
        }
        static_cast<void>(b.pop(&choice));
        add_size_and_rate(b, fixed_size);
        params.push_back(b.pop(&object));
    }

    // --- State ---------------------------------------------------------------

    void set_state(CaptureState new_state)
    {
        const std::lock_guard lock(mutex);
        if (state != CaptureState::closed) {
            state = new_state;
        }
    }

    /// Final. Leaves both wake fds readable.
    void close(std::string reason)
    {
        {
            const std::lock_guard lock(mutex);
            if (state == CaptureState::closed) {
                return;
            }
            state = CaptureState::closed;
            error = std::move(reason);
            log::info(log_component, "stream closed: {}", error);
        }
        wake(frame_wake);
        wake(cursor_wake);
        if (loop != nullptr) {
            pw_thread_loop_signal(loop, false);
        }
    }

    // --- Callbacks (loop thread) --------------------------------------------

    static void on_core_done(void* data, std::uint32_t id, int seq)
    {
        auto& self = *static_cast<Impl*>(data);
        if (id == PW_ID_CORE && seq == self.sync_seq) {
            self.sync_done = true;
            pw_thread_loop_signal(self.loop, false);
        }
    }

    static void on_core_error(void* data, std::uint32_t id, int /*seq*/, int res, const char* message)
    {
        auto& self = *static_cast<Impl*>(data);
        const std::string_view text = message != nullptr ? message : "";
        log::warn(log_component, "PipeWire error on object {}: {} ({})", id, text, std::strerror(-res));
        if (id == PW_ID_CORE && res == -EPIPE) {
            self.close(std::format("PipeWire connection lost: {}", text));
        }
    }

    static void on_global_remove(void* data, std::uint32_t id)
    {
        auto& self = *static_cast<Impl*>(data);
        if (id == self.target_node) {
            self.close("the screen cast node was removed");
        }
    }

    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state new_state, const char* message)
    {
        auto& self = *static_cast<Impl*>(data);
        log::debug(log_component, "stream state {} -> {}", pw_stream_state_as_string(old),
                   pw_stream_state_as_string(new_state));
        switch (new_state) {
        case PW_STREAM_STATE_ERROR:
            self.close(std::format("stream error: {}", message != nullptr ? message : "unknown"));
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            if (old != PW_STREAM_STATE_UNCONNECTED) {
                self.close("stream disconnected");
            }
            break;
        case PW_STREAM_STATE_CONNECTING:
            self.set_state(CaptureState::connecting);
            break;
        case PW_STREAM_STATE_PAUSED:
            self.set_state(CaptureState::paused);
            break;
        case PW_STREAM_STATE_STREAMING:
            self.set_state(CaptureState::streaming);
            break;
        }
    }

    static void on_param_changed(void* data, std::uint32_t id, const spa_pod* param)
    {
        auto& self = *static_cast<Impl*>(data);
        if (id == SPA_PARAM_Format) {
            self.format_changed(param);
        }
    }

    static void on_add_buffer(void* data, pw_buffer* buffer)
    {
        auto& self = *static_cast<Impl*>(data);
        self.slots[buffer] = std::make_unique<BufferSlot>();
    }

    /// Also for buffers the capture holds: PipeWire takes them back when it
    /// renegotiates, and they must not be requeued afterwards.
    static void on_remove_buffer(void* data, pw_buffer* buffer)
    {
        auto& self = *static_cast<Impl*>(data);
        {
            const std::lock_guard lock(self.mutex);
            ++self.buffer_generation;
            if (self.pending_held.buffer == buffer) {
                self.pending_held.buffer = nullptr;
            }
            if (self.front_held.buffer == buffer) {
                self.front_held.buffer = nullptr;
            }
            std::erase(self.returned, buffer);
        }
        self.slots.erase(buffer);
    }

    static void on_process(void* data) { static_cast<Impl*>(data)->process(); }

    static void on_release(void* data, std::uint64_t /*count*/)
    {
        auto& self = *static_cast<Impl*>(data);
        std::vector<pw_buffer*> buffers;
        {
            const std::lock_guard lock(self.mutex);
            buffers.swap(self.returned);
        }
        if (self.stream == nullptr) {
            return;
        }
        for (pw_buffer* buffer : buffers) {
            pw_stream_queue_buffer(self.stream, buffer);
        }
    }

    static void on_renegotiate(void* data, std::uint64_t /*count*/)
    {
        auto& self = *static_cast<Impl*>(data);
        if (self.stream == nullptr) {
            return;
        }
        pw::PodBuilder b(format_pod_capacity);
        auto params = self.build_formats(b);
        if (b.overflowed()) {
            self.close("format parameters do not fit");
            return;
        }
        log::info(log_component, "renegotiating the stream format");
        pw_stream_update_params(self.stream, params.data(), static_cast<std::uint32_t>(params.size()));
    }

    // --- Format ----------------------------------------------------------------

    void format_changed(const spa_pod* param)
    {
        format.reset();
        if (param == nullptr) {
            return;
        }
        std::uint32_t media_type = 0;
        std::uint32_t media_subtype = 0;
        spa_video_info_raw info{};
        if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
            media_subtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &info) < 0) {
            close("the producer chose a format that is not raw video");
            return;
        }
        const auto negotiated = layout_from_spa(info.format);
        if (!negotiated || info.size.width == 0 || info.size.height == 0) {
            close(std::format("unsupported video format {} at {}x{}", static_cast<std::uint32_t>(info.format),
                              info.size.width, info.size.height));
            return;
        }
        const bool uses_dmabuf = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
        format = info;
        layout = *negotiated;
        warned_few_buffers = false;
        logged_held = false;
        if (uses_dmabuf) {
            log::info(log_component, "format: {}x{}, spa format {}, dmabuf modifier {:#x}", info.size.width,
                      info.size.height, static_cast<std::uint32_t>(info.format), info.modifier);
        } else {
            log::info(log_component, "format: {}x{}, spa format {}, shared memory", info.size.width, info.size.height,
                      static_cast<std::uint32_t>(info.format));
        }

        pw::PodBuilder b(4096);
        std::vector<const spa_pod*> params;
        spa_pod_frame object{};
        spa_pod_frame choice{};
        b.push_object(&object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        b.int_range(SPA_PARAM_BUFFERS_buffers, 8, 2, 16);
        b.prop(SPA_PARAM_BUFFERS_dataType);
        b.push_choice(&choice, SPA_CHOICE_Flags);
        b.int_(uses_dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
        static_cast<void>(b.pop(&choice));
        params.push_back(b.pop(&object));
        constexpr auto header_size = static_cast<std::int32_t>(sizeof(spa_meta_header));
        constexpr auto region_size = static_cast<std::int32_t>(sizeof(spa_meta_region));
        params.push_back(b.meta(SPA_META_Header, header_size));
        params.push_back(b.meta(SPA_META_VideoCrop, region_size));
        params.push_back(b.meta_range(SPA_META_VideoDamage, region_size * damage_regions_default, region_size,
                                      region_size * damage_regions_max));
        params.push_back(b.meta_range(SPA_META_Cursor, cursor_meta_size(max_cursor_size, max_cursor_size),
                                      cursor_meta_size(1, 1), cursor_meta_size(max_cursor_size, max_cursor_size)));
        if (b.overflowed()) {
            close("buffer parameters do not fit");
            return;
        }
        pw_stream_update_params(stream, params.data(), static_cast<std::uint32_t>(params.size()));
    }

    /// The modifier of the current format does not work for CPU access: stop
    /// offering it and renegotiate (on the next loop iteration).
    void dmabuf_failed()
    {
        if (!format) {
            return;
        }
        const std::uint64_t modifier = format->modifier;
        if (!failed_modifiers.insert(modifier).second) {
            return;
        }
        log::warn(log_component, "cannot read dmabufs with modifier {:#x}; falling back", modifier);
        for (FormatOffer& offer : offers) {
            if (offer.layout == layout) {
                std::erase(offer.modifiers, modifier);
            }
        }
        pw::loop_signal_event(pw_thread_loop_get_loop(loop), renegotiate_event);
    }

    // --- Frames ----------------------------------------------------------------

    void process()
    {
        if (stream == nullptr) {
            return;
        }
        pw_buffer* latest = nullptr;
        Rect latest_view{};
        bool view_changed = false;
        std::uint64_t handled = 0;
        DamageAccumulator damage;
        while (pw_buffer* buffer = pw_stream_dequeue_buffer(stream)) {
            ++handled;
            const spa_buffer* spa = buffer->buffer;
            const auto header = read_struct<spa_meta_header>(find_meta(spa, SPA_META_Header), 0);
            if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) != 0) {
                pw_stream_queue_buffer(stream, buffer);
                continue;
            }
            if (log::enabled(log::Level::trace)) {
                const auto cursor_meta = read_struct<spa_meta_cursor>(find_meta(spa, SPA_META_Cursor), 0);
                log::trace(log_component, "buffer seq {} frame {} cursor id {}", header ? header->seq : 0,
                           has_frame_data(spa), cursor_meta ? cursor_meta->id : 0);
            }
            handle_cursor(spa);
            if (!format || !has_frame_data(spa)) {
                pw_stream_queue_buffer(stream, buffer);
                continue;
            }
            ++frames_received;
            const Rect bounds{0, 0, clamp_to_int32(format->size.width), clamp_to_int32(format->size.height)};
            collect_damage(spa, bounds, damage);
            const Rect view = read_view(spa, bounds);
            if (latest != nullptr) {
                view_changed = view_changed || view != latest_view;
                pw_stream_queue_buffer(stream, latest);
            }
            latest = buffer;
            latest_view = view;
        }
        if (latest != nullptr) {
            DamageAccumulator view_damage = damage_in_view(damage, latest_view, view_changed);
            bool held = false;
            if (!view_damage.empty()) {  // else only pixels outside the crop changed
                held = hold_frame(latest, latest_view, view_damage);
                if (!held) {
                    publish_frame(latest, latest_view, view_damage);
                }
            }
            if (!held) {
                pw_stream_queue_buffer(stream, latest);
            }
        }
        buffers_received.fetch_add(handled, std::memory_order_release);
    }

    /// SPA_META_VideoDamage: an invalid region or the end of the array ends the
    /// list; no metadata or no regions means everything changed.
    static void collect_damage(const spa_buffer* spa, const Rect& bounds, DamageAccumulator& damage)
    {
        const auto meta = find_meta(spa, SPA_META_VideoDamage);
        bool any = false;
        for (std::size_t offset = 0; offset + sizeof(spa_meta_region) <= meta.size();
             offset += sizeof(spa_meta_region)) {
            const auto region = read_struct<spa_meta_region>(meta, offset);
            if (!region || region->region.size.width == 0 || region->region.size.height == 0) {
                break;
            }
            any = true;
            const Rect rect{region->region.position.x, region->region.position.y,
                            clamp_to_int32(region->region.size.width), clamp_to_int32(region->region.size.height)};
            if (const auto clipped = intersect(rect, bounds)) {
                damage.add(*clipped);
            }
        }
        if (!any) {
            damage.add_full();
        }
    }

    /// The part of the buffer to show: SPA_META_VideoCrop if present and valid.
    static Rect read_view(const spa_buffer* spa, const Rect& bounds)
    {
        const auto crop = read_struct<spa_meta_region>(find_meta(spa, SPA_META_VideoCrop), 0);
        if (!crop || crop->region.size.width == 0 || crop->region.size.height == 0) {
            return bounds;
        }
        const Rect rect{crop->region.position.x, crop->region.position.y, clamp_to_int32(crop->region.size.width),
                        clamp_to_int32(crop->region.size.height)};
        return intersect(rect, bounds).value_or(bounds);
    }

    /// The damage in the coordinates of `view`: everything when the view
    /// changed.
    DamageAccumulator damage_in_view(DamageAccumulator& damage, const Rect& view, bool view_changed) const
    {
        DamageAccumulator view_damage;
        if (damage.full() || view_changed || published_view != view) {
            view_damage.add_full();
        } else {
            for (const Rect& rect : damage.take()) {
                if (const auto clipped = intersect(rect, view)) {
                    view_damage.add(Rect{clipped->x - view.x, clipped->y - view.y, clipped->width, clipped->height});
                }
            }
        }
        return view_damage;
    }

    /// Makes the pending frame, with `mutex` held: merges its damage, and
    /// gives back a dmabuf that was pending (returned for requeueing).
    [[nodiscard]] pw_buffer* set_pending_locked(std::uint32_t width, std::uint32_t height,
                                                DamageAccumulator& view_damage)
    {
        pw_buffer* superseded = std::exchange(pending_held.buffer, nullptr);
        pending_width = width;
        pending_height = height;
        pending_sequence = frames_received;
        if (view_damage.full()) {
            pending_damage.add_full();
        } else {
            for (const Rect& rect : view_damage.take()) {
                pending_damage.add(rect);
            }
        }
        frame_pending = true;
        size = {width, height};
        return superseded;
    }

    void publish_frame(pw_buffer* buffer, const Rect& view, DamageAccumulator& view_damage)
    {
        const auto width = static_cast<std::uint32_t>(view.width);
        const auto height = static_cast<std::uint32_t>(view.height);
        work.resize(std::size_t{width} * height * 4U);
        if (!read_pixels(buffer, view, work)) {
            return;
        }
        published_view = view;
        pw_buffer* superseded = nullptr;
        {
            const std::lock_guard lock(mutex);
            std::swap(pending, work);
            superseded = set_pending_locked(width, height, view_damage);
            pending_held = HeldBuffer{};
            pending_is_dmabuf = false;
        }
        if (superseded != nullptr) {
            pw_stream_queue_buffer(stream, superseded);
        }
        wake(frame_wake);
    }

    /// With FrameAccess::dmabuf: keeps a dmabuf buffer dequeued as the
    /// pending frame, without reading it. False when the frame is to be read
    /// instead: shared memory, a crop (encoders take whole buffers), too few
    /// buffers, or a consumer that wants pixels.
    bool hold_frame(pw_buffer* buffer, const Rect& view, DamageAccumulator& view_damage)
    {
        {
            const std::lock_guard lock(mutex);
            if (wanted_access != FrameAccess::dmabuf) {
                return false;
            }
        }
        const auto planes = planes_of(buffer->buffer);
        if (!format || (format->flags & SPA_VIDEO_FLAG_MODIFIER) == 0 || planes.empty() ||
            planes[0].type != SPA_DATA_DmaBuf) {
            return false;
        }
        const std::uint32_t width = format->size.width;
        const std::uint32_t height = format->size.height;
        if (view != Rect{0, 0, clamp_to_int32(width), clamp_to_int32(height)}) {
            return false;
        }
        if (slots.size() < min_buffers_to_hold) {
            if (!warned_few_buffers) {
                warned_few_buffers = true;
                log::warn(log_component, "the stream has only {} buffers; reading dmabufs instead of passing them on",
                          slots.size());
            }
            return false;
        }
        HeldBuffer held;
        held.buffer = buffer;
        held.view = view;
        held.dmabuf.drm_format = drm_fourcc(layout);
        held.dmabuf.modifier = format->modifier;
        held.dmabuf.width = width;
        held.dmabuf.height = height;
        for (const spa_data& data : planes) {
            if (data.type != SPA_DATA_DmaBuf || held.dmabuf.plane_count == held.fds.size() || data.chunk == nullptr) {
                break;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            const int fd = ::fcntl(static_cast<int>(data.fd), F_DUPFD_CLOEXEC, 3);
            if (fd < 0) {
                return false;
            }
            const std::uint32_t plane = held.dmabuf.plane_count++;
            held.fds.at(plane) = UniqueFd(fd);
            std::uint32_t pitch = plane == 0 ? width * 4U : 0U;
            if (data.chunk->stride > 0) {
                pitch = static_cast<std::uint32_t>(data.chunk->stride);
            }
            held.dmabuf.planes.at(plane) = DmabufPlane{fd, data.chunk->offset, pitch};
        }
        if (held.dmabuf.plane_count == 0) {
            return false;
        }
        if (!logged_held) {
            logged_held = true;
            log::info(log_component, "passing dmabufs on without reading them (modifier {:#x}, {} buffers)",
                      format->modifier, slots.size());
        }
        published_view = view;
        pw_buffer* superseded = nullptr;
        {
            const std::lock_guard lock(mutex);
            held.dmabuf.generation = buffer_generation;
            superseded = set_pending_locked(width, height, view_damage);
            pending_held = std::move(held);  // the old descriptors close with `held`
            pending_is_dmabuf = true;
        }
        if (superseded != nullptr) {
            pw_stream_queue_buffer(stream, superseded);
        }
        wake(frame_wake);
        return true;
    }

    /// Converts the `view` part of the buffer's pixels to BGRX in `out`.
    bool read_pixels(pw_buffer* buffer, const Rect& view, std::span<std::byte> out)
    {
        const spa_buffer* spa = buffer->buffer;
        if (!format) {
            return false;
        }
        const spa_video_info_raw& info = *format;
        const auto planes = planes_of(spa);
        const spa_data& plane = planes[0];
        const std::uint32_t width = info.size.width;
        const std::uint32_t height = info.size.height;
        const std::size_t stride =
            plane.chunk->stride > 0 ? static_cast<std::size_t>(plane.chunk->stride) : std::size_t{width} * 4U;
        if (stride < std::size_t{width} * 4U) {
            warn_bad_frame("stride is smaller than a row");
            return false;
        }
        switch (plane.type) {
        case SPA_DATA_MemPtr:
        case SPA_DATA_MemFd: {
            if (plane.data == nullptr || plane.maxsize == 0) {
                warn_bad_frame("shared memory buffer is not mapped");
                return false;
            }
            const std::span memory(static_cast<const std::byte*>(plane.data), plane.maxsize);
            return convert_view(memory.subspan(plane.chunk->offset % plane.maxsize), stride, view, out);
        }
        case SPA_DATA_DmaBuf: {
            DmabufImage image{width, height, drm_fourcc(layout), info.modifier, 0, {}, {}, {}};
            for (const spa_data& data : planes) {
                if (data.type != SPA_DATA_DmaBuf || image.planes == image.fds.size() || data.chunk == nullptr) {
                    break;
                }
                image.fds.at(image.planes) = static_cast<int>(data.fd);
                image.strides.at(image.planes) = static_cast<std::uint32_t>(std::max(data.chunk->stride, 0));
                image.offsets.at(image.planes) = data.chunk->offset;
                ++image.planes;
            }
            if (info.modifier == drm_format_mod_linear) {
                return read_linear_dmabuf(buffer, image, stride, view, out);
            }
            if (dmabuf == nullptr) {
                dmabuf_failed();
                return false;
            }
            const auto mapping = dmabuf->map(image);
            if (!mapping) {
                dmabuf_failed();
                return false;
            }
            return convert_view(mapping->pixels(), mapping->stride(), view, out);
        }
        default:
            warn_bad_frame("unknown buffer memory type");
            return false;
        }
    }

    bool read_linear_dmabuf(pw_buffer* buffer, const DmabufImage& image, std::size_t stride, const Rect& view,
                            std::span<std::byte> out)
    {
        const auto slot = slots.find(buffer);
        if (slot == slots.end() || image.planes == 0) {
            return false;
        }
        BufferSlot& mapped = *slot->second;
        const int fd = image.fds[0];
        if (mapped.map == nullptr) {
            const off_t length = ::lseek(fd, 0, SEEK_END);
            if (length <= 0) {
                dmabuf_failed();
                return false;
            }
            void* map = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                dmabuf_failed();
                return false;
            }
            mapped.map = map;
            mapped.map_size = static_cast<std::size_t>(length);
        }
        const std::span memory(static_cast<const std::byte*>(mapped.map), mapped.map_size);
        if (image.offsets[0] >= memory.size()) {
            warn_bad_frame("dmabuf offset is outside the buffer");
            return false;
        }
        const DmabufReadAccess access(fd);
        return convert_view(memory.subspan(image.offsets[0]), stride, view, out);
    }

    bool convert_view(std::span<const std::byte> image, std::size_t stride, const Rect& view, std::span<std::byte> out)
    {
        const auto width = static_cast<std::uint32_t>(view.width);
        const auto height = static_cast<std::uint32_t>(view.height);
        const std::size_t start = (static_cast<std::size_t>(view.y) * stride) + (static_cast<std::size_t>(view.x) * 4U);
        if (start > image.size() || image.size() - start < image_extent(stride, width, height)) {
            warn_bad_frame("buffer is smaller than the frame");
            return false;
        }
        convert_to_bgrx(layout, image.subspan(start), stride, width, height, out);
        return true;
    }

    void warn_bad_frame(std::string_view why)
    {
        if (!warned_bad_frame) {
            warned_bad_frame = true;
            log::warn(log_component, "dropping frame: {}", why);
        }
    }

    // --- Cursor ----------------------------------------------------------------

    /// SPA_META_Cursor. id 0: the cursor is not on this stream (KWin, and mutter
    /// when it left the monitor), so it is hidden. A bitmap of size 0 is an
    /// invisible cursor (mutter). bitmap_offset 0: position only, the shape
    /// did not change.
    void handle_cursor(const spa_buffer* spa)
    {
        const auto meta = find_meta(spa, SPA_META_Cursor);
        const auto cursor_meta = read_struct<spa_meta_cursor>(meta, 0);
        if (!cursor_meta) {
            return;
        }
        std::optional<CursorImage> shape;
        std::optional<std::pair<std::int32_t, std::int32_t>> position;
        if (cursor_meta->id == 0) {
            cursor_on_stream = false;
        } else {
            cursor_on_stream = true;
            const std::pair new_position{cursor_meta->position.x, cursor_meta->position.y};
            if (cursor_position != new_position) {
                cursor_position = new_position;
                position = new_position;
            }
            if (cursor_meta->bitmap_offset >= sizeof(spa_meta_cursor)) {
                shape = read_cursor_bitmap(meta, *cursor_meta);
            }
        }
        const bool visible = cursor_on_stream && cursor_has_shape && !cursor_shape_empty;
        if (!shape && !position && visible == cursor_visible) {
            return;
        }
        cursor_visible = visible;
        {
            const std::lock_guard lock(mutex);
            if (!cursor_pending) {
                pending_cursor = CursorUpdate{};
                cursor_pending = true;
            }
            if (shape) {
                pending_cursor.shape = std::move(shape);
            }
            if (position) {
                pending_cursor.position = position;
            }
            pending_cursor.visible = visible;
        }
        wake(cursor_wake);
    }

    std::optional<CursorImage> read_cursor_bitmap(std::span<const std::byte> meta, const spa_meta_cursor& cursor_meta)
    {
        const auto bitmap = read_struct<spa_meta_bitmap>(meta, cursor_meta.bitmap_offset);
        if (!bitmap) {
            warn_bad_cursor("bitmap header outside the metadata");
            return std::nullopt;
        }
        if (bitmap->size.width == 0 || bitmap->size.height == 0 || bitmap->offset == 0) {
            cursor_shape_empty = true;
            return std::nullopt;
        }
        const auto bitmap_layout = layout_from_spa(bitmap->format);
        if (!bitmap_layout) {
            // Format 0 means "no new bitmap"; other formats we cannot read.
            if (bitmap->format != 0) {
                warn_bad_cursor("unsupported bitmap format");
            }
            return std::nullopt;
        }
        const std::uint32_t width = bitmap->size.width;
        const std::uint32_t height = bitmap->size.height;
        const std::size_t stride = bitmap->stride > 0 ? static_cast<std::size_t>(bitmap->stride) : 0;
        const std::size_t start = std::size_t{cursor_meta.bitmap_offset} + bitmap->offset;
        if (width > 1024 || height > 1024 || stride < std::size_t{width} * 4U || start > meta.size() ||
            meta.size() - start < image_extent(stride, width, height)) {
            warn_bad_cursor("bitmap outside the metadata");
            return std::nullopt;
        }
        CursorImage image;
        image.width = width;
        image.height = height;
        image.hotspot_x = cursor_meta.hotspot.x;
        image.hotspot_y = cursor_meta.hotspot.y;
        image.pixels =
            convert_cursor(*bitmap_layout, meta.subspan(start), stride, width, height, options.cursor_premultiplied);
        cursor_has_shape = true;
        cursor_shape_empty = false;
        return image;
    }

    void warn_bad_cursor(std::string_view why)
    {
        if (!warned_bad_cursor) {
            warned_bad_cursor = true;
            log::warn(log_component, "ignoring cursor: {}", why);
        }
    }

    // --- Session thread --------------------------------------------------------

    std::optional<Frame> take_frame()
    {
        Frame frame;
        bool returned_buffer = false;
        {
            const std::lock_guard lock(mutex);
            if (!frame_pending) {
                return std::nullopt;
            }
            returned_buffer = return_front_locked();
            std::vector<Rect> damage = pending_damage.take();
            if (pending_width != front_width || pending_height != front_height) {
                damage.clear();  // The size changed: everything.
            }
            front_width = pending_width;
            front_height = pending_height;
            frame_pending = false;
            if (state != CaptureState::closed) {
                drain(frame_wake);
            }
            if (pending_is_dmabuf) {
                front_held = std::exchange(pending_held, HeldBuffer{});
                pending_is_dmabuf = false;
                front_is_dmabuf = true;
                front_has_pixels = false;
                frame.dmabuf = front_held.dmabuf;
            } else {
                std::swap(front, pending);
                front_has_pixels = true;
                frame.image = front_image();
            }
            frame.damage = std::move(damage);
            frame.sequence = pending_sequence;
        }
        if (returned_buffer) {
            pw::loop_signal_event(pw_thread_loop_get_loop(loop), release_event);
        }
        return frame;
    }

    [[nodiscard]] codec::ImageView front_image() const
    {
        return {std::span<const std::byte>(front).first(std::size_t{front_width} * front_height * 4U), front_width,
                front_height, std::size_t{front_width} * 4U};
    }

    /// Gives the session's dmabuf frame back, with `mutex` held. True if a
    /// buffer now waits in `returned` for the loop thread.
    bool return_front_locked()
    {
        if (!front_is_dmabuf) {
            return false;
        }
        front_is_dmabuf = false;
        pw_buffer* buffer = std::exchange(front_held.buffer, nullptr);
        front_held = HeldBuffer{};
        if (buffer == nullptr) {
            return false;  // PipeWire removed it meanwhile
        }
        returned.push_back(buffer);
        return true;
    }

    void release_frame()
    {
        bool returned_buffer = false;
        {
            const std::lock_guard lock(mutex);
            returned_buffer = return_front_locked();
            front_has_pixels = false;
        }
        if (returned_buffer) {
            pw::loop_signal_event(pw_thread_loop_get_loop(loop), release_event);
        }
    }

    /// Reads the dmabuf the session holds into `front`, with the loop locked:
    /// PipeWire cannot remove the buffer meanwhile, and the mappings are the
    /// loop thread's.
    std::optional<codec::ImageView> map_frame()
    {
        if (front_has_pixels) {
            return front_image();
        }
        if (!front_is_dmabuf) {
            return std::nullopt;
        }
        const pw::LoopLock loop_lock(loop);
        pw_buffer* buffer = nullptr;
        Rect view;
        {
            const std::lock_guard lock(mutex);
            buffer = front_held.buffer;
            view = front_held.view;
        }
        if (buffer == nullptr) {
            return std::nullopt;  // PipeWire took it back
        }
        front.resize(std::size_t{front_width} * front_height * 4U);
        if (!read_pixels(buffer, view, front)) {
            return std::nullopt;
        }
        front_has_pixels = true;
        return front_image();
    }

    std::optional<CursorUpdate> take_cursor()
    {
        const std::lock_guard lock(mutex);
        if (!cursor_pending) {
            return std::nullopt;
        }
        cursor_pending = false;
        if (state != CaptureState::closed) {
            drain(cursor_wake);
        }
        return std::exchange(pending_cursor, CursorUpdate{});
    }
};

namespace {

const pw_core_events& core_events()
{
    static const pw_core_events events = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = &PipeWireCapture::Impl::on_core_done;
        e.error = &PipeWireCapture::Impl::on_core_error;
        return e;
    }();
    return events;
}

const pw_registry_events& registry_events()
{
    static const pw_registry_events events = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global_remove = &PipeWireCapture::Impl::on_global_remove;
        return e;
    }();
    return events;
}

const pw_stream_events& stream_events()
{
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &PipeWireCapture::Impl::on_state_changed;
        e.param_changed = &PipeWireCapture::Impl::on_param_changed;
        e.add_buffer = &PipeWireCapture::Impl::on_add_buffer;
        e.remove_buffer = &PipeWireCapture::Impl::on_remove_buffer;
        e.process = &PipeWireCapture::Impl::on_process;
        return e;
    }();
    return events;
}

}  // namespace

PipeWireCapture::Impl::~Impl()
{
    if (loop != nullptr) {
        {
            const pw::LoopLock lock(loop);
            if (stream != nullptr) {
                spa_hook_remove(&stream_listener);
                pw_stream_destroy(stream);
                stream = nullptr;
            }
            if (registry != nullptr) {
                spa_hook_remove(&registry_listener);
                // pw_registry is a pw_proxy (pipewire/core.h).
                pw_proxy_destroy(
                    reinterpret_cast<pw_proxy*>(registry));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                registry = nullptr;
            }
            if (core != nullptr) {
                spa_hook_remove(&core_listener);
                pw_core_disconnect(core);
                core = nullptr;
            }
            if (renegotiate_event != nullptr) {
                pw::loop_destroy_source(pw_thread_loop_get_loop(loop), renegotiate_event);
                renegotiate_event = nullptr;
            }
            if (release_event != nullptr) {
                pw::loop_destroy_source(pw_thread_loop_get_loop(loop), release_event);
                release_event = nullptr;
            }
        }
        pw_thread_loop_stop(loop);
    }
    if (context != nullptr) {
        pw_context_destroy(context);
    }
    if (loop != nullptr) {
        pw_thread_loop_destroy(loop);
    }
    slots.clear();
    if (pw_initialized) {
        pw_deinit();
    }
}

Result<std::unique_ptr<PipeWireCapture>> PipeWireCapture::create(int pipewire_fd, std::uint32_t node_id,
                                                                 const PipeWireCaptureOptions& options)
{
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->requested_size = options.size;
    impl->target_node = node_id;
    impl->frame_wake = UniqueFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    impl->cursor_wake = UniqueFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (impl->frame_wake.get() < 0 || impl->cursor_wake.get() < 0) {
        return fail(Errc::io, "cannot create an eventfd");
    }

    pw_init(nullptr, nullptr);
    impl->pw_initialized = true;
    if (options.dmabuf) {
        impl->dmabuf = DmabufReader::open(options.render_node);
    }
    impl->init_offers();

    impl->loop = pw_thread_loop_new("farland-pipewire", nullptr);
    if (impl->loop == nullptr) {
        return fail(Errc::io, "cannot create the PipeWire thread loop");
    }
    impl->context = pw_context_new(pw_thread_loop_get_loop(impl->loop), nullptr, 0);
    if (impl->context == nullptr) {
        return fail(Errc::io, "cannot create a PipeWire context");
    }
    if (pw_thread_loop_start(impl->loop) < 0) {
        return fail(Errc::io, "cannot start the PipeWire thread loop");
    }

    const pw::LoopLock lock(impl->loop);
    if (pipewire_fd >= 0) {
        // pw_context_connect_fd takes ownership, so give it a copy.
        const int fd = ::fcntl(pipewire_fd, F_DUPFD_CLOEXEC, 3);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (fd < 0) {
            return fail(Errc::io, "cannot duplicate the PipeWire fd");
        }
        impl->core = pw_context_connect_fd(impl->context, fd, nullptr, 0);
    } else {
        impl->core = pw_context_connect(impl->context, nullptr, 0);
    }
    if (impl->core == nullptr) {
        log::warn(log_component, "cannot connect to PipeWire: {}", std::strerror(errno));
        return fail(Errc::io, "cannot connect to PipeWire");
    }
    pw::core_add_listener(impl->core, &impl->core_listener, &core_events(), impl.get());
    impl->registry = pw::core_get_registry(impl->core);
    if (impl->registry == nullptr) {
        return fail(Errc::io, "cannot get the PipeWire registry");
    }
    pw::registry_add_listener(impl->registry, &impl->registry_listener, &registry_events(), impl.get());

    // One round trip, so that a dead remote fails here and not later. It
    // comes before the stream: libpipewire leaks the stream's exported node
    // when the core dies while pw_stream_connect exports it.
    impl->sync_seq = pw::core_sync(impl->core, PW_ID_CORE, 0);
    const auto is_closed = [&impl] {
        const std::lock_guard state_lock(impl->mutex);
        return impl->state == CaptureState::closed;
    };
    while (!impl->sync_done) {
        if (is_closed()) {
            return fail(Errc::io, "the PipeWire connection failed");
        }
        if (pw_thread_loop_timed_wait(impl->loop, connect_timeout_seconds) != 0) {
            return fail(Errc::io, "PipeWire did not answer");
        }
    }

    const std::array<spa_dict_item, 4> items{{
        {PW_KEY_MEDIA_TYPE, "Video"},
        {PW_KEY_MEDIA_CATEGORY, "Capture"},
        {PW_KEY_MEDIA_ROLE, "Screen"},
        {PW_KEY_NODE_NAME, options.stream_name.c_str()},
    }};
    const spa_dict dict{0, static_cast<std::uint32_t>(items.size()), items.data()};
    impl->stream = pw_stream_new(impl->core, options.stream_name.c_str(), pw_properties_new_dict(&dict));
    if (impl->stream == nullptr) {
        return fail(Errc::io, "cannot create a PipeWire stream");
    }
    pw_stream_add_listener(impl->stream, &impl->stream_listener, &stream_events(), impl.get());
    impl->renegotiate_event =
        pw::loop_add_event(pw_thread_loop_get_loop(impl->loop), &Impl::on_renegotiate, impl.get());
    impl->release_event = pw::loop_add_event(pw_thread_loop_get_loop(impl->loop), &Impl::on_release, impl.get());

    pw::PodBuilder b(format_pod_capacity);
    auto params = impl->build_formats(b);
    if (b.overflowed()) {
        return fail(Errc::limit_exceeded, "format parameters do not fit");
    }
    // The portal only exposes the screen cast node on this remote, so the
    // node id is the target (as OBS and gnome-remote-desktop do it).
    // pw_stream_flags is a bit mask.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                    PW_STREAM_FLAG_DONT_RECONNECT);
    if (pw_stream_connect(impl->stream, PW_DIRECTION_INPUT, node_id, flags, params.data(),
                          static_cast<std::uint32_t>(params.size())) < 0) {
        return fail(Errc::io, "cannot connect the PipeWire stream");
    }
    return std::unique_ptr<PipeWireCapture>(new PipeWireCapture(std::move(impl)));
}

PipeWireCapture::PipeWireCapture(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PipeWireCapture::~PipeWireCapture() = default;

FrameSource& PipeWireCapture::frames() noexcept
{
    return impl_->frames;
}

CursorSource& PipeWireCapture::cursor() noexcept
{
    return impl_->cursor;
}

CaptureState PipeWireCapture::state() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->state;
}

std::string PipeWireCapture::error() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

std::uint32_t PipeWireCapture::node_id() const
{
    const pw::LoopLock lock(impl_->loop);
    return impl_->stream != nullptr ? pw_stream_get_node_id(impl_->stream) : SPA_ID_INVALID;
}

void PipeWireCapture::request_size(std::uint32_t width, std::uint32_t height)
{
    const pw::LoopLock lock(impl_->loop);
    const std::pair size{width, height};
    if (impl_->requested_size == size) {
        return;
    }
    impl_->requested_size = size;
    log::info(log_component, "asking the producer for {}x{}", width, height);
    if (impl_->renegotiate_event != nullptr) {
        pw::loop_signal_event(pw_thread_loop_get_loop(impl_->loop), impl_->renegotiate_event);
    }
}

std::uint64_t PipeWireCapture::buffers_received() const noexcept
{
    return impl_->buffers_received.load(std::memory_order_acquire);
}

}  // namespace farland::platform::portal
