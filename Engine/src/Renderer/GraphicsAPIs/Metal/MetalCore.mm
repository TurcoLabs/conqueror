#ifdef CQ_METAL_ENABLED

#include "MetalCore.h"
#include "Core/Logging/Log.h"

#if defined(CQ_PLATFORM_IOS)
    // iOS / iPadOS: oyun görünümünün kendi CAMetalLayer'ı (Platform/iOS/GLFWiOS.mm)
    #import <UIKit/UIKit.h>
    #include "Platform/iOS/iOSPlatform.h"
    #include <GLFW/glfw3.h>
#else
    #import <Cocoa/Cocoa.h>
    #define GLFW_EXPOSE_NATIVE_COCOA
    #include <GLFW/glfw3.h>
    #include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <atomic>
#include <mach/mach_time.h>

namespace Conqueror::MTL
{
    const char* g_DiagLabel = "frame";
    std::atomic<int> g_DiagErrors{ 0 };
    std::atomic<int> g_DiagSlow{ 0 };

    namespace
    {
        constexpr uint64_t kChunkSize = 32ull * 1024 * 1024;
    }

    Core& Core::Get()
    {
        static Core s;
        return s;
    }

    uint64_t Core::NowNs()
    {
        static mach_timebase_info_data_t tb{};
        if (tb.denom == 0)
            mach_timebase_info(&tb);
        return mach_absolute_time() * tb.numer / tb.denom;
    }

    uint32_t Core::FramebufferWidth() const
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(m_Window, &w, &h);
        return (uint32_t)std::max(w, 1);
    }

    uint32_t Core::FramebufferHeight() const
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(m_Window, &w, &h);
        return (uint32_t)std::max(h, 1);
    }

    bool Core::Init(GLFWwindow* window, std::string& error)
    {
        m_Window = window;
        m_Device = MTLCreateSystemDefaultDevice();
        if (!m_Device)
        {
            error = "No Metal device";
            return false;
        }
        m_DeviceName = [[m_Device name] UTF8String];
        m_VideoMemory = [m_Device recommendedMaxWorkingSetSize];
        m_Queue = [m_Device newCommandQueue];
        if (!m_Queue)
        {
            error = "Cannot create the Metal command queue";
            return false;
        }

#if defined(CQ_PLATFORM_IOS)
        m_Layer = (__bridge CAMetalLayer*)iOS::MetalLayer();
        if (!m_Layer)
        {
            error = "No Metal view";
            return false;
        }
        m_Layer.device = m_Device;
        m_Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;   // motor çıktısı zaten sRGB kodlu
        m_Layer.framebufferOnly = YES;
        m_Layer.maximumDrawableCount = 3;
        m_Layer.contentsScale = iOS::ContentScale();
        m_Layer.drawableSize = CGSizeMake(FramebufferWidth(), FramebufferHeight());
#else
        NSWindow* nsWindow = (NSWindow*)glfwGetCocoaWindow(window);
        if (!nsWindow)
        {
            error = "No Cocoa window";
            return false;
        }
        m_Layer = [CAMetalLayer layer];
        m_Layer.device = m_Device;
        m_Layer.pixelFormat = MTLPixelFormatBGRA8Unorm;   // motor çıktısı zaten sRGB kodlu
        m_Layer.framebufferOnly = YES;
        m_Layer.maximumDrawableCount = 3;
        m_Layer.contentsScale = [nsWindow backingScaleFactor];
        m_Layer.drawableSize = CGSizeMake(FramebufferWidth(), FramebufferHeight());
        NSView* view = [nsWindow contentView];
        [view setWantsLayer:YES];
        [view setLayer:m_Layer];
#endif

        m_Inflight = dispatch_semaphore_create(kFrames);
        for (Frame& f : m_Frames)
            if (!NewChunk(f, kChunkSize))
            {
                error = "Cannot allocate the upload ring";
                return false;
            }
        CQ_CORE_INFO("[Metal] Device: {0} ({1} MB working set)", m_DeviceName, m_VideoMemory / (1024 * 1024));
        return true;
    }

    void Core::SetVSync(bool enabled)
    {
#if defined(CQ_PLATFORM_IOS)
        (void)enabled;   // iOS'ta sunum her zaman ekran tazelemesiyle (CADisplayLink)
#else
        if (m_Layer)
            m_Layer.displaySyncEnabled = enabled ? YES : NO;
#endif
    }

    bool Core::NewChunk(Frame& f, uint64_t size)
    {
        id<MTLBuffer> b = [m_Device newBufferWithLength:std::max(size, kChunkSize) options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        if (!b)
            return false;
        f.Chunks.push_back(b);
        return true;
    }

    UploadAlloc Core::AllocUpload(uint64_t size, uint64_t alignment)
    {
        Frame& f = m_Frames[m_FrameIndex];
        alignment = std::max<uint64_t>(alignment, 16);
        size = std::max<uint64_t>(size, 1);
        for (;;)
        {
            id<MTLBuffer> c = f.Chunks[f.Chunk];
            const uint64_t at = (f.Offset + alignment - 1) / alignment * alignment;
            if (at + size <= [c length])
            {
                f.Offset = at + size;
                return { (uint8_t*)[c contents] + at, c, at };
            }
            if (f.Chunk + 1 < f.Chunks.size())
            {
                ++f.Chunk;
                f.Offset = 0;
                continue;
            }
            if (!NewChunk(f, size + alignment))
            {
                CQ_CORE_ERROR("[Metal] Upload ring allocation of {0} bytes failed", size);
                return {};
            }
            f.Chunk = f.Chunks.size() - 1;
            f.Offset = 0;
        }
    }

    id<MTLCommandBuffer> Core::CommandBuffer()
    {
        if (!m_Command)
            m_Command = [m_Queue commandBuffer];
        return m_Command;
    }

    void Core::EndEncoding()
    {
        if (m_Render)
        {
            [m_Render endEncoding];
            m_Render = nil;
            ++m_EncoderEpoch;
        }
        if (m_Blit)
        {
            [m_Blit endEncoding];
            m_Blit = nil;
        }
    }

    id<MTLRenderCommandEncoder> Core::BeginRender(MTLRenderPassDescriptor* pass)
    {
        EndEncoding();
        m_Render = [CommandBuffer() renderCommandEncoderWithDescriptor:pass];
        return m_Render;
    }

    id<MTLBlitCommandEncoder> Core::Blit()
    {
        if (m_Blit)
            return m_Blit;
        EndEncoding();
        m_Blit = [CommandBuffer() blitCommandEncoder];
        return m_Blit;
    }

    void Core::Flush()
    {
        if (!m_Device)
            return;
        EndEncoding();
        if (m_Command)
        {
            [m_Command commit];
            [m_Command waitUntilCompleted];
            if (m_Command.error && g_DiagErrors.fetch_add(1) < 40)
                CQ_CORE_ERROR("[Metal diag] command buffer failed (flush after '{0}', frame {1}): {2}", g_DiagLabel ? g_DiagLabel : "?", m_FrameSerial,
                              [[m_Command.error description] UTF8String]);
            else if (!m_Command.error && m_Command.GPUEndTime - m_Command.GPUStartTime > 0.1 && g_DiagSlow.fetch_add(1) < 40)
                CQ_CORE_WARN("[Metal diag] slow command buffer (flush after '{0}', frame {1}): GPU {2:.1f} ms", g_DiagLabel ? g_DiagLabel : "?", m_FrameSerial,
                             (m_Command.GPUEndTime - m_Command.GPUStartTime) * 1000.0);
            m_Command = nil;
        }
    }

    id<CAMetalDrawable> Core::Drawable()
    {
        if (m_Drawable)
            return m_Drawable;
        const CGSize want = CGSizeMake(FramebufferWidth(), FramebufferHeight());
        if (m_Layer.drawableSize.width != want.width || m_Layer.drawableSize.height != want.height)
            m_Layer.drawableSize = want;
        m_Drawable = [m_Layer nextDrawable];
        return m_Drawable;
    }

    void Core::Present()
    {
        EndEncoding();
        id<MTLCommandBuffer> cmd = CommandBuffer();
        if (m_Drawable)
            [cmd presentDrawable:m_Drawable];
        const uint64_t serial = m_FrameSerial;
        dispatch_semaphore_t sem = m_Inflight;
        volatile uint64_t* completed = &m_CompletedFrame;
        const char* label = g_DiagLabel;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.error && g_DiagErrors.fetch_add(1) < 40)
                CQ_CORE_ERROR("[Metal diag] command buffer failed (frame {0}, last '{1}', GPU {2:.1f} ms): {3}", serial, label ? label : "?",
                              (done.GPUEndTime - done.GPUStartTime) * 1000.0, [[done.error description] UTF8String]);
            else if (!done.error && done.GPUEndTime - done.GPUStartTime > 0.25 && g_DiagSlow.fetch_add(1) < 40)
                CQ_CORE_WARN("[Metal diag] slow frame command buffer (frame {0}, last '{1}'): GPU {2:.1f} ms", serial, label ? label : "?",
                             (done.GPUEndTime - done.GPUStartTime) * 1000.0);
            if (!done.error && (serial == 30 || serial == 120 || serial == 600))
                CQ_CORE_WARN("[Metal diag] frame {0} command buffer: GPU {1:.1f} ms", serial, (done.GPUEndTime - done.GPUStartTime) * 1000.0);
            if (*completed < serial)
                *completed = serial;
            dispatch_semaphore_signal(sem);
        }];
        [cmd commit];
        m_Command = nil;
        m_Drawable = nil;
        ++m_FrameSerial;
        m_FrameIndex = (m_FrameIndex + 1) % kFrames;
        // En çok kFrames kare uçuşta: sıradaki halkanın karesi bitene kadar bekle
        dispatch_semaphore_wait(m_Inflight, DISPATCH_TIME_FOREVER);
        Frame& f = m_Frames[m_FrameIndex];
        if (f.Chunks.size() > 1)
            f.Chunks.resize(1);
        f.Chunk = 0;
        f.Offset = 0;
    }

    void Core::Shutdown()
    {
        if (!m_Device)
            return;
        Flush();
        // Uçuştaki karelerin bitmesini bekle (semafor sayısı geri gelsin)
        id<MTLCommandBuffer> fence = [m_Queue commandBuffer];
        [fence commit];
        [fence waitUntilCompleted];
        for (Frame& f : m_Frames)
            f.Chunks.clear();
        m_Drawable = nil;
#if !defined(CQ_PLATFORM_IOS)
        if (m_Layer)
        {
            NSWindow* nsWindow = (NSWindow*)glfwGetCocoaWindow(m_Window);
            if (nsWindow)
                [[nsWindow contentView] setLayer:nil];
        }
#endif
        m_Layer = nil;
        m_Queue = nil;
        m_Device = nil;
    }
}

#endif
