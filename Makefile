# Desktop build. ROM-derived data lives in build/ and is never committed.
# Mac: brew install sdl3. Linux / Steam Deck: see tools/deck/build.sh (Steam Runtime container).
CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
CXXFLAGS += -Ithird_party/glm -Isrc/core/shim_include $(EXTRA_CFLAGS)
LDLIBS ?= -lz
# the compiler for tools run during the build (m68kmake): the target's own unless cross-compiling
HOSTCC ?= $(CC)
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null || echo -I/opt/homebrew/include)
SDL_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null || echo -L/opt/homebrew/lib -lSDL3)
FT_CFLAGS ?= $(shell pkg-config --cflags freetype2 2>/dev/null || echo -I/opt/homebrew/include/freetype2)
FT_LIBS ?= $(shell pkg-config --libs freetype2 2>/dev/null || echo -L/opt/homebrew/lib -lfreetype)
ARCH ?= $(shell uname -m)
ifneq ($(filter arm64 aarch64,$(ARCH)),)
CORO_ASM = src/rt/coro_arm64.S
else ifeq ($(TARGET_OS),windows)
CORO_ASM = src/rt/coro_win64.S
else
CORO_ASM = src/rt/coro_x86_64.S
endif

CORE = src/core/v60_device.cpp src/core/mb86233.cpp
SOUND = src/machine/m1audio.cpp src/machine/multipcm.cpp third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_ssg.cpp
MUSASHI_OBJ = $(OBJ)/musashi/m68kcpu.o $(OBJ)/musashi/m68kops.o $(OBJ)/musashi/m68kdasm.o $(OBJ)/musashi/softfloat.o
MACHINE = src/machine/model1.cpp src/machine/model1_tgp.cpp src/machine/model1_tgp_lle.cpp src/machine/model1_video.cpp src/machine/model1_sky.cpp src/machine/model1_interp.cpp src/machine/model1_gpu.cpp src/machine/model1_edge_aa.cpp src/machine/model1io.cpp
Z80_OBJ = $(OBJ)/z80/z80.o
DASM = third_party/mame/v60/v60d.cpp
# the recompile done on the player's machine: analysis + native code (sljit, BSD-2)
NATIVE = src/rt/analyse.cpp src/rt/native.cpp $(DASM)
SLJIT_OBJ = $(OBJ)/third_party/sljit/sljitLir.o

OBJ ?= build/obj
BIN ?= build/bin
# the version and date shown in the menu: the git tag (or tag-commits-hash between releases) and the
# date of the last commit. Builds without git (a container) are handed them in the environment.
VF_VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
VF_DATE ?= $(shell git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
# (the commit too: what a GPU failure is remembered by, src/host/main.cpp's BUILD)
VF_COMMIT ?= $(shell git rev-parse --short HEAD 2>/dev/null)
# rewritten only when they change, so main is rebuilt for a new version and not otherwise
$(OBJ)/version.h: FORCE
	@mkdir -p $(OBJ)
	@printf '#define VF_VERSION "%s"\n#define VF_DATE "%s"\n#define VF_COMMIT "%s"\n' "$(VF_VERSION)" "$(VF_DATE)" "$(VF_COMMIT)" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
FORCE:
objs = $(patsubst %.cpp,$(OBJ)/%.o,$(1))

all: $(BIN)/vf $(BIN)/vfrun $(BIN)/vfdis $(BIN)/vfcov

RT = src/rt/rec_rt.cpp src/rt/coro.cpp
GEN = build/gen/vf_rec.cpp

# Musashi: m68kmake generates the opcode tables from m68k_in.c
$(BIN)/m68kmake: third_party/musashi/m68kmake.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O1 -w $< -o $@
build/musashi/m68kops.c: $(BIN)/m68kmake third_party/musashi/m68k_in.c
	@mkdir -p build/musashi
	$(BIN)/m68kmake build/musashi third_party/musashi/m68k_in.c >/dev/null
MUSASHI_CFLAGS = -O2 -w -Ithird_party/musashi -Ibuild/musashi
$(OBJ)/musashi/m68kops.o: build/musashi/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(MUSASHI_CFLAGS) -c $< -o $@
$(OBJ)/musashi/m68kcpu.o: third_party/musashi/m68kcpu.c build/musashi/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(MUSASHI_CFLAGS) -c $< -o $@
$(OBJ)/musashi/m68kdasm.o: third_party/musashi/m68kdasm.c build/musashi/m68kops.c
	@mkdir -p $(dir $@)
	$(CC) $(MUSASHI_CFLAGS) -c $< -o $@
$(OBJ)/musashi/softfloat.o: third_party/musashi/softfloat/softfloat.c
	@mkdir -p $(dir $@)
	$(CC) $(MUSASHI_CFLAGS) -c $< -o $@

$(SLJIT_OBJ): third_party/sljit/sljitLir.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -DSLJIT_CONFIG_AUTO=1 -c $< -o $@

$(OBJ)/z80/z80.o: third_party/z80/z80.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -c $< -o $@

$(OBJ)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(BIN)/v60recomp: $(call objs,tools/recomp/v60recomp.cpp src/rt/analyse.cpp $(DASM))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

# every coverage run in build/cov counts (their union)
COVS = $(wildcard build/cov/*.txt)
comma = ,
empty =
space = $(empty) $(empty)
build/gen/vf_prog.txt: $(BIN)/v60recomp build/vf/maincpu.bin $(COVS)
	@mkdir -p $(dir $@)
	$(BIN)/v60recomp build/vf/maincpu.bin $(subst $(space),$(comma),$(COVS)) $@

$(GEN): build/gen/vf_prog.txt tools/recomp/emit.py
	python3 tools/recomp/emit.py build/vf/maincpu.bin build/gen/vf_prog.txt $@

$(OBJ)/build/gen/vf_rec.o: $(GEN)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -O1 -Isrc/rt -c $< -o $@

$(BIN)/vflock: $(call objs,tools/vflock.cpp $(CORE) $(MACHINE) $(SOUND) $(RT) $(NATIVE)) $(SLJIT_OBJ) $(MUSASHI_OBJ) $(Z80_OBJ) $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM)) $(OBJ)/build/gen/vf_rec.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

rec: $(BIN)/vflock

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(OBJ)/tools/vfrun.o $(OBJ)/tools/vfscript.o: CXXFLAGS += $(SDL_CFLAGS)
$(BIN)/vfrun: $(call objs,tools/vfrun.cpp src/host/stb_image_impl.cpp $(CORE) $(MACHINE) $(SOUND) $(RT)) $(MUSASHI_OBJ) $(Z80_OBJ) $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM)) $(OBJ)/build/gen/vf_rec.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS)

# RmlUi (MIT): the in-game menu. Its core and the SDL backend, built with FreeType.
RML_SRC = $(wildcard third_party/rmlui/Source/Core/*.cpp third_party/rmlui/Source/Core/*/*.cpp) \
	third_party/rmlui/Backends/RmlUi_Platform_SDL.cpp third_party/rmlui/Backends/RmlUi_Renderer_SDL.cpp
RML_CFLAGS = -DRMLUI_STATIC_LIB -DRMLUI_FONT_ENGINE_FREETYPE -DRMLUI_SDL_VERSION_MAJOR=3 -Ithird_party/rmlui/Include -Ithird_party/rmlui/Backends
$(OBJ)/third_party/rmlui/%.o: CXXFLAGS += $(RML_CFLAGS) $(FT_CFLAGS) $(SDL_CFLAGS) -w
$(OBJ)/src/host/stb_image_impl.o: CXXFLAGS += -w
$(OBJ)/src/host/main.o $(OBJ)/src/host/menu.o: CXXFLAGS += $(SDL_CFLAGS) $(RML_CFLAGS)
$(OBJ)/src/host/main.o: $(OBJ)/version.h
$(OBJ)/src/host/main.o: CXXFLAGS += -I$(OBJ)
# The 3D on the GPU in the game (src/gpu, Display > 3D renderer): Metal for an Apple target; SDL_GPU
# elsewhere (Direct3D 12 or Vulkan: gpu3d_sdlgpu.cpp, its shaders precompiled in gpu3d_shaders.h by
# tools/gpu/shaders.sh); GPU3D=none: the CPU's only
GPU3D ?= $(if $(findstring apple,$(shell $(CXX) -dumpmachine 2>/dev/null)),metal,sdlgpu)
ifeq ($(GPU3D),metal)
GPU3D_OBJ = $(OBJ)/src/gpu/gpu3d_metal.o
GPU3D_LIBS = -framework Metal -framework Foundation -framework QuartzCore
GPU3D_DEF = -DVF_GPU3D_METAL=1
endif
ifeq ($(GPU3D),sdlgpu)
GPU3D_OBJ = $(OBJ)/src/gpu/gpu3d_sdlgpu.o
$(OBJ)/src/gpu/gpu3d_sdlgpu.o: CXXFLAGS += $(SDL_CFLAGS)
GPU3D_DEF = -DVF_GPU3D_SDL=1
endif
$(OBJ)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -MMD -c $< -o $@
$(OBJ)/src/host/main.o: CXXFLAGS += $(GPU3D_DEF)
$(BIN)/vf: $(call objs,src/host/main.cpp src/host/menu.cpp src/host/romset.cpp src/host/stb_image_impl.cpp $(CORE) $(MACHINE) $(SOUND) $(RT) $(RML_SRC)) $(MUSASHI_OBJ) $(Z80_OBJ) $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM)) $(OBJ)/build/gen/vf_rec.o $(GPU3D_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS) $(FT_LIBS) $(GPU3D_LIBS)

# The release build: no generated C++ in it (nothing made from the ROM). The game is recompiled
# to native code from the player's ROM when it starts (src/rt/native.cpp, assets/vf_map.txt).
$(OBJ)/src/host/main_native.o: src/host/main.cpp $(OBJ)/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $(RML_CFLAGS) $(GPU3D_DEF) -I$(OBJ) -DVF_NATIVE -MMD -c $< -o $@
$(BIN)/vf-native: $(OBJ)/src/host/main_native.o $(call objs,src/host/menu.cpp src/host/romset.cpp src/host/stb_image_impl.cpp src/rt/rec_table_none.cpp $(CORE) $(MACHINE) $(SOUND) $(RT) $(NATIVE) $(RML_SRC)) $(SLJIT_OBJ) $(MUSASHI_OBJ) $(Z80_OBJ) $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM)) $(GPU3D_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS) $(FT_LIBS) $(GPU3D_LIBS)
native: $(BIN)/vf-native

$(BIN)/vfcov: $(call objs,tools/vfcov.cpp $(CORE) $(MACHINE) $(SOUND)) $(MUSASHI_OBJ) $(Z80_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/vfscript: $(call objs,tools/vfscript.cpp src/host/stb_image_impl.cpp $(CORE) $(MACHINE) $(SOUND) $(RT)) $(MUSASHI_OBJ) $(Z80_OBJ) $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM)) $(OBJ)/build/gen/vf_rec.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS)

$(BIN)/vfdis: $(call objs,tools/vfdis.cpp $(DASM))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

regions: build/vf/maincpu.bin
build/vf/maincpu.bin: roms/vf.zip tools/romset.py
	python3 tools/romset.py roms/vf.zip build/vf

clean:
	rm -rf build/obj build/bin

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
.PHONY: all clean regions
