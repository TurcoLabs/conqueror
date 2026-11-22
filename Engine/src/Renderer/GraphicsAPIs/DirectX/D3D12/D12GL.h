#pragma once

#ifdef CQ_D3D12_ENABLED

// GL -> Direct3D 12 çeviri katmanı. VkGL ve DxGL ile aynı ön yüzü (GLEmu/GLFrontEnd.inl) kullanır.
//
// Temel kurallar (DxGL ile aynı):
//  * Görüntüler GL yönünde tutulur; köşe gölgelendiricisi y'yi çevirir, z'yi [0,w]'ya taşır. Sunumda dikey çevrilir.
//  * GL_CCW ön yüz => FrontCounterClockwise = FALSE.
// D3D12'ye özgü:
//  * Tek kök imzası (D3D12Core.h): b0..b11 kök CBV, t0..t63 ve s0..s15 tabloları.
//  * Kaynak durumları kaynak başına izlenir; geçişler çizimden hemen önce kaydedilir.
//  * Arabellekler GL anlamını korur: güncelleme kayıtlı çizimleri etkilemez. Sık güncellenen veri yükleme halkasından
//    (yalnızca bu kare geçerli), seyrek güncellenen veri kalıcı varsayılan yığın arabelleğine komut listesinde kopyalanır.

#include "D3D12Core.h"
#include "../../GLEmu/GLEmuCommon.h"
#include "../Common/DxShared.h"

#include <glad/glad.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Conqueror::D12GL
{
    using DX12::CpuDescriptor;

    struct Texture
    {
        GLenum Target = 0;
        GLenum InternalFormat = 0;
        DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;        // kaynak (derinlikte typeless)
        DXGI_FORMAT SRVFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT RTVFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;
        bool Depth = false, Stencil = false;
        uint32_t Width = 0, Height = 0, Levels = 0, Layers = 1, Samples = 1;
        bool Immutable = false;
        bool Integer = false;
        bool Renderable = false;
        bool Sampleable = false;
        uint32_t DefinedLevels = 0;
        ID3D12Resource* Image = nullptr;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        CpuDescriptor SRV;
        int SRVLevels = 0;
        std::unordered_map<uint32_t, CpuDescriptor> RTVs;   // düzey | katman << 8
        std::unordered_map<uint32_t, CpuDescriptor> DSVs;
        // Hesaplama (307): depolama görüntüsü olarak bağlanınca ALLOW_UNORDERED_ACCESS ile yeniden oluşturulur
        bool StorageCapable = false;
        std::unordered_map<uint32_t, CpuDescriptor> UAVs;        // düzey | katman << 8
        std::unordered_map<uint32_t, CpuDescriptor> LevelSRVs;
        // Örnekleyici parametreleri (ön yüz yazar)
        GLenum MinFilter = GL_NEAREST_MIPMAP_LINEAR, MagFilter = GL_LINEAR;
        GLenum WrapS = GL_REPEAT, WrapT = GL_REPEAT, WrapR = GL_REPEAT;
        GLenum CompareMode = GL_NONE, CompareFunc = GL_LEQUAL;
        float MaxAnisotropy = 1.0f, MinLod = -1000.0f, MaxLod = 1000.0f, LodBias = 0.0f;
        int BaseLevel = 0, MaxLevel = 1000;
        float BorderColor[4] = { 0, 0, 0, 0 };
    };

    struct Buffer
    {
        std::vector<uint8_t> Shadow;
        size_t DirtyBegin = SIZE_MAX, DirtyEnd = 0;  // CPU'nun yazdığı aralık (GPU'ya ait arabellekte yalnız bu yüklenir, 307)
        GLenum Usage = GL_STATIC_DRAW;
        bool Dirty = true;
        size_t HighWater = 0;
        uint64_t Version = 1;
        // GPU'daki geçerli sürüm
        uint64_t GpuVersion = 0;
        D3D12_GPU_VIRTUAL_ADDRESS GpuAddress = 0;
        uint64_t GpuSize = 0;
        bool GpuIsRing = false;
        uint64_t RingFrame = 0;
        // Kalıcı varsayılan yığın arabelleği
        ID3D12Resource* Dedicated = nullptr;
        uint64_t DedicatedSize = 0;
        D3D12_RESOURCE_STATES DedicatedState = D3D12_RESOURCE_STATE_COMMON;
        // Güncelleme sıklığı (halka ya da kalıcı seçimi)
        uint64_t WriteFrame = 0;
        uint32_t WritesThisFrame = 0;
        uint32_t RingFrames = 0;                         // ardışık halka kullanılan kare sayısı
        // Hesaplama (307): depolama / dolaylı arabellek olarak bağlanınca içerik GPU'daki UAV'li varsayılan yığın arabelleğindedir
        bool GpuOwned = false;
        ID3D12Resource* Storage = nullptr;
        uint64_t StorageSize = 0;
        D3D12_RESOURCE_STATES StorageState = D3D12_RESOURCE_STATE_COMMON;
        std::unordered_map<uint64_t, CpuDescriptor> StorageUAVs, StorageSRVs;
    };

    struct Attachment
    {
        uint32_t Texture = 0;
        uint32_t Renderbuffer = 0;
        int Level = 0;
        int Layer = 0;
        bool Valid() const { return Texture || Renderbuffer; }
    };

    struct Framebuffer
    {
        Attachment Color[8];
        Attachment Depth;
        Attachment Stencil;
        GLenum DrawBuffers[8] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE };
        int NumDrawBuffers = 1;
        GLenum ReadBuffer = GL_COLOR_ATTACHMENT0;
    };

    struct Attrib
    {
        bool Enabled = false;
        GLint Size = 4;
        GLenum Type = GL_FLOAT;
        bool Normalized = false;
        bool Integer = false;
        GLsizei Stride = 0;
        uint64_t Offset = 0;
        uint32_t Buffer = 0;
        GLuint Divisor = 0;
    };

    struct VertexArray
    {
        Attrib Attribs[16];
        uint32_t ElementBuffer = 0;
    };

    struct ShaderObject
    {
        GLenum Type = 0;
        std::string Source;
        bool Compiled = false;
        std::string Log;
        int AttachCount = 0;
        bool DeletePending = false;
    };

    using DXCommon::UniformSlot;
    using DXCommon::SamplerBinding;
    using DXCommon::BlockBinding;
    using DXCommon::VertexInput;

    struct Program
    {
        std::string Label;                  // glObjectLabel (motorun gölgelendirici adı, 323)
        uint64_t Id = 0;
        std::vector<uint32_t> Shaders;
        // Hesaplama gölgelendiricisi (307): görüntü uniform'ları ve depolama blokları; IsCompute ise köşe / parça aşaması yok
        bool IsCompute = false;
        uint32_t LocalSize[3] = { 1, 1, 1 };
        std::vector<GLEmu::ImageBinding> Images;
        std::vector<GLEmu::StorageBlockBinding> StorageBlocks;
        bool Linked = false;
        std::string Log;
        std::vector<uint8_t> VS, PS;                     // DXBC
        std::vector<uint8_t> CS;
        ID3D12PipelineState* ComputePSO = nullptr;      // hesaplama programı (307)
        int GlobalsRegister = -1;
        uint32_t GlobalsSize = 0;
        std::vector<uint8_t> Globals;
        bool GlobalsDirty = true;
        D3D12_GPU_VIRTUAL_ADDRESS GlobalsAddress = 0;    // halkada (GlobalsFrame karesinde geçerli)
        uint64_t GlobalsFrame = 0;
        std::vector<UniformSlot> Uniforms;
        std::unordered_map<std::string, int> UniformByName;
        std::vector<SamplerBinding> Samplers;
        std::vector<BlockBinding> Blocks;
        std::vector<VertexInput> Inputs;
        uint32_t ColorOutputs = 1;
        uint32_t ColorOutputMask = 1;
        bool DeletePending = false;
    };

    struct QueryObject
    {
        uint32_t Index = UINT32_MAX;                     // zaman damgası yığını dizini
        uint64_t Serial = 0;                             // yazan gönderim
        bool Written = false;
        uint32_t OcclIndex = UINT32_MAX;                 // örtüşme sorgusu yığını dizini
        uint64_t OcclSerial = 0;
        bool Occlusion = false, Any = false;
    };

    struct State
    {
        GLEmu::ComputeBindings Compute;     // glBindImageTexture / depolama arabelleği / dolaylı dispatch (307)
        bool Blend = false, DepthTest = false, CullFace = false, ScissorTest = false, StencilTest = false;
        bool PolygonOffsetFill = false, PrimitiveRestart = false;
        GLenum BlendSrcRGB = GL_ONE, BlendDstRGB = GL_ZERO, BlendSrcA = GL_ONE, BlendDstA = GL_ZERO;
        GLenum BlendEqRGB = GL_FUNC_ADD, BlendEqA = GL_FUNC_ADD;
        bool DepthMask = true;
        GLenum DepthFunc = GL_LESS;
        GLenum CullMode = GL_BACK;
        GLenum FrontFace = GL_CCW;
        GLenum PolygonMode = GL_FILL;
        float PolygonOffsetFactor = 0.0f, PolygonOffsetUnits = 0.0f;
        bool ColorMask[4] = { true, true, true, true };
        uint8_t ColorMaskBits[8] = { 15, 15, 15, 15, 15, 15, 15, 15 };   // ek başına (glColorMaski): R=1 G=2 B=4 A=8
        float LineWidth = 1.0f;
        GLenum StencilFunc = GL_ALWAYS;
        GLint StencilRef = 0;
        GLuint StencilValueMask = 0xFFFFFFFF, StencilWriteMask = 0xFFFFFFFF;
        GLenum StencilFail = GL_KEEP, StencilZFail = GL_KEEP, StencilZPass = GL_KEEP;
        GLint Viewport[4] = { 0, 0, 0, 0 };
        GLint Scissor[4] = { 0, 0, 0, 0 };
        float ClearColor[4] = { 0, 0, 0, 0 };
        float ClearDepth = 1.0f;
        GLint ClearStencil = 0;
        uint32_t Program = 0;
        uint32_t VAO = 0;
        uint32_t ArrayBuffer = 0;
        uint32_t ElementBufferNoVAO = 0;
        uint32_t UniformBuffer = 0;
        uint32_t PixelUnpackBuffer = 0;
        uint32_t DrawFBO = 0, ReadFBO = 0;
        uint32_t Renderbuffer = 0;
        uint32_t ActiveTexture = 0;
        std::array<uint32_t, 64> Tex2D{}, TexCube{}, Tex2DMS{}, Samplers{};
        std::array<uint32_t, 64> UBOBindings{};
        std::array<GLintptr, 64> UBOOffsets{};
        std::array<GLsizeiptr, 64> UBOSizes{};
        GLint UnpackAlignment = 4, PackAlignment = 4, UnpackRowLength = 0;
        GLenum ClipOrigin = GL_LOWER_LEFT;
    };

    struct Context
    {
        State S;
        std::unordered_map<uint32_t, std::unique_ptr<Texture>> Textures;
        std::unordered_map<uint32_t, std::unique_ptr<Texture>> Renderbuffers;
        std::unordered_map<uint32_t, std::unique_ptr<Buffer>> Buffers;
        std::unordered_map<uint32_t, std::unique_ptr<Framebuffer>> Framebuffers;
        std::unordered_map<uint32_t, std::unique_ptr<VertexArray>> VertexArrays;
        std::unordered_map<uint32_t, std::unique_ptr<ShaderObject>> ShaderObjects;
        std::unordered_map<uint32_t, std::unique_ptr<Program>> Programs;
        std::unordered_map<uint32_t, std::unique_ptr<Texture>> SamplerObjects;
        uint32_t NextSamplerName = 1;           // glGenSamplers / glCreateSamplers
        std::unordered_map<uint32_t, QueryObject> QueryObjects;
        uint32_t NextName = 1;
        uint64_t NextProgramId = 1;
        VertexArray DefaultVAO;

        Texture BackColor;
        Texture BackDepth;
        Framebuffer DefaultFB;

        // Çıkış birleştirici bağlaması
        bool Rendering = false;
        uint32_t RenderingFBO = UINT32_MAX;
        uint64_t RenderingKey = 0;
        uint64_t RenderingEpoch = 0;
        uint32_t RenderWidth = 0, RenderHeight = 0;
        Texture* RenderColor[8] = {};
        Texture* RenderDepth = nullptr;
        DXGI_FORMAT RenderColorFormats[8] = {};
        bool RenderColorInteger[8] = {};
        D3D12_CPU_DESCRIPTOR_HANDLE BoundRTVs[8] = {};
        D3D12_CPU_DESCRIPTOR_HANDLE BoundDSV{};
        uint32_t BoundRTVCount = 0;
        DXGI_FORMAT RenderingDepthFormat = DXGI_FORMAT_UNKNOWN;
        bool RenderingHasDepth = false, RenderingHasStencil = false;
        uint32_t RenderingSamples = 1;

        // Kalıcı nesneler
        std::unordered_map<uint64_t, ID3D12PipelineState*> Pipelines;
        std::unordered_map<uint64_t, CpuDescriptor> SamplerCache;
        CpuDescriptor NullSRV;                           // kullanılmayan tablo yuvaları
        CpuDescriptor NullUAV;                           // kullanılmayan UAV yuvaları (307)
        std::unordered_map<uint64_t, ID3D12CommandSignature*> CommandSignatures;   // ExecuteIndirect: tür | adım << 8
        CpuDescriptor NullRTV;                           // boş renk yuvası (biçim R8G8B8A8)
        CpuDescriptor NullSampler;
        ID3D12Resource* NullVertexBuffer = nullptr;      // kapalı öznitelik: (0,0,0,1)
        ID3D12Resource* ZeroConstants = nullptr;         // bağlanmamış blok: 64 KB sıfır
        std::vector<std::unique_ptr<Texture>> InternalTextures;
        Texture* NullTexture2D = nullptr;
        Texture* NullTextureCube = nullptr;
        Texture* NullTextureInt = nullptr;

        std::string Vendor, Renderer, Version;
        std::vector<std::string> Extensions;
        GLDEBUGPROC DebugCallback = nullptr;
        const void* DebugUserParam = nullptr;
        bool Live = false;
    };

    Context& Ctx();

    bool Install(std::string& error);
    void Shutdown();
    void BeginFrameResources();
    void SwapBuffers();

    Texture* GetTexture(uint32_t name);
    Texture* GetRenderbuffer(uint32_t name);
    Buffer* GetBuffer(uint32_t name);
    Framebuffer* GetFramebuffer(uint32_t name);
    Program* GetProgram(uint32_t name);
    Texture* AttachmentTexture(const Attachment& a);

    bool AllocateTexture(Texture& t, GLenum target, GLenum internalFormat, uint32_t w, uint32_t h, uint32_t levels, uint32_t samples);
    void DestroyTextureImage(Texture& t);
    void Transition(Texture& t, D3D12_RESOURCE_STATES state);
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(Texture& t);
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(Texture& t, int level, int layer);
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV(Texture& t, int level, int layer);
    CpuDescriptor CreateSRV(Texture& t, int mostDetailedMip, int mipLevels, int layer);   // tek kullanımlık (çağıran bırakır)
    D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(const Texture& params, bool integer, bool comparison);
    void UploadTexture(Texture& t, int level, int layer, int x, int y, int w, int h, GLenum format, GLenum type, const void* data);
    void ReadImage(Texture& t, int level, int layer, int x, int y, int w, int h, GLenum format, GLenum type, void* out, int packAlignment);
    // Arabelleğin GPU adresi (gerekirse yükleme / kopya kaydedilir); minSize: sabit arabellek bildirilen boyu
    D3D12_GPU_VIRTUAL_ADDRESS BufferAddress(Buffer& b, uint64_t minSize, uint64_t* size = nullptr);

    using GLEmu::Limits;
    const Limits& GetLimits();
    uint32_t ClampSamples(GLsizei samples, bool depth);
    void ReleaseBuffer(Buffer& b);
    void WriteTimestamp(QueryObject& q);
    bool ReadTimestamp(QueryObject& q, bool wait, uint64_t& ns);
    void BeginOcclusion(QueryObject& q);
    void EndOcclusion(QueryObject& q);
    bool ReadOcclusion(QueryObject& q, bool wait, uint64_t& samples);

    VertexArray* CurrentVAO();
    bool Live();
    void FlushAndWaitGL();
    bool GrowLevels(Texture& t, uint32_t levels);
    void GenerateMipmaps(Texture& t);
    void ClearTexture(Texture& t, int level, GLenum format, GLenum type, const void* data);
    void BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);

    void EndRendering();
    bool EnsureRendering();
    void Draw(GLenum mode, GLint first, GLsizei count, GLsizei instances, bool indexed, GLenum indexType, const void* indices, GLint baseVertex);
    void ClearCurrent(GLbitfield mask);

    // Hesaplama (307)
    bool EnsureStorageImage(Texture& t);
    D3D12_CPU_DESCRIPTOR_HANDLE GetUAV(Texture& t, int level, int layer);
    D3D12_CPU_DESCRIPTOR_HANDLE GetLevelSRV(Texture& t, int level, int layer);
    ID3D12Resource* StorageBuffer(Buffer& b);
    void TransitionStorage(Buffer& b, D3D12_RESOURCE_STATES state);
    D3D12_CPU_DESCRIPTOR_HANDLE StorageUAV(Buffer& b, size_t offset, size_t size);
    D3D12_CPU_DESCRIPTOR_HANDLE StorageSRV(Buffer& b, size_t offset, size_t size);
    bool LinkProgram(Program& p);
    // Hesaplama gölgelendiricisi (307): geçerli program IsCompute ise dispatch (indirectBuffer 0: doğrudan x / y / z; değilse o arabellekte
    // indirectOffset'teki üç uint), glMemoryBarrier
    void DispatchComputeGL(uint32_t x, uint32_t y, uint32_t z, uint32_t indirectBuffer, int64_t indirectOffset);
    void ProgramLabelGL(Program& p);       // 323: glObjectLabel(GL_PROGRAM) arka uç nesnelerine hata ayıklama adı verir
    void NameD3D12Object(ID3D12Object* o, const std::string& label);
    void MemoryBarrierGL(GLbitfield barriers);
    // 307: GPU'ya ait arabellek (hesaplama gölgelendiricisinin yazdığı) kopyası / geri okuması (false: ön yüz gölge kopyayı kullanır),
    // glFenceSync gönderim sayacı, dolaylı çizim ve katmanın sağlamadığı GL işlevleri (GetProcAddress boş döner)
    bool CopyBufferGL(Buffer& src, Buffer& dst, size_t srcOffset, size_t dstOffset, size_t size);
    bool ReadBufferGL(Buffer& b, size_t offset, size_t size, void* out);
    uint64_t SubmitSerialGL();
    bool SerialCompletedGL(uint64_t serial);
    void DrawIndirectGL(GLenum mode, bool indexed, GLenum indexType, uint32_t indirectBuffer, int64_t offset, GLsizei drawCount, GLsizei stride,
                        uint32_t countBuffer, int64_t countOffset);
    bool BackendSupportsGL(const char* name);
    void DestroyProgram(Program& p);
    UniformSlot* FindUniform(Program& p, GLint location);

    template <typename T>
    void SafeRelease(T*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    // GPU işi bitince bırak
    template <typename T>
    void DeferRelease(T*& p)
    {
        if (p)
        {
            DX12::Core::Get().DeferRelease(p);
            p = nullptr;
        }
    }
}

#endif
