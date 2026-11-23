#ifdef CQ_VULKAN_ENABLED

#include "VkGL.h"
#include "../GLEmu/GLShaderUtil.h"
#include "Renderer/ShaderCompiler/ShaderCompiler.h"
#include "Core/Logging/Log.h"

#include <spirv_cross.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>

namespace Conqueror::VkGL
{
    using VK::Core;

    namespace
    {
        // GL GLSL -> Vulkan GLSL: sürüm satırı, gl_VertexID/InstanceID, derinlik aralığı [-1,1] -> [0,1]
        std::string PatchSource(const std::string& src, GLenum type)
        {
            std::string s = src;
            // #version satırı (yorum ve boşlukları atlayarak ilk yönerge)
            std::regex versionRe(R"(#version[^\n]*)");
            std::smatch vm;
            // 4.60 korunur (316: GL_EXT_ray_query 4.60 ister), diğerleri 4.50
            const bool v460 = std::regex_search(s, vm, versionRe) && vm.str().find("460") != std::string::npos;
            if (std::regex_search(s, versionRe))
                s = std::regex_replace(s, versionRe, v460 ? "#version 460" : "#version 450", std::regex_constants::format_first_only);
            else
                s = "#version 450\n" + s;
            s = std::regex_replace(s, std::regex(R"(\bgl_VertexID\b)"), "gl_VertexIndex");
            s = std::regex_replace(s, std::regex(R"(\bgl_InstanceID\b)"), "gl_InstanceIndex");
            if (type == GL_VERTEX_SHADER)
            {
                std::regex mainRe(R"(\bvoid\s+main\s*\(\s*(void)?\s*\))");
                if (std::regex_search(s, mainRe))
                {
                    s = std::regex_replace(s, mainRe, "void cq_vk_main()", std::regex_constants::format_first_only);
                    s += "\nvoid main()\n{\n    cq_vk_main();\n    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
                }
            }
            return s;
        }

        ShaderStage StageOf(GLenum t)
        {
            switch (t)
            {
                case GL_FRAGMENT_SHADER: return ShaderStage::Fragment;
                case GL_GEOMETRY_SHADER: return ShaderStage::Geometry;
                case GL_COMPUTE_SHADER: return ShaderStage::Compute;
                default: return ShaderStage::Vertex;
            }
        }

        GLenum GLTypeOf(const spirv_cross::SPIRType& t)
        {
            using BT = spirv_cross::SPIRType::BaseType;
            if (t.columns > 1)
            {
                if (t.columns == 2) return GL_FLOAT_MAT2;
                if (t.columns == 3) return t.vecsize == 3 ? GL_FLOAT_MAT3 : GL_FLOAT_MAT3x4;
                return t.vecsize == 4 ? GL_FLOAT_MAT4 : GL_FLOAT_MAT4x3;
            }
            if (t.basetype == BT::Int)
                return t.vecsize == 1 ? GL_INT : t.vecsize == 2 ? GL_INT_VEC2 : t.vecsize == 3 ? GL_INT_VEC3 : GL_INT_VEC4;
            if (t.basetype == BT::UInt)
                return t.vecsize == 1 ? GL_UNSIGNED_INT : t.vecsize == 2 ? GL_UNSIGNED_INT_VEC2 : t.vecsize == 3 ? GL_UNSIGNED_INT_VEC3 : GL_UNSIGNED_INT_VEC4;
            if (t.basetype == BT::Boolean)
                return t.vecsize == 1 ? GL_BOOL : GL_BOOL_VEC4;
            return t.vecsize == 1 ? GL_FLOAT : t.vecsize == 2 ? GL_FLOAT_VEC2 : t.vecsize == 3 ? GL_FLOAT_VEC3 : GL_FLOAT_VEC4;
        }

        void AddSlot(Program& p, const std::string& name, const UniformSlot& slot)
        {
            const int loc = (int)p.Uniforms.size();
            p.Uniforms.push_back(slot);
            p.Uniforms.back().Name = name;
            if (!p.UniformByName.count(name))
                p.UniformByName[name] = loc;
        }

        // CQ_Globals üyelerini düzleştir: diziler eleman eleman, yapılar alan alan ayrı konum alır
        void Flatten(spirv_cross::Compiler& comp, const spirv_cross::SPIRType& st, uint32_t base, const std::string& prefix, Program& p)
        {
            for (uint32_t i = 0; i < (uint32_t)st.member_types.size(); ++i)
            {
                const std::string name = prefix + comp.get_member_name(st.self, i);
                const uint32_t off = base + comp.type_struct_member_offset(st, i);
                const spirv_cross::SPIRType& mt = comp.get_type(st.member_types[i]);
                if (!mt.array.empty())
                {
                    const uint32_t count = std::max(1u, mt.array.back());
                    const uint32_t stride = comp.type_struct_member_array_stride(st, i);
                    const spirv_cross::SPIRType& elem = comp.get_type(mt.parent_type);
                    for (uint32_t e = 0; e < count; ++e)
                    {
                        const std::string en = name + "[" + std::to_string(e) + "]";
                        if (elem.basetype == spirv_cross::SPIRType::Struct)
                        {
                            Flatten(comp, elem, off + e * stride, en + ".", p);
                            continue;
                        }
                        UniformSlot slot;
                        slot.Offset = off + e * stride;
                        slot.Type = GLTypeOf(mt);
                        slot.MatrixStride = mt.columns > 1 ? comp.type_struct_member_matrix_stride(st, i) : 16;
                        slot.ArrayStride = stride;
                        slot.ArrayRemaining = (int)(count - e);
                        AddSlot(p, en, slot);
                        if (e == 0)
                            p.UniformByName[name] = (int)p.Uniforms.size() - 1;
                    }
                    continue;
                }
                if (mt.basetype == spirv_cross::SPIRType::Struct)
                {
                    Flatten(comp, mt, off, name + ".", p);
                    continue;
                }
                UniformSlot slot;
                slot.Offset = off;
                slot.Type = GLTypeOf(mt);
                slot.MatrixStride = mt.columns > 1 ? comp.type_struct_member_matrix_stride(st, i) : 16;
                AddSlot(p, name, slot);
            }
        }

        VkFormat InputFormat(const spirv_cross::SPIRType& t, bool& integer)
        {
            using BT = spirv_cross::SPIRType::BaseType;
            integer = t.basetype == BT::Int || t.basetype == BT::UInt;
            const uint32_t n = t.vecsize;
            if (t.basetype == BT::Int)
                return n == 1 ? VK_FORMAT_R32_SINT : n == 2 ? VK_FORMAT_R32G32_SINT : n == 3 ? VK_FORMAT_R32G32B32_SINT : VK_FORMAT_R32G32B32A32_SINT;
            if (t.basetype == BT::UInt)
                return n == 1 ? VK_FORMAT_R32_UINT : n == 2 ? VK_FORMAT_R32G32_UINT : n == 3 ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32A32_UINT;
            return n == 1 ? VK_FORMAT_R32_SFLOAT : n == 2 ? VK_FORMAT_R32G32_SFLOAT : n == 3 ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        }

        // Bağlama noktalarını yeniden numaralandır, tek kümeye topla ve GL'ye özgü uniform konumlarını at
        // (otomatik konum eşlemesi örnekleyicilere Location verir; Vulkan'da yalnızca girdi/çıktıda geçerli)
        void PatchBindings(std::vector<uint32_t>& spv, const std::map<uint32_t, uint32_t>& idToBinding)
        {
            if (spv.size() < 5)
                return;
            std::set<uint32_t> uniformVars;
            for (size_t i = 5; i < spv.size();)
            {
                const uint32_t op = spv[i] & 0xFFFF, count = spv[i] >> 16;
                if (count == 0)
                    break;
                // OpVariable <type> <id> <storage class>: UniformConstant(0), Uniform(2), StorageBuffer(12)
                if (op == 59 && count >= 4 && i + 3 < spv.size() && (spv[i + 3] == 0 || spv[i + 3] == 2 || spv[i + 3] == 12))
                    uniformVars.insert(spv[i + 2]);
                i += count;
            }
            std::vector<uint32_t> out(spv.begin(), spv.begin() + 5);
            out.reserve(spv.size());
            for (size_t i = 5; i < spv.size();)
            {
                const uint32_t op = spv[i] & 0xFFFF, count = spv[i] >> 16;
                if (count == 0 || i + count > spv.size())
                {
                    out.insert(out.end(), spv.begin() + (std::ptrdiff_t)i, spv.end());
                    break;
                }
                if (op == 71 && count >= 3)
                {
                    const uint32_t target = spv[i + 1], decoration = spv[i + 2];
                    // OpDecorate <uniform> Location: at
                    if ((decoration == 30 || decoration == 31) && uniformVars.count(target))
                    {
                        i += count;
                        continue;
                    }
                }
                const size_t at = out.size();
                out.insert(out.end(), spv.begin() + (std::ptrdiff_t)i, spv.begin() + (std::ptrdiff_t)(i + count));
                // OpDecorate <id> Binding <n>
                if (op == 71 && count >= 4 && out[at + 2] == 33)
                {
                    auto it = idToBinding.find(out[at + 1]);
                    if (it != idToBinding.end())
                        out[at + 3] = it->second;
                }
                // OpDecorate <id> DescriptorSet <n> -> tek küme
                if (op == 71 && count >= 4 && out[at + 2] == 34)
                    out[at + 3] = 0;
                i += count;
            }
            spv.swap(out);
        }

        // GL uniform başlangıç değerleri ("uniform float u_X = 0.6;"): Vulkan bloğunda kaybolur, Globals'a yazılır
        struct UniformInit
        {
            std::string Name;
            std::string Type;
            std::vector<double> Values;
        };

        std::vector<UniformInit> ParseUniformInitializers(const std::string& src)
        {
            std::vector<UniformInit> out;
            static const std::regex s_Init(R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|bvec[234])\s+(\w+)\s*=\s*([^;]+);)");
            static const std::regex s_Number(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?|true|false)");
            for (std::sregex_iterator m(src.begin(), src.end(), s_Init), end; m != end; ++m)
            {
                UniformInit u;
                u.Type = (*m)[1].str();
                u.Name = (*m)[2].str();
                const std::string expr = (*m)[3].str();
                for (std::sregex_iterator n(expr.begin(), expr.end(), s_Number), e2; n != e2; ++n)
                {
                    const std::string t = n->str();
                    if (t == "true") u.Values.push_back(1.0);
                    else if (t == "false") u.Values.push_back(0.0);
                    else u.Values.push_back(std::strtod(t.c_str(), nullptr));
                }
                // vecN(..) yapıcısındaki "N" de sayı olarak yakalanır: tür adının sayısını at
                if (u.Type.size() > 3 && std::isdigit((unsigned char)u.Type.back()) && !u.Values.empty() && expr.find('(') != std::string::npos)
                {
                    const std::string head = expr.substr(0, expr.find('('));
                    if (head.find_first_of("0123456789") != std::string::npos)
                        u.Values.erase(u.Values.begin());
                }
                if (!u.Values.empty())
                    out.push_back(std::move(u));
            }
            return out;
        }

        void ApplyUniformInitializers(Program& p, const std::vector<UniformInit>& inits)
        {
            for (const auto& u : inits)
            {
                auto it = p.UniformByName.find(u.Name);
                if (it == p.UniformByName.end())
                    continue;
                const UniformSlot& slot = p.Uniforms[(size_t)it->second];
                if (slot.Sampler >= 0)
                    continue;
                int comps = 1;
                if (u.Type.size() > 3 && std::isdigit((unsigned char)u.Type.back()))
                    comps = u.Type.back() - '0';
                const bool isFloat = u.Type == "float" || u.Type.rfind("vec", 0) == 0;
                for (int i = 0; i < comps; ++i)
                {
                    const double v = u.Values[std::min<size_t>((size_t)i, u.Values.size() - 1)];
                    const size_t off = (size_t)slot.Offset + (size_t)i * 4;
                    if (off + 4 > p.Globals.size())
                        break;
                    if (isFloat)
                    {
                        const float f = (float)v;
                        std::memcpy(p.Globals.data() + off, &f, 4);
                    }
                    else
                    {
                        const int32_t iv = (int32_t)v;
                        std::memcpy(p.Globals.data() + off, &iv, 4);
                    }
                }
            }
        }
    }

    void DestroyProgram(Program& p)
    {
        auto& core = Core::Get();
        if (!core.IsInitialized())
            return;
        VkDevice dev = core.Device();
        VkShaderModule v = p.Vert, f = p.Frag, cs = p.Comp;
        VkPipeline cp = p.ComputePipeline;
        VkDescriptorSetLayout sl = p.SetLayout;
        VkPipelineLayout pl = p.Layout;
        core.DeferDelete([dev, v, f, cs, cp, sl, pl]() {
            if (v) vkDestroyShaderModule(dev, v, nullptr);
            if (f) vkDestroyShaderModule(dev, f, nullptr);
            if (cs) vkDestroyShaderModule(dev, cs, nullptr);
            if (cp) vkDestroyPipeline(dev, cp, nullptr);
            if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
            if (sl) vkDestroyDescriptorSetLayout(dev, sl, nullptr);
        });
        p.Vert = p.Frag = p.Comp = VK_NULL_HANDLE;
        p.ComputePipeline = VK_NULL_HANDLE;
        p.SetLayout = VK_NULL_HANDLE;
        p.Layout = VK_NULL_HANDLE;
    }

    bool LinkProgram(Program& p)
    {
        auto& c = Ctx();
        auto& core = Core::Get();
        DestroyProgram(p);
        p.Linked = false;
        p.Log.clear();
        p.Uniforms.clear();
        p.UniformByName.clear();
        p.Samplers.clear();
        p.Blocks.clear();
        p.Inputs.clear();
        p.Images.clear();
        p.StorageBlocks.clear();
        p.AccelStructBindings.clear();
        p.IsCompute = false;
        p.LocalSize[0] = p.LocalSize[1] = p.LocalSize[2] = 1;
        p.GlobalsBinding = -1;
        p.GlobalsSize = 0;

        std::map<ShaderStage, std::string> sources;
        std::map<std::string, int> explicitBindings;   // kaynakta layout(binding = N) ile verilen örnekleyiciler
        // Bellek niteleyicileri uniform'dan önce ya da sonra olabilir ("uniform readonly image2D u_In": eskiden ad yerine "image2D"
        // yakalanıyor, yol izleyicinin süzgecinde iki görüntü de birim 0'a düşüyordu)
        static const std::regex s_LayoutUniform(
            R"(layout\s*\(([^)]*)\)\s*(?:(?:readonly|writeonly|coherent|volatile|restrict)\s+)*uniform\s+(?:(?:readonly|writeonly|coherent|volatile|restrict|lowp|mediump|highp)\s+)*\w+\s+(\w+))");
        static const std::regex s_Binding(R"(binding\s*=\s*(\d+))");
        std::vector<UniformInit> inits;
        static const bool s_ForceSplit = [] { const char* e = std::getenv("CQ_VKGL_SPLIT_SAMPLERS"); return e && e[0] == '1'; }();
        const bool splitSamplers = s_ForceSplit || core.IsPortability() || core.Properties().limits.maxPerStageDescriptorSamplers < 64;
        for (uint32_t name : p.Shaders)
        {
            auto it = c.ShaderObjects.find(name);
            if (it == c.ShaderObjects.end())
                continue;
            std::string patched = PatchSource(it->second->Source, it->second->Type);
            // Metal (MoltenVK) aşama başına 16 örnekleyici: büyük örnekleyici dizileri ayrık doku + tek örnekleyici
            if (splitSamplers)
                patched = GLEmu::SplitLargeSamplerArrays(patched, 8);
            sources[StageOf(it->second->Type)] = std::move(patched);
            const std::string& src = it->second->Source;
            for (auto& u : ParseUniformInitializers(src))
                inits.push_back(std::move(u));
            for (std::sregex_iterator m(src.begin(), src.end(), s_LayoutUniform), end; m != end; ++m)
            {
                const std::string qualifiers = (*m)[1].str();
                std::smatch b;
                if (std::regex_search(qualifiers, b, s_Binding))
                    explicitBindings[(*m)[2].str()] = std::stoi(b[1].str());
            }
        }
        // Hesaplama programı (307): tek hesaplama aşaması (NIS, CMAA2, FSR 2); köşe / parça aşamasıyla birlikte olamaz
        const bool compute = sources.count(ShaderStage::Compute) != 0;
        if (compute && sources.size() != 1)
        {
            p.Log = "A compute program cannot have other shader stages";
            return false;
        }
        if (!compute && (!sources.count(ShaderStage::Vertex) || !sources.count(ShaderStage::Fragment)))
        {
            p.Log = "Program needs a vertex and a fragment shader";
            return false;
        }
        // Hata ayıklama: CQ_VKGL_DUMP=<klasör> çevrilen kaynakları yazar
        static const char* s_Dump = std::getenv("CQ_VKGL_DUMP");
        if (s_Dump && *s_Dump)
            for (const auto& [stage, text] : sources)
                std::ofstream(std::string(s_Dump) + "/prog" + std::to_string(c.NextProgramId) + (stage == ShaderStage::Vertex ? ".vert" : stage == ShaderStage::Compute ? ".comp" : ".frag")) << text;
        std::map<ShaderStage, std::vector<uint32_t>> spirv;
        std::string errors;
        if (!ShaderCompiler::CompileToSPIRV(sources, "vkgl", spirv, errors, true))
        {
            p.Log = errors;
            return false;
        }

        // ── Yansıtma: kaynaklar ada göre birleşik, bağlama noktaları yeniden numaralanır ──
        struct Pending
        {
            std::string Name;
            enum Kind { Block, Globals, Sampler, Image, SamplerOnly, StorageImage, StorageBuffer, AccelStruct } K = Block;
            uint32_t OrigBinding = 0;
            uint32_t Size = 0;
            uint32_t Count = 1;
            GLenum Target = GL_TEXTURE_2D;
            bool Shadow = false, Integer = false;
            bool Writable = true;               // depolama görüntüsü / arabelleği: readonly değil
        };
        std::vector<Pending> resources;
        auto findRes = [&](const std::string& name) -> int {
            for (int i = 0; i < (int)resources.size(); ++i)
                if (resources[(size_t)i].Name == name)
                    return i;
            return -1;
        };
        std::map<ShaderStage, std::map<uint32_t, int>> stageIdToRes;

        for (auto& [stage, words] : spirv)
        {
            spirv_cross::Compiler comp(words);
            spirv_cross::ShaderResources res = comp.get_shader_resources();
            // Depolama görüntüleri / blokları yalnız etkin olanlar (GL'de kullanılmayan blok etkin değildir, bağlanması gerekmez: ortak ışın
            // izleme başlığını içeren ama Tris'i okumayan ortalama gölgelendiricisi gibi)
            const spirv_cross::ShaderResources activeRes = comp.get_shader_resources(comp.get_active_interface_variables());
            for (const auto& ub : res.uniform_buffers)
            {
                const auto& type = comp.get_type(ub.base_type_id);
                std::string blockName = comp.get_name(ub.base_type_id);
                if (blockName.empty())
                    blockName = ub.name;
                const bool globals = blockName == "CQ_Globals";
                int idx = findRes(blockName);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = blockName;
                    r.K = globals ? Pending::Globals : Pending::Block;
                    r.OrigBinding = comp.get_decoration(ub.id, spv::DecorationBinding);
                    r.Size = (uint32_t)comp.get_declared_struct_size(type);
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                else
                {
                    resources[(size_t)idx].Size = std::max(resources[(size_t)idx].Size, (uint32_t)comp.get_declared_struct_size(type));
                }
                stageIdToRes[stage][ub.id] = idx;
                if (globals)
                {
                    // Üye tablosu (her iki aşamada aynı düzen; ilk gelen yeterli ama en büyüğü al)
                    if ((uint32_t)comp.get_declared_struct_size(type) >= p.GlobalsSize)
                    {
                        p.Uniforms.erase(std::remove_if(p.Uniforms.begin(), p.Uniforms.end(), [](const UniformSlot& u) { return u.Sampler < 0; }),
                                         p.Uniforms.end());
                        p.UniformByName.clear();
                        Flatten(comp, type, 0, "", p);
                        p.GlobalsSize = (uint32_t)comp.get_declared_struct_size(type);
                    }
                }
            }
            for (const auto& si : res.sampled_images)
            {
                int idx = findRes(si.name);
                const auto& type = comp.get_type(si.type_id);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = si.name;
                    r.K = Pending::Sampler;
                    r.OrigBinding = comp.get_decoration(si.id, spv::DecorationBinding);
                    r.Count = type.array.empty() ? 1u : std::max(1u, type.array.back());
                    r.Target = type.image.dim == spv::DimCube ? GL_TEXTURE_CUBE_MAP : type.image.ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
                    r.Shadow = type.image.depth;
                    const auto& sampled = comp.get_type(type.image.type);
                    r.Integer = sampled.basetype == spirv_cross::SPIRType::Int || sampled.basetype == spirv_cross::SPIRType::UInt;
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][si.id] = idx;
            }
            // Ayrık doku dizileri (SplitLargeSamplerArrays) ve ortak örnekleyicileri
            for (const auto& si : res.separate_images)
            {
                int idx = findRes(si.name);
                const auto& type = comp.get_type(si.type_id);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = si.name;
                    r.K = Pending::Image;
                    r.Count = type.array.empty() ? 1u : std::max(1u, type.array.back());
                    r.Target = type.image.dim == spv::DimCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
                    const auto& sampled = comp.get_type(type.image.type);
                    r.Integer = sampled.basetype == spirv_cross::SPIRType::Int || sampled.basetype == spirv_cross::SPIRType::UInt;
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][si.id] = idx;
            }
            // Depolama görüntüleri (image2D / uimage2D) ve depolama blokları (std430 buffer): yalnız hesaplama programında
            for (const auto& si : activeRes.storage_images)
            {
                int idx = findRes(si.name);
                const auto& type = comp.get_type(si.type_id);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = si.name;
                    r.K = Pending::StorageImage;
                    r.OrigBinding = comp.get_decoration(si.id, spv::DecorationBinding);
                    const auto& sampled = comp.get_type(type.image.type);
                    r.Integer = sampled.basetype == spirv_cross::SPIRType::Int || sampled.basetype == spirv_cross::SPIRType::UInt;
                    r.Writable = !comp.has_decoration(si.id, spv::DecorationNonWritable);
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][si.id] = idx;
            }
            for (const auto& sb : activeRes.storage_buffers)
            {
                std::string blockName = comp.get_name(sb.base_type_id);
                if (blockName.empty())
                    blockName = sb.name;
                int idx = findRes(blockName);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = blockName;
                    r.K = Pending::StorageBuffer;
                    r.OrigBinding = comp.get_decoration(sb.id, spv::DecorationBinding);
                    const spirv_cross::Bitset flags = comp.get_buffer_block_flags(sb.id);
                    r.Writable = !flags.get(spv::DecorationNonWritable);
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][sb.id] = idx;
            }
            // 316: donanım ışın sorgusunun hızlandırma yapısı (GL_EXT_ray_query; TLAS'ı VkGLRayQuery kurar)
            for (const auto& as : activeRes.acceleration_structures)
            {
                int idx = findRes(as.name);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = as.name;
                    r.K = Pending::AccelStruct;
                    r.OrigBinding = comp.get_decoration(as.id, spv::DecorationBinding);
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][as.id] = idx;
            }
            if (stage == ShaderStage::Compute)
                for (uint32_t k = 0; k < 3; ++k)
                    p.LocalSize[k] = std::max(1u, comp.get_execution_mode_argument(spv::ExecutionModeLocalSize, k));
            for (const auto& si : res.separate_samplers)
            {
                int idx = findRes(si.name);
                if (idx < 0)
                {
                    Pending r;
                    r.Name = si.name;
                    r.K = Pending::SamplerOnly;
                    resources.push_back(r);
                    idx = (int)resources.size() - 1;
                }
                stageIdToRes[stage][si.id] = idx;
            }
            if (stage == ShaderStage::Vertex)
            {
                for (const auto& in : res.stage_inputs)
                {
                    // mat4 / dizi girdileri ardışık konumlar kaplar (sütun / eleman başına bir konum)
                    const auto& type = comp.get_type(in.type_id);
                    const uint32_t base = comp.get_decoration(in.id, spv::DecorationLocation);
                    const uint32_t columns = std::max(1u, type.columns);
                    const uint32_t elements = type.array.empty() ? 1u : std::max(1u, type.array[0]);
                    for (uint32_t k = 0; k < columns * elements; ++k)
                    {
                        VertexInput vi;
                        vi.Location = (int)(base + k);
                        vi.Format = InputFormat(type, vi.Integer);
                        vi.Name = k == 0 ? in.name : in.name + "#" + std::to_string(k);
                        p.Inputs.push_back(vi);
                    }
                }
            }
            if (stage == ShaderStage::Fragment)
            {
                uint32_t outputs = 0, mask = 0;
                for (const auto& out : res.stage_outputs)
                {
                    const auto& type = comp.get_type(out.type_id);
                    const uint32_t loc = comp.get_decoration(out.id, spv::DecorationLocation);
                    const uint32_t count = type.array.empty() ? 1u : std::max(1u, type.array[0]);
                    for (uint32_t k = 0; k < count && loc + k < 32; ++k)
                        mask |= 1u << (loc + k);
                    outputs = std::max(outputs, loc + count);
                }
                p.ColorOutputs = std::max(1u, outputs);
                p.ColorOutputMask = mask;
            }
        }

        // Yeni bağlama noktaları: bloklar, global blok, örnekleyiciler
        uint32_t next = 0;
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        std::vector<uint32_t> finalBinding(resources.size(), 0);
        // Ortak örnekleyiciler önce numaralanır: doku dizisi kaydı kendi örnekleyicisinin bağlamasını bilmeli
        std::map<std::string, uint32_t> sharedSamplerBinding;
        for (int pass = 0; pass < 7; ++pass)
        {
            for (size_t i = 0; i < resources.size(); ++i)
            {
                const auto& r = resources[i];
                const bool match = (pass == 0 && r.K == Pending::Block) || (pass == 1 && r.K == Pending::Globals) || (pass == 2 && r.K == Pending::SamplerOnly) ||
                                   (pass == 3 && (r.K == Pending::Sampler || r.K == Pending::Image)) || (pass == 4 && r.K == Pending::StorageImage) ||
                                   (pass == 5 && r.K == Pending::StorageBuffer) || (pass == 6 && r.K == Pending::AccelStruct);
                if (!match)
                    continue;
                finalBinding[i] = next;
                VkDescriptorSetLayoutBinding lb{};
                lb.binding = next;
                lb.descriptorType = r.K == Pending::Sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : r.K == Pending::Image ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                                    r.K == Pending::SamplerOnly ? VK_DESCRIPTOR_TYPE_SAMPLER : r.K == Pending::StorageImage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                                    r.K == Pending::StorageBuffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                                    r.K == Pending::AccelStruct ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                lb.descriptorCount = r.Count;
                lb.stageFlags = compute ? VK_SHADER_STAGE_COMPUTE_BIT : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                layoutBindings.push_back(lb);
                if (r.K == Pending::StorageImage)
                {
                    // GL: birim kaynaktaki layout(binding) (motor görüntülerinkini siler, glUniform1i ile verir); uniform konumu alır
                    GLEmu::ImageBinding ib;
                    ib.Name = r.Name;
                    ib.Binding = next;
                    ib.Integer = r.Integer;
                    ib.Writable = r.Writable;
                    if (auto eb = explicitBindings.find(r.Name); eb != explicitBindings.end())
                        ib.Unit = eb->second;
                    p.Images.push_back(ib);
                    UniformSlot slot;
                    slot.Image = (int)p.Images.size() - 1;
                    slot.Type = r.Integer ? GL_UNSIGNED_INT_IMAGE_2D : GL_IMAGE_2D;
                    AddSlot(p, r.Name, slot);
                }
                else if (r.K == Pending::StorageBuffer)
                {
                    GLEmu::StorageBlockBinding sb;
                    sb.Name = r.Name;
                    sb.Binding = next;
                    sb.GLBinding = r.OrigBinding;
                    sb.Writable = r.Writable;
                    p.StorageBlocks.push_back(sb);
                }
                else if (r.K == Pending::AccelStruct)
                    p.AccelStructBindings.push_back(next);
                else if (r.K == Pending::Block)
                    p.Blocks.push_back({ r.Name, next, r.OrigBinding, r.Size });
                else if (r.K == Pending::Globals)
                    p.GlobalsBinding = (int)next;
                else if (r.K == Pending::SamplerOnly)
                    sharedSamplerBinding[r.Name] = next;
                else
                {
                    // Ayrık doku dizisi: GL adı "<ad>_cqtex" son ekinden arındırılır, eşi "<ad>_cqsmp"
                    const bool image = r.K == Pending::Image;
                    const std::string glName = image && r.Name.size() > 6 ? r.Name.substr(0, r.Name.size() - 6) : r.Name;
                    SamplerBinding sb;
                    sb.Name = glName;
                    sb.Binding = next;
                    sb.SharedSampler = image;
                    if (image)
                        if (auto ss = sharedSamplerBinding.find(glName + "_cqsmp"); ss != sharedSamplerBinding.end())
                            sb.SamplerSlot = ss->second;
                    sb.Count = r.Count;
                    sb.Target = r.Target;
                    sb.Shadow = r.Shadow;
                    sb.Integer = r.Integer;
                    // GL: başlangıç birimi kaynaktaki layout(binding) değeri; yoksa 0 (otomatik eşlenen bağlama değil)
                    sb.Units.assign(r.Count, 0);
                    if (auto eb = explicitBindings.find(glName); eb != explicitBindings.end())
                        for (uint32_t e = 0; e < r.Count; ++e)
                            sb.Units[e] = eb->second + (int)e;
                    p.Samplers.push_back(sb);
                    // Örnekleyiciler de uniform konumu alır (glUniform1i ile birim)
                    const int sIdx = (int)p.Samplers.size() - 1;
                    for (uint32_t e = 0; e < r.Count; ++e)
                    {
                        UniformSlot slot;
                        slot.Sampler = sIdx;
                        slot.SamplerElement = (int)e;
                        slot.Type = GL_SAMPLER_2D;
                        slot.ArrayRemaining = (int)(r.Count - e);
                        const std::string n = r.Count > 1 ? glName + "[" + std::to_string(e) + "]" : glName;
                        AddSlot(p, n, slot);
                        if (e == 0 && r.Count > 1)
                            p.UniformByName[glName] = (int)p.Uniforms.size() - 1;
                    }
                }
                ++next;
            }
        }
        for (auto& [stage, words] : spirv)
        {
            std::map<uint32_t, uint32_t> remap;
            for (const auto& [id, resIdx] : stageIdToRes[stage])
                remap[id] = finalBinding[(size_t)resIdx];
            PatchBindings(words, remap);
        }

        // ── Vulkan nesneleri ──
        VkDevice dev = core.Device();
        auto makeModule = [&](const std::vector<uint32_t>& words) {
            VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            mi.codeSize = words.size() * 4;
            mi.pCode = words.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev, &mi, nullptr, &m);
            return m;
        };
        if (compute)
            p.Comp = makeModule(spirv[ShaderStage::Compute]);
        else
        {
            p.Vert = makeModule(spirv[ShaderStage::Vertex]);
            p.Frag = makeModule(spirv[ShaderStage::Fragment]);
        }
        VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dl.bindingCount = (uint32_t)layoutBindings.size();
        dl.pBindings = layoutBindings.data();
        vkCreateDescriptorSetLayout(dev, &dl, nullptr, &p.SetLayout);
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &p.SetLayout;
        vkCreatePipelineLayout(dev, &pli, nullptr, &p.Layout);

        p.Globals.assign(std::max<uint32_t>(p.GlobalsSize, 16), 0);
        ApplyUniformInitializers(p, inits);
        p.GlobalsDirty = true;
        p.GlobalsFrame = 0;
        p.Id = c.NextProgramId++;
        p.IsCompute = compute;
        if (compute && p.Comp && p.Layout)
        {
            VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = p.Comp;
            ci.stage.pName = "main";
            ci.layout = p.Layout;
            if (vkCreateComputePipelines(dev, core.PipelineCache(), 1, &ci, nullptr, &p.ComputePipeline) != VK_SUCCESS)
                p.ComputePipeline = VK_NULL_HANDLE;
        }
        p.Linked = compute ? (p.Comp && p.Layout && p.ComputePipeline) : (p.Vert && p.Frag && p.Layout);
        if (!p.Linked)
            p.Log = compute ? "Vulkan compute pipeline creation failed" : "Vulkan shader module creation failed";
        else if (!p.Label.empty())
            ProgramLabelGL(p);   // yeniden bağlamada yeni nesneler de adını alır
        return p.Linked;
    }

    // 323: VK_EXT_debug_utils nesne adı (uzantı yoksa sessizce atlanır)
    void NameVkObject(VkObjectType type, uint64_t handle, const std::string& name)
    {
        static PFN_vkSetDebugUtilsObjectNameEXT fn = nullptr;
        static VkInstance looked = VK_NULL_HANDLE;
        auto& core = VK::Core::Get();
        if (!core.HasDebugUtils() || !handle || name.empty())
            return;
        if (looked != core.Instance())
        {
            looked = core.Instance();
            fn = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(core.Instance(), "vkSetDebugUtilsObjectNameEXT");
        }
        if (!fn)
            return;
        VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name.c_str();
        fn(core.Device(), &info);
    }

    void ProgramLabelGL(Program& p)
    {
        NameVkObject(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)p.Vert, p.Label + " (vertex)");
        NameVkObject(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)p.Frag, p.Label + " (fragment)");
        NameVkObject(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)p.Comp, p.Label + " (compute)");
        NameVkObject(VK_OBJECT_TYPE_PIPELINE, (uint64_t)p.ComputePipeline, p.Label);
        NameVkObject(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)p.Layout, p.Label);
    }

    UniformSlot* FindUniform(Program& p, GLint location)
    {
        if (location < 0 || location >= (GLint)p.Uniforms.size())
            return nullptr;
        return &p.Uniforms[(size_t)location];
    }
}

#endif
