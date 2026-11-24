#ifdef CQ_METAL_ENABLED

// Yansıtma ortak (GLEmu/GLReflect.cpp); burada SPIR-V -> MSL (SPIRV-Cross) -> MTLLibrary (çalışma anında derleme).
// Dizinler: sabit arabellek yazmacı b -> [[buffer(16 + b)]], doku / örnekleyici yazmacı -> [[texture(r)]] / [[sampler(r)]].

#include "MtlGL.h"
#include "Core/Logging/Log.h"

#include <spirv_msl.hpp>

#include <cstdlib>
#include <fstream>

namespace Conqueror::MtlGL
{
    using MTL::Core;

    namespace
    {
        // Parametre adı 'id' olamaz: Objective-C'nin id türünü gölgeliyordu (id<MTLLibrary> derlenmiyordu)
        // stage: 0 köşe, 1 parça, 2 hesaplama. Depolama görüntüleri ve blokları için Metal dizinleri rp'den (LinkProgram atar)
        id<MTLFunction> CompileStage(std::vector<uint32_t> words, int stage, uint64_t programId, const GLEmu::ReflectedProgram& rp, std::string& log,
                                     bool* needsBufferSizes = nullptr)
        {
            const bool vertex = stage == 0;
            std::string msl;
            std::string entry = "main0";
            try
            {
                spirv_cross::CompilerMSL comp(std::move(words));
                spirv_cross::CompilerMSL::Options mo;
#if defined(CQ_PLATFORM_IOS)
                mo.platform = spirv_cross::CompilerMSL::Options::iOS;
#else
                mo.platform = spirv_cross::CompilerMSL::Options::macOS;
#endif
                mo.set_msl_version(2, 1);
                // 316: ışın sorgusu (intersection_query, kullanıcı örnek kimliği) MSL 2.4 ister
                if (stage == 2 && rp.AccelStructRegister >= 0)
                    mo.set_msl_version(2, 4);
                // Hesaplama (307): görüntü atomikleri (FSR 2'nin r32ui sayaçları) Metal 3.1'de doğrudan
                // Hesaplamada doku atomikleri için Metal 3.1 (çizim gölgelendiricileri sınanmış 2.1 çıktısında kalır)
                if (stage == 2 && Core::Get().SupportsTextureAtomics())
                    mo.set_msl_version(3, 1);
                mo.enable_decoration_binding = true;   // SPIR-V Binding = Metal dizini
                mo.buffer_size_buffer_index = MTL::kBufferSizeIndex;   // varsayılan 25 sabit arabellek aralığına (16..27) düşerdi
                comp.set_msl_options(mo);
                spirv_cross::CompilerGLSL::Options go = comp.get_common_options();
                go.vertex.flip_vert_y = true;          // GL satır düzeni korunur (görüntüler GL yönünde tutulur)
                go.vertex.fixup_clipspace = true;      // z: [-w,w] -> [0,w]
                comp.set_common_options(go);
                // Sabit arabellekler köşe öznitelik arabelleklerinden (0..15) sonra
                spirv_cross::ShaderResources res = comp.get_shader_resources();
                for (const auto& ub : res.uniform_buffers)
                {
                    const uint32_t b = comp.get_decoration(ub.id, spv::DecorationBinding);
                    comp.set_decoration(ub.id, spv::DecorationBinding, MTL::kConstantBufferBase + b);
                }
                // Depolama görüntüleri -> doku dizini, depolama blokları -> arabellek dizini (ad eşleşmesiyle; GLReflect'in adlarıyla aynı)
                for (const auto& si : res.storage_images)
                    for (const auto& ib : rp.Images)
                        if (ib.Name == si.name)
                            comp.set_decoration(si.id, spv::DecorationBinding, ib.Binding);
                for (const auto& sb : res.storage_buffers)
                {
                    std::string blockName = comp.get_name(sb.base_type_id);
                    if (blockName.empty())
                        blockName = sb.name;
                    for (const auto& blk : rp.StorageBlocks)
                        if (blk.Name == blockName)
                            comp.set_decoration(sb.id, spv::DecorationBinding, blk.Binding);
                }
                // 316: hızlandırma yapısı -> arabellek dizini (LinkProgram depolama bloklarından sonra atar)
                for (const auto& as : res.acceleration_structures)
                    if (rp.AccelStructRegister >= 0)
                        comp.set_decoration(as.id, spv::DecorationBinding, (uint32_t)rp.AccelStructRegister);
                msl = comp.compile();
                if (needsBufferSizes)
                    *needsBufferSizes = comp.needs_buffer_size_buffer();
                for (const auto& ep : comp.get_entry_points_and_stages())
                    entry = comp.get_cleansed_entry_point_name(ep.name, ep.execution_model);
            }
            catch (const std::exception& e)
            {
                log = std::string("SPIRV-Cross MSL: ") + e.what();
                return nil;
            }
            static const char* s_Dump = std::getenv("CQ_MTLGL_DUMP");
            if (s_Dump && *s_Dump)
                std::ofstream(std::string(s_Dump) + "/prog" + std::to_string(programId) + (vertex ? ".vs.metal" : stage == 2 ? ".cs.metal" : ".fs.metal")) << msl;
            NSError* err = nil;
            MTLCompileOptions* opts = [MTLCompileOptions new];
            if (stage == 2 && rp.AccelStructRegister >= 0)
                if (@available(macOS 12.0, iOS 15.0, tvOS 15.0, *))
                    opts.languageVersion = MTLLanguageVersion2_4;
            if (stage == 2 && Core::Get().SupportsTextureAtomics())
                if (@available(macOS 14.0, iOS 17.0, tvOS 17.0, *))
                    opts.languageVersion = MTLLanguageVersion3_1;
            // Doku atomiği olmayan GPU'da SPIRV-Cross görüntü atomiklerini dokunun ardındaki bir arabellekle taklit eder (dokunun dizinindeki
            // ek 'device atomic_uint*' bağımsız değişkeni): motorun dokuları arabellek üstüne kurulmadığı için atomikler dokuya ulaşmaz, sonuç
            // sessizce yanlış olurdu (ve depolama bloklarının dizinleriyle çakışıyordu). Program bağlanmaz; CMAA 2 / FSR 2 bu GPU'da kullanılamaz
            if (msl.find("_atomic [[buffer(") != std::string::npos)
            {
                log = "[compute] image atomics need Metal texture atomics (Metal 3.1, Apple A14 / M1 or newer); not available on this GPU";
                return nil;
            }
            id<MTLLibrary> lib = [Core::Get().Device() newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()] options:opts error:&err];
            if (!lib)
            {
                log = std::string(vertex ? "[vertex] " : stage == 2 ? "[compute] " : "[fragment] ") + (err ? [[err localizedDescription] UTF8String] : "MSL compile failed");
                return nil;
            }
            id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
            if (!fn)
                log = "MSL entry point '" + entry + "' not found";
            return fn;
        }
    }

    void DestroyProgram(Program& p)
    {
        p.VS = nil;
        p.PS = nil;
        p.ComputePipeline = nil;
        p.NeedsBufferSizes = false;
        p.GlobalsBuffer = nil;
        p.GlobalsFrame = 0;
    }

    bool LinkProgram(Program& p)
    {
        auto& c = Ctx();
        DestroyProgram(p);
        p.Linked = false;
        p.Log.clear();

        std::vector<GLEmu::ShaderSource> shaders;
        for (uint32_t name : p.Shaders)
            if (auto it = c.ShaderObjects.find(name); it != c.ShaderObjects.end())
                shaders.push_back({ it->second->Type, &it->second->Source });
        GLEmu::ResourceLimits limits;
        limits.ConstantBuffers = MTL::kMaxConstantBuffers;
        limits.Resources = MTL::kMaxTextures;
        limits.Samplers = MTL::kMaxSamplers;
        limits.ApiName = "Metal";
        limits.UnorderedAccess = 16;
        GLEmu::ReflectedProgram rp;
        if (!GLEmu::ReflectProgram(shaders, limits, rp, p.Log))
            return false;
        // Metal dizinleri (307): depolama görüntüleri örneklenen dokulardan sonra [[texture(n)]]; depolama blokları hesaplamada [[buffer(0..)]],
        // çizimde köşe arabelleği yuvalarından sonra (sabit arabellekler 16'dan başlar)
        {
            uint32_t tex = rp.TextureRegisters;
            for (auto& ib : rp.Images)
                ib.Binding = tex++;
            uint32_t buf = rp.Compute ? 0u : (uint32_t)rp.Inputs.size();
            for (auto& blk : rp.StorageBlocks)
                blk.Binding = buf++;
            // 316: hızlandırma yapısı depolama bloklarından sonraki arabellek dizininde (GLReflect'in t# yazmacı Metal'de kullanılmaz)
            if (rp.AccelStructRegister >= 0)
                rp.AccelStructRegister = (int)buf++;
            if (tex > MTL::kMaxTextures || buf > MTL::kConstantBufferBase)
            {
                p.Log = "Metal: too many storage images / buffers (" + std::to_string(rp.Images.size()) + " / " + std::to_string(rp.StorageBlocks.size()) + ")";
                return false;
            }
        }
        const uint64_t programId = c.NextProgramId;
        if (rp.Compute)
        {
            id<MTLFunction> cs = CompileStage(std::move(rp.ComputeSpirv), 2, programId, rp, p.Log, &p.NeedsBufferSizes);
            if (!cs)
                return false;
            NSError* err = nil;
            p.ComputePipeline = [Core::Get().Device() newComputePipelineStateWithFunction:cs error:&err];
            if (!p.ComputePipeline)
            {
                p.Log = std::string("[compute] ") + (err ? [[err localizedDescription] UTF8String] : "compute pipeline creation failed");
                return false;
            }
        }
        else
        {
            bool vsSizes = false, psSizes = false;
            p.VS = CompileStage(std::move(rp.VertexSpirv), 0, programId, rp, p.Log, &vsSizes);
            if (!p.VS)
                return false;
            p.PS = CompileStage(std::move(rp.FragmentSpirv), 1, programId, rp, p.Log, &psSizes);
            p.NeedsBufferSizes = vsSizes || psSizes;
            if (!p.PS)
                return false;
        }
        p.IsCompute = rp.Compute;
        for (int k = 0; k < 3; ++k)
            p.LocalSize[k] = rp.LocalSize[k];
        p.Images = rp.Images;
        p.StorageBlocks = rp.StorageBlocks;
        p.AccelStructBuffer = rp.AccelStructRegister;

        p.Uniforms = std::move(rp.Uniforms);
        p.UniformByName = std::move(rp.UniformByName);
        p.Samplers = std::move(rp.Samplers);
        p.Blocks = std::move(rp.Blocks);
        p.Inputs = std::move(rp.Inputs);
        p.ColorOutputs = rp.ColorOutputs;
        p.ColorOutputMask = rp.ColorOutputMask;
        p.GlobalsRegister = rp.GlobalsRegister;
        p.GlobalsSize = rp.GlobalsSize;
        p.Globals = std::move(rp.Globals);
        p.GlobalsDirty = true;
        p.Id = c.NextProgramId++;
        p.Linked = true;
        if (!p.Label.empty())
            ProgramLabelGL(p);   // yeniden bağlamada yeni nesneler de adını alır
        return true;
    }

    // 323: Metal label (Xcode GPU yakalamasında görünür); çizim boru hattının adı oluşturulurken verilir (MtlGLDraw)
    void ProgramLabelGL(Program& p)
    {
        NSString* name = p.Label.empty() ? nil : [NSString stringWithUTF8String:p.Label.c_str()];
        if (p.VS)
            p.VS.label = name;
        if (p.PS)
            p.PS.label = name;
    }

    UniformSlot* FindUniform(Program& p, GLint location)
    {
        if (location < 0 || location >= (GLint)p.Uniforms.size())
            return nullptr;
        return &p.Uniforms[(size_t)location];
    }
}

#endif
