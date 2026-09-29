/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCompositing/WebGL/GLFunctions.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/SharedImageBuffer.h>
#endif

#if defined(AK_OS_MACOS) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)) || defined(AK_OS_WINDOWS)
#    define ENABLE_WEBGL_CPU_PAINTING_SURFACE
#endif

namespace Compositor {

class OpenGLContext : public Compositing::WebGL::GLFunctions {
public:
    AK_ALLOC_WITH_KMALLOC;

    using WebGLVersion = Compositing::WebGL::WebGLVersion;

    struct DrawingBufferOptions {
        bool depth;
        bool stencil;
        bool antialias;
    };

    static OwnPtr<OpenGLContext> create(RefPtr<Gfx::SkiaBackendContext>, WebGLVersion, DrawingBufferOptions);

    void notify_content_will_change();
    void clear_buffer_to_default_values();
    void allocate_painting_surface_if_needed();

    struct Impl;
    OpenGLContext(RefPtr<Gfx::SkiaBackendContext>, Impl, WebGLVersion, DrawingBufferOptions);

    ~OpenGLContext();

    void make_current();

    void present();

    // Like present(), except that a WebGL 2 context doesn't wait for the GPU to finish the frame: it gets copied into a
    // buffer, and only reaches the painting surface in finish_asynchronous_present(). Returns whether that's pending.
    bool present_asynchronously();
    void finish_asynchronous_present();

    void set_size(Gfx::IntSize const&);

    RefPtr<Gfx::PaintingSurface> surface();

    u32 default_framebuffer() const;
    u32 default_renderbuffer() const;

    // Works around a driver bug before passing the call on to GL, see the definition.
    void blit_framebuffer(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1, GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1, GLbitfield mask, GLenum filter);
    void renderbuffer_storage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void renderbuffer_storage_multisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
    void framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);

    Vector<String> get_supported_opengl_extensions();

private:
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Gfx::IntSize m_size;
    RefPtr<Gfx::PaintingSurface> m_painting_surface;
#ifdef AK_OS_MACOS
    OwnPtr<Gfx::SharedImageBuffer> m_shared_image_buffer;
#endif
    NonnullOwnPtr<Impl> m_impl;
    WebGLVersion m_webgl_version;
    [[maybe_unused]] DrawingBufferOptions m_drawing_buffer_options;

    void free_surface_resources();
    bool drawing_buffer_can_have_stencil();
    bool uses_mesa_d3d12();
    bool read_framebuffer_has_unresolvable_depth();
    GLenum renderbuffer_format_for(GLenum);
#if defined(AK_OS_MACOS)
    void allocate_iosurface_painting_surface();
#endif
#if defined(USE_VULKAN_DMABUF_IMAGES)
    bool allocate_vkimage_painting_surface();
#endif
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    void allocate_cpu_painting_surface();
    void copy_default_framebuffer_to_cpu_painting_surface();
    void read_default_framebuffer(u32 pixel_pack_buffer, void* destination);
    Gfx::Bitmap& readback_bitmap();
#endif
};

}
