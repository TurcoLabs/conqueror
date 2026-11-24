#!/usr/bin/env bash
# 316: DirectX Shader Compiler çalışma zamanı (dxcompiler.dll + dxil.dll, yalnız Windows). Direct3D 12'de donanım ışın sorgusu (DXR 1.1
# satır içi ışın izleme) Shader Model 6.5 ister: çeviri katmanı ışın sorgulu hesaplama gölgelendiricilerini DXC ile DXIL'e derler, dxil.dll
# onu imzalar. DLL'ler editörün (build/bin) yanında yoksa Direct3D 12 ışın izlemeyi hesaplama BVH'si ile yapar. dxcompiler.dll LLVM /
# NCSA ve MIT, dxil.dll Microsoft'un dağıtım lisansıyla oyunla birlikte dağıtılabilir (lisans dosyaları DLL'lerle birlikte kopyalanır).
# CMake: -DCQ_DXC_DIR=<klasör> derlemede bin/'e kopyalar; Build Settings oyuncunun yanındaki DLL'leri Windows oyunlarına taşır.
#
#   Tools/DXC/fetch-dxc.sh <klasör>
set -euo pipefail
DXC_TAG="v1.9.2609"
DXC_ZIP="dxc_2026_09_29.zip"
DEST="${1:?usage: $0 <folder>}"
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT
curl -sSfL -o "$TMP/dxc.zip" "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_ZIP}"
mkdir -p "$DEST"
python3 -I - "$TMP/dxc.zip" "$DEST" <<'PY'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    p = n.replace('\\', '/')
    base = p.split('/')[-1]
    if p.startswith('bin/x64/') and base in ('dxcompiler.dll', 'dxil.dll'):
        open(os.path.join(sys.argv[2], base), 'wb').write(z.read(n))
    elif ('LICENSE' in base.upper() or 'LICENCE' in base.upper()) and not p.startswith('inc/'):
        open(os.path.join(sys.argv[2], 'DXC-' + base), 'wb').write(z.read(n))
PY
echo "dxcompiler.dll, dxil.dll and their licenses are in $DEST"
