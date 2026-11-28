#pragma once

#include "Core/Base/Base.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace Conqueror
{
    // 355 / 334: exact, unjittered depth of a view at output resolution. The view's recorded opaque draws (Renderer3D::RenderGeometryDepth)
    // are rendered again at Factor() x the output resolution per axis (Factor()^2 samples per output pixel at known positions: sample s
    // of pixel p is texel p * Factor() + (s % Factor(), s / Factor()) of DepthTexture(), centred at (sub + 0.5) / Factor() inside the pixel).
    // RangeTexture() (RG32F) holds, per output pixel, the nearest and farthest view distance (eye depth) of its samples (1e30: sky /
    // nothing), ShareTexture() (R32F) the share of the samples on the nearest surface. WideTexture() (RG32F, only when Render was asked
    // for it) holds the nearest / farthest of RangeTexture() over the (2 r + 1) x (2 r + 1) output pixels around each pixel.
    // The editor grid finds how much of a pixel the scene hides from it; TAAU tells a thin surface its internal samples missed this frame
    // (a sky sliver under a bridge, the gap between two cubes) from a surface that is really gone.
    // A multisample depth was tried first: the sample positions the driver reported (glGetMultisamplefv) did not match the order of the
    // samples in the texture (radeonsi), and are not reliable on other drivers either.
    class CQ_API ExactDepth
    {
    public:
        ExactDepth() = default;
        ~ExactDepth();
        ExactDepth(const ExactDepth&) = delete;
        ExactDepth& operator=(const ExactDepth&) = delete;

        // Renders the view's recorded draws with viewProj (unjittered) and resolves the per-pixel range; projection gives the eye-depth
        // conversion. Returns false (and the textures keep no valid data) if the view recorded nothing, the targets cannot be created
        // (size limit) or the view is in wireframe. Saves and restores the GL state it touches. neighbourhoodRadius r > 0: also fill
        // WideTexture() (r <= 5)
        bool Render(const void* view, const glm::mat4& viewProj, const glm::mat4& projection, uint32_t width, uint32_t height,
                    int neighbourhoodRadius = 0);
        void Release();

        uint32_t DepthTexture() const { return m_Depth; }
        uint32_t RangeTexture() const { return m_Range; }
        uint32_t ShareTexture() const { return m_Share; }
        uint32_t WideTexture() const { return m_Wide; }
        uint32_t Width() const { return m_Width; }
        uint32_t Height() const { return m_Height; }
        static constexpr int Factor() { return kFactor; }
        // Shared GL objects (the range resolve program); Renderer::Shutdown calls this while the context is still current
        static void ShutdownShared();
        // (2,2) and (3,2) elements of the projection: eye depth = y / (z_ndc + x)
        static glm::vec2 ProjZ(const glm::mat4& projection) { return glm::vec2(projection[2][2], projection[3][2]); }

    private:
        bool EnsureTargets(uint32_t width, uint32_t height);

        static constexpr int kFactor = 3;
        uint32_t m_Depth = 0, m_DepthFramebuffer = 0;
        uint32_t m_Range = 0, m_RangeFramebuffer = 0, m_Share = 0;
        uint32_t m_Wide = 0, m_WideFramebuffer = 0;
        uint32_t m_WideTmp = 0, m_WideTmpFramebuffer = 0;   // yatay geçişin ara sonucu
        uint32_t m_Width = 0, m_Height = 0;
        bool m_Failed = false;
    };
}
