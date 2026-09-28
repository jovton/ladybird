/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>

#include <Compositor/OpenGLContext.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibTest/TestCase.h>

// Stands in for Skia's Vulkan backend on a device that can't share images as DMA-BUFs, like Mesa's Dozen driver on WSL2.
// WebGL must not need anything from it besides the Vulkan context.
class VulkanBackendContextWithoutDmaBufs final : public Gfx::SkiaBackendContext {
public:
    GrDirectContext* sk_context() const override { return nullptr; }
    Gfx::MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }
    Gfx::VulkanContext const& vulkan_context() override { return m_vulkan_context; }

private:
    void flush_and_submit_impl(SkSurface*) override { VERIFY_NOT_REACHED(); }
    void flush_and_submit_async_impl(SkSurface*, Function<void()>&&) override { VERIFY_NOT_REACHED(); }

    Gfx::VulkanContext m_vulkan_context;
};

static constexpr Compositor::OpenGLContext::DrawingBufferOptions drawing_buffer_options { .depth = false, .stencil = false, .antialias = false };

TEST_CASE(webgl_works_on_vulkan_devices_that_cannot_share_dmabufs)
{
    // Without a GPU backend, WebGL also paints into a CPU surface, so this tells whether EGL works here at all.
    if (!Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL1, drawing_buffer_options)) {
        warnln("No EGL display available, skipping");
        return;
    }

    auto backend = adopt_ref(*new VulkanBackendContextWithoutDmaBufs);
    EXPECT(!backend->vulkan_context().supports_dmabuf_images);
    auto context = Compositor::OpenGLContext::create(backend, Compositor::OpenGLContext::WebGLVersion::WebGL1, drawing_buffer_options);
    EXPECT(context);
    if (!context)
        return;

    Gfx::IntSize size { 4, 3 };
    context->set_size(size);
    context->make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, context->default_framebuffer());

    // Give each row its own color. GL counts rows from the bottom.
    glEnable(GL_SCISSOR_TEST);
    for (int y = 0; y < size.height(); ++y) {
        glScissor(0, y, size.width(), 1);
        glClearColor(static_cast<float>((y + 1) * 40) / 255, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);

    context->present();
    auto surface = context->surface();
    EXPECT(surface);
    if (!surface)
        return;

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);
    for (int row = 0; row < size.height(); ++row) {
        for (int x = 0; x < size.width(); ++x)
            EXPECT_EQ(bitmap->get_pixel(x, row), Gfx::Color(static_cast<u8>((size.height() - row) * 40), 0, 255));
    }
}
