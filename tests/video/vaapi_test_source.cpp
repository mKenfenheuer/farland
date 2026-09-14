// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "vaapi_test_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <vector>

#if defined(FARLAND_TEST_HAVE_GBM)
#include <gbm.h>
#endif

namespace farland::test {

namespace {

void copy_rows(const codec::ImageView& image, std::byte* dst, std::size_t pitch)
{
    const std::size_t row = std::size_t{image.width} * 4U;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(dst + (y * pitch), image.data.data() + (y * image.stride), row);
    }
}

void quiet(void* /*context*/, const char* /*message*/) {}

class VaSource final : public DmabufSource {
public:
    VaSource() { image_.image_id = VA_INVALID_ID; }
    VaSource(const VaSource&) = delete;
    VaSource& operator=(const VaSource&) = delete;
    VaSource(VaSource&&) = delete;
    VaSource& operator=(VaSource&&) = delete;
    ~VaSource() override
    {
        for (std::uint32_t i = 0; i < descriptor_.num_objects; ++i) {
            ::close(descriptor_.objects[i].fd);
        }
        if (image_.image_id != VA_INVALID_ID) {
            vaDestroyImage(display_, image_.image_id);
        }
        if (surface_ != VA_INVALID_SURFACE) {
            vaDestroySurfaces(display_, &surface_, 1);
        }
        if (display_ != nullptr) {
            vaTerminate(display_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool open(const std::string& render_node, std::uint32_t width, std::uint32_t height)
    {
        fd_ = ::open(render_node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0 || (display_ = vaGetDisplayDRM(fd_)) == nullptr) {
            return false;
        }
        vaSetInfoCallback(display_, &quiet, nullptr);
        int major = 0;
        int minor = 0;
        if (vaInitialize(display_, &major, &minor) != VA_STATUS_SUCCESS) {
            return false;
        }
        VASurfaceAttrib format{};
        format.type = VASurfaceAttribPixelFormat;
        format.flags = VA_SURFACE_ATTRIB_SETTABLE;
        format.value.type = VAGenericValueTypeInteger;
        format.value.value.i = static_cast<int>(VA_FOURCC_BGRX);
        if (vaCreateSurfaces(display_, VA_RT_FORMAT_RGB32, width, height, &surface_, 1, &format, 1) !=
            VA_STATUS_SUCCESS) {
            surface_ = VA_INVALID_SURFACE;
            return false;
        }
        std::vector<VAImageFormat> formats(static_cast<std::size_t>(vaMaxNumImageFormats(display_)));
        int count = 0;
        vaQueryImageFormats(display_, formats.data(), &count);
        formats.resize(static_cast<std::size_t>(count));
        const auto bgrx = std::ranges::find(formats, static_cast<unsigned>(VA_FOURCC_BGRX), &VAImageFormat::fourcc);
        if (bgrx == formats.end() || vaCreateImage(display_, &*bgrx, static_cast<int>(width), static_cast<int>(height),
                                                   &image_) != VA_STATUS_SUCCESS) {
            image_.image_id = VA_INVALID_ID;
            return false;
        }
        if (vaExportSurfaceHandle(display_, surface_, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                  &descriptor_) != VA_STATUS_SUCCESS) {
            descriptor_.num_objects = 0;
            return false;
        }
        const auto& layer = descriptor_.layers[0];
        frame_.fourcc = layer.drm_format;
        frame_.modifier = descriptor_.objects[layer.object_index[0]].drm_format_modifier;
        frame_.width = width;
        frame_.height = height;
        frame_.plane_count = layer.num_planes;
        for (std::uint32_t i = 0; i < layer.num_planes && i < 4; ++i) {
            frame_.planes.at(i) = {.fd = descriptor_.objects[layer.object_index[i]].fd,
                                   .offset = layer.offset[i],
                                   .pitch = layer.pitch[i]};
        }
        return true;
    }

    bool write(const codec::ImageView& image) override
    {
        void* data = nullptr;
        if (vaMapBuffer(display_, image_.buf, &data) != VA_STATUS_SUCCESS) {
            return false;
        }
        copy_rows(image, static_cast<std::byte*>(data) + image_.offsets[0], image_.pitches[0]);
        vaUnmapBuffer(display_, image_.buf);
        return vaPutImage(display_, surface_, image_.image_id, 0, 0, image.width, image.height, 0, 0, image.width,
                          image.height) == VA_STATUS_SUCCESS &&
               vaSyncSurface(display_, surface_) == VA_STATUS_SUCCESS;
    }

    const video::DmabufFrame& frame() const noexcept override { return frame_; }

private:
    int fd_ = -1;
    VADisplay display_ = nullptr;
    VASurfaceID surface_ = VA_INVALID_SURFACE;
    VAImage image_{};
    VADRMPRIMESurfaceDescriptor descriptor_{};
    video::DmabufFrame frame_;
};

#if defined(FARLAND_TEST_HAVE_GBM)
class GbmSource final : public DmabufSource {
public:
    GbmSource() = default;
    GbmSource(const GbmSource&) = delete;
    GbmSource& operator=(const GbmSource&) = delete;
    GbmSource(GbmSource&&) = delete;
    GbmSource& operator=(GbmSource&&) = delete;
    ~GbmSource() override
    {
        for (std::uint32_t i = 0; i < frame_.plane_count; ++i) {
            ::close(frame_.planes.at(i).fd);
        }
        if (bo_ != nullptr) {
            gbm_bo_destroy(bo_);
        }
        if (device_ != nullptr) {
            gbm_device_destroy(device_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool open(const std::string& render_node, std::uint32_t width, std::uint32_t height, bool linear)
    {
        fd_ = ::open(render_node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0 || (device_ = gbm_create_device(fd_)) == nullptr) {
            return false;
        }
        bo_ = gbm_bo_create(device_, width, height, GBM_FORMAT_XRGB8888,
                            static_cast<std::uint32_t>(GBM_BO_USE_RENDERING) |
                                (linear ? static_cast<std::uint32_t>(GBM_BO_USE_LINEAR) : 0U));
        if (bo_ == nullptr) {
            return false;
        }
        frame_.fourcc = video::drm_fourcc::xrgb8888;
        frame_.modifier = gbm_bo_get_modifier(bo_);
        frame_.width = width;
        frame_.height = height;
        const int planes = gbm_bo_get_plane_count(bo_);
        for (int i = 0; i < planes && i < 4; ++i) {
            frame_.planes.at(static_cast<std::size_t>(i)) = {.fd = gbm_bo_get_fd_for_plane(bo_, i),
                                                             .offset = gbm_bo_get_offset(bo_, i),
                                                             .pitch = gbm_bo_get_stride_for_plane(bo_, i)};
            ++frame_.plane_count;
        }
        return frame_.plane_count > 0 && frame_.planes[0].fd >= 0;
    }

    bool write(const codec::ImageView& image) override
    {
        std::uint32_t stride = 0;
        void* map_data = nullptr;
        void* pixels = gbm_bo_map(bo_, 0, 0, image.width, image.height, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
        if (pixels == nullptr) {
            return false;
        }
        copy_rows(image, static_cast<std::byte*>(pixels), stride);
        gbm_bo_unmap(bo_, map_data);
        return true;
    }

    const video::DmabufFrame& frame() const noexcept override { return frame_; }

private:
    int fd_ = -1;
    gbm_device* device_ = nullptr;
    gbm_bo* bo_ = nullptr;
    video::DmabufFrame frame_;
};
#endif

}  // namespace

std::unique_ptr<DmabufSource> make_va_source(const std::string& render_node, std::uint32_t width, std::uint32_t height)
{
    auto source = std::make_unique<VaSource>();
    if (!source->open(render_node, width, height)) {
        return nullptr;
    }
    return source;
}

std::unique_ptr<DmabufSource> make_gbm_source(const std::string& render_node, std::uint32_t width, std::uint32_t height,
                                              bool linear)
{
#if defined(FARLAND_TEST_HAVE_GBM)
    auto source = std::make_unique<GbmSource>();
    if (!source->open(render_node, width, height, linear)) {
        return nullptr;
    }
    return source;
#else
    static_cast<void>(render_node);
    static_cast<void>(width);
    static_cast<void>(height);
    static_cast<void>(linear);
    return nullptr;
#endif
}

}  // namespace farland::test
