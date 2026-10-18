// Virtua Fighter, as the shared code knows it (src/games/game.h): registered at start-up.
#include "profile.h"
#include "gameplay.h"
#include "tgp_hle.h"
#include "../script.h"

namespace game::vf {

std::unique_ptr<auto_play> make_auto_play(const std::string &scenario, uint32_t seed, bool checks);
bool script_macro(const std::string &cmd, std::istream &args, macro &out);

namespace {
const registration REGISTERED{ {
	&PROFILE, &IO_PROFILE, &VIDEO,
	create_vf_tgp_hle,
	create_gameplay,
	make_auto_play,
	script_macro,
} };
}

}
