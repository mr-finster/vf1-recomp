// Virtua Racing in the desktop host (src/host/game_host.h): its set and controls; the rest is the
// shared host's.
#include "../../host/game_host.h"
#include "controls.h"
#include "romset.h"

namespace game::vr {
namespace {

class host final : public game_host
{
public:
	const romset::definition &romset() const override { return ROMSET; }
	const control_profile &controls() const override { return CONTROLS; }
};

}
}

std::unique_ptr<game_host> make_game_host() { return std::make_unique<game::vr::host>(); }
