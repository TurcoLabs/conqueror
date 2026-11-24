#ifdef CQ_D3D12_ENABLED

#include "D12GL.h"
#include "Renderer/GraphicsAPIs/DirectX/Common/DxShared.h"
#include "Renderer/GraphicsAPIs/GLEmu/GLEmuCommon.h"
#include "Core/Logging/Log.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

// 316: DXR 1.1 satır içi ışın sorgusu (Direct3D 12, Raytracing Tier 1.1 + Shader Model 6.5). VkGLRayQuery'nin karşılığı: motorun
// ışın izleyicisi (RayTracingScene) BLAS'ları mesh başına bir kez, TLAS'ı örnekler değişince kurar; kurulum komutları D3D12 katmanının
// kare komut listesine sırayla kaydedilir (yapının üstünde UAV bariyeriyle): aynı karede önce kaydedilen dispatch'ler eski yapıyı,
// sonrakiler yenisini görür. Hesaplama gölgelendiricileri GL_EXT_ray_query ile "u_TLAS"ı izler; çeviri katmanı onu HLSL RayQuery'ye
// (SPIRV-Cross, SM 6.5) ve DXC ile DXIL'e çevirir, TLAS dispatch'te SRV tablosuna (RAYTRACING_ACCELERATION_STRUCTURE görünümü) girer
namespace Conqueror::D12GL
{
    using DX12::Core;

    namespace
    {
        ID3D12Resource* CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav)
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = heap;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = std::max<uint64_t>(size, 256);
            rd.Height = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_UNKNOWN;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource* r = nullptr;
            if (FAILED(Core::Get().Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, __uuidof(ID3D12Resource), (void**)&r)))
                return nullptr;
            return r;
        }

        ID3D12Resource* UploadBuffer(const void* data, uint64_t size)
        {
            ID3D12Resource* r = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
            if (!r)
                return nullptr;
            void* mapped = nullptr;
            D3D12_RANGE none{ 0, 0 };
            if (FAILED(r->Map(0, &none, &mapped)) || !mapped)
            {
                r->Release();
                return nullptr;
            }
            if (data && size)
                std::memcpy(mapped, data, (size_t)size);
            r->Unmap(0, nullptr);
            return r;
        }

        void UAVBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = r;
            list->ResourceBarrier(1, &b);
        }

        class RayQuery final : public GLEmu::RayQueryBackend
        {
        public:
            explicit RayQuery(ID3D12Device5* device) : m_Device(device) {}

            const char* Name() const override { return "Direct3D 12 DXR 1.1 (inline ray query)"; }

            uint32_t CreateBLAS(const float* positions, uint32_t triCount) override
            {
                if (!positions || triCount == 0)
                    return 0;
                const uint64_t bytes = (uint64_t)triCount * 9 * sizeof(float);
                // Köşeler: ana bilgisayardan görünür arabellek; kurulum komutu çalıştıktan sonra (kare sonu) bırakılır
                ID3D12Resource* vertices = UploadBuffer(positions, bytes);
                if (!vertices)
                    return 0;
                D3D12_RAYTRACING_GEOMETRY_DESC geom{};
                geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;   // opak olmayan örnek FORCE_NON_OPAQUE ile (Cutout / saydam)
                geom.Triangles.VertexBuffer.StartAddress = vertices->GetGPUVirtualAddress();
                geom.Triangles.VertexBuffer.StrideInBytes = 3 * sizeof(float);
                geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geom.Triangles.VertexCount = triCount * 3;
                geom.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
                in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                in.NumDescs = 1;
                in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                in.pGeometryDescs = &geom;
                ID3D12Resource* storage = nullptr;
                const bool ok = Build(in, storage, false);
                Core::Get().DeferRelease(vertices);
                if (!ok)
                    return 0;
                const uint32_t id = m_NextId++;
                m_Blas[id] = storage;
                return id;
            }

            void DestroyBLAS(uint32_t id) override
            {
                auto it = m_Blas.find(id);
                if (it == m_Blas.end())
                    return;
                Core::Get().DeferRelease(it->second);
                m_Blas.erase(it);
            }

            bool BuildTLAS(const GLEmu::RayQueryInstance* instances, uint32_t count) override
            {
                std::vector<D3D12_RAYTRACING_INSTANCE_DESC> data;
                data.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const GLEmu::RayQueryInstance& src = instances[i];
                    auto it = m_Blas.find(src.Blas);
                    if (it == m_Blas.end())
                        continue;
                    D3D12_RAYTRACING_INSTANCE_DESC d{};
                    std::memcpy(d.Transform, src.Transform, sizeof(d.Transform));   // ikisi de satır sıralı 3 x 4
                    d.InstanceID = src.CustomIndex & 0xFFFFFFu;
                    d.InstanceMask = 0xFF;
                    d.InstanceContributionToHitGroupIndex = 0;
                    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | (src.Opaque ? 0u : (UINT)D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE);
                    d.AccelerationStructure = it->second->GetGPUVirtualAddress();
                    data.push_back(d);
                }
                // Örnek verisi: her kurulumda yeni arabellek (aynı karede ikinci kurulum ilkinin okuyacağını ezmesin), kare sonunda bırakılır
                ID3D12Resource* inst = UploadBuffer(data.empty() ? nullptr : data.data(), std::max<size_t>(data.size(), 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
                if (!inst)
                    return false;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
                in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                in.NumDescs = (UINT)data.size();
                in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                in.InstanceDescs = inst->GetGPUVirtualAddress();
                // Yer yetiyorsa aynı yapı yeniden kurulur (önce kaydedilen dispatch'ler bariyerden önce eskisini okur); büyürse yenisi,
                // eskisi kare sonunda bırakılır
                ID3D12Resource* before = m_Tlas;
                const bool ok = Build(in, m_Tlas, true);
                Core::Get().DeferRelease(inst);
                if (!ok)
                    return false;
                if (m_Tlas != before || !m_TlasSRV.Valid())
                {
                    if (!m_TlasSRV.Valid())
                        m_TlasSRV = Core::Get().Pool(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).Allocate();
                    if (!m_TlasSRV.Valid())
                        return false;
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                    sv.Format = DXGI_FORMAT_UNKNOWN;
                    sv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.RaytracingAccelerationStructure.Location = m_Tlas->GetGPUVirtualAddress();
                    Core::Get().Device()->CreateShaderResourceView(nullptr, &sv, m_TlasSRV.Handle);
                }
                return true;
            }

            const DX12::CpuDescriptor* TlasSRV() const { return (m_Tlas && m_TlasSRV.Valid()) ? &m_TlasSRV : nullptr; }

            void Shutdown()
            {
                for (auto& [id, r] : m_Blas)
                    if (r) r->Release();
                m_Blas.clear();
                if (m_Tlas) m_Tlas->Release();
                if (m_Scratch) m_Scratch->Release();
                m_Tlas = m_Scratch = nullptr;
                m_ScratchSize = m_TlasSize = 0;
                if (m_TlasSRV.Valid())
                    Core::Get().Pool(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV).Free(m_TlasSRV);
                if (m_Device) m_Device->Release();
                m_Device = nullptr;
            }

        private:
            // Yapıyı (yoksa ya da yer yetmiyorsa yenisini oluşturarak) kurar; komut kare komut listesine kaydedilir
            bool Build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in, ID3D12Resource*& storage, bool tlas)
            {
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
                m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pre);
                if (pre.ResultDataMaxSizeInBytes == 0)
                    return false;
                const uint64_t have = tlas ? m_TlasSize : (storage ? storage->GetDesc().Width : 0);
                if (!storage || have < pre.ResultDataMaxSizeInBytes)
                {
                    if (storage)
                        Core::Get().DeferRelease(storage);
                    // TLAS: büyüdükçe yeniden oluşturmasın diye pay
                    const uint64_t want = tlas ? pre.ResultDataMaxSizeInBytes * 3 / 2 : pre.ResultDataMaxSizeInBytes;
                    storage = CreateBuffer(want, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
                    if (!storage)
                        return false;
                    if (tlas)
                        m_TlasSize = want;
                }
                // Karalama alanı: tek arabellek (kurulumlar arasında UAV bariyeri), büyürse eskisi kare sonunda bırakılır
                const uint64_t scratchNeed = std::max(pre.ScratchDataSizeInBytes, pre.UpdateScratchDataSizeInBytes);
                if (m_ScratchSize < scratchNeed)
                {
                    if (m_Scratch)
                        Core::Get().DeferRelease(m_Scratch);
                    m_ScratchSize = std::max<uint64_t>(scratchNeed, 1u << 20);
                    m_Scratch = CreateBuffer(m_ScratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                    if (!m_Scratch)
                    {
                        m_ScratchSize = 0;
                        return false;
                    }
                }
                ID3D12GraphicsCommandList4* list = nullptr;
                if (FAILED(Core::Get().List()->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void**)&list)) || !list)
                    return false;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
                desc.DestAccelerationStructureData = storage->GetGPUVirtualAddress();
                desc.Inputs = in;
                desc.ScratchAccelerationStructureData = m_Scratch->GetGPUVirtualAddress();
                // Önceki dispatch'lerin yapıyı / karalama alanını okuması ve yazması bitsin (VkGL'deki tam bariyerin karşılığı)
                UAVBarrier(list, nullptr);
                list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
                UAVBarrier(list, nullptr);
                list->Release();
                return true;
            }

            ID3D12Device5* m_Device = nullptr;
            std::unordered_map<uint32_t, ID3D12Resource*> m_Blas;
            uint32_t m_NextId = 1;
            ID3D12Resource* m_Tlas = nullptr;
            uint64_t m_TlasSize = 0;
            ID3D12Resource* m_Scratch = nullptr;
            uint64_t m_ScratchSize = 0;
            DX12::CpuDescriptor m_TlasSRV;
        };

        RayQuery* g_RayQuery = nullptr;
    }

    void InstallRayQuery()
    {
        if (g_RayQuery)
            return;
        ID3D12Device* device = Core::Get().Device();
        ID3D12Device5* device5 = nullptr;
        if (!device || FAILED(device->QueryInterface(__uuidof(ID3D12Device5), (void**)&device5)) || !device5)
        {
            CQ_CORE_INFO("[D12GL] Hardware ray queries not available (no ID3D12Device5): ray tracing uses the compute BVH");
            return;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        const bool tier = SUCCEEDED(device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) && o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_5 };
        const bool model = SUCCEEDED(device5->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
        const bool dxc = DXCommon::HasDXC();
        if (!tier || !model || !dxc)
        {
            CQ_CORE_INFO("[D12GL] Hardware ray queries not available ({0}): ray tracing uses the compute BVH",
                         !tier ? "the device has no DXR 1.1 / Raytracing Tier 1.1" : !model ? "the device has no Shader Model 6.5"
                                                                                       : "dxcompiler.dll / dxil.dll not found next to the executable");
            device5->Release();
            return;
        }
        g_RayQuery = new RayQuery(device5);
        GLEmu::SetRayQueryBackend(g_RayQuery);
        CQ_CORE_INFO("[D12GL] Hardware ray queries available (DXR 1.1 inline ray tracing): ray traced effects and Path Tracing use acceleration structures");
    }

    void ShutdownRayQuery()
    {
        if (!g_RayQuery)
            return;
        GLEmu::SetRayQueryBackend(nullptr);
        g_RayQuery->Shutdown();
        delete g_RayQuery;
        g_RayQuery = nullptr;
    }

    const DX12::CpuDescriptor* CurrentTLASSRV()
    {
        return g_RayQuery ? g_RayQuery->TlasSRV() : nullptr;
    }
}

#endif
