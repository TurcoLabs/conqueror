#ifdef CQ_D3D11_ENABLED

// GLSL -> HLSL -> DXBC çevirisi ve yansıtma ortak katmandadır (DirectX/Common/DxShaderTranslate.cpp);
// burada yalnızca D3D11 gölgelendirici nesneleri ve globaller sabit arabelleği oluşturulur.

#include "DxGL.h"
#include "Core/Logging/Log.h"

namespace Conqueror::DxGL
{
    using DX::Core;

    void DestroyProgram(Program& p)
    {
        SafeRelease(p.VS);
        SafeRelease(p.PS);
        SafeRelease(p.CS);
        SafeRelease(p.GlobalsBuffer);
        for (auto& [k, l] : p.InputLayouts)
            if (l) l->Release();
        p.InputLayouts.clear();
        p.VSBytecode.clear();
    }

    bool LinkProgram(Program& p)
    {
        auto& c = Ctx();
        auto& core = Core::Get();
        DestroyProgram(p);
        p.Linked = false;
        p.Log.clear();

        std::vector<DXCommon::ShaderSource> shaders;
        for (uint32_t name : p.Shaders)
            if (auto it = c.ShaderObjects.find(name); it != c.ShaderObjects.end())
                shaders.push_back({ it->second->Type, &it->second->Source });
        DXCommon::ShaderLimits limits;
        limits.ConstantBuffers = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
        limits.Resources = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        limits.Samplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        limits.UnorderedAccess = D3D11_PS_CS_UAV_REGISTER_COUNT;
        limits.ApiName = "Direct3D 11";
        DXCommon::TranslatedProgram tp;
        if (!DXCommon::TranslateProgram(shaders, c.NextProgramId, limits, tp, p.Log))
            return false;

        p.Uniforms = std::move(tp.Uniforms);
        p.UniformByName = std::move(tp.UniformByName);
        p.Samplers = std::move(tp.Samplers);
        p.Blocks = std::move(tp.Blocks);
        p.Inputs = std::move(tp.Inputs);
        p.ColorOutputs = tp.ColorOutputs;
        p.ColorOutputMask = tp.ColorOutputMask;
        p.GlobalsRegister = tp.GlobalsRegister;
        p.GlobalsSize = tp.GlobalsSize;
        p.Globals = std::move(tp.Globals);
        p.GlobalsDirty = true;
        p.IsCompute = tp.Compute;
        for (int k = 0; k < 3; ++k)
            p.LocalSize[k] = tp.LocalSize[k];
        p.Images = std::move(tp.Images);
        p.StorageBlocks = std::move(tp.StorageBlocks);

        if (p.IsCompute)
            core.Device()->CreateComputeShader(tp.CS.data(), tp.CS.size(), nullptr, &p.CS);
        else
        {
            core.Device()->CreateVertexShader(tp.VS.data(), tp.VS.size(), nullptr, &p.VS);
            core.Device()->CreatePixelShader(tp.PS.data(), tp.PS.size(), nullptr, &p.PS);
        }
        p.VSBytecode = std::move(tp.VS);
        if (p.GlobalsRegister >= 0)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (UINT)p.Globals.size();
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            core.Device()->CreateBuffer(&bd, nullptr, &p.GlobalsBuffer);
        }
        p.Id = c.NextProgramId++;
        p.Linked = (p.IsCompute ? p.CS != nullptr : (p.VS && p.PS)) && (p.GlobalsRegister < 0 || p.GlobalsBuffer);
        if (!p.Linked)
            p.Log = "Direct3D 11 shader object creation failed";
        else if (!p.Label.empty())
            ProgramLabelGL(p);   // yeniden bağlamada yeni nesneler de adını alır
        return p.Linked;
    }

    // 323: hata ayıklama adı (WKPDID_D3DDebugObjectName: PIX, RenderDoc ve D3D11 hata katmanı iletilerinde görünür)
    void ProgramLabelGL(Program& p)
    {
        static const GUID kDebugObjectName = { 0x429b8c22, 0x9188, 0x4b0c, { 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 } };
        const UINT size = (UINT)p.Label.size();
        auto name = [&](ID3D11DeviceChild* o) {
            if (o)
                o->SetPrivateData(kDebugObjectName, size, size ? p.Label.data() : nullptr);
        };
        name(p.VS);
        name(p.PS);
        name(p.CS);
        name(p.GlobalsBuffer);
    }

    UniformSlot* FindUniform(Program& p, GLint location)
    {
        if (location < 0 || location >= (GLint)p.Uniforms.size())
            return nullptr;
        return &p.Uniforms[(size_t)location];
    }
}

#endif
