#!/bin/bash
# The GPU's 3D shaders for SDL_GPU (src/gpu/gpu3d.hlsl), and Low antialiasing's edge pass for SDL's
# gpu renderer (src/gpu/edge_aa.hlsl, a fragment shader), compiled with Microsoft's DXC into SPIR-V
# (Vulkan: Linux, the Steam Deck, Windows without Direct3D 12) and DXIL (Direct3D 12: Windows),
# and from the SPIR-V into MSL (Metal: Macs) with SPIRV-Cross, into src/gpu/gpu3d_shaders.h, which is
# committed: the builds don't need DXC. Run after changing gpu3d.hlsl.
#   DXC=/path/to/dxc tools/gpu/shaders.sh     (DXC's Linux release has libdxil.so next to it, which
#                                             signs the DXIL as Direct3D 12 wants it)
# Without DXC here (a Mac): DXC_LINUX=dir/with/bin/dxc runs it in an x86-64 Linux container.
# NO_DXC=1: the SPIR-V and DXIL as the committed header has them (only the MSL made again).
#   SPIRV_CROSS=/path/to/spirv-cross (default: spirv-cross)
set -e
cd "$(dirname "$0")/../.."
OUT=build/shaders
mkdir -p $OUT
KERNELS="clear rows solid shadow pixels target"
FLAGS="-T cs_6_0 -E main -HV 2021 -O3"
PS_FLAGS="-T ps_6_0 -E main -HV 2021 -O3"   # (-Gis, IEEE strictness, for DXIL: SPIR-V's would need Vulkan 1.2; `precise` does it there)
if [ -n "$NO_DXC" ]; then
	python3 - "$OUT" <<'PY'
import re, sys
s = open("src/gpu/gpu3d_shaders.h").read()
for m in re.finditer(r'gpu3d_(\w+)_(spv|dxil)\[\d+\] = \{([^}]*)\}', s):
    open(f"{sys.argv[1]}/{m.group(1)}.{m.group(2)}", "wb").write(bytes(int(x) for x in m.group(3).replace("\n", " ").replace("\t", " ").split(",") if x.strip()))
PY
elif [ -z "$DXC" ] && [ -n "$DXC_LINUX" ]; then
	docker run --rm --platform linux/amd64 -v "$PWD":/src -v "$DXC_LINUX":/dxc -w /src -e LD_LIBRARY_PATH=/dxc/lib ubuntu:24.04 \
		bash -c "for k in $KERNELS; do K=\$(echo \$k | tr a-z A-Z); \
			/dxc/bin/dxc $FLAGS -D K_\$K -spirv -fspv-target-env=vulkan1.0 -Fo $OUT/\$k.spv src/gpu/gpu3d.hlsl || exit 1; \
			/dxc/bin/dxc $FLAGS -D K_\$K -Gis -Fo $OUT/\$k.dxil src/gpu/gpu3d.hlsl || exit 1; done; \
		/dxc/bin/dxc $PS_FLAGS -spirv -fspv-target-env=vulkan1.0 -Fo $OUT/edge_aa.spv src/gpu/edge_aa.hlsl || exit 1; \
		/dxc/bin/dxc $PS_FLAGS -Gis -Fo $OUT/edge_aa.dxil src/gpu/edge_aa.hlsl || exit 1"
else
	DXC=${DXC:-dxc}
	for k in $KERNELS; do
		K=$(echo $k | tr a-z A-Z)
		$DXC $FLAGS -D K_$K -spirv -fspv-target-env=vulkan1.0 -Fo $OUT/$k.spv src/gpu/gpu3d.hlsl
		$DXC $FLAGS -D K_$K -Gis -Fo $OUT/$k.dxil src/gpu/gpu3d.hlsl
	done
	$DXC $PS_FLAGS -spirv -fspv-target-env=vulkan1.0 -Fo $OUT/edge_aa.spv src/gpu/edge_aa.hlsl
	$DXC $PS_FLAGS -Gis -Fo $OUT/edge_aa.dxil src/gpu/edge_aa.hlsl
fi
# MSL from the SPIR-V: each compute kernel's bindings as SDL_GPU's Metal has them (tools/gpu/msl_bind.py), the edge
# pass's as they are (a fragment shader's textures in order, as the SPIR-V's); "main0", SPIRV-Cross's own name
SPIRV_CROSS=${SPIRV_CROSS:-spirv-cross}
for k in $KERNELS; do
	$SPIRV_CROSS --reflect --output $OUT/$k.json $OUT/$k.spv
	python3 tools/gpu/msl_bind.py $OUT/$k.spv $OUT/$k.mtl.spv $OUT/$k.json >/dev/null
	$SPIRV_CROSS --msl --msl-version 20100 --msl-decoration-binding --rename-entry-point main main0 comp $OUT/$k.mtl.spv --output $OUT/$k.msl
done
$SPIRV_CROSS --msl --msl-version 20100 --msl-decoration-binding --rename-entry-point main main0 frag $OUT/edge_aa.spv --output $OUT/edge_aa.msl
python3 - "$OUT" $KERNELS edge_aa > src/gpu/gpu3d_shaders.h <<'PY'
import sys, hashlib
out, kernels = sys.argv[1], sys.argv[2:]
print("// Made by tools/gpu/shaders.sh from src/gpu/gpu3d.hlsl and edge_aa.hlsl (DXC, SPIRV-Cross): don't edit.")
print("#pragma once")
print("#include <cstddef>")
print("#include <cstdint>")
for k in kernels:
    for ext in ("spv", "dxil"):
        data = open(f"{out}/{k}.{ext}", "rb").read()
        print(f"alignas(4) static const uint8_t gpu3d_{k}_{ext}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            print("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
        print("};")
    msl = open(f"{out}/{k}.msl", "rb").read() + b"\0"   # (MSL is source: its size less this end)
    print(f"static const char gpu3d_{k}_msl[{len(msl)}] = {{")
    for i in range(0, len(msl), 24):
        print("\t" + ",".join(str(b) for b in msl[i:i + 24]) + ",")
    print("};")
src = open("src/gpu/gpu3d.hlsl", "rb").read() + open("src/gpu/edge_aa.hlsl", "rb").read()
print(f'static const char gpu3d_shaders_source_sha1[] = "{hashlib.sha1(src).hexdigest()}";')
PY
ls -la src/gpu/gpu3d_shaders.h
