#!/bin/sh
# Adds the "mali" video driver (SDL_malivideo.c, PowerVR fbdev EGL of the TrimUI consoles) to an SDL3
# source tree: apply.sh <SDL3 source dir>. Idempotent.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
S=$1
[ -f "$S/src/video/SDL_video.c" ] || { echo "not an SDL3 tree: $S"; exit 1; }
mkdir -p "$S/src/video/mali"
cp "$HERE/SDL_malivideo.c" "$S/src/video/mali/"
grep -q MALI_bootstrap "$S/src/video/SDL_sysvideo.h" || \
  sed -i 's/^extern VideoBootStrap VIVANTE_bootstrap;/extern VideoBootStrap VIVANTE_bootstrap;\nextern VideoBootStrap MALI_bootstrap;/' "$S/src/video/SDL_sysvideo.h"
grep -q MALI_bootstrap "$S/src/video/SDL_video.c" || \
  sed -i 's/^#ifdef SDL_VIDEO_DRIVER_VIVANTE$/#ifdef SDL_VIDEO_DRIVER_MALI\n    \&MALI_bootstrap,\n#endif\n#ifdef SDL_VIDEO_DRIVER_VIVANTE/' "$S/src/video/SDL_video.c"
grep -q SDL_VIDEO_DRIVER_MALI "$S/include/build_config/SDL_build_config.h.cmake" || \
  sed -i 's/^#cmakedefine SDL_VIDEO_DRIVER_VIVANTE 1$/#cmakedefine SDL_VIDEO_DRIVER_VIVANTE 1\n#cmakedefine SDL_VIDEO_DRIVER_MALI 1/' "$S/include/build_config/SDL_build_config.h.cmake"
grep -q SDL_MALI_TRIMUI "$S/CMakeLists.txt" || \
  sed -i 's/^    CheckVivante()$/    CheckVivante()\n    if(SDL_MALI_TRIMUI)\n      set(SDL_VIDEO_DRIVER_MALI 1)\n      set(HAVE_SDL_VIDEO TRUE)\n      sdl_glob_sources("${SDL3_SOURCE_DIR}\/src\/video\/mali\/*.c")\n    endif()/' "$S/CMakeLists.txt"
for f in src/video/SDL_sysvideo.h src/video/SDL_video.c include/build_config/SDL_build_config.h.cmake CMakeLists.txt; do
  grep -q 'MALI' "$S/$f" || { echo "patch failed: $f"; exit 1; }
done
echo "mali driver added to $S"
