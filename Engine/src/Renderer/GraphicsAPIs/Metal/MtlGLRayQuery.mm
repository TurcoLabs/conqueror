#ifdef CQ_METAL_ENABLED

#include "MtlGL.h"
#include "Renderer/GraphicsAPIs/GLEmu/GLEmuCommon.h"
#include "Core/Logging/Log.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

// 316: Metal ışın izleme (MTLAccelerationStructure + MSL raytracing::intersection_query). VkGLRayQuery / D12GLRayQuery'nin karşılığı:
// motorun ışın izleyicisi (RayTracingScene) BLAS'ları mesh başına bir kez, TLAS'ı örnekler değişince kurar; kurulum komutları katmanın
// kare komut arabelleğinde bir hızlandırma yapısı kodlayıcısıyla sırayla kodlanır (Metal kaynak izlemesi okuma / yazma sırasını korur).
// Hesaplama gölgelendiricileri GL_EXT_ray_query ile "u_TLAS"ı izler; çeviri katmanı onu MSL 2.4 intersection_query'ye çevirir
// (SPIRV-Cross), TLAS dispatch'te arabellek dizinine bağlanır, örneklenen BLAS'lar useResource ile kodlayıcıya bildirilir.
// Özel dizin (rayQueryGetIntersectionInstanceCustomIndexEXT) MSL'de get_*_user_instance_id: kullanıcı kimlikli örnek tanımlayıcısı
namespace Conqueror::MtlGL
{
    using MTL::Core;

    namespace
    {
        class API_AVAILABLE(macos(12.0), ios(15.0), tvos(16.0)) RayQuery final : public GLEmu::RayQueryBackend
        {
        public:
            const char* Name() const override { return "Metal ray tracing (intersection queries)"; }

            uint32_t CreateBLAS(const float* positions, uint32_t triCount) override
            {
                if (!positions || triCount == 0)
                    return 0;
                id<MTLDevice> dev = Core::Get().Device();
                id<MTLBuffer> vertices = [dev newBufferWithBytes:positions length:(NSUInteger)triCount * 9 * sizeof(float) options:MTLResourceStorageModeShared];
                if (!vertices)
                    return 0;
                MTLAccelerationStructureTriangleGeometryDescriptor* geom = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                geom.vertexBuffer = vertices;
                geom.vertexBufferOffset = 0;
                geom.vertexStride = 3 * sizeof(float);
                geom.triangleCount = triCount;
                geom.opaque = YES;   // opak olmayan örnek MTLAccelerationStructureInstanceOptionNonOpaque ile (Cutout / saydam)
                MTLPrimitiveAccelerationStructureDescriptor* desc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
                desc.geometryDescriptors = @[ geom ];
                desc.usage = MTLAccelerationStructureUsageNone;
                id<MTLAccelerationStructure> as = Build(desc);
                if (!as)
                    return 0;
                const uint32_t id = m_NextId++;
                m_Blas[id] = as;
                return id;
            }

            void DestroyBLAS(uint32_t id) override
            {
                // Komut arabelleği kullandığı yapıyı tutar (retainedReferences): burada yalnız katmanın başvurusu bırakılır
                m_Blas.erase(id);
            }

            bool BuildTLAS(const GLEmu::RayQueryInstance* instances, uint32_t count) override
            {
                std::vector<MTLAccelerationStructureUserIDInstanceDescriptor> descs;
                descs.reserve(count);
                NSMutableArray<id<MTLAccelerationStructure>>* used = [NSMutableArray array];
                std::unordered_map<uint32_t, uint32_t> slotOf;   // BLAS kimliği -> instancedAccelerationStructures dizini
                for (uint32_t i = 0; i < count; ++i)
                {
                    const GLEmu::RayQueryInstance& in = instances[i];
                    auto it = m_Blas.find(in.Blas);
                    if (it == m_Blas.end())
                        continue;
                    auto slot = slotOf.find(in.Blas);
                    if (slot == slotOf.end())
                    {
                        slot = slotOf.emplace(in.Blas, (uint32_t)used.count).first;
                        [used addObject:it->second];
                    }
                    MTLAccelerationStructureUserIDInstanceDescriptor d{};
                    // Transform satır sıralı 3 x 4, MTLPackedFloat4x3 dört sütun (her biri x, y, z)
                    for (int col = 0; col < 4; ++col)
                        for (int row = 0; row < 3; ++row)
                            d.transformationMatrix.columns[col].elements[row] = in.Transform[row * 4 + col];
                    d.options = MTLAccelerationStructureInstanceOptionDisableTriangleCulling |
                                (in.Opaque ? MTLAccelerationStructureInstanceOptionOpaque : MTLAccelerationStructureInstanceOptionNonOpaque);
                    d.mask = 0xFF;
                    d.intersectionFunctionTableOffset = 0;
                    d.accelerationStructureIndex = slot->second;
                    d.userID = in.CustomIndex & 0xFFFFFFu;
                    descs.push_back(d);
                }
                id<MTLDevice> dev = Core::Get().Device();
                const NSUInteger bytes = (NSUInteger)std::max<size_t>(descs.size(), 1) * sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
                id<MTLBuffer> instanceBuffer = descs.empty() ? [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]
                                                             : [dev newBufferWithBytes:descs.data() length:bytes options:MTLResourceStorageModeShared];
                if (!instanceBuffer)
                    return false;
                MTLInstanceAccelerationStructureDescriptor* desc = [MTLInstanceAccelerationStructureDescriptor descriptor];
                desc.instancedAccelerationStructures = used;
                desc.instanceCount = descs.size();
                desc.instanceDescriptorBuffer = instanceBuffer;
                desc.instanceDescriptorBufferOffset = 0;
                desc.instanceDescriptorStride = sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
                desc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
                // Her kurulumda yeni yapı: önceki dispatch'ler (aynı komut arabelleğinde) eskisini okur, komut arabelleği onu tutar
                id<MTLAccelerationStructure> tlas = Build(desc);
                if (!tlas)
                    return false;
                m_Tlas = tlas;
                m_Used = used;
                return true;
            }

            id<MTLAccelerationStructure> Tlas() const { return m_Tlas; }
            NSArray<id<MTLAccelerationStructure>>* Used() const { return m_Used; }

            void Shutdown()
            {
                m_Blas.clear();
                m_Tlas = nil;
                m_Used = nil;
            }

        private:
            id<MTLAccelerationStructure> Build(MTLAccelerationStructureDescriptor* desc)
            {
                id<MTLDevice> dev = Core::Get().Device();
                const MTLAccelerationStructureSizes sizes = [dev accelerationStructureSizesWithDescriptor:desc];
                if (sizes.accelerationStructureSize == 0)
                    return nil;
                id<MTLAccelerationStructure> as = [dev newAccelerationStructureWithSize:sizes.accelerationStructureSize];
                id<MTLBuffer> scratch = [dev newBufferWithLength:std::max<NSUInteger>(sizes.buildScratchBufferSize, 256) options:MTLResourceStorageModePrivate];
                if (!as || !scratch)
                    return nil;
                // Katmanın açık kodlayıcısı kapatılır (aynı anda tek kodlayıcı), kurulum kendi kodlayıcısında
                Core::Get().EndEncoding();
                id<MTLAccelerationStructureCommandEncoder> enc = [Core::Get().CommandBuffer() accelerationStructureCommandEncoder];
                if (!enc)
                    return nil;
                [enc buildAccelerationStructure:as descriptor:desc scratchBuffer:scratch scratchBufferOffset:0];
                [enc endEncoding];
                return as;
            }

            std::unordered_map<uint32_t, id<MTLAccelerationStructure>> m_Blas;
            uint32_t m_NextId = 1;
            id<MTLAccelerationStructure> m_Tlas = nil;
            NSArray<id<MTLAccelerationStructure>>* m_Used = nil;
        };

        GLEmu::RayQueryBackend* g_RayQuery = nullptr;
    }

    void InstallRayQuery()
    {
        if (g_RayQuery)
            return;
        if (@available(macOS 12.0, iOS 15.0, tvOS 16.0, *))
        {
            if (![Core::Get().Device() supportsRaytracing])
            {
                CQ_CORE_INFO("[MtlGL] Hardware ray queries not supported by this GPU: ray tracing uses the compute BVH");
                return;
            }
            g_RayQuery = new RayQuery();
            GLEmu::SetRayQueryBackend(g_RayQuery);
            CQ_CORE_INFO("[MtlGL] Hardware ray queries available (Metal ray tracing): ray traced effects and Path Tracing use acceleration structures");
            return;
        }
        CQ_CORE_INFO("[MtlGL] Hardware ray queries need macOS 12 / iOS 15 / tvOS 16: ray tracing uses the compute BVH");
    }

    void ShutdownRayQuery()
    {
        if (!g_RayQuery)
            return;
        GLEmu::SetRayQueryBackend(nullptr);
        if (@available(macOS 12.0, iOS 15.0, tvOS 16.0, *))
            static_cast<RayQuery*>(g_RayQuery)->Shutdown();
        delete g_RayQuery;
        g_RayQuery = nullptr;
    }

    bool BindRayQueryTLAS(id<MTLComputeCommandEncoder> enc, uint32_t bufferIndex)
    {
        if (!g_RayQuery)
            return false;
        if (@available(macOS 12.0, iOS 15.0, tvOS 16.0, *))
        {
            auto* rq = static_cast<RayQuery*>(g_RayQuery);
            if (!rq->Tlas())
                return false;
            [enc setAccelerationStructure:rq->Tlas() atBufferIndex:bufferIndex];
            // Örnek yapısının başvurduğu BLAS'lar kodlayıcıda dolaylı kullanılır: Metal'e bildirilir (yoksa GPU onları göremeyebilir)
            for (id<MTLAccelerationStructure> blas in rq->Used())
                [enc useResource:blas usage:MTLResourceUsageRead];
            return true;
        }
        return false;
    }
}

#endif
