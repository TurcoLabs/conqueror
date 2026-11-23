#include "RayTracedEffects.h"
#include "Renderer/RayTracing/RayTracingScene.h"

#include <glad/glad.h>

#include <algorithm>
#include <cmath>

// Ortak sahne / BVH / ışık / gökyüzü: Renderer/RayTracing/RayTracingScene (birim 0-3: gökyüzü küpleri, doku ortalamaları, atlas)
namespace Conqueror
{
    namespace
    {
        const char* kFxCommon = R"(
uniform sampler2D u_FxDepth;
uniform mat4 u_FxInvViewProj;
uniform vec3 u_FxCamera;
uniform ivec2 u_FxSize;

vec3 fxWorld(vec2 uv, float d)
{
    vec4 p = u_FxInvViewProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

// Tam çözünürlüklü derinliğin texel'i (yarım çözünürlüklü hedefin pikseli en yakın texel'e hizalanır, SSAO gibi)
ivec2 fxTexel(ivec2 px)
{
    ivec2 full = textureSize(u_FxDepth, 0);
    return clamp(ivec2((vec2(px) + 0.5) / vec2(u_FxSize) * vec2(full)), ivec2(0), full - 1);
}

float fxIGN(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
)";

        const char* kReflectionMain = R"(
layout(rgba16f, binding = 0) uniform writeonly image2D u_OutColor;
layout(r16f, binding = 1) uniform writeonly image2D u_OutHit;
uniform sampler2D u_FxScene;
uniform sampler2D u_FxNormal;
uniform mat4 u_FxViewProj;
uniform vec3 u_FxParams;   // x en büyük uzaklık, y en büyük pürüzlülük, z kare

vec3 octDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main()
{
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, u_FxSize)))
        return;
    ivec2 tx = fxTexel(px);
    vec2 uv = (vec2(tx) + 0.5) / vec2(textureSize(u_FxDepth, 0));
    vec4 g = texelFetch(u_FxNormal, ivec2(uv * vec2(textureSize(u_FxNormal, 0))), 0);
    float depth = texelFetch(u_FxDepth, tx, 0).r;
    vec4 outC = vec4(0.0);
    float outH = 0.0;
    if (u_NodeCount > 0 && g.w >= 1.5 && g.z <= u_FxParams.y && depth < 1.0)
    {
        vec3 P = fxWorld(uv, depth);
        vec3 N = octDecode(g.xy);
        vec3 V = P - u_FxCamera;
        float camDist = length(V);
        V /= max(camDist, 1e-5);
        vec3 R = reflect(V, N);
        vec3 o = P + N * max(0.02, camDist * 0.002);
        float t;
        vec2 bc;
        int ti = trace(o, R, u_FxParams.x, t, bc, false);
        if (ti >= 0)
        {
            vec3 hp = o + R * t;
            vec3 color = vec3(0.0);
            bool fromScreen = false;
            // Ekranda görünen isabet: sahnenin aydınlatılmış rengi (GI, gölge, sis dahil; Unity HDRP "Mixed" izleme gibi).
            // Ekran dışı / örtülü isabet: BVH'nin malzemesi ve ışıkları ile gölgelendirilir
            vec4 clip = u_FxViewProj * vec4(hp, 1.0);
            if (clip.w > 0.0)
            {
                vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
                if (all(greaterThanEqual(suv, vec2(0.0))) && all(lessThanEqual(suv, vec2(1.0))))
                {
                    ivec2 stx = clamp(ivec2(suv * vec2(textureSize(u_FxDepth, 0))), ivec2(0), textureSize(u_FxDepth, 0) - 1);
                    float sd = texelFetch(u_FxDepth, stx, 0).r;
                    vec3 sp = fxWorld(suv, sd);
                    float tol = max(0.05, 0.01 * length(hp - u_FxCamera));
                    if (sd < 1.0 && distance(sp, hp) < tol)
                    {
                        color = textureLod(u_FxScene, suv, 0.0).rgb;
                        fromScreen = true;
                    }
                }
            }
            if (!fromScreen)
            {
                s_Seed = (uint(px.x) * 1973u + uint(px.y) * 9277u + uint(u_FxParams.z) * 26699u) | 1u;
                g_TexDetail = true;
                Hit h = hitInfo(ti, o, R, t, bc);
                vec3 po = h.P + h.Ng * 1e-3 * max(1.0, t);
                vec3 lambert = (1.0 - h.M.Params.x) * h.M.Albedo.rgb;
                color = h.M.Emission.rgb + directLightStochastic(h.P, po, h.N, h.M) + ambient(h.N) * lambert;
            }
            if (any(isnan(color)) || any(isinf(color)))
                color = vec3(0.0);
            float distanceFade = 1.0 - smoothstep(0.6, 1.0, t / u_FxParams.x);
            float roughFade = 1.0 - smoothstep(u_FxParams.y * 0.5, u_FxParams.y, g.z);
            float conf = distanceFade * roughFade;
            outC = vec4(min(color, vec3(64.0)) * conf, conf);
            outH = length(hp - P);
        }
    }
    imageStore(u_OutColor, px, outC);
    imageStore(u_OutHit, px, vec4(outH));
}
)";

        const char* kAOMain = R"(
layout(r8, binding = 0) uniform writeonly image2D u_OutAO;
uniform vec4 u_FxAO;      // x yarıçap, y yoğunluk, z üs, w ışın sayısı
uniform float u_FxFrame;

void main()
{
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, u_FxSize)))
        return;
    ivec2 full = textureSize(u_FxDepth, 0);
    ivec2 tx = fxTexel(px);
    float d = texelFetch(u_FxDepth, tx, 0).r;
    if (u_NodeCount == 0 || d >= 0.99999)
    {
        imageStore(u_OutAO, px, vec4(1.0));
        return;
    }
    vec2 texel = 1.0 / vec2(full);
    vec2 uv = (vec2(tx) + 0.5) * texel;
    vec3 p = fxWorld(uv, d);
    // Normal derinlikten (komşulardan farkı küçük olan taraf: kenarlarda doğru normal)
    vec3 pr = fxWorld(uv + vec2(texel.x, 0.0), texelFetch(u_FxDepth, min(tx + ivec2(1, 0), full - 1), 0).r);
    vec3 pl = fxWorld(uv - vec2(texel.x, 0.0), texelFetch(u_FxDepth, max(tx - ivec2(1, 0), ivec2(0)), 0).r);
    vec3 pt = fxWorld(uv + vec2(0.0, texel.y), texelFetch(u_FxDepth, min(tx + ivec2(0, 1), full - 1), 0).r);
    vec3 pb = fxWorld(uv - vec2(0.0, texel.y), texelFetch(u_FxDepth, max(tx - ivec2(0, 1), ivec2(0)), 0).r);
    vec3 dx = distance(pr, p) < distance(pl, p) ? pr - p : p - pl;
    vec3 dy = distance(pt, p) < distance(pb, p) ? pt - p : p - pb;
    vec3 n = cross(dx, dy);
    if (dot(n, n) < 1e-20)
    {
        imageStore(u_OutAO, px, vec4(1.0));
        return;
    }
    n = normalize(n);
    if (dot(n, u_FxCamera - p) < 0.0)
        n = -n;
    vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    float camDist = distance(p, u_FxCamera);
    vec3 o = p + n * max(0.01, camDist * 0.001);
    int rays = int(u_FxAO.w);
    float radius = u_FxAO.x;
    float a = fxIGN(vec2(px) + 5.588238 * u_FxFrame);
    float b = fxIGN(vec2(px.y, px.x) + 17.0 + 3.1179 * u_FxFrame);
    float occ = 0.0;
    for (int i = 0; i < rays; ++i)
    {
        // Kosinüs ağırlıklı, katmanlı yönler (piksel başına döndürülür)
        float u1 = (float(i) + b) / float(rays);
        float phi = 6.2831853 * fract(float(i) * 0.6180339887 + a);
        float r = sqrt(u1);
        vec3 dir = tangent * (cos(phi) * r) + bitangent * (sin(phi) * r) + n * sqrt(max(1.0 - u1, 0.0));
        float t;
        vec2 bc;
        if (trace(o, dir, radius, t, bc, true) >= 0)
        {
            // Kameraya 0.7 m'den yakın örtücü (birinci şahıs elleri, silah) daha uzaktaki yüzeyi örtmez (SSAO ile aynı kural)
            vec3 hp = o + dir * t;
            if (distance(hp, u_FxCamera) < 0.7 && camDist >= 0.7)
                continue;
            occ += 1.0;
        }
    }
    float ao = 1.0 - occ / float(max(rays, 1)) * u_FxAO.y;
    imageStore(u_OutAO, px, vec4(pow(clamp(ao, 0.0, 1.0), u_FxAO.z)));
}
)";

        struct State
        {
            uint32_t Reflection = 0, AO = 0;
            bool ReflectionTried = false, AOTried = false;
            uint32_t LastReflection = 0, LastAO = 0;
        };
        State& S() { static auto& s = *new State(); return s; }

        GLint U(uint32_t program, const char* name) { return glGetUniformLocation(program, name); }

        bool TargetSize(uint32_t texture, GLint& w, GLint& h)
        {
            w = h = 0;
            if (!texture)
                return false;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &w);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &h);
            return w > 0 && h > 0;
        }

        void SetCommon(uint32_t program, const glm::mat4& viewProj, const glm::vec3& camera, GLint w, GLint h, uint32_t depth)
        {
            const glm::mat4 inv = glm::inverse(viewProj);
            glUniformMatrix4fv(U(program, "u_FxInvViewProj"), 1, GL_FALSE, &inv[0][0]);
            glUniform3fv(U(program, "u_FxCamera"), 1, &camera.x);
            glUniform2i(U(program, "u_FxSize"), w, h);
            glUniform1i(U(program, "u_FxDepth"), 4);
            glBindTextureUnit(4, depth);
        }
    }

    namespace RayTracedEffects
    {
        bool Supported()
        {
            State& s = S();
            if (!s.ReflectionTried)
            {
                s.ReflectionTried = true;
                s.Reflection = RayTracingScene::CompileCompute("Ray Traced Reflections", "layout(local_size_x = 8, local_size_y = 8) in;",
                                                               std::string(kFxCommon) + kReflectionMain);
            }
            return s.Reflection != 0;
        }

        bool TraceReflections(const ReflectionInputs& in, uint32_t color, uint32_t hit)
        {
            State& s = S();
            s.LastReflection = 0;
            GLint w, h;
            if (!Supported() || !in.Depth || !in.Normal || !in.Scene || !TargetSize(color, w, h))
                return false;
            if (RayTracingScene::GetStats().Triangles == 0)
                return false;
            const uint32_t p = s.Reflection;
            RayTracingScene::Bind(p, 0, 1, 2);
            SetCommon(p, in.ViewProj, in.CameraPos, w, h, in.Depth);
            glUniformMatrix4fv(U(p, "u_FxViewProj"), 1, GL_FALSE, &in.ViewProj[0][0]);
            glUniform3f(U(p, "u_FxParams"), std::max(in.MaxDistance, 1.0f), std::clamp(in.MaxRoughness, 0.01f, 1.0f), (float)(in.Frame % 4096u));
            // Doku mip seviyesi için piksel açısı: yansıma ışınının ayak izi yaklaşık olarak görüş ışınınınki
            const float pixelAngle = 2.0f / (std::max(std::abs(in.ViewProj[1][1]), 1e-3f) * (float)std::max(h * 2, 1));
            glUniform1f(U(p, "u_PixelAngle"), pixelAngle);
            glUniform1i(U(p, "u_FxScene"), 5);
            glUniform1i(U(p, "u_FxNormal"), 6);
            glBindTextureUnit(5, in.Scene);
            glBindTextureUnit(6, in.Normal);
            glBindImageTexture(0, color, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glBindImageTexture(1, hit, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
            glDispatchCompute(((uint32_t)w + 7) / 8, ((uint32_t)h + 7) / 8, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
            for (GLuint unit = 0; unit <= 6; ++unit)
                glBindTextureUnit(unit, 0);
            glUseProgram(0);
            s.LastReflection = (uint32_t)(w * h);
            return true;
        }

        bool TraceAO(const AOInputs& in, uint32_t ao)
        {
            State& s = S();
            s.LastAO = 0;
            if (!s.AOTried)
            {
                s.AOTried = true;
                s.AO = RayTracingScene::CompileCompute("Ray Traced Ambient Occlusion", "layout(local_size_x = 8, local_size_y = 8) in;",
                                                       std::string(kFxCommon) + kAOMain);
            }
            GLint w, h;
            if (!s.AO || !in.Depth || !TargetSize(ao, w, h) || RayTracingScene::GetStats().Triangles == 0)
                return false;
            const uint32_t p = s.AO;
            RayTracingScene::Bind(p, 0, 1, 2);
            SetCommon(p, in.ViewProj, in.CameraPos, w, h, in.Depth);
            glUniform4f(U(p, "u_FxAO"), std::max(in.Radius, 0.01f), std::max(in.Intensity, 0.0f), std::max(in.Power, 0.01f), (float)std::clamp(in.Rays, 1, 64));
            glUniform1f(U(p, "u_FxFrame"), (float)(in.Frame % 64u));
            glBindImageTexture(0, ao, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
            glDispatchCompute(((uint32_t)w + 7) / 8, ((uint32_t)h + 7) / 8, 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
            glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
            for (GLuint unit = 0; unit <= 4; ++unit)
                glBindTextureUnit(unit, 0);
            glUseProgram(0);
            s.LastAO = (uint32_t)(w * h);
            return true;
        }

        void GetLastCounts(uint32_t& reflectionPixels, uint32_t& aoPixels)
        {
            reflectionPixels = S().LastReflection;
            aoPixels = S().LastAO;
        }
    }
}
