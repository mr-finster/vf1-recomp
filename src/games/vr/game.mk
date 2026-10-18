# Virtua Racing's build settings (the Makefile's GAME_RULES make its targets from these)
GAMES += vr
# the desktop build (build/bin/vr) runs on the interpreter until more of the game is covered
vr_HOST_REC = none
# the scripted runs model1cov records for the recompile (make vr-coverage, vr-static)
vr_COV_SCENARIOS = default alternate
vr_COV_FRAMES ?= 20000
vr_LOCK_SCENARIOS = default alternate
vr_LOCK_FRAMES ?= 22000

# its tests (tests/games/vr)
$(OBJ)/tests/games/vr/profile_test.o: CXXFLAGS += $(SDL_CFLAGS)
$(BIN)/vr_profile_test: $(OBJ)/tests/games/vr/profile_test.o $(GAMES_LINK)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)
test-vr-profile: $(BIN)/vr_profile_test
	$(BIN)/vr_profile_test

$(BIN)/vr_gameplay_test: tests/games/vr/gameplay_test.cpp src/games/vr/gameplay.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@
test-vr-gameplay: $(BIN)/vr_gameplay_test
	$(BIN)/vr_gameplay_test

$(BIN)/vr_tgp_track_test: tests/games/vr/tgp_track_test.cpp src/games/vr/tgp.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@
test-vr-tgp-track: $(BIN)/vr_tgp_track_test
	$(BIN)/vr_tgp_track_test
.PHONY: test-vr-profile test-vr-gameplay test-vr-tgp-track
