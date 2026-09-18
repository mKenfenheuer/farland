// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "fake_kwin.hpp"

#include <farland/platform/kwin/wayland_connection.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <ext-data-control-v1-server-protocol.h>
#include <fcntl.h>
#include <future>
#include <kde-output-device-v2-server-protocol.h>
#include <kde-output-management-v2-server-protocol.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <wayland-server.h>
#include <zkde-screencast-unstable-v1-server-protocol.h>

namespace farland::test {

using platform::kwin::ignore_event;

namespace {

constexpr std::uint32_t custom_modes_capability = 0x2000;
constexpr std::int32_t refresh = 60000;

struct Mode {
    wl_resource* resource = nullptr;
    int width = 0;
    int height = 0;
};

struct ModeList {
    int width = 0;
    int height = 0;
    std::vector<std::pair<int, int>> modes;
};

struct Configuration {
    FakeKWin::State* state = nullptr;
    std::optional<std::vector<std::pair<int, int>>> custom;
    wl_resource* mode = nullptr;
};

struct OfferContext {
    FakeKWin::State* state = nullptr;
    /// The client's source it stands for; null for the desktop's selection.
    wl_resource* source = nullptr;
};

struct SourceContext {
    std::vector<std::string> mime_types;
    FakeKWin::State* state = nullptr;
};

template <typename T>
T& context(wl_resource* resource)
{
    return *static_cast<T*>(wl_resource_get_user_data(resource));
}

template <typename T>
void destroy_data(wl_resource* resource)
{
    delete static_cast<T*>(wl_resource_get_user_data(resource));
}

void destroy_resource(wl_client* /*client*/, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

}  // namespace

struct FakeKWin::State {
    Options options;
    wl_display* display = nullptr;
    wl_client* client = nullptr;
    int client_fd = -1;
    int wake_fd = -1;
    wl_event_source* wake_source = nullptr;
    std::vector<wl_global*> globals;
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
    std::atomic<bool> stop{false};
    std::thread thread;

    std::vector<wl_resource*> outputs;
    std::vector<wl_resource*> devices;
    std::vector<Mode> modes;
    std::size_t current = 0;
    int configurations = 0;
    int custom_mode_lists = 0;

    std::vector<wl_resource*> streams;
    std::uint32_t next_node = 40;

    std::vector<wl_resource*> data_devices;
    wl_resource* client_source = nullptr;
    std::optional<std::pair<std::vector<std::string>, std::string>> desktop;

    static State& of(wl_resource* resource) { return *static_cast<State*>(wl_resource_get_user_data(resource)); }

    void forget(wl_resource* resource)
    {
        for (auto* list : {&outputs, &devices, &streams, &data_devices}) {
            std::erase(*list, resource);
        }
        if (client_source == resource) {
            client_source = nullptr;
        }
    }

    // --- wl_output, wl_seat

    void send_output(wl_resource* output)
    {
        const auto& mode = modes.at(current);
        wl_output_send_mode(output, WL_OUTPUT_MODE_CURRENT, mode.width, mode.height, refresh);
        if (wl_resource_get_version(output) >= WL_OUTPUT_DONE_SINCE_VERSION) {
            wl_output_send_done(output);
        }
    }

    // --- kde_output_device_v2

    void add_mode(int width, int height)
    {
        Mode mode{nullptr, width, height};
        if (!devices.empty()) {
            auto* device = devices.front();
            mode.resource =
                wl_resource_create(client, &kde_output_device_mode_v2_interface, wl_resource_get_version(device), 0);
            wl_resource_set_implementation(mode.resource, nullptr, nullptr, nullptr);
            kde_output_device_v2_send_mode(device, mode.resource);
            kde_output_device_mode_v2_send_size(mode.resource, width, height);
            kde_output_device_mode_v2_send_refresh(mode.resource, refresh);
        }
        modes.push_back(mode);
    }

    void apply(wl_resource* resource)
    {
        auto& configuration = context<Configuration>(resource);
        ++configurations;
        if (options.refuse_configurations) {
            kde_output_configuration_v2_send_failed(resource);
            return;
        }
        auto* device = devices.empty() ? nullptr : devices.front();
        if (configuration.custom) {
            ++custom_mode_lists;
            for (const auto& [width, height] : *configuration.custom) {
                add_mode(width, height);
            }
        }
        if (configuration.mode != nullptr) {
            const auto it = std::ranges::find(modes, configuration.mode, &Mode::resource);
            if (it != modes.end()) {
                current = static_cast<std::size_t>(it - modes.begin());
                if (device != nullptr) {
                    kde_output_device_v2_send_current_mode(device, configuration.mode);
                }
                for (auto* output : outputs) {
                    send_output(output);
                }
            }
        }
        if (device != nullptr) {
            kde_output_device_v2_send_done(device);
        }
        kde_output_configuration_v2_send_applied(resource);
    }

    // --- ext-data-control

    void announce(wl_resource* device)
    {
        wl_resource* source = client_source;
        const std::vector<std::string>* types = nullptr;
        if (source != nullptr) {
            types = &context<SourceContext>(source).mime_types;
        } else if (desktop) {
            types = &desktop->first;
        }
        if (types == nullptr) {
            ext_data_control_device_v1_send_selection(device, nullptr);
            return;
        }
        auto* offer =
            wl_resource_create(client, &ext_data_control_offer_v1_interface, wl_resource_get_version(device), 0);
        auto* offer_data = new OfferContext{this, source};
        wl_resource_set_implementation(offer, &offer_context_impl, offer_data,
                                       [](wl_resource* r) { destroy_data<OfferContext>(r); });
        ext_data_control_device_v1_send_data_offer(device, offer);
        for (const auto& mime : *types) {
            ext_data_control_offer_v1_send_offer(offer, mime.c_str());
        }
        ext_data_control_device_v1_send_selection(device, offer);
    }

    void announce_all()
    {
        for (auto* device : data_devices) {
            announce(device);
        }
    }

    static const struct ext_data_control_offer_v1_interface offer_context_impl;
};

const struct ext_data_control_offer_v1_interface FakeKWin::State::offer_context_impl{
    .receive =
        [](wl_client* /*client*/, wl_resource* resource, const char* mime_type, std::int32_t fd) {
            auto& offer = context<OfferContext>(resource);
            auto& state = *offer.state;
            if (offer.source == nullptr) {
                if (state.desktop) {
                    const auto& text = state.desktop->second;
                    [[maybe_unused]] const auto io = ::write(fd, text.data(), text.size());
                }
            } else if (offer.source == state.client_source) {
                ext_data_control_source_v1_send_send(offer.source, mime_type, fd);
            }
            ::close(fd);
        },
    .destroy = &destroy_resource,
};

FakeKWin::FakeKWin() : FakeKWin(Options{}) {}

FakeKWin::FakeKWin(Options options) : state_(std::make_unique<State>())
{
    auto& s = *state_;
    s.options = std::move(options);
    s.modes.push_back(Mode{nullptr, s.options.width, s.options.height});
    s.display = wl_display_create();
    std::array<int, 2> fds{-1, -1};
    if (s.display == nullptr || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) != 0) {
        throw std::runtime_error("cannot create the fake KWin");
    }
    s.client_fd = fds[0];
    s.client = wl_client_create(s.display, fds[1]);
    s.wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    s.wake_source = wl_event_loop_add_fd(
        wl_display_get_event_loop(s.display), s.wake_fd, WL_EVENT_READABLE,
        [](int fd, std::uint32_t /*mask*/, void* data) -> int {
            std::uint64_t count = 0;
            [[maybe_unused]] const auto io = ::read(fd, &count, sizeof(count));
            auto& state = *static_cast<State*>(data);
            for (;;) {
                std::function<void()> task;
                {
                    const std::scoped_lock lock(state.mutex);
                    if (state.tasks.empty()) {
                        break;
                    }
                    task = std::move(state.tasks.front());
                    state.tasks.pop_front();
                }
                task();
            }
            return 0;
        },
        &s);

    // wl_output
    s.globals.push_back(wl_global_create(
        s.display, &wl_output_interface, 4, &s,
        [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto& state = *static_cast<State*>(data);
            auto* resource = wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
            static const struct wl_output_interface impl{.release = &destroy_resource};
            wl_resource_set_implementation(resource, &impl, &state, [](wl_resource* r) { State::of(r).forget(r); });
            state.outputs.push_back(resource);
            wl_output_send_geometry(resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "farland", "fake",
                                    WL_OUTPUT_TRANSFORM_NORMAL);
            if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
                wl_output_send_scale(resource, 1);
                wl_output_send_name(resource, state.options.output_name.c_str());
                wl_output_send_description(resource, "a fake output");
            }
            state.send_output(resource);
        }));

    // wl_seat
    s.globals.push_back(wl_global_create(s.display, &wl_seat_interface, 5, &s,
                                         [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
                                             auto* resource = wl_resource_create(client, &wl_seat_interface,
                                                                                 static_cast<int>(version), id);
                                             static const struct wl_seat_interface impl{
                                                 .get_pointer = ignore_event,
                                                 .get_keyboard = ignore_event,
                                                 .get_touch = ignore_event,
                                                 .release = &destroy_resource,
                                             };
                                             wl_resource_set_implementation(resource, &impl, data, nullptr);
                                             wl_seat_send_capabilities(resource, 0);
                                             if (version >= WL_SEAT_NAME_SINCE_VERSION) {
                                                 wl_seat_send_name(resource, "seat0");
                                             }
                                         }));

    // zkde_screencast_unstable_v1
    if (s.options.screencast) {
        s.globals.push_back(wl_global_create(
            s.display, &zkde_screencast_unstable_v1_interface, 5, &s,
            [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
                auto* resource =
                    wl_resource_create(client, &zkde_screencast_unstable_v1_interface, static_cast<int>(version), id);
                static const struct zkde_screencast_unstable_v1_interface impl{
                    .stream_output =
                        [](wl_client* c, wl_resource* manager, std::uint32_t stream_id, wl_resource* /*output*/,
                           std::uint32_t /*pointer*/) {
                            auto& state = State::of(manager);
                            auto* stream = wl_resource_create(c, &zkde_screencast_stream_unstable_v1_interface,
                                                              wl_resource_get_version(manager), stream_id);
                            static const struct zkde_screencast_stream_unstable_v1_interface stream_impl{
                                .close = &destroy_resource};
                            wl_resource_set_implementation(stream, &stream_impl, &state,
                                                           [](wl_resource* r) { State::of(r).forget(r); });
                            state.streams.push_back(stream);
                            if (state.options.fail_streams) {
                                zkde_screencast_stream_unstable_v1_send_failed(stream, "no screen casting here");
                            } else {
                                zkde_screencast_stream_unstable_v1_send_created(stream, state.next_node++);
                            }
                        },
                    .stream_window = ignore_event,
                    .destroy = &destroy_resource,
                    .stream_virtual_output = ignore_event,
                    .stream_region = ignore_event,
                    .stream_virtual_output_with_description = ignore_event,
                };
                wl_resource_set_implementation(resource, &impl, data, nullptr);
            }));
    }

    // kde_output_device_v2
    s.globals.push_back(wl_global_create(
        s.display, &kde_output_device_v2_interface, 20, &s,
        [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto& state = *static_cast<State*>(data);
            auto* device = wl_resource_create(client, &kde_output_device_v2_interface, static_cast<int>(version), id);
            wl_resource_set_implementation(device, nullptr, &state, [](wl_resource* r) { State::of(r).forget(r); });
            state.devices.push_back(device);
            kde_output_device_v2_send_name(device, state.options.output_name.c_str());
            kde_output_device_v2_send_capabilities(device, custom_modes_capability);
            auto modes = std::move(state.modes);
            state.modes.clear();
            for (const auto& mode : modes) {
                state.add_mode(mode.width, mode.height);
            }
            kde_output_device_v2_send_current_mode(device, state.modes.at(state.current).resource);
            kde_output_device_v2_send_done(device);
        }));

    // kde_output_management_v2
    s.globals.push_back(wl_global_create(
        s.display, &kde_output_management_v2_interface, 19, &s,
        [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource =
                wl_resource_create(client, &kde_output_management_v2_interface, static_cast<int>(version), id);
            static const struct kde_output_management_v2_interface impl{
                .create_configuration =
                    [](wl_client* c, wl_resource* manager, std::uint32_t configuration_id) {
                        auto* configuration = wl_resource_create(c, &kde_output_configuration_v2_interface,
                                                                 wl_resource_get_version(manager), configuration_id);
                        static const struct kde_output_configuration_v2_interface configuration_impl{
                            .enable = ignore_event,
                            .mode = [](wl_client* /*c*/, wl_resource* r, wl_resource* /*device*/,
                                       wl_resource* mode) { context<Configuration>(r).mode = mode; },
                            .transform = ignore_event,
                            .position = ignore_event,
                            .scale = ignore_event,
                            .apply = [](wl_client* /*c*/,
                                        wl_resource* r) { context<Configuration>(r).state->apply(r); },
                            .destroy = &destroy_resource,
                            .overscan = ignore_event,
                            .set_vrr_policy = ignore_event,
                            .set_rgb_range = ignore_event,
                            .set_primary_output = ignore_event,
                            .set_priority = ignore_event,
                            .set_high_dynamic_range = ignore_event,
                            .set_sdr_brightness = ignore_event,
                            .set_wide_color_gamut = ignore_event,
                            .set_auto_rotate_policy = ignore_event,
                            .set_icc_profile_path = ignore_event,
                            .set_brightness_overrides = ignore_event,
                            .set_sdr_gamut_wideness = ignore_event,
                            .set_color_profile_source = ignore_event,
                            .set_brightness = ignore_event,
                            .set_color_power_tradeoff = ignore_event,
                            .set_dimming = ignore_event,
                            .set_replication_source = ignore_event,
                            .set_ddc_ci_allowed = ignore_event,
                            .set_max_bits_per_color = ignore_event,
                            .set_edr_policy = ignore_event,
                            .set_sharpness = ignore_event,
                            .set_custom_modes =
                                [](wl_client* /*c*/, wl_resource* r, wl_resource* /*device*/, wl_resource* list) {
                                    context<Configuration>(r).custom = context<ModeList>(list).modes;
                                },
                            .set_auto_brightness = ignore_event,
                        };
                        wl_resource_set_implementation(configuration, &configuration_impl,
                                                       new Configuration{&State::of(manager), std::nullopt, nullptr},
                                                       [](wl_resource* r) { destroy_data<Configuration>(r); });
                    },
                .create_mode_list =
                    [](wl_client* c, wl_resource* manager, std::uint32_t list_id) {
                        auto* list = wl_resource_create(c, &kde_mode_list_v2_interface,
                                                        wl_resource_get_version(manager), list_id);
                        static const struct kde_mode_list_v2_interface list_impl{
                            .destroy = &destroy_resource,
                            .add_mode =
                                [](wl_client* /*c*/, wl_resource* r) {
                                    context<ModeList>(r).modes.emplace_back(context<ModeList>(r).width,
                                                                            context<ModeList>(r).height);
                                },
                            .set_resolution =
                                [](wl_client* /*c*/, wl_resource* r, std::uint32_t width, std::uint32_t height) {
                                    context<ModeList>(r).width = static_cast<int>(width);
                                    context<ModeList>(r).height = static_cast<int>(height);
                                },
                            .set_refresh_rate = ignore_event,
                            .set_reduced_blanking = ignore_event,
                        };
                        wl_resource_set_implementation(list, &list_impl, new ModeList,
                                                       [](wl_resource* r) { destroy_data<ModeList>(r); });
                    },
            };
            wl_resource_set_implementation(resource, &impl, data, nullptr);
        }));

    // ext_data_control_manager_v1
    s.globals.push_back(wl_global_create(
        s.display, &ext_data_control_manager_v1_interface, 1, &s,
        [](wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
            auto* resource =
                wl_resource_create(client, &ext_data_control_manager_v1_interface, static_cast<int>(version), id);
            static const struct ext_data_control_manager_v1_interface impl{
                .create_data_source =
                    [](wl_client* c, wl_resource* manager, std::uint32_t source_id) {
                        auto* source = wl_resource_create(c, &ext_data_control_source_v1_interface,
                                                          wl_resource_get_version(manager), source_id);
                        static const struct ext_data_control_source_v1_interface source_impl{
                            .offer = [](wl_client* /*c*/, wl_resource* r,
                                        const char* mime) { context<SourceContext>(r).mime_types.emplace_back(mime); },
                            .destroy = &destroy_resource,
                        };
                        wl_resource_set_implementation(source, &source_impl, new SourceContext{{}, &State::of(manager)},
                                                       [](wl_resource* r) {
                                                           auto& source_context = context<SourceContext>(r);
                                                           source_context.state->forget(r);
                                                           destroy_data<SourceContext>(r);
                                                       });
                    },
                .get_data_device =
                    [](wl_client* c, wl_resource* manager, std::uint32_t device_id, wl_resource* /*seat*/) {
                        auto& state = State::of(manager);
                        auto* device = wl_resource_create(c, &ext_data_control_device_v1_interface,
                                                          wl_resource_get_version(manager), device_id);
                        static const struct ext_data_control_device_v1_interface device_impl{
                            .set_selection =
                                [](wl_client* /*c*/, wl_resource* r, wl_resource* source) {
                                    auto& st = State::of(r);
                                    if (st.client_source != nullptr && st.client_source != source) {
                                        ext_data_control_source_v1_send_cancelled(st.client_source);
                                    }
                                    st.client_source = source;
                                    st.desktop.reset();
                                    st.announce_all();
                                },
                            .destroy = &destroy_resource,
                            .set_primary_selection = ignore_event,
                        };
                        wl_resource_set_implementation(device, &device_impl, &state,
                                                       [](wl_resource* r) { State::of(r).forget(r); });
                        state.data_devices.push_back(device);
                        state.announce(device);
                    },
                .destroy = &destroy_resource,
            };
            wl_resource_set_implementation(resource, &impl, data, nullptr);
        }));

    s.thread = std::thread([&s] {
        auto* loop = wl_display_get_event_loop(s.display);
        while (!s.stop.load()) {
            wl_display_flush_clients(s.display);
            wl_event_loop_dispatch(loop, 50);
        }
    });
}

FakeKWin::~FakeKWin()
{
    auto& s = *state_;
    s.stop.store(true);
    std::uint64_t one = 1;
    [[maybe_unused]] const auto io = ::write(s.wake_fd, &one, sizeof(one));
    s.thread.join();
    wl_display_destroy_clients(s.display);
    for (auto* global : s.globals) {
        wl_global_destroy(global);
    }
    if (s.wake_source != nullptr) {
        wl_event_source_remove(s.wake_source);
    }
    wl_display_destroy(s.display);
    ::close(s.wake_fd);
    if (s.client_fd >= 0) {
        ::close(s.client_fd);
    }
}

int FakeKWin::take_client_fd()
{
    return std::exchange(state_->client_fd, -1);
}

void FakeKWin::run(const std::function<void()>& task)
{
    std::promise<void> done;
    {
        const std::scoped_lock lock(state_->mutex);
        state_->tasks.emplace_back([&] {
            task();
            wl_display_flush_clients(state_->display);
            done.set_value();
        });
    }
    std::uint64_t one = 1;
    [[maybe_unused]] const auto io = ::write(state_->wake_fd, &one, sizeof(one));
    done.get_future().wait();
}

void FakeKWin::close_streams()
{
    run([this] {
        for (auto* stream : state_->streams) {
            zkde_screencast_stream_unstable_v1_send_closed(stream);
        }
    });
}

std::pair<int, int> FakeKWin::current_mode()
{
    std::pair<int, int> size;
    run([&] {
        const auto& mode = state_->modes.at(state_->current);
        size = {mode.width, mode.height};
    });
    return size;
}

int FakeKWin::configurations()
{
    int count = 0;
    run([&] { count = state_->configurations; });
    return count;
}

int FakeKWin::custom_mode_lists()
{
    int count = 0;
    run([&] { count = state_->custom_mode_lists; });
    return count;
}

void FakeKWin::desktop_copy(std::vector<std::string> mime_types, std::string text)
{
    run([&] {
        auto& s = *state_;
        if (s.client_source != nullptr) {
            ext_data_control_source_v1_send_cancelled(s.client_source);
            s.client_source = nullptr;
        }
        s.desktop.emplace(std::move(mime_types), std::move(text));
        s.announce_all();
    });
}

UniqueFd FakeKWin::desktop_paste(const std::string& mime_type)
{
    UniqueFd result;
    run([&] {
        if (state_->client_source == nullptr) {
            return;
        }
        std::array<int, 2> fds{-1, -1};
        if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
            return;
        }
        result.reset(fds[0]);
        ext_data_control_source_v1_send_send(state_->client_source, mime_type.c_str(), fds[1]);
        ::close(fds[1]);
    });
    return result;
}

std::vector<std::string> FakeKWin::client_selection()
{
    std::vector<std::string> types;
    run([&] {
        if (state_->client_source != nullptr) {
            types = context<SourceContext>(state_->client_source).mime_types;
        }
    });
    return types;
}

}  // namespace farland::test
