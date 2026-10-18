#include "tgp.h"
#include "../../machine/model1.h"

// The command numbers and argument counts, as MAME's shared table has them
const game::vr::tgp_track_hle::command game::vr::tgp_track_hle::commands[] = {
	{ 0x0d, 1, &tgp_track_hle::track_select },
	{ 0x0e, 4, &tgp_track_hle::load_base },
	{ 0x18, 1, &tgp_track_hle::track_read_quad },
	{ 0x2c, 1, &tgp_track_hle::track_read_tri },
	{ 0x2e, 3, &tgp_track_hle::vlength },
	{ 0x30, 1, &tgp_track_hle::track_read_info },
	{ 0x31, 12, &tgp_track_hle::colbox_set },
	{ 0x32, 3, &tgp_track_hle::colbox_test },
	{ 0x36, 4, &tgp_track_hle::track_lookup },
	{ 0x40, 3, &tgp_track_hle::col_setcirc },
	{ 0x41, 2, &tgp_track_hle::col_testpt },
	{ 0x47, 4, &tgp_track_hle::car_move },
};

const game::vr::tgp_track_hle::command *game::vr::tgp_track_hle::find(uint32_t f)
{
	for (const command &c : commands)
		if (c.number == f) return &c;
	return nullptr;
}

int game::vr::tgp_track_hle::arguments(uint32_t f)
{
	const command *c = find(f);
	return c ? c->arguments : -1;
}

bool game::vr::tgp_track_hle::run(uint32_t f)
{
	const command *c = find(f);
	if (!c) return false;
	(this->*c->handler)();
	return true;
}

void game::vr::tgp_track_hle::track_select()
{
	int32_t track = m_machine.fifoin_pop();
	M1_LOG(0, "TGP track_select %d (%x)\n", track, m_machine.tgp_caller());
	m_state.select_track(track);
}

void game::vr::tgp_track_hle::load_base()
{
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	const float z = m_machine.fifoin_pop_f();
	const float radius = m_machine.fifoin_pop_f();
	m_state.set_base(x, y, z, radius);
	M1_LOG(0, "TGP load_base %f, %f, %f, %f (%x)\n", x, y, z, radius, m_machine.tgp_caller());
}

void game::vr::tgp_track_hle::track_read_quad()
{
	const u32 index = m_machine.fifoin_pop();
	M1_LOG(0, "TGP track_read_quad %d (%x)\n", index, m_machine.tgp_caller());
	for (const u32 word : m_state.read_quad(m_machine.tgp_data(), index))
		m_machine.fifoout_push(word);
}

void game::vr::tgp_track_hle::track_read_tri()
{
	const u32 index = m_machine.fifoin_pop();
	M1_LOG(0, "TGP track_read_tri %d (%x)\n", index, m_machine.tgp_caller());
	for (const u32 word : m_state.read_tri(m_machine.tgp_data(), index))
		m_machine.fifoout_push(word);
}

void game::vr::tgp_track_hle::vlength()
{
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	const float z = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP vlength %f, %f, %f (%x)\n", x, y, z, m_machine.tgp_caller());
	m_machine.fifoout_push_f(m_state.vector_length(x, y, z));
}

void game::vr::tgp_track_hle::track_read_info()
{
	const u16 index = m_machine.fifoin_pop();
	M1_LOG(0, "TGP track_read_info %d (%x)\n", index, m_machine.tgp_caller());
	m_machine.fifoout_push(m_state.read_info(m_machine.tgp_data(), index));
}

void game::vr::tgp_track_hle::colbox_set()
{
	std::array<float, 12> box;
	for (float &value : box)
		value = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP colbox_set (%x)\n", m_machine.tgp_caller());
	m_state.set_collision_box(box);
}

void game::vr::tgp_track_hle::colbox_test()
{
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	const float z = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP colbox_test %f, %f, %f (%x)\n", x, y, z, m_machine.tgp_caller());
	m_machine.fifoout_push_f(-1);
}

void game::vr::tgp_track_hle::col_setcirc()
{
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	const float radius = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP col_setcirc %f, %f, %f (%x)\n", x, y, radius, m_machine.tgp_caller());
	m_state.set_circle(x, y, radius);
}

void game::vr::tgp_track_hle::col_testpt()
{
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP col_testpt %f, %f (%x)\n", x, y, m_machine.tgp_caller());
	m_machine.fifoout_push_f(m_state.circle_test(x, y));
}

void game::vr::tgp_track_hle::car_move()
{
	const s16 angle = m_machine.fifoin_pop();
	const float distance = m_machine.fifoin_pop_f();
	const float x = m_machine.fifoin_pop_f();
	const float y = m_machine.fifoin_pop_f();
	const float dx = distance * model1::tsin(angle);
	const float dy = distance * model1::tcos(angle);
	M1_LOG(0, "TGP car_move (%d, %f), (%f, %f) (%x)\n", angle, distance, x, y, m_machine.tgp_caller());
	m_machine.fifoout_push_f(dx);
	m_machine.fifoout_push_f(dy);
	m_machine.fifoout_push_f(x + dx);
	m_machine.fifoout_push_f(y + dy);
}

void game::vr::tgp_track_hle::track_lookup()
{
	const float height_query = m_machine.fifoin_pop_f();
	const u32 index = m_machine.fifoin_pop();
	const float x = m_machine.fifoin_pop_f();
	const float z = m_machine.fifoin_pop_f();

	M1_LOG(0, "TGP track_lookup %f, 0x%x, %f, %f (%x)\n", height_query, index, x, z, m_machine.tgp_caller());
	const auto result = m_state.lookup(m_machine.tgp_data(), index, x, z, height_query);

	m_machine.tgp_ram_write(0x0000, 0);
	m_machine.tgp_ram_write(0x0001, f2u(result.height));
	m_machine.tgp_ram_write(0x0002, result.entry);
}
