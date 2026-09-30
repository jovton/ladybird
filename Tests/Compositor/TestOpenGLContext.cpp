/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <Compositor/OpenGLContext.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
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

// EGL has to be shut down before the process exits. Otherwise the GPU driver can still have a thread running when its
// library gets unloaded during exit, which crashes with Mesa's d3d12 driver on WSL. Shutting it down after each test
// doesn't work there either, as the Intel driver crashes when EGL gets initialized again in the same process.
static void shut_down_egl_at_exit(EGLDisplay display)
{
    static EGLDisplay s_display = EGL_NO_DISPLAY;
    if (display == EGL_NO_DISPLAY || s_display != EGL_NO_DISPLAY)
        return;
    s_display = display;
    atexit([] {
        eglTerminate(s_display);
        eglReleaseThread();
    });
}

TEST_CASE(small_webgl_frames_can_be_presented_one_after_another)
{
    auto context = Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL1, drawing_buffer_options);
    if (!context) {
        warnln("No EGL display available, skipping");
        return;
    }

    // With Mesa's d3d12 driver, reading back a drawing buffer this size could hang the second time, see OpenGLContext::present().
    // That only happens while the driver has no free buffer of that size cached yet, which is why this test comes first.
    Gfx::IntSize size { 100, 100 };
    context->set_size(size);
    context->make_current();
    shut_down_egl_at_exit(eglGetCurrentDisplay());
    for (int frame = 1; frame <= 3; ++frame) {
        context->make_current();
        glBindFramebuffer(GL_FRAMEBUFFER, context->default_framebuffer());
        glClearColor(static_cast<float>(frame * 80) / 255, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        context->present();
    }

    auto surface = context->surface();
    EXPECT(surface);
    if (surface) {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
        surface->read_into_bitmap(*bitmap);
        EXPECT_EQ(bitmap->get_pixel(50, 50), Gfx::Color(240, 0, 0));
    }
}

// Paints each row of the drawing buffer its own color, and checks that it reaches the painting surface the right way up.
// Returns the EGL display the context used.
static EGLDisplay check_webgl_rows_reach_the_painting_surface(RefPtr<Gfx::SkiaBackendContext> backend)
{
    auto context = Compositor::OpenGLContext::create(move(backend), Compositor::OpenGLContext::WebGLVersion::WebGL1, drawing_buffer_options);
    EXPECT(context);
    if (!context)
        return EGL_NO_DISPLAY;

    Gfx::IntSize size { 4, 3 };
    context->set_size(size);
    context->make_current();
    auto display = eglGetCurrentDisplay();
    glBindFramebuffer(GL_FRAMEBUFFER, context->default_framebuffer());

    // GL counts rows from the bottom.
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
        return display;

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    surface->read_into_bitmap(*bitmap);
    for (int row = 0; row < size.height(); ++row) {
        for (int x = 0; x < size.width(); ++x)
            EXPECT_EQ(bitmap->get_pixel(x, row), Gfx::Color(static_cast<u8>((size.height() - row) * 40), 0, 255));
    }
    return display;
}

TEST_CASE(webgl_works_on_vulkan_devices_that_cannot_share_dmabufs)
{
    // Without a GPU backend, WebGL also paints into a CPU surface, so this tells whether EGL works here at all.
    if (!Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL1, drawing_buffer_options)) {
        warnln("No EGL display available, skipping");
        return;
    }

    auto backend = adopt_ref(*new VulkanBackendContextWithoutDmaBufs);
    EXPECT(!backend->vulkan_context().supports_dmabuf_images);
    shut_down_egl_at_exit(check_webgl_rows_reach_the_painting_surface(move(backend)));
}

// With Mesa's d3d12 driver, clearing the stencil of a depth-stencil texture loses the GPU device, so there every
// depth-stencil texture image must end up depth-only, however it was allocated and whichever mip level or layer gets
// attached, see OpenGLContext::tex_storage2d(). Other drivers keep the stencil. This only attaches the images, it never
// clears stencil, so it can't lose the device itself.
TEST_CASE(depth_stencil_texture_images_are_attached_without_stencil_on_d3d12)
{
    auto context = Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL2, drawing_buffer_options);
    if (!context) {
        warnln("No EGL display available, skipping");
        return;
    }
    context->set_size({ 8, 8 });
    context->make_current();
    shut_down_egl_at_exit(eglGetCurrentDisplay());
    auto const* renderer = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
    bool drops_stencil = renderer && StringView { renderer, strlen(renderer) }.contains("D3D12"sv);

    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    auto check_attachment = [&](StringView what) {
        GLint depth_type = GL_NONE;
        GLint stencil_type = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depth_type);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencil_type);
        if (depth_type != GL_TEXTURE || stencil_type != (drops_stencil ? GL_NONE : GL_TEXTURE))
            warnln("{}: depth attachment type {:#x}, stencil attachment type {:#x}", what, depth_type, stencil_type);
        EXPECT_EQ(depth_type, GL_TEXTURE);
        EXPECT_EQ(stencil_type, static_cast<GLint>(drops_stencil ? GL_NONE : GL_TEXTURE));
        auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            warnln("{}: framebuffer status {:#x}", what, status);
        EXPECT_EQ(status, static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        // Detach, so the next case starts from an empty framebuffer.
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    };

    GLuint textures[5] {};
    glGenTextures(5, textures);

    // A mip level other than 0, allocated with texImage2D().
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    // A mutable texture's level other than the base one can only be attached once its mip chain is complete.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    context->tex_image2d_robust_angle(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 8, 8, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, nullptr);
    context->tex_image2d_robust_angle(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, 4, 4, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, nullptr);
    context->framebuffer_texture2d(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, textures[0], 1);
    check_attachment("texImage2D, level 1"sv);

    // A mip level other than 0, allocated with texStorage2D().
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    context->tex_storage2d(GL_TEXTURE_2D, 2, GL_DEPTH24_STENCIL8, 8, 8);
    context->framebuffer_texture2d(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, textures[1], 1);
    check_attachment("texStorage2D, level 1"sv);

    // A layer of a 2D array texture, allocated with texStorage3D().
    glBindTexture(GL_TEXTURE_2D_ARRAY, textures[2]);
    context->tex_storage3d(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH32F_STENCIL8, 8, 8, 3);
    context->framebuffer_texture_layer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, textures[2], 0, 2);
    check_attachment("texStorage3D, layer 2"sv);

    // A layer of a 2D array texture, allocated with texImage3D().
    glBindTexture(GL_TEXTURE_2D_ARRAY, textures[3]);
    context->tex_image3d_robust_angle(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, 8, 8, 2, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, nullptr);
    context->framebuffer_texture_layer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, textures[3], 0, 1);
    check_attachment("texImage3D, layer 1"sv);

    // A face of a cube map.
    glBindTexture(GL_TEXTURE_CUBE_MAP, textures[4]);
    context->tex_storage2d(GL_TEXTURE_CUBE_MAP, 1, GL_DEPTH24_STENCIL8, 8, 8);
    context->framebuffer_texture2d(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, textures[4], 0);
    check_attachment("texStorage2D cube map, face -Y"sv);

    context->delete_textures(5, textures);
    glDeleteFramebuffers(1, &framebuffer);
}


// A page can ask texStorage2D() for any number of mip levels. GL rejects impossible counts, and the Compositor must not
// spend time on them either before it does: with Mesa's d3d12 driver, it tracks each level, see
// OpenGLContext::tex_storage2d().
TEST_CASE(impossible_mip_level_counts_are_rejected_quickly)
{
    auto context = Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL2, drawing_buffer_options);
    if (!context) {
        warnln("No EGL display available, skipping");
        return;
    }
    context->set_size({ 8, 8 });
    context->make_current();
    shut_down_egl_at_exit(eglGetCurrentDisplay());
    while (glGetError() != GL_NO_ERROR) { }

    GLuint textures[2] {};
    glGenTextures(2, textures);
    auto start = MonotonicTime::now();
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    context->tex_storage2d(GL_TEXTURE_2D, 0x7fffffff, GL_DEPTH24_STENCIL8, 8, 8);
    EXPECT_NE(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    glBindTexture(GL_TEXTURE_CUBE_MAP, textures[1]);
    context->tex_storage2d(GL_TEXTURE_CUBE_MAP, 0x7fffffff, GL_RGBA8, 8, 8);
    EXPECT_NE(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    context->tex_image2d_robust_angle(GL_TEXTURE_2D, 0x7fffffff, GL_DEPTH24_STENCIL8, 1, 1, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, nullptr);
    EXPECT_NE(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    EXPECT((MonotonicTime::now() - start) < AK::Duration::from_seconds(1));
    context->delete_textures(2, textures);
}

// Draws a quad at depth 0.5 into a framebuffer with the given depth texture, and returns which of its 8 columns got
// drawn: those whose stored depth is greater than 0.5.
static String columns_in_front_of_stored_depth(Compositor::OpenGLContext& context, GLuint depth_texture)
{
    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    context.framebuffer_texture2d(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

    // With Mesa's d3d12 driver, the texture must have lost its stencil, which is what keeps the device alive.
    auto const* renderer = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
    if (renderer && StringView { renderer, strlen(renderer) }.contains("D3D12"sv)) {
        GLint stencil_type = GL_NONE;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencil_type);
        EXPECT_EQ(stencil_type, GL_NONE);
    }

    auto compile = [](GLenum type, char const* source) {
        auto shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        return shader;
    };
    auto program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    glUseProgram(program);
    GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glViewport(0, 0, 8, 8);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_DEPTH_TEST);

    u8 pixels[8 * 4] {};
    glReadPixels(0, 4, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    StringBuilder columns;
    for (int x = 0; x < 8; ++x)
        columns.append(pixels[x * 4] ? 'x' : '.');

    glDeleteBuffers(1, &buffer);
    glDeleteProgram(program);
    glDeleteRenderbuffers(1, &color);
    glDeleteFramebuffers(1, &framebuffer);
    return MUST(columns.to_string());
}

// Depth-stencil data a page uploads must keep its depth when the texture gets a depth-only format on d3d12, see
// drop_stencil_from_upload(). The left half of each texture is at depth 0.25, the right half at 0.75, so a quad at 0.5
// shows up in the right half only.
TEST_CASE(depth_stencil_data_keeps_its_depth)
{
    auto context = Compositor::OpenGLContext::create(nullptr, Compositor::OpenGLContext::WebGLVersion::WebGL2, drawing_buffer_options);
    if (!context) {
        warnln("No EGL display available, skipping");
        return;
    }
    context->set_size({ 8, 8 });
    context->make_current();
    shut_down_egl_at_exit(eglGetCurrentDisplay());
    while (glGetError() != GL_NO_ERROR) { }

    auto depth24_at = [](int x) { return static_cast<u32>((x < 4 ? 0.25 : 0.75) * 0xffffff) << 8 | 0x5a; };
    auto depth32f_at = [](int x) { return x < 4 ? 0.25f : 0.75f; };

    GLuint textures[3] {};
    glGenTextures(3, textures);

    // UNSIGNED_INT_24_8, with texImage2D().
    Vector<u32> texels24;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x)
            texels24.append(depth24_at(x));
    }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    context->tex_image2d_robust_angle(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 8, 8, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, texels24.size() * 4, texels24.data());
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    EXPECT_EQ(columns_in_front_of_stored_depth(*context, textures[0]), "....xxxx"sv);

    // FLOAT_32_UNSIGNED_INT_24_8_REV, laid out with a longer row length and skipped pixels and rows, which the depths get
    // repacked from.
    constexpr int row_length = 11, skip_pixels = 2, skip_rows = 1;
    Vector<u32> texels32f;
    texels32f.resize((skip_rows + 8) * row_length * 2);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            auto index = ((skip_rows + y) * row_length + skip_pixels + x) * 2;
            float depth = depth32f_at(x);
            memcpy(&texels32f[index], &depth, sizeof(float));
            texels32f[index + 1] = 0x5a;
        }
    }
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
    context->tex_image2d_robust_angle(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, 8, 8, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, texels32f.size() * 4, texels32f.data());
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    GLint row_length_after = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length_after);
    EXPECT_EQ(row_length_after, row_length);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    EXPECT_EQ(columns_in_front_of_stored_depth(*context, textures[1]), "....xxxx"sv);

    // UNSIGNED_INT_24_8, with texSubImage2D() into a texture allocated without data.
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    context->tex_storage2d(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, 8, 8);
    context->tex_sub_image2d_robust_angle(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, texels24.size() * 4, texels24.data());
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
    EXPECT_EQ(columns_in_front_of_stored_depth(*context, textures[2]), "....xxxx"sv);

    context->delete_textures(3, textures);
}
