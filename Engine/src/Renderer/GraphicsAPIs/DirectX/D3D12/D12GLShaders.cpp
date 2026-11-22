#ifdef CQ_D3D12_ENABLED

// GLSL -> HLSL -> DXBC çevirisi ve yansıtma ortak katmandadır (DirectX/Common/DxShaderTranslate.cpp). D3D12'de
// gölgelendirici nesnesi yoktur: bayt kodu boru hattı durumu (PSO) oluşturulurken kullanılır.

#include "D12GL.h"
#include "Core/Logging/Log.h"

namespace Conqueror::D12GL
{
    void DestroyProgram(Program& p)
    {
        p.VS.clear();
        p.PS.clear();
        p.CS.clear();
        DeferRelease(p.ComputePSO);
        p.GlobalsAddress = 0;
        p.GlobalsFrame = 0;
        // Bu programın PSO'ları anahtarda program kimliği taşır; yeni bağlamada yeni kimlik alınır
    }

    bool LinkProgram(Program& p)
    {
        auto& c = Ctx();
        DestroyProgram(p);
        p.Linked = false;
        p.Log.clear();

        std::vector<DXCommon::ShaderSource> shaders;
        for (uint32_t name : p.Shaders)
            if (auto it = c.ShaderObjects.find(name); it != c.ShaderObjects.end())
                shaders.push_back({ it->second->Type, &it->second->Source });
        DXCommon::ShaderLimits limits;
        limits.ConstantBuffers = DX12::kRootCBVs;
        limits.Resources = DX12::kMaxSRVs;
        limits.Samplers = DX12::kMaxSamplers;
        limits.UnorderedAccess = DX12::kMaxUAVs;
        limits.ApiName = "Direct3D 12";
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
        p.VS = std::move(tp.VS);
        p.PS = std::move(tp.PS);
        p.CS = std::move(tp.CS);
        p.IsCompute = tp.Compute;
        for (int k = 0; k < 3; ++k)
            p.LocalSize[k] = tp.LocalSize[k];
        p.Images = std::move(tp.Images);
        p.StorageBlocks = std::move(tp.StorageBlocks);
        p.Id = c.NextProgramId++;
        if (p.IsCompute)
        {
            // Hesaplama PSO'su programla birlikte (durum anahtarı yok)
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = DX12::Core::Get().RootSignature();
            d.CS = { p.CS.data(), p.CS.size() };
            if (FAILED(DX12::Core::Get().Device()->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&p.ComputePSO)))
                p.ComputePSO = nullptr;
            p.Linked = p.ComputePSO != nullptr;
            if (!p.Linked)
                p.Log = "Direct3D 12 compute pipeline creation failed";
            else if (!p.Label.empty())
                ProgramLabelGL(p);
            return p.Linked;
        }
        p.Linked = !p.VS.empty() && !p.PS.empty();
        if (!p.Linked)
            p.Log = "Direct3D 12 shader compilation produced no bytecode";
        return p.Linked;
    }

    // 323: ID3D12Object::SetName (PIX / RenderDoc / hata katmanı); çizim PSO'ları oluşturulurken aynı adı alır (D12GLDraw)
    void NameD3D12Object(ID3D12Object* o, const std::string& label)
    {
        if (!o || label.empty())
            return;
        std::wstring w(label.begin(), label.end());
        o->SetName(w.c_str());
    }

    void ProgramLabelGL(Program& p) { NameD3D12Object(p.ComputePSO, p.Label); }

    UniformSlot* FindUniform(Program& p, GLint location)
    {
        if (location < 0 || location >= (GLint)p.Uniforms.size())
            return nullptr;
        return &p.Uniforms[(size_t)location];
    }
}

#endif
