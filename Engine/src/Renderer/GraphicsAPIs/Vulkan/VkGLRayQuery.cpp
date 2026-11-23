#ifdef CQ_VULKAN_ENABLED

#include "VkGL.h"
#include "Renderer/GraphicsAPIs/GLEmu/GLEmuCommon.h"
#include "Core/Logging/Log.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

// 316: donanım ışın sorgusu (VK_KHR_acceleration_structure + VK_KHR_ray_query). Motorun ışın izleyicisi (RayTracingScene) BLAS'ları
// mesh başına bir kez, TLAS'ı örnekler değişince kurar; kurulum komutları VkGL'nin kare komut arabelleğine sırayla kaydedilir (tam
// bariyerle): aynı karede önce kaydedilen dispatch'ler eski yapıyı, sonrakiler yenisini görür. Hesaplama gölgelendiricileri
// GL_EXT_ray_query ile "u_TLAS"ı izler (VkGLDraw: hızlandırma yapısı tanımlayıcısı)
namespace Conqueror::VkGL
{
    using VK::Core;

    namespace
    {

        struct Allocation
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkDeviceMemory Memory = VK_NULL_HANDLE;
            void* Mapped = nullptr;
            VkDeviceSize Size = 0;
        };

        VkDeviceAddress AddressOf(VkBuffer buffer)
        {
            VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            ai.buffer = buffer;
            return vkGetBufferDeviceAddress(Core::Get().Device(), &ai);
        }

        bool Allocate(Allocation& a, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
        {
            a = {};
            a.Size = std::max<VkDeviceSize>(size, 256);
            const VkMemoryPropertyFlags props = hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            return Core::Get().CreateBuffer(a.Size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, props, a.Buffer, a.Memory, hostVisible ? &a.Mapped : nullptr);
        }

        // Kare bitince (GPU artık kullanmıyorken) silinir
        void Release(Allocation& a)
        {
            if (!a.Buffer)
                return;
            VkDevice dev = Core::Get().Device();
            VkBuffer b = a.Buffer;
            VkDeviceMemory m = a.Memory;
            Core::Get().DeferDelete([dev, b, m]() {
                vkDestroyBuffer(dev, b, nullptr);
                vkFreeMemory(dev, m, nullptr);
            });
            a = {};
        }

        void ReleaseAS(VkAccelerationStructureKHR as)
        {
            if (!as)
                return;
            VkDevice dev = Core::Get().Device();
            Core::Get().DeferDelete([dev, as]() { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });
        }

        class RayQuery final : public GLEmu::RayQueryBackend
        {
        public:
            const char* Name() const override { return "Vulkan ray query"; }

            uint32_t CreateBLAS(const float* positions, uint32_t triCount) override
            {
                if (!positions || triCount == 0)
                    return 0;
                // Köşeler: ana bilgisayardan görünür arabellek (bir kez kurulur; kurulum bitince silinir)
                Allocation vertices;
                const VkDeviceSize bytes = (VkDeviceSize)triCount * 9 * sizeof(float);
                if (!Allocate(vertices, bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true))
                    return 0;
                std::memcpy(vertices.Mapped, positions, (size_t)bytes);

                VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
                geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;   // opak olmayan örnek FORCE_NO_OPAQUE ile (Cutout / saydam)
                geom.geometry.triangles = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
                geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                geom.geometry.triangles.vertexData.deviceAddress = AddressOf(vertices.Buffer);
                geom.geometry.triangles.vertexStride = 3 * sizeof(float);
                geom.geometry.triangles.maxVertex = triCount * 3 - 1;
                geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

                Blas blas;
                if (!Build(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geom, triCount, blas.AS, blas.Storage))
                {
                    Release(vertices);
                    return 0;
                }
                Release(vertices);   // kurulum komutu çalıştıktan sonra silinir (kare sonu)
                VkAccelerationStructureDeviceAddressInfoKHR ai{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
                ai.accelerationStructure = blas.AS;
                blas.Address = vkGetAccelerationStructureDeviceAddressKHR(Core::Get().Device(), &ai);
                const uint32_t id = m_NextId++;
                m_Blas[id] = blas;
                return id;
            }

            void DestroyBLAS(uint32_t id) override
            {
                auto it = m_Blas.find(id);
                if (it == m_Blas.end())
                    return;
                ReleaseAS(it->second.AS);
                Release(it->second.Storage);
                m_Blas.erase(it);
            }

            bool BuildTLAS(const GLEmu::RayQueryInstance* instances, uint32_t count) override
            {
                std::vector<VkAccelerationStructureInstanceKHR> data;
                data.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    const GLEmu::RayQueryInstance& in = instances[i];
                    auto it = m_Blas.find(in.Blas);
                    if (it == m_Blas.end())
                        continue;
                    VkAccelerationStructureInstanceKHR v{};
                    std::memcpy(&v.transform, in.Transform, sizeof(v.transform));
                    v.instanceCustomIndex = in.CustomIndex & 0xFFFFFFu;
                    v.mask = 0xFF;
                    v.instanceShaderBindingTableRecordOffset = 0;
                    v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | (in.Opaque ? 0u : (uint32_t)VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
                    v.accelerationStructureReference = it->second.Address;
                    data.push_back(v);
                }
                // Örnek verisi: her kurulumda yeni arabellek (aynı karede ikinci kurulum ilkinin okuyacağını ezmesin), kare sonunda silinir
                Allocation inst;
                if (!Allocate(inst, std::max<size_t>(data.size(), 1) * sizeof(VkAccelerationStructureInstanceKHR),
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true))
                    return false;
                if (!data.empty())
                    std::memcpy(inst.Mapped, data.data(), data.size() * sizeof(VkAccelerationStructureInstanceKHR));
                VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
                geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                geom.geometry.instances = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
                geom.geometry.instances.arrayOfPointers = VK_FALSE;
                geom.geometry.instances.data.deviceAddress = AddressOf(inst.Buffer);
                // Yer yetiyorsa aynı yapı yeniden kurulur (önce kaydedilen dispatch'ler bariyerden önce eskisini okur); büyürse yenisi,
                // eskisi kare sonunda silinir
                const bool ok = Build(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geom, (uint32_t)data.size(), m_Tlas, m_TlasStorage);
                Release(inst);
                return ok;
            }

            VkAccelerationStructureKHR Tlas() const { return m_Tlas; }

            void Shutdown()
            {
                VkDevice dev = Core::Get().Device();
                auto destroy = [dev](VkAccelerationStructureKHR as, Allocation& a) {
                    if (as) vkDestroyAccelerationStructureKHR(dev, as, nullptr);
                    if (a.Buffer) vkDestroyBuffer(dev, a.Buffer, nullptr);
                    if (a.Memory) vkFreeMemory(dev, a.Memory, nullptr);
                    a = {};
                };
                for (auto& [id, b] : m_Blas)
                    destroy(b.AS, b.Storage);
                m_Blas.clear();
                destroy(m_Tlas, m_TlasStorage);
                m_Tlas = VK_NULL_HANDLE;
                destroy(VK_NULL_HANDLE, m_Scratch);
            }

        private:
            struct Blas
            {
                VkAccelerationStructureKHR AS = VK_NULL_HANDLE;
                Allocation Storage;
                VkDeviceAddress Address = 0;
            };

            // Yapıyı (as boşsa ya da yer yetmiyorsa yenisini oluşturarak) kurar; komutlar kare komut arabelleğine kaydedilir
            bool Build(VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geom, uint32_t primitives,
                       VkAccelerationStructureKHR& as, Allocation& storage)
            {
                VkDevice dev = Core::Get().Device();
                VkAccelerationStructureBuildGeometryInfoKHR info{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
                info.type = type;
                info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
                info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                info.geometryCount = 1;
                info.pGeometries = &geom;
                VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
                vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
                if (sizes.accelerationStructureSize == 0)
                    return false;
                if (!as || storage.Size < sizes.accelerationStructureSize)
                {
                    ReleaseAS(as);
                    Release(storage);
                    as = VK_NULL_HANDLE;
                    // TLAS: büyüdükçe yeniden oluşturmasın diye pay
                    const VkDeviceSize want = type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR ? sizes.accelerationStructureSize * 3 / 2
                                                                                                   : sizes.accelerationStructureSize;
                    if (!Allocate(storage, want, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
                        return false;
                    VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
                    ci.buffer = storage.Buffer;
                    ci.size = storage.Size;
                    ci.type = type;
                    if (vkCreateAccelerationStructureKHR(dev, &ci, nullptr, &as) != VK_SUCCESS)
                    {
                        Release(storage);
                        as = VK_NULL_HANDLE;
                        return false;
                    }
                }
                // Karalama alanı: tek arabellek (kurulumlar arasında tam bariyer), büyürse eskisi kare sonunda silinir
                const VkDeviceSize scratchNeed = sizes.buildScratchSize + 256;
                if (m_Scratch.Size < scratchNeed)
                {
                    Release(m_Scratch);
                    if (!Allocate(m_Scratch, std::max<VkDeviceSize>(scratchNeed, 1u << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false))
                        return false;
                }
                const VkDeviceAddress scratch = (AddressOf(m_Scratch.Buffer) + 255) & ~(VkDeviceAddress)255;
                info.dstAccelerationStructure = as;
                info.scratchData.deviceAddress = scratch;
                VkAccelerationStructureBuildRangeInfoKHR range{};
                range.primitiveCount = primitives;
                const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
                VkCommandBuffer cmd = TransferCmd();   // render geçişi dışında
                Core::Get().FullBarrier(cmd);
                vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges);
                Core::Get().FullBarrier(cmd);
                return true;
            }

            std::unordered_map<uint32_t, Blas> m_Blas;
            uint32_t m_NextId = 1;
            VkAccelerationStructureKHR m_Tlas = VK_NULL_HANDLE;
            Allocation m_TlasStorage;
            Allocation m_Scratch;
        };

        RayQuery* g_RayQuery = nullptr;
    }

    void InstallRayQuery()
    {
        if (g_RayQuery)
            return;
        if (!Core::Get().HasRayQuery())
        {
            CQ_CORE_INFO("[VkGL] Hardware ray queries {0}: ray tracing uses the compute BVH",
                         Core::Get().RayQueryEnabled() ? "enabled on the device but their functions are missing" : "not supported by this device / driver");
            return;
        }
        g_RayQuery = new RayQuery();
        GLEmu::SetRayQueryBackend(g_RayQuery);
        CQ_CORE_INFO("[VkGL] Hardware ray queries available (VK_KHR_ray_query): ray traced effects and Path Tracing use acceleration structures");
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

    VkAccelerationStructureKHR CurrentTLAS()
    {
        return g_RayQuery ? g_RayQuery->Tlas() : VK_NULL_HANDLE;
    }
}

#endif
