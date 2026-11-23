#pragma once

// Motor sürümü (Launcher, Hakkında penceresi, proje dosyaları, sürüm paketleri: Tools/Release/package.py ve make_manifest.py buradan okur).
// Kök CMakeLists.txt'teki project(... VERSION) ile aynı olmalı: yapılandırma farklıysa durur
#define CQ_ENGINE_VERSION "0.579.3"
#define CQ_ENGINE_VERSION_MAJOR 0
#define CQ_ENGINE_VERSION_MINOR 579
#define CQ_ENGINE_VERSION_PATCH 3

namespace Conqueror
{
    inline constexpr const char* EngineVersion = CQ_ENGINE_VERSION;
}
