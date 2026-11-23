#pragma once

#include "Core/Base/Base.h"

#include <array>
#include <cstdint>
#include <string>

// GL ön yüzünün (GLFrontEnd.inl) arka uçtan istediği cihaz sınırları
namespace Conqueror::GLEmu
{
    // 323: çeviri katmanlarında çağrılan uygulanmamış GL işlevleri (her ad bir kez sayılır; testler 0 bekler)
    CQ_API void NoteUnimplementedCall(const char* name);
    CQ_API int UnimplementedCallCount();
    CQ_API std::string UnimplementedCallNames();

    // 316: donanım ışın sorgusu (Vulkan VK_KHR_ray_query + VK_KHR_acceleration_structure; ileride DXR / Metal RT aynı arayüzle).
    // Destekleyen çeviri katmanı başlangıçta kaydeder; motorun ışın izleyicisi (RayTracingScene) BLAS / TLAS'ı burada kurar ve
    // compute gölgelendiricileri GL_EXT_ray_query ile "u_TLAS" adlı hızlandırma yapısını izler (yazılım BVH'si yerine)
    struct RayQueryInstance
    {
        float Transform[12];          // dünya matrisinin ilk üç satırı (3 x 4, satır sıralı)
        uint32_t CustomIndex = 0;     // gölgelendiricide rayQueryGetIntersectionInstanceCustomIndexEXT (24 bit)
        uint32_t Blas = 0;            // CreateBLAS kimliği
        bool Opaque = true;           // false: aday kesişimler gölgelendiricide (Cutout / saydam kapsaması) onaylanır
    };

    class RayQueryBackend
    {
    public:
        virtual ~RayQueryBackend() = default;
        virtual const char* Name() const = 0;
        // positions: triCount x 3 köşe (xyz, sıkışık); üçgen sırası gölgelendiricideki ilkel dizinidir. 0: kurulamadı
        virtual uint32_t CreateBLAS(const float* positions, uint32_t triCount) = 0;
        virtual void DestroyBLAS(uint32_t id) = 0;
        // Örnekler değişince yeniden kurulur (komut akışına sırayla kaydedilir: önceki dispatch'ler eski yapıyı görür)
        virtual bool BuildTLAS(const RayQueryInstance* instances, uint32_t count) = 0;
    };
    CQ_API void SetRayQueryBackend(RayQueryBackend* backend);
    CQ_API RayQueryBackend* GetRayQueryBackend();
    struct Limits
    {
        int32_t MaxTextureSize = 16384;
        int32_t MaxCubeMapSize = 16384;
        int32_t MaxTextureUnits = 32;
        int32_t MaxVertexAttribs = 16;
        int32_t MaxUniformBufferBindings = 64;
        int32_t MaxUniformBlockSize = 65536;
        int32_t UniformBufferAlignment = 256;
        int32_t MaxColorAttachments = 8;
        int32_t MaxViewport[2] = { 16384, 16384 };
        float MaxAnisotropy = 1.0f;
        float LineWidthRange[2] = { 1.0f, 1.0f };
        int32_t VideoMemoryKB = 0;
        bool Compute = false;            // hesaplama gölgelendiricisi (glDispatchCompute, görüntü / depolama arabelleği) destekleniyor
    };

    // ── Hesaplama gölgelendiricisi (307): ön yüzün tuttuğu bağlamalar, arka uçlar dispatch'te okur ──
    // glBindImageTexture birimi
    struct ImageUnit
    {
        uint32_t Texture = 0;
        int32_t Level = 0;
        bool Layered = false;
        int32_t Layer = 0;
        uint32_t Access = 0x88B8;        // GL_READ_ONLY
        uint32_t Format = 0x8058;        // GL_RGBA8
    };

    struct ComputeBindings
    {
        std::array<ImageUnit, 8> Images{};
        std::array<uint32_t, 16> Storage{};          // GL_SHADER_STORAGE_BUFFER bağlama noktaları (glBindBufferBase / Range)
        std::array<int64_t, 16> StorageOffsets{};
        std::array<int64_t, 16> StorageSizes{};      // 0: arabelleğin sonuna kadar
        uint32_t StorageBuffer = 0;                  // glBindBuffer(GL_SHADER_STORAGE_BUFFER)
        uint32_t DispatchIndirect = 0;               // glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER)
        uint32_t DrawIndirect = 0;                   // glBindBuffer(GL_DRAW_INDIRECT_BUFFER)
        uint32_t Parameter = 0;                      // glBindBuffer(GL_PARAMETER_BUFFER): dolaylı çizim sayısı
        uint32_t CopyRead = 0, CopyWrite = 0;        // GL_COPY_READ_BUFFER / GL_COPY_WRITE_BUFFER
    };

    // Programdaki görüntü uniform'u (image2D / uimage2D): birimi glUniform1i ya da kaynaktaki layout(binding)
    struct ImageBinding
    {
        std::string Name;
        uint32_t Binding = 0;            // arka ucun bağlama noktası / yazmacı
        int Unit = 0;                    // GL görüntü birimi
        bool Integer = false;
        bool Writable = true;
        bool SRV = false;                // D3D / Metal: salt okunur görüntü kaynak görünümü (t#) olarak bağlanır, yazılabilir UAV (u#)
    };

    // Programdaki depolama bloğu (std430 buffer)
    struct StorageBlockBinding
    {
        std::string Name;
        uint32_t Binding = 0;            // arka ucun bağlama noktası / yazmacı
        uint32_t GLBinding = 0;          // layout(binding = N)
        bool Writable = true;
        bool SRV = false;                // D3D / Metal: salt okunur blok ham kaynak görünümü (ByteAddressBuffer, t#)
    };
}
