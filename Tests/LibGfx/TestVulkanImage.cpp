/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/VulkanImage.h>
#include <LibTest/TestCase.h>

TEST_CASE(shared_images_are_refused_without_dmabuf_support)
{
    // A device without the DMA-BUF extensions has no functions to export images with, so this must fail rather than
    // call through them.
    Gfx::VulkanContext context;
    EXPECT(!context.supports_dmabuf_images);
    EXPECT(!context.ext_procs.get_memory_fd);

    auto image = Gfx::create_shared_vulkan_image(context, 16, 16, VK_FORMAT_B8G8R8A8_UNORM, {});
    EXPECT(image.is_error());
}
