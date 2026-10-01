/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Define GL_GLEXT_PROTOTYPES before OpenGLContext.h pulls in the GL headers without it.
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

// Define EGL_EGLEXT_PROTOTYPES before eglext.h pulls in the ANGLE extension headers without it.
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
extern "C" {
#include <EGL/eglext_angle.h>
}

#include <AK/Checked.h>
#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#include <Compositor/OpenGLContext.h>

// Enable WebGL if we're on macOS and can use Metal, if Linux can use ANGLE's OpenGL backend for CPU-painting tests,
// if Windows can use ANGLE's Direct3D 11 backend, or if we can use shareable Vulkan images.
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE) || defined(USE_VULKAN_DMABUF_IMAGES)
#    define ENABLE_WEBGL 1
#endif

namespace Compositor {

using namespace Compositing::WebGL;

struct OpenGLContext::Impl {
    AK_ALLOC_WITH_KMALLOC;

    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { EGL_NO_CONFIG_KHR };
    EGLContext context { EGL_NO_CONTEXT };
    EGLSurface surface { EGL_NO_SURFACE };

    // The drawing buffer's color, which gets painted. With antialiasing, the page draws into msaa_framebuffer instead,
    // and it gets resolved into this one, see resolve_drawing_buffer().
    GLuint framebuffer { 0 };
    GLuint color_buffer { 0 };
    // The depth and stencil of the framebuffer the page draws into (multisampled with antialiasing).
    GLuint depth_buffer { 0 };
    GLuint msaa_framebuffer { 0 };
    GLuint msaa_color_buffer { 0 };
    // Whether a multisampled drawing buffer resolves correctly, found out once, see allocate_msaa_drawing_buffer().
    Optional<bool> msaa_works {};
    EGLint texture_target { 0 };
    bool uses_cpu_painting_surface { false };

#ifdef USE_VULKAN_DMABUF_IMAGES
    EGLImage egl_image { EGL_NO_IMAGE };
    struct {
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_dma_buf_formats { nullptr };
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dma_buf_modifiers { nullptr };
    } ext_procs;
#endif

    RefPtr<Gfx::Bitmap> readback_bitmap {};

    // WebGL 2 contexts read their drawing buffer back into this pixel pack buffer without waiting for the GPU, see
    // present_asynchronously().
    GLuint readback_buffer { 0 };
    Gfx::IntSize readback_buffer_size {};
    bool readback_pending { false };

    Optional<bool> uses_mesa_d3d12 {};
    // From ANGLE_get_tex_level_parameter, which the stencil workarounds use on d3d12 to see what a texture image got.
    PFNGLGETTEXLEVELPARAMETERIVANGLEPROC get_tex_level_parameteriv { nullptr };

    // Renderbuffers that the page gave a stencil format, but that got a depth-only one instead, see renderbuffer_storage().
    HashMap<GLuint, GLenum> renderbuffers_without_stencil {};
    // The same for texture images, see tex_storage2d(). A key names one attachable image: the texture, which of its
    // images (see texture_image_kind()) and the mip level, see texture_image_key().
    HashTable<u64> texture_images_without_stencil {};
};

OpenGLContext::OpenGLContext(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl, WebGLVersion webgl_version, DrawingBufferOptions drawing_buffer_options)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(impl))
    , m_webgl_version(webgl_version)
    , m_drawing_buffer_options(drawing_buffer_options)
{
}

OpenGLContext::~OpenGLContext()
{
#ifdef ENABLE_WEBGL
    free_surface_resources();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_impl->display, m_impl->context);
#endif
}

void OpenGLContext::free_surface_resources()
{
#ifdef ENABLE_WEBGL
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    if (m_impl->framebuffer) {
        glDeleteFramebuffers(1, &m_impl->framebuffer);
        m_impl->framebuffer = 0;
    }

    if (m_impl->color_buffer) {
        glDeleteTextures(1, &m_impl->color_buffer);
        m_impl->color_buffer = 0;
    }

    if (m_impl->depth_buffer) {
        glDeleteRenderbuffers(1, &m_impl->depth_buffer);
        m_impl->depth_buffer = 0;
    }

    if (m_impl->msaa_framebuffer) {
        glDeleteFramebuffers(1, &m_impl->msaa_framebuffer);
        m_impl->msaa_framebuffer = 0;
    }

    if (m_impl->msaa_color_buffer) {
        glDeleteRenderbuffers(1, &m_impl->msaa_color_buffer);
        m_impl->msaa_color_buffer = 0;
    }

    if (m_impl->readback_buffer) {
        glDeleteBuffers(1, &m_impl->readback_buffer);
        m_impl->readback_buffer = 0;
        m_impl->readback_buffer_size = {};
        m_impl->readback_pending = false;
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    if (m_impl->egl_image != EGL_NO_IMAGE) {
        eglDestroyImage(m_impl->display, m_impl->egl_image);
        m_impl->egl_image = EGL_NO_IMAGE;
    }
#    endif

    if (m_impl->surface != EGL_NO_SURFACE) {
#    ifdef AK_OS_MACOS
        eglReleaseTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
#    endif
        eglDestroySurface(m_impl->display, m_impl->surface);
        m_impl->surface = EGL_NO_SURFACE;
    }
#endif
}

#ifdef ENABLE_WEBGL
static EGLConfig get_egl_config(EGLDisplay display)
{
    EGLint const config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint number_of_configs;
    eglChooseConfig(display, config_attribs, NULL, 0, &number_of_configs);

    Vector<EGLConfig> configs;
    configs.resize(number_of_configs);
    eglChooseConfig(display, config_attribs, configs.data(), number_of_configs, &number_of_configs);
    return number_of_configs > 0 ? configs[0] : EGL_NO_CONFIG_KHR;
}
#endif

OwnPtr<OpenGLContext> OpenGLContext::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, WebGLVersion webgl_version, [[maybe_unused]] DrawingBufferOptions drawing_buffer_options)
{
#ifdef ENABLE_WEBGL
#    if defined(AK_OS_MACOS) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID))
    bool use_cpu_painting_surface = !skia_backend_context;
#    elif defined(AK_OS_WINDOWS)
    // FIXME: Share the drawing buffer with Skia's Direct3D 12 device (via an NT shared handle opened on ANGLE's
    //        Direct3D 11 device) instead of reading it back to the CPU every frame.
    bool use_cpu_painting_surface = true;
#    else
    bool use_cpu_painting_surface = false;
#    endif

#    ifdef USE_VULKAN_DMABUF_IMAGES
    // The drawing buffer can only be shared with Skia's Vulkan device as a DMA-BUF, which not every device supports.
    if (skia_backend_context && !skia_backend_context->vulkan_context().supports_dmabuf_images) {
#        ifdef ENABLE_WEBGL_CPU_PAINTING_SURFACE
        use_cpu_painting_surface = true;
#        else
        return {};
#        endif
    }
#    endif

#    if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
    if (!use_cpu_painting_surface && !skia_backend_context)
        return {};
#    endif

#    if !defined(AK_OS_MACOS) && !defined(USE_VULKAN_DMABUF_IMAGES)
    if (!use_cpu_painting_surface)
        return {};
#    endif

    EGLAttrib display_attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
#    if defined(AK_OS_MACOS)
        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
#    elif defined(USE_VULKAN_DMABUF_IMAGES) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID))
        EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
        EGL_PLATFORM_SURFACELESS_MESA,
#    elif defined(AK_OS_WINDOWS)
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
#    endif
        EGL_NONE,
    };

    auto display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), display_attributes);
    if (display == EGL_NO_DISPLAY) {
        dbgln("Failed to get EGL display");
        return {};
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        dbgln("Failed to initialize EGL");
        return {};
    }

    auto* config = get_egl_config(display);
    if (config == EGL_NO_CONFIG_KHR) {
        dbgln("Failed to find EGLConfig");
        return {};
    }

    EGLint texture_target;
#    if defined(AK_OS_MACOS)
    if (use_cpu_painting_surface) {
        texture_target = EGL_TEXTURE_2D;
    } else {
        eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target);
        VERIFY(texture_target == EGL_TEXTURE_RECTANGLE_ANGLE || texture_target == EGL_TEXTURE_2D);
    }
#    else
    texture_target = EGL_TEXTURE_2D;
#    endif

    EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        webgl_version == WebGLVersion::WebGL1 ? 2 : 3,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE,
        EGL_TRUE,
        EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE,
        EGL_TRUE,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE,
        EGL_FALSE,
#    ifdef USE_VULKAN_DMABUF_IMAGES
        // we need GL_OES_EGL_image
        EGL_EXTENSIONS_ENABLED_ANGLE,
        EGL_TRUE,
#    endif
        EGL_NONE,
        EGL_NONE,
    };
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        dbgln("Failed to create EGL context");
        return {};
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    PFNEGLQUERYDMABUFFORMATSEXTPROC pfn_egl_query_dma_buf_formats_ext = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC pfn_egl_query_dma_buf_modifiers_ext = nullptr;

    if (!use_cpu_painting_surface) {
        pfn_egl_query_dma_buf_formats_ext = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        if (!pfn_egl_query_dma_buf_formats_ext) {
            dbgln("eglQueryDmaBufFormatsEXT unavailable");
            return {};
        }

        pfn_egl_query_dma_buf_modifiers_ext = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        if (!pfn_egl_query_dma_buf_modifiers_ext) {
            dbgln("eglQueryDmaBufModifiersEXT unavailable");
            return {};
        }
    }
#    endif

    return make<OpenGLContext>(skia_backend_context, Impl {
                                                         .display = display,
                                                         .config = config,
                                                         .context = context,
                                                         .texture_target = texture_target,
                                                         .uses_cpu_painting_surface = use_cpu_painting_surface,
#    ifdef USE_VULKAN_DMABUF_IMAGES
                                                         .ext_procs = {
                                                             .query_dma_buf_formats = pfn_egl_query_dma_buf_formats_ext,
                                                             .query_dma_buf_modifiers = pfn_egl_query_dma_buf_modifiers_ext,
                                                         },
#    endif
                                                     },
        webgl_version, drawing_buffer_options);
#else
    (void)skia_backend_context;
    (void)webgl_version;
    return {};
#endif
}

void OpenGLContext::notify_content_will_change()
{
#ifdef ENABLE_WEBGL
    if (m_impl->uses_cpu_painting_surface)
        return;
    m_painting_surface->notify_content_will_change();
#endif
}

void OpenGLContext::clear_buffer_to_default_values()
{
#ifdef ENABLE_WEBGL
    GLint original_framebuffer;
    GLint original_renderbuffer;
    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_DRAW_FRAMEBUFFER;
        framebuffer_binding = GL_DRAW_FRAMEBUFFER_BINDING;
    }
    glGetIntegerv(framebuffer_binding, &original_framebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &original_renderbuffer);

    glBindFramebuffer(framebuffer_target, default_framebuffer());
    glBindRenderbuffer(GL_RENDERBUFFER, default_renderbuffer());

    Array<GLfloat, 4> current_clear_color;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, current_clear_color.data());

    GLfloat current_clear_depth;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &current_clear_depth);

    GLint current_clear_stencil;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &current_clear_stencil);

    // The implicit clear value for the color buffer is (0, 0, 0, 0)
    glClearColor(0, 0, 0, 0);

    // The implicit clear value for the depth buffer is 1.0.
    glClearDepthf(1.0f);

    // The implicit clear value for the stencil buffer is 0.
    glClearStencil(0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Restore the clear values.
    glClearColor(current_clear_color[0], current_clear_color[1], current_clear_color[2], current_clear_color[3]);
    glClearDepthf(current_clear_depth);
    glClearStencil(current_clear_stencil);

    glBindFramebuffer(framebuffer_target, original_framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, original_renderbuffer);
#endif
}

#ifdef AK_OS_MACOS
void OpenGLContext::allocate_iosurface_painting_surface()
{
    m_shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::create(m_size));
    m_painting_surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*m_shared_image_buffer, *m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_IOSURFACE_PLANE_ANGLE,
        0,
        EGL_TEXTURE_TARGET,
        m_impl->texture_target,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,
        GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT,
        EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE,
        GL_UNSIGNED_BYTE,
        EGL_NONE,
        EGL_NONE,
    };
    m_impl->surface = eglCreatePbufferFromClientBuffer(m_impl->display, EGL_IOSURFACE_ANGLE, m_shared_image_buffer->iosurface_handle().core_foundation_pointer(), m_impl->config, surface_attributes);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer);
    auto result = eglBindTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
    VERIFY(result == EGL_TRUE);

    glViewport(0, 0, m_size.width(), m_size.height());
}
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
bool OpenGLContext::allocate_vkimage_painting_surface()
{
    VERIFY(m_skia_backend_context);

    VkFormat vulkan_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t drm_format = Gfx::vk_format_to_drm_format(vulkan_format);

    // Ensure that our format is supported by the implementation.
    // FIXME: try other formats if not?
    EGLint num_formats = 0;
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, 0, nullptr, &num_formats);
    Vector<EGLint> egl_formats;
    egl_formats.resize(num_formats);
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, num_formats, egl_formats.data(), &num_formats);
    if (egl_formats.find(drm_format) == egl_formats.end()) {
        dbgln("Compositor: DMABUF format {:#x} is not supported by this driver; falling back to a CPU painting surface", drm_format);
        return false;
    }

    EGLint num_modifiers = 0;
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, 0, nullptr, nullptr, &num_modifiers);
    Vector<uint64_t> egl_modifiers;
    egl_modifiers.resize(num_modifiers);
    Vector<EGLBoolean> external_only;
    external_only.resize(num_modifiers);
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, num_modifiers, egl_modifiers.data(), external_only.data(), &num_modifiers);
    Vector<uint64_t> renderable_modifiers;
    for (int i = 0; i < num_modifiers; ++i) {
        if (!external_only[i]) {
            renderable_modifiers.append(egl_modifiers[i]);
        }
    }

    auto vulkan_image_or_error = Gfx::create_shared_vulkan_image(m_skia_backend_context->vulkan_context(), m_size.width(), m_size.height(), vulkan_format, renderable_modifiers);
    if (vulkan_image_or_error.is_error()) {
        dbgln("Compositor: create_shared_vulkan_image failed ({}); falling back to a CPU painting surface", vulkan_image_or_error.error());
        return false;
    }
    auto vulkan_image = vulkan_image_or_error.release_value();
    m_painting_surface = Gfx::PaintingSurface::create_from_vkimage(*m_skia_backend_context, vulkan_image, Gfx::PaintingSurface::Origin::BottomLeft);

    EGLAttrib attribs[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_LINUX_DRM_FOURCC_EXT,
        drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        vulkan_image->get_dma_buf_fd(), // EGL takes ownership of the fd
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<uint32_t>(vulkan_image->info.row_pitch),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier >> 32),
        EGL_NONE,
    };
    m_impl->egl_image = eglCreateImage(m_impl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (m_impl->egl_image == EGL_NO_IMAGE) {
        dbgln("Compositor: eglCreateImage for the DMABUF failed; falling back to a CPU painting surface");
        return false;
    }

    m_impl->surface = EGL_NO_SURFACE;
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(GL_TEXTURE_2D, m_impl->color_buffer);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_impl->egl_image);

    glViewport(0, 0, m_size.width(), m_size.height());

    return true;
}
#endif

#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
void OpenGLContext::allocate_cpu_painting_surface()
{
    m_painting_surface = Gfx::PaintingSurface::create_with_size(
        m_size,
        Gfx::BitmapFormat::BGRA8888,
        Gfx::AlphaType::Premultiplied);

    m_impl->surface = EGL_NO_SURFACE;
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(GL_TEXTURE_2D, m_impl->color_buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint original_pixel_unpack_buffer = 0;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &original_pixel_unpack_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_size.width(), m_size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (m_webgl_version == WebGLVersion::WebGL2)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, original_pixel_unpack_buffer);

    glViewport(0, 0, m_size.width(), m_size.height());
}

Gfx::Bitmap& OpenGLContext::readback_bitmap()
{
    // The pixels only pass through this bitmap on their way into the painting surface, so it can be reused.
    auto& bitmap = m_impl->readback_bitmap;
    if (!bitmap || bitmap->size() != m_size)
        bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, m_size));
    return *bitmap;
}

// Reads the drawing buffer, bottom row first and tightly packed, into destination, or into the given pixel pack buffer at
// the offset that destination stands for. The page's framebuffer binding and pixel pack state are left as they were.
void OpenGLContext::read_default_framebuffer(u32 pixel_pack_buffer, void* destination)
{
    VERIFY(pixel_pack_buffer == 0 || m_webgl_version == WebGLVersion::WebGL2);

    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_READ_FRAMEBUFFER;
        framebuffer_binding = GL_READ_FRAMEBUFFER_BINDING;
    }

    GLint original_framebuffer;
    glGetIntegerv(framebuffer_binding, &original_framebuffer);

    GLint original_read_buffer = GL_COLOR_ATTACHMENT0;
    if (m_webgl_version == WebGLVersion::WebGL2)
        glGetIntegerv(GL_READ_BUFFER, &original_read_buffer);

    resolve_drawing_buffer();
    glBindFramebuffer(framebuffer_target, m_impl->framebuffer);
    if (m_webgl_version == WebGLVersion::WebGL2)
        glReadBuffer(GL_COLOR_ATTACHMENT0);

    GLint original_pack_alignment;
    glGetIntegerv(GL_PACK_ALIGNMENT, &original_pack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    GLint original_pixel_pack_buffer = 0;
    GLint original_pack_row_length = 0;
    GLint original_pack_skip_pixels = 0;
    GLint original_pack_skip_rows = 0;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &original_pixel_pack_buffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_pack_buffer);

        glGetIntegerv(GL_PACK_ROW_LENGTH, &original_pack_row_length);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &original_pack_skip_pixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &original_pack_skip_rows);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    }

    // Read all rows with one call: reading back into client memory waits for the GPU, which is slow with a hardware
    // driver.
    glReadPixels(0, 0, m_size.width(), m_size.height(), GL_RGBA, GL_UNSIGNED_BYTE, destination);

    if (m_webgl_version == WebGLVersion::WebGL2) {
        glPixelStorei(GL_PACK_ROW_LENGTH, original_pack_row_length);
        glPixelStorei(GL_PACK_SKIP_PIXELS, original_pack_skip_pixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, original_pack_skip_rows);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, original_pixel_pack_buffer);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, original_pack_alignment);

    glBindFramebuffer(framebuffer_target, original_framebuffer);
    if (m_webgl_version == WebGLVersion::WebGL2)
        glReadBuffer(original_read_buffer);
}

void OpenGLContext::copy_default_framebuffer_to_cpu_painting_surface()
{
    VERIFY(m_impl->uses_cpu_painting_surface);
    VERIFY(m_painting_surface);

    // This reads the current frame, which supersedes one that is still being read back asynchronously.
    m_impl->readback_pending = false;

    auto& bitmap = readback_bitmap();
    auto row_size = static_cast<size_t>(m_size.width()) * 4;
    VERIFY(bitmap.pitch() == row_size);
    read_default_framebuffer(0, bitmap.scanline_u8(0));

    // GL's rows go from bottom to top, so flip them.
    auto spare_row = MUST(ByteBuffer::create_uninitialized(row_size));
    for (int top = 0, bottom = m_size.height() - 1; top < bottom; ++top, --bottom) {
        memcpy(spare_row.data(), bitmap.scanline_u8(top), row_size);
        memcpy(bitmap.scanline_u8(top), bitmap.scanline_u8(bottom), row_size);
        memcpy(bitmap.scanline_u8(bottom), spare_row.data(), row_size);
    }

    m_painting_surface->write_from_bitmap(bitmap);
}
#endif

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef ENABLE_WEBGL
    if (m_painting_surface)
        return;

    free_surface_resources();

    VERIFY(!m_size.is_empty());

#    if defined(AK_OS_MACOS)
    if (m_impl->uses_cpu_painting_surface) {
        allocate_cpu_painting_surface();
    } else {
        allocate_iosurface_painting_surface();
    }
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
#        if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    if (m_impl->uses_cpu_painting_surface) {
        allocate_cpu_painting_surface();
    } else if (!allocate_vkimage_painting_surface()) {
        // This driver advertises the DMABUF EGL extensions but can't actually allocate a Vulkan/DMABUF painting surface
        // (unsupported format, modifier, or image import). Fall back to a CPU painting surface.
        m_impl->uses_cpu_painting_surface = true;
        free_surface_resources();
        allocate_cpu_painting_surface();
    }
#        else
    (void)allocate_vkimage_painting_surface();
#        endif
#    elif (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)) || defined(AK_OS_WINDOWS)
    allocate_cpu_painting_surface();
#    endif
    VERIFY(m_painting_surface);
    VERIFY(eglGetCurrentContext() == m_impl->context);

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer, 0);

    auto stencil = m_drawing_buffer_options.stencil && drawing_buffer_can_have_stencil();
    if (drawing_buffer_can_have_antialias() && allocate_msaa_drawing_buffer(stencil))
        return;

    if (m_drawing_buffer_options.depth || stencil) {
        glGenRenderbuffers(1, &m_impl->depth_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);

        if (m_drawing_buffer_options.depth && stencil) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else if (m_drawing_buffer_options.depth) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else {
            VERIFY(stencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        }
    }

    VERIFY(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
#endif
}

// Antialiasing gives the drawing buffer a multisampled framebuffer to draw into, which gets resolved into the one that
// is painted. It needs OpenGL ES 3 for multisampled renderbuffers and glBlitFramebuffer(), so only WebGL 2 gets it:
// WebGL 1 contexts are OpenGL ES 2 contexts.
bool OpenGLContext::drawing_buffer_can_have_antialias()
{
#ifdef ENABLE_WEBGL
    return m_drawing_buffer_options.antialias && m_webgl_version == WebGLVersion::WebGL2 && m_impl->msaa_works != false;
#else
    return false;
#endif
}

bool OpenGLContext::drawing_buffer_has_antialias()
{
    make_current();
    return m_impl->msaa_framebuffer != 0;
}

// Adds the multisampled framebuffer the page draws into, with the drawing buffer's depth and stencil, to
// m_impl->framebuffer, which then just keeps the color. Returns false, allocating nothing, where that doesn't work.
bool OpenGLContext::allocate_msaa_drawing_buffer(bool stencil)
{
#ifdef ENABLE_WEBGL
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    // 4 samples is what other browsers use, and what every OpenGL ES 3 implementation supports.
    auto samples = min(max_samples, 4);
    if (samples < 2) {
        m_impl->msaa_works = false;
        return false;
    }

    glGenFramebuffers(1, &m_impl->msaa_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->msaa_framebuffer);

    // The color must match the resolve target's format exactly, which glTexImage2D(GL_RGBA, GL_UNSIGNED_BYTE) makes RGBA8.
    glGenRenderbuffers(1, &m_impl->msaa_color_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_impl->msaa_color_buffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, m_size.width(), m_size.height());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_impl->msaa_color_buffer);

    if (m_drawing_buffer_options.depth || stencil) {
        glGenRenderbuffers(1, &m_impl->depth_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);
        if (m_drawing_buffer_options.depth && stencil) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else if (m_drawing_buffer_options.depth) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_STENCIL_INDEX8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        }
    }

    // Find out once whether this works here, before the page has issued any GL commands, so that checking glGetError()
    // can't swallow one of its errors: the resolve target's format differs on some painting surfaces.
    if (!m_impl->msaa_works.has_value()) {
        while (glGetError() != GL_NO_ERROR) { }
        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete)
            resolve_drawing_buffer();
        m_impl->msaa_works = complete && glGetError() == GL_NO_ERROR;
    }

    // Even where it works, a later allocation can fail, as pages can make the drawing buffer as large as GL allows and
    // GPU memory can run out. The drawing buffer then just isn't antialiased, rather than taking the Compositor down.
    if (!*m_impl->msaa_works || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &m_impl->msaa_framebuffer);
        glDeleteRenderbuffers(1, &m_impl->msaa_color_buffer);
        if (m_impl->depth_buffer)
            glDeleteRenderbuffers(1, &m_impl->depth_buffer);
        m_impl->msaa_framebuffer = 0;
        m_impl->msaa_color_buffer = 0;
        m_impl->depth_buffer = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
        return false;
    }

    return true;
#else
    (void)stencil;
    return false;
#endif
}

// Brings the painted framebuffer up to date with what the page drew into the multisampled one. Only the color gets
// resolved: resolving multisampled depth can remove the GPU device with Mesa's d3d12 driver, see blit_framebuffer().
void OpenGLContext::resolve_drawing_buffer()
{
#ifdef ENABLE_WEBGL
    if (!m_impl->msaa_framebuffer)
        return;

    GLint original_read_framebuffer = 0;
    GLint original_draw_framebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read_framebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw_framebuffer);
    // The scissor test applies to blits, and the page's scissor rectangle mustn't.
    GLboolean scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor_test)
        glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_impl->msaa_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_impl->framebuffer);
    glBlitFramebuffer(0, 0, m_size.width(), m_size.height(), 0, 0, m_size.width(), m_size.height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (scissor_test)
        glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, original_read_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, original_draw_framebuffer);
#endif
}

// The page reads the drawing buffer's pixels from the resolved framebuffer, as a multisampled one can't be read.
// Returns whether it redirected the read framebuffer, which end_reading_drawing_buffer() then undoes.
bool OpenGLContext::begin_reading_drawing_buffer()
{
#ifdef ENABLE_WEBGL
    if (!m_impl->msaa_framebuffer)
        return false;
    GLint read_framebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    if (static_cast<GLuint>(read_framebuffer) != m_impl->msaa_framebuffer)
        return false;
    resolve_drawing_buffer();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_impl->framebuffer);
    return true;
#else
    return false;
#endif
}

void OpenGLContext::end_reading_drawing_buffer(bool redirected)
{
#ifdef ENABLE_WEBGL
    if (redirected)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_impl->msaa_framebuffer);
#else
    (void)redirected;
#endif
}

void OpenGLContext::read_pixels_robust_angle(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    auto redirected = begin_reading_drawing_buffer();
    GLFunctions::read_pixels_robust_angle(x, y, width, height, format, type, buf_size, length, columns, rows, pixels);
    end_reading_drawing_buffer(redirected);
}

void OpenGLContext::copy_tex_image2d(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)
{
    auto redirected = begin_reading_drawing_buffer();
    GLFunctions::copy_tex_image2d(target, level, internalformat, x, y, width, height, border);
    end_reading_drawing_buffer(redirected);
}

void OpenGLContext::copy_tex_sub_image2d(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height)
{
    auto redirected = begin_reading_drawing_buffer();
    GLFunctions::copy_tex_sub_image2d(target, level, xoffset, yoffset, x, y, width, height);
    end_reading_drawing_buffer(redirected);
}

bool OpenGLContext::drawing_buffer_has_stencil()
{
    make_current();
    return m_drawing_buffer_options.stencil && drawing_buffer_can_have_stencil();
}

bool OpenGLContext::drawing_buffer_can_have_stencil()
{
#ifdef ENABLE_WEBGL
    // With Mesa's d3d12 driver on WSL2 (seen with Mesa 26.2 on an Intel GPU), the first draw with the stencil test
    // enabled on a framebuffer that has a stencil buffer removes the GPU device: nothing gets drawn from then on, in any
    // WebGL context of the process. Without a stencil buffer, the stencil test just passes, so leave it out there.
    // Framebuffers of the page's own get the same treatment, see renderbuffer_format_for().
    // FIXME: Other operations can remove the device as well, like resolving multisampled depth (see blit_framebuffer()).
    //        Detecting a lost device (create the contexts with EGL_LOSE_CONTEXT_ON_RESET_EXT and check
    //        glGetGraphicsResetStatusEXT() after each frame) and letting the browser restart the Compositor would make
    //        pages see webglcontextlost for the ones not worked around yet. Application::recover_compositor_process()
    //        crashes the browser after three restarts, though, so a page that keeps doing this would need to be
    //        stopped before then.
    return !uses_mesa_d3d12();
#else
    return true;
#endif
}

// OpenGL ES 3.0 can't say what format a texture image got, which the stencil workarounds need to know after
// texImage2D() and texImage3D(), see texture_image_is(). ANGLE can, once asked to.
void OpenGLContext::enable_texture_level_queries()
{
#ifdef ENABLE_WEBGL
    auto lists = [](GLenum name) {
        auto const* extensions = reinterpret_cast<char const*>(glGetString(name));
        return extensions && StringView { extensions, strlen(extensions) }.split_view(' ').contains_slow("GL_ANGLE_get_tex_level_parameter"sv);
    };
    if (!lists(GL_EXTENSIONS)) {
        if (!lists(GL_REQUESTABLE_EXTENSIONS_ANGLE))
            return;
        auto request_extension = reinterpret_cast<PFNGLREQUESTEXTENSIONANGLEPROC>(eglGetProcAddress("glRequestExtensionANGLE"));
        if (!request_extension)
            return;
        request_extension("GL_ANGLE_get_tex_level_parameter");
        if (!lists(GL_EXTENSIONS))
            return;
    }
    m_impl->get_tex_level_parameteriv = reinterpret_cast<PFNGLGETTEXLEVELPARAMETERIVANGLEPROC>(eglGetProcAddress("glGetTexLevelParameterivANGLE"));
#endif
}

bool OpenGLContext::uses_mesa_d3d12()
{
#ifdef ENABLE_WEBGL
    if (!m_impl->uses_mesa_d3d12.has_value()) {
        auto const* renderer = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
        m_impl->uses_mesa_d3d12 = renderer && StringView { renderer, strlen(renderer) }.contains("D3D12"sv);
        if (*m_impl->uses_mesa_d3d12)
            enable_texture_level_queries();
    }
    return *m_impl->uses_mesa_d3d12;
#else
    return false;
#endif
}

void OpenGLContext::blit_framebuffer(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1, GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1, GLbitfield mask, GLenum filter)
{
#ifdef ENABLE_WEBGL
    // With Mesa's d3d12 driver on WSL2 (seen with Mesa 26.2 on an Intel GPU), resolving the depth of a multisampled
    // framebuffer removes the GPU device, unless its depth buffer is DEPTH_COMPONENT16 or DEPTH_COMPONENT32F: nothing
    // gets drawn from then on, in any WebGL context of the process. three.js does this for every multisampled render
    // target. Resolving just the color keeps pages working, as few of them read the resolved depth back.
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && uses_mesa_d3d12() && read_framebuffer_has_unresolvable_depth()) {
        mask &= ~(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (!mask)
            return;
    }

    // Blitting color from an antialiased drawing buffer reads the resolved one, like any other read of its pixels. Its
    // depth and stencil stay in the multisampled framebuffer.
    if (mask & GL_COLOR_BUFFER_BIT) {
        if (auto redirected = begin_reading_drawing_buffer()) {
            GLFunctions::blit_framebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, GL_COLOR_BUFFER_BIT, filter);
            end_reading_drawing_buffer(redirected);
            mask &= ~GL_COLOR_BUFFER_BIT;
            if (!mask)
                return;
        }
    }
#endif
    GLFunctions::blit_framebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

// Picks a depth-only format for a format with stencil, or returns the format as it is.
static GLenum format_without_stencil(GLenum format)
{
    switch (format) {
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH_STENCIL_OES:
        return GL_DEPTH_COMPONENT24;
    case GL_DEPTH32F_STENCIL8:
        return GL_DEPTH_COMPONENT32F;
    case GL_STENCIL_INDEX8:
        return GL_DEPTH_COMPONENT16;
    default:
        return format;
    }
}

GLenum OpenGLContext::renderbuffer_format_for(GLenum format)
{
#ifdef ENABLE_WEBGL
    // Pages can give framebuffers of their own a stencil buffer, which runs into the same device loss with Mesa's d3d12
    // driver as the drawing buffer does, see drawing_buffer_can_have_stencil(). So they get depth only there, too.
    return uses_mesa_d3d12() ? format_without_stencil(format) : format;
#else
    return format;
#endif
}

// Records the format substitution of the bound renderbuffer, once GL has allocated its storage. GL can reject the
// allocation, keeping the renderbuffer as it was, so this goes by what the renderbuffer actually got. That can't use
// glGetError(), which would take the error from the page.
void OpenGLContext::note_renderbuffer_storage(GLenum requested_format, GLenum format, GLsizei width, GLsizei height, bool within_limits)
{
#ifdef ENABLE_WEBGL
    if (!uses_mesa_d3d12())
        return;
    GLint renderbuffer = 0;
    GLint allocated_format = GL_NONE;
    GLint allocated_width = 0;
    GLint allocated_height = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    if (!renderbuffer)
        return;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &allocated_format);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &allocated_width);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &allocated_height);
    // The renderbuffer must have exactly the format it was given. Every format a page can give a renderbuffer is sized
    // (DEPTH_STENCIL always gets substituted), so GL reports it as it is. Failing the same way, but with storage that
    // already matched, looks the same afterwards, so the allocation must also have been within GL's limits.
    bool substituted = format != requested_format;
    bool allocated = within_limits && allocated_width == width && allocated_height == height && static_cast<GLenum>(allocated_format) == format;
    if (!allocated)
        return;
    if (substituted)
        m_impl->renderbuffers_without_stencil.set(static_cast<GLuint>(renderbuffer), requested_format);
    else
        m_impl->renderbuffers_without_stencil.remove(static_cast<GLuint>(renderbuffer));
#else
    (void)requested_format;
    (void)format;
    (void)width;
    (void)height;
#endif
}

void OpenGLContext::renderbuffer_storage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
    auto format = renderbuffer_format_for(internalformat);
    bool within_limits = renderbuffer_storage_is_within_limits(0, width, height);
    GLFunctions::renderbuffer_storage(target, format, width, height);
    note_renderbuffer_storage(internalformat, format, width, height, within_limits);
}

void OpenGLContext::renderbuffer_storage_multisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height)
{
    auto format = renderbuffer_format_for(internalformat);
    bool within_limits = renderbuffer_storage_is_within_limits(samples, width, height);
    GLFunctions::renderbuffer_storage_multisample(target, samples, format, width, height);
    note_renderbuffer_storage(internalformat, format, width, height, within_limits);
}

bool OpenGLContext::renderbuffer_storage_is_within_limits(GLsizei samples, GLsizei width, GLsizei height)
{
#ifdef ENABLE_WEBGL
    if (!uses_mesa_d3d12())
        return false;
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
    GLint max_samples = 0;
    if (samples > 0)
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    return width >= 0 && height >= 0 && width <= max_size && height <= max_size && samples >= 0 && samples <= max_samples;
#else
    (void)samples;
    (void)width;
    (void)height;
    return false;
#endif
}

void OpenGLContext::delete_renderbuffers(GLsizei n, GLuint const* renderbuffers)
{
#ifdef ENABLE_WEBGL
    // GL may give a deleted renderbuffer's name to a new one, which must not inherit the old one's substitution.
    for (GLsizei i = 0; i < n; ++i)
        m_impl->renderbuffers_without_stencil.remove(renderbuffers[i]);
#endif
    GLFunctions::delete_renderbuffers(n, renderbuffers);
}

void OpenGLContext::framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
#ifdef ENABLE_WEBGL
    if (auto original_format = m_impl->renderbuffers_without_stencil.get(renderbuffer); original_format.has_value()) {
        // The renderbuffer has no stencil, so it can only be the depth attachment, if the page wanted depth from it.
        bool has_depth = *original_format != GL_STENCIL_INDEX8;
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
            attachment = has_depth ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
        if (attachment == GL_STENCIL_ATTACHMENT || (attachment == GL_DEPTH_ATTACHMENT && !has_depth))
            renderbuffer = 0;
    }
#endif
    GLFunctions::framebuffer_renderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}

// Depth-stencil textures run into the same device loss with Mesa's d3d12 driver as stencil renderbuffers do (clearing
// their stencil is enough), see renderbuffer_format_for(). So on d3d12 every depth-stencil texture image gets a
// depth-only format, whichever call allocates it (tex_storage2d(), tex_storage3d(), tex_image2d_robust_angle(),
// tex_image3d_robust_angle()) and whatever its mip level, and is only ever attached as depth. WebGL can attach any mip
// level of a 2D texture or cube map face, and any layer of a 2D array texture (depth formats can't be 3D textures), so
// what got substituted is tracked per attachable image: the texture, the kind of image and the mip level. The layers of
// a 2D array texture share their level's format.

// Which image of a texture a target names: 0 for a 2D texture, 1 to 6 for the faces of a cube map, 7 for a 2D array
// texture. Other targets can't hold depth.
static Optional<u8> texture_image_kind(GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return 0;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        return static_cast<u8>(1 + target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    if (target == GL_TEXTURE_2D_ARRAY)
        return 7;
    return {};
}

static u64 texture_image_key(GLuint texture, u8 kind, GLint level)
{
    return (static_cast<u64>(texture) << 32) | (static_cast<u64>(kind) << 16) | static_cast<u16>(level);
}

static GLenum texture_binding_for(GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return GL_TEXTURE_BINDING_2D;
    if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
        return GL_TEXTURE_BINDING_CUBE_MAP;
    if (target == GL_TEXTURE_2D_ARRAY)
        return GL_TEXTURE_BINDING_2D_ARRAY;
    return GL_NONE;
}

static GLenum depth_only_format_for(GLenum internalformat)
{
    if (internalformat == GL_DEPTH24_STENCIL8)
        return GL_DEPTH_COMPONENT24;
    if (internalformat == GL_DEPTH32F_STENCIL8)
        return GL_DEPTH_COMPONENT32F;
    return internalformat;
}

// GL accepts 1 to log2(largest dimension) + 1 mip levels, and rejects other counts, allocating nothing. Nothing may be
// tracked for those either: a page asking for 2^31 levels would otherwise keep the Compositor busy for minutes.
static bool is_valid_level_count(GLsizei levels, GLsizei width, GLsizei height)
{
    if (levels < 1 || width < 1 || height < 1)
        return false;
    auto largest = static_cast<u32>(max(width, height));
    GLsizei max_levels = 1;
    while (largest >>= 1)
        ++max_levels;
    return levels <= max_levels;
}

// No texture can have mip levels beyond this, whatever its size, so GL rejects them.
static constexpr GLint max_mip_level = 31;

// A depth-stencil upload, turned into a depth-only one by drop_stencil_from_upload().
struct DepthOnlyUpload {
    // Set when the data had to be repacked. It's tightly packed, unlike the page's data, see UnpackStateOverride.
    ByteBuffer repacked;
};

// Turns a depth-stencil upload into a depth-only one. Without data, which is how render targets get made, that only means
// other formats. UNSIGNED_INT_24_8 data keeps its depth in the upper 24 bits of each 32-bit texel, which is exactly
// what uploading the same bytes as UNSIGNED_INT into a 24-bit depth format keeps. FLOAT_32_UNSIGNED_INT_24_8_REV data
// has 8 bytes per texel, a float depth followed by the stencil, so its depths get repacked. Returns nothing if the upload
// isn't depth-stencil, or if the data is too short, in which case GL rejects the upload anyway.
static Optional<DepthOnlyUpload> drop_stencil_from_upload(GLint* internalformat, GLenum& format, GLenum& type, GLsizei width, GLsizei height, GLsizei depth, GLsizei& buf_size, void const*& pixels, bool is_3d)
{
    if (format != GL_DEPTH_STENCIL_OES || width < 0 || height < 0 || depth < 0)
        return {};

    DepthOnlyUpload upload;
    if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
        if (pixels && width > 0 && height > 0 && depth > 0) {
            GLint alignment = 4, row_length = 0, skip_pixels = 0, skip_rows = 0, image_height = 0, skip_images = 0;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
            glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
            if (is_3d) {
                glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
                glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
            }
            if (alignment < 1 || row_length < 0 || skip_pixels < 0 || skip_rows < 0 || image_height < 0 || skip_images < 0)
                return {};

            // The page's data is laid out the way GL reads it, see "Unpacking" in the OpenGL ES 3.0 spec. All of this is
            // page-controlled and not validated by GL yet, so every step is checked for overflow, and an overflow leaves
            // the upload to GL, which rejects it. Once the end of the data is known to fit in buf_size, no offset of a
            // texel before it can overflow.
            constexpr u64 texel_size = 8;
            Checked<u64> row_stride = static_cast<u64>(row_length > 0 ? row_length : width);
            row_stride *= texel_size;
            row_stride += static_cast<u64>(alignment - 1);
            if (row_stride.has_overflow())
                return {};
            u64 aligned_row_stride = row_stride.value() / static_cast<u64>(alignment) * static_cast<u64>(alignment);
            Checked<u64> image_stride = aligned_row_stride;
            image_stride *= static_cast<u64>(image_height > 0 ? image_height : height);
            if (image_stride.has_overflow())
                return {};

            Checked<u64> end = static_cast<u64>(skip_images) + static_cast<u64>(depth) - 1;
            end *= image_stride.value();
            Checked<u64> rows = static_cast<u64>(skip_rows) + static_cast<u64>(height) - 1;
            rows *= aligned_row_stride;
            end += rows;
            Checked<u64> pixels_in_row = static_cast<u64>(skip_pixels) + static_cast<u64>(width);
            pixels_in_row *= texel_size;
            end += pixels_in_row;
            if (end.has_overflow() || end.value() > static_cast<u64>(max(buf_size, 0)))
                return {};

            auto offset_of = [&](u64 x, u64 y, u64 z) {
                return (static_cast<u64>(skip_images) + z) * image_stride.value() + (static_cast<u64>(skip_rows) + y) * aligned_row_stride + (static_cast<u64>(skip_pixels) + x) * texel_size;
            };
            // The data's end doesn't bound the repacked size: rows and images can overlap (e.g. UNPACK_ROW_LENGTH = 1), so
            // the texel count can be far larger, up to overflowing. Like the overflows above, such uploads are left to GL
            // unchanged, and so is anything whose repacked size doesn't fit buf_size's type.
            Checked<size_t> repacked_size = static_cast<size_t>(width);
            repacked_size *= static_cast<size_t>(height);
            repacked_size *= static_cast<size_t>(depth);
            repacked_size *= sizeof(float);
            if (repacked_size.has_overflow() || repacked_size.value() > static_cast<size_t>(NumericLimits<GLsizei>::max()))
                return {};
            auto repacked = ByteBuffer::create_uninitialized(repacked_size.value());
            if (repacked.is_error())
                return {};
            upload.repacked = repacked.release_value();
            auto const* source = static_cast<u8 const*>(pixels);
            auto* destination = upload.repacked.data();
            for (GLsizei z = 0; z < depth; ++z) {
                for (GLsizei y = 0; y < height; ++y) {
                    for (GLsizei x = 0; x < width; ++x) {
                        memcpy(destination, source + offset_of(x, y, z), sizeof(float));
                        destination += sizeof(float);
                    }
                }
            }
            pixels = upload.repacked.data();
            buf_size = static_cast<GLsizei>(upload.repacked.size());
        }
        if (internalformat)
            *internalformat = GL_DEPTH_COMPONENT32F;
        type = GL_FLOAT;
    } else if (type == GL_UNSIGNED_INT_24_8_OES) {
        if (internalformat)
            *internalformat = *internalformat == GL_DEPTH24_STENCIL8 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT;
        type = GL_UNSIGNED_INT;
    } else {
        return {};
    }
    format = GL_DEPTH_COMPONENT;
    return upload;
}

// Sets tightly packed unpack state for uploading repacked data, and puts the page's back afterwards.
class UnpackStateOverride {
public:
    explicit UnpackStateOverride(bool active)
        : m_active(active)
    {
        if (!m_active)
            return;
        for (size_t i = 0; i < parameters.size(); ++i) {
            glGetIntegerv(parameters[i], &m_saved[i]);
            glPixelStorei(parameters[i], parameters[i] == GL_UNPACK_ALIGNMENT ? 4 : 0);
        }
    }

    ~UnpackStateOverride()
    {
        if (!m_active)
            return;
        for (size_t i = 0; i < parameters.size(); ++i)
            glPixelStorei(parameters[i], m_saved[i]);
    }

private:
    static constexpr Array<GLenum, 6> parameters { GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES };
    bool m_active { false };
    Array<GLint, 6> m_saved {};
};

void OpenGLContext::note_texture_stencil(GLenum target, GLint first_level, GLint level_count, bool has_stencil_dropped)
{
    auto binding = texture_binding_for(target);
    if (binding == GL_NONE)
        return;
    GLint texture = 0;
    glGetIntegerv(binding, &texture);

    // Allocating a whole cube map (texStorage2D) covers all of its faces.
    Vector<u8, 6> kinds;
    if (target == GL_TEXTURE_CUBE_MAP) {
        for (u8 face = 1; face <= 6; ++face)
            kinds.append(face);
    } else if (auto kind = texture_image_kind(target); kind.has_value()) {
        kinds.append(*kind);
    }

    for (auto kind : kinds) {
        for (GLint level = first_level; level < first_level + level_count; ++level) {
            auto key = texture_image_key(static_cast<GLuint>(texture), kind, level);
            if (has_stencil_dropped)
                m_impl->texture_images_without_stencil.set(key);
            else
                m_impl->texture_images_without_stencil.remove(key);
        }
    }
}

// Whether the texture bound to target is immutable, i.e. has storage from texStorage2D()/texStorage3D(), and how many levels
// it got. Only WebGL 2 contexts have immutable textures.
static Optional<GLint> immutable_levels_of_bound_texture(GLenum target)
{
    auto parameter_target = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? GL_TEXTURE_CUBE_MAP : target;
    GLint immutable = GL_FALSE;
    glGetTexParameteriv(parameter_target, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    if (!immutable)
        return {};
    GLint levels = 0;
    glGetTexParameteriv(parameter_target, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
    return levels;
}

void OpenGLContext::tex_storage2d(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
#ifdef ENABLE_WEBGL
    if (uses_mesa_d3d12() && texture_binding_for(target) != GL_NONE) {
        auto depth_only_format = depth_only_format_for(internalformat);
        // GL rejects storage for a texture that already has some, and impossible level counts, keeping the texture as it
        // was. Only a texture that GL made immutable with these levels got them.
        bool was_immutable = immutable_levels_of_bound_texture(target).has_value();
        GLFunctions::tex_storage2d(target, levels, depth_only_format, width, height);
        if (!was_immutable && is_valid_level_count(levels, width, height) && immutable_levels_of_bound_texture(target) == levels)
            note_texture_stencil(target, 0, levels, depth_only_format != internalformat);
        return;
    }
#endif
    GLFunctions::tex_storage2d(target, levels, internalformat, width, height);
}

void OpenGLContext::tex_storage3d(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)
{
#ifdef ENABLE_WEBGL
    if (uses_mesa_d3d12() && target == GL_TEXTURE_2D_ARRAY) {
        auto depth_only_format = depth_only_format_for(internalformat);
        bool was_immutable = immutable_levels_of_bound_texture(target).has_value();
        GLFunctions::tex_storage3d(target, levels, depth_only_format, width, height, depth);
        // The layers of a 2D array texture don't get smaller with its mip levels.
        if (!was_immutable && is_valid_level_count(levels, width, height) && immutable_levels_of_bound_texture(target) == levels)
            note_texture_stencil(target, 0, levels, depth_only_format != internalformat);
        return;
    }
#endif
    GLFunctions::tex_storage3d(target, levels, internalformat, width, height, depth);
}

void OpenGLContext::tex_image2d_robust_angle(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, GLsizei buf_size, void const* pixels)
{
#ifdef ENABLE_WEBGL
    if (uses_mesa_d3d12() && texture_image_kind(target).has_value() && level >= 0 && level <= max_mip_level) {
        auto requested_internalformat = internalformat;
        auto upload = drop_stencil_from_upload(&internalformat, format, type, width, height, 1, buf_size, pixels, false);
        bool can_succeed = tex_image_can_succeed(target, width, height, 1, border);
        {
            UnpackStateOverride unpack_state { upload.has_value() && !upload->repacked.is_empty() };
            GLFunctions::tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, buf_size, pixels);
        }
        if (can_succeed && texture_image_is(target, level, upload.has_value() ? GL_NONE : requested_internalformat, width, height, 1))
            note_texture_stencil(target, level, 1, upload.has_value());
        return;
    }
#endif
    GLFunctions::tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, buf_size, pixels);
}

void OpenGLContext::tex_image3d_robust_angle(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, GLsizei buf_size, void const* pixels)
{
#ifdef ENABLE_WEBGL
    if (uses_mesa_d3d12() && target == GL_TEXTURE_2D_ARRAY && level >= 0 && level <= max_mip_level) {
        auto requested_internalformat = internalformat;
        auto upload = drop_stencil_from_upload(&internalformat, format, type, width, height, depth, buf_size, pixels, true);
        bool can_succeed = tex_image_can_succeed(target, width, height, depth, border);
        {
            UnpackStateOverride unpack_state { upload.has_value() && !upload->repacked.is_empty() };
            GLFunctions::tex_image3d_robust_angle(target, level, internalformat, width, height, depth, border, format, type, buf_size, pixels);
        }
        if (can_succeed && texture_image_is(target, level, upload.has_value() ? GL_NONE : requested_internalformat, width, height, depth))
            note_texture_stencil(target, level, 1, upload.has_value());
        return;
    }
#endif
    GLFunctions::tex_image3d_robust_angle(target, level, internalformat, width, height, depth, border, format, type, buf_size, pixels);
}

// Whether the texture image at target and level now is what texImage2D()/texImage3D() asked for, i.e. whether GL accepted
// the call. For a depth-stencil request that got a depth-only format (internalformat GL_NONE), that means some depth-only
// format; for a sized format, exactly that one. Unsized formats (like GL_RGBA) are reported sized, so only their size is
// compared. Without ANGLE_get_tex_level_parameter, this can't be told, and tex_image_can_succeed() has to do.
// The texture level parameters ANGLE_get_tex_level_parameter answers, from OpenGL ES 3.1.
static constexpr GLenum texture_width_parameter = 0x1000;          // GL_TEXTURE_WIDTH
static constexpr GLenum texture_height_parameter = 0x1001;         // GL_TEXTURE_HEIGHT
static constexpr GLenum texture_internal_format_parameter = 0x1003; // GL_TEXTURE_INTERNAL_FORMAT
static constexpr GLenum texture_depth_parameter = 0x8071;          // GL_TEXTURE_DEPTH

static bool is_unsized_format(GLint format)
{
    switch (format) {
    case GL_RGB:
    case GL_RGBA:
    case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA:
    case GL_ALPHA:
    case GL_DEPTH_COMPONENT:
    case GL_DEPTH_STENCIL_OES:
        return true;
    default:
        return false;
    }
}

bool OpenGLContext::texture_image_is(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth)
{
#ifdef ENABLE_WEBGL
    auto get = m_impl->get_tex_level_parameteriv;
    if (!get)
        return true;
    GLint actual_format = GL_NONE, actual_width = 0, actual_height = 0, actual_depth = 1;
    get(target, level, texture_internal_format_parameter, &actual_format);
    get(target, level, texture_width_parameter, &actual_width);
    get(target, level, texture_height_parameter, &actual_height);
    if (target == GL_TEXTURE_2D_ARRAY)
        get(target, level, texture_depth_parameter, &actual_depth);
    if (actual_width != width || actual_height != height || actual_depth != depth)
        return false;
    if (internalformat == GL_NONE)
        return actual_format == GL_DEPTH_COMPONENT16 || actual_format == GL_DEPTH_COMPONENT24 || actual_format == GL_DEPTH_COMPONENT32F;
    return is_unsized_format(internalformat) || actual_format == internalformat;
#else
    (void)target;
    (void)level;
    (void)internalformat;
    (void)width;
    (void)height;
    (void)depth;
    return false;
#endif
}

// texImage2D()/texImage3D() leave the texture as it was when GL rejects them, so their substitution must only be recorded
// when they can succeed. There's no way to ask GL afterwards (OpenGL ES 3.0 can't query a texture level's format), and
// glGetError() would take the error from the page, so this rules out what GL is sure to reject.
bool OpenGLContext::tex_image_can_succeed(GLenum target, GLsizei width, GLsizei height, GLsizei depth, GLint border)
{
    if (border != 0 || width < 0 || height < 0 || depth < 0)
        return false;
    if (m_webgl_version == WebGLVersion::WebGL2 && immutable_levels_of_bound_texture(target).has_value())
        return false;
    bool is_cube_map_face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
    if (is_cube_map_face && width != height)
        return false;
    GLint max_size = 0;
    glGetIntegerv(is_cube_map_face ? GL_MAX_CUBE_MAP_TEXTURE_SIZE : GL_MAX_TEXTURE_SIZE, &max_size);
    if (width > max_size || height > max_size)
        return false;
    if (target == GL_TEXTURE_2D_ARRAY) {
        GLint max_layers = 0;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
        if (depth > max_layers)
            return false;
    }
    return true;
}

// Whether the image at the given level of the texture bound to target got a depth-only format, see note_texture_stencil().
bool OpenGLContext::bound_texture_image_has_stencil_dropped(GLenum target, GLint level)
{
    auto kind = texture_image_kind(target);
    auto binding = texture_binding_for(target);
    if (!kind.has_value() || binding == GL_NONE || level < 0 || level > max_mip_level)
        return false;
    GLint texture = 0;
    glGetIntegerv(binding, &texture);
    return m_impl->texture_images_without_stencil.contains(texture_image_key(static_cast<GLuint>(texture), *kind, level));
}

// Depth-stencil data for an image that got a depth-only format has to become depth-only data as well, or GL rejects it.
void OpenGLContext::tex_sub_image2d_robust_angle(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size, void const* pixels)
{
#ifdef ENABLE_WEBGL
    Optional<DepthOnlyUpload> upload;
    if (format == GL_DEPTH_STENCIL_OES && !m_impl->texture_images_without_stencil.is_empty() && bound_texture_image_has_stencil_dropped(target, level))
        upload = drop_stencil_from_upload(nullptr, format, type, width, height, 1, buf_size, pixels, false);
    UnpackStateOverride unpack_state { upload.has_value() && !upload->repacked.is_empty() };
#endif
    GLFunctions::tex_sub_image2d_robust_angle(target, level, xoffset, yoffset, width, height, format, type, buf_size, pixels);
}

void OpenGLContext::tex_sub_image3d_robust_angle(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, GLsizei buf_size, void const* pixels)
{
#ifdef ENABLE_WEBGL
    Optional<DepthOnlyUpload> upload;
    if (format == GL_DEPTH_STENCIL_OES && !m_impl->texture_images_without_stencil.is_empty() && bound_texture_image_has_stencil_dropped(target, level))
        upload = drop_stencil_from_upload(nullptr, format, type, width, height, depth, buf_size, pixels, true);
    UnpackStateOverride unpack_state { upload.has_value() && !upload->repacked.is_empty() };
#endif
    GLFunctions::tex_sub_image3d_robust_angle(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, buf_size, pixels);
}

// A texture image that has no stencil can only be the depth attachment.
void OpenGLContext::attach_texture_image_without_stencil(GLenum& attachment, GLuint& texture, u8 kind, GLint level)
{
    if (!m_impl->texture_images_without_stencil.contains(texture_image_key(texture, kind, level)))
        return;
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT)
        attachment = GL_DEPTH_ATTACHMENT;
    else if (attachment == GL_STENCIL_ATTACHMENT)
        texture = 0;
}

void OpenGLContext::framebuffer_texture2d(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
#ifdef ENABLE_WEBGL
    if (auto kind = texture_image_kind(textarget); kind.has_value() && texture != 0)
        attach_texture_image_without_stencil(attachment, texture, *kind, level);
#endif
    GLFunctions::framebuffer_texture2d(target, attachment, textarget, texture, level);
}

void OpenGLContext::framebuffer_texture_layer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
#ifdef ENABLE_WEBGL
    // Of the textures with layers, only 2D arrays can hold depth.
    if (texture != 0)
        attach_texture_image_without_stencil(attachment, texture, *texture_image_kind(GL_TEXTURE_2D_ARRAY), level);
#endif
    GLFunctions::framebuffer_texture_layer(target, attachment, texture, level, layer);
}

void OpenGLContext::delete_textures(GLsizei n, GLuint const* textures)
{
#ifdef ENABLE_WEBGL
    // GL may give a deleted texture's name to a new texture, which must not inherit the old one's substitutions.
    if (!m_impl->texture_images_without_stencil.is_empty()) {
        for (GLsizei i = 0; i < n; ++i) {
            auto texture = static_cast<u64>(textures[i]);
            m_impl->texture_images_without_stencil.remove_all_matching([&](u64 key) { return (key >> 32) == texture; });
        }
    }
#endif
    GLFunctions::delete_textures(n, textures);
}

bool OpenGLContext::read_framebuffer_has_unresolvable_depth()
{
#ifdef ENABLE_WEBGL
    GLint read_framebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    if (read_framebuffer == 0 || static_cast<GLuint>(read_framebuffer) == m_impl->framebuffer)
        return false;

    for (GLenum attachment : { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT }) {
        GLint type = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type != GL_RENDERBUFFER)
            continue;
        GLint renderbuffer = 0;
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &renderbuffer);

        GLint original_renderbuffer = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &original_renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        GLint samples = 0;
        GLint format = GL_NONE;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
        glBindRenderbuffer(GL_RENDERBUFFER, original_renderbuffer);

        if (samples > 0 && format != GL_DEPTH_COMPONENT16 && format != GL_DEPTH_COMPONENT32F)
            return true;
    }
    return false;
#else
    return false;
#endif
}

void OpenGLContext::set_size(Gfx::IntSize const& size)
{
    if (m_size != size) {
        m_painting_surface = nullptr;
        m_impl->readback_pending = false;
    }
    m_size = size;
}

void OpenGLContext::make_current()
{
#ifdef ENABLE_WEBGL
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);
#endif
}

void OpenGLContext::present()
{
#ifdef ENABLE_WEBGL
    make_current();

#    if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    if (m_impl->uses_cpu_painting_surface) {
        // Reading the drawing buffer back waits for all rendering to it to finish, so it needs no glFinish(). The glFlush()
        // works around a deadlock in the d3d12 driver of Mesa 26.2 (which WSL2 uses): reading back 32 to 64 KiB, like a
        // 100x100 canvas, gets a staging buffer that takes up a whole slab, so the next readback needs a new slab. While
        // creating it, the driver frees the buffers the GPU is done with, still holding the slab allocator's lock
        // (d3d12_bo_new() calls d3d12_screen_reclaim_completed() inside pb_slab_manager_create_buffer()). Freeing the
        // previous readback's staging buffer then takes that same lock, and the Compositor hangs. Submitting the frame
        // first frees those buffers with no lock held. It doesn't help when nothing was drawn since the last readback,
        // as there's nothing to submit then. The asynchronous readback isn't affected, since Mesa never puts a pixel pack
        // buffer in a slab.
        glFlush();
        copy_default_framebuffer_to_cpu_painting_surface();
        return;
    }
#    endif

    // The painting surface is the resolved framebuffer's color.
    resolve_drawing_buffer();

    // "Before the drawing buffer is presented for compositing the implementation shall ensure that all rendering operations have been flushed to the drawing buffer."
    // With Metal, glFlush flushes the command buffer, but without waiting for it to be scheduled or completed.
    // eglWaitUntilWorkScheduledANGLE flushes the command buffer, and waits until it has been scheduled, hence the name.
    // eglWaitUntilWorkScheduledANGLE only has an effect on CGL and Metal backends, so we only use it on macOS.
#    if defined(AK_OS_MACOS)
    eglWaitUntilWorkScheduledANGLE(m_impl->display);
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    // FIXME: CPU sync for now, but it would be better to export a fence and have Skia wait for it before reading from the surface
    glFinish();
#    endif
#endif
}

bool OpenGLContext::present_asynchronously()
{
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    // Reading the frame back into client memory would block the Compositor until the GPU has finished it. A pixel pack
    // buffer (which needs OpenGL ES 3, so WebGL 2) lets the GPU make the copy, to be picked up once the frame is needed.
    if (m_impl->uses_cpu_painting_surface && m_webgl_version == WebGLVersion::WebGL2) {
        make_current();
        if (!m_impl->readback_buffer)
            glGenBuffers(1, &m_impl->readback_buffer);
        if (m_impl->readback_buffer_size != m_size) {
            GLint original_pixel_pack_buffer = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &original_pixel_pack_buffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_impl->readback_buffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(m_size.width()) * m_size.height() * 4, nullptr, GL_STREAM_READ);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, original_pixel_pack_buffer);
            m_impl->readback_buffer_size = m_size;
        }
        read_default_framebuffer(m_impl->readback_buffer, nullptr);
        glFlush();
        m_impl->readback_pending = true;
        return true;
    }
#endif
    present();
    return false;
}

void OpenGLContext::finish_asynchronous_present()
{
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    if (!m_impl->readback_pending)
        return;
    m_impl->readback_pending = false;
    make_current();
    VERIFY(m_painting_surface);

    auto& bitmap = readback_bitmap();
    auto row_size = static_cast<size_t>(m_size.width()) * 4;

    GLint original_pixel_pack_buffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &original_pixel_pack_buffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_impl->readback_buffer);

    // Mapping the buffer waits for the copy into it, if the GPU hasn't made it yet.
    auto const* pixels = static_cast<u8 const*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(row_size) * m_size.height(), GL_MAP_READ_BIT));
    if (pixels) {
        // GL's rows go from bottom to top, so flip them on the way into the bitmap.
        for (int source_y = 0; source_y < m_size.height(); ++source_y)
            memcpy(bitmap.scanline_u8(m_size.height() - source_y - 1), pixels + source_y * row_size, row_size);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, original_pixel_pack_buffer);

    if (!pixels) {
        dbgln("Compositor: Could not map the buffer a WebGL frame was read back into");
        return;
    }
    m_painting_surface->write_from_bitmap(bitmap);
#endif
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::surface()
{
    return m_painting_surface;
}

u32 OpenGLContext::default_renderbuffer() const
{
    return m_impl->depth_buffer;
}

u32 OpenGLContext::default_framebuffer() const
{
    // The page draws into the multisampled framebuffer, when there is one.
    if (m_impl->msaa_framebuffer)
        return m_impl->msaa_framebuffer;
    return m_impl->framebuffer;
}

Vector<String> OpenGLContext::get_supported_opengl_extensions()
{
#ifdef ENABLE_WEBGL
    make_current();

    Vector<String> extensions;

    if (auto const* extensions_string = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS))) {
        StringView extensions_view(extensions_string, strlen(extensions_string));
        for (auto extension : extensions_view.split_view(' '))
            extensions.append(MUST(String::from_utf8(extension)));
    }

    if (auto const* requestable_extensions_string = reinterpret_cast<char const*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE))) {
        StringView requestable_extensions_view(requestable_extensions_string, strlen(requestable_extensions_string));
        for (auto extension : requestable_extensions_view.split_view(' '))
            extensions.append(MUST(String::from_utf8(extension)));
    }

    return extensions;
#else
    (void)m_webgl_version;
    return {};
#endif
}

}
