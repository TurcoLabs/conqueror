#pragma once

#include "Core/Base/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

// Işın izlemeli yansıma ve ortam örtmesi (316; Unity HDRP Ray-Traced Reflections / Ray-Traced Ambient Occlusion, Unreal'ın ışın
// izlemeli yansıma ve AO'su). Path Tracing ve Ray Traced GI'nin sahne BVH'si (Renderer/RayTracing/RayTracingScene) compute
// shader ile izlenir: ekran dışındaki ve görünmeyen nesneler de yansır, örtü ekranın dışındaki geometriden de gelir. Sahne o karede
// RayTracingScene::Update ile güncellenmiş olmalı (Scene.cpp, ayarlar açıkken). Çıktılar son işlemenin ekran uzayı efektleriyle
// aynı biçimde: yansıma SSR'nin birikim / birleştirme geçişlerine, AO SSAO'nun bulanıklaştırma geçişine girer.
namespace Conqueror
{
    namespace RayTracedEffects
    {
        // Compute shader ve sahne BVH'si kullanılabilir mi (GL 4.3+ ya da compute destekli çeviri katmanı)
        CQ_API bool Supported();

        struct ReflectionInputs
        {
            uint32_t Scene = 0;        // HDR sahne rengi (ekranda görünen isabetler buradan okunur)
            uint32_t Depth = 0;        // sahne derinliği (tam çözünürlük)
            uint32_t Normal = 0;       // xy oktahedral dünya normali, z pürüzlülük, w 2 + metalik (SSR eki)
            glm::mat4 ViewProj{ 1.0f };   // sahnenin çizildiği (titreşimli) izdüşüm x görüş
            glm::vec3 CameraPos{ 0.0f };
            float MaxDistance = 40.0f;
            float MaxRoughness = 0.6f;
            uint32_t Frame = 0;
        };
        // color: RGBA16F (ön çarpımlı yansıma rengi x güven, a güven), hit: R16F (yüzeyden yansıyan noktaya uzaklık, 0 = isabet yok).
        // Boyut hedeften okunur. false: desteklenmiyor / sahne boş (çağıran ekran uzayı izine döner)
        CQ_API bool TraceReflections(const ReflectionInputs& in, uint32_t color, uint32_t hit);

        struct AOInputs
        {
            uint32_t Depth = 0;
            glm::mat4 ViewProj{ 1.0f };
            glm::vec3 CameraPos{ 0.0f };
            float Radius = 0.6f;
            float Intensity = 1.0f;
            float Power = 1.5f;
            int Rays = 4;
            uint32_t Frame = 0;
        };
        // ao: R8 (1 = örtü yok), SSAO ile aynı anlam (Intensity / Power aynı uygulanır)
        CQ_API bool TraceAO(const AOInputs& in, uint32_t ao);

        // Son karede izlenen ışın sayısı (yansıma, AO; inceleme / test)
        CQ_API void GetLastCounts(uint32_t& reflectionPixels, uint32_t& aoPixels);
    }
}
