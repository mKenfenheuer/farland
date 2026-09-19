// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/log.hpp>
#include <farland/platform/portal/pipewire_util.hpp>

#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

/// The plumbing shared by farland's own PipeWire streams: the virtual
/// microphone and the capture of the desktop's output (pipewire_audio.cpp),
/// and the virtual camera (pipewire_camera.cpp). Not for the screen cast,
/// which runs on the portal's remote and has its own connection.
namespace farland::platform::portal::pw_host {

inline constexpr std::string_view log_component = "platform.pipewire";
/// How long the PipeWire daemon may take to answer.
inline constexpr int connect_timeout_seconds = 5;

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

/// The first data block of a PipeWire buffer, if it is mapped.
inline spa_data* first_data(pw_buffer* buffer)
{
    const spa_buffer* b = buffer->buffer;
    if (b == nullptr || b->n_datas == 0) {
        return nullptr;
    }
    spa_data& d = std::span(b->datas, b->n_datas).front();
    return d.data != nullptr && d.chunk != nullptr ? &d : nullptr;
}

/// A pw_thread_loop connected to the user's PipeWire daemon, and the one
/// stream on it. The owner registers the stream's events with itself as data
/// and calls shutdown() first thing in its destructor: stream callbacks may
/// run until the stream is destroyed.
class StreamHost {
public:
    StreamHost() = default;
    StreamHost(const StreamHost&) = delete;
    StreamHost& operator=(const StreamHost&) = delete;
    StreamHost(StreamHost&&) = delete;
    StreamHost& operator=(StreamHost&&) = delete;
    ~StreamHost() { shutdown(); }

    /// Starts the loop and connects; on return the loop runs, unlocked.
    [[nodiscard]] Result<void> connect(const char* thread_name, int wake_on_close)
    {
        wake_on_close_ = wake_on_close;
        pw_init(nullptr, nullptr);
        initialized_ = true;
        loop_ = pw_thread_loop_new(thread_name, nullptr);
        if (loop_ == nullptr) {
            return fail(Errc::io, "cannot create the PipeWire thread loop");
        }
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (context_ == nullptr) {
            return fail(Errc::io, "cannot create a PipeWire context");
        }
        if (pw_thread_loop_start(loop_) < 0) {
            return fail(Errc::io, "cannot start the PipeWire thread loop");
        }
        const pw::LoopLock lock(loop_);
        // The user's own daemon: $PIPEWIRE_REMOTE or pipewire-0 in $XDG_RUNTIME_DIR.
        core_ = pw_context_connect(context_, nullptr, 0);
        if (core_ == nullptr) {
            log::warn(log_component, "cannot connect to PipeWire: {}", std::strerror(errno));
            return fail(Errc::io, "cannot connect to the user's PipeWire daemon");
        }
        pw::core_add_listener(core_, &core_listener_, &core_events(), this);
        sync_seq_ = pw::core_sync(core_, PW_ID_CORE, 0);
        while (!sync_done_) {
            if (closed()) {
                return fail(Errc::io, "the PipeWire connection failed");
            }
            if (pw_thread_loop_timed_wait(loop_, connect_timeout_seconds) != 0) {
                return fail(Errc::io, "PipeWire did not answer");
            }
        }
        return {};
    }

    /// Destroys the stream and the connection. Idempotent.
    void shutdown()
    {
        if (loop_ != nullptr) {
            {
                const pw::LoopLock lock(loop_);
                if (stream_ != nullptr) {
                    spa_hook_remove(&stream_listener_);
                    pw_stream_destroy(stream_);
                    stream_ = nullptr;
                }
                if (core_ != nullptr) {
                    spa_hook_remove(&core_listener_);
                    pw_core_disconnect(core_);
                    core_ = nullptr;
                }
            }
            pw_thread_loop_stop(loop_);
        }
        if (context_ != nullptr) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        if (loop_ != nullptr) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        if (initialized_) {
            pw_deinit();
            initialized_ = false;
        }
    }

    /// Creates the stream and connects it to PW_ID_ANY, which the session
    /// manager links. `params` are the EnumFormat pods the caller built.
    /// Takes the loop lock.
    [[nodiscard]] Result<void> start_stream(const char* name, std::span<const spa_dict_item> properties,
                                            const pw_stream_events& events, void* data, pw_direction direction,
                                            std::span<const spa_pod* const> params)
    {
        const pw::LoopLock lock(loop_);
        const spa_dict dict{0, static_cast<std::uint32_t>(properties.size()), properties.data()};
        stream_ = pw_stream_new(core_, name, pw_properties_new_dict(&dict));
        if (stream_ == nullptr) {
            return fail(Errc::io, "cannot create a PipeWire stream");
        }
        pw_stream_add_listener(stream_, &stream_listener_, &events, data);
        // pw_stream_flags is a bit mask.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                        PW_STREAM_FLAG_RT_PROCESS);
        if (pw_stream_connect(
                stream_, direction, PW_ID_ANY, flags,
                const_cast<const spa_pod**>(params.data()),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
                static_cast<std::uint32_t>(params.size())) < 0) {
            return fail(Errc::io, "cannot connect the PipeWire stream");
        }
        return {};
    }

    [[nodiscard]] pw_stream* stream() const noexcept { return stream_; }

    void close(std::string why)
    {
        {
            const std::scoped_lock lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            error_ = std::move(why);
            log::warn(log_component, "{}", error_);
        }
        if (wake_on_close_ >= 0) {
            static_cast<void>(::eventfd_write(wake_on_close_, 1));
        }
    }
    [[nodiscard]] bool closed() const
    {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }
    [[nodiscard]] std::string error() const
    {
        const std::scoped_lock lock(mutex_);
        return error_;
    }

    /// For the owner's stream events.
    void state_changed(pw_stream_state old, pw_stream_state state, const char* message)
    {
        log::debug(log_component, "stream state {} -> {}", pw_stream_state_as_string(old),
                   pw_stream_state_as_string(state));
        if (state == PW_STREAM_STATE_ERROR) {
            close(std::format("PipeWire stream error: {}", message != nullptr ? message : "unknown"));
        } else if (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED) {
            close("the PipeWire stream was disconnected");
        }
    }

private:
    static void on_core_done(void* data, std::uint32_t id, int seq)
    {
        auto& self = *static_cast<StreamHost*>(data);
        if (id == PW_ID_CORE && seq == self.sync_seq_) {
            self.sync_done_ = true;
            pw_thread_loop_signal(self.loop_, false);
        }
    }

    static void on_core_error(void* data, std::uint32_t id, int /*seq*/, int /*res*/, const char* message)
    {
        auto& self = *static_cast<StreamHost*>(data);
        if (id == PW_ID_CORE) {
            self.close(std::format("PipeWire: {}", message != nullptr ? message : "connection error"));
            pw_thread_loop_signal(self.loop_, false);
        }
    }

    static const pw_core_events& core_events()
    {
        static const pw_core_events events = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = &StreamHost::on_core_done;
            e.error = &StreamHost::on_core_error;
            return e;
        }();
        return events;
    }

    bool initialized_ = false;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook core_listener_{};
    spa_hook stream_listener_{};
    int sync_seq_ = 0;
    bool sync_done_ = false;
    int wake_on_close_ = -1;
    mutable std::mutex mutex_;
    bool closed_ = false;
    std::string error_;
};

}  // namespace farland::platform::portal::pw_host
