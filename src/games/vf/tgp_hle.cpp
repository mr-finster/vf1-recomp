#include "tgp_hle.h"
#include <memory>

// MAME's table is shared by every Model 1 game. Here, Virtua Racing's track and collision
// commands (0d, 0e, 18, 2c, 2e, 30-32, 36, 40, 41, 47) are left out because VF never sends them.
// They live in src/games/vr/tgp.cpp.
const vf_tgp_hle::function vf_tgp_hle::table[] = {
	{ &model1::fadd,            2 }, /* 0x00 */
	{ &model1::fsub,            2 },
	{ &model1::fmul,            2 },
	{ &model1::fdiv,            2 },
	{ nullptr,                  0 },
	{ &model1::matrix_push,     0 },
	{ &model1::matrix_pop,      0 },
	{ &model1::matrix_write,   12 },
	{ &model1::clear_stack,     0 },
	{ &model1::matrix_mul,     12 },
	{ &model1::anglev,          2 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::anglep,          4 },

	{ &model1::matrix_ident,    0 }, /* 0x10 */
	{ &model1::matrix_read,     0 },
	{ &model1::matrix_trans,    3 },
	{ &model1::matrix_scale,    3 },
	{ &model1::matrix_rotx,     1 },
	{ &model1::matrix_roty,     1 },
	{ &model1::matrix_rotz,     1 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::transform_point, 3 },
	{ &model1::fsin_m1,         1 },
	{ &model1::fcos_m1,         1 },
	{ &model1::fsinm_m1,        2 },
	{ &model1::fcosm_m1,        2 },
	{ &model1::distance3,       6 },

	{ nullptr,                  0 }, /* 0x20 */
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::acc_set,         1 },
	{ &model1::acc_get,         0 },
	{ &model1::acc_add,         1 },
	{ &model1::acc_sub,         1 },
	{ &model1::acc_mul,         1 },
	{ &model1::acc_div,         1 },
	{ &model1::f42,             3 },
	{ &model1::f43,             6 },
	{ nullptr,                  0 },
	{ &model1::fsqrt,           1 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },

	{ nullptr,                  0 }, /* 0x30 */
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },

	{ nullptr,                  0 }, /* 0x40 */
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::distance,        4 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::cpa,            12 },
	{ nullptr,                  0 },
	{ &model1::vmat_store,      1 },
	{ &model1::vmat_restore,    1 },
	{ nullptr,                  0 },
	{ &model1::vmat_mul,        2 },
	{ &model1::vmat_read,       1 },
	{ &model1::matrix_unrot,    0 },

	{ &model1::f80,             0 }, /* 0x50 */
	{ nullptr,                  0 },
	{ &model1::matrix_rtrans,   0 },
	{ nullptr,                  0 },
	{ &model1::vmat_save,       1 },
	{ &model1::vmat_load,       1 },
	{ &model1::ram_setadr,      1 },
	{ nullptr,                  3, &vf_tgp_hle::groundbox_test },
	{ nullptr,                  0 },
	{ nullptr,                  4, &vf_tgp_hle::set_list },
	{ nullptr,                  0 },
	{ nullptr,                  0 },
	{ &model1::f92,             4 },
	{ &model1::f93,             1 },
	{ &model1::f94,             1 },
	{ &model1::vmat_flatten,    0 },

	{ &model1::vmat_load1,      1 }, /* 0x60 */
	{ &model1::ram_trans,       0 },
	{ nullptr,                  1, &vf_tgp_hle::start_list },
	{ &model1::f99,             0 },
	{ nullptr,                  0, &vf_tgp_hle::get_list },
	{ nullptr,                  7, &vf_tgp_hle::groundbox_set },
	{ &model1::f102,            8 },
	{ &model1::f103,            1 }
};

void vf_tgp_hle::reset()
{
	m_active_list_pos = 0;
	m_active_list_length = 0;
	m_list_length = 0;
}

void vf_tgp_hle::groundbox_test()
{
	float a = m_machine.fifoin_pop_f();
	float b = m_machine.fifoin_pop_f();
	float c = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP groundbox_test %f, %f, %f (%x)\n", a, b, c, m_machine.tgp_caller());
	float x = m_machine.tgp_matrix()[0]*a + m_machine.tgp_matrix()[3]*b + m_machine.tgp_matrix()[6]*c + m_machine.tgp_matrix()[9];
	float z = m_machine.tgp_matrix()[2]*a + m_machine.tgp_matrix()[5]*b + m_machine.tgp_matrix()[8]*c + m_machine.tgp_matrix()[11];
	m_machine.fifoout_push(x < m_xmin || x > m_xmax);
	m_machine.fifoout_push(1);
	m_machine.fifoout_push(z < m_zmin || z > m_zmax);
}

void vf_tgp_hle::groundbox_set()
{
	float a = m_machine.fifoin_pop_f();
	float b = m_machine.fifoin_pop_f();
	float c = m_machine.fifoin_pop_f();
	float d = m_machine.fifoin_pop_f();
	float e = m_machine.fifoin_pop_f();
	float f = m_machine.fifoin_pop_f();
	float g = m_machine.fifoin_pop_f();
	M1_LOG(0, "TGP groundbox_set %f, %f, %f, %f, %f, %f, %f (%x)\n", a, b, c, d, e, f, g, m_machine.tgp_caller());
	m_xmin = e;
	m_xmax = d;
	m_zmin = g;
	m_zmax = f;
	m_yground = b;
	m_yfloor = a;
	m_yjump = c;
}

void vf_tgp_hle::set_list()
{
	u32 a = m_machine.fifoin_pop();
	u32 b = m_machine.fifoin_pop();
	u32 c = m_machine.fifoin_pop();
	u32 d = m_machine.fifoin_pop();
	(void)a;
	(void)b;
	(void)c;
	M1_LOG(0, "TGP list set base 0x%x, 0x%x, %d, length=%d (%x)\n", a, b, c, d, m_machine.tgp_caller());
	m_list_length = d;
}

void vf_tgp_hle::start_list()
{
	u32 a = m_machine.fifoin_pop();
	M1_LOG(0, "TGP load list start %d (%x)\n", a, m_machine.tgp_caller());
	m_active_list_length = m_list_length;
	m_active_list_pos = 0;
}

void vf_tgp_hle::get_list()
{
	M1_LOG(0, "TGP f100 get list (%x)\n", m_machine.tgp_caller());
	for (uint32_t i = 0; i < m_list_length; i++)
		m_machine.fifoout_push_f((rand() % 1000) / 100.0f);
}

void vf_tgp_hle::run()
{
	static_assert(std::size(table) == 0x68);
	if (m_active_list_length) {
		while (m_active_list_pos < m_active_list_length && !m_machine.tgp_input().is_empty()) {
			u32 v = m_machine.tgp_input().pop();
			m_active_list_pos++;
			M1_LOG(0, "TGP load list %d/%d: %08x\n", m_active_list_pos, m_active_list_length, v);
		}
		if (m_active_list_pos == m_active_list_length)
			m_active_list_pos = m_active_list_length = 0;
		if (m_machine.tgp_input().is_empty())
			return;
	}

	while (!m_machine.tgp_input().is_empty()) {
		u32 f = m_machine.tgp_input().peek(0) >> 23;
		if (f >= std::size(table) || (!table[f].cb && !table[f].vf_cb)) {
			M1_LOG(0, "TGP function %d unimplemented (%x)\n", f, m_machine.tgp_caller());
			m_machine.m_tgp_missing[f]++;
			m_machine.tgp_input().pop();
			return;
		}

		if (m_machine.tgp_input().size() < unsigned(table[f].count + 1))
			return;

		m_machine.tgp_input().pop();
		m_machine.m_tgp_calls[f]++;
		if (table[f].vf_cb)
			(this->*table[f].vf_cb)();
		else
			(m_machine.*table[f].cb)();
	}
}

namespace game {

tgp_hle_callbacks create_vf_tgp_hle(model1 &machine)
{
	auto hle = std::make_shared<vf_tgp_hle>(machine);
	return {
		[hle] { hle->run(); },
		[hle] { hle->reset(); }
	};
}

}
