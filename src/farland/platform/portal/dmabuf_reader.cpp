// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/dmabuf_reader.hpp>
#include <farland/platform/portal/pixel_formats.hpp>

#include <map>
#include <utility>

#ifdef FARLAND_HAVE_GBM
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <gbm.h>
#include <string_view>
#include <unistd.h>
#endif

namespace farland::platform::portal {

namespace {
constexpr std::string_view log_component = "platform.dmabuf";
}  // namespace

struct DmabufReader::State {
    int fd = -1;
    gbm_device* device = nullptr;
    std::map<std::uint32_t, std::vector<std::uint64_t>> modifiers;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State();
};

#ifdef FARLAND_HAVE_GBM

DmabufReader::State::~State()
{
    if (device != nullptr) {
        gbm_device_destroy(device);
    }
    if (fd >= 0) {
        ::close(fd);
    }
}

DmabufReader::Mapping::Mapping(gbm_bo* bo, void* map_data, std::span<const std::byte> pixels,
                               std::size_t stride) noexcept
    : bo_(bo), map_data_(map_data), pixels_(pixels), stride_(stride)
{
}

DmabufReader::Mapping::Mapping(Mapping&& other) noexcept
    : bo_(std::exchange(other.bo_, nullptr)), map_data_(std::exchange(other.map_data_, nullptr)),
      pixels_(other.pixels_), stride_(other.stride_)
{
}

DmabufReader::Mapping::~Mapping()
{
    if (bo_ != nullptr) {
        gbm_bo_unmap(bo_, map_data_);
        gbm_bo_destroy(bo_);
    }
}

namespace {

/// Asks EGL (on the GBM device) which modifiers the driver imports per format.
std::map<std::uint32_t, std::vector<std::uint64_t>> query_modifiers(gbm_device* device)
{
    std::map<std::uint32_t, std::vector<std::uint64_t>> result;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): EGL hands out function pointers untyped.
    const auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    const auto query_dmabuf_modifiers =
        reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (get_platform_display == nullptr || query_dmabuf_modifiers == nullptr) {
        log::info(log_component, "EGL cannot query dmabuf modifiers");
        return result;
    }
    EGLDisplay display = get_platform_display(EGL_PLATFORM_GBM_KHR, device, nullptr);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) == EGL_FALSE) {
        log::info(log_component, "no EGL display on the GBM device");
        return result;
    }
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (extensions != nullptr &&
        std::string_view(extensions).find("EGL_EXT_image_dma_buf_import_modifiers") != std::string_view::npos) {
        for (const PixelLayout layout : all_pixel_layouts) {
            const std::uint32_t format = drm_fourcc(layout);
            EGLint count = 0;
            if (query_dmabuf_modifiers(display, static_cast<EGLint>(format), 0, nullptr, nullptr, &count) ==
                    EGL_FALSE ||
                count <= 0) {
                continue;
            }
            std::vector<EGLuint64KHR> modifiers(static_cast<std::size_t>(count));
            std::vector<EGLBoolean> external_only(static_cast<std::size_t>(count));
            if (query_dmabuf_modifiers(display, static_cast<EGLint>(format), count, modifiers.data(),
                                       external_only.data(), &count) == EGL_FALSE) {
                continue;
            }
            auto& list = result[format];
            for (std::size_t i = 0; i < static_cast<std::size_t>(count) && i < modifiers.size(); ++i) {
                if (external_only[i] == EGL_FALSE && modifiers[i] != drm_format_mod_invalid) {
                    list.push_back(modifiers[i]);
                }
            }
        }
    }
    eglTerminate(display);
    return result;
}

int open_render_node(const std::string& render_node, std::string& opened)
{
    if (!render_node.empty()) {
        opened = render_node;
        return ::open(render_node.c_str(), O_RDWR | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    for (int minor = 128; minor < 192; ++minor) {
        std::string path = std::format("/dev/dri/renderD{}", minor);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (fd >= 0) {
            opened = std::move(path);
            return fd;
        }
    }
    return -1;
}

}  // namespace

std::unique_ptr<DmabufReader> DmabufReader::open(const std::string& render_node)
{
    auto state = std::make_unique<State>();
    std::string path;
    state->fd = open_render_node(render_node, path);
    if (state->fd < 0) {
        log::info(log_component, "no DRM render node; tiled dmabufs are not offered");
        return nullptr;
    }
    state->device = gbm_create_device(state->fd);
    if (state->device == nullptr) {
        log::info(log_component, "GBM does not support {}; tiled dmabufs are not offered", path);
        return nullptr;
    }
    state->modifiers = query_modifiers(state->device);
    log::info(log_component, "importing dmabufs through {} ({})", path, gbm_device_get_backend_name(state->device));
    return std::unique_ptr<DmabufReader>(new DmabufReader(std::move(state)));
}

std::optional<DmabufReader::Mapping> DmabufReader::map(const DmabufImage& image) const
{
    if (image.planes == 0 || image.planes > image.fds.size()) {
        return std::nullopt;
    }
    gbm_import_fd_modifier_data data{};
    data.width = image.width;
    data.height = image.height;
    data.format = image.drm_format;
    data.num_fds = image.planes;
    data.modifier = image.modifier;
    for (std::uint32_t i = 0; i < image.planes; ++i) {
        data.fds[i] = image.fds[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        data.strides[i] =
            static_cast<int>(image.strides[i]);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        data.offsets[i] =
            static_cast<int>(image.offsets[i]);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
    gbm_bo* bo = gbm_bo_import(state_->device, GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_RENDERING);
    if (bo == nullptr) {
        return std::nullopt;
    }
    std::uint32_t stride = 0;
    void* map_data = nullptr;
    void* pixels = gbm_bo_map(bo, 0, 0, image.width, image.height, GBM_BO_TRANSFER_READ, &stride, &map_data);
    if (pixels == nullptr || stride < image.width * 4U) {
        if (pixels != nullptr) {
            gbm_bo_unmap(bo, map_data);
        }
        gbm_bo_destroy(bo);
        return std::nullopt;
    }
    const std::size_t size = image_extent(stride, image.width, image.height);
    return Mapping(bo, map_data, std::span(static_cast<const std::byte*>(pixels), size), stride);
}

#else  // !FARLAND_HAVE_GBM

DmabufReader::State::~State() = default;

DmabufReader::Mapping::Mapping(gbm_bo* bo, void* map_data, std::span<const std::byte> pixels,
                               std::size_t stride) noexcept
    : bo_(bo), map_data_(map_data), pixels_(pixels), stride_(stride)
{
}

DmabufReader::Mapping::Mapping(Mapping&& other) noexcept
    : bo_(std::exchange(other.bo_, nullptr)), map_data_(std::exchange(other.map_data_, nullptr)),
      pixels_(other.pixels_), stride_(other.stride_)
{
}

DmabufReader::Mapping::~Mapping() = default;

std::unique_ptr<DmabufReader> DmabufReader::open(const std::string& /*render_node*/)
{
    log::debug(log_component, "built without GBM and EGL; tiled dmabufs are not offered");
    return nullptr;
}

std::optional<DmabufReader::Mapping> DmabufReader::map(const DmabufImage& /*image*/) const
{
    return std::nullopt;
}

#endif  // FARLAND_HAVE_GBM

DmabufReader::DmabufReader(std::unique_ptr<State> state) : state_(std::move(state)) {}

DmabufReader::~DmabufReader() = default;

std::vector<std::uint64_t> DmabufReader::modifiers(std::uint32_t drm_format) const
{
    const auto it = state_->modifiers.find(drm_format);
    return it == state_->modifiers.end() ? std::vector<std::uint64_t>{} : it->second;
}

}  // namespace farland::platform::portal
