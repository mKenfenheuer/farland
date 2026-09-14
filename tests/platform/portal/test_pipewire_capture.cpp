// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests of PipeWireCapture against a private PipeWire daemon and a
// producer stream that plays the compositor. Skipped where no `pipewire`
// binary is installed (or FARLAND_PIPEWIRE does not point to one).
//
// Dmabufs come from /dev/udmabuf (LINEAR, over a memfd); those tests skip
// where it cannot be opened, as in the CI containers.

#include <farland/platform/portal/pipewire_capture.hpp>
#include <farland/platform/portal/pipewire_util.hpp>
#include <farland/platform/portal/pixel_formats.hpp>

#include <catch2/catch_test_macros.hpp>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/udmabuf.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern char** environ;

// Frame and cursor specs below name only the fields a test cares about.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifdef __clang__
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif

using namespace farland::platform;
using namespace farland::platform::portal;
namespace fs = std::filesystem;

namespace {

constexpr int timeout_ms = 5000;

bool wait_readable(int fd, int ms = timeout_ms)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}

template <class Predicate>
bool wait_until(Predicate predicate, int ms = timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// --- The daemon ---------------------------------------------------------------

/// A pipewire daemon with a minimal configuration (no session manager, no
/// devices) in a temporary runtime directory, shared by all tests.
class Daemon {
public:
    /// The daemon, or nullptr (and `why`) if it cannot run here.
    static Daemon* get(std::string& why)
    {
        static std::string failure;
        static const std::unique_ptr<Daemon> daemon = start(failure);
        why = failure;
        return daemon.get();
    }

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;
    ~Daemon()
    {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// A new client connection to the daemon, as the portal would hand out.
    [[nodiscard]] int connect() const
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string path = (dir_ / "pipewire-0").string();
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

private:
    Daemon(fs::path dir, pid_t pid) : dir_(std::move(dir)), pid_(pid) {}

    static std::string find_binary()
    {
        if (const char* env = std::getenv("FARLAND_PIPEWIRE"); env != nullptr && *env != '\0') {
            return env;
        }
        const char* path = std::getenv("PATH");
        std::string_view rest = path != nullptr ? path : "/usr/bin:/bin";
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            const fs::path candidate = fs::path(rest.substr(0, colon)) / "pipewire";
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate.string();
            }
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        }
        return {};
    }

    static std::unique_ptr<Daemon> start(std::string& why)
    {
        const std::string binary = find_binary();
        if (binary.empty()) {
            why = "no pipewire binary in PATH (set FARLAND_PIPEWIRE)";
            return nullptr;
        }
        std::string templ = (fs::temp_directory_path() / "farland-pw-XXXXXX").string();
        if (::mkdtemp(templ.data()) == nullptr) {
            why = "mkdtemp failed";
            return nullptr;
        }
        const fs::path dir = templ;
        const fs::path conf = dir / "pipewire.conf";
        std::ofstream(conf) << R"(context.properties = {
    core.daemon = true
    core.name = pipewire-0
    support.dbus = false
    mem.allow-mlock = false
}
context.spa-libs = {
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
)";
        std::vector<std::string> env_strings;
        for (char** e = environ; *e != nullptr; ++e) {
            const std::string_view entry = *e;
            if (!entry.starts_with("PIPEWIRE_") && !entry.starts_with("XDG_RUNTIME_DIR=")) {
                env_strings.emplace_back(entry);
            }
        }
        env_strings.push_back("PIPEWIRE_RUNTIME_DIR=" + dir.string());
        env_strings.push_back("XDG_RUNTIME_DIR=" + dir.string());
        env_strings.emplace_back("PIPEWIRE_DEBUG=2");
        std::vector<char*> envp;
        for (std::string& s : env_strings) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);
        std::string arg0 = binary;
        std::string arg1 = "-c";
        std::string arg2 = conf.string();
        std::array<char*, 4> argv{arg0.data(), arg1.data(), arg2.data(), nullptr};

        const std::string log_path = (dir / "pipewire.log").string();
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        pid_t pid = -1;
        const int spawned = ::posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        if (spawned != 0) {
            why = "cannot start " + binary;
            return nullptr;
        }
        auto daemon = std::unique_ptr<Daemon>(new Daemon(dir, pid));
        const bool up = wait_until([&] {
            const int fd = daemon->connect();
            if (fd < 0) {
                return false;
            }
            ::close(fd);
            return true;
        });
        if (!up) {
            std::ifstream log(log_path);
            why = "the pipewire daemon did not come up: " +
                  std::string(std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>());
            return nullptr;
        }
        return daemon;
    }

    fs::path dir_;
    pid_t pid_;
};

// --- The producer ---------------------------------------------------------------

/// Byte order of a layout in memory, as in its name.
std::string_view byte_order(PixelLayout layout)
{
    switch (layout) {
    case PixelLayout::bgrx:
        return "BGRX";
    case PixelLayout::bgra:
        return "BGRA";
    case PixelLayout::rgbx:
        return "RGBX";
    case PixelLayout::rgba:
        return "RGBA";
    case PixelLayout::xrgb:
        return "XRGB";
    case PixelLayout::argb:
        return "ARGB";
    case PixelLayout::xbgr:
        return "XBGR";
    case PixelLayout::abgr:
        return "ABGR";
    }
    return "";
}

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// The test picture: every pixel differs, and so does every frame.
Rgb pattern(std::uint32_t x, std::uint32_t y, std::uint32_t frame)
{
    return {static_cast<std::uint8_t>(x * 3 + frame), static_cast<std::uint8_t>(y * 5 + frame * 7),
            static_cast<std::uint8_t>(x + y + frame * 13)};
}

void put(std::span<std::byte> out, PixelLayout layout, Rgb c, std::uint8_t alpha)
{
    std::size_t i = 0;
    for (const char ch : byte_order(layout)) {
        out[i++] = std::byte{ch == 'R' ? c.r : ch == 'G' ? c.g : ch == 'B' ? c.b : alpha};
    }
}

struct CursorSpec {
    std::uint32_t id = 1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t hotspot_x = 0;
    std::int32_t hotspot_y = 0;
    /// No bitmap at all (bitmap_offset 0), mutter's empty sprite (all-zero
    /// bitmap), or a real one.
    enum class Bitmap : std::uint8_t { none, empty, image } bitmap = Bitmap::none;
    PixelLayout layout = PixelLayout::rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> pixels;  // tightly packed, in `layout`
};

struct FrameSpec {
    std::uint32_t frame = 0;  // picks the pattern
    bool has_frame = true;    // false: a cursor-only buffer (chunk size 0)
    /// nullopt: the damage metadata holds no valid region.
    std::optional<std::vector<Rect>> damage;
    std::optional<Rect> crop;
    /// nullopt: cursor id 0.
    std::optional<CursorSpec> cursor;
};

constexpr std::uint32_t cursor_max = 64;

/// A LINEAR dmabuf made by /dev/udmabuf from a memfd, standing in for a
/// compositor's buffer.
class Udmabuf {
public:
    static bool available()
    {
        const int fd = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        ::close(fd);
        return true;
    }

    /// nullptr on failure (it runs on PipeWire's thread, where REQUIRE cannot).
    static std::unique_ptr<Udmabuf> create(std::size_t size)
    {
        const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        auto buffer = std::unique_ptr<Udmabuf>(new Udmabuf);
        buffer->size_ = (size + page - 1) / page * page;
        buffer->memfd_ = ::memfd_create("farland-test", MFD_ALLOW_SEALING | MFD_CLOEXEC);
        if (buffer->memfd_ < 0 || ::ftruncate(buffer->memfd_, static_cast<off_t>(buffer->size_)) != 0 ||
            ::fcntl(buffer->memfd_, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
            return nullptr;
        }
        const int device = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (device < 0) {
            return nullptr;
        }
        udmabuf_create request{};
        request.memfd = static_cast<std::uint32_t>(buffer->memfd_);
        request.flags = UDMABUF_FLAGS_CLOEXEC;
        request.size = buffer->size_;
        buffer->fd_ = ::ioctl(device, UDMABUF_CREATE, &request);
        ::close(device);
        void* map = ::mmap(nullptr, buffer->size_, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->memfd_, 0);
        if (buffer->fd_ < 0 || map == MAP_FAILED) {
            return nullptr;
        }
        buffer->map_ = map;
        return buffer;
    }

    Udmabuf(const Udmabuf&) = delete;
    Udmabuf& operator=(const Udmabuf&) = delete;
    Udmabuf(Udmabuf&&) = delete;
    Udmabuf& operator=(Udmabuf&&) = delete;
    ~Udmabuf()
    {
        if (map_ != nullptr) {
            ::munmap(map_, size_);
        }
        for (const int fd : {fd_, memfd_}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] void* data() const { return map_; }

private:
    Udmabuf() = default;

    int memfd_ = -1;
    int fd_ = -1;
    void* map_ = nullptr;
    std::size_t size_ = 0;
};

class Producer {
public:
    struct Options {
        PixelLayout layout = PixelLayout::bgrx;
        std::uint32_t width = 64;
        std::uint32_t height = 48;
        bool damage_meta = true;
        bool crop_meta = true;
        bool cursor_meta = true;
        /// LINEAR dmabufs from /dev/udmabuf (allocated by the producer, as
        /// compositors do) instead of shared memory.
        bool dmabuf = false;
        /// Exactly this many buffers with `dmabuf`.
        std::int32_t buffers = 4;
    };

    Producer(int fd, const Options& options) : options_(options)
    {
        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("test-producer", nullptr);
        REQUIRE(loop_ != nullptr);
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        pw_thread_loop_start(loop_);
        const pw::LoopLock lock(loop_);
        core_ = pw_context_connect_fd(context_, fd, nullptr, 0);
        REQUIRE(core_ != nullptr);
        const std::array<spa_dict_item, 2> items{
            {{PW_KEY_MEDIA_CLASS, "Video/Source"}, {PW_KEY_NODE_NAME, "test-producer"}}};
        const spa_dict dict{0, static_cast<std::uint32_t>(items.size()), items.data()};
        stream_ = pw_stream_new(core_, "test-producer", pw_properties_new_dict(&dict));
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = &Producer::on_state_changed;
            e.param_changed = &Producer::on_param_changed;
            e.add_buffer = &Producer::on_add_buffer;
            e.remove_buffer = &Producer::on_remove_buffer;
            e.process = &Producer::on_process;
            return e;
        }();
        pw_stream_add_listener(stream_, &listener_, &events, this);
        pw::PodBuilder b;
        const spa_pod* format = build_format(b);
        const auto memory = options_.dmabuf ? PW_STREAM_FLAG_ALLOC_BUFFERS : PW_STREAM_FLAG_MAP_BUFFERS;
        pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | memory), &format, 1);
    }

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;
    Producer(Producer&&) = delete;
    Producer& operator=(Producer&&) = delete;
    ~Producer()
    {
        {
            const pw::LoopLock lock(loop_);
            if (link_ != nullptr) {
                pw_proxy_destroy(link_);
            }
            spa_hook_remove(&listener_);
            pw_stream_destroy(stream_);
            pw_core_disconnect(core_);
        }
        pw_thread_loop_stop(loop_);
        pw_context_destroy(context_);
        pw_thread_loop_destroy(loop_);
        pw_deinit();
    }

    [[nodiscard]] std::uint32_t node_id()
    {
        std::uint32_t id = SPA_ID_INVALID;
        REQUIRE(wait_until([&] {
            const pw::LoopLock lock(loop_);
            id = pw_stream_get_node_id(stream_);
            return id != SPA_ID_INVALID;
        }));
        return id;
    }

    /// Links our output to `input_node` (there is no session manager).
    void link_to(std::uint32_t input_node)
    {
        const std::uint32_t output = node_id();
        const pw::LoopLock lock(loop_);
        const std::string out = std::to_string(output);
        const std::string in = std::to_string(input_node);
        const std::array<spa_dict_item, 2> items{{{"link.output.node", out.c_str()}, {"link.input.node", in.c_str()}}};
        const spa_dict dict{0, static_cast<std::uint32_t>(items.size()), items.data()};
        link_ = static_cast<pw_proxy*>(
            pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &dict, 0));
        REQUIRE(link_ != nullptr);
    }

    void wait_streaming()
    {
        const pw::LoopLock lock(loop_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (state_ != PW_STREAM_STATE_STREAMING || !format_) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            pw_thread_loop_timed_wait(loop_, 1);
        }
    }

    /// Asks for a new size and waits until it is negotiated.
    void resize(std::uint32_t width, std::uint32_t height)
    {
        const pw::LoopLock lock(loop_);
        options_.width = width;
        options_.height = height;
        format_.reset();
        pw::PodBuilder b;
        const spa_pod* format = build_format(b);
        pw_stream_update_params(stream_, &format, 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!format_ || format_->size.width != width || state_ != PW_STREAM_STATE_STREAMING) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            pw_thread_loop_timed_wait(loop_, 1);
        }
    }

    [[nodiscard]] std::size_t stride() const { return (std::size_t{options_.width} * 4) + 16; }
    /// A dmabuf could not be allocated.
    [[nodiscard]] bool allocation_failed() const { return allocation_failed_.load(); }

    /// Produces one buffer and runs graph cycles until `delivered()` says the
    /// consumer handled it. One buffer at a time: a cycle that starts before
    /// the consumer finished the previous one can lose that buffer.
    template <class Delivered>
    void send(const FrameSpec& spec, Delivered delivered)
    {
        const pw::LoopLock lock(loop_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        pending_ = &spec;
        sent_ = false;
        while (!sent_ || !delivered()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            pw_stream_trigger_process(stream_);
            timespec abstime{};
            pw_thread_loop_get_time(loop_, &abstime, 10 * SPA_NSEC_PER_MSEC);
            pw_thread_loop_timed_wait_full(loop_, &abstime);
        }
        pending_ = nullptr;
    }

private:
    const spa_pod* build_format(pw::PodBuilder& b) const
    {
        spa_pod_frame f{};
        b.push_object(&f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        b.prop(SPA_FORMAT_mediaType);
        b.id(SPA_MEDIA_TYPE_video);
        b.prop(SPA_FORMAT_mediaSubtype);
        b.id(SPA_MEDIA_SUBTYPE_raw);
        b.prop(SPA_FORMAT_VIDEO_format);
        b.id(to_spa_format(options_.layout));
        if (options_.dmabuf) {
            b.prop(SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            b.long_(static_cast<std::int64_t>(drm_format_mod_linear));
        }
        b.prop(SPA_FORMAT_VIDEO_size);
        b.rectangle(options_.width, options_.height);
        b.prop(SPA_FORMAT_VIDEO_framerate);
        b.fraction(0, 1);
        b.prop(SPA_FORMAT_VIDEO_maxFramerate);
        b.fraction(30, 1);
        return b.pop(&f);
    }

    static void on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* /*error*/)
    {
        auto& self = *static_cast<Producer*>(data);
        self.state_ = state;
        pw_thread_loop_signal(self.loop_, false);
    }

    static void on_param_changed(void* data, std::uint32_t id, const spa_pod* param)
    {
        auto& self = *static_cast<Producer*>(data);
        if (id != SPA_PARAM_Format || param == nullptr) {
            return;
        }
        spa_video_info_raw info{};
        spa_format_video_raw_parse(param, &info);
        self.format_ = info;
        const auto stride = static_cast<std::int32_t>(self.stride());
        pw::PodBuilder b;
        std::vector<const spa_pod*> params;
        spa_pod_frame f{};
        spa_pod_frame c{};
        b.push_object(&f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        if (self.options_.dmabuf) {
            const std::int32_t n = self.options_.buffers;
            b.int_range(SPA_PARAM_BUFFERS_buffers, n, n, n);
        } else {
            b.int_range(SPA_PARAM_BUFFERS_buffers, 4, 2, 8);
        }
        b.prop(SPA_PARAM_BUFFERS_blocks);
        b.int_(1);
        b.prop(SPA_PARAM_BUFFERS_size);
        b.int_(stride * static_cast<std::int32_t>(info.size.height));
        b.prop(SPA_PARAM_BUFFERS_stride);
        b.int_(stride);
        b.prop(SPA_PARAM_BUFFERS_dataType);
        b.push_choice(&c, SPA_CHOICE_Flags);
        b.int_(1 << (self.options_.dmabuf ? SPA_DATA_DmaBuf : SPA_DATA_MemFd));
        static_cast<void>(b.pop(&c));
        params.push_back(b.pop(&f));
        params.push_back(b.meta(SPA_META_Header, sizeof(spa_meta_header)));
        if (self.options_.damage_meta) {
            params.push_back(b.meta(SPA_META_VideoDamage, sizeof(spa_meta_region) * 4));
        }
        if (self.options_.crop_meta) {
            params.push_back(b.meta(SPA_META_VideoCrop, sizeof(spa_meta_region)));
        }
        if (self.options_.cursor_meta) {
            params.push_back(
                b.meta(SPA_META_Cursor, static_cast<std::int32_t>(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) +
                                                                  (cursor_max * cursor_max * 4))));
        }
        pw_stream_update_params(self.stream_, params.data(), static_cast<std::uint32_t>(params.size()));
        pw_thread_loop_signal(self.loop_, false);
    }

    /// With `dmabuf`, the producer allocates the memory (ALLOC_BUFFERS), as
    /// mutter and KWin do.
    static void on_add_buffer(void* data, pw_buffer* buffer)
    {
        auto& self = *static_cast<Producer*>(data);
        if (!self.options_.dmabuf || !self.format_ || buffer->buffer->n_datas == 0) {
            return;
        }
        auto dmabuf = Udmabuf::create(self.stride() * self.format_->size.height);
        if (dmabuf == nullptr) {
            self.allocation_failed_ = true;
            return;
        }
        spa_data& plane = buffer->buffer->datas[0];
        plane.type = SPA_DATA_DmaBuf;
        plane.flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
        plane.fd = dmabuf->fd();
        plane.mapoffset = 0;
        plane.maxsize = static_cast<std::uint32_t>(dmabuf->size());
        plane.data = dmabuf->data();
        self.dmabufs_[buffer] = std::move(dmabuf);
    }

    static void on_remove_buffer(void* data, pw_buffer* buffer)
    {
        static_cast<Producer*>(data)->dmabufs_.erase(buffer);
    }

    static void on_process(void* data)
    {
        auto& self = *static_cast<Producer*>(data);
        if (self.pending_ == nullptr || self.sent_ || !self.format_) {
            return;
        }
        pw_buffer* buffer = pw_stream_dequeue_buffer(self.stream_);
        if (buffer == nullptr) {
            return;
        }
        self.fill(*buffer->buffer, *self.pending_);
        pw_stream_queue_buffer(self.stream_, buffer);
        self.sent_ = true;
        pw_thread_loop_signal(self.loop_, false);
    }

    void fill(spa_buffer& buffer, const FrameSpec& spec)
    {
        const std::uint32_t width = format_->size.width;
        const std::uint32_t height = format_->size.height;
        spa_data& plane = buffer.datas[0];
        if (auto* header = static_cast<spa_meta_header*>(
                spa_buffer_find_meta_data(&buffer, SPA_META_Header, sizeof(spa_meta_header)))) {
            *header = spa_meta_header{};
            header->seq = seq_++;
        }
        if (spec.has_frame) {
            const std::span memory(static_cast<std::byte*>(plane.data), plane.maxsize);
            REQUIRE(memory.size() >= stride() * height);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    put(memory.subspan((y * stride()) + (x * 4), 4), options_.layout, pattern(x, y, spec.frame), 0xff);
                }
            }
            plane.chunk->offset = 0;
            plane.chunk->size = static_cast<std::uint32_t>(stride() * height);
            plane.chunk->stride = static_cast<std::int32_t>(stride());
            plane.chunk->flags = SPA_CHUNK_FLAG_NONE;
        } else {
            plane.chunk->size = 0;
            plane.chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
        }
        if (spa_meta* meta = spa_buffer_find_meta(&buffer, SPA_META_VideoDamage)) {
            const std::span regions(static_cast<spa_meta_region*>(meta->data), meta->size / sizeof(spa_meta_region));
            std::size_t n = 0;
            for (const Rect& r : spec.damage.value_or(std::vector<Rect>{})) {
                if (n == regions.size()) {
                    break;
                }
                regions[n].region.position = spa_point{r.x, r.y};
                regions[n].region.size =
                    spa_rectangle{static_cast<std::uint32_t>(r.width), static_cast<std::uint32_t>(r.height)};
                ++n;
            }
            if (n < regions.size()) {
                regions[n] = spa_meta_region{};
            }
        }
        if (auto* crop = static_cast<spa_meta_region*>(
                spa_buffer_find_meta_data(&buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)))) {
            *crop = spa_meta_region{};
            if (spec.crop) {
                crop->region.position = spa_point{spec.crop->x, spec.crop->y};
                crop->region.size = spa_rectangle{static_cast<std::uint32_t>(spec.crop->width),
                                                  static_cast<std::uint32_t>(spec.crop->height)};
            }
        }
        if (spa_meta* meta = spa_buffer_find_meta(&buffer, SPA_META_Cursor)) {
            write_cursor(*meta, spec.cursor);
        }
    }

    static void write_cursor(spa_meta& meta, const std::optional<CursorSpec>& spec)
    {
        const std::span bytes(static_cast<std::byte*>(meta.data), meta.size);
        spa_meta_cursor cursor{};
        if (spec) {
            cursor.id = spec->id;
            cursor.position = spa_point{spec->x, spec->y};
            cursor.hotspot = spa_point{spec->hotspot_x, spec->hotspot_y};
            if (spec->bitmap != CursorSpec::Bitmap::none) {
                cursor.bitmap_offset = sizeof(spa_meta_cursor);
                spa_meta_bitmap bitmap{};
                if (spec->bitmap == CursorSpec::Bitmap::image) {
                    bitmap.format = to_spa_format(spec->layout);
                    bitmap.size = spa_rectangle{spec->width, spec->height};
                    bitmap.stride = static_cast<std::int32_t>(spec->width * 4);
                    bitmap.offset = sizeof(spa_meta_bitmap);
                    REQUIRE(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + spec->pixels.size() <= bytes.size());
                    std::memcpy(bytes.subspan(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap)).data(),
                                spec->pixels.data(), spec->pixels.size());
                }
                std::memcpy(bytes.subspan(sizeof(spa_meta_cursor)).data(), &bitmap, sizeof(bitmap));
            }
        }
        std::memcpy(bytes.data(), &cursor, sizeof(cursor));
    }

    Options options_;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    pw_proxy* link_ = nullptr;
    spa_hook listener_{};
    pw_stream_state state_ = PW_STREAM_STATE_UNCONNECTED;
    std::optional<spa_video_info_raw> format_;
    const FrameSpec* pending_ = nullptr;
    bool sent_ = false;
    std::uint64_t seq_ = 0;
    std::unordered_map<pw_buffer*, std::unique_ptr<Udmabuf>> dmabufs_;
    std::atomic<bool> allocation_failed_{false};
};

// --- Fixture --------------------------------------------------------------------

struct Pipeline {
    std::unique_ptr<Producer> producer;
    std::unique_ptr<PipeWireCapture> capture;
};

/// A producer and a capture linked to it, streaming. Skips without a daemon.
Pipeline make_pipeline(const Producer::Options& options)
{
    std::string why;
    Daemon* daemon = Daemon::get(why);
    if (daemon == nullptr) {
        SKIP(why);
    }
    Pipeline p;
    const int producer_fd = daemon->connect();
    REQUIRE(producer_fd >= 0);
    p.producer = std::make_unique<Producer>(producer_fd, options);
    const std::uint32_t producer_node = p.producer->node_id();

    const int capture_fd = daemon->connect();
    REQUIRE(capture_fd >= 0);
    auto capture = PipeWireCapture::create(capture_fd, producer_node);
    ::close(capture_fd);  // The capture has its own copy.
    REQUIRE(capture.has_value());
    p.capture = std::move(*capture);
    std::uint32_t capture_node = SPA_ID_INVALID;
    REQUIRE(wait_until([&] {
        capture_node = p.capture->node_id();
        return capture_node != SPA_ID_INVALID;
    }));
    p.producer->link_to(capture_node);
    p.producer->wait_streaming();
    REQUIRE(wait_until([&] { return p.capture->state() == CaptureState::streaming; }));
    REQUIRE_FALSE(p.producer->allocation_failed());
    return p;
}

/// make_pipeline with a producer of udmabufs; skips without /dev/udmabuf.
Pipeline make_dmabuf_pipeline(Producer::Options options)
{
    if (!Udmabuf::available()) {
        SKIP("cannot open /dev/udmabuf");
    }
    options.dmabuf = true;
    return make_pipeline(options);
}

/// Sends one buffer and waits until the capture has handled it.
void deliver(Pipeline& p, const FrameSpec& spec)
{
    const std::uint64_t before = p.capture->buffers_received();
    p.producer->send(spec, [&] { return p.capture->buffers_received() > before; });
}

std::optional<Frame> next_frame(PipeWireCapture& capture)
{
    if (!wait_readable(capture.frames().wake_fd())) {
        return std::nullopt;
    }
    return capture.frames().take_frame();
}

std::optional<CursorUpdate> next_cursor(PipeWireCapture& capture)
{
    if (!wait_readable(capture.cursor().wake_fd())) {
        return std::nullopt;
    }
    return capture.cursor().take_cursor();
}

/// Compares a captured frame against pattern(frame) over `view`.
void check_pixels(const Frame& f, std::uint32_t frame, const Rect& view)
{
    REQUIRE(f.image.width == static_cast<std::uint32_t>(view.width));
    REQUIRE(f.image.height == static_cast<std::uint32_t>(view.height));
    REQUIRE(f.image.stride == std::size_t{f.image.width} * 4);
    REQUIRE(f.image.data.size() >= f.image.stride * f.image.height);
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < f.image.height; ++y) {
        for (std::uint32_t x = 0; x < f.image.width; ++x) {
            const auto expected =
                pattern(x + static_cast<std::uint32_t>(view.x), y + static_cast<std::uint32_t>(view.y), frame);
            const auto px = f.image.data.subspan((y * f.image.stride) + (x * 4), 4);
            if (px[0] != std::byte{expected.b} || px[1] != std::byte{expected.g} || px[2] != std::byte{expected.r}) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

/// The BGRX dmabuf the capture passed on holds pattern(frame): mapped here,
/// through the capture's own descriptor, as an encoder would import it.
void check_dmabuf(const Dmabuf& dmabuf, std::uint32_t frame, std::size_t stride)
{
    REQUIRE(dmabuf.plane_count == 1);
    const DmabufPlane& plane = dmabuf.planes[0];
    REQUIRE(plane.fd >= 0);
    CHECK(plane.pitch == stride);
    const off_t size = ::lseek(plane.fd, 0, SEEK_END);
    REQUIRE(size > 0);
    void* map = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, plane.fd, 0);
    REQUIRE(map != MAP_FAILED);
    const std::span memory(static_cast<const std::byte*>(map), static_cast<std::size_t>(size));
    REQUIRE(memory.size() >= plane.offset + (stride * dmabuf.height));
    std::size_t mismatches = 0;
    for (std::uint32_t y = 0; y < dmabuf.height; ++y) {
        for (std::uint32_t x = 0; x < dmabuf.width; ++x) {
            const auto expected = pattern(x, y, frame);
            const auto px = memory.subspan(plane.offset + (y * stride) + (x * 4), 4);
            if (px[0] != std::byte{expected.b} || px[1] != std::byte{expected.g} || px[2] != std::byte{expected.r}) {
                ++mismatches;
            }
        }
    }
    ::munmap(map, static_cast<std::size_t>(size));
    CHECK(mismatches == 0);
}

std::vector<std::byte> cursor_pixels(PixelLayout layout, std::uint32_t width, std::uint32_t height, std::uint8_t alpha)
{
    std::vector<std::byte> pixels(std::size_t{width} * height * 4);
    for (std::uint32_t i = 0; i < width * height; ++i) {
        // Premultiplied: colour <= alpha.
        const Rgb c{static_cast<std::uint8_t>((i * 3) % (alpha + 1U)),
                    static_cast<std::uint8_t>((i * 5) % (alpha + 1U)),
                    static_cast<std::uint8_t>((i * 7) % (alpha + 1U))};
        put(std::span(pixels).subspan(std::size_t{i} * 4, 4), layout, c, alpha);
    }
    return pixels;
}

}  // namespace

TEST_CASE("PipeWire capture converts every shared-memory pixel format", "[portal][pipewire]")
{
    for (const PixelLayout layout : all_pixel_layouts) {
        CAPTURE(byte_order(layout));
        auto p = make_pipeline({.layout = layout});
        deliver(p, {.frame = 1, .damage = std::vector<Rect>{{0, 0, 64, 48}}});
        const auto frame = next_frame(*p.capture);
        REQUIRE(frame.has_value());
        check_pixels(*frame, 1, {0, 0, 64, 48});
        CHECK(frame->damage.empty());  // The first frame is all new.
        CHECK(p.capture->frames().size() == std::pair<std::uint32_t, std::uint32_t>{64, 48});
    }
}

TEST_CASE("PipeWire capture merges the damage of frames nobody took", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    deliver(p, {.frame = 1, .damage = std::vector<Rect>{{0, 0, 64, 48}}});
    REQUIRE(next_frame(*p.capture).has_value());

    deliver(p, {.frame = 2, .damage = std::vector<Rect>{{1, 2, 3, 4}}});
    deliver(p, {.frame = 3, .damage = std::vector<Rect>{{10, 10, 4, 4}, {60, 40, 10, 10}}});
    // Buffers are handled in order: once this cursor update is out, frame 3 is in.
    deliver(p, {.has_frame = false, .cursor = CursorSpec{.x = 5, .y = 5}});
    REQUIRE(next_cursor(*p.capture).has_value());

    const auto frame = p.capture->frames().take_frame();
    REQUIRE(frame.has_value());
    check_pixels(*frame, 3, {0, 0, 64, 48});
    CHECK(frame->damage == std::vector<Rect>{{1, 2, 3, 4}, {10, 10, 4, 4}, {60, 40, 4, 8}});
    CHECK(frame->sequence == 3);
    CHECK_FALSE(p.capture->frames().take_frame().has_value());
    CHECK_FALSE(wait_readable(p.capture->frames().wake_fd(), 0));

    // Damage metadata without a valid region: everything changed.
    deliver(p, {.frame = 4, .damage = std::nullopt});
    const auto full = next_frame(*p.capture);
    REQUIRE(full.has_value());
    CHECK(full->damage.empty());
    check_pixels(*full, 4, {0, 0, 64, 48});
}

TEST_CASE("PipeWire capture treats frames without damage metadata as all new", "[portal][pipewire]")
{
    auto p = make_pipeline({.damage_meta = false, .crop_meta = false, .cursor_meta = false});
    for (std::uint32_t i = 1; i <= 3; ++i) {
        deliver(p, {.frame = i, .damage = std::vector<Rect>{{0, 0, 1, 1}}});
        const auto frame = next_frame(*p.capture);
        REQUIRE(frame.has_value());
        CHECK(frame->damage.empty());
        check_pixels(*frame, i, {0, 0, 64, 48});
    }
    CHECK_FALSE(wait_readable(p.capture->cursor().wake_fd(), 0));
}

TEST_CASE("PipeWire capture reports the cursor shape, position and visibility", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    const auto shape = cursor_pixels(PixelLayout::rgba, 8, 6, 0x80);
    CursorSpec cursor{.x = 20,
                      .y = 30,
                      .hotspot_x = 2,
                      .hotspot_y = 3,
                      .bitmap = CursorSpec::Bitmap::image,
                      .layout = PixelLayout::rgba,
                      .width = 8,
                      .height = 6,
                      .pixels = shape};

    SECTION("shape, then position-only updates")
    {
        deliver(p, {.has_frame = false, .cursor = cursor});
        auto update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        REQUIRE(update->shape.has_value());
        CHECK(update->shape->width == 8);
        CHECK(update->shape->height == 6);
        CHECK(update->shape->hotspot_x == 2);
        CHECK(update->shape->hotspot_y == 3);
        CHECK(update->shape->pixels == convert_cursor(PixelLayout::rgba, shape, 32, 8, 6, true));
        CHECK(update->position == std::pair<std::int32_t, std::int32_t>{20, 30});
        CHECK(update->visible);
        CHECK_FALSE(p.capture->frames().take_frame().has_value());  // Cursor-only buffer.

        CursorSpec moved{.x = 21, .y = 31};
        deliver(p, {.has_frame = false, .cursor = moved});
        update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK_FALSE(update->shape.has_value());
        CHECK(update->position == std::pair<std::int32_t, std::int32_t>{21, 31});
        CHECK(update->visible);

        // The same position again is no news; a frame proves it was handled.
        deliver(p, {.frame = 1, .cursor = moved});
        REQUIRE(next_frame(*p.capture).has_value());
        CHECK_FALSE(wait_readable(p.capture->cursor().wake_fd(), 0));
    }

    SECTION("updates the session did not take are merged")
    {
        deliver(p, {.has_frame = false, .cursor = cursor});
        deliver(p, {.has_frame = false, .cursor = CursorSpec{.x = 40, .y = 41}});
        deliver(p, {.frame = 1, .cursor = CursorSpec{.x = 40, .y = 41}});
        REQUIRE(next_frame(*p.capture).has_value());
        const auto update = p.capture->cursor().take_cursor();
        REQUIRE(update.has_value());
        CHECK(update->shape.has_value());
        CHECK(update->position == std::pair<std::int32_t, std::int32_t>{40, 41});
        CHECK(update->visible);
        CHECK_FALSE(p.capture->cursor().take_cursor().has_value());
    }

    SECTION("an empty bitmap hides the cursor until the next shape")
    {
        deliver(p, {.has_frame = false, .cursor = cursor});
        REQUIRE(next_cursor(*p.capture).has_value());

        deliver(p, {.has_frame = false, .cursor = CursorSpec{.x = 22, .y = 30, .bitmap = CursorSpec::Bitmap::empty}});
        auto update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK_FALSE(update->visible);
        CHECK_FALSE(update->shape.has_value());

        deliver(p, {.has_frame = false, .cursor = CursorSpec{.x = 23, .y = 30}});
        update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK_FALSE(update->visible);
        CHECK(update->position == std::pair<std::int32_t, std::int32_t>{23, 30});

        cursor.layout = PixelLayout::argb;
        cursor.pixels = cursor_pixels(PixelLayout::argb, 8, 6, 0xff);
        deliver(p, {.has_frame = false, .cursor = cursor});
        update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK(update->visible);
        REQUIRE(update->shape.has_value());
        CHECK(update->shape->pixels == convert_cursor(PixelLayout::argb, cursor.pixels, 32, 8, 6, true));
    }

    SECTION("id 0 hides the cursor, which comes back with its old shape")
    {
        deliver(p, {.has_frame = false, .cursor = cursor});
        REQUIRE(next_cursor(*p.capture).has_value());

        deliver(p, {.has_frame = false, .cursor = std::nullopt});
        auto update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK_FALSE(update->visible);
        CHECK_FALSE(update->position.has_value());

        deliver(p, {.has_frame = false, .cursor = CursorSpec{.x = 50, .y = 10}});
        update = next_cursor(*p.capture);
        REQUIRE(update.has_value());
        CHECK(update->visible);
        CHECK_FALSE(update->shape.has_value());
        CHECK(update->position == std::pair<std::int32_t, std::int32_t>{50, 10});
    }
}

TEST_CASE("PipeWire capture follows size changes", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    deliver(p, {.frame = 1});
    REQUIRE(next_frame(*p.capture).has_value());

    p.producer->resize(80, 60);
    deliver(p, {.frame = 2, .damage = std::vector<Rect>{{0, 0, 2, 2}}});
    const auto frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    check_pixels(*frame, 2, {0, 0, 80, 60});
    CHECK(frame->damage.empty());
    CHECK(p.capture->frames().size() == std::pair<std::uint32_t, std::uint32_t>{80, 60});
}

TEST_CASE("PipeWire capture applies the crop", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    const Rect crop{8, 4, 32, 24};
    deliver(p, {.frame = 1, .crop = crop});
    auto frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    check_pixels(*frame, 1, crop);
    CHECK(p.capture->frames().size() == std::pair<std::uint32_t, std::uint32_t>{32, 24});

    // Damage moves with the crop and is clipped to it; damage outside it is no frame.
    deliver(p, {.frame = 2, .damage = std::vector<Rect>{{0, 0, 4, 4}}, .crop = crop});
    deliver(p, {.frame = 3, .damage = std::vector<Rect>{{10, 10, 4, 4}, {36, 26, 10, 10}}, .crop = crop});
    frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    check_pixels(*frame, 3, crop);
    CHECK(frame->damage == std::vector<Rect>{{2, 6, 4, 4}, {28, 22, 4, 2}});
}

TEST_CASE("PipeWire capture closes when the producer goes away", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    deliver(p, {.frame = 1});
    REQUIRE(next_frame(*p.capture).has_value());
    CHECK_FALSE(p.capture->closed());

    p.producer.reset();
    REQUIRE(wait_readable(p.capture->frames().wake_fd()));
    CHECK(p.capture->closed());
    CHECK_FALSE(p.capture->error().empty());
    CHECK_FALSE(p.capture->frames().take_frame().has_value());
    // Closed stays readable, like a socket at end of file.
    CHECK(wait_readable(p.capture->frames().wake_fd(), 0));
    CHECK(wait_readable(p.capture->cursor().wake_fd(), 0));
}

TEST_CASE("PipeWire capture fails on a dead remote", "[portal][pipewire]")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) == 0);
    ::close(fds[1]);
    const auto capture = PipeWireCapture::create(fds[0], 42);
    ::close(fds[0]);
    CHECK_FALSE(capture.has_value());
}

TEST_CASE("PipeWire capture passes dmabufs on without reading them and gives each buffer back", "[portal][pipewire]")
{
    auto p = make_dmabuf_pipeline({});
    p.capture->frames().set_access(FrameAccess::dmabuf);
    // Twelve frames through four buffers: each must go back to the producer
    // once the next is taken, or the producer runs dry and deliver() fails.
    for (std::uint32_t i = 1; i <= 12; ++i) {
        CAPTURE(i);
        const Rect damage{static_cast<std::int32_t>(i), 1, 2, 2};
        deliver(p, {.frame = i, .damage = std::vector<Rect>{damage}});
        const auto frame = next_frame(*p.capture);
        REQUIRE(frame.has_value());
        CHECK(frame->image.data.empty());
        REQUIRE(frame->dmabuf.has_value());
        CHECK(frame->dmabuf->drm_format == drm_fourcc(PixelLayout::bgrx));
        CHECK(frame->dmabuf->modifier == drm_format_mod_linear);
        CHECK((frame->dmabuf->width == 64 && frame->dmabuf->height == 48));
        check_dmabuf(*frame->dmabuf, i, p.producer->stride());
        if (i == 1) {
            CHECK(frame->damage.empty());  // The first frame is all new.
        } else {
            CHECK(frame->damage == std::vector<Rect>{damage});
        }
    }
}

TEST_CASE("PipeWire capture keeps the producer going while the session holds a dmabuf", "[portal][pipewire]")
{
    auto p = make_dmabuf_pipeline({});
    p.capture->frames().set_access(FrameAccess::dmabuf);
    deliver(p, {.frame = 1});
    const auto held = next_frame(*p.capture);
    REQUIRE((held.has_value() && held->dmabuf.has_value()));

    // Nobody takes the next frames: the newest waits, older ones go straight
    // back, and their damage is merged.
    std::vector<Rect> damage;
    for (std::uint32_t i = 2; i <= 12; ++i) {
        damage.push_back({static_cast<std::int32_t>(i * 4), 0, 2, 2});
        deliver(p, {.frame = i, .damage = std::vector<Rect>{damage.back()}});
    }
    // The held buffer was never given back, so nothing was drawn into it.
    check_dmabuf(*held->dmabuf, 1, p.producer->stride());

    const auto latest = p.capture->frames().take_frame();
    REQUIRE((latest.has_value() && latest->dmabuf.has_value()));
    check_dmabuf(*latest->dmabuf, 12, p.producer->stride());
    CHECK(latest->damage == damage);
    CHECK(latest->sequence == 12);
    CHECK(latest->dmabuf->generation == held->dmabuf->generation);
}

TEST_CASE("PipeWire capture reads a held dmabuf on demand and in CPU mode", "[portal][pipewire]")
{
    auto p = make_dmabuf_pipeline({});
    auto& frames = p.capture->frames();
    frames.set_access(FrameAccess::dmabuf);
    deliver(p, {.frame = 1});
    REQUIRE(next_frame(*p.capture).value().dmabuf.has_value());

    // An encoder refused it: the session wants this one frame's pixels.
    const auto pixels = frames.map_frame();
    REQUIRE(pixels.has_value());
    check_pixels(Frame{.image = *pixels}, 1, {0, 0, 64, 48});
    frames.release_frame();
    CHECK_FALSE(frames.map_frame().has_value());

    // Back to CPU frames: read from the dmabuf (LINEAR, mmapped).
    frames.set_access(FrameAccess::cpu);
    deliver(p, {.frame = 2});
    auto frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    CHECK_FALSE(frame->dmabuf.has_value());
    check_pixels(*frame, 2, {0, 0, 64, 48});

    // A cropped frame is read too: encoders take whole buffers.
    frames.set_access(FrameAccess::dmabuf);
    const Rect crop{8, 4, 32, 24};
    deliver(p, {.frame = 3, .crop = crop});
    frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    CHECK_FALSE(frame->dmabuf.has_value());
    check_pixels(*frame, 3, crop);
    CHECK(frame->damage.empty());  // A new view: everything.
}

TEST_CASE("PipeWire capture reads dmabufs when the stream has too few buffers to hold one", "[portal][pipewire]")
{
    auto p = make_dmabuf_pipeline({.buffers = 3});
    p.capture->frames().set_access(FrameAccess::dmabuf);
    for (std::uint32_t i = 1; i <= 4; ++i) {
        deliver(p, {.frame = i});
        const auto frame = next_frame(*p.capture);
        REQUIRE(frame.has_value());
        CHECK_FALSE(frame->dmabuf.has_value());
        check_pixels(*frame, i, {0, 0, 64, 48});
    }
}

TEST_CASE("PipeWire capture marks dmabufs of renegotiated buffers with a new generation", "[portal][pipewire]")
{
    auto p = make_dmabuf_pipeline({});
    p.capture->frames().set_access(FrameAccess::dmabuf);
    deliver(p, {.frame = 1});
    const auto before = next_frame(*p.capture);
    REQUIRE((before.has_value() && before->dmabuf.has_value()));

    p.producer->resize(80, 60);
    deliver(p, {.frame = 2});
    const auto after = next_frame(*p.capture);
    REQUIRE((after.has_value() && after->dmabuf.has_value()));
    CHECK((after->dmabuf->width == 80 && after->dmabuf->height == 60));
    CHECK(after->dmabuf->generation != before->dmabuf->generation);
    CHECK(after->damage.empty());
    check_dmabuf(*after->dmabuf, 2, p.producer->stride());
}

TEST_CASE("PipeWire capture reads shared memory whatever the consumer asks for", "[portal][pipewire]")
{
    auto p = make_pipeline({});
    p.capture->frames().set_access(FrameAccess::dmabuf);
    deliver(p, {.frame = 1});
    const auto frame = next_frame(*p.capture);
    REQUIRE(frame.has_value());
    CHECK_FALSE(frame->dmabuf.has_value());
    check_pixels(*frame, 1, {0, 0, 64, 48});
}
