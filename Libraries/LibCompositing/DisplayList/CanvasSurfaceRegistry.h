/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGfx/PaintingSurface.h>

namespace Compositing {

class CanvasSurfaceRegistry {
    AK_MAKE_NONCOPYABLE(CanvasSurfaceRegistry);
    AK_MAKE_DEFAULT_MOVABLE(CanvasSurfaceRegistry);

public:
    CanvasSurfaceRegistry() = default;
    ~CanvasSurfaceRegistry() = default;

    CanvasId allocate_canvas_id()
    {
        return CanvasId { m_next_canvas_id++ };
    }

    CanvasId create_canvas_surface(NonnullRefPtr<Gfx::PaintingSurface> surface)
    {
        auto id = allocate_canvas_id();
        set_canvas_surface(id, move(surface));
        return id;
    }

    void set_canvas_surface(CanvasId id, NonnullRefPtr<Gfx::PaintingSurface> surface)
    {
        m_surfaces.set(id, move(surface));
        m_content_generations.set(id, ++m_last_content_generation);
    }

    void remove_canvas_surface(CanvasId id)
    {
        m_surfaces.remove(id);
        m_content_generations.remove(id);
        m_pending_content_resolvers.remove(id);
    }

    // A surface can get its content only once it's needed, such as a WebGL drawing buffer that is still being read
    // back from the GPU. The resolver then runs once, before anything looks up the surface.
    void set_pending_content_resolver(CanvasId id, Function<void()> resolver)
    {
        m_pending_content_resolvers.set(id, move(resolver));
    }

    Gfx::PaintingSurface const* canvas_surface(CanvasId id) const
    {
        if (auto resolver = m_pending_content_resolvers.take(id); resolver.has_value())
            (*resolver)();
        return m_surfaces.get(id).value_or(nullptr);
    }

    u64 canvas_content_generation(CanvasId id) const
    {
        return m_content_generations.get(id).value_or(0);
    }

private:
    u64 m_next_canvas_id { 1 };
    u64 m_last_content_generation { 0 };
    HashMap<CanvasId, NonnullRefPtr<Gfx::PaintingSurface>> m_surfaces;
    HashMap<CanvasId, u64> m_content_generations;
    mutable HashMap<CanvasId, Function<void()>> m_pending_content_resolvers;
};

}
