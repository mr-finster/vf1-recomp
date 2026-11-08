// The game's own coin sound (Online > Settings > Match found chime, issue #44): what the cabinet plays when a coin goes
// in, made at run time by a sound board of its own (m1audio: its 68000, MultiPCMs and YM3438) from the ROM's sound
// program and samples (nothing of it stored). The main board asks for it with three bytes on the link, 9E 0A 7F
// (logged at two coins from the attract; alone on a fresh sound board it is the effect heard then). The sound is about
// 0.7 s; a fresh board plays it within a few frames.
//
// Made once at the start, before the game's own sound board exists: m1audio's 68000 is Musashi, one CPU a process (its
// memory callbacks find the board through a global), so a second board may only run while the game's isn't there yet;
// the game's board, made and reset after, starts its 68000 afresh. About 0.1 s.
#pragma once
#include "../machine/m1audio.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace coin_sound {

constexpr int RATE = m1audio::OUT_RATE;

// Stereo 16-bit at RATE, from the sound's first sample to where it has died away; empty if the board played nothing
inline std::vector<int16_t> make(std::vector<uint8_t> &sndcpu, std::vector<uint8_t> &pcm1, std::vector<uint8_t> &pcm2)
{
	if (sndcpu.empty() || pcm1.empty() || pcm2.empty()) return {};
	std::vector<int16_t> out;
	{
		m1audio b(sndcpu, pcm1, pcm2);
		b.reset();
		static const uint8_t coin[] = { 0x9E, 0x0A, 0x7F };
		size_t sent = 0;
		const double frame_us = 1e6 / 57.52;
		for (int f = 0; f < 90; f++)   // (1.5 s: the sound and its tail)
			for (int part = 0; part < 16; part++) {   // (a byte a sixteenth of a frame: about the link's 320 us a byte)
				if (sent < sizeof coin) b.rx_byte(coin[sent++]);
				b.run(frame_us / 16, out);
				while (b.tx_pending()) b.tx_byte();   // (its answers: no one listens)
			}
	}
	// from the first sample heard to the last above about -60 dB
	size_t first = 0, last = 0;
	while (first < out.size() && std::abs(int(out[first])) < 8) first++;
	for (size_t i = first; i < out.size(); i++)
		if (std::abs(int(out[i])) >= 32) last = i;
	if (first >= out.size()) return {};
	first &= ~size_t(1);
	last = std::min(out.size(), (last | 1) + 1);
	std::vector<int16_t> pcm(out.begin() + ptrdiff_t(first), out.begin() + ptrdiff_t(last));
	const size_t fade = std::min<size_t>(pcm.size() / 2, RATE / 200);   // (5 ms at the end: cut where it's quiet, not at a zero)
	for (size_t i = 0; i < fade; i++) {
		const float k = float(i) / float(fade);
		pcm[pcm.size() - 2 - 2 * i] = int16_t(pcm[pcm.size() - 2 - 2 * i] * k);
		pcm[pcm.size() - 1 - 2 * i] = int16_t(pcm[pcm.size() - 1 - 2 * i] * k);
	}
	return pcm;
}

} // namespace coin_sound
