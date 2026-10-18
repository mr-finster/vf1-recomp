# Desktop build. ROM-derived data lives in build/ and is never committed.
# Mac: brew install sdl3. Linux / Steam Deck: see tools/linux/package.sh (Steam Runtime container).
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
# the player's MAME set read at launch: the shared loader (each host adds its game's tables)
ROMSET = src/machine/model1_romset.cpp
# the games: each folder in src/games registers itself (src/games/<name>/game.cpp), so every binary
# has them all; only a host's own part (host.cpp) and its ROM table (romset.cpp) are one game's
GAMES_SRC = src/games/game.cpp $(filter-out %/host.cpp %/romset.cpp,$(wildcard src/games/*/*.cpp))
# the tools' game parts (tools/tool_game.h: tools/games/<name>/tool.cpp), chosen at run time
TOOL_GAME = tools/tool_game.cpp $(wildcard tools/games/*/tool.cpp)
MACHINE = src/machine/model1.cpp src/machine/model1_tgp.cpp src/machine/model1_tgp_lle.cpp src/machine/model1_video.cpp src/machine/model1_sky.cpp src/machine/model1_interp.cpp src/machine/model1_gpu.cpp src/machine/model1_edge_aa.cpp src/machine/model1io.cpp $(GAMES_SRC)
Z80_OBJ = $(OBJ)/z80/z80.o
DASM = third_party/mame/v60/v60d.cpp
# the recompile done on the player's machine: analysis + native code (sljit, BSD-2)
NATIVE = src/rt/analyse.cpp src/rt/native.cpp $(DASM)
SLJIT_OBJ = $(OBJ)/third_party/sljit/sljitLir.o

OBJ ?= build/obj
BIN ?= build/bin
# the version and date shown in the menu: the git tag (or tag-commits-hash between releases) and the
# date of the last commit. Builds without git (a container) are handed them in the environment.
M1_VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
M1_DATE ?= $(shell git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
# rewritten only when they change, so main is rebuilt for a new version and not otherwise
$(OBJ)/version.h: FORCE
	@mkdir -p $(OBJ)
	@printf '#define M1_VERSION "%s"\n#define M1_DATE "%s"\n' "$(M1_VERSION)" "$(M1_DATE)" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
FORCE:
objs = $(patsubst %.cpp,$(OBJ)/%.o,$(1))

# (and every game's desktop build: GAME_RULES below)
.DEFAULT_GOAL := all
all: $(BIN)/model1run $(BIN)/model1dis $(BIN)/model1cov

RT = src/rt/rec_rt.cpp src/rt/coro.cpp
CORO_OBJ = $(patsubst %.S,$(OBJ)/%.o,$(CORO_ASM))
# a build with no generated C++ in it: the interpreter, or the native code made at start-up
REC_NONE = $(OBJ)/src/rt/rec_table_none.o

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

comma = ,
empty =
space = $(empty) $(empty)

# the lockstep checker with no generated C++ in it: --native for any game (each game's own
# <game>lock-static has its generated C++: GAME_RULES)
$(BIN)/model1lock: $(call objs,tools/model1lock.cpp $(CORE) $(MACHINE) $(SOUND) $(RT) $(NATIVE)) $(REC_NONE) $(SLJIT_OBJ) $(MUSASHI_OBJ) $(Z80_OBJ) $(CORO_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(OBJ)/tools/model1run.o $(OBJ)/tools/model1script.o: CXXFLAGS += $(SDL_CFLAGS)
$(OBJ)/tools/games/%/tool.o: CXXFLAGS += $(SDL_CFLAGS)
MODEL1RUN_OBJS = $(call objs,tools/model1run.cpp $(TOOL_GAME) src/host/stb_image_impl.cpp $(CORE) $(MACHINE) $(SOUND) $(RT))
$(BIN)/model1run: $(MODEL1RUN_OBJS) $(REC_NONE) $(MUSASHI_OBJ) $(Z80_OBJ) $(CORO_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS)

# RmlUi (MIT): the in-game menu. Its core and the SDL backend, built with FreeType.
RML_SRC = $(wildcard third_party/rmlui/Source/Core/*.cpp third_party/rmlui/Source/Core/*/*.cpp) \
	third_party/rmlui/Backends/RmlUi_Platform_SDL.cpp third_party/rmlui/Backends/RmlUi_Renderer_SDL.cpp
RML_CFLAGS = -DRMLUI_STATIC_LIB -DRMLUI_FONT_ENGINE_FREETYPE -DRMLUI_SDL_VERSION_MAJOR=3 -Ithird_party/rmlui/Include -Ithird_party/rmlui/Backends
$(OBJ)/third_party/rmlui/%.o: CXXFLAGS += $(RML_CFLAGS) $(FT_CFLAGS) $(SDL_CFLAGS) -w
$(OBJ)/src/host/stb_image_impl.o: CXXFLAGS += -w
# the desktop host (src/host) and each game's part of it (src/games/<game>/host.cpp: a binary links one)
$(OBJ)/src/host/main.o $(OBJ)/src/host/menu.o: CXXFLAGS += $(SDL_CFLAGS) $(RML_CFLAGS)
$(OBJ)/src/games/%/host.o: CXXFLAGS += $(SDL_CFLAGS) $(RML_CFLAGS)
$(OBJ)/src/host/main.o: $(OBJ)/version.h
$(OBJ)/src/host/main.o: CXXFLAGS += -I$(OBJ)
# The 3D on the GPU in the game (src/gpu, Display > 3D renderer): Metal for an Apple target; SDL_GPU
# elsewhere (Direct3D 12 or Vulkan: gpu3d_sdlgpu.cpp, its shaders precompiled in gpu3d_shaders.h by
# tools/gpu/shaders.sh); GPU3D=none: the CPU's only
GPU3D ?= $(if $(findstring apple,$(shell $(CXX) -dumpmachine 2>/dev/null)),metal,sdlgpu)
ifeq ($(GPU3D),metal)
GPU3D_OBJ = $(OBJ)/src/gpu/gpu3d_metal.o
GPU3D_LIBS = -framework Metal -framework Foundation -framework QuartzCore
GPU3D_DEF = -DM1_GPU3D_METAL=1
endif
ifeq ($(GPU3D),sdlgpu)
GPU3D_OBJ = $(OBJ)/src/gpu/gpu3d_sdlgpu.o
$(OBJ)/src/gpu/gpu3d_sdlgpu.o: CXXFLAGS += $(SDL_CFLAGS)
GPU3D_DEF = -DM1_GPU3D_SDL=1
endif
$(OBJ)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -MMD -c $< -o $@
$(OBJ)/src/host/main.o: CXXFLAGS += $(GPU3D_DEF)
# The release build (<game>-native: GAME_RULES) has no generated C++ in it (nothing made from the ROM):
# the game is recompiled to native code from the player's ROM when it starts (src/rt/native.cpp,
# assets/games/<game>/<game>_map.txt).
$(OBJ)/src/host/main_native.o: src/host/main.cpp $(OBJ)/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $(RML_CFLAGS) $(GPU3D_DEF) -I$(OBJ) -DM1_NATIVE -MMD -c $< -o $@

$(BIN)/model1cov: $(call objs,tools/model1cov.cpp $(CORE) $(MACHINE) $(SOUND)) $(MUSASHI_OBJ) $(Z80_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

MODEL1SCRIPT_OBJS = $(call objs,tools/model1script.cpp $(TOOL_GAME) src/host/stb_image_impl.cpp $(CORE) $(MACHINE) $(SOUND) $(RT))
$(BIN)/model1script: $(MODEL1SCRIPT_OBJS) $(REC_NONE) $(MUSASHI_OBJ) $(Z80_OBJ) $(CORO_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS) $(SDL_LIBS)

$(BIN)/model1dis: $(call objs,tools/model1dis.cpp $(DASM))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BIN)/model1io_adc_test: tests/model1io_adc_test.cpp $(OBJ)/src/machine/model1io.o $(Z80_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

test-model1io-adc: $(BIN)/model1io_adc_test
	$(BIN)/model1io_adc_test

# (the games come from the registry, so a game's tests (its game.mk) link the machine with every game in it)
GAMES_LINK = $(call objs,$(CORE) $(MACHINE) $(SOUND)) $(MUSASHI_OBJ) $(Z80_OBJ)
# The launch-time ROM loader against tools/romset.py (needs the player's zips: those present are checked)
$(BIN)/romset_test: $(call objs,tests/romset_test.cpp $(ROMSET) $(wildcard src/games/*/romset.cpp))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

IOBOARD_ZIP = $(wildcard roms/model1io.zip)

# ---- The games ----
# Each game's folder has a game.mk with its name (GAMES += <game>) and these settings; GAME_RULES
# makes the rest, the same for every game:
#   <game>_HOST_REC        gen: the desktop build links the generated C++; none: the interpreter
#   <game>_COV_SCENARIOS   the scripted runs model1cov records as the recompile's hints ("default":
#                          the game's plain run; none: only the runtime entries)
#   <game>_COV_FRAMES      their length
#   <game>_LOCK_SCENARIOS  the runs <game>-lockstep and <game>-native-lockstep check
#   <game>_LOCK_FRAMES     their length
# The targets for a game g:
#   g                  build/bin/g, the desktop build (and build/g from roms/g.zip, when there)
#   g-native           build/bin/g-native, the release build: native code made at start-up
#   g-regions          build/g from roms/g.zip (tools/romset.py, its table tools/games/g/romset.py)
#   g-coverage         the recompile's hints in build/cov/g
#   g-static           build/bin/grun-static: model1run with g's generated C++ (build/gen/g_rec.cpp)
#   g-lockstep         the generated C++ against the interpreter (build/bin/glock-static)
#   g-native-lockstep  the start-up native code against the interpreter (model1lock --native)
#   build/bin/gscript-static  model1script with g's generated C++
GAMES :=
include $(wildcard src/games/*/game.mk)

scenario_flag = $(if $(filter default,$(1)),,--scenario=$(1))

define COV_RULE
build/cov/$(1)/coverage-$(2).txt: $$(BIN)/model1cov build/$(1)/maincpu.bin
	@mkdir -p $$(dir $$@)
	$$(BIN)/model1cov --game $(1) $(call scenario_flag,$(2)) build/$(1) $$($(1)_COV_FRAMES) $$@
endef

define GAME_RULES
$(1)_LOCAL_REGIONS = $$(if $$(wildcard roms/$(1).zip),build/$(1)/maincpu.bin)
build/$(1)/maincpu.bin: roms/$(1).zip tools/romset.py tools/games/$(1)/romset.py $$(IOBOARD_ZIP)
	python3 tools/romset.py --game $(1) roms/$(1).zip build/$(1)
$(1)-regions: build/$(1)/maincpu.bin

# the recompile's hints: the coverage runs, and the entries the generated code fell back to the
# interpreter for (src/rt/rec_rt.cpp notes them in build/cov/$(1)/runtime_entries.txt)
$(1)_HINTS = $$(foreach s,$$($(1)_COV_SCENARIOS),build/cov/$(1)/coverage-$$(s).txt) build/cov/$(1)/runtime_entries.txt
build/cov/$(1)/runtime_entries.txt:
	@mkdir -p $$(dir $$@)
	@touch $$@
$(1)-coverage: $$($(1)_HINTS)
build/gen/$(1)_listing.txt: $$(BIN)/v60recomp build/$(1)/maincpu.bin $$($(1)_HINTS)
	@mkdir -p $$(dir $$@)
	$$(BIN)/v60recomp build/$(1)/maincpu.bin $$(subst $$(space),$$(comma),$$(strip $$($(1)_HINTS))) $$@ build/gen/$(1)_map.txt
build/gen/$(1)_rec.cpp: build/gen/$(1)_listing.txt build/$(1)/maincpu.bin tools/recomp/emit.py
	python3 tools/recomp/emit.py build/$(1)/maincpu.bin $$< $$@
$$(OBJ)/build/gen/$(1)_rec.o: build/gen/$(1)_rec.cpp
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) -O1 -Isrc/rt -c $$< -o $$@

# the desktop host with the game's part of it and its ROM table
$$(BIN)/$(1): $$(call objs,src/host/main.cpp src/host/menu.cpp src/games/$(1)/host.cpp $$(ROMSET) src/games/$(1)/romset.cpp src/host/stb_image_impl.cpp $$(CORE) $$(MACHINE) $$(SOUND) $$(RT) $$(RML_SRC)) $$(MUSASHI_OBJ) $$(Z80_OBJ) $$(CORO_OBJ) $$(if $$(filter gen,$$($(1)_HOST_REC)),$$(OBJ)/build/gen/$(1)_rec.o,$$(REC_NONE)) $$(GPU3D_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDLIBS) $$(SDL_LIBS) $$(FT_LIBS) $$(GPU3D_LIBS)
$(1): $$(BIN)/$(1) $$($(1)_LOCAL_REGIONS)
all: $$(BIN)/$(1)

$$(BIN)/$(1)-native: $$(OBJ)/src/host/main_native.o $$(call objs,src/host/menu.cpp src/games/$(1)/host.cpp $$(ROMSET) src/games/$(1)/romset.cpp src/host/stb_image_impl.cpp $$(CORE) $$(MACHINE) $$(SOUND) $$(RT) $$(NATIVE) $$(RML_SRC)) $$(REC_NONE) $$(SLJIT_OBJ) $$(MUSASHI_OBJ) $$(Z80_OBJ) $$(CORO_OBJ) $$(GPU3D_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDLIBS) $$(SDL_LIBS) $$(FT_LIBS) $$(GPU3D_LIBS)
$(1)-native: $$(BIN)/$(1)-native $$($(1)_LOCAL_REGIONS)

# the headless tools with the game's generated C++ (run with M1_REC=1; the lockstep always)
$$(BIN)/$(1)run-static: $$(MODEL1RUN_OBJS) $$(OBJ)/build/gen/$(1)_rec.o $$(MUSASHI_OBJ) $$(Z80_OBJ) $$(CORO_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDLIBS) $$(SDL_LIBS)
$(1)-static: $$(BIN)/$(1)run-static
$$(BIN)/$(1)script-static: $$(MODEL1SCRIPT_OBJS) $$(OBJ)/build/gen/$(1)_rec.o $$(MUSASHI_OBJ) $$(Z80_OBJ) $$(CORO_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDLIBS) $$(SDL_LIBS)
$$(BIN)/$(1)lock-static: $$(OBJ)/tools/model1lock.o $$(call objs,$$(CORE) $$(MACHINE) $$(SOUND) $$(RT) $$(NATIVE)) $$(OBJ)/build/gen/$(1)_rec.o $$(SLJIT_OBJ) $$(MUSASHI_OBJ) $$(Z80_OBJ) $$(CORO_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$^ -o $$@ $$(LDLIBS)
$(1)-lockstep: $$(BIN)/$(1)lock-static
	$$(foreach s,$$($(1)_LOCK_SCENARIOS),$$(BIN)/$(1)lock-static --game $(1) build/$(1) $$($(1)_LOCK_FRAMES) --strict --scenarios $$(call scenario_flag,$$(s)) &&) true
$(1)-native-lockstep: $$(BIN)/model1lock $$($(1)_LOCAL_REGIONS)
	$$(foreach s,$$($(1)_LOCK_SCENARIOS),$$(BIN)/model1lock --game $(1) build/$(1) $$($(1)_LOCK_FRAMES) --native --strict --scenarios $$(call scenario_flag,$$(s)) &&) true

.PHONY: $(1) $(1)-native $(1)-regions $(1)-coverage $(1)-static $(1)-lockstep $(1)-native-lockstep
endef

$(foreach g,$(GAMES),$(foreach s,$($(g)_COV_SCENARIOS),$(eval $(call COV_RULE,$(g),$(s)))))
$(foreach g,$(GAMES),$(eval $(call GAME_RULES,$(g))))

test-romset: $(BIN)/romset_test $(foreach g,$(GAMES),$($(g)_LOCAL_REGIONS))
	@[ -n "$(wildcard $(GAMES:%=roms/%.zip))" ] || { echo "test-romset: none of $(GAMES:%=roms/%.zip)" >&2; exit 1; }
	@$(foreach g,$(GAMES),if [ -f roms/$(g).zip ]; then $(BIN)/romset_test $(g) roms/$(g).zip $(IOBOARD_ZIP) build/$(g) || exit 1; fi;) true

clean:
	rm -rf build/obj build/bin

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
.PHONY: all clean native regions rec test-romset test-model1io-adc
