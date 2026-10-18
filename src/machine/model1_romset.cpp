#include "model1_romset.h"
#include <filesystem>
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace romset {

namespace {

// ---- a minimal zip reader: the central directory, stored and deflated entries ----

struct entry { std::string name; uint32_t crc = 0, csize = 0, usize = 0, local = 0; uint16_t method = 0; };

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

struct zip
{
	std::vector<uint8_t> data;
	std::vector<entry> entries;

	bool open(const std::string &path)
	{
		// The sets are small (vf.zip is about 14 MB): anything over 256 MB isn't one, and isn't read.
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(path, ec);
		if (ec || size < 22 || size > (256u << 20)) return false;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) return false;
		const long n = long(size);
		data.resize(size_t(n));
		const bool got = fread(data.data(), 1, data.size(), f) == data.size();
		fclose(f);
		if (!got) return false;
		// the end of central directory record, within the last 64 KB
		long eocd = -1;
		for (long i = n - 22; i >= 0 && i >= n - 22 - 65535; i--)
			if (le32(&data[i]) == 0x06054b50) { eocd = i; break; }
		if (eocd < 0) return false;
		const uint16_t count = le16(&data[eocd + 10]);
		size_t at = le32(&data[eocd + 16]);          // offsets in size_t: a corrupt one can't wrap round
		for (int k = 0; k < count; k++) {
			if (at + 46 > data.size() || le32(&data[at]) != 0x02014b50) return false;
			entry e;
			e.method = le16(&data[at + 10]);
			e.crc = le32(&data[at + 16]);
			e.csize = le32(&data[at + 20]);
			e.usize = le32(&data[at + 24]);
			const uint16_t nlen = le16(&data[at + 28]), xlen = le16(&data[at + 30]), clen = le16(&data[at + 32]);
			e.local = le32(&data[at + 42]);
			if (at + 46 + nlen > data.size()) return false;
			e.name.assign(reinterpret_cast<const char *>(&data[at + 46]), nlen);
			const size_t slash = e.name.find_last_of('/');
			if (slash != std::string::npos) e.name = e.name.substr(slash + 1);
			entries.push_back(e);
			at += 46 + nlen + xlen + clen;
		}
		return true;
	}

	bool read(const entry &e, std::vector<uint8_t> &out) const
	{
		const size_t local = e.local;
		if (local + 30 > data.size() || le32(&data[local]) != 0x04034b50) return false;
		const size_t start = local + 30 + le16(&data[local + 26]) + le16(&data[local + 28]);
		if (start + size_t(e.csize) > data.size() || e.usize > (64u << 20)) return false;   // no ROM is over 64 MB
		out.resize(e.usize);
		if (e.method == 0) {
			if (e.csize != e.usize) return false;
			memcpy(out.data(), &data[start], e.usize);
		} else if (e.method == 8) {
			z_stream z{};
			if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
			z.next_in = const_cast<Bytef *>(&data[start]);
			z.avail_in = e.csize;
			z.next_out = out.data();
			z.avail_out = e.usize;
			const int r = inflate(&z, Z_FINISH);
			inflateEnd(&z);
			if (r != Z_STREAM_END || z.total_out != e.usize) return false;
		} else {
			return false;
		}
		return uint32_t(crc32(0, out.data(), uInt(out.size()))) == e.crc;
	}
};

// The CPU board's own ROMs: the TGP's tables and the board's data, the same in every Model 1 set.
const rom CPU_BOARD[] = {
	{ "opr14742.bin", 0x446a1085, COPRO_TABLES, 0x0, WORD32 },
	{ "opr14743.bin", 0xe8953554, COPRO_TABLES, 0x2, WORD32 },
	{ "opr14744.bin", 0x730ea9e0, OTHER_DATA, 0x0, WORD32, "opr-14744.58" },
	{ "opr14745.bin", 0x4c934d96, OTHER_DATA, 0x2, WORD32, "opr-14745.59" },
	{ "opr14746.bin", 0x2a266cbd, OTHER_DATA, 0x40000, WORD32, "opr-14746.62" },
	{ "opr14747.bin", 0xa4ad5e19, OTHER_DATA, 0x40002, WORD32, "opr-14747.63" },
	{ "opr14748.bin", 0x4a532cb8, OTHER_OTHER_DATA, 0x0, RAW },
};
const uint32_t IOBOARD_CRC = 0x2d093304;   // epr-14869b.25, in model1io.zip

void place(std::vector<uint8_t> &r, const rom &ro, const std::vector<uint8_t> &d)
{
	const size_t off = ro.offset;
	switch (ro.k) {
	case RAW:
		for (size_t i = 0; i < d.size() && off + i < r.size(); i++) r[off + i] = d[i];
		break;
	case BYTE16:
		for (size_t i = 0; i < d.size() && off + 2 * i < r.size(); i++) r[off + 2 * i] = d[i];
		break;
	case BYTE32:
		for (size_t i = 0; i < d.size() && off + 4 * i < r.size(); i++) r[off + 4 * i] = d[i];
		break;
	case WORDSWAP:
		for (size_t i = 0; i + 1 < d.size() && off + i + 1 < r.size(); i += 2) { r[off + i] = d[i + 1]; r[off + i + 1] = d[i]; }
		break;
	case WORD32:
		for (size_t i = 0; i + 1 < d.size() && off + 2 * i + 1 < r.size(); i += 2) { r[off + 2 * i] = d[i]; r[off + 2 * i + 1] = d[i + 1]; }
		break;
	}
}

} // namespace

bool is_game_set(const definition &set, const std::string &path)
{
	zip z;
	if (!z.open(path)) return false;
	for (const entry &e : z.entries)
		if (e.crc == set.main_crc) return true;
	return false;
}

result load(const definition &set, const std::vector<std::string> &zips)
{
	result res;
	std::vector<zip> zs(zips.size());
	for (size_t i = 0; i < zips.size(); i++)
		if (!zs[i].open(zips[i])) { res.error = "Couldn't read " + zips[i] + " as a zip file."; return res; }
	// every file by CRC (and by name), across all the zips
	auto find = [&](const char *name, const char *alias, uint32_t crc) -> std::pair<const zip *, const entry *> {
		for (const zip &z : zs)
			for (const entry &e : z.entries)
				if (e.crc == crc && (e.name == name || (alias && e.name == alias))) return { &z, &e };
		for (const zip &z : zs)
			for (const entry &e : z.entries)
				if (e.crc == crc) return { &z, &e };
		return { nullptr, nullptr };
	};
	std::vector<uint8_t> regions[N_REGIONS];
	for (int r = 0; r < N_REGIONS; r++) regions[r].assign(set.regions[r].size, set.regions[r].fill);
	std::vector<std::string> missing;
	std::vector<uint8_t> d;
	auto take = [&](const rom &ro) {
		auto [z, e] = find(ro.name, ro.alias, ro.crc);
		if (!z) {
			// a file of the right name but the wrong contents is a different version of the game
			bool wrong = false;
			for (const zip &zz : zs) for (const entry &ee : zz.entries) if (ee.name == ro.name) wrong = true;
			missing.push_back(std::string(ro.name) + (wrong ? " (a different version)" : ""));
			return true;
		}
		if (!z->read(*e, d)) { res.error = std::string("The zip's copy of ") + ro.name + " is damaged."; return false; }
		place(regions[ro.region], ro, d);
		return true;
	};
	for (const rom &ro : CPU_BOARD) if (!take(ro)) return res;
	for (const rom &ro : set.roms) if (!take(ro)) return res;
	if (!missing.empty()) {
		res.error = std::string("This isn't a complete ") + set.display_name + " set (MAME's \"" + set.mame_name + "\"). Missing: ";
		for (size_t i = 0; i < missing.size() && i < 4; i++) res.error += (i ? ", " : "") + missing[i];
		if (missing.size() > 4) res.error += " and " + std::to_string(missing.size() - 4) + " more";
		res.error += ".";
		return res;
	}
	res.regions.title = set.title;
	res.regions.maincpu = std::move(regions[MAINCPU]);
	res.regions.copro_tables = std::move(regions[COPRO_TABLES]);
	res.regions.copro_data = std::move(regions[COPRO_DATA]);
	res.regions.polygons = std::move(regions[POLYGONS]);
	res.regions.sndcpu = std::move(regions[SNDCPU]);
	res.regions.pcm1 = std::move(regions[PCM1]);
	res.regions.pcm2 = std::move(regions[PCM2]);
	if (!regions[TGP_PROGRAM].empty()) { res.regions.tgp_program = std::move(regions[TGP_PROGRAM]); res.has_tgp = true; }
	if (const supplement *s = set.tgp_program) {
		auto [z, e] = find(s->name, nullptr, s->crc);
		if (!z || !z->read(*e, d)) { res.error = s->missing; return res; }
		res.regions.tgp_program = d;
		res.has_tgp = true;
	}
	if (auto [z, e] = find("epr-14869b.25", nullptr, IOBOARD_CRC); z && z->read(*e, d)) { res.regions.ioboard = d; res.has_ioboard = true; }
	res.ok = true;
	return res;
}

static std::vector<const definition *> &sets()
{
	static std::vector<const definition *> all;
	return all;
}

registration::registration(const definition &d)
{
	if (find(d.title.name)) {
		fprintf(stderr, "romset \"%.*s\" registered twice\n", int(d.title.name.size()), d.title.name.data());
		std::abort();
	}
	sets().push_back(&d);
}

const definition *find(std::string_view game_name)
{
	for (const definition *d : sets())
		if (d->title.name == game_name) return d;
	return nullptr;
}

std::vector<const definition *> registered() { return sets(); }

} // namespace romset
