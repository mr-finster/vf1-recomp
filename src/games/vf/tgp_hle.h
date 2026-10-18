#pragma once
#include "../../machine/model1.h"

class vf_tgp_hle
{
public:
	explicit vf_tgp_hle(model1 &machine) : m_machine(machine) {}
	void run();
	void reset();

private:
	struct function
	{
		model1::tgp_func cb;
		int count;
		void (vf_tgp_hle::*vf_cb)() = nullptr;
	};

	static const function table[];
	model1 &m_machine;
	uint32_t m_active_list_pos = 0, m_active_list_length = 0;
	float m_xmin = 0, m_xmax = 0, m_zmin = 0, m_zmax = 0;
	float m_yground = 0, m_yfloor = 0, m_yjump = 0;
	uint32_t m_rng = 1;
	void groundbox_test();
	void groundbox_set();
	void set_list();
	void start_list();
	void get_list();
	uint32_t m_list_length = 0;
	int rand() { m_rng = m_rng * 1103515245 + 12345; return int((m_rng >> 16) & 0x7fff); }
};

namespace game {
tgp_hle_callbacks create_vf_tgp_hle(model1 &machine);
}
