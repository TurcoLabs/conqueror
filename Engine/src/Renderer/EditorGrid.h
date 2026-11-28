#pragma once

#include "Core/Base/Base.h"
#include <glm/glm.hpp>

namespace Conqueror
{
    struct RenderCamera;

    struct EditorGridSettings
    {
        bool Visible = true;
        float BaseSize = 1.0f;      // smallest cell size (can match snapping)
        float Opacity = 1.0f;
        float FadeDistance = 0.0f;  // 0 = automatic based on camera altitude
        // Light colored lines: distinct against the default sky grey ground (~0.56)
        // (the previous 0.55 thin line matched the ground tone and was barely visible)
        glm::vec3 MinorColor = glm::vec3(0.86f);
        glm::vec3 MajorColor = glm::vec3(1.0f);
    };

    // Shader-based infinite grid for editor viewports
    class CQ_API EditorGrid
    {
    public:
        static EditorGridSettings& Settings();
        // plane2D: XY plane (2D view); otherwise XZ ground plane.
        // background: grid rendered in background (right before the sky); in 2D views sprites and
        // models always appear in front of the grid
        static void Render(const RenderCamera& camera, bool plane2D, bool linearOutput = true, bool background = false);
        // Viewports with temporal anti-aliasing (TAA, Temporal Stability, temporal upscaler): 3D grid is not drawn directly into the scene,
        // but deferred to render atop the final image after anti-aliasing and tonemapping (similar to Unity Scene grid / Unreal editor primitives).
        // Called when rendering to scene target. Returns false if no temporal method is active; grid should be drawn directly.
        static bool DeferToComposite(const RenderCamera& camera);
        // Renders the deferred grid onto the final image (colorTexture, sRGB); no-op if nothing was deferred. sceneDepth: viewport's
        // (multisample-resolved) scene depth (PostProcessStack::GetSceneDepthTexture)
        // objectVelocity: PostProcessStack::GetObjectVelocityTexture (0: no moving / deleted objects this frame)
        // view: the viewport's PostProcessStack (its recorded opaque draws give the exact, unjittered output-resolution depth the grid is
        // occluded by, 355; Renderer3D::RenderGeometryDepth). Returns true if the grid was drawn
        static bool RenderDeferred(uint32_t colorTexture, uint32_t width, uint32_t height, uint32_t sceneDepth, uint32_t objectVelocity = 0,
                                   const void* view = nullptr);
        static void Shutdown();
        // Automation (355): the last composite's image before the grid was blended in (RGBA8) and its size; 0 if none yet
        static uint32_t GetLastBelowTexture(uint32_t& width, uint32_t& height);
        // Automation (355): the last composite's exact depth range (RG32F nearest / farthest eye depth per pixel, ExactDepth); 0 if none
        static uint32_t GetLastExactRange();
    };
}
