#!/usr/bin/env bash
# 316: DirectX Shader Compiler çalışma zamanı (dxcompiler.dll + dxil.dll, yalnız Windows). Direct3D 12'de donanım ışın sorgusu (DXR 1.1
# satır içi ışın izleme) Shader Model 6.5 ister: çeviri katmanı ışın sorgulu hesaplama gölgelendiricilerini DXC ile DXIL'e derler, dxil.dll
# onu imzalar. DLL'ler editörün (build/bin) yanında yoksa Direct3D 12 ışın izlemeyi hesaplama BVH'si ile yapar. dxcompiler.dll LLVM /
# NCSA ve MIT, dxil.dll Microsoft'un dağıtım lisansıyla oyunla birlikte dağıtılabilir (lisans dosyaları DLL'lerle birlikte kopyalanır).
# CMake: -DCQ_DXC_DIR=<klasör> derlemede bin/'e kopyalar; Build Settings oyuncunun yanındaki DLL'leri Windows oyunlarına taşır.
#
#   Tools/DXC/fetch-dxc.sh <klasör> [x64|arm64]
set -euo pipefail
DXC_TAG="v1.9.2609"
DXC_ZIP="dxc_2026_09_29.zip"
DEST="${1:?usage: $0 <folder> [x64|arm64]}"
ARCH="${2:-x64}"
case "$ARCH" in x64|arm64) ;; *) echo "arch must be x64 or arm64" >&2; exit 1 ;; esac
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
curl -sSfL -o "$TMP/dxc.zip" "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_ZIP}"
mkdir -p "$DEST"
PY_BIN="python3"; command -v python3 >/dev/null 2>&1 || PY_BIN="python"
"$PY_BIN" -I - "$TMP/dxc.zip" "$DEST" "$ARCH" <<'PY'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
arch = sys.argv[3]
for n in z.namelist():
    p = n.replace('\\', '/')
    base = p.split('/')[-1]
    if p.startswith('bin/%s/' % arch) and base in ('dxcompiler.dll', 'dxil.dll'):
        open(os.path.join(sys.argv[2], base), 'wb').write(z.read(n))
    elif ('LICENSE' in base.upper() or 'LICENCE' in base.upper()) and not p.startswith('inc/'):
        open(os.path.join(sys.argv[2], 'DXC-' + base), 'wb').write(z.read(n))
PY
for f in dxcompiler.dll dxil.dll; do [ -f "$DEST/$f" ] || { echo "$f ($ARCH) not found in $DXC_ZIP" >&2; exit 1; }; done
echo "dxcompiler.dll, dxil.dll ($ARCH) and their licenses are in $DEST"
