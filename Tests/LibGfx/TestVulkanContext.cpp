/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibGfx/VulkanContext.h>
#include <LibTest/TestCase.h>

TEST_CASE(cpu_vulkan_devices_are_only_used_with_dmabuf_support)
{
    // Mesa's lavapipe implements Vulkan on the CPU. Painting with it and reading every frame back would be slower than
    // Skia's own CPU rasterizer, so it may only be used when it can share images as DMA-BUFs.
    static constexpr auto icd_directory = "/usr/share/vulkan/icd.d"sv;
    StringBuilder lavapipe_icd_files;
    auto result = Core::Directory::for_each_entry(icd_directory, Core::DirIterator::SkipDots, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        if (entry.name.starts_with("lvp_icd"sv) && entry.name.ends_with(".json"sv)) {
            if (!lavapipe_icd_files.is_empty())
                lavapipe_icd_files.append(':');
            lavapipe_icd_files.appendff("{}/{}", icd_directory, entry.name);
        }
        return IterationDecision::Continue;
    });
    if (result.is_error() || lavapipe_icd_files.is_empty()) {
        warnln("Mesa's lavapipe is not installed, skipping");
        return;
    }

    // Make lavapipe the only Vulkan driver the loader knows about.
    auto icd_files = lavapipe_icd_files.to_byte_string();
    MUST(Core::Environment::set("VK_DRIVER_FILES"sv, icd_files, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("VK_ICD_FILENAMES"sv, icd_files, Core::Environment::Overwrite::Yes));

    auto context = Gfx::create_vulkan_context();
    if (context.is_error())
        return;

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context.value().physical_device, &properties);
    EXPECT_EQ(properties.deviceType, VK_PHYSICAL_DEVICE_TYPE_CPU);
    EXPECT(context.value().supports_dmabuf_images);
}
