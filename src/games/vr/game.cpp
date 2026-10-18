// Virtua Racing, as the shared code knows it (src/games/game.h): registered at start-up. It runs its
// TGP's own program (no HLE table) and has no script bots.
#include "profile.h"
#include "gameplay.h"
#include "../script.h"

namespace game::vr {

std::unique_ptr<auto_play> make_auto_play(const std::string &scenario, uint32_t seed, bool checks);

namespace {
const registration REGISTERED{ {
	&PROFILE, &IO_PROFILE, &VIDEO,
	nullptr,
	[](model1 &) { return create_gameplay(); },
	make_auto_play,
	nullptr,
} };
}

}
