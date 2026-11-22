#ifdef CQ_D3D11_ENABLED

#include "DxGL.h"
#include "Core/Logging/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Conqueror::DxGL
{
    using DX::Core;

    Context& Ctx()
    {
        static Context c;
        return c;
    }

    Texture* GetTexture(uint32_t name)
    {
        auto& c = Ctx();
        auto it = c.Textures.find(name);
        return it == c.Textures.end() ? nullptr : it->second.get();
    }

    Texture* GetRenderbuffer(uint32_t name)
    {
        auto& c = Ctx();
        auto it = c.Renderbuffers.find(name);
        return it == c.Renderbuffers.end() ? nullptr : it->second.get();
    }

    Buffer* GetBuffer(uint32_t name)
    {
        auto& c = Ctx();
        auto it = c.Buffers.find(name);
        return it == c.Buffers.end() ? nullptr : it->second.get();
    }

    Framebuffer* GetFramebuffer(uint32_t name)
    {
        auto& c = Ctx();
        if (name == 0)
            return &c.DefaultFB;
        auto it = c.Framebuffers.find(name);
        return it == c.Framebuffers.end() ? nullptr : it->second.get();
    }

    Program* GetProgram(uint32_t name)
    {
        auto& c = Ctx();
        auto it = c.Programs.find(name);
        return it == c.Programs.end() ? nullptr : it->second.get();
    }

    Texture* AttachmentTexture(const Attachment& a)
    {
        auto& c = Ctx();
        if (a.Texture == 0xFFFFFFF0u) return &c.BackColor;
        if (a.Texture == 0xFFFFFFF1u) return &c.BackDepth;
        if (a.Texture) return GetTexture(a.Texture);
        if (a.Renderbuffer) return GetRenderbuffer(a.Renderbuffer);
        return nullptr;
    }

    using DXCommon::FormatInfo;
    using DXCommon::FormatOf;
    using DXCommon::MaxLevels;

    namespace
    {
        UINT FormatSupport(DXGI_FORMAT f)
        {
            UINT support = 0;
            if (f != DXGI_FORMAT_UNKNOWN)
                Core::Get().Device()->CheckFormatSupport(f, &support);
            return support;
        }

        UINT Subresource(const Texture& t, int level, int layer) { return D3D11CalcSubresource((UINT)level, (UINT)layer, t.Levels); }
    }

    void DestroyTextureImage(Texture& t)
    {
        // Bağlı çıkış görünümleri serbest kalacak: birleştirici önbelleği geçersiz
        EndRendering();
        SafeRelease(t.SRV);
        for (auto& [k, v] : t.RTVs)
            if (v) v->Release();
        for (auto& [k, v] : t.DSVs)
            if (v) v->Release();
        t.RTVs.clear();
        t.DSVs.clear();
        for (auto& [k, v] : t.UAVs)
            if (v) v->Release();
        for (auto& [k, v] : t.LevelSRVs)
            if (v) v->Release();
        t.UAVs.clear();
        t.LevelSRVs.clear();
        t.StorageCapable = false;
        SafeRelease(t.Image);
        t.SRVLevels = 0;
        t.DefinedLevels = 0;
    }

    // glBindImageTexture (307): doku UNORDERED_ACCESS bayrağıyla yeniden oluşturulup içeriği kopyalanır (her hedefe baştan konmaz:
    // UAV bayrağı bazı GPU'larda renk sıkıştırmasını kapatır)
    bool EnsureStorageImage(Texture& t)
    {
        if (!t.Image)
            return false;
        if (t.StorageCapable)
            return true;
        if (t.Depth || t.Samples > 1 || !(FormatSupport(t.RTVFormat) & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
            return false;
        auto& core = Core::Get();
        D3D11_TEXTURE2D_DESC desc{};
        t.Image->GetDesc(&desc);
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        ID3D11Texture2D* img = nullptr;
        if (FAILED(core.Device()->CreateTexture2D(&desc, nullptr, &img)) || !img)
            return false;
        core.Context()->CopyResource(img, t.Image);
        const uint32_t defined = t.DefinedLevels;
        DestroyTextureImage(t);
        t.Image = img;
        t.DefinedLevels = defined;
        t.StorageCapable = true;
        return true;
    }

    ID3D11UnorderedAccessView* GetUAV(Texture& t, int level, int layer)
    {
        if (!t.Image || !t.StorageCapable)
            return nullptr;
        const uint32_t key = (uint32_t)level | ((uint32_t)layer << 8);
        if (auto it = t.UAVs.find(key); it != t.UAVs.end())
            return it->second;
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = t.RTVFormat;
        if (t.Layers > 1)
        {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MipSlice = (UINT)level;
            d.Texture2DArray.FirstArraySlice = (UINT)layer;
            d.Texture2DArray.ArraySize = 1;
        }
        else
        {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = (UINT)level;
        }
        ID3D11UnorderedAccessView* v = nullptr;
        if (FAILED(Core::Get().Device()->CreateUnorderedAccessView(t.Image, &d, &v)))
            v = nullptr;
        t.UAVs[key] = v;
        return v;
    }

    // Salt okunur depolama görüntüsü (imageLoad -> Texture2D.Load): tek düzeyli kaynak görünümü
    ID3D11ShaderResourceView* GetLevelSRV(Texture& t, int level, int layer)
    {
        if (!t.Image)
            return nullptr;
        const uint32_t key = (uint32_t)level | ((uint32_t)layer << 8);
        if (auto it = t.LevelSRVs.find(key); it != t.LevelSRVs.end())
            return it->second;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = t.SRVFormat;
        if (t.Layers > 1)
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MostDetailedMip = (UINT)level;
            d.Texture2DArray.MipLevels = 1;
            d.Texture2DArray.FirstArraySlice = (UINT)layer;
            d.Texture2DArray.ArraySize = 1;
        }
        else
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MostDetailedMip = (UINT)level;
            d.Texture2D.MipLevels = 1;
        }
        ID3D11ShaderResourceView* v = nullptr;
        if (FAILED(Core::Get().Device()->CreateShaderResourceView(t.Image, &d, &v)))
            v = nullptr;
        t.LevelSRVs[key] = v;
        return v;
    }

    bool AllocateTexture(Texture& t, GLenum target, GLenum internalFormat, uint32_t w, uint32_t h, uint32_t levels, uint32_t samples)
    {
        auto& core = Core::Get();
        DestroyTextureImage(t);
        const FormatInfo fi = FormatOf(internalFormat);
        t.Target = target;
        t.InternalFormat = internalFormat;
        t.Format = fi.Resource;
        t.SRVFormat = fi.SRV;
        t.RTVFormat = fi.RTV;
        t.DSVFormat = fi.DSV;
        t.Depth = fi.Depth;
        t.Stencil = fi.Stencil;
        t.Integer = fi.Integer;
        t.Width = std::max(1u, w);
        t.Height = std::max(1u, h);
        t.Levels = std::max(1u, std::min(levels, MaxLevels(t.Width, t.Height)));
        t.Layers = target == GL_TEXTURE_CUBE_MAP ? 6u : 1u;
        t.Samples = std::max(1u, samples);
        if (t.Samples > 1)
            t.Levels = 1;

        const UINT srvSupport = FormatSupport(fi.SRV);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = t.Width;
        desc.Height = t.Height;
        desc.MipLevels = t.Levels;
        desc.ArraySize = t.Layers;
        desc.Format = fi.Resource;
        desc.SampleDesc.Count = t.Samples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (srvSupport & (D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_LOAD))
            desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        if (fi.Depth)
            desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        else if (FormatSupport(fi.RTV) & D3D11_FORMAT_SUPPORT_RENDER_TARGET)
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (t.Layers == 6)
            desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        // Mip zinciri kendi küçültme geçişimizle üretilir (GenerateMipmaps): hedef olabilen her biçimde çalışır
        t.AutoMips = !fi.Depth && t.Levels > 1 && (desc.BindFlags & D3D11_BIND_RENDER_TARGET) && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE);
        if (FAILED(core.Device()->CreateTexture2D(&desc, nullptr, &t.Image)) || !t.Image)
        {
            CQ_CORE_ERROR("[DxGL] Cannot create {0}x{1} texture (format {2}, {3} samples)", t.Width, t.Height, (int)fi.Resource, t.Samples);
            t.Image = nullptr;
            return false;
        }
        // GL'de tanımsız; temizlemek tutarlı sonuç verir (VkGL ile aynı)
        auto* ctx = core.Context();
        for (uint32_t layer = 0; layer < t.Layers; ++layer)
        {
            if (fi.Depth)
            {
                if (ID3D11DepthStencilView* dsv = GetDSV(t, 0, (int)layer))
                    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            }
            else if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
            {
                const float zero[4] = { 0, 0, 0, 0 };
                if (ID3D11RenderTargetView* rtv = GetRTV(t, 0, (int)layer))
                    ctx->ClearRenderTargetView(rtv, zero);
            }
        }
        return true;
    }

    ID3D11ShaderResourceView* GetSRV(Texture& t)
    {
        if (!t.Image)
            return nullptr;
        int levels = (int)t.Levels;
        if (!t.Immutable && t.DefinedLevels)
        {
            levels = 0;
            while (levels < (int)t.Levels && (t.DefinedLevels & (1u << levels)))
                ++levels;
            levels = std::max(levels, 1);
        }
        levels = std::clamp(std::min(levels, t.MaxLevel + 1), 1, (int)t.Levels);
        if (t.SRV && t.SRVLevels == levels)
            return t.SRV;
        SafeRelease(t.SRV);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = t.SRVFormat;
        if (t.Samples > 1)
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        else if (t.Layers == 6)
        {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            vd.TextureCube.MipLevels = (UINT)levels;
        }
        else
        {
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = (UINT)levels;
        }
        if (FAILED(Core::Get().Device()->CreateShaderResourceView(t.Image, &vd, &t.SRV)))
            t.SRV = nullptr;
        t.SRVLevels = levels;
        return t.SRV;
    }

    ID3D11RenderTargetView* GetRTV(Texture& t, int level, int layer)
    {
        if (!t.Image || t.Depth)
            return nullptr;
        const uint32_t key = (uint32_t)level | ((uint32_t)layer << 8);
        if (auto it = t.RTVs.find(key); it != t.RTVs.end())
            return it->second;
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        vd.Format = t.RTVFormat;
        if (t.Samples > 1)
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        else if (t.Layers > 1)
        {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = (UINT)level;
            vd.Texture2DArray.FirstArraySlice = (UINT)layer;
            vd.Texture2DArray.ArraySize = 1;
        }
        else
        {
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = (UINT)level;
        }
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(Core::Get().Device()->CreateRenderTargetView(t.Image, &vd, &rtv)))
            rtv = nullptr;
        t.RTVs[key] = rtv;
        return rtv;
    }

    ID3D11DepthStencilView* GetDSV(Texture& t, int level, int layer)
    {
        if (!t.Image || !t.Depth)
            return nullptr;
        const uint32_t key = (uint32_t)level | ((uint32_t)layer << 8);
        if (auto it = t.DSVs.find(key); it != t.DSVs.end())
            return it->second;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        vd.Format = t.DSVFormat;
        if (t.Samples > 1)
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        else if (t.Layers > 1)
        {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipSlice = (UINT)level;
            vd.Texture2DArray.FirstArraySlice = (UINT)layer;
            vd.Texture2DArray.ArraySize = 1;
        }
        else
        {
            vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = (UINT)level;
        }
        ID3D11DepthStencilView* dsv = nullptr;
        if (FAILED(Core::Get().Device()->CreateDepthStencilView(t.Image, &vd, &dsv)))
            dsv = nullptr;
        t.DSVs[key] = dsv;
        return dsv;
    }

    ID3D11SamplerState* GetSampler(const Texture& p, bool integer, bool comparison)
    {
        auto& c = Ctx();
        const DXCommon::SamplerDesc d = DXCommon::MakeSamplerDesc(p, integer, comparison);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = (D3D11_FILTER)d.Filter;
        sd.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)d.AddressU;
        sd.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)d.AddressV;
        sd.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)d.AddressW;
        sd.MipLODBias = d.MipLODBias;
        sd.MaxAnisotropy = d.MaxAnisotropy;
        sd.ComparisonFunc = (D3D11_COMPARISON_FUNC)d.ComparisonFunc;
        for (int i = 0; i < 4; ++i)
            sd.BorderColor[i] = d.BorderColor[i];
        sd.MinLOD = d.MinLOD;
        sd.MaxLOD = d.MaxLOD;
        const uint64_t key = d.Key;
        if (auto it = c.SamplerCache.find(key); it != c.SamplerCache.end())
            return it->second;
        ID3D11SamplerState* s = nullptr;
        if (FAILED(Core::Get().Device()->CreateSamplerState(&sd, &s)))
            s = nullptr;
        c.SamplerCache[key] = s;
        return s;
    }

    void UploadTexture(Texture& t, int level, int layer, int x, int y, int w, int h, GLenum format, GLenum type, const void* data)
    {
        if (!t.Image || !data || w <= 0 || h <= 0)
            return;
        if (t.Depth || t.Samples > 1)
        {
            CQ_CORE_WARN("[DxGL] Uploads to depth / multisample textures are not supported");
            return;
        }
        auto& c = Ctx();
        static thread_local std::vector<uint8_t> staging;
        size_t uploadPitch = 0;
        const uint8_t* upload = DXCommon::ConvertUpload(t.Format, w, h, format, type, data, c.S.UnpackRowLength, c.S.UnpackAlignment, staging, uploadPitch);
        D3D11_BOX box{ (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
        Core::Get().Context()->UpdateSubresource(t.Image, Subresource(t, level, layer), &box, upload, (UINT)uploadPitch, 0);
    }

    void ReadImage(Texture& t, int level, int layer, int x, int y, int w, int h, GLenum format, GLenum type, void* out, int packAlignment)
    {
        if (!t.Image || !out || w <= 0 || h <= 0)
            return;
        auto& core = Core::Get();
        auto* ctx = core.Context();
        ID3D11Texture2D* source = t.Image;
        UINT sourceSub = Subresource(t, level, layer);
        ID3D11Texture2D* resolved = nullptr;
        const uint32_t lw = std::max(1u, t.Width >> level), lh = std::max(1u, t.Height >> level);
        if (t.Samples > 1)
        {
            if (t.Depth)
            {
                CQ_CORE_WARN("[DxGL] Reading a multisample depth texture is not supported");
                return;
            }
            D3D11_TEXTURE2D_DESC rd{};
            rd.Width = lw;
            rd.Height = lh;
            rd.MipLevels = 1;
            rd.ArraySize = 1;
            rd.Format = t.Format;
            rd.SampleDesc.Count = 1;
            rd.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(core.Device()->CreateTexture2D(&rd, nullptr, &resolved)))
                return;
            ctx->ResolveSubresource(resolved, 0, t.Image, 0, t.RTVFormat);
            source = resolved;
            sourceSub = 0;
        }
        // Derinlik kaynakları yalnızca tüm alt kaynak olarak kopyalanabilir
        const bool whole = t.Depth;
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = whole ? lw : (UINT)w;
        sd.Height = whole ? lh : (UINT)h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = t.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(core.Device()->CreateTexture2D(&sd, nullptr, &staging)))
        {
            SafeRelease(resolved);
            return;
        }
        if (whole)
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, sourceSub, nullptr);
        else
        {
            D3D11_BOX box{ (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, sourceSub, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            staging->Release();
            SafeRelease(resolved);
            return;
        }
        const int ox = whole ? x : 0, oy = whole ? y : 0;
        const uint8_t* origin = static_cast<const uint8_t*>(mapped.pData) + (size_t)mapped.RowPitch * (size_t)oy + (size_t)ox * DXCommon::TexelBytes(t.Format);
        DXCommon::ConvertReadback(t.Format, origin, mapped.RowPitch, w, h, format, type, out, packAlignment);
        ctx->Unmap(staging, 0);
        staging->Release();
        SafeRelease(resolved);
    }

    bool GrowLevels(Texture& t, uint32_t levels)
    {
        levels = std::min(levels, MaxLevels(t.Width, t.Height));
        if (!t.Image || levels <= t.Levels || t.Samples > 1)
            return t.Image != nullptr;
        ID3D11Texture2D* old = t.Image;
        old->AddRef();   // AllocateTexture eskiyi bırakır; kopya için tut
        const uint32_t oldLevels = t.Levels;
        const uint32_t defined = t.DefinedLevels;
        if (!AllocateTexture(t, t.Target, t.InternalFormat, t.Width, t.Height, levels, 1))
        {
            old->Release();
            return false;
        }
        auto* ctx = Core::Get().Context();
        for (uint32_t l = 0; l < oldLevels; ++l)
        {
            if (!(defined & (1u << l)))
                continue;
            for (uint32_t layer = 0; layer < t.Layers; ++layer)
                ctx->CopySubresourceRegion(t.Image, Subresource(t, (int)l, (int)layer), 0, 0, 0, old, D3D11CalcSubresource(l, layer, oldLevels), nullptr);
        }
        old->Release();
        t.DefinedLevels = defined;
        return true;
    }

    void ClearTexture(Texture& t, int level, GLenum format, GLenum type, const void* data)
    {
        if (!t.Image || level < 0 || level >= (int)t.Levels)
            return;
        auto* ctx = Core::Get().Context();
        float color[4], depth = 1.0f;
        uint8_t stencil = 0;
        DXCommon::ParseClearValue(t.Depth, t.Integer, format, type, data, color, depth, stencil);
        for (uint32_t layer = 0; layer < t.Layers; ++layer)
        {
            if (t.Depth)
            {
                if (ID3D11DepthStencilView* dsv = GetDSV(t, level, (int)layer))
                    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | (t.Stencil ? D3D11_CLEAR_STENCIL : 0), depth, stencil);
            }
            else if (ID3D11RenderTargetView* rtv = GetRTV(t, level, (int)layer))
                ctx->ClearRenderTargetView(rtv, color);
        }
        t.DefinedLevels |= 1u << level;
    }

    // ── Arabellekler ──
    namespace
    {
        bool EnsureDynamic(ID3D11Buffer*& buffer, size_t& capacity, size_t size, UINT bind)
        {
            if (buffer && capacity >= size)
                return true;
            SafeRelease(buffer);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (UINT)size;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = bind;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(Core::Get().Device()->CreateBuffer(&bd, nullptr, &buffer)))
            {
                buffer = nullptr;
                capacity = 0;
                return false;
            }
            capacity = size;
            return true;
        }

        void Refill(ID3D11Buffer* buffer, const Buffer& b, size_t capacity)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            auto* ctx = Core::Get().Context();
            if (FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                return;
            const size_t bytes = std::min({ b.Shadow.size(), b.HighWater, capacity });
            if (bytes)
                std::memcpy(m.pData, b.Shadow.data(), bytes);
            ctx->Unmap(buffer, 0);
        }
    }

    // GPU'ya ait arabellek (307): DEFAULT, UAV + SRV + köşe + indeks bağlamalı, ham görünümler ve dolaylı argümanlar. Oluşturulurken gölgenin
    // tamamı, sonra yalnız CPU'nun yazdığı aralık (komut sırasıyla UpdateSubresource; GPU'nun yazdığı kısım korunur)
    ID3D11Buffer* StorageBuffer(Buffer& b)
    {
        auto& core = Core::Get();
        const size_t size = (std::max<size_t>(b.Shadow.size(), 16) + 15) & ~(size_t)15;
        if (!b.Storage || b.StorageSize < size)
        {
            for (auto& [k, v] : b.StorageUAVs)
                if (v) v->Release();
            for (auto& [k, v] : b.StorageSRVs)
                if (v) v->Release();
            b.StorageUAVs.clear();
            b.StorageSRVs.clear();
            SafeRelease(b.Storage);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (UINT)size;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
            std::vector<uint8_t> init(size, 0);
            std::copy(b.Shadow.begin(), b.Shadow.end(), init.begin());
            D3D11_SUBRESOURCE_DATA sd{ init.data(), 0, 0 };
            if (FAILED(core.Device()->CreateBuffer(&bd, &sd, &b.Storage)))
            {
                CQ_CORE_ERROR("[DxGL] Cannot create a {0} byte storage buffer", size);
                b.Storage = nullptr;
                return nullptr;
            }
            b.StorageSize = size;
        }
        else if (b.Dirty && b.DirtyBegin < b.DirtyEnd && b.DirtyBegin < b.Shadow.size())
        {
            const size_t from = b.DirtyBegin, to = std::min(b.DirtyEnd, b.Shadow.size());
            D3D11_BOX box{ (UINT)from, 0, 0, (UINT)to, 1, 1 };
            core.Context()->UpdateSubresource(b.Storage, 0, &box, b.Shadow.data() + from, 0, 0);
        }
        b.Dirty = false;
        b.DirtyBegin = SIZE_MAX;
        b.DirtyEnd = 0;
        return b.Storage;
    }

    ID3D11UnorderedAccessView* StorageUAV(Buffer& b, size_t offset, size_t size)
    {
        if (!StorageBuffer(b))
            return nullptr;
        offset &= ~(size_t)3;
        size = std::min(size, b.StorageSize - std::min(offset, b.StorageSize)) & ~(size_t)3;
        if (size == 0)
            return nullptr;
        const uint64_t key = ((uint64_t)offset << 32) | (uint64_t)size;
        if (auto it = b.StorageUAVs.find(key); it != b.StorageUAVs.end())
            return it->second;
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = (UINT)(offset / 4);
        d.Buffer.NumElements = (UINT)(size / 4);
        d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ID3D11UnorderedAccessView* v = nullptr;
        if (FAILED(Core::Get().Device()->CreateUnorderedAccessView(b.Storage, &d, &v)))
            v = nullptr;
        b.StorageUAVs[key] = v;
        return v;
    }

    ID3D11ShaderResourceView* StorageSRV(Buffer& b, size_t offset, size_t size)
    {
        if (!StorageBuffer(b))
            return nullptr;
        offset &= ~(size_t)3;
        size = std::min(size, b.StorageSize - std::min(offset, b.StorageSize)) & ~(size_t)3;
        if (size == 0)
            return nullptr;
        const uint64_t key = ((uint64_t)offset << 32) | (uint64_t)size;
        if (auto it = b.StorageSRVs.find(key); it != b.StorageSRVs.end())
            return it->second;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx.FirstElement = (UINT)(offset / 4);
        d.BufferEx.NumElements = (UINT)(size / 4);
        d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ID3D11ShaderResourceView* v = nullptr;
        if (FAILED(Core::Get().Device()->CreateShaderResourceView(b.Storage, &d, &v)))
            v = nullptr;
        b.StorageSRVs[key] = v;
        return v;
    }

    ID3D11Buffer* GeometryBuffer(Buffer& b)
    {
        if (b.GpuOwned)
            return StorageBuffer(b);
        if (b.Dirty)
        {
            ++b.Version;
            b.Dirty = false;
        }
        const size_t size = (std::max<size_t>(b.Shadow.size(), 16) + 15) & ~(size_t)15;
        const bool created = !b.Geometry || b.GeometrySize < size;
        if (!EnsureDynamic(b.Geometry, b.GeometrySize, size, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER))
            return nullptr;
        if (created || b.GeometryVersion != b.Version)
        {
            Refill(b.Geometry, b, b.GeometrySize);
            b.GeometryVersion = b.Version;
        }
        return b.Geometry;
    }

    ID3D11Buffer* ConstantBuffer(Buffer& b)
    {
        if (b.GpuOwned)
        {
            // Sabit arabellek bayrağı UAV ile birleşemez: GPU'daki içerik ayrı bir DEFAULT sabit arabelleğe kopyalanır
            ID3D11Buffer* src = StorageBuffer(b);
            const size_t size = std::min<size_t>(b.StorageSize, 65536);
            if (!src)
                return nullptr;
            if (!b.StorageConstant || b.StorageConstantSize < size)
            {
                SafeRelease(b.StorageConstant);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = (UINT)size;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                if (FAILED(Core::Get().Device()->CreateBuffer(&bd, nullptr, &b.StorageConstant)))
                    return nullptr;
                b.StorageConstantSize = size;
            }
            D3D11_BOX box{ 0, 0, 0, (UINT)size, 1, 1 };
            Core::Get().Context()->CopySubresourceRegion(b.StorageConstant, 0, 0, 0, 0, src, 0, &box);
            return b.StorageConstant;
        }
        if (b.Dirty)
        {
            ++b.Version;
            b.Dirty = false;
        }
        // D3D11 sabit arabellek sınırı 64 KB
        const size_t size = std::min<size_t>((std::max<size_t>(b.Shadow.size(), 16) + 15) & ~(size_t)15, 65536);
        const bool created = !b.Constant || b.ConstantSize < size;
        if (!EnsureDynamic(b.Constant, b.ConstantSize, size, D3D11_BIND_CONSTANT_BUFFER))
            return nullptr;
        if (created || b.ConstantVersion != b.Version)
        {
            Refill(b.Constant, b, b.ConstantSize);
            b.ConstantVersion = b.Version;
        }
        return b.Constant;
    }
}

#endif
