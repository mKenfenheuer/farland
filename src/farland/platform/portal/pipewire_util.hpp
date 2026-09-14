// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// Small helpers around libpipewire for C++ callers (internal to the portal
/// backend and its tests).
namespace farland::platform::portal::pw {

// PipeWire's method "calls" are macros built from GNU statement expressions and
// C casts, which farland's warning flags reject. They are wrapped here, once.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef __clang__
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#pragma clang diagnostic ignored "-Wgnu-statement-expression"
#endif
// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast)

inline int core_add_listener(pw_core* core, spa_hook* hook, const pw_core_events* events, void* data)
{
    return pw_core_add_listener(core, hook, events, data);
}

inline int core_sync(pw_core* core, std::uint32_t id, int seq)
{
    return pw_core_sync(core, id, seq);
}

inline pw_registry* core_get_registry(pw_core* core)
{
    return pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
}

inline int registry_add_listener(pw_registry* registry, spa_hook* hook, const pw_registry_events* events, void* data)
{
    return pw_registry_add_listener(registry, hook, events, data);
}

inline spa_source* loop_add_event(pw_loop* loop, spa_source_event_func_t func, void* data)
{
    return pw_loop_add_event(loop, func, data);
}

inline int loop_signal_event(pw_loop* loop, spa_source* source)
{
    return pw_loop_signal_event(loop, source);
}

inline void loop_destroy_source(pw_loop* loop, spa_source* source)
{
    pw_loop_destroy_source(loop, source);
}

// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-reinterpret-cast)
#pragma GCC diagnostic pop

/// Holds a pw_thread_loop's lock for its lifetime.
class LoopLock {
public:
    explicit LoopLock(pw_thread_loop* loop) : loop_(loop) { pw_thread_loop_lock(loop_); }
    ~LoopLock() { pw_thread_loop_unlock(loop_); }
    LoopLock(const LoopLock&) = delete;
    LoopLock& operator=(const LoopLock&) = delete;
    LoopLock(LoopLock&&) = delete;
    LoopLock& operator=(LoopLock&&) = delete;

private:
    pw_thread_loop* loop_;
};

/// A spa_pod_builder over a fixed buffer, so that the pods it returns stay
/// valid while the builder lives.
class PodBuilder {
public:
    explicit PodBuilder(std::size_t capacity = 16384) : buffer_(capacity)
    {
        spa_pod_builder_init(&builder_, buffer_.data(), static_cast<std::uint32_t>(buffer_.size()));
    }
    PodBuilder(const PodBuilder&) = delete;
    PodBuilder& operator=(const PodBuilder&) = delete;
    PodBuilder(PodBuilder&&) = delete;
    PodBuilder& operator=(PodBuilder&&) = delete;
    ~PodBuilder() = default;

    [[nodiscard]] spa_pod_builder* get() noexcept { return &builder_; }
    /// True if something did not fit; the pods built so far are then unusable.
    [[nodiscard]] bool overflowed() const noexcept { return builder_.state.offset > builder_.size; }

    void push_object(spa_pod_frame* frame, std::uint32_t type, std::uint32_t id)
    {
        spa_pod_builder_push_object(&builder_, frame, type, id);
    }
    void push_choice(spa_pod_frame* frame, std::uint32_t type)
    {
        spa_pod_builder_push_choice(&builder_, frame, type, 0);
    }
    [[nodiscard]] const spa_pod* pop(spa_pod_frame* frame)
    {
        return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder_, frame));
    }
    void prop(std::uint32_t key, std::uint32_t flags = 0) { spa_pod_builder_prop(&builder_, key, flags); }
    void id(std::uint32_t value) { spa_pod_builder_id(&builder_, value); }
    void int_(std::int32_t value) { spa_pod_builder_int(&builder_, value); }
    void long_(std::int64_t value) { spa_pod_builder_long(&builder_, value); }
    void rectangle(std::uint32_t width, std::uint32_t height) { spa_pod_builder_rectangle(&builder_, width, height); }
    void fraction(std::uint32_t num, std::uint32_t denom) { spa_pod_builder_fraction(&builder_, num, denom); }

    /// key: Int range (default, min, max).
    void int_range(std::uint32_t key, std::int32_t def, std::int32_t min, std::int32_t max)
    {
        spa_pod_frame f{};
        prop(key);
        push_choice(&f, SPA_CHOICE_Range);
        int_(def);
        int_(min);
        int_(max);
        static_cast<void>(pop(&f));
    }
    /// A SPA_PARAM_Meta object for `type` with a fixed size.
    [[nodiscard]] const spa_pod* meta(std::uint32_t type, std::int32_t size)
    {
        spa_pod_frame f{};
        push_object(&f, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
        prop(SPA_PARAM_META_type);
        id(type);
        prop(SPA_PARAM_META_size);
        int_(size);
        return pop(&f);
    }
    /// A SPA_PARAM_Meta object for `type` with a size range.
    [[nodiscard]] const spa_pod* meta_range(std::uint32_t type, std::int32_t def, std::int32_t min, std::int32_t max)
    {
        spa_pod_frame f{};
        push_object(&f, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
        prop(SPA_PARAM_META_type);
        id(type);
        int_range(SPA_PARAM_META_size, def, min, max);
        return pop(&f);
    }

private:
    std::vector<std::byte> buffer_;
    spa_pod_builder builder_{};
};

}  // namespace farland::platform::portal::pw
