#include "ExactDepth.h"
#include "Renderer/RHI/Shader.h"
#include "Renderer/Renderer.h"
#include "Renderer/Utilities/Renderer3D/Renderer3D.h"
#include "Core/Profiling/FrameProfiler.h"

#include <glad/glad.h>
#include <memory>
#include <algorithm>

namespace Conqueror
{
    namespace
    {
        std::shared_ptr<Shader> s_RangeShader;
        bool s_RangeFailed = false;
        uint32_t s_EmptyVAO = 0;

        bool EnsureRangeShader()
        {
            if (s_RangeFailed)
                return false;
            if (!s_RangeShader)
            {
                s_RangeShader = Shader::Create("Engine/src/Shaders/PostProcess/ExactDepthRange.cqsh");
                if (!s_RangeShader || !s_RangeShader->IsValid())
                {
                    s_RangeShader.reset();
                    s_RangeFailed = true;
                    return false;
                }
                glGenVertexArrays(1, &s_EmptyVAO);
            }
            return true;
        }
    }

    void ExactDepth::ShutdownShared()
    {
        s_RangeShader.reset();
        if (s_EmptyVAO)
        {
            glDeleteVertexArrays(1, &s_EmptyVAO);
            s_EmptyVAO = 0;
        }
        s_RangeFailed = false;
    }

    ExactDepth::~ExactDepth()
    {
        Release();
    }

    void ExactDepth::Release()
    {
        if (m_DepthFramebuffer) glDeleteFramebuffers(1, &m_DepthFramebuffer);
        if (m_RangeFramebuffer) glDeleteFramebuffers(1, &m_RangeFramebuffer);
        if (m_WideFramebuffer) glDeleteFramebuffers(1, &m_WideFramebuffer);
        if (m_Wide) glDeleteTextures(1, &m_Wide);
        if (m_WideTmpFramebuffer) glDeleteFramebuffers(1, &m_WideTmpFramebuffer);
        if (m_WideTmp) glDeleteTextures(1, &m_WideTmp);
        if (m_Depth) glDeleteTextures(1, &m_Depth);
        if (m_Range) glDeleteTextures(1, &m_Range);
        if (m_Share) glDeleteTextures(1, &m_Share);
        m_Depth = m_DepthFramebuffer = m_Range = m_RangeFramebuffer = m_Share = m_Wide = m_WideFramebuffer = m_WideTmp = m_WideTmpFramebuffer = 0;
        m_Width = m_Height = 0;
        m_Failed = false;
    }

    bool ExactDepth::EnsureTargets(uint32_t width, uint32_t height)
    {
        if (m_DepthFramebuffer && m_Width == width && m_Height == height)
            return true;
        if (m_Failed && m_Width == width && m_Height == height)
            return false;
        Release();
        m_Width = width;
        m_Height = height;
        GLint maxSize = 4096;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
        if ((GLint)(width * m_Factor) > maxSize || (GLint)(height * m_Factor) > maxSize)
        {
            m_Failed = true;
            return false;
        }
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Depth);
        glTextureStorage2D(m_Depth, 1, GL_DEPTH_COMPONENT32F, (GLsizei)(width * m_Factor), (GLsizei)(height * m_Factor));
        glTextureParameteri(m_Depth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Depth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_Depth, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glCreateFramebuffers(1, &m_DepthFramebuffer);
        glNamedFramebufferTexture(m_DepthFramebuffer, GL_DEPTH_ATTACHMENT, m_Depth, 0);
        glNamedFramebufferDrawBuffer(m_DepthFramebuffer, GL_NONE);
        glNamedFramebufferReadBuffer(m_DepthFramebuffer, GL_NONE);
        auto texture = [&](uint32_t& tex, GLenum format) {
            glCreateTextures(GL_TEXTURE_2D, 1, &tex);
            glTextureStorage2D(tex, 1, format, (GLsizei)width, (GLsizei)height);
            glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        };
        auto colorTarget = [&](uint32_t& tex, uint32_t& fb, GLenum format) {
            texture(tex, format);
            glCreateFramebuffers(1, &fb);
            glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, tex, 0);
        };
        colorTarget(m_Range, m_RangeFramebuffer, GL_RG32F);
        texture(m_Share, GL_R32F);
        glNamedFramebufferTexture(m_RangeFramebuffer, GL_COLOR_ATTACHMENT1, m_Share, 0);
        const GLenum rangeBuffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glNamedFramebufferDrawBuffers(m_RangeFramebuffer, 2, rangeBuffers);
        colorTarget(m_Wide, m_WideFramebuffer, GL_RG32F);
        colorTarget(m_WideTmp, m_WideTmpFramebuffer, GL_RG32F);
        if (glCheckNamedFramebufferStatus(m_DepthFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE
            || glCheckNamedFramebufferStatus(m_RangeFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE
            || glCheckNamedFramebufferStatus(m_WideFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE
            || glCheckNamedFramebufferStatus(m_WideTmpFramebuffer, GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            Release();
            m_Width = width;
            m_Height = height;
            m_Failed = true;
            return false;
        }
        return true;
    }

    bool ExactDepth::Render(const void* view, const glm::mat4& viewProj, const glm::mat4& projection, uint32_t width, uint32_t height,
                            int neighbourhoodRadius)
    {
        if (!view || width == 0 || height == 0 || !EnsureRangeShader() || !EnsureTargets(width, height))
            return false;

        GLint prevDraw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
        GLint prevViewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        const GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean blend = glIsEnabled(GL_BLEND);
        const GLboolean cull = glIsEnabled(GL_CULL_FACE);
        const GLboolean offsetFill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        GLboolean depthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        GLint depthFunc = GL_LESS;
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        GLint polygonMode[2] = { GL_FILL, GL_FILL };
        glGetIntegerv(GL_POLYGON_MODE, polygonMode);
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        // Aralık geçişinin kullandığı doku birimleri (0, 1): çağıranın bağları geri yüklenir
        GLint activeUnit = GL_TEXTURE0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
        GLint unitTex[2] = { 0, 0 };
        for (int u = 0; u < 2; ++u)
        {
            glActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &unitTex[u]);
        }
        glActiveTexture((GLenum)activeUnit);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_DepthFramebuffer);
        glViewport(0, 0, (GLsizei)(width * m_Factor), (GLsizei)(height * m_Factor));
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        // Bağlı çerçeve tamponunun temizlemesi: GL öykünme katmanı (Vulkan / Direct3D arka ucu) DSA temizlemeyi (glClearNamedFramebufferfv)
        // desteklemiyor, çağrı yok sayılıyor ve önceki karelerin derinliği kalıyordu (sürüklenen küpün eski yerleri ızgarayı örtüyordu)
        bool drew = false;
        {
            CQ_PROFILE_GPU("Exact Depth");
            const float one = 1.0f;
            glClearBufferfv(GL_DEPTH, 0, &one);
            drew = Renderer3D::RenderGeometryDepth(view, viewProj);
        }
        if (drew)
        {
            CQ_PROFILE_GPU("Exact Range");
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_RangeFramebuffer);
            glViewport(0, 0, (GLsizei)width, (GLsizei)height);
            s_RangeShader->Bind();
            s_RangeShader->SetInt("u_Mode", 0);
            s_RangeShader->SetInt("u_Factor", m_Factor);
            s_RangeShader->SetFloat2("u_ProjZ", ProjZ(projection));
            glBindTextureUnit(0, m_Depth);
            glBindTextureUnit(1, 0);   // u_RangeIn: hedefin kendisi bağlı kalmasın (geri besleme döngüsü)
            s_RangeShader->SetInt("u_Depth", 0);
            s_RangeShader->SetInt("u_RangeIn", 1);
            glBindVertexArray(s_EmptyVAO);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            Renderer::GetStats().DrawCalls++;
            if (neighbourhoodRadius > 0)
            {
                // (2 r + 1)^2 en yakın / en uzak: önce yatay, sonra dikey
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_WideTmpFramebuffer);
                s_RangeShader->SetInt("u_Radius", std::min(neighbourhoodRadius, 5));
                s_RangeShader->SetInt("u_Mode", 1);
                glBindTextureUnit(1, m_Range);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_WideFramebuffer);
                s_RangeShader->SetInt("u_Mode", 2);
                glBindTextureUnit(1, m_WideTmp);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                Renderer::GetStats().DrawCalls += 2;
            }
            glBindVertexArray(0);
            glBindTextureUnit(0, (GLuint)unitTex[0]);
            glBindTextureUnit(1, (GLuint)unitTex[1]);
        }

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevDraw);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        glDepthFunc((GLenum)depthFunc);
        glDepthMask(depthMask);
        glPolygonMode(GL_FRONT_AND_BACK, (GLenum)polygonMode[0]);
        if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (offsetFill) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
        glUseProgram((GLuint)program);
        return drew;
    }
}
