# Virtua Fighter's build settings (the Makefile's GAME_RULES make its targets from these)
GAMES += vf
# the desktop build (build/bin/vf) links the generated C++ (build/gen/vf_rec.cpp)
vf_HOST_REC = gen
# no scripted coverage runs: the recompile's hints are build/cov/vf/runtime_entries.txt
vf_COV_SCENARIOS =
vf_COV_FRAMES ?= 20000
vf_LOCK_SCENARIOS = default
vf_LOCK_FRAMES ?= 18000

# the repository's first game keeps the short names
native: vf-native
regions: vf-regions
rec: $(BIN)/vflock-static

# its tests (tests/games/vf)
$(OBJ)/tests/games/vf/profile_test.o: CXXFLAGS += $(SDL_CFLAGS)
$(BIN)/vf_profile_test: $(OBJ)/tests/games/vf/profile_test.o $(GAMES_LINK)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)
test-vf-profile: $(BIN)/vf_profile_test
	$(BIN)/vf_profile_test
.PHONY: test-vf-profile
