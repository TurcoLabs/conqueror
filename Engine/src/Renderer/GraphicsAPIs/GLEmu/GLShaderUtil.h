#pragma once

// GL çeviri katmanlarının ortak GLSL dönüşümleri (VkGL, DxGL)

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace Conqueror::GLEmu
{
    inline size_t MatchingBracket(const std::string& s, size_t open)
    {
        int depth = 0;
        for (size_t i = open; i < s.size(); ++i)
        {
            if (s[i] == '[')
                ++depth;
            else if (s[i] == ']' && --depth == 0)
                return i;
        }
        return std::string::npos;
    }

    // Büyük birleşik örnekleyici dizilerini (sampler2D u_Textures[32]) ayrık doku dizisi + tek örnekleyiciye çevirir:
    //   uniform texture2D u_Textures_cqtex[32]; uniform sampler u_Textures_cqsmp;
    //   u_Textures[i] -> sampler2D(u_Textures_cqtex[i], u_Textures_cqsmp)
    // Direct3D 11 ve Metal aşama başına en çok 16 örnekleyici destekler. Dizideki tüm dokular aynı örnekleyici
    // durumunu kullanır (çizimdeki ilk geçerli dokununki).
    inline std::string SplitLargeSamplerArrays(const std::string& src, uint32_t maxCombined)
    {
        static const std::regex s_Decl(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+(?:(?:lowp|mediump|highp)\s+)?(sampler2D|samplerCube|isampler2D|usampler2D)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;)");
        struct Found
        {
            std::string Type, Name;
            uint32_t Count;
        };
        std::vector<Found> found;
        for (std::sregex_iterator m(src.begin(), src.end(), s_Decl), end; m != end; ++m)
        {
            const uint32_t count = (uint32_t)std::stoul((*m)[3].str());
            if (count > maxCombined)
                found.push_back({ (*m)[1].str(), (*m)[2].str(), count });
        }
        std::string s = src;
        for (const auto& f : found)
        {
            const std::string texType = f.Type == "samplerCube" ? "textureCube" : f.Type == "isampler2D" ? "itexture2D" : f.Type == "usampler2D" ? "utexture2D" : "texture2D";
            const std::regex decl("(?:layout\\s*\\([^)]*\\)\\s*)?uniform\\s+(?:(?:lowp|mediump|highp)\\s+)?" + f.Type + "\\s+" + f.Name + "\\s*\\[\\s*\\d+\\s*\\]\\s*;");
            s = std::regex_replace(s, decl, "uniform " + texType + " " + f.Name + "_cqtex[" + std::to_string(f.Count) + "]; uniform sampler " + f.Name + "_cqsmp;",
                                   std::regex_constants::format_first_only);
            const std::regex use("\\b" + f.Name + "\\s*\\[");
            std::string out;
            size_t pos = 0;
            std::smatch m;
            std::string rest = s;
            while (std::regex_search(rest, m, use))
            {
                const size_t at = pos + (size_t)m.position(0);
                const size_t open = at + (size_t)m.length(0) - 1;
                const size_t close = MatchingBracket(s, open);
                if (close == std::string::npos)
                    break;
                out += s.substr(pos, at - pos);
                out += f.Type + "(" + f.Name + "_cqtex" + s.substr(open, close - open + 1) + ", " + f.Name + "_cqsmp)";
                pos = close + 1;
                rest = s.substr(pos);
            }
            out += s.substr(pos);
            s = out;
        }
        return s;
    }

    // Motor dokularının sabit durumlu ortak örnekleyicileri (Direct3D 11 / 12, Metal ve WebGPU aşama başına 16 örnekleyici bağlar;
    // doku sınırı 31 - 128). Aydınlatmanın motor dokuları (BRDF LUT, düzlemsel yansıma, APV, Ray Traced GI atlasları, IBL ve yansıtma
    // probu küp haritaları) motorda hep aynı durumla kurulur: bunlar ayrık doku + gruptaki tek örnekleyiciye çevrilir, sabit durumu
    // katman verir (Unreal'in View.SharedBilinearClampedSampler'ı gibi). PBR / Terrain 19 - 22 birleşik örnekleyiciden 9 - 10'a iner,
    // bu API'lerde de yansıtma probu, APV, düzlemsel yansıma ve dinamik GI örneklenir. Gruplar (SamplerBinding::FixedSampler):
    //   0: doğrusal, kenara kenetli, mip yok (motorun GL_LINEAR + GL_CLAMP_TO_EDGE dokuları; texelFetch okunanlar örnekleyici kullanmaz)
    //   1: üç doğrusal (mip'li), kenara kenetli (önceden süzülmüş küp haritaları: IBL, yansıtma probları)
    //   uniform sampler2D u_BRDFLUT;   ->  uniform sampler cqsmp_fixed0; uniform texture2D u_BRDFLUT_cqgt0;
    //   u_BRDFLUT                      ->  sampler2D(u_BRDFLUT_cqgt0, cqsmp_fixed0)
    enum class FixedSamplerKind : int
    {
        LinearClamp = 0,
        TrilinearClamp = 1,
        Count
    };

    struct GroupedSampler
    {
        const char* Name;
        FixedSamplerKind Kind;
    };

    inline const std::vector<GroupedSampler>& EngineGroupedSamplers()
    {
        static const std::vector<GroupedSampler> s_List = {
            { "u_BRDFLUT", FixedSamplerKind::LinearClamp },             // EnvironmentMap::GetBRDFLUT: LINEAR, CLAMP_TO_EDGE
            { "u_PlanarReflection", FixedSamplerKind::LinearClamp },    // düzlemsel yansıma hedefi (Scene.cpp): LINEAR, CLAMP_TO_EDGE, 1 mip
            { "u_GIIrradiance", FixedSamplerKind::LinearClamp },        // RayTracedGI MakeTexture(linear): LINEAR, CLAMP_TO_EDGE
            { "u_GIDistance", FixedSamplerKind::LinearClamp },
            { "u_GIProbeData", FixedSamplerKind::LinearClamp },         // texelFetch
            { "u_APVPositionTex", FixedSamplerKind::LinearClamp },      // texelFetch
            { "u_APVSHTex", FixedSamplerKind::LinearClamp },            // texelFetch
            { "u_IBLPrefiltered", FixedSamplerKind::TrilinearClamp },   // EnvironmentMap prefiltered: LINEAR_MIPMAP_LINEAR, CLAMP_TO_EDGE
            { "u_ReflectionProbes", FixedSamplerKind::TrilinearClamp }, // yakalanan EnvironmentMap'lerin prefiltered küpleri
        };
        return s_List;
    }

    // "<ad>_cqgt<k>" ayrık dokusu ise gruptaki örnekleyici türü (k), değilse -1; glName GL tarafındaki uniform adı
    inline int GroupedSamplerKindOf(const std::string& name, std::string* glName = nullptr)
    {
        if (name.size() < 7 || name.compare(name.size() - 6, 5, "_cqgt") != 0)
            return -1;
        const char k = name.back();
        if (k < '0' || k >= (char)('0' + (int)FixedSamplerKind::Count))
            return -1;
        if (glName)
            *glName = name.substr(0, name.size() - 6);
        return k - '0';
    }

    inline std::string GroupedSamplerName(int kind) { return "cqsmp_fixed" + std::to_string(kind); }

    inline std::string GroupEngineSamplers(const std::string& src)
    {
        std::string s = src;
        bool declared[(int)FixedSamplerKind::Count] = {};
        for (const auto& g : EngineGroupedSamplers())
        {
            const std::regex decl("(?:layout\\s*\\([^)]*\\)\\s*)?uniform\\s+(?:(?:lowp|mediump|highp)\\s+)?(sampler2D|samplerCube)\\s+" + std::string(g.Name) +
                                  "\\s*(\\[\\s*\\w+\\s*\\])?\\s*;");
            std::smatch m;
            if (!std::regex_search(s, m, decl))
                continue;
            const int kind = (int)g.Kind;
            const std::string type = m[1].str();
            const std::string array = m[2].str();
            const std::string tex = std::string(g.Name) + "_cqgt" + std::to_string(kind);
            const std::string smp = GroupedSamplerName(kind);
            // Grubun örnekleyicisi ilk üyenin bildirimi yerinde (her kullanım kendi bildiriminden sonra gelir, o da bundan sonra)
            const std::string repl = (declared[kind] ? std::string() : "uniform sampler " + smp + "; ") + "uniform " +
                                     (type == "samplerCube" ? "textureCube " : "texture2D ") + tex + array + ";";
            declared[kind] = true;
            const size_t at = (size_t)m.position(0), len = (size_t)m.length(0);
            s = s.substr(0, at) + repl + s.substr(at + len);
            // Kullanımlar: dizi ise u_X[i] -> type(u_X_cqgtk[i], smp), değilse u_X -> type(u_X_cqgtk, smp)
            const std::regex use("\\b" + std::string(g.Name) + "\\b");
            std::string out;
            size_t pos = 0;
            for (;;)
            {
                std::smatch u;
                const std::string rest = s.substr(pos);
                if (!std::regex_search(rest, u, use))
                    break;
                const size_t ua = pos + (size_t)u.position(0);
                size_t end = ua + (size_t)u.length(0);
                std::string index;
                if (!array.empty())
                {
                    size_t open = end;
                    while (open < s.size() && (s[open] == ' ' || s[open] == '\t'))
                        ++open;
                    if (open >= s.size() || s[open] != '[')
                    {
                        // Dizinin kendisi (indissiz kullanım): dönüştürülemez, olduğu gibi bırakılır (derleyici hatası gösterir)
                        out += s.substr(pos, end - pos);
                        pos = end;
                        continue;
                    }
                    const size_t close = MatchingBracket(s, open);
                    if (close == std::string::npos)
                        break;
                    index = s.substr(open, close - open + 1);
                    end = close + 1;
                }
                out += s.substr(pos, ua - pos) + type + "(" + tex + index + ", " + smp + ")";
                pos = end;
            }
            out += s.substr(pos);
            s = out;
        }
        return s;
    }
}
