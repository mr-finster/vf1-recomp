// Standalone NEC V60 interpreter. The flag macros, register names and PSW helpers below are
// MAME's v60.cpp (BSD-3-Clause, copyright-holders: Farfetch'd, R. Belmont); reset, run and the
// IRQ entry are MAME's too, without the device framework.
#include "v60_device.h"
#include <csetjmp>
#include <cstdarg>

// (the interpreter's run: where an instruction it can't run goes, the CPU stopped (as the PowerPC of VF3 stops), not the
// process. Only code the game's ROM doesn't have gets there: a state from elsewhere's, a watcher's snapshot from a
// hostile host. Outside the interpreter's run (the native code's) it stops the process, as it always has)
static thread_local jmp_buf *g_trap = nullptr;

void v60_fatal(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
	fputc('\n', stderr);
	if (g_trap) longjmp(*g_trap, 1);
	abort();
}

void v60_log(const char *fmt, ...)
{
	if (!getenv("V60_LOG")) return;
	va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

#include "v60_macros.h"

void v60_device::v60SaveStack()
{
	if (PSW & 0x10000000)
		ISP = SP;
	else
		m_reg[37 + ((PSW >> 24) & 3)] = SP;
}

void v60_device::v60ReloadStack()
{
	if (PSW & 0x10000000)
		SP = ISP;
	else
		SP = m_reg[37 + ((PSW >> 24) & 3)];
}

uint32_t v60_device::v60ReadPSW()
{
	PSW &= 0xfffffff0;
	PSW |= (_Z?1:0) | (_S?2:0) | (_OV?4:0) | (_CY?8:0);
	return PSW;
}

void v60_device::v60WritePSW(uint32_t newval)
{
	/* determine if we need to save / restore the stacks */
	int updateStack = 0;

	/* if the interrupt state is changing, we definitely need to update */
	if ((newval ^ PSW) & 0x10000000)
		updateStack = 1;

	/* if we are not in interrupt mode and the level is changing, we also must update */
	else if (!(PSW & 0x10000000) && ((newval ^ PSW) & 0x03000000))
		updateStack = 1;

	/* save the previous stack value */
	if (updateStack)
		v60SaveStack();

	/* set the new value and update the flags */
	PSW = newval;
	_Z =  (uint8_t)(PSW & 1);
	_S =  (uint8_t)(PSW & 2);
	_OV = (uint8_t)(PSW & 4);
	_CY = (uint8_t)(PSW & 8);

	/* fetch the new stack value */
	if (updateStack)
		v60ReloadStack();
}


uint32_t v60_device::v60_update_psw_for_exception(int is_interrupt, int target_level)
{
	uint32_t oldPSW = v60ReadPSW();
	uint32_t newPSW = oldPSW;

	// Change to interrupt context
	newPSW &= ~(3 << 24);  // PSW.EL = 0
	newPSW |= target_level << 24; // set target level
	newPSW &= ~(1 << 18);  // PSW.IE = 0
	newPSW &= ~(1 << 16);  // PSW.TE = 0
	newPSW &= ~(1 << 27);  // PSW.TP = 0
	newPSW &= ~(1 << 17);  // PSW.AE = 0
	newPSW &= ~(1 << 29);  // PSW.EM = 0
	if (is_interrupt)
		newPSW |=  (1 << 28);// PSW.IS = 1
	newPSW |=  (1 << 31);  // PSW.ASA = 1
	v60WritePSW(newPSW);

	return oldPSW;
}




// Addressing mode decoding functions
#include "../../third_party/mame/v60/am.hxx"

// Opcode functions
#include "../../third_party/mame/v60/op12.hxx"
#include "../../third_party/mame/v60/op2.hxx"
#include "../../third_party/mame/v60/op3.hxx"
#include "../../third_party/mame/v60/op4.hxx"
#include "../../third_party/mame/v60/op5.hxx"
#include "../../third_party/mame/v60/op6.hxx"
#include "../../third_party/mame/v60/op7a.hxx"

uint32_t v60_device::opUNHANDLED()
{
	fatalerror("Unhandled OpCode found : %02x at %08x\n", OpRead16(PC), PC);
}

// Opcode jump table
#include "../../third_party/mame/v60/optable.hxx"

void v60_device::reset()
{
	m_stopped = false;
	m_stall_io = 0;
	m_irq_line = CLEAR_LINE;
	m_nmi_line = CLEAR_LINE;
	for (int i = 0; i < 68; i++)
		m_reg[i] = 0;
	m_reg[45] = 0x00006000; // PIR: V60
	m_op1 = m_op2 = 0; m_flag1 = m_flag2 = 0; m_instflags = 0; m_lenop1 = m_lenop2 = 0;
	m_subop = 0; m_bamoffset1 = m_bamoffset2 = 0; m_amflag = 0; m_amout = 0; m_bamoffset = 0;
	m_amlength1 = m_amlength2 = 0; m_modadd = 0; m_modm = 0; m_modval = m_modval2 = 0;
	m_modwritevalb = 0; m_modwritevalh = 0; m_modwritevalw = 0; m_moddim = 0;
	m_icount = 0;

	PSW   = 0x10000000;
	PC    = m_start_pc & 0xffffff;
	SBR   = 0x00000000;
	SYCW  = 0x00000070;
	TKCW  = 0x0000e000;
	PSW2  = 0x0000f002;
	_CY = _OV = _S = _Z = 0;
}

void v60_device::v60_do_irq(int vector)
{
	standard_irq_callback(vector, PC);
	uint32_t oldPSW = v60_update_psw_for_exception(1, 0);
	SP -= 4;
	m_program->write_dword_unaligned(SP, oldPSW);
	SP -= 4;
	m_program->write_dword_unaligned(SP, PC);
	uint32_t from = PC;
	PC = GETINTVECT(vector);
	m_irq_taken = true;
	if (on_irq) on_irq(vector, from, PC);
}

void v60_device::v60_try_irq()
{
	if (m_irq_line == CLEAR_LINE)
		return;
	if ((PSW & (1 << 18)) != 0) {
		if (m_irq_line != ASSERT_LINE)
			m_irq_line = CLEAR_LINE;
		int vector = m_irq_cycle();
		v60_do_irq(vector + 0x40);
	}
}

void v60_device::set_nmi_line(int state)
{
	if (state == ASSERT_LINE) {
		if (m_nmi_line == CLEAR_LINE) {
			m_nmi_line = ASSERT_LINE;
			v60_do_irq(2);
		}
	} else
		m_nmi_line = CLEAR_LINE;
}

// MAME applies a CPU's input lines through the scheduler, so a line raised by a memory write
// is taken after the current instruction, never in the middle of it.
void v60_device::set_irq_line(int state)
{
	m_irq_line = state;
}

uint32_t v60_device::step()
{
	if (m_irq_line != CLEAR_LINE)
		v60_try_irq();
	m_PPC = PC;
	uint32_t inc = (this->*s_OpCodeTable[OpRead8(PC)])();
	PC += inc;
	return m_PPC;
}

int v60_device::run(int cycles)
{
	if (m_stopped) { m_icount = 0; return cycles; }
	jmp_buf trap;
	jmp_buf *const was = g_trap;
	g_trap = &trap;
	if (setjmp(trap)) {   // (an instruction it can't run: stopped till a reset or a load)
		g_trap = was;
		m_stopped = true;
		m_icount = 0;
		return cycles;
	}
	run_trapped(cycles);
	g_trap = was;
	return cycles;
}

void v60_device::run_trapped(int cycles)
{
	// what the last slice overran is owed by this one, as on MAME's scheduler
	m_icount += cycles;
	if (m_irq_line != CLEAR_LINE)
		v60_try_irq();
	while (m_icount > 0) {
		m_PPC = PC;
		if (on_exec) on_exec(PC);
		m_icount -= 8;
		uint32_t inc = (this->*s_OpCodeTable[OpRead8(PC)])();
		PC += inc;
		if (m_irq_line != CLEAR_LINE)
			v60_try_irq();
	}
}
