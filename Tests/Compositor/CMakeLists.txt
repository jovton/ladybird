set(TEST_SOURCES
    TestAsyncScrollTree.cpp
    TestContextState.cpp
    TestScrollbarController.cpp
    TestVSyncScheduler.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    ladybird_test("${source}" Compositor LIBS compositorservice LibCompositing LibGfx)
endforeach()

if (NOT WIN32)
    ladybird_test("TestTransportPlatform.cpp" Compositor LIBS compositorservice LibCompositing LibGfx LibWeb)
endif()

if (USE_VULKAN_DMABUF_IMAGES)
    ladybird_test("TestOpenGLContext.cpp" Compositor LIBS compositorservice LibCompositing LibGfx ${ANGLE_TARGETS})
endif()
