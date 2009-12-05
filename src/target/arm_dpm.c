/*
 * Copyright (C) 2009 by David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "armv4_5.h"		/* REVISIT to become arm.h */
#include "arm_dpm.h"
#include <jtag/jtag.h>
#include "register.h"
#include "breakpoints.h"
#include "target_type.h"


/**
 * @file
 * Implements various ARM DPM operations using architectural debug registers.
 * These routines layer over core-specific communication methods to cope with
 * implementation differences between cores like ARM1136 and Cortex-A8.
 */

/*----------------------------------------------------------------------*/

/*
 * Coprocessor support
 */

/* Read coprocessor */
static int dpm_mrc(struct target *target, int cpnum,
		uint32_t op1, uint32_t op2, uint32_t CRn, uint32_t CRm,
		uint32_t *value)
{
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = arm->dpm;
	int retval;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("MRC p%d, %d, r0, c%d, c%d, %d", cpnum,
			(int) op1, (int) CRn,
			(int) CRm, (int) op2);

	/* read coprocessor register into R0; return via DCC */
	retval = dpm->instr_read_data_r0(dpm,
			ARMV4_5_MRC(cpnum, op1, 0, CRn, CRm, op2),
			value);

	/* (void) */ dpm->finish(dpm);
	return retval;
}

static int dpm_mcr(struct target *target, int cpnum,
		uint32_t op1, uint32_t op2, uint32_t CRn, uint32_t CRm,
		uint32_t value)
{
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = arm->dpm;
	int retval;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	LOG_DEBUG("MCR p%d, %d, r0, c%d, c%d, %d", cpnum,
			(int) op1, (int) CRn,
			(int) CRm, (int) op2);

	/* read DCC into r0; then write coprocessor register from R0 */
	retval = dpm->instr_write_data_r0(dpm,
			ARMV4_5_MCR(cpnum, op1, 0, CRn, CRm, op2),
			value);

	/* (void) */ dpm->finish(dpm);
	return retval;
}

/*----------------------------------------------------------------------*/

/*
 * Register access utilities
 */

/* Toggles between recorded core mode (USR, SVC, etc) and a temporary one.
 * Routines *must* restore the original mode before returning!!
 */
static int dpm_modeswitch(struct arm_dpm *dpm, enum arm_mode mode)
{
	int retval;
	uint32_t cpsr;

	/* restore previous mode */
	if (mode == ARM_MODE_ANY)
		cpsr = buf_get_u32(dpm->arm->cpsr->value, 0, 32);

	/* else force to the specified mode */
	else
		cpsr = mode;

	retval = dpm->instr_write_data_r0(dpm, ARMV4_5_MSR_GP(0, 0xf, 0), cpsr);

	if (dpm->instr_cpsr_sync)
		retval = dpm->instr_cpsr_sync(dpm);

	return retval;
}

/* just read the register -- rely on the core mode being right */
static int dpm_read_reg(struct arm_dpm *dpm, struct reg *r, unsigned regnum)
{
	uint32_t value;
	int retval;

	switch (regnum) {
	case 0 ... 14:
		/* return via DCC:  "MCR p14, 0, Rnum, c0, c5, 0" */
		retval = dpm->instr_read_data_dcc(dpm,
				ARMV4_5_MCR(14, 0, regnum, 0, 5, 0),
				&value);
		break;
	case 15:	/* PC */
		/* "MOV r0, pc"; then return via DCC */
		retval = dpm->instr_read_data_r0(dpm, 0xe1a0000f, &value);

		/* NOTE: this seems like a slightly awkward place to update
		 * this value ... but if the PC gets written (the only way
		 * to change what we compute), the arch spec says subsequent
		 * reads return values which are "unpredictable".  So this
		 * is always right except in those broken-by-intent cases.
		 */
		switch (dpm->arm->core_state) {
		case ARM_STATE_ARM:
			value -= 8;
			break;
		case ARM_STATE_THUMB:
		case ARM_STATE_THUMB_EE:
			value -= 4;
			break;
		case ARM_STATE_JAZELLE:
			/* core-specific ... ? */
			LOG_WARNING("Jazelle PC adjustment unknown");
			break;
		}
		break;
	default:
		/* 16: "MRS r0, CPSR"; then return via DCC
		 * 17: "MRS r0, SPSR"; then return via DCC
		 */
		retval = dpm->instr_read_data_r0(dpm,
				ARMV4_5_MRS(0, regnum & 1),
				&value);
		break;
	}

	if (retval == ERROR_OK) {
		buf_set_u32(r->value, 0, 32, value);
		r->valid = true;
		r->dirty = false;
		LOG_DEBUG("READ: %s, %8.8x", r->name, (unsigned) value);
	}

	return retval;
}

/* just write the register -- rely on the core mode being right */
static int dpm_write_reg(struct arm_dpm *dpm, struct reg *r, unsigned regnum)
{
	int retval;
	uint32_t value = buf_get_u32(r->value, 0, 32);

	switch (regnum) {
	case 0 ... 14:
		/* load register from DCC:  "MRC p14, 0, Rnum, c0, c5, 0" */
		retval = dpm->instr_write_data_dcc(dpm,
				ARMV4_5_MRC(14, 0, regnum, 0, 5, 0),
				value);
		break;
	case 15:	/* PC */
		/* read r0 from DCC; then "MOV pc, r0" */
		retval = dpm->instr_write_data_r0(dpm, 0xe1a0f000, value);
		break;
	default:
		/* 16: read r0 from DCC, then "MSR r0, CPSR_cxsf"
		 * 17: read r0 from DCC, then "MSR r0, SPSR_cxsf"
		 */
		retval = dpm->instr_write_data_r0(dpm,
				ARMV4_5_MSR_GP(0, 0xf, regnum & 1),
				value);

		if (regnum == 16 && dpm->instr_cpsr_sync)
			retval = dpm->instr_cpsr_sync(dpm);

		break;
	}

	if (retval == ERROR_OK) {
		r->dirty = false;
		LOG_DEBUG("WRITE: %s, %8.8x", r->name, (unsigned) value);
	}

	return retval;
}

/**
 * Read basic registers of the the current context:  R0 to R15, and CPSR;
 * sets the core mode (such as USR or IRQ) and state (such as ARM or Thumb).
 * In normal operation this is called on entry to halting debug state,
 * possibly after some other operations supporting restore of debug state
 * or making sure the CPU is fully idle (drain write buffer, etc).
 */
int arm_dpm_read_current_registers(struct arm_dpm *dpm)
{
	struct arm *arm = dpm->arm;
	uint32_t cpsr;
	int retval;
	struct reg *r;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	/* read R0 first (it's used for scratch), then CPSR */
	r = arm->core_cache->reg_list + 0;
	if (!r->valid) {
		retval = dpm_read_reg(dpm, r, 0);
		if (retval != ERROR_OK)
			goto fail;
	}
	r->dirty = true;

	retval = dpm->instr_read_data_r0(dpm, ARMV4_5_MRS(0, 0), &cpsr);
	if (retval != ERROR_OK)
		goto fail;

	/* update core mode and state, plus shadow mapping for R8..R14 */
	arm_set_cpsr(arm, cpsr);

	/* REVISIT we can probably avoid reading R1..R14, saving time... */
	for (unsigned i = 1; i < 16; i++) {
		r = arm_reg_current(arm, i);
		if (r->valid)
			continue;

		retval = dpm_read_reg(dpm, r, i);
		if (retval != ERROR_OK)
			goto fail;
	}

	/* NOTE: SPSR ignored (if it's even relevant). */

	/* REVISIT the debugger can trigger various exceptions.  See the
	 * ARMv7A architecture spec, section C5.7, for more info about
	 * what defenses are needed; v6 debug has the most issues.
	 */

fail:
	/* (void) */ dpm->finish(dpm);
	return retval;
}

/**
 * Writes all modified core registers for all processor modes.  In normal
 * operation this is called on exit from halting debug state.
 *
 * @param dpm: represents the processor
 * @param bpwp: true ensures breakpoints and watchpoints are set,
 *	false ensures they are cleared
 */
int arm_dpm_write_dirty_registers(struct arm_dpm *dpm, bool bpwp)
{
	struct arm *arm = dpm->arm;
	struct reg_cache *cache = arm->core_cache;
	int retval;
	bool did_write;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;

	/* enable/disable watchpoints */
	for (unsigned i = 0; i < dpm->nwp; i++) {
		struct dpm_wp *dwp = dpm->dwp + i;
		struct watchpoint *wp = dwp->wp;
		bool disable;

		/* Avoid needless I/O ... leave watchpoints alone
		 * unless they're removed, or need updating because
		 * of single-stepping or running debugger code.
		 */
		if (!wp) {
			if (!dwp->dirty)
				continue;
			dwp->dirty = false;
			/* removed or startup; we must disable it */
			disable = true;
		} else if (bpwp) {
			if (!dwp->dirty)
				continue;
			/* disabled, but we must set it */
			dwp->dirty = disable = false;
			wp->set = true;
		} else {
			if (!wp->set)
				continue;
			/* set, but we must temporarily disable it */
			dwp->dirty = disable = true;
			wp->set = false;
		}

		if (disable)
			retval = dpm->bpwp_disable(dpm, 16 + i);
		else
			retval = dpm->bpwp_enable(dpm, 16 + i,
					wp->address, dwp->control);

		if (retval != ERROR_OK)
			LOG_ERROR("%s: can't %s HW watchpoint %d",
					target_name(arm->target),
					disable ? "disable" : "enable",
					i);
	}

	/* NOTE:  writes to breakpoint and watchpoint registers might
	 * be queued, and need (efficient/batched) flushing later.
	 */

	/* Scan the registers until we find one that's both dirty and
	 * eligible for flushing.  Flush that and everything else that
	 * shares the same core mode setting.  Typically this won't
	 * actually find anything to do...
	 */
	do {
		enum arm_mode mode = ARM_MODE_ANY;

		did_write = false;

		/* check everything except our scratch register R0 */
		for (unsigned i = 1; i < cache->num_regs; i++) {
			struct arm_reg *r;
			unsigned regnum;

			/* also skip PC, CPSR, and non-dirty */
			if (i == 15)
				continue;
			if (arm->cpsr == cache->reg_list + i)
				continue;
			if (!cache->reg_list[i].dirty)
				continue;

			r = cache->reg_list[i].arch_info;
			regnum = r->num;

			/* may need to pick and set a mode */
			if (!did_write) {
				enum arm_mode tmode;

				did_write = true;
				mode = tmode = r->mode;

				/* cope with special cases */
				switch (regnum) {
				case 8 ... 12:
					/* r8..r12 "anything but FIQ" case;
					 * we "know" core mode is accurate
					 * since we haven't changed it yet
					 */
					if (arm->core_mode == ARM_MODE_FIQ
							&& ARM_MODE_ANY
								!= mode)
						tmode = ARM_MODE_USR;
					break;
				case 16:
					/* SPSR */
					regnum++;
					break;
				}

				/* REVISIT error checks */
				if (tmode != ARM_MODE_ANY)
					retval = dpm_modeswitch(dpm, tmode);
			}
			if (r->mode != mode)
				continue;

			retval = dpm_write_reg(dpm,
					&cache->reg_list[i],
					regnum);

		}

	} while (did_write);

	/* Restore original CPSR ... assuming either that we changed it,
	 * or it's dirty.  Must write PC to ensure the return address is
	 * defined, and must not write it before CPSR.
	 */
	retval = dpm_modeswitch(dpm, ARM_MODE_ANY);
	arm->cpsr->dirty = false;

	retval = dpm_write_reg(dpm, &cache->reg_list[15], 15);
	cache->reg_list[15].dirty = false;

	/* flush R0 -- it's *very* dirty by now */
	retval = dpm_write_reg(dpm, &cache->reg_list[0], 0);
	cache->reg_list[0].dirty = false;

	/* (void) */ dpm->finish(dpm);
done:
	return retval;
}

/* Returns ARM_MODE_ANY or temporary mode to use while reading the
 * specified register ... works around flakiness from ARM core calls.
 * Caller already filtered out SPSR access; mode is never MODE_SYS
 * or MODE_ANY.
 */
static enum arm_mode dpm_mapmode(struct arm *arm,
		unsigned num, enum arm_mode mode)
{
	enum arm_mode amode = arm->core_mode;

	/* don't switch if the mode is already correct */
	if (amode == ARM_MODE_SYS)
		 amode = ARM_MODE_USR;
	if (mode == amode)
		return ARM_MODE_ANY;

	switch (num) {
	/* don't switch for non-shadowed registers (r0..r7, r15/pc, cpsr) */
	case 0 ... 7:
	case 15:
	case 16:
		break;
	/* r8..r12 aren't shadowed for anything except FIQ */
	case 8 ... 12:
		if (mode == ARM_MODE_FIQ)
			return mode;
		break;
	/* r13/sp, and r14/lr are always shadowed */
	case 13:
	case 14:
		return mode;
	default:
		LOG_WARNING("invalid register #%u", num);
		break;
	}
	return ARM_MODE_ANY;
}


/*
 * Standard ARM register accessors ... there are three methods
 * in "struct arm", to support individual read/write and bulk read
 * of registers.
 */

static int arm_dpm_read_core_reg(struct target *target, struct reg *r,
		int regnum, enum arm_mode mode)
{
	struct arm_dpm *dpm = target_to_arm(target)->dpm;
	int retval;

	if (regnum < 0 || regnum > 16)
		return ERROR_INVALID_ARGUMENTS;

	if (regnum == 16) {
		if (mode != ARM_MODE_ANY)
			regnum = 17;
	} else
		mode = dpm_mapmode(dpm->arm, regnum, mode);

	/* REVISIT what happens if we try to read SPSR in a core mode
	 * which has no such register?
	 */

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	if (mode != ARM_MODE_ANY) {
		retval = dpm_modeswitch(dpm, mode);
		if (retval != ERROR_OK)
			goto fail;
	}

	retval = dpm_read_reg(dpm, r, regnum);
	/* always clean up, regardless of error */

	if (mode != ARM_MODE_ANY)
		/* (void) */ dpm_modeswitch(dpm, ARM_MODE_ANY);

fail:
	/* (void) */ dpm->finish(dpm);
	return retval;
}

static int arm_dpm_write_core_reg(struct target *target, struct reg *r,
		int regnum, enum arm_mode mode, uint32_t value)
{
	struct arm_dpm *dpm = target_to_arm(target)->dpm;
	int retval;


	if (regnum < 0 || regnum > 16)
		return ERROR_INVALID_ARGUMENTS;

	if (regnum == 16) {
		if (mode != ARM_MODE_ANY)
			regnum = 17;
	} else
		mode = dpm_mapmode(dpm->arm, regnum, mode);

	/* REVISIT what happens if we try to write SPSR in a core mode
	 * which has no such register?
	 */

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		return retval;

	if (mode != ARM_MODE_ANY) {
		retval = dpm_modeswitch(dpm, mode);
		if (retval != ERROR_OK)
			goto fail;
	}

	retval = dpm_write_reg(dpm, r, regnum);
	/* always clean up, regardless of error */

	if (mode != ARM_MODE_ANY)
		/* (void) */ dpm_modeswitch(dpm, ARM_MODE_ANY);

fail:
	/* (void) */ dpm->finish(dpm);
	return retval;
}

static int arm_dpm_full_context(struct target *target)
{
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = arm->dpm;
	struct reg_cache *cache = arm->core_cache;
	int retval;
	bool did_read;

	retval = dpm->prepare(dpm);
	if (retval != ERROR_OK)
		goto done;

	do {
		enum arm_mode mode = ARM_MODE_ANY;

		did_read = false;

		/* We "know" arm_dpm_read_current_registers() was called so
		 * the unmapped registers (R0..R7, PC, AND CPSR) and some
		 * view of R8..R14 are current.  We also "know" oddities of
		 * register mapping: special cases for R8..R12 and SPSR.
		 *
		 * Pick some mode with unread registers and read them all.
		 * Repeat until done.
		 */
		for (unsigned i = 0; i < cache->num_regs; i++) {
			struct arm_reg *r;

			if (cache->reg_list[i].valid)
				continue;
			r = cache->reg_list[i].arch_info;

			/* may need to pick a mode and set CPSR */
			if (!did_read) {
				did_read = true;
				mode = r->mode;

				/* For R8..R12 when we've entered debug
				 * state in FIQ mode... patch mode.
				 */
				if (mode == ARM_MODE_ANY)
					mode = ARM_MODE_USR;

				/* REVISIT error checks */
				retval = dpm_modeswitch(dpm, mode);
			}
			if (r->mode != mode)
				continue;

			/* CPSR was read, so "R16" must mean SPSR */
			retval = dpm_read_reg(dpm,
					&cache->reg_list[i],
					(r->num == 16) ? 17 : r->num);

		}

	} while (did_read);

	retval = dpm_modeswitch(dpm, ARM_MODE_ANY);
	/* (void) */ dpm->finish(dpm);
done:
	return retval;
}


/*----------------------------------------------------------------------*/

/*
 * Breakpoint and Watchpoint support.
 *
 * Hardware {break,watch}points are usually left active, to minimize
 * debug entry/exit costs.  When they are set or cleared, it's done in
 * batches.  Also, DPM-conformant hardware can update debug registers
 * regardless of whether the CPU is running or halted ... though that
 * fact isn't currently leveraged.
 */

static int dpm_watchpoint_setup(struct arm_dpm *dpm, unsigned index,
		struct watchpoint *wp)
{
	uint32_t addr = wp->address;
	uint32_t control;

	/* this hardware doesn't support data value matching or masking */
	if (wp->value || wp->mask != ~(uint32_t)0) {
		LOG_DEBUG("watchpoint values and masking not supported");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	control = (1 << 0)	/* enable */
		| (3 << 1);	/* both user and privileged access */

	switch (wp->rw) {
	case WPT_READ:
		control |= 1 << 3;
		break;
	case WPT_WRITE:
		control |= 2 << 3;
		break;
	case WPT_ACCESS:
		control |= 3 << 3;
		break;
	}

	/* Match 1, 2, or all 4 byte addresses in this word.
	 *
	 * FIXME:  v7 hardware allows lengths up to 2 GB, and has eight
	 * byte address select bits.  Support larger wp->length, if addr
	 * is suitably aligned.
	 */
	switch (wp->length) {
	case 1:
		control |= (1 << (addr & 3)) << 5;
		addr &= ~3;
		break;
	case 2:
		/* require 2-byte alignment */
		if (!(addr & 1)) {
			control |= (3 << (addr & 2)) << 5;
			break;
		}
		/* FALL THROUGH */
	case 4:
		/* require 4-byte alignment */
		if (!(addr & 3)) {
			control |= 0xf << 5;
			break;
		}
		/* FALL THROUGH */
	default:
		LOG_DEBUG("bad watchpoint length or alignment");
		return ERROR_INVALID_ARGUMENTS;
	}

	/* other control bits:
	 * bits 9:12 == 0 ... only checking up to four byte addresses (v7 only)
	 * bits 15:14 == 0 ... both secure and nonsecure states (v6.1+ only)
	 * bit 20 == 0 ... not linked to a context ID
	 * bit 28:24 == 0 ... not ignoring N LSBs (v7 only)
	 */

	dpm->dwp[index].wp = wp;
	dpm->dwp[index].control = control;
	dpm->dwp[index].dirty = true;

	/* hardware is updated in write_dirty_registers() */
	return ERROR_OK;
}


static int dpm_add_watchpoint(struct target *target, struct watchpoint *wp)
{
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = arm->dpm;
	int retval = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	if (dpm->bpwp_enable) {
		for (unsigned i = 0; i < dpm->nwp; i++) {
			if (!dpm->dwp[i].wp) {
				retval = dpm_watchpoint_setup(dpm, i, wp);
				break;
			}
		}
	}

	return retval;
}

static int dpm_remove_watchpoint(struct target *target, struct watchpoint *wp)
{
	struct arm *arm = target_to_arm(target);
	struct arm_dpm *dpm = arm->dpm;
	int retval = ERROR_INVALID_ARGUMENTS;

	for (unsigned i = 0; i < dpm->nwp; i++) {
		if (dpm->dwp[i].wp == wp) {
			dpm->dwp[i].wp = NULL;
			dpm->dwp[i].dirty = true;

			/* hardware is updated in write_dirty_registers() */
			retval = ERROR_OK;
			break;
		}
	}

	return retval;
}

void arm_dpm_report_wfar(struct arm_dpm *dpm, uint32_t addr)
{
	switch (dpm->arm->core_state) {
	case ARM_STATE_ARM:
		addr -= 8;
		break;
	case ARM_STATE_THUMB:
	case ARM_STATE_THUMB_EE:
		addr -= 4;
		break;
	case ARM_STATE_JAZELLE:
		/* ?? */
		break;
	}
	dpm->wp_pc = addr;
}

/*----------------------------------------------------------------------*/

/*
 * Other debug and support utilities
 */

void arm_dpm_report_dscr(struct arm_dpm *dpm, uint32_t dscr)
{
	struct target *target = dpm->arm->target;

	dpm->dscr = dscr;

	/* Examine debug reason */
	switch (DSCR_ENTRY(dscr)) {
	case 6:		/* Data abort (v6 only) */
	case 7:		/* Prefetch abort (v6 only) */
		/* FALL THROUGH -- assume a v6 core in abort mode */
	case 0:		/* HALT request from debugger */
	case 4:		/* EDBGRQ */
		target->debug_reason = DBG_REASON_DBGRQ;
		break;
	case 1:		/* HW breakpoint */
	case 3:		/* SW BKPT */
	case 5:		/* vector catch */
		target->debug_reason = DBG_REASON_BREAKPOINT;
		break;
	case 2:		/* asynch watchpoint */
	case 10:	/* precise watchpoint */
		target->debug_reason = DBG_REASON_WATCHPOINT;
		break;
	default:
		target->debug_reason = DBG_REASON_UNDEFINED;
		break;
	}
}

/*----------------------------------------------------------------------*/

/*
 * Setup and management support.
 */

/**
 * Hooks up this DPM to its associated target; call only once.
 * Initially this only covers the register cache.
 *
 * Oh, and watchpoints.  Yeah.
 */
int arm_dpm_setup(struct arm_dpm *dpm)
{
	struct arm *arm = dpm->arm;
	struct target *target = arm->target;
	struct reg_cache *cache;

	arm->dpm = dpm;

	/* register access setup */
	arm->full_context = arm_dpm_full_context;
	arm->read_core_reg = arm_dpm_read_core_reg;
	arm->write_core_reg = arm_dpm_write_core_reg;

	cache = arm_build_reg_cache(target, arm);
	if (!cache)
		return ERROR_FAIL;

	*register_get_last_cache_p(&target->reg_cache) = cache;

	/* coprocessor access setup */
	arm->mrc = dpm_mrc;
	arm->mcr = dpm_mcr;

	/* breakpoint and watchpoint setup */
	target->type->add_watchpoint = dpm_add_watchpoint;
	target->type->remove_watchpoint = dpm_remove_watchpoint;

	/* FIXME add breakpoint support */
	/* FIXME add vector catch support */

	dpm->nbp = 1 + ((dpm->didr >> 24) & 0xf);
	dpm->dbp = calloc(dpm->nbp, sizeof *dpm->dbp);

	dpm->nwp = 1 + ((dpm->didr >> 28) & 0xf);
	dpm->dwp = calloc(dpm->nwp, sizeof *dpm->dwp);

	if (!dpm->dbp || !dpm->dwp) {
		free(dpm->dbp);
		free(dpm->dwp);
		return ERROR_FAIL;
	}

	LOG_INFO("%s: hardware has %d breakpoints, %d watchpoints",
			target_name(target), dpm->nbp, dpm->nwp);

	/* REVISIT ... and some of those breakpoints could match
	 * execution context IDs...
	 */

	return ERROR_OK;
}

/**
 * Reinitializes DPM state at the beginning of a new debug session
 * or after a reset which may have affected the debug module.
 */
int arm_dpm_initialize(struct arm_dpm *dpm)
{
	/* Disable all breakpoints and watchpoints at startup. */
	if (dpm->bpwp_disable) {
		unsigned i;

		for (i = 0; i < dpm->nbp; i++)
			(void) dpm->bpwp_disable(dpm, i);
		for (i = 0; i < dpm->nwp; i++)
			(void) dpm->bpwp_disable(dpm, 16 + i);
	} else
		LOG_WARNING("%s: can't disable breakpoints and watchpoints",
			target_name(dpm->arm->target));

	return ERROR_OK;
}
