#pragma once

#include "Core/Base/Base.h"

#include <cstdint>
#include <string>

// Kamera -> doku (Godot SubViewport + ViewportTexture / Unity Camera.targetTexture): bu bileşenli kamera ekrana
// değil "rt://Ad" dokusuna çizer; doku her doku alanında kullanılabilir (güvenlik kamerası, mini harita, ayna, portal).
// Sahne HDR çizilir, ACES ton eşleme + pozlama ile sRGB dokuya çözülür. Kare başına sahne başına bir kez güncellenir.
namespace Conqueror
{
    class Scene;

    enum class RenderTargetUpdate : int { Always = 0, Once, Disabled };

    struct RenderTargetComponent
    {
        std::string Asset;                    // .cqrt Render Texture (doluysa ad ve boyut ondan gelir)
        std::string Name = "CameraView";
        int Width = 512;
        int Height = 512;
        RenderTargetUpdate UpdateMode = RenderTargetUpdate::Always;
        float Exposure = 0.0f;                // EV (sahnenin pozlamasına eklenir)
        // 313: Unity RenderTexture.antiAliasing / useMipMap + autoGenerateMips. Kenar yumuşatma: 0 yok, 1 2x, 2 4x, 3 8x MSAA
        // (sahne çok örnekli hedefe çizilir, sonra çözülür); mipmap: her çizimden sonra üretilir, küçültülen doku (küçük önizleme,
        // uzaktaki monitör) basamaklı / karıncalı görünmez
        int AntiAliasing = 0;
        bool GenerateMipmaps = false;

        // Çalışma zamanı
        bool RenderedOnce = false;
        int Renders = 0;                      // bilgi
    };

    namespace CameraRenderTargets
    {
        // Ana görünümden önce çağrılır (sahne başına kare başına bir kez)
        CQ_API void Render(Scene& scene);
        // GPU kaynakları (Renderer kapanmadan önce)
        CQ_API void Shutdown();
    }

    void RegisterRenderTargetComponents();
}
