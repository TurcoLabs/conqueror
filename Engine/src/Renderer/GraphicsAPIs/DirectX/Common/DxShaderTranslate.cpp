#if defined(CQ_D3D11_ENABLED) || defined(CQ_D3D12_ENABLED)

// Yansıtma ortak (GLEmu/GLReflect.cpp); burada SPIR-V -> HLSL (SPIRV-Cross, SM 5.0, y çevirme + derinlik [0,1]) ->
// DXBC (d3dcompiler_47). D3D11 ve D3D12 aynı DXBC'yi kullanır.

#include "DxShared.h"
#include "Core/Logging/Log.h"

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <spirv_hlsl.hpp>

#include <cstdlib>
#include <fstream>
#include <regex>

namespace Conqueror::DXCommon
{
    namespace
    {
        // Bazı HLSL derleyicileri (Wine/vkd3d) isnan/isinf içsel işlevlerini tanımaz: bit deseniyle birebir eşdeğeri
        std::string PortableHLSL(const std::string& hlsl)
        {
            static const std::regex s_IsNan(R"(\bisnan\s*\()"), s_IsInf(R"(\bisinf\s*\()");
            const bool nan = std::regex_search(hlsl, s_IsNan), inf = std::regex_search(hlsl, s_IsInf);
            if (!nan && !inf)
                return hlsl;
            std::string helpers;
            const char* types[][2] = { { "float", "bool" }, { "float2", "bool2" }, { "float3", "bool3" }, { "float4", "bool4" } };
            for (const auto& t : types)
            {
                if (nan)
                    helpers += std::string(t[1]) + " cq_isnan(" + t[0] + " x) { return (asuint(x) & 0x7FFFFFFFu) > 0x7F800000u; }\n";
                if (inf)
                    helpers += std::string(t[1]) + " cq_isinf(" + t[0] + " x) { return (asuint(x) & 0x7FFFFFFFu) == 0x7F800000u; }\n";
            }
            std::string out = std::regex_replace(hlsl, s_IsNan, "cq_isnan(");
            out = std::regex_replace(out, s_IsInf, "cq_isinf(");
            return helpers + out;
        }

    }

    namespace
    {
        // 316: DXIL derleyicisi (DirectXShaderCompiler, dxcompiler.dll + imzalama için dxil.dll) çalışma anında açılır: satır içi ışın
        // sorgusu (RayQuery, DXR 1.1) Shader Model 6.5 ister, d3dcompiler_47 (FXC) en çok 5.1 derler
        DxcCreateInstanceProc DxcCreate()
        {
            static const DxcCreateInstanceProc s_Create = []() -> DxcCreateInstanceProc {
                HMODULE m = LoadLibraryA("dxcompiler.dll");
                return m ? reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(m, "DxcCreateInstance"))) : nullptr;
            }();
            return s_Create;
        }
    }

    // dxil.dll de gerekir: DXC derlediği DXIL'i onunla imzalar (imzasız DXIL'den D3D12 boru hattı oluşturmaz)
    bool HasDXC()
    {
        static const bool s_Dxil = LoadLibraryA("dxil.dll") != nullptr;
        return s_Dxil && DxcCreate() != nullptr;
    }

    bool CompileDXC(const std::string& source, const wchar_t* target, std::vector<uint8_t>& out, std::string& errors)
    {
        const DxcCreateInstanceProc create = DxcCreate();
        if (!create)
        {
            errors = "dxcompiler.dll is not available";
            return false;
        }
        IDxcCompiler3* compiler = nullptr;
        if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&compiler))) || !compiler)
        {
            errors = "DxcCreateInstance(CLSID_DxcCompiler) failed";
            return false;
        }
        DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
        const wchar_t* args[] = { L"-E", L"main", L"-T", target, L"-O3", L"-HV", L"2021" };
        IDxcResult* result = nullptr;
        const HRESULT hr = compiler->Compile(&buffer, args, (UINT32)(sizeof(args) / sizeof(args[0])), nullptr, __uuidof(IDxcResult), reinterpret_cast<void**>(&result));
        compiler->Release();
        if (FAILED(hr) || !result)
        {
            errors = "DXC compile call failed";
            return false;
        }
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8* err = nullptr;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&err), nullptr)) && err)
        {
            if (err->GetStringLength())
                errors.assign(err->GetStringPointer(), err->GetStringLength());
            err->Release();
        }
        IDxcBlob* object = nullptr;
        if (SUCCEEDED(status))
            result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object), nullptr);
        result->Release();
        if (FAILED(status) || !object || object->GetBufferSize() == 0)
        {
            if (object)
                object->Release();
            if (errors.empty())
                errors = "DXC produced no shader object";
            return false;
        }
        out.assign((const uint8_t*)object->GetBufferPointer(), (const uint8_t*)object->GetBufferPointer() + object->GetBufferSize());
        object->Release();
        return true;
    }

    bool CompileHLSL(const std::string& source, const char* name, const char* entry, const char* target, std::vector<uint8_t>& out, std::string& errors)
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &blob, &err);
        if (err)
        {
            errors.assign((const char*)err->GetBufferPointer(), err->GetBufferSize());
            err->Release();
        }
        if (FAILED(hr) || !blob)
        {
            if (errors.empty())
                errors = "D3DCompile failed";
            if (blob)
                blob->Release();
            return false;
        }
        out.assign((const uint8_t*)blob->GetBufferPointer(), (const uint8_t*)blob->GetBufferPointer() + blob->GetBufferSize());
        blob->Release();
        return true;
    }

    bool TranslateProgram(const std::vector<ShaderSource>& shaders, uint64_t id, const ShaderLimits& limits, TranslatedProgram& p, std::string& log)
    {
        p = TranslatedProgram{};
        if (!GLEmu::ReflectProgram(shaders, limits, p, log))
            return false;
        static const char* s_Dump = std::getenv("CQ_DXGL_DUMP");
        if (p.Compute)
        {
            // Hesaplama (307): salt okunur depolama görüntüleri Texture2D (t#; UAV okuması D3D11'de yalnız R32 biçimlerinde garanti),
            // yazılabilirler RWTexture2D (u#); depolama blokları ByteAddressBuffer / RWByteAddressBuffer
            // 316: hızlandırma yapısı kullanan (GL_EXT_ray_query) program satır içi ışın sorgusuyla SM 6.5'e (DXC, DXIL)
            const bool rayQuery = p.AccelStructRegister >= 0;
            std::string hlsl;
            try
            {
                spirv_cross::CompilerHLSL hc(p.ComputeSpirv);
                spirv_cross::CompilerHLSL::Options ho;
                ho.shader_model = rayQuery ? 65 : 50;
                ho.nonwritable_uav_texture_as_srv = true;
                hc.set_hlsl_options(ho);
                hlsl = rayQuery ? hc.compile() : PortableHLSL(hc.compile());
            }
            catch (const std::exception& e)
            {
                log = std::string("SPIRV-Cross HLSL: ") + e.what();
                return false;
            }
            if (s_Dump && *s_Dump)
                std::ofstream(std::string(s_Dump) + "/prog" + std::to_string(id) + ".cs.hlsl") << hlsl;
            std::string err;
            if (rayQuery ? !CompileDXC(hlsl, L"cs_6_5", p.CS, err) : !CompileHLSL(hlsl, "dxgl_cs", "main", "cs_5_0", p.CS, err))
            {
                log = std::string("[compute] ") + err;
                return false;
            }
            return true;
        }
        for (int stage = 0; stage < 2; ++stage)
        {
            const bool vs = stage == 0;
            std::string hlsl;
            try
            {
                spirv_cross::CompilerHLSL hc(vs ? p.VertexSpirv : p.FragmentSpirv);
                spirv_cross::CompilerHLSL::Options ho;
                ho.shader_model = 50;
                ho.flatten_matrix_vertex_input_semantics = true;
                ho.nonwritable_uav_texture_as_srv = true;   // salt okunur depolama görüntüsü t# (GLReflect yazmaç düzeni)
                hc.set_hlsl_options(ho);
                spirv_cross::CompilerGLSL::Options go = hc.get_common_options();
                go.vertex.flip_vert_y = true;          // GL satır düzeni korunur (görüntüler GL yönünde tutulur)
                go.vertex.fixup_clipspace = true;      // z: [-w,w] -> [0,w]
                hc.set_common_options(go);
                hlsl = PortableHLSL(hc.compile());
            }
            catch (const std::exception& e)
            {
                log = std::string("SPIRV-Cross HLSL: ") + e.what();
                return false;
            }
            if (s_Dump && *s_Dump)
                std::ofstream(std::string(s_Dump) + "/prog" + std::to_string(id) + (vs ? ".vs.hlsl" : ".ps.hlsl")) << hlsl;
            std::string err;
            if (!CompileHLSL(hlsl, vs ? "dxgl_vs" : "dxgl_ps", "main", vs ? "vs_5_0" : "ps_5_0", vs ? p.VS : p.PS, err))
            {
                log = std::string(vs ? "[vertex] " : "[fragment] ") + err;
                return false;
            }
        }
        return true;
    }
}

#endif
