// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/wlroots/outputs.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <wayland-client.h>
#include <wlr-output-management-unstable-v1-client-protocol.h>

namespace farland::platform::wlroots {

namespace {

constexpr std::string_view log_component = "platform.wlroots.outputs";
constexpr auto setup_timeout = std::chrono::seconds(5);
/// Refresh of the custom modes, in mHz: the headless backend's default.
constexpr std::int32_t custom_refresh = 60'000;

bool rotated(std::int32_t transform)
{
    // WL_OUTPUT_TRANSFORM_90, _270 and their flipped variants.
    return (transform & 1) != 0;
}

}  // namespace

std::vector<std::size_t> screen_order(const std::vector<Output>& outputs)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].present && outputs[i].enabled) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, [&outputs](std::size_t a, std::size_t b) {
        const auto& ra = outputs[a].logical;
        const auto& rb = outputs[b].logical;
        return std::pair(ra.x, ra.y) < std::pair(rb.x, rb.y);
    });
    return order;
}

Rect layout_box(const std::vector<Output>& outputs)
{
    std::optional<Rect> box;
    for (const auto& o : outputs) {
        if (!o.present || !o.enabled || o.logical.width <= 0 || o.logical.height <= 0) {
            continue;
        }
        if (!box) {
            box = o.logical;
            continue;
        }
        const std::int32_t right = std::max(box->x + box->width, o.logical.x + o.logical.width);
        const std::int32_t bottom = std::max(box->y + box->height, o.logical.y + o.logical.height);
        box->x = std::min(box->x, o.logical.x);
        box->y = std::min(box->y, o.logical.y);
        box->width = right - box->x;
        box->height = bottom - box->y;
    }
    return box.value_or(Rect{});
}

struct Outputs::Impl {
    struct OutputState {
        Impl* impl = nullptr;
        std::uint32_t global = 0;
        wl_output* output = nullptr;
        std::string name;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::int32_t scale = 1;
        std::int32_t transform = 0;
        bool present = true;
    };
    struct HeadState {
        Impl* impl = nullptr;
        zwlr_output_head_v1* head = nullptr;
        std::string name;
        bool enabled = false;
        std::int32_t x = 0;
        std::int32_t y = 0;
        double scale = 1.0;
        std::int32_t transform = 0;
    };
    using Size = std::pair<std::uint32_t, std::uint32_t>;

    wayland::Connection* connection = nullptr;
    std::vector<std::unique_ptr<OutputState>> states;
    std::vector<Output> outputs;
    std::uint64_t generation = 0;

    zwlr_output_manager_v1* manager = nullptr;
    std::uint32_t manager_version = 0;
    std::vector<std::unique_ptr<HeadState>> heads;
    std::map<zwlr_output_mode_v1*, Size> modes;
    std::uint32_t serial = 0;
    std::optional<std::vector<std::pair<std::string, Size>>> wanted;
    std::vector<std::pair<std::string, Size>> sent;
    zwlr_output_configuration_v1* configuration = nullptr;
    /// Its heads: the protocol has no destructor for them, so the proxies
    /// are freed with the configuration.
    std::vector<zwlr_output_configuration_head_v1*> configuration_heads;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl()
    {
        destroy_configuration();
        for (auto& [mode, size] : modes) {
            release(mode);
        }
        for (auto& head : heads) {
            release(head->head);
        }
        if (manager != nullptr) {
            zwlr_output_manager_v1_destroy(manager);
        }
        for (auto& state : states) {
            if (state->output != nullptr) {
                release(state->output);
            }
        }
    }

    void destroy_configuration()
    {
        for (auto* head : configuration_heads) {
            zwlr_output_configuration_head_v1_destroy(head);
        }
        configuration_heads.clear();
        if (configuration != nullptr) {
            zwlr_output_configuration_v1_destroy(configuration);
            configuration = nullptr;
        }
    }

    void release(zwlr_output_mode_v1* mode) const
    {
        if (manager_version >= ZWLR_OUTPUT_MODE_V1_RELEASE_SINCE_VERSION) {
            zwlr_output_mode_v1_release(mode);
        } else {
            zwlr_output_mode_v1_destroy(mode);
        }
    }
    void release(zwlr_output_head_v1* head) const
    {
        if (manager_version >= ZWLR_OUTPUT_HEAD_V1_RELEASE_SINCE_VERSION) {
            zwlr_output_head_v1_release(head);
        } else {
            zwlr_output_head_v1_destroy(head);
        }
    }
    static void release(wl_output* output)
    {
        if (wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
            wl_output_release(output);
        } else {
            wl_output_destroy(output);
        }
    }

    /// Rebuilds `outputs` from the wl_output and head state.
    void update()
    {
        std::vector<Output> next;
        std::int32_t x = 0;
        for (const auto& s : states) {
            Output o;
            o.global = s->global;
            o.output = s->output;
            o.name = s->name;
            o.width = s->width;
            o.height = s->height;
            o.present = s->present;
            double scale = s->scale > 0 ? s->scale : 1;
            std::int32_t transform = s->transform;
            const auto head = std::ranges::find(heads, s->name, &HeadState::name);
            if (head != heads.end() && !s->name.empty()) {
                o.enabled = (*head)->enabled;
                o.logical.x = (*head)->x;
                o.logical.y = (*head)->y;
                scale = (*head)->scale > 0 ? (*head)->scale : 1.0;
                transform = (*head)->transform;
            } else {
                // Without output management: side by side in the order announced.
                o.logical.x = x;
            }
            auto w = static_cast<std::int32_t>(std::lround(s->width / scale));
            auto h = static_cast<std::int32_t>(std::lround(s->height / scale));
            if (rotated(transform)) {
                std::swap(w, h);
            }
            o.logical.width = w;
            o.logical.height = h;
            x += w;
            next.push_back(std::move(o));
        }
        const bool changed =
            next.size() != outputs.size() || !std::ranges::equal(next, outputs, [](const Output& a, const Output& b) {
                return a.output == b.output && a.name == b.name && a.width == b.width && a.height == b.height &&
                       a.logical == b.logical && a.enabled == b.enabled && a.present == b.present;
            });
        if (changed) {
            outputs = std::move(next);
            ++generation;
            for (const auto& o : outputs) {
                log::debug(log_component, "output {}: {}x{} at {},{} ({}x{} logical){}", o.name, o.width, o.height,
                           o.logical.x, o.logical.y, o.logical.width, o.logical.height, o.present ? "" : ", removed");
            }
        }
    }

    void bind_output(const wayland::Global& global)
    {
        auto state = std::make_unique<OutputState>();
        state->impl = this;
        state->global = global.name;
        state->output = connection->bind<wl_output>(global, &wl_output_interface, 4);
        static constexpr wl_output_listener listener{
            .geometry = [](void* data, wl_output* /*output*/, std::int32_t /*x*/, std::int32_t /*y*/,
                           std::int32_t /*physical_width*/, std::int32_t /*physical_height*/, std::int32_t /*subpixel*/,
                           const char* /*make*/, const char* /*model*/,
                           std::int32_t transform) { static_cast<OutputState*>(data)->transform = transform; },
            .mode =
                [](void* data, wl_output* /*output*/, std::uint32_t flags, std::int32_t width, std::int32_t height,
                   std::int32_t /*refresh*/) {
                    if ((flags & WL_OUTPUT_MODE_CURRENT) != 0 && width > 0 && height > 0) {
                        auto* s = static_cast<OutputState*>(data);
                        s->width = static_cast<std::uint32_t>(width);
                        s->height = static_cast<std::uint32_t>(height);
                    }
                },
            .done = [](void* data, wl_output* /*output*/) { static_cast<OutputState*>(data)->impl->update(); },
            .scale = [](void* data, wl_output* /*output*/,
                        std::int32_t factor) { static_cast<OutputState*>(data)->scale = factor; },
            .name = [](void* data, wl_output* /*output*/,
                       const char* name) { static_cast<OutputState*>(data)->name = name; },
            .description = [](void* /*data*/, wl_output* /*output*/, const char* /*description*/) {},
        };
        wl_output_add_listener(state->output, &listener, state.get());
        states.push_back(std::move(state));
    }

    void bind_manager(const wayland::Global& global)
    {
        manager_version = std::min<std::uint32_t>(global.version, 4);
        manager = connection->bind<zwlr_output_manager_v1>(global, &zwlr_output_manager_v1_interface, 4);
        static constexpr zwlr_output_manager_v1_listener listener{
            .head = [](void* data, zwlr_output_manager_v1* /*manager*/,
                       zwlr_output_head_v1* head) { static_cast<Impl*>(data)->add_head(head); },
            .done =
                [](void* data, zwlr_output_manager_v1* /*manager*/, std::uint32_t done_serial) {
                    auto* self = static_cast<Impl*>(data);
                    self->serial = done_serial;
                    self->update();
                    self->try_apply();
                },
            .finished =
                [](void* data, zwlr_output_manager_v1* /*manager*/) {
                    auto* self = static_cast<Impl*>(data);
                    zwlr_output_manager_v1_destroy(self->manager);
                    self->manager = nullptr;
                },
        };
        zwlr_output_manager_v1_add_listener(manager, &listener, this);
    }

    void add_head(zwlr_output_head_v1* head)
    {
        auto state = std::make_unique<HeadState>();
        state->impl = this;
        state->head = head;
        static constexpr zwlr_output_mode_v1_listener mode_listener{
            .size =
                [](void* data, zwlr_output_mode_v1* mode, std::int32_t width, std::int32_t height) {
                    static_cast<Impl*>(data)->modes[mode] = Size{static_cast<std::uint32_t>(std::max(width, 0)),
                                                                 static_cast<std::uint32_t>(std::max(height, 0))};
                },
            .refresh = [](void* /*data*/, zwlr_output_mode_v1* /*mode*/, std::int32_t /*refresh*/) {},
            .preferred = [](void* /*data*/, zwlr_output_mode_v1* /*mode*/) {},
            .finished =
                [](void* data, zwlr_output_mode_v1* mode) {
                    auto* self = static_cast<Impl*>(data);
                    self->modes.erase(mode);
                    self->release(mode);
                },
        };
        static constexpr zwlr_output_head_v1_listener listener{
            .name = [](void* data, zwlr_output_head_v1* /*head*/,
                       const char* name) { static_cast<HeadState*>(data)->name = name; },
            .description = [](void* /*data*/, zwlr_output_head_v1* /*head*/, const char* /*description*/) {},
            .physical_size = [](void* /*data*/, zwlr_output_head_v1* /*head*/, std::int32_t /*width*/,
                                std::int32_t /*height*/) {},
            .mode =
                [](void* data, zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* mode) {
                    auto* impl = static_cast<HeadState*>(data)->impl;
                    impl->modes.emplace(mode, Size{});
                    zwlr_output_mode_v1_add_listener(mode, &mode_listener, impl);
                },
            .enabled = [](void* data, zwlr_output_head_v1* /*head*/,
                          std::int32_t enabled) { static_cast<HeadState*>(data)->enabled = enabled != 0; },
            .current_mode = [](void* /*data*/, zwlr_output_head_v1* /*head*/, zwlr_output_mode_v1* /*mode*/) {},
            .position =
                [](void* data, zwlr_output_head_v1* /*head*/, std::int32_t x, std::int32_t y) {
                    auto* s = static_cast<HeadState*>(data);
                    s->x = x;
                    s->y = y;
                },
            .transform = [](void* data, zwlr_output_head_v1* /*head*/,
                            std::int32_t transform) { static_cast<HeadState*>(data)->transform = transform; },
            .scale = [](void* data, zwlr_output_head_v1* /*head*/,
                        wl_fixed_t scale) { static_cast<HeadState*>(data)->scale = wl_fixed_to_double(scale); },
            .finished =
                [](void* data, zwlr_output_head_v1* finished) {
                    auto* impl = static_cast<HeadState*>(data)->impl;
                    impl->release(finished);
                    // Destroys the HeadState.
                    std::erase_if(impl->heads, [finished](const auto& h) { return h->head == finished; });
                },
            .make = [](void* /*data*/, zwlr_output_head_v1* /*head*/, const char* /*make*/) {},
            .model = [](void* /*data*/, zwlr_output_head_v1* /*head*/, const char* /*model*/) {},
            .serial_number = [](void* /*data*/, zwlr_output_head_v1* /*head*/, const char* /*serial*/) {},
            .adaptive_sync = [](void* /*data*/, zwlr_output_head_v1* /*head*/, std::uint32_t /*state*/) {},
        };
        zwlr_output_head_v1_add_listener(head, &listener, state.get());
        heads.push_back(std::move(state));
    }

    [[nodiscard]] bool needs_change(const std::vector<std::pair<std::string, Size>>& sizes) const
    {
        return std::ranges::any_of(sizes, [this](const auto& wanted_size) {
            const auto it = std::ranges::find(outputs, wanted_size.first, &Output::name);
            return it == outputs.end() || std::pair(it->width, it->height) != wanted_size.second;
        });
    }

    void try_apply()
    {
        if (!wanted || configuration != nullptr || manager == nullptr || serial == 0) {
            return;
        }
        if (!needs_change(*wanted)) {
            wanted.reset();
            return;
        }
        sent = std::move(*wanted);
        wanted.reset();
        configuration = zwlr_output_manager_v1_create_configuration(manager, serial);
        static constexpr zwlr_output_configuration_v1_listener listener{
            .succeeded = [](void* data,
                            zwlr_output_configuration_v1* /*configuration*/) { static_cast<Impl*>(data)->finish(""); },
            .failed =
                [](void* data, zwlr_output_configuration_v1* /*configuration*/) {
                    static_cast<Impl*>(data)->finish("the compositor refused the new output sizes");
                },
            .cancelled =
                [](void* data, zwlr_output_configuration_v1* /*configuration*/) {
                    // The outputs changed meanwhile: try again with the new state.
                    auto* self = static_cast<Impl*>(data);
                    if (!self->wanted) {
                        self->wanted = self->sent;
                    }
                    self->finish("");
                },
        };
        zwlr_output_configuration_v1_add_listener(configuration, &listener, this);
        // The screens named, left to right at their new logical widths;
        // other enabled outputs keep their place.
        std::int32_t x = 0;
        for (const auto& [name, size] : sent) {
            const auto head = std::ranges::find(heads, name, &HeadState::name);
            if (head == heads.end() || !(*head)->enabled) {
                continue;
            }
            auto* config = zwlr_output_configuration_v1_enable_head(configuration, (*head)->head);
            configuration_heads.push_back(config);
            zwlr_output_configuration_head_v1_set_custom_mode(config, static_cast<std::int32_t>(size.first),
                                                              static_cast<std::int32_t>(size.second), custom_refresh);
            zwlr_output_configuration_head_v1_set_position(config, x, 0);
            const double scale = (*head)->scale > 0 ? (*head)->scale : 1.0;
            x += static_cast<std::int32_t>(
                std::lround((rotated((*head)->transform) ? size.second : size.first) / scale));
            log::info(log_component, "resizing output {} to {}x{}", name, size.first, size.second);
        }
        for (const auto& head : heads) {
            if (std::ranges::find(sent, head->name, &std::pair<std::string, Size>::first) != sent.end() &&
                head->enabled) {
                continue;
            }
            if (head->enabled) {
                configuration_heads.push_back(zwlr_output_configuration_v1_enable_head(configuration, head->head));
            } else {
                zwlr_output_configuration_v1_disable_head(configuration, head->head);
            }
        }
        zwlr_output_configuration_v1_apply(configuration);
        connection->flush();
    }

    void finish(std::string_view failure)
    {
        if (!failure.empty()) {
            log::warn(log_component, "{}", failure);
        }
        destroy_configuration();
        try_apply();
    }
};

Result<std::unique_ptr<Outputs>> Outputs::create(wayland::Connection& connection)
{
    auto impl = std::make_unique<Impl>();
    impl->connection = &connection;
    for (const auto& global : connection.globals()) {
        if (global.interface == "wl_output") {
            impl->bind_output(global);
        }
    }
    if (const auto* manager = connection.find("zwlr_output_manager_v1")) {
        impl->bind_manager(*manager);
    }
    if (impl->states.empty()) {
        return fail(Errc::unsupported, "the compositor has no outputs");
    }
    // The output events, then the heads with their modes.
    auto* raw = impl.get();
    if (!connection.wait_until(
            [raw] {
                return std::ranges::all_of(raw->states, [](const auto& s) { return s->width > 0; }) &&
                       (raw->manager == nullptr || raw->serial != 0);
            },
            setup_timeout)) {
        return fail(Errc::io, "the compositor did not describe its outputs");
    }
    raw->update();
    connection.on_global_removed([raw](std::uint32_t name) {
        for (auto& state : raw->states) {
            if (state->global == name && state->present) {
                state->present = false;
                raw->update();
            }
        }
    });
    return std::unique_ptr<Outputs>(new Outputs(std::move(impl)));
}

Outputs::Outputs(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Outputs::~Outputs() = default;

const std::vector<Output>& Outputs::list() const noexcept
{
    return impl_->outputs;
}

std::uint64_t Outputs::generation() const noexcept
{
    return impl_->generation;
}

bool Outputs::can_resize() const noexcept
{
    return impl_->manager != nullptr;
}

bool Outputs::resizing() const noexcept
{
    return impl_->wanted.has_value() || impl_->configuration != nullptr;
}

void Outputs::request_sizes(std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> sizes)
{
    impl_->wanted = std::move(sizes);
    impl_->try_apply();
}

}  // namespace farland::platform::wlroots
