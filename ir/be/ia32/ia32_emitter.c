/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief       This file implements the ia32 node emitter.
 * @author      Christian Wuerdig, Matthias Braun
 *
 * Summary table for x86 floatingpoint compares:
 * (remember effect of unordered on x86: ZF=1, PF=1, CF=1)
 *
 *   pnc_Eq  => !P && E
 *   pnc_Lt  => !P && B
 *   pnc_Le  => !P && BE
 *   pnc_Gt  => A
 *   pnc_Ge  => AE
 *   pnc_Lg  => NE
 *   pnc_Leg => NP  (ordered)
 *   pnc_Uo  => P
 *   pnc_Ue  => E
 *   pnc_Ul  => B
 *   pnc_Ule => BE
 *   pnc_Ug  => P || A
 *   pnc_Uge => P || AE
 *   pnc_Ne  => P || NE
 */
#include "config.h"

#include <limits.h>

#include "xmalloc.h"
#include "tv.h"
#include "iredges.h"
#include "debug.h"
#include "irgwalk.h"
#include "irprintf.h"
#include "irop_t.h"
#include "irargs_t.h"
#include "irprog_t.h"
#include "iredges_t.h"
#include "irtools.h"
#include "execfreq.h"
#include "error.h"
#include "raw_bitset.h"
#include "dbginfo.h"
#include "lc_opts.h"
#include "ircons.h"

#include "besched.h"
#include "benode.h"
#include "beabi.h"
#include "bedwarf.h"
#include "beemitter.h"
#include "begnuas.h"
#include "beirg.h"

#include "ia32_emitter.h"
#include "ia32_common_transform.h"
#include "gen_ia32_emitter.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_nodes_attr.h"
#include "ia32_new_nodes.h"
#include "ia32_architecture.h"
#include "bearch_ia32_t.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static const ia32_isa_t *isa;
static char              pic_base_label[128];
static ir_label_t        exc_label_id;
static int               mark_spill_reload = 0;
static int               do_pic;

static bool              sp_relative;
static int               frame_type_size;
static int               callframe_offset;

/** Return the next block in Block schedule */
static ir_node *get_prev_block_sched(const ir_node *block)
{
	return (ir_node*)get_irn_link(block);
}

/** Checks if the current block is a fall-through target. */
static int is_fallthrough(const ir_node *cfgpred)
{
	ir_node *pred;

	if (!is_Proj(cfgpred))
		return 1;
	pred = get_Proj_pred(cfgpred);
	if (is_ia32_SwitchJmp(pred))
		return 0;

	return 1;
}

/**
 * returns non-zero if the given block needs a label
 * because of being a jump-target (and not a fall-through)
 */
static int block_needs_label(const ir_node *block)
{
	int need_label = 1;
	int  n_cfgpreds = get_Block_n_cfgpreds(block);

	if (get_Block_entity(block) != NULL)
		return 1;

	if (n_cfgpreds == 0) {
		need_label = 0;
	} else if (n_cfgpreds == 1) {
		ir_node *cfgpred       = get_Block_cfgpred(block, 0);
		ir_node *cfgpred_block = get_nodes_block(cfgpred);

		if (get_prev_block_sched(block) == cfgpred_block
				&& is_fallthrough(cfgpred)) {
			need_label = 0;
		}
	}

	return need_label;
}

/**
 * Add a number to a prefix. This number will not be used a second time.
 */
static char *get_unique_label(char *buf, size_t buflen, const char *prefix)
{
	static unsigned long id = 0;
	snprintf(buf, buflen, "%s%s%lu", be_gas_get_private_prefix(), prefix, ++id);
	return buf;
}

/**
 * Emit the name of the 8bit low register
 */
static void emit_8bit_register(const arch_register_t *reg)
{
	assert(reg->index == REG_GP_EAX || reg->index == REG_GP_EBX
			|| reg->index == REG_GP_ECX || reg->index == REG_GP_EDX);

	be_emit_char('%');
	be_emit_char(reg->name[1]); /* get the basic name of the register */
	be_emit_char('l');
}

/**
 * Emit the name of the 8bit high register
 */
static void emit_8bit_register_high(const arch_register_t *reg)
{
	assert(reg->index == REG_GP_EAX || reg->index == REG_GP_EBX
			|| reg->index == REG_GP_ECX || reg->index == REG_GP_EDX);

	be_emit_char('%');
	be_emit_char(reg->name[1]); /* get the basic name of the register */
	be_emit_char('h');
}

static void emit_16bit_register(const arch_register_t *reg)
{
	be_emit_char('%');
	be_emit_string(reg->name + 1); /* skip the 'e' prefix of the 32bit names */
}

/**
 * emit a register, possible shortened by a mode
 *
 * @param reg   the register
 * @param mode  the mode of the register or NULL for full register
 */
static void emit_register(const arch_register_t *reg, const ir_mode *mode)
{
	if (mode != NULL) {
		int size = get_mode_size_bits(mode);
		switch (size) {
			case  8: emit_8bit_register(reg);  return;
			case 16: emit_16bit_register(reg); return;
		}
		assert(mode_is_float(mode) || size == 32);
	}

	be_emit_char('%');
	be_emit_string(reg->name);
}

static void ia32_emit_entity(ir_entity *entity, int no_pic_adjust)
{
	be_gas_emit_entity(entity);

	if (get_entity_owner(entity) == get_tls_type()) {
		if (!entity_has_definition(entity)) {
			be_emit_cstring("@INDNTPOFF");
		} else {
			be_emit_cstring("@NTPOFF");
		}
	}

	if (do_pic && !no_pic_adjust) {
		be_emit_char('-');
		be_emit_string(pic_base_label);
	}
}

static void emit_ia32_Immediate_no_prefix(const ir_node *node)
{
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(node);

	if (attr->symconst != NULL) {
		if (attr->sc_sign)
			be_emit_char('-');
		ia32_emit_entity(attr->symconst, attr->no_pic_adjust);
	}
	if (attr->symconst == NULL || attr->offset != 0) {
		if (attr->symconst != NULL) {
			be_emit_irprintf("%+d", attr->offset);
		} else {
			be_emit_irprintf("0x%X", attr->offset);
		}
	}
}

static void emit_ia32_Immediate(const ir_node *node)
{
	be_emit_char('$');
	emit_ia32_Immediate_no_prefix(node);
}

static void ia32_emit_mode_suffix_mode(const ir_mode *mode)
{
	assert(mode_is_int(mode) || mode_is_reference(mode));
	switch (get_mode_size_bits(mode)) {
		case 8:  be_emit_char('b');     return;
		case 16: be_emit_char('w');     return;
		case 32: be_emit_char('l');     return;
		/* gas docu says q is the suffix but gcc, objdump and icc use ll
		 * apparently */
		case 64: be_emit_cstring("ll"); return;
	}
	panic("Can't output mode_suffix for %+F", mode);
}

static void ia32_emit_x87_mode_suffix(ir_node const *const node)
{
	ir_mode *mode;

	/* we only need to emit the mode on address mode */
	if (get_ia32_op_type(node) == ia32_Normal)
		return;

	mode = get_ia32_ls_mode(node);
	assert(mode != NULL);

	if (mode_is_float(mode)) {
		switch (get_mode_size_bits(mode)) {
			case  32: be_emit_char('s'); return;
			case  64: be_emit_char('l'); return;
			/* long doubles have different sizes due to alignment on different
			 * platforms. */
			case  80:
			case  96:
			case 128: be_emit_char('t'); return;
		}
	} else {
		assert(mode_is_int(mode) || mode_is_reference(mode));
		switch (get_mode_size_bits(mode)) {
			case 16: be_emit_char('s');     return;
			case 32: be_emit_char('l');     return;
			/* gas docu says q is the suffix but gcc, objdump and icc use ll
			 * apparently */
			case 64: be_emit_cstring("ll"); return;
		}
	}
	panic("Can't output mode_suffix for %+F", mode);
}

static char get_xmm_mode_suffix(ir_mode *mode)
{
	assert(mode_is_float(mode));
	switch (get_mode_size_bits(mode)) {
	case 32: return 's';
	case 64: return 'd';
	default: panic("Invalid XMM mode");
	}
}

static void ia32_emit_xmm_mode_suffix(ir_node const *const node)
{
	ir_mode *mode = get_ia32_ls_mode(node);
	assert(mode != NULL);
	be_emit_char(get_xmm_mode_suffix(mode));
}

/**
 * Returns the target block for a control flow node.
 */
static ir_node *get_cfop_target_block(const ir_node *irn)
{
	assert(get_irn_mode(irn) == mode_X);
	return (ir_node*)get_irn_link(irn);
}

/**
 * Emits the target label for a control flow node.
 */
static void ia32_emit_cfop_target(const ir_node *node)
{
	ir_node *block = get_cfop_target_block(node);
	be_gas_emit_block_name(block);
}

/**
 * Emit the suffix for a compare instruction.
 */
static void ia32_emit_condition_code(ia32_condition_code_t cc)
{
	switch (cc) {
	case ia32_cc_overflow:      be_emit_cstring("o");  return;
	case ia32_cc_not_overflow:  be_emit_cstring("no"); return;
	case ia32_cc_float_below:
	case ia32_cc_float_unordered_below:
	case ia32_cc_below:         be_emit_cstring("b");  return;
	case ia32_cc_float_above_equal:
	case ia32_cc_float_unordered_above_equal:
	case ia32_cc_above_equal:   be_emit_cstring("ae"); return;
	case ia32_cc_float_equal:
	case ia32_cc_equal:         be_emit_cstring("e");  return;
	case ia32_cc_float_not_equal:
	case ia32_cc_not_equal:     be_emit_cstring("ne"); return;
	case ia32_cc_float_below_equal:
	case ia32_cc_float_unordered_below_equal:
	case ia32_cc_below_equal:   be_emit_cstring("be"); return;
	case ia32_cc_float_above:
	case ia32_cc_float_unordered_above:
	case ia32_cc_above:         be_emit_cstring("a");  return;
	case ia32_cc_sign:          be_emit_cstring("s");  return;
	case ia32_cc_not_sign:      be_emit_cstring("ns"); return;
	case ia32_cc_parity:        be_emit_cstring("p");  return;
	case ia32_cc_not_parity:    be_emit_cstring("np"); return;
	case ia32_cc_less:          be_emit_cstring("l");  return;
	case ia32_cc_greater_equal: be_emit_cstring("ge"); return;
	case ia32_cc_less_equal:    be_emit_cstring("le"); return;
	case ia32_cc_greater:       be_emit_cstring("g");  return;
	case ia32_cc_float_parity_cases:
	case ia32_cc_additional_float_cases:
		break;
	}
	panic("Invalid ia32 condition code");
}

typedef enum ia32_emit_mod_t {
	EMIT_NONE         = 0,
	EMIT_RESPECT_LS   = 1U << 0,
	EMIT_ALTERNATE_AM = 1U << 1,
	EMIT_LONG         = 1U << 2,
	EMIT_HIGH_REG     = 1U << 3,
	EMIT_LOW_REG      = 1U << 4,
	EMIT_16BIT_REG    = 1U << 5
} ia32_emit_mod_t;
ENUM_BITSET(ia32_emit_mod_t)

/**
 * Emits address mode.
 */
static void ia32_emit_am(ir_node const *const node)
{
	ir_entity *ent       = get_ia32_am_sc(node);
	int        offs      = get_ia32_am_offs_int(node);
	ir_node   *base      = get_irn_n(node, n_ia32_base);
	int        has_base  = !is_ia32_NoReg_GP(base);
	ir_node   *idx       = get_irn_n(node, n_ia32_index);
	int        has_index = !is_ia32_NoReg_GP(idx);

	/* just to be sure... */
	assert(!is_ia32_use_frame(node) || get_ia32_frame_ent(node) != NULL);

	if (get_ia32_am_tls_segment(node))
		be_emit_cstring("%gs:");

	/* emit offset */
	if (ent != NULL) {
		const ia32_attr_t *attr = get_ia32_attr_const(node);
		if (is_ia32_am_sc_sign(node))
			be_emit_char('-');
		ia32_emit_entity(ent, attr->data.am_sc_no_pic_adjust);
	}

	/* also handle special case if nothing is set */
	if (offs != 0 || (ent == NULL && !has_base && !has_index)) {
		if (ent != NULL) {
			be_emit_irprintf("%+d", offs);
		} else {
			be_emit_irprintf("%d", offs);
		}
	}

	if (has_base || has_index) {
		be_emit_char('(');

		/* emit base */
		if (has_base) {
			const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_base);
			emit_register(reg, NULL);
		}

		/* emit index + scale */
		if (has_index) {
			const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_index);
			int scale;
			be_emit_char(',');
			emit_register(reg, NULL);

			scale = get_ia32_am_scale(node);
			if (scale > 0) {
				be_emit_irprintf(",%d", 1 << scale);
			}
		}
		be_emit_char(')');
	}
}

static ia32_condition_code_t determine_final_cc(ir_node const *node, int flags_pos, ia32_condition_code_t cc);

void ia32_emitf(ir_node const *const node, char const *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	be_emit_char('\t');
	for (;;) {
		const char      *start = fmt;
		ia32_emit_mod_t  mod   = EMIT_NONE;

		while (*fmt != '%' && *fmt != '\n' && *fmt != '\0')
			++fmt;
		if (fmt != start) {
			be_emit_string_len(start, fmt - start);
		}

		if (*fmt == '\n') {
			be_emit_char('\n');
			be_emit_write_line();
			be_emit_char('\t');
			++fmt;
			if (*fmt == '\0')
				break;
			continue;
		}

		if (*fmt == '\0')
			break;

		++fmt;
		for (;;) {
			switch (*fmt) {
			case '*': mod |= EMIT_ALTERNATE_AM; break;
			case '#': mod |= EMIT_RESPECT_LS;   break;
			case 'l': mod |= EMIT_LONG;         break;
			case '>': mod |= EMIT_HIGH_REG;     break;
			case '<': mod |= EMIT_LOW_REG;      break;
			case '^': mod |= EMIT_16BIT_REG;    break;
			default:
				goto end_of_mods;
			}
			++fmt;
		}
end_of_mods:

		switch (*fmt++) {
			arch_register_t const *reg;
			ir_node         const *imm;

			case '%':
				be_emit_char('%');
				break;

			case 'A': {
				switch (*fmt++) {
					case 'F':
						if (get_ia32_op_type(node) == ia32_AddrModeS) {
							goto emit_AM;
						} else {
							assert(get_ia32_op_type(node) == ia32_Normal);
							ia32_x87_attr_t const *const x87_attr = get_ia32_x87_attr_const(node);
							arch_register_t const *const out      = x87_attr->x87[2];
							arch_register_t const *      in       = x87_attr->x87[1];
							if (out == in)
								in = x87_attr->x87[0];
							be_emit_irprintf("%%%s, %%%s", in->name, out->name);
							break;
						}

emit_AM:
					case 'M':
						if (mod & EMIT_ALTERNATE_AM)
							be_emit_char('*');
						ia32_emit_am(node);
						break;

					case 'R':
						reg = va_arg(ap, const arch_register_t*);
						if (get_ia32_op_type(node) == ia32_AddrModeS) {
							goto emit_AM;
						} else {
							goto emit_R;
						}

					case 'S':
						if (get_ia32_op_type(node) == ia32_AddrModeS) {
							++fmt;
							goto emit_AM;
						} else {
							assert(get_ia32_op_type(node) == ia32_Normal);
							goto emit_S;
						}

					default: goto unknown;
				}
				break;
			}

			case 'B':
				imm = get_irn_n(node, n_ia32_binary_right);
				if (is_ia32_Immediate(imm)) {
					emit_ia32_Immediate(imm);
					be_emit_cstring(", ");
					if (get_ia32_op_type(node) == ia32_AddrModeS) {
						ia32_emit_am(node);
					} else {
						assert(get_ia32_op_type(node) == ia32_Normal);
						reg = arch_get_irn_register_in(node, n_ia32_binary_left);
						emit_register(reg, get_ia32_ls_mode(node));
					}
				} else {
					if (get_ia32_op_type(node) == ia32_AddrModeS) {
						ia32_emit_am(node);
					} else {
						assert(get_ia32_op_type(node) == ia32_Normal);
						reg = arch_get_irn_register_in(node, n_ia32_binary_right);
						emit_register(reg, get_ia32_ls_mode(node));
					}
					be_emit_cstring(", ");
					reg = arch_get_irn_register_in(node, n_ia32_binary_left);
					emit_register(reg, get_ia32_ls_mode(node));
				}
				break;

			case 'D':
				if (*fmt < '0' || '9' < *fmt)
					goto unknown;
				reg = arch_get_irn_register_out(node, *fmt++ - '0');
				goto emit_R;

			case 'F':
				if (*fmt == 'M') {
					++fmt;
					ia32_emit_x87_mode_suffix(node);
				} else if (*fmt == 'P') {
					++fmt;
					ia32_x87_attr_t const *const attr = get_ia32_x87_attr_const(node);
					if (attr->pop)
						be_emit_char('p');
				} else if (*fmt == 'R') {
					++fmt;
					/* NOTE: Work around a gas quirk for non-commutative operations if the
					 * destination register is not %st0.  In this case r/non-r is swapped.
					 * %st0 = %st0 - %st1 -> fsub  %st1, %st0 (as expected)
					 * %st0 = %st1 - %st0 -> fsubr %st1, %st0 (as expected)
					 * %st1 = %st0 - %st1 -> fsub  %st0, %st1 (expected: fsubr)
					 * %st1 = %st1 - %st0 -> fsubr %st0, %st1 (expected: fsub)
					 * In fact this corresponds to the encoding of the instruction:
					 * - The r suffix selects whether %st0 is on the left (no r) or on the
					 *   right (r) side of the executed operation.
					 * - The placement of %st0 selects whether the result is written to
					 *   %st0 (right) or the other register (left).
					 * This results in testing whether the left operand register is %st0
					 * instead of the expected test whether the output register equals the
					 * left operand register. */
					ia32_x87_attr_t const *const attr = get_ia32_x87_attr_const(node);
					if (get_ia32_op_type(node) == ia32_Normal ?
					    attr->x87[0] != &ia32_registers[REG_ST0] :
					    attr->attr.data.ins_permuted)
						be_emit_char('r');
				} else if (*fmt == 'X') {
					++fmt;
					ia32_emit_xmm_mode_suffix(node);
				} else if ('0' <= *fmt && *fmt <= '2') {
					const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
					be_emit_char('%');
					be_emit_string(attr->x87[*fmt++ - '0']->name);
				} else {
					goto unknown;
				}
				break;

			case 'I':
				imm = node;
emit_I:
				if (!(mod & EMIT_ALTERNATE_AM))
					be_emit_char('$');
				emit_ia32_Immediate_no_prefix(imm);
				break;

			case 'L':
				ia32_emit_cfop_target(node);
				break;

			case 'M': {
				ir_mode *mode = get_ia32_ls_mode(node);
				if (!mode)
					mode = mode_Iu;
				if (mod & EMIT_RESPECT_LS) {
					if (get_mode_size_bits(mode) == 32)
						break;
					be_emit_char(mode_is_signed(mode) ? 's' : 'z');
				}
				ia32_emit_mode_suffix_mode(mode);
				break;
			}

			case 'P': {
				ia32_condition_code_t cc;
				if (*fmt == 'X') {
					++fmt;
					cc = (ia32_condition_code_t)va_arg(ap, int);
				} else if ('0' <= *fmt && *fmt <= '9') {
					cc = get_ia32_condcode(node);
					cc = determine_final_cc(node, *fmt - '0', cc);
					++fmt;
				} else {
					goto unknown;
				}
				ia32_emit_condition_code(cc);
				break;
			}

			case 'R':
				reg = va_arg(ap, const arch_register_t*);
emit_R:
				if (mod & EMIT_ALTERNATE_AM)
					be_emit_char('*');
				if (mod & EMIT_HIGH_REG) {
					emit_8bit_register_high(reg);
				} else if (mod & EMIT_LOW_REG) {
					emit_8bit_register(reg);
				} else if (mod & EMIT_16BIT_REG) {
					emit_16bit_register(reg);
				} else {
					emit_register(reg, mod & EMIT_RESPECT_LS ? get_ia32_ls_mode(node) : NULL);
				}
				break;

emit_S:
			case 'S': {
				unsigned pos;

				if (*fmt < '0' || '9' < *fmt)
					goto unknown;

				pos = *fmt++ - '0';
				imm = get_irn_n(node, pos);
				if (is_ia32_Immediate(imm)) {
					goto emit_I;
				} else {
					reg = arch_get_irn_register_in(node, pos);
					goto emit_R;
				}
			}

			case 's': {
				const char *str = va_arg(ap, const char*);
				be_emit_string(str);
				break;
			}

			case 'u':
				if (mod & EMIT_LONG) {
					unsigned long num = va_arg(ap, unsigned long);
					be_emit_irprintf("%lu", num);
				} else {
					unsigned num = va_arg(ap, unsigned);
					be_emit_irprintf("%u", num);
				}
				break;

			case 'd':
				if (mod & EMIT_LONG) {
					long num = va_arg(ap, long);
					be_emit_irprintf("%ld", num);
				} else {
					int num = va_arg(ap, int);
					be_emit_irprintf("%d", num);
				}
				break;

			default:
unknown:
				panic("unknown format conversion");
		}
	}

	be_emit_finish_line_gas(node);
	va_end(ap);
}

static void emit_ia32_IMul(const ir_node *node)
{
	ir_node               *left    = get_irn_n(node, n_ia32_IMul_left);
	const arch_register_t *out_reg = arch_get_irn_register_out(node, pn_ia32_IMul_res);

	/* do we need the 3-address form? */
	if (is_ia32_NoReg_GP(left) ||
			arch_get_irn_register_in(node, n_ia32_IMul_left) != out_reg) {
		ia32_emitf(node, "imul%M %#S4, %#AS3, %#D0");
	} else {
		ia32_emitf(node, "imul%M %#AS4, %#S3");
	}
}

/**
 * walks up a tree of copies/perms/spills/reloads to find the original value
 * that is moved around
 */
static ir_node *find_original_value(ir_node *node)
{
	if (irn_visited(node))
		return NULL;

	mark_irn_visited(node);
	if (be_is_Copy(node)) {
		return find_original_value(be_get_Copy_op(node));
	} else if (be_is_CopyKeep(node)) {
		return find_original_value(be_get_CopyKeep_op(node));
	} else if (is_Proj(node)) {
		ir_node *pred = get_Proj_pred(node);
		if (be_is_Perm(pred)) {
			return find_original_value(get_irn_n(pred, get_Proj_proj(node)));
		} else if (be_is_MemPerm(pred)) {
			return find_original_value(get_irn_n(pred, get_Proj_proj(node) + 1));
		} else if (is_ia32_Load(pred)) {
			return find_original_value(get_irn_n(pred, n_ia32_Load_mem));
		} else if (is_ia32_Store(pred)) {
			return find_original_value(get_irn_n(pred, n_ia32_Store_val));
		} else {
			return node;
		}
	} else if (is_Phi(node)) {
		int i, arity;
		arity = get_irn_arity(node);
		for (i = 0; i < arity; ++i) {
			ir_node *in  = get_irn_n(node, i);
			ir_node *res = find_original_value(in);

			if (res != NULL)
				return res;
		}
		return NULL;
	} else {
		return node;
	}
}

static ia32_condition_code_t determine_final_cc(const ir_node *node,
		int flags_pos, ia32_condition_code_t cc)
{
	ir_node           *flags = get_irn_n(node, flags_pos);
	const ia32_attr_t *flags_attr;
	flags = skip_Proj(flags);

	if (is_ia32_Sahf(flags)) {
		ir_node *cmp = get_irn_n(flags, n_ia32_Sahf_val);
		if (!(is_ia32_FucomFnstsw(cmp) || is_ia32_FucomppFnstsw(cmp) || is_ia32_FtstFnstsw(cmp))) {
			inc_irg_visited(current_ir_graph);
			cmp = find_original_value(cmp);
			assert(cmp != NULL);
			assert(is_ia32_FucomFnstsw(cmp) || is_ia32_FucomppFnstsw(cmp) || is_ia32_FtstFnstsw(cmp));
		}

		flags_attr = get_ia32_attr_const(cmp);
	} else {
		flags_attr = get_ia32_attr_const(flags);
	}

	if (flags_attr->data.ins_permuted)
		cc = ia32_invert_condition_code(cc);
	return cc;
}

/**
 * Emits an exception label for a given node.
 */
static void ia32_emit_exc_label(const ir_node *node)
{
	be_emit_string(be_gas_insn_label_prefix());
	be_emit_irprintf("%lu", get_ia32_exc_label_id(node));
}

/**
 * Returns the Proj with projection number proj and NOT mode_M
 */
static ir_node *get_proj(const ir_node *node, long proj)
{
	ir_node *src;

	assert(get_irn_mode(node) == mode_T && "expected mode_T node");

	foreach_out_edge(node, edge) {
		src = get_edge_src_irn(edge);

		assert(is_Proj(src) && "Proj expected");
		if (get_irn_mode(src) == mode_M)
			continue;

		if (get_Proj_proj(src) == proj)
			return src;
	}
	return NULL;
}

static int can_be_fallthrough(const ir_node *node)
{
	ir_node *target_block = get_cfop_target_block(node);
	ir_node *block        = get_nodes_block(node);
	return get_prev_block_sched(target_block) == block;
}

/**
 * Emits the jump sequence for a conditional jump (cmp + jmp_true + jmp_false)
 */
static void emit_ia32_Jcc(const ir_node *node)
{
	int                   need_parity_label = 0;
	ia32_condition_code_t cc                = get_ia32_condcode(node);
	const ir_node        *proj_true;
	const ir_node        *proj_false;

	cc = determine_final_cc(node, 0, cc);

	/* get both Projs */
	proj_true = get_proj(node, pn_ia32_Jcc_true);
	assert(proj_true && "Jcc without true Proj");

	proj_false = get_proj(node, pn_ia32_Jcc_false);
	assert(proj_false && "Jcc without false Proj");

	if (can_be_fallthrough(proj_true)) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		cc         = ia32_negate_condition_code(cc);
	}

	if (cc & ia32_cc_float_parity_cases) {
		/* Some floating point comparisons require a test of the parity flag,
		 * which indicates that the result is unordered */
		if (cc & ia32_cc_negated) {
			ia32_emitf(proj_true, "jp %L");
		} else {
			/* we need a local label if the false proj is a fallthrough
			 * as the falseblock might have no label emitted then */
			if (can_be_fallthrough(proj_false)) {
				need_parity_label = 1;
				ia32_emitf(proj_false, "jp 1f");
			} else {
				ia32_emitf(proj_false, "jp %L");
			}
		}
	}
	ia32_emitf(proj_true, "j%PX %L", (int)cc);
	if (need_parity_label) {
		be_emit_cstring("1:\n");
		be_emit_write_line();
	}

	/* the second Proj might be a fallthrough */
	if (can_be_fallthrough(proj_false)) {
		if (be_options.verbose_asm)
			ia32_emitf(proj_false, "/* fallthrough to %L */");
	} else {
		ia32_emitf(proj_false, "jmp %L");
	}
}

/**
 * Emits an ia32 Setcc. This is mostly easy but some floating point compares
 * are tricky.
 */
static void emit_ia32_Setcc(const ir_node *node)
{
	const arch_register_t *dreg = arch_get_irn_register_out(node, pn_ia32_Setcc_res);

	ia32_condition_code_t cc = get_ia32_condcode(node);
	cc = determine_final_cc(node, n_ia32_Setcc_eflags, cc);
	if (cc & ia32_cc_float_parity_cases) {
		if (cc & ia32_cc_negated) {
			ia32_emitf(node, "set%PX %<R", (int)cc, dreg);
			ia32_emitf(node, "setp %>R", dreg);
			ia32_emitf(node, "orb %>R, %<R", dreg, dreg);
		} else {
			ia32_emitf(node, "set%PX %<R", (int)cc, dreg);
			ia32_emitf(node, "setnp %>R", dreg);
			ia32_emitf(node, "andb %>R, %<R", dreg, dreg);
		}
	} else {
		ia32_emitf(node, "set%PX %#R", (int)cc, dreg);
	}
}

static void emit_ia32_CMovcc(const ir_node *node)
{
	const ia32_attr_t     *attr = get_ia32_attr_const(node);
	const arch_register_t *out  = arch_get_irn_register_out(node, pn_ia32_res);
	ia32_condition_code_t  cc   = get_ia32_condcode(node);
	const arch_register_t *in_true;
	const arch_register_t *in_false;

	cc = determine_final_cc(node, n_ia32_CMovcc_eflags, cc);
	/* although you can't set ins_permuted in the constructor it might still
	 * be set by memory operand folding
	 * Permuting inputs of a cmov means the condition is negated!
	 */
	if (attr->data.ins_permuted)
		cc = ia32_negate_condition_code(cc);

	in_true  = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_true));
	in_false = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_false));

	/* should be same constraint fullfilled? */
	if (out == in_false) {
		/* yes -> nothing to do */
	} else if (out == in_true) {
		const arch_register_t *tmp;

		assert(get_ia32_op_type(node) == ia32_Normal);

		cc = ia32_negate_condition_code(cc);

		tmp      = in_true;
		in_true  = in_false;
		in_false = tmp;
	} else {
		/* we need a mov */
		ia32_emitf(node, "movl %R, %R", in_false, out);
	}

	if (cc & ia32_cc_float_parity_cases) {
		panic("CMov with floatingpoint compare/parity not supported yet");
	}

	ia32_emitf(node, "cmov%PX %#AR, %#R", (int)cc, in_true, out);
}

/**
 * Emits code for a SwitchJmp
 */
static void emit_ia32_SwitchJmp(const ir_node *node)
{
	ir_entity             *jump_table = get_ia32_am_sc(node);
	const ir_switch_table *table      = get_ia32_switch_table(node);

	ia32_emitf(node, "jmp %*AM");
	be_emit_jump_table(node, table, jump_table, get_cfop_target_block);
}

/**
 * Emits code for a unconditional jump.
 */
static void emit_ia32_Jmp(const ir_node *node)
{
	/* we have a block schedule */
	if (can_be_fallthrough(node)) {
		if (be_options.verbose_asm)
			ia32_emitf(node, "/* fallthrough to %L */");
	} else {
		ia32_emitf(node, "jmp %L");
	}
}

/**
 * Emit an inline assembler operand.
 *
 * @param node  the ia32_ASM node
 * @param s     points to the operand (a %c)
 *
 * @return  pointer to the first char in s NOT in the current operand
 */
static const char* emit_asm_operand(const ir_node *node, const char *s)
{
	const ia32_attr_t     *ia32_attr = get_ia32_attr_const(node);
	const ia32_asm_attr_t *attr      = CONST_CAST_IA32_ATTR(ia32_asm_attr_t,
                                                            ia32_attr);
	const arch_register_t *reg;
	const ia32_asm_reg_t  *asm_regs = attr->register_map;
	const ia32_asm_reg_t  *asm_reg;
	char                   c;
	char                   modifier = 0;
	int                    num;
	int                    p;

	assert(*s == '%');
	c = *(++s);

	/* parse modifiers */
	switch (c) {
	case 0:
		ir_fprintf(stderr, "Warning: asm text (%+F) ends with %%\n", node);
		be_emit_char('%');
		return s + 1;
	case '%':
		be_emit_char('%');
		return s + 1;
	case 'w':
	case 'b':
	case 'h':
		modifier = c;
		++s;
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		break;
	default:
		ir_fprintf(stderr,
				"Warning: asm text (%+F) contains unknown modifier '%c' for asm op\n",
				node, c);
		++s;
		break;
	}

	/* parse number */
	if (sscanf(s, "%d%n", &num, &p) != 1) {
		ir_fprintf(stderr, "Warning: Couldn't parse assembler operand (%+F)\n",
		           node);
		return s;
	} else {
		s += p;
	}

	if (num < 0 || ARR_LEN(asm_regs) <= (size_t)num) {
		ir_fprintf(stderr,
				"Error: Custom assembler references invalid input/output (%+F)\n",
				node);
		return s;
	}
	asm_reg = & asm_regs[num];
	assert(asm_reg->valid);

	/* get register */
	if (asm_reg->use_input == 0) {
		reg = arch_get_irn_register_out(node, asm_reg->inout_pos);
	} else {
		ir_node *pred = get_irn_n(node, asm_reg->inout_pos);

		/* might be an immediate value */
		if (is_ia32_Immediate(pred)) {
			emit_ia32_Immediate(pred);
			return s;
		}
		reg = arch_get_irn_register_in(node, asm_reg->inout_pos);
	}
	if (reg == NULL) {
		ir_fprintf(stderr,
				"Warning: no register assigned for %d asm op (%+F)\n",
				num, node);
		return s;
	}

	if (asm_reg->memory) {
		be_emit_char('(');
	}

	/* emit it */
	if (modifier != 0) {
		switch (modifier) {
		case 'b':
			emit_8bit_register(reg);
			break;
		case 'h':
			emit_8bit_register_high(reg);
			break;
		case 'w':
			emit_16bit_register(reg);
			break;
		default:
			panic("Invalid asm op modifier");
		}
	} else {
		emit_register(reg, asm_reg->memory ? mode_Iu : asm_reg->mode);
	}

	if (asm_reg->memory) {
		be_emit_char(')');
	}

	return s;
}

/**
 * Emits code for an ASM pseudo op.
 */
static void emit_ia32_Asm(const ir_node *node)
{
	const void            *gen_attr = get_irn_generic_attr_const(node);
	const ia32_asm_attr_t *attr
		= CONST_CAST_IA32_ATTR(ia32_asm_attr_t, gen_attr);
	ident                 *asm_text = attr->asm_text;
	const char            *s        = get_id_str(asm_text);

	be_emit_cstring("#APP\n");
	be_emit_write_line();

	if (s[0] != '\t')
		be_emit_char('\t');

	while (*s != 0) {
		if (*s == '%') {
			s = emit_asm_operand(node, s);
		} else {
			be_emit_char(*s++);
		}
	}

	be_emit_cstring("\n#NO_APP\n");
	be_emit_write_line();
}


/**
 * Emit movsb/w instructions to make mov count divideable by 4
 */
static void emit_CopyB_prolog(unsigned size)
{
	if (size & 1)
		ia32_emitf(NULL, "movsb");
	if (size & 2)
		ia32_emitf(NULL, "movsw");
}

/**
 * Emit rep movsd instruction for memcopy.
 */
static void emit_ia32_CopyB(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);

	emit_CopyB_prolog(size);
	ia32_emitf(node, "rep movsd");
}

/**
 * Emits unrolled memcopy.
 */
static void emit_ia32_CopyB_i(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);

	emit_CopyB_prolog(size);

	size >>= 2;
	while (size--) {
		ia32_emitf(NULL, "movsd");
	}
}


/**
 * Emit code for conversions (I, FP), (FP, I) and (FP, FP).
 */
static void emit_ia32_Conv_with_FP(const ir_node *node, const char* conv_f,
		const char* conv_d)
{
	ir_mode            *ls_mode = get_ia32_ls_mode(node);
	int                 ls_bits = get_mode_size_bits(ls_mode);
	const char         *conv    = ls_bits == 32 ? conv_f : conv_d;

	ia32_emitf(node, "cvt%s %AS3, %D0", conv);
}

static void emit_ia32_Conv_I2FP(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "si2ss", "si2sd");
}

static void emit_ia32_Conv_FP2I(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "ss2si", "sd2si");
}

static void emit_ia32_Conv_FP2FP(const ir_node *node)
{
	emit_ia32_Conv_with_FP(node, "sd2ss", "ss2sd");
}

/**
 * Emits code to increase stack pointer.
 */
static void emit_be_IncSP(const ir_node *node)
{
	int offs = be_get_IncSP_offset(node);

	if (offs == 0)
		return;

	if (offs > 0) {
		ia32_emitf(node, "subl $%u, %D0", offs);
	} else {
		ia32_emitf(node, "addl $%u, %D0", -offs);
	}
}

/**
 * Emits code for Copy/CopyKeep.
 */
static void Copy_emitter(const ir_node *node, const ir_node *op)
{
	const arch_register_t *in  = arch_get_irn_register(op);
	const arch_register_t *out = arch_get_irn_register(node);

	if (in == out) {
		return;
	}
	/* copies of vf nodes aren't real... */
	if (arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_vfp])
		return;

	ia32_emitf(node, "movl %R, %R", in, out);
}

static void emit_be_Copy(const ir_node *node)
{
	Copy_emitter(node, be_get_Copy_op(node));
}

static void emit_be_CopyKeep(const ir_node *node)
{
	Copy_emitter(node, be_get_CopyKeep_op(node));
}

/**
 * Emits code for exchange.
 */
static void emit_be_Perm(const ir_node *node)
{
	const arch_register_t *in0, *in1;
	const arch_register_class_t *cls0, *cls1;

	in0 = arch_get_irn_register(get_irn_n(node, 0));
	in1 = arch_get_irn_register(get_irn_n(node, 1));

	cls0 = arch_register_get_class(in0);
	cls1 = arch_register_get_class(in1);

	assert(cls0 == cls1 && "Register class mismatch at Perm");

	if (cls0 == &ia32_reg_classes[CLASS_ia32_gp]) {
		ia32_emitf(node, "xchg %R, %R", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_xmm]) {
		ia32_emitf(NULL, "xorpd %R, %R", in1, in0);
		ia32_emitf(NULL, "xorpd %R, %R", in0, in1);
		ia32_emitf(node, "xorpd %R, %R", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_vfp]) {
		/* is a NOP */
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_st]) {
		/* is a NOP */
	} else {
		panic("unexpected register class in be_Perm (%+F)", node);
	}
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_mov(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "movl %R, %R", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_neg(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "negl %R", reg);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_sbb0(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "sbbl $0, %R", reg);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_sbb(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "sbbl %R, %R", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_xchg(const ir_node* node, const arch_register_t *src, const arch_register_t *dst)
{
	ia32_emitf(node, "xchgl %R, %R", src, dst);
}

/* helper function for emit_ia32_Minus64Bit */
static void emit_zero(const ir_node* node, const arch_register_t *reg)
{
	ia32_emitf(node, "xorl %R, %R", reg, reg);
}

static void emit_ia32_Minus64Bit(const ir_node *node)
{
	const arch_register_t *in_lo  = arch_get_irn_register_in(node, 0);
	const arch_register_t *in_hi  = arch_get_irn_register_in(node, 1);
	const arch_register_t *out_lo = arch_get_irn_register_out(node, 0);
	const arch_register_t *out_hi = arch_get_irn_register_out(node, 1);

	if (out_lo == in_lo) {
		if (out_hi != in_hi) {
			/* a -> a, b -> d */
			goto zero_neg;
		} else {
			/* a -> a, b -> b */
			goto normal_neg;
		}
	} else if (out_lo == in_hi) {
		if (out_hi == in_lo) {
			/* a -> b, b -> a */
			emit_xchg(node, in_lo, in_hi);
			goto normal_neg;
		} else {
			/* a -> b, b -> d */
			emit_mov(node, in_hi, out_hi);
			emit_mov(node, in_lo, out_lo);
			goto normal_neg;
		}
	} else {
		if (out_hi == in_lo) {
			/* a -> c, b -> a */
			emit_mov(node, in_lo, out_lo);
			goto zero_neg;
		} else if (out_hi == in_hi) {
			/* a -> c, b -> b */
			emit_mov(node, in_lo, out_lo);
			goto normal_neg;
		} else {
			/* a -> c, b -> d */
			emit_mov(node, in_lo, out_lo);
			goto zero_neg;
		}
	}

normal_neg:
	emit_neg( node, out_hi);
	emit_neg( node, out_lo);
	emit_sbb0(node, out_hi);
	return;

zero_neg:
	emit_zero(node, out_hi);
	emit_neg( node, out_lo);
	emit_sbb( node, in_hi, out_hi);
}

static void emit_ia32_GetEIP(const ir_node *node)
{
	ia32_emitf(node, "call %s", pic_base_label);
	be_emit_irprintf("%s:\n", pic_base_label);
	be_emit_write_line();
	ia32_emitf(node, "popl %D0");
}

static void emit_ia32_ClimbFrame(const ir_node *node)
{
	const ia32_climbframe_attr_t *attr = get_ia32_climbframe_attr_const(node);

	ia32_emitf(node, "movl %S0, %D0");
	ia32_emitf(node, "movl $%u, %S1", attr->count);
	be_gas_emit_block_name(node);
	be_emit_cstring(":\n");
	be_emit_write_line();
	ia32_emitf(node, "movl (%D0), %D0");
	ia32_emitf(node, "dec %S1");
	be_emit_cstring("\tjnz ");
	be_gas_emit_block_name(node);
	be_emit_finish_line_gas(node);
}

static void emit_be_Return(const ir_node *node)
{
	unsigned pop = be_Return_get_pop(node);

	if (pop > 0 || be_Return_get_emit_pop(node)) {
		ia32_emitf(node, "ret $%u", pop);
	} else {
		ia32_emitf(node, "ret");
	}
}

static void emit_Nothing(const ir_node *node)
{
	(void) node;
}


/**
 * Enters the emitter functions for handled nodes into the generic
 * pointer of an opcode.
 */
static void ia32_register_emitters(void)
{
#define IA32_EMIT(a)    op_ia32_##a->ops.generic = (op_func)emit_ia32_##a
#define EMIT(a)         op_##a->ops.generic = (op_func)emit_##a
#define IGN(a)          op_##a->ops.generic = (op_func)emit_Nothing
#define BE_EMIT(a)      op_be_##a->ops.generic = (op_func)emit_be_##a
#define BE_IGN(a)       op_be_##a->ops.generic = (op_func)emit_Nothing

	/* first clear the generic function pointer for all ops */
	ir_clear_opcodes_generic_func();

	/* register all emitter functions defined in spec */
	ia32_register_spec_emitters();

	/* other ia32 emitter functions */
	IA32_EMIT(Asm);
	IA32_EMIT(CMovcc);
	IA32_EMIT(Conv_FP2FP);
	IA32_EMIT(Conv_FP2I);
	IA32_EMIT(Conv_I2FP);
	IA32_EMIT(CopyB);
	IA32_EMIT(CopyB_i);
	IA32_EMIT(GetEIP);
	IA32_EMIT(IMul);
	IA32_EMIT(Jcc);
	IA32_EMIT(Setcc);
	IA32_EMIT(Minus64Bit);
	IA32_EMIT(SwitchJmp);
	IA32_EMIT(ClimbFrame);
	IA32_EMIT(Jmp);

	/* benode emitter */
	BE_EMIT(Copy);
	BE_EMIT(CopyKeep);
	BE_EMIT(IncSP);
	BE_EMIT(Perm);
	BE_EMIT(Return);

	BE_IGN(Keep);
	BE_IGN(Start);

	/* firm emitter */
	IGN(Phi);

#undef BE_EMIT
#undef EMIT
#undef IGN
#undef IA32_EMIT
}

typedef void (*emit_func_ptr) (const ir_node *);

/**
 * Assign and emit an exception label if the current instruction can fail.
 */
static void ia32_assign_exc_label(ir_node *node)
{
	/* assign a new ID to the instruction */
	set_ia32_exc_label_id(node, ++exc_label_id);
	/* print it */
	ia32_emit_exc_label(node);
	be_emit_char(':');
	be_emit_pad_comment();
	be_emit_cstring("/* exception to Block ");
	ia32_emit_cfop_target(node);
	be_emit_cstring(" */\n");
	be_emit_write_line();
}

/**
 * Emits code for a node.
 */
static void ia32_emit_node(ir_node *node)
{
	ir_op *op = get_irn_op(node);

	DBG((dbg, LEVEL_1, "emitting code for %+F\n", node));

	if (is_ia32_irn(node)) {
		if (get_ia32_exc_label(node)) {
			/* emit the exception label of this instruction */
			ia32_assign_exc_label(node);
		}
		if (mark_spill_reload) {
			if (is_ia32_is_spill(node)) {
				ia32_emitf(NULL, "xchg %ebx, %ebx        /* spill mark */");
			}
			if (is_ia32_is_reload(node)) {
				ia32_emitf(NULL, "xchg %edx, %edx        /* reload mark */");
			}
			if (is_ia32_is_remat(node)) {
				ia32_emitf(NULL, "xchg %ecx, %ecx        /* remat mark */");
			}
		}
	}
	if (op->ops.generic) {
		emit_func_ptr func = (emit_func_ptr) op->ops.generic;

		be_dwarf_location(get_irn_dbg_info(node));

		(*func) (node);
	} else {
		emit_Nothing(node);
		ir_fprintf(stderr, "Error: No emit handler for node %+F (%+G, graph %+F)\n", node, node, current_ir_graph);
		abort();
	}

	if (sp_relative) {
		int sp_change = arch_get_sp_bias(node);
		if (sp_change != 0) {
			assert(sp_change != SP_BIAS_RESET);
			callframe_offset += sp_change;
			be_dwarf_callframe_offset(callframe_offset);
		}
	}
}

/**
 * Emits gas alignment directives
 */
static void ia32_emit_alignment(unsigned align, unsigned skip)
{
	ia32_emitf(NULL, ".p2align %u,,%u", align, skip);
}

/**
 * Emits gas alignment directives for Labels depended on cpu architecture.
 */
static void ia32_emit_align_label(void)
{
	unsigned align        = ia32_cg_config.label_alignment;
	unsigned maximum_skip = ia32_cg_config.label_alignment_max_skip;
	ia32_emit_alignment(align, maximum_skip);
}

/**
 * Test whether a block should be aligned.
 * For cpus in the P4/Athlon class it is useful to align jump labels to
 * 16 bytes. However we should only do that if the alignment nops before the
 * label aren't executed more often than we have jumps to the label.
 */
static int should_align_block(const ir_node *block)
{
	static const double DELTA = .0001;
	ir_node *prev      = get_prev_block_sched(block);
	double   prev_freq = 0;  /**< execfreq of the fallthrough block */
	double   jmp_freq  = 0;  /**< execfreq of all non-fallthrough blocks */
	double   block_freq;
	int      i, n_cfgpreds;

	if (ia32_cg_config.label_alignment_factor <= 0)
		return 0;

	block_freq = get_block_execfreq(block);
	if (block_freq < DELTA)
		return 0;

	n_cfgpreds = get_Block_n_cfgpreds(block);
	for (i = 0; i < n_cfgpreds; ++i) {
		const ir_node *pred      = get_Block_cfgpred_block(block, i);
		double         pred_freq = get_block_execfreq(pred);

		if (pred == prev) {
			prev_freq += pred_freq;
		} else {
			jmp_freq  += pred_freq;
		}
	}

	if (prev_freq < DELTA && !(jmp_freq < DELTA))
		return 1;

	jmp_freq /= prev_freq;

	return jmp_freq > ia32_cg_config.label_alignment_factor;
}

/**
 * Emit the block header for a block.
 *
 * @param block       the block
 * @param prev_block  the previous block
 */
static void ia32_emit_block_header(ir_node *block)
{
	ir_graph     *irg        = current_ir_graph;
	int           need_label = block_needs_label(block);

	if (block == get_irg_end_block(irg))
		return;

	if (ia32_cg_config.label_alignment > 0) {
		/* align the current block if:
		 * a) if should be aligned due to its execution frequency
		 * b) there is no fall-through here
		 */
		if (should_align_block(block)) {
			ia32_emit_align_label();
		} else {
			/* if the predecessor block has no fall-through,
			   we can always align the label. */
			int i;
			int has_fallthrough = 0;

			for (i = get_Block_n_cfgpreds(block) - 1; i >= 0; --i) {
				ir_node *cfg_pred = get_Block_cfgpred(block, i);
				if (can_be_fallthrough(cfg_pred)) {
					has_fallthrough = 1;
					break;
				}
			}

			if (!has_fallthrough)
				ia32_emit_align_label();
		}
	}

	be_gas_begin_block(block, need_label);
}

/**
 * Walks over the nodes in a block connected by scheduling edges
 * and emits code for each node.
 */
static void ia32_gen_block(ir_node *block)
{
	ia32_emit_block_header(block);

	if (sp_relative) {
		ir_graph *irg = get_irn_irg(block);
		callframe_offset = 4; /* 4 bytes for the return address */
		/* ESP guessing, TODO perform a real ESP simulation */
		if (block != get_irg_start_block(irg)) {
			callframe_offset += frame_type_size;
		}
		be_dwarf_callframe_offset(callframe_offset);
	}

	/* emit the contents of the block */
	be_dwarf_location(get_irn_dbg_info(block));
	sched_foreach(block, node) {
		ia32_emit_node(node);
	}
}

typedef struct exc_entry {
	ir_node *exc_instr;  /** The instruction that can issue an exception. */
	ir_node *block;      /** The block to call then. */
} exc_entry;

/**
 * Block-walker:
 * Sets labels for control flow nodes (jump target).
 * Links control predecessors to there destination blocks.
 */
static void ia32_gen_labels(ir_node *block, void *data)
{
	exc_entry **exc_list = (exc_entry**)data;
	ir_node *pred;
	int     n;

	for (n = get_Block_n_cfgpreds(block) - 1; n >= 0; --n) {
		pred = get_Block_cfgpred(block, n);
		set_irn_link(pred, block);

		pred = skip_Proj(pred);
		if (is_ia32_irn(pred) && get_ia32_exc_label(pred)) {
			exc_entry e;

			e.exc_instr = pred;
			e.block     = block;
			ARR_APP1(exc_entry, *exc_list, e);
			set_irn_link(pred, block);
		}
	}
}

/**
 * Compare two exception_entries.
 */
static int cmp_exc_entry(const void *a, const void *b)
{
	const exc_entry *ea = (const exc_entry*)a;
	const exc_entry *eb = (const exc_entry*)b;

	if (get_ia32_exc_label_id(ea->exc_instr) < get_ia32_exc_label_id(eb->exc_instr))
		return -1;
	return +1;
}

static parameter_dbg_info_t *construct_parameter_infos(ir_graph *irg)
{
	ir_entity            *entity    = get_irg_entity(irg);
	ir_type              *type      = get_entity_type(entity);
	size_t                n_params  = get_method_n_params(type);
	be_stack_layout_t    *layout    = be_get_irg_stack_layout(irg);
	ir_type              *arg_type  = layout->arg_type;
	size_t                n_members = get_compound_n_members(arg_type);
	parameter_dbg_info_t *infos     = XMALLOCNZ(parameter_dbg_info_t, n_params);
	size_t                i;

	for (i = 0; i < n_members; ++i) {
		ir_entity *member = get_compound_member(arg_type, i);
		size_t     param;
		if (!is_parameter_entity(member))
			continue;
		param = get_entity_parameter_number(member);
		if (param == IR_VA_START_PARAMETER_NUMBER)
			continue;
		assert(infos[param].entity == NULL && infos[param].reg == NULL);
		infos[param].reg    = NULL;
		infos[param].entity = member;
	}

	return infos;
}

/**
 * Main driver. Emits the code for one routine.
 */
void ia32_gen_routine(ir_graph *irg)
{
	ir_entity        *entity    = get_irg_entity(irg);
	exc_entry        *exc_list  = NEW_ARR_F(exc_entry, 0);
	const arch_env_t *arch_env  = be_get_irg_arch_env(irg);
	ia32_irg_data_t  *irg_data  = ia32_get_irg_data(irg);
	ir_node         **blk_sched = irg_data->blk_sched;
	be_stack_layout_t *layout   = be_get_irg_stack_layout(irg);
	parameter_dbg_info_t *infos;
	int i, n;

	isa    = (ia32_isa_t*) arch_env;
	do_pic = be_options.pic;

	be_gas_elf_type_char = '@';

	ia32_register_emitters();

	get_unique_label(pic_base_label, sizeof(pic_base_label), "PIC_BASE");

	infos = construct_parameter_infos(irg);
	be_gas_emit_function_prolog(entity, ia32_cg_config.function_alignment,
	                            infos);
	xfree(infos);

	sp_relative = layout->sp_relative;
	if (layout->sp_relative) {
		ir_type *frame_type = get_irg_frame_type(irg);
		frame_type_size = get_type_size_bytes(frame_type);
		be_dwarf_callframe_register(&ia32_registers[REG_ESP]);
	} else {
		/* well not entirely correct here, we should emit this after the
		 * "movl esp, ebp" */
		be_dwarf_callframe_register(&ia32_registers[REG_EBP]);
		/* TODO: do not hardcode the following */
		be_dwarf_callframe_offset(8);
		be_dwarf_callframe_spilloffset(&ia32_registers[REG_EBP], -8);
	}

	/* we use links to point to target blocks */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, ia32_gen_labels, NULL, &exc_list);

	/* initialize next block links */
	n = ARR_LEN(blk_sched);
	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		ir_node *prev  = i > 0 ? blk_sched[i-1] : NULL;

		set_irn_link(block, prev);
	}

	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];

		ia32_gen_block(block);
	}

	be_gas_emit_function_epilog(entity);

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);

	/* Sort the exception table using the exception label id's.
	   Those are ascending with ascending addresses. */
	qsort(exc_list, ARR_LEN(exc_list), sizeof(exc_list[0]), cmp_exc_entry);
	{
		size_t e;

		for (e = 0; e < ARR_LEN(exc_list); ++e) {
			be_emit_cstring("\t.long ");
			ia32_emit_exc_label(exc_list[e].exc_instr);
			be_emit_char('\n');
			be_emit_cstring("\t.long ");
			be_gas_emit_block_name(exc_list[e].block);
			be_emit_char('\n');
		}
	}
	DEL_ARR_F(exc_list);
}

static const lc_opt_table_entry_t ia32_emitter_options[] = {
	LC_OPT_ENT_BOOL("mark_spill_reload",   "mark spills and reloads with ud opcodes", &mark_spill_reload),
	LC_OPT_LAST
};

/* ==== Experimental binary emitter ==== */

static unsigned char reg_gp_map[N_ia32_gp_REGS];
//static unsigned char reg_mmx_map[N_ia32_mmx_REGS];
//static unsigned char reg_sse_map[N_ia32_xmm_REGS];

static void build_reg_map(void)
{
	reg_gp_map[REG_GP_EAX] = 0x0;
	reg_gp_map[REG_GP_ECX] = 0x1;
	reg_gp_map[REG_GP_EDX] = 0x2;
	reg_gp_map[REG_GP_EBX] = 0x3;
	reg_gp_map[REG_GP_ESP] = 0x4;
	reg_gp_map[REG_GP_EBP] = 0x5;
	reg_gp_map[REG_GP_ESI] = 0x6;
	reg_gp_map[REG_GP_EDI] = 0x7;
}

/** Returns the encoding for a pnc field. */
static unsigned char pnc2cc(ia32_condition_code_t cc)
{
	return cc & 0xf;
}

/** Sign extension bit values for binops */
enum SignExt {
	UNSIGNED_IMM = 0,  /**< unsigned immediate */
	SIGNEXT_IMM  = 2,  /**< sign extended immediate */
};

/** The mod encoding of the ModR/M */
enum Mod {
	MOD_IND          = 0x00, /**< [reg1] */
	MOD_IND_BYTE_OFS = 0x40, /**< [reg1 + byte ofs] */
	MOD_IND_WORD_OFS = 0x80, /**< [reg1 + word ofs] */
	MOD_REG          = 0xC0  /**< reg1 */
};

/** create R/M encoding for ModR/M */
#define ENC_RM(x) (x)
/** create REG encoding for ModR/M */
#define ENC_REG(x) ((x) << 3)

/** create encoding for a SIB byte */
#define ENC_SIB(scale, index, base) ((scale) << 6 | (index) << 3 | (base))

/* Node: The following routines are supposed to append bytes, words, dwords
   to the output stream.
   Currently the implementation is stupid in that it still creates output
   for an "assembler" in the form of .byte, .long
   We will change this when enough infrastructure is there to create complete
   machine code in memory/object files */

static void bemit8(const unsigned char byte)
{
	be_emit_irprintf("\t.byte 0x%x\n", byte);
	be_emit_write_line();
}

static void bemit16(const unsigned short u16)
{
	be_emit_irprintf("\t.word 0x%x\n", u16);
	be_emit_write_line();
}

static void bemit32(const unsigned u32)
{
	be_emit_irprintf("\t.long 0x%x\n", u32);
	be_emit_write_line();
}

/**
 * Emit address of an entity. If @p is_relative is true then a relative
 * offset from behind the address to the entity is created.
 */
static void bemit_entity(ir_entity *entity, bool entity_sign, int offset,
                         bool is_relative)
{
	if (entity == NULL) {
		bemit32(offset);
		return;
	}

	/* the final version should remember the position in the bytestream
	   and patch it with the correct address at linktime... */
	be_emit_cstring("\t.long ");
	if (entity_sign)
		be_emit_char('-');
	be_gas_emit_entity(entity);

	if (get_entity_owner(entity) == get_tls_type()) {
		if (!entity_has_definition(entity)) {
			be_emit_cstring("@INDNTPOFF");
		} else {
			be_emit_cstring("@NTPOFF");
		}
	}

	if (is_relative) {
		be_emit_cstring("-.");
		offset -= 4;
	}

	if (offset != 0) {
		be_emit_irprintf("%+d", offset);
	}
	be_emit_char('\n');
	be_emit_write_line();
}

static void bemit_jmp_destination(const ir_node *dest_block)
{
	be_emit_cstring("\t.long ");
	be_gas_emit_block_name(dest_block);
	be_emit_cstring(" - . - 4\n");
	be_emit_write_line();
}

/* end emit routines, all emitters following here should only use the functions
   above. */

typedef enum reg_modifier {
	REG_LOW  = 0,
	REG_HIGH = 1
} reg_modifier_t;

/** Create a ModR/M byte for src1,src2 registers */
static void bemit_modrr(const arch_register_t *src1,
                        const arch_register_t *src2)
{
	unsigned char modrm = MOD_REG;
	modrm |= ENC_RM(reg_gp_map[src1->index]);
	modrm |= ENC_REG(reg_gp_map[src2->index]);
	bemit8(modrm);
}

/** Create a ModR/M8 byte for src1,src2 registers */
static void bemit_modrr8(reg_modifier_t high_part1, const arch_register_t *src1,
						 reg_modifier_t high_part2, const arch_register_t *src2)
{
	unsigned char modrm = MOD_REG;
	modrm |= ENC_RM(reg_gp_map[src1->index] +  (high_part1 == REG_HIGH ? 4 : 0));
	modrm |= ENC_REG(reg_gp_map[src2->index] + (high_part2 == REG_HIGH ? 4 : 0));
	bemit8(modrm);
}

/** Create a ModR/M byte for one register and extension */
static void bemit_modru(const arch_register_t *reg, unsigned ext)
{
	unsigned char modrm = MOD_REG;
	assert(ext <= 7);
	modrm |= ENC_RM(reg_gp_map[reg->index]);
	modrm |= ENC_REG(ext);
	bemit8(modrm);
}

/** Create a ModR/M8 byte for one register */
static void bemit_modrm8(reg_modifier_t high_part, const arch_register_t *reg)
{
	unsigned char modrm = MOD_REG;
	assert(reg_gp_map[reg->index] < 4);
	modrm |= ENC_RM(reg_gp_map[reg->index] + (high_part == REG_HIGH ? 4 : 0));
	modrm |= MOD_REG;
	bemit8(modrm);
}

/**
 * Calculate the size of an signed immediate in bytes.
 *
 * @param offset  an offset
 */
static unsigned get_signed_imm_size(int offset)
{
	if (-128 <= offset && offset < 128) {
		return 1;
	} else if (-32768 <= offset && offset < 32768) {
		return 2;
	} else {
		return 4;
	}
}

/**
 * Emit an address mode.
 *
 * @param reg   content of the reg field: either a register index or an opcode extension
 * @param node  the node
 */
static void bemit_mod_am(unsigned reg, const ir_node *node)
{
	ir_entity *ent       = get_ia32_am_sc(node);
	int        offs      = get_ia32_am_offs_int(node);
	ir_node   *base      = get_irn_n(node, n_ia32_base);
	int        has_base  = !is_ia32_NoReg_GP(base);
	ir_node   *idx       = get_irn_n(node, n_ia32_index);
	int        has_index = !is_ia32_NoReg_GP(idx);
	unsigned   modrm     = 0;
	unsigned   sib       = 0;
	unsigned   emitoffs  = 0;
	bool       emitsib   = false;
	unsigned   base_enc;

	/* set the mod part depending on displacement */
	if (ent != NULL) {
		modrm |= MOD_IND_WORD_OFS;
		emitoffs = 32;
	} else if (offs == 0) {
		modrm |= MOD_IND;
		emitoffs = 0;
	} else if (-128 <= offs && offs < 128) {
		modrm |= MOD_IND_BYTE_OFS;
		emitoffs = 8;
	} else {
		modrm |= MOD_IND_WORD_OFS;
		emitoffs = 32;
	}

	if (has_base) {
		const arch_register_t *base_reg = arch_get_irn_register(base);
		base_enc = reg_gp_map[base_reg->index];
	} else {
		/* Use the EBP encoding + MOD_IND if NO base register. There is
		 * always a 32bit offset present in this case. */
		modrm    = MOD_IND;
		base_enc = 0x05;
		emitoffs = 32;
	}

	/* Determine if we need a SIB byte. */
	if (has_index) {
		const arch_register_t *reg_index = arch_get_irn_register(idx);
		int                    scale     = get_ia32_am_scale(node);
		assert(scale < 4);
		/* R/M set to ESP means SIB in 32bit mode. */
		modrm   |= ENC_RM(0x04);
		sib      = ENC_SIB(scale, reg_gp_map[reg_index->index], base_enc);
		emitsib = true;
	} else if (base_enc == 0x04) {
		/* for the above reason we are forced to emit a SIB when base is ESP.
		 * Only the base is used, index must be ESP too, which means no index.
		 */
		modrm   |= ENC_RM(0x04);
		sib      = ENC_SIB(0, 0x04, 0x04);
		emitsib  = true;
	} else {
		modrm |= ENC_RM(base_enc);
	}

	/* We are forced to emit an 8bit offset as EBP base without offset is a
	 * special case for SIB without base register. */
	if (base_enc == 0x05 && emitoffs == 0) {
		modrm    |= MOD_IND_BYTE_OFS;
		emitoffs  = 8;
	}

	modrm |= ENC_REG(reg);

	bemit8(modrm);
	if (emitsib)
		bemit8(sib);

	/* emit displacement */
	if (emitoffs == 8) {
		bemit8((unsigned) offs);
	} else if (emitoffs == 32) {
		bemit_entity(ent, is_ia32_am_sc_sign(node), offs, false);
	}
}

/**
 * Emit a binop with a immediate operand.
 *
 * @param node        the node to emit
 * @param opcode_eax  the opcode for the op eax, imm variant
 * @param opcode      the opcode for the reg, imm variant
 * @param ruval       the opcode extension for opcode
 */
static void bemit_binop_with_imm(
	const ir_node *node,
	unsigned char opcode_ax,
	unsigned char opcode, unsigned char ruval)
{
	/* Use in-reg, because some instructions (cmp, test) have no out-reg. */
	const ir_node               *op   = get_irn_n(node, n_ia32_binary_right);
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(op);
	unsigned                     size;

	/* Some instructions (test) have no short form with 32bit value + 8bit
	 * immediate. */
	if (attr->symconst != NULL || opcode & SIGNEXT_IMM) {
		size = 4;
	} else {
		/* check for sign extension */
		size = get_signed_imm_size(attr->offset);
	}

	switch (size) {
	case 1:
		bemit8(opcode | SIGNEXT_IMM);
		/* cmp has this special mode */
		if (get_ia32_op_type(node) == ia32_AddrModeS) {
			bemit_mod_am(ruval, node);
		} else {
			const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_binary_left);
			bemit_modru(reg, ruval);
		}
		bemit8((unsigned char)attr->offset);
		return;
	case 2:
	case 4:
		/* check for eax variant: this variant is shorter for 32bit immediates only */
		if (get_ia32_op_type(node) == ia32_AddrModeS) {
			bemit8(opcode);
			bemit_mod_am(ruval, node);
		} else {
			const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_binary_left);
			if (reg->index == REG_GP_EAX) {
				bemit8(opcode_ax);
			} else {
				bemit8(opcode);
				bemit_modru(reg, ruval);
			}
		}
		bemit_entity(attr->symconst, attr->sc_sign, attr->offset, false);
		return;
	}
	panic("invalid imm size?!?");
}

/**
 * Emits a binop.
 */
static void bemit_binop_2(const ir_node *node, unsigned code)
{
	const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_binary_left);
	bemit8(code);
	if (get_ia32_op_type(node) == ia32_Normal) {
		const arch_register_t *op2 = arch_get_irn_register_in(node, n_ia32_binary_right);
		bemit_modrr(op2, out);
	} else {
		bemit_mod_am(reg_gp_map[out->index], node);
	}
}

/**
 * Emit a binop.
 */
static void bemit_binop(const ir_node *node, const unsigned char opcodes[4])
{
	ir_node *right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		bemit_binop_with_imm(node, opcodes[1], opcodes[2], opcodes[3]);
	} else {
		bemit_binop_2(node, opcodes[0]);
	}
}

/**
 * Emit an unop.
 */
static void bemit_unop(const ir_node *node, unsigned char code, unsigned char ext, int input)
{
	bemit8(code);
	if (get_ia32_op_type(node) == ia32_Normal) {
		const arch_register_t *in = arch_get_irn_register_in(node, input);
		bemit_modru(in, ext);
	} else {
		bemit_mod_am(ext, node);
	}
}

static void bemit_unop_reg(const ir_node *node, unsigned char code, int input)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);
	bemit_unop(node, code, reg_gp_map[out->index], input);
}

static void bemit_unop_mem(const ir_node *node, unsigned char code, unsigned char ext)
{
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node));
	if (size == 16)
		bemit8(0x66);
	bemit8(size == 8 ? code : code + 1);
	bemit_mod_am(ext, node);
}

static void bemit_0f_unop_reg(ir_node const *const node, unsigned char const code, int const input)
{
	bemit8(0x0F);
	bemit_unop_reg(node, code, input);
}

static void bemit_immediate(const ir_node *node, bool relative)
{
	const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(node);
	bemit_entity(attr->symconst, attr->sc_sign, attr->offset, relative);
}

static void bemit_copy(const ir_node *copy)
{
	const arch_register_t *in  = arch_get_irn_register_in(copy, 0);
	const arch_register_t *out = arch_get_irn_register_out(copy, 0);

	if (in == out)
		return;
	/* copies of vf nodes aren't real... */
	if (arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_vfp])
		return;

	assert(arch_register_get_class(in) == &ia32_reg_classes[CLASS_ia32_gp]);
	bemit8(0x8B);
	bemit_modrr(in, out);
}

static void bemit_perm(const ir_node *node)
{
	const arch_register_t       *in0  = arch_get_irn_register(get_irn_n(node, 0));
	const arch_register_t       *in1  = arch_get_irn_register(get_irn_n(node, 1));
	const arch_register_class_t *cls0 = arch_register_get_class(in0);

	assert(cls0 == arch_register_get_class(in1) && "Register class mismatch at Perm");

	if (cls0 == &ia32_reg_classes[CLASS_ia32_gp]) {
		if (in0->index == REG_GP_EAX) {
			bemit8(0x90 + reg_gp_map[in1->index]);
		} else if (in1->index == REG_GP_EAX) {
			bemit8(0x90 + reg_gp_map[in0->index]);
		} else {
			bemit8(0x87);
			bemit_modrr(in0, in1);
		}
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_xmm]) {
		panic("unimplemented"); // TODO implement
		//ia32_emitf(NULL, "xorpd %R, %R", in1, in0);
		//ia32_emitf(NULL, "xorpd %R, %R", in0, in1);
		//ia32_emitf(node, "xorpd %R, %R", in1, in0);
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_vfp]) {
		/* is a NOP */
	} else if (cls0 == &ia32_reg_classes[CLASS_ia32_st]) {
		/* is a NOP */
	} else {
		panic("unexpected register class in be_Perm (%+F)", node);
	}
}

static void bemit_xor0(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);
	bemit8(0x31);
	bemit_modrr(out, out);
}

static void bemit_mov_const(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);
	bemit8(0xB8 + reg_gp_map[out->index]);
	bemit_immediate(node, false);
}

/**
 * Creates a function for a Binop with 3 possible encodings.
 */
#define BINOP(op, op0, op1, op2, op2_ext)                                 \
static void bemit_ ## op(const ir_node *node) {                           \
	static const unsigned char op ## _codes[] = {op0, op1, op2, op2_ext}; \
	bemit_binop(node, op ## _codes);                                      \
}

/*    insn  def  eax,imm   imm */
BINOP(add,  0x03, 0x05, 0x81, 0)
BINOP(or,   0x0B, 0x0D, 0x81, 1)
BINOP(adc,  0x13, 0x15, 0x81, 2)
BINOP(sbb,  0x1B, 0x1D, 0x81, 3)
BINOP(and,  0x23, 0x25, 0x81, 4)
BINOP(sub,  0x2B, 0x2D, 0x81, 5)
BINOP(xor,  0x33, 0x35, 0x81, 6)
BINOP(test, 0x85, 0xA9, 0xF7, 0)

#define BINOPMEM(op, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	ir_node *val; \
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node)); \
	if (size == 16) \
		bemit8(0x66); \
	val = get_irn_n(node, n_ia32_unary_op); \
	if (is_ia32_Immediate(val)) { \
		const ia32_immediate_attr_t *attr   = get_ia32_immediate_attr_const(val); \
		int                          offset = attr->offset; \
		if (attr->symconst == NULL && get_signed_imm_size(offset) == 1) { \
			bemit8(0x83); \
			bemit_mod_am(ext, node); \
			bemit8(offset); \
		} else { \
			bemit8(0x81); \
			bemit_mod_am(ext, node); \
			if (size == 16) { \
				bemit16(offset); \
			} else { \
				bemit_entity(attr->symconst, attr->sc_sign, offset, false); \
			} \
		} \
	} else { \
		bemit8(ext << 3 | 1); \
		bemit_mod_am(reg_gp_map[arch_get_irn_register(val)->index], node); \
	} \
} \
 \
static void bemit_##op##8bit(const ir_node *node) \
{ \
	ir_node *val = get_irn_n(node, n_ia32_unary_op); \
	if (is_ia32_Immediate(val)) { \
		bemit8(0x80); \
		bemit_mod_am(ext, node); \
		bemit8(get_ia32_immediate_attr_const(val)->offset); \
	} else { \
		bemit8(ext << 3); \
		bemit_mod_am(reg_gp_map[arch_get_irn_register(val)->index], node); \
	} \
}

BINOPMEM(addmem,  0)
BINOPMEM(ormem,   1)
BINOPMEM(andmem,  4)
BINOPMEM(submem,  5)
BINOPMEM(xormem,  6)


/**
 * Creates a function for an Unop with code /ext encoding.
 */
#define UNOP(op, code, ext, input)              \
static void bemit_ ## op(const ir_node *node) { \
	bemit_unop(node, code, ext, input);         \
}

UNOP(not,     0xF7, 2, n_ia32_Not_val)
UNOP(neg,     0xF7, 3, n_ia32_Neg_val)
UNOP(mul,     0xF7, 4, n_ia32_Mul_right)
UNOP(imul1op, 0xF7, 5, n_ia32_IMul1OP_right)
UNOP(div,     0xF7, 6, n_ia32_Div_divisor)
UNOP(idiv,    0xF7, 7, n_ia32_IDiv_divisor)

/* TODO: am support for IJmp */
UNOP(ijmp,    0xFF, 4, n_ia32_IJmp_target)

#define SHIFT(op, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	const arch_register_t *out   = arch_get_irn_register_out(node, 0); \
	ir_node               *count = get_irn_n(node, 1); \
	if (is_ia32_Immediate(count)) { \
		int offset = get_ia32_immediate_attr_const(count)->offset; \
		if (offset == 1) { \
			bemit8(0xD1); \
			bemit_modru(out, ext); \
		} else { \
			bemit8(0xC1); \
			bemit_modru(out, ext); \
			bemit8(offset); \
		} \
	} else { \
		bemit8(0xD3); \
		bemit_modru(out, ext); \
	} \
} \
 \
static void bemit_##op##mem(const ir_node *node) \
{ \
	ir_node *count; \
	unsigned size = get_mode_size_bits(get_ia32_ls_mode(node)); \
	if (size == 16) \
		bemit8(0x66); \
	count = get_irn_n(node, 1); \
	if (is_ia32_Immediate(count)) { \
		int offset = get_ia32_immediate_attr_const(count)->offset; \
		if (offset == 1) { \
			bemit8(size == 8 ? 0xD0 : 0xD1); \
			bemit_mod_am(ext, node); \
		} else { \
			bemit8(size == 8 ? 0xC0 : 0xC1); \
			bemit_mod_am(ext, node); \
			bemit8(offset); \
		} \
	} else { \
		bemit8(size == 8 ? 0xD2 : 0xD3); \
		bemit_mod_am(ext, node); \
	} \
}

SHIFT(rol, 0)
SHIFT(ror, 1)
SHIFT(shl, 4)
SHIFT(shr, 5)
SHIFT(sar, 7)

static void bemit_shld(const ir_node *node)
{
	const arch_register_t *in  = arch_get_irn_register_in(node, n_ia32_ShlD_val_low);
	const arch_register_t *out = arch_get_irn_register_out(node, pn_ia32_ShlD_res);
	ir_node *count = get_irn_n(node, n_ia32_ShlD_count);
	bemit8(0x0F);
	if (is_ia32_Immediate(count)) {
		bemit8(0xA4);
		bemit_modrr(out, in);
		bemit8(get_ia32_immediate_attr_const(count)->offset);
	} else {
		bemit8(0xA5);
		bemit_modrr(out, in);
	}
}

static void bemit_shrd(const ir_node *node)
{
	const arch_register_t *in  = arch_get_irn_register_in(node, n_ia32_ShrD_val_low);
	const arch_register_t *out = arch_get_irn_register_out(node, pn_ia32_ShrD_res);
	ir_node *count = get_irn_n(node, n_ia32_ShrD_count);
	bemit8(0x0F);
	if (is_ia32_Immediate(count)) {
		bemit8(0xAC);
		bemit_modrr(out, in);
		bemit8(get_ia32_immediate_attr_const(count)->offset);
	} else {
		bemit8(0xAD);
		bemit_modrr(out, in);
	}
}

static void bemit_sbb0(ir_node const *const node)
{
	arch_register_t const *const out = arch_get_irn_register_out(node, pn_ia32_Sbb0_res);
	unsigned char          const reg = reg_gp_map[out->index];
	bemit8(0x1B);
	bemit8(MOD_REG | ENC_REG(reg) | ENC_RM(reg));
}

/**
 * binary emitter for setcc.
 */
static void bemit_setcc(const ir_node *node)
{
	const arch_register_t *dreg = arch_get_irn_register_out(node, pn_ia32_Setcc_res);

	ia32_condition_code_t cc = get_ia32_condcode(node);
	cc = determine_final_cc(node, n_ia32_Setcc_eflags, cc);
	if (cc & ia32_cc_float_parity_cases) {
		if (cc & ia32_cc_negated) {
			/* set%PNC <dreg */
			bemit8(0x0F);
			bemit8(0x90 | pnc2cc(cc));
			bemit_modrm8(REG_LOW, dreg);

			/* setp >dreg */
			bemit8(0x0F);
			bemit8(0x9A);
			bemit_modrm8(REG_HIGH, dreg);

			/* orb %>dreg, %<dreg */
			bemit8(0x08);
			bemit_modrr8(REG_LOW, dreg, REG_HIGH, dreg);
		} else {
			 /* set%PNC <dreg */
			bemit8(0x0F);
			bemit8(0x90 | pnc2cc(cc));
			bemit_modrm8(REG_LOW, dreg);

			/* setnp >dreg */
			bemit8(0x0F);
			bemit8(0x9B);
			bemit_modrm8(REG_HIGH, dreg);

			/* andb %>dreg, %<dreg */
			bemit8(0x20);
			bemit_modrr8(REG_LOW, dreg, REG_HIGH, dreg);
		}
	} else {
		/* set%PNC <dreg */
		bemit8(0x0F);
		bemit8(0x90 | pnc2cc(cc));
		bemit_modrm8(REG_LOW, dreg);
	}
}

static void bemit_bsf(ir_node const *const node)
{
	bemit_0f_unop_reg(node, 0xBC, n_ia32_Bsf_operand);
}

static void bemit_bsr(ir_node const *const node)
{
	bemit_0f_unop_reg(node, 0xBD, n_ia32_Bsr_operand);
}

static void bemit_bswap(ir_node const *const node)
{
	bemit8(0x0F);
	bemit_modru(arch_get_irn_register_out(node, pn_ia32_Bswap_res), 1);
}

static void bemit_bt(ir_node const *const node)
{
	bemit8(0x0F);
	arch_register_t const *const lreg  = arch_get_irn_register_in(node, n_ia32_Bt_left);
	ir_node         const *const right = get_irn_n(node, n_ia32_Bt_right);
	if (is_ia32_Immediate(right)) {
		ia32_immediate_attr_t const *const attr   = get_ia32_immediate_attr_const(right);
		int                          const offset = attr->offset;
		assert(!attr->symconst);
		assert(get_signed_imm_size(offset) == 1);
		bemit8(0xBA);
		bemit_modru(lreg, 4);
		bemit8(offset);
	} else {
		bemit8(0xA3);
		bemit_modrr(lreg, arch_get_irn_register(right));
	}
}

static void bemit_cmovcc(const ir_node *node)
{
	const ia32_attr_t     *attr         = get_ia32_attr_const(node);
	int                    ins_permuted = attr->data.ins_permuted;
	const arch_register_t *out          = arch_get_irn_register_out(node, pn_ia32_res);
	ia32_condition_code_t  cc           = get_ia32_condcode(node);
	const arch_register_t *in_true;
	const arch_register_t *in_false;

	cc = determine_final_cc(node, n_ia32_CMovcc_eflags, cc);

	in_true  = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_true));
	in_false = arch_get_irn_register(get_irn_n(node, n_ia32_CMovcc_val_false));

	/* should be same constraint fullfilled? */
	if (out == in_false) {
		/* yes -> nothing to do */
	} else if (out == in_true) {
		assert(get_ia32_op_type(node) == ia32_Normal);
		ins_permuted = !ins_permuted;
		in_true      = in_false;
	} else {
		/* we need a mov */
		bemit8(0x8B); // mov %in_false, %out
		bemit_modrr(in_false, out);
	}

	if (ins_permuted)
		cc = ia32_negate_condition_code(cc);

	if (cc & ia32_cc_float_parity_cases)
		panic("cmov can't handle parity float cases");

	bemit8(0x0F);
	bemit8(0x40 | pnc2cc(cc));
	if (get_ia32_op_type(node) == ia32_Normal) {
		bemit_modrr(in_true, out);
	} else {
		bemit_mod_am(reg_gp_map[out->index], node);
	}
}

static void bemit_cmp(const ir_node *node)
{
	unsigned  ls_size = get_mode_size_bits(get_ia32_ls_mode(node));
	ir_node  *right;

	if (ls_size == 16)
		bemit8(0x66);

	right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		/* Use in-reg, because some instructions (cmp, test) have no out-reg. */
		const ir_node               *op   = get_irn_n(node, n_ia32_binary_right);
		const ia32_immediate_attr_t *attr = get_ia32_immediate_attr_const(op);
		unsigned                     size;

		if (attr->symconst != NULL) {
			size = 4;
		} else {
			/* check for sign extension */
			size = get_signed_imm_size(attr->offset);
		}

		switch (size) {
			case 1:
				bemit8(0x81 | SIGNEXT_IMM);
				/* cmp has this special mode */
				if (get_ia32_op_type(node) == ia32_AddrModeS) {
					bemit_mod_am(7, node);
				} else {
					const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_binary_left);
					bemit_modru(reg, 7);
				}
				bemit8((unsigned char)attr->offset);
				return;
			case 2:
			case 4:
				/* check for eax variant: this variant is shorter for 32bit immediates only */
				if (get_ia32_op_type(node) == ia32_AddrModeS) {
					bemit8(0x81);
					bemit_mod_am(7, node);
				} else {
					const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_binary_left);
					if (reg->index == REG_GP_EAX) {
						bemit8(0x3D);
					} else {
						bemit8(0x81);
						bemit_modru(reg, 7);
					}
				}
				if (ls_size == 16) {
					bemit16(attr->offset);
				} else {
					bemit_entity(attr->symconst, attr->sc_sign, attr->offset, false);
				}
				return;
		}
		panic("invalid imm size?!?");
	} else {
		const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_binary_left);
		bemit8(0x3B);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *op2 = arch_get_irn_register_in(node, n_ia32_binary_right);
			bemit_modrr(op2, out);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_cmp8bit(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_binary_right);
	if (is_ia32_Immediate(right)) {
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_Cmp_left);
			if (out->index == REG_GP_EAX) {
				bemit8(0x3C);
			} else {
				bemit8(0x80);
				bemit_modru(out, 7);
			}
		} else {
			bemit8(0x80);
			bemit_mod_am(7, node);
		}
		bemit8(get_ia32_immediate_attr_const(right)->offset);
	} else {
		const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_Cmp_left);
		bemit8(0x3A);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *in = arch_get_irn_register_in(node, n_ia32_Cmp_right);
			bemit_modrr(out, in);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_test8bit(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_Test8Bit_right);
	if (is_ia32_Immediate(right)) {
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_Test8Bit_left);
			if (out->index == REG_GP_EAX) {
				bemit8(0xA8);
			} else {
				bemit8(0xF6);
				bemit_modru(out, 0);
			}
		} else {
			bemit8(0xF6);
			bemit_mod_am(0, node);
		}
		bemit8(get_ia32_immediate_attr_const(right)->offset);
	} else {
		const arch_register_t *out = arch_get_irn_register_in(node, n_ia32_Test8Bit_left);
		bemit8(0x84);
		if (get_ia32_op_type(node) == ia32_Normal) {
			const arch_register_t *in = arch_get_irn_register_in(node, n_ia32_Test8Bit_right);
			bemit_modrr(out, in);
		} else {
			bemit_mod_am(reg_gp_map[out->index], node);
		}
	}
}

static void bemit_imul(const ir_node *node)
{
	ir_node *right = get_irn_n(node, n_ia32_IMul_right);
	/* Do we need the immediate form? */
	if (is_ia32_Immediate(right)) {
		int imm = get_ia32_immediate_attr_const(right)->offset;
		if (get_signed_imm_size(imm) == 1) {
			bemit_unop_reg(node, 0x6B, n_ia32_IMul_left);
			bemit8(imm);
		} else {
			bemit_unop_reg(node, 0x69, n_ia32_IMul_left);
			bemit32(imm);
		}
	} else {
		bemit_0f_unop_reg(node, 0xAF, n_ia32_IMul_right);
	}
}

static void bemit_dec(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, pn_ia32_Dec_res);
	bemit8(0x48 + reg_gp_map[out->index]);
}

static void bemit_inc(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, pn_ia32_Inc_res);
	bemit8(0x40 + reg_gp_map[out->index]);
}

#define UNOPMEM(op, code, ext) \
static void bemit_##op(const ir_node *node) \
{ \
	bemit_unop_mem(node, code, ext); \
}

UNOPMEM(notmem, 0xF6, 2)
UNOPMEM(negmem, 0xF6, 3)
UNOPMEM(incmem, 0xFE, 0)
UNOPMEM(decmem, 0xFE, 1)

static void bemit_ldtls(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);

	bemit8(0x65); // gs:
	if (out->index == REG_GP_EAX) {
		bemit8(0xA1); // movl 0, %eax
	} else {
		bemit8(0x8B); // movl 0, %reg
		bemit8(MOD_IND | ENC_REG(reg_gp_map[out->index]) | ENC_RM(0x05));
	}
	bemit32(0);
}

/**
 * Emit a Lea.
 */
static void bemit_lea(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);
	bemit8(0x8D);
	bemit_mod_am(reg_gp_map[out->index], node);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_mov(const arch_register_t *src, const arch_register_t *dst)
{
	bemit8(0x8B); // movl %src, %dst
	bemit_modrr(src, dst);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_neg(const arch_register_t *reg)
{
	bemit8(0xF7); // negl %reg
	bemit_modru(reg, 3);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_sbb0(const arch_register_t *reg)
{
	bemit8(0x83); // sbbl $0, %reg
	bemit_modru(reg, 3);
	bemit8(0);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_sbb(const arch_register_t *src, const arch_register_t *dst)
{
	bemit8(0x1B); // sbbl %src, %dst
	bemit_modrr(src, dst);
}

/* helper function for bemit_minus64bit */
static void bemit_helper_xchg(const arch_register_t *src, const arch_register_t *dst)
{
	if (src->index == REG_GP_EAX) {
		bemit8(0x90 + reg_gp_map[dst->index]); // xchgl %eax, %dst
	} else if (dst->index == REG_GP_EAX) {
		bemit8(0x90 + reg_gp_map[src->index]); // xchgl %src, %eax
	} else {
		bemit8(0x87); // xchgl %src, %dst
		bemit_modrr(src, dst);
	}
}

/* helper function for bemit_minus64bit */
static void bemit_helper_zero(const arch_register_t *reg)
{
	bemit8(0x33); // xorl %reg, %reg
	bemit_modrr(reg, reg);
}

static void bemit_minus64bit(const ir_node *node)
{
	const arch_register_t *in_lo  = arch_get_irn_register_in(node, 0);
	const arch_register_t *in_hi  = arch_get_irn_register_in(node, 1);
	const arch_register_t *out_lo = arch_get_irn_register_out(node, 0);
	const arch_register_t *out_hi = arch_get_irn_register_out(node, 1);

	if (out_lo == in_lo) {
		if (out_hi != in_hi) {
			/* a -> a, b -> d */
			goto zero_neg;
		} else {
			/* a -> a, b -> b */
			goto normal_neg;
		}
	} else if (out_lo == in_hi) {
		if (out_hi == in_lo) {
			/* a -> b, b -> a */
			bemit_helper_xchg(in_lo, in_hi);
			goto normal_neg;
		} else {
			/* a -> b, b -> d */
			bemit_helper_mov(in_hi, out_hi);
			bemit_helper_mov(in_lo, out_lo);
			goto normal_neg;
		}
	} else {
		if (out_hi == in_lo) {
			/* a -> c, b -> a */
			bemit_helper_mov(in_lo, out_lo);
			goto zero_neg;
		} else if (out_hi == in_hi) {
			/* a -> c, b -> b */
			bemit_helper_mov(in_lo, out_lo);
			goto normal_neg;
		} else {
			/* a -> c, b -> d */
			bemit_helper_mov(in_lo, out_lo);
			goto zero_neg;
		}
	}

normal_neg:
	bemit_helper_neg( out_hi);
	bemit_helper_neg( out_lo);
	bemit_helper_sbb0(out_hi);
	return;

zero_neg:
	bemit_helper_zero(out_hi);
	bemit_helper_neg( out_lo);
	bemit_helper_sbb( in_hi, out_hi);
}

/**
 * Emit a single opcode.
 */
#define EMIT_SINGLEOP(op, code)                 \
static void bemit_ ## op(const ir_node *node) { \
	(void) node;                                \
	bemit8(code);                               \
}

//EMIT_SINGLEOP(daa,  0x27)
//EMIT_SINGLEOP(das,  0x2F)
//EMIT_SINGLEOP(aaa,  0x37)
//EMIT_SINGLEOP(aas,  0x3F)
//EMIT_SINGLEOP(nop,  0x90)
EMIT_SINGLEOP(cwtl,  0x98)
EMIT_SINGLEOP(cltd,  0x99)
//EMIT_SINGLEOP(fwait, 0x9B)
EMIT_SINGLEOP(sahf,  0x9E)
//EMIT_SINGLEOP(popf, 0x9D)
EMIT_SINGLEOP(leave, 0xC9)
EMIT_SINGLEOP(int3,  0xCC)
//EMIT_SINGLEOP(iret, 0xCF)
//EMIT_SINGLEOP(xlat, 0xD7)
//EMIT_SINGLEOP(lock, 0xF0)
EMIT_SINGLEOP(rep,   0xF3)
//EMIT_SINGLEOP(halt, 0xF4)
EMIT_SINGLEOP(cmc,   0xF5)
EMIT_SINGLEOP(stc,   0xF9)
//EMIT_SINGLEOP(cli,  0xFA)
//EMIT_SINGLEOP(sti,  0xFB)
//EMIT_SINGLEOP(std,  0xFD)

/**
 * Emits a MOV out, [MEM].
 */
static void bemit_load(const ir_node *node)
{
	const arch_register_t *out = arch_get_irn_register_out(node, 0);

	if (out->index == REG_GP_EAX) {
		ir_node   *base      = get_irn_n(node, n_ia32_base);
		int        has_base  = !is_ia32_NoReg_GP(base);
		ir_node   *idx       = get_irn_n(node, n_ia32_index);
		int        has_index = !is_ia32_NoReg_GP(idx);
		if (!has_base && !has_index) {
			ir_entity *ent  = get_ia32_am_sc(node);
			int        offs = get_ia32_am_offs_int(node);
			/* load from constant address to EAX can be encoded
			   as 0xA1 [offset] */
			bemit8(0xA1);
			bemit_entity(ent, 0, offs, false);
			return;
		}
	}
	bemit8(0x8B);
	bemit_mod_am(reg_gp_map[out->index], node);
}

/**
 * Emits a MOV [mem], in.
 */
static void bemit_store(const ir_node *node)
{
	const ir_node *value = get_irn_n(node, n_ia32_Store_val);
	unsigned       size  = get_mode_size_bits(get_ia32_ls_mode(node));

	if (is_ia32_Immediate(value)) {
		if (size == 8) {
			bemit8(0xC6);
			bemit_mod_am(0, node);
			bemit8(get_ia32_immediate_attr_const(value)->offset);
		} else if (size == 16) {
			bemit8(0x66);
			bemit8(0xC7);
			bemit_mod_am(0, node);
			bemit16(get_ia32_immediate_attr_const(value)->offset);
		} else {
			bemit8(0xC7);
			bemit_mod_am(0, node);
			bemit_immediate(value, false);
		}
	} else {
		const arch_register_t *in = arch_get_irn_register_in(node, n_ia32_Store_val);

		if (in->index == REG_GP_EAX) {
			ir_node   *base      = get_irn_n(node, n_ia32_base);
			int        has_base  = !is_ia32_NoReg_GP(base);
			ir_node   *idx       = get_irn_n(node, n_ia32_index);
			int        has_index = !is_ia32_NoReg_GP(idx);
			if (!has_base && !has_index) {
				ir_entity *ent  = get_ia32_am_sc(node);
				int        offs = get_ia32_am_offs_int(node);
				/* store to constant address from EAX can be encoded as
				 * 0xA2/0xA3 [offset]*/
				if (size == 8) {
					bemit8(0xA2);
				} else {
					if (size == 16)
						bemit8(0x66);
					bemit8(0xA3);
				}
				bemit_entity(ent, 0, offs, false);
				return;
			}
		}

		if (size == 8) {
			bemit8(0x88);
		} else {
			if (size == 16)
				bemit8(0x66);
			bemit8(0x89);
		}
		bemit_mod_am(reg_gp_map[in->index], node);
	}
}

static void bemit_conv_i2i(const ir_node *node)
{
	/*        8 16 bit source
	 * movzx B6 B7
	 * movsx BE BF */
	ir_mode *const smaller_mode = get_ia32_ls_mode(node);
	unsigned       opcode       = 0xB6;
	if (mode_is_signed(smaller_mode))           opcode |= 0x08;
	if (get_mode_size_bits(smaller_mode) == 16) opcode |= 0x01;
	bemit_0f_unop_reg(node, opcode, n_ia32_Conv_I2I_val);
}

static void bemit_popcnt(ir_node const *const node)
{
	bemit8(0xF3);
	bemit_0f_unop_reg(node, 0xB8, n_ia32_Popcnt_operand);
}

/**
 * Emit a Push.
 */
static void bemit_push(const ir_node *node)
{
	const ir_node *value = get_irn_n(node, n_ia32_Push_val);

	if (is_ia32_Immediate(value)) {
		const ia32_immediate_attr_t *attr
			= get_ia32_immediate_attr_const(value);
		unsigned size = get_signed_imm_size(attr->offset);
		if (attr->symconst)
			size = 4;
		switch (size) {
		case 1:
			bemit8(0x6A);
			bemit8((unsigned char)attr->offset);
			break;
		case 2:
		case 4:
			bemit8(0x68);
			bemit_immediate(value, false);
			break;
		}
	} else if (is_ia32_NoReg_GP(value)) {
		bemit8(0xFF);
		bemit_mod_am(6, node);
	} else {
		const arch_register_t *reg = arch_get_irn_register_in(node, n_ia32_Push_val);
		bemit8(0x50 + reg_gp_map[reg->index]);
	}
}

/**
 * Emit a Pop.
 */
static void bemit_pop(const ir_node *node)
{
	const arch_register_t *reg = arch_get_irn_register_out(node, pn_ia32_Pop_res);
	bemit8(0x58 + reg_gp_map[reg->index]);
}

static void bemit_popmem(const ir_node *node)
{
	bemit8(0x8F);
	bemit_mod_am(0, node);
}

static void bemit_call(const ir_node *node)
{
	ir_node *proc = get_irn_n(node, n_ia32_Call_addr);

	if (is_ia32_Immediate(proc)) {
		bemit8(0xE8);
		bemit_immediate(proc, true);
	} else {
		bemit_unop(node, 0xFF, 2, n_ia32_Call_addr);
	}
}

static void bemit_jmp(const ir_node *dest_block)
{
	bemit8(0xE9);
	bemit_jmp_destination(dest_block);
}

static void bemit_jump(const ir_node *node)
{
	if (can_be_fallthrough(node))
		return;

	bemit_jmp(get_cfop_target_block(node));
}

static void bemit_jcc(ia32_condition_code_t pnc, const ir_node *dest_block)
{
	unsigned char cc = pnc2cc(pnc);
	bemit8(0x0F);
	bemit8(0x80 + cc);
	bemit_jmp_destination(dest_block);
}

static void bemit_jp(bool odd, const ir_node *dest_block)
{
	bemit8(0x0F);
	bemit8(0x8A + odd);
	bemit_jmp_destination(dest_block);
}

static void bemit_ia32_jcc(const ir_node *node)
{
	ia32_condition_code_t cc = get_ia32_condcode(node);
	const ir_node        *proj_true;
	const ir_node        *proj_false;
	const ir_node        *dest_true;
	const ir_node        *dest_false;

	cc = determine_final_cc(node, 0, cc);

	/* get both Projs */
	proj_true = get_proj(node, pn_ia32_Jcc_true);
	assert(proj_true && "Jcc without true Proj");

	proj_false = get_proj(node, pn_ia32_Jcc_false);
	assert(proj_false && "Jcc without false Proj");

	if (can_be_fallthrough(proj_true)) {
		/* exchange both proj's so the second one can be omitted */
		const ir_node *t = proj_true;

		proj_true  = proj_false;
		proj_false = t;
		cc         = ia32_negate_condition_code(cc);
	}

	dest_true  = get_cfop_target_block(proj_true);
	dest_false = get_cfop_target_block(proj_false);

	if (cc & ia32_cc_float_parity_cases) {
		/* Some floating point comparisons require a test of the parity flag,
		 * which indicates that the result is unordered */
		if (cc & ia32_cc_negated) {
			bemit_jp(false, dest_true);
		} else {
			/* we need a local label if the false proj is a fallthrough
			 * as the falseblock might have no label emitted then */
			if (can_be_fallthrough(proj_false)) {
				bemit8(0x7A);
				bemit8(0x06);  // jp + 6
			} else {
				bemit_jp(false, dest_false);
			}
		}
	}
	bemit_jcc(cc, dest_true);

	/* the second Proj might be a fallthrough */
	if (can_be_fallthrough(proj_false)) {
		/* it's a fallthrough */
	} else {
		bemit_jmp(dest_false);
	}
}

static void bemit_switchjmp(const ir_node *node)
{
	ir_entity             *jump_table = get_ia32_am_sc(node);
	const ir_switch_table *table      = get_ia32_switch_table(node);

	bemit8(0xFF); // jmp *tbl.label(,%in,4)
	bemit_mod_am(0x05, node);

	be_emit_jump_table(node, table, jump_table, get_cfop_target_block);
}

/**
 * Emits a return.
 */
static void bemit_return(const ir_node *node)
{
	unsigned pop = be_Return_get_pop(node);
	if (pop > 0 || be_Return_get_emit_pop(node)) {
		bemit8(0xC2);
		assert(pop <= 0xffff);
		bemit16(pop);
	} else {
		bemit8(0xC3);
	}
}

static void bemit_subsp(const ir_node *node)
{
	const arch_register_t *out;
	/* sub %in, %esp */
	bemit_sub(node);
	/* mov %esp, %out */
	bemit8(0x8B);
	out = arch_get_irn_register_out(node, 1);
	bemit8(MOD_REG | ENC_REG(reg_gp_map[out->index]) | ENC_RM(0x04));
}

static void bemit_incsp(const ir_node *node)
{
	int                    offs;
	const arch_register_t *reg;
	unsigned               size;
	unsigned               ext;

	offs = be_get_IncSP_offset(node);
	if (offs == 0)
		return;

	if (offs > 0) {
		ext = 5; /* sub */
	} else {
		ext = 0; /* add */
		offs = -offs;
	}

	size = get_signed_imm_size(offs);
	bemit8(size == 1 ? 0x83 : 0x81);

	reg  = arch_get_irn_register_out(node, 0);
	bemit_modru(reg, ext);

	if (size == 1) {
		bemit8(offs);
	} else {
		bemit32(offs);
	}
}

static void bemit_copybi(const ir_node *node)
{
	unsigned size = get_ia32_copyb_size(node);
	if (size & 1)
		bemit8(0xA4); // movsb
	if (size & 2) {
		bemit8(0x66);
		bemit8(0xA5); // movsw
	}
	size >>= 2;
	while (size--) {
		bemit8(0xA5); // movsl
	}
}

static void bemit_fbinop(ir_node const *const node, unsigned const op_fwd, unsigned const op_rev)
{
	ia32_x87_attr_t const *const attr = get_ia32_x87_attr_const(node);
	arch_register_t const *const st0  = &ia32_registers[REG_ST0];
	if (get_ia32_op_type(node) == ia32_Normal) {
		arch_register_t const *const out = attr->x87[2];
		assert(out == attr->x87[0] || out == attr->x87[1]);
		assert(!attr->attr.data.ins_permuted);

		unsigned char op0 = 0xD8;
		if (out != st0) op0 |= 0x04;
		if (attr->pop)  op0 |= 0x02;
		bemit8(op0);

		unsigned               op  = op_rev;
		arch_register_t const *reg = attr->x87[0];
		if (reg == st0) {
			op  = op_fwd;
			reg = attr->x87[1];
		}
		bemit8(MOD_REG | ENC_REG(op) | ENC_RM(reg->index));
	} else {
		assert(attr->x87[2] == st0);
		assert(!attr->pop);

		unsigned const size = get_mode_size_bits(get_ia32_ls_mode(node));
		bemit8(size == 32 ? 0xD8 : 0xDC);
		bemit_mod_am(attr->attr.data.ins_permuted ? op_rev : op_fwd, node);
	}
}

static void bemit_fop_reg(ir_node const *const node, unsigned char const op0, unsigned char const op1)
{
	bemit8(op0);
	bemit8(op1 + get_ia32_x87_attr_const(node)->x87[0]->index);
}

static void bemit_fabs(const ir_node *node)
{
	(void)node;

	bemit8(0xD9);
	bemit8(0xE1);
}

static void bemit_fadd(const ir_node *node)
{
	bemit_fbinop(node, 0, 0);
}

static void bemit_fchs(const ir_node *node)
{
	(void)node;

	bemit8(0xD9);
	bemit8(0xE0);
}

static void bemit_fdiv(const ir_node *node)
{
	bemit_fbinop(node, 6, 7);
}

static void bemit_ffreep(ir_node const *const node)
{
	bemit_fop_reg(node, 0xDF, 0xC0);
}

static void bemit_fild(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 16:
			bemit8(0xDF); // filds
			bemit_mod_am(0, node);
			return;

		case 32:
			bemit8(0xDB); // fildl
			bemit_mod_am(0, node);
			return;

		case 64:
			bemit8(0xDF); // fildll
			bemit_mod_am(5, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fist(const ir_node *node)
{
	unsigned       op;
	unsigned const size = get_mode_size_bits(get_ia32_ls_mode(node));
	switch (size) {
	case 16: bemit8(0xDF); op = 2; break; // fist[p]s
	case 32: bemit8(0xDB); op = 2; break; // fist[p]l
	case 64: bemit8(0xDF); op = 6; break; // fistpll
	default: panic("invalid mode size");
	}
	if (get_ia32_x87_attr_const(node)->pop)
		++op;
	// There is only a pop variant for 64 bit integer store.
	assert(size < 64 || get_ia32_x87_attr_const(node)->pop);
	bemit_mod_am(op, node);
}

static void bemit_fisttp(ir_node const *const node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
	case 16: bemit8(0xDF); break; // fisttps
	case 32: bemit8(0xDB); break; // fisttpl
	case 64: bemit8(0xDD); break; // fisttpll
	default: panic("Invalid mode size");
	}
	bemit_mod_am(1, node);
}

static void bemit_fld(const ir_node *node)
{
	switch (get_mode_size_bits(get_ia32_ls_mode(node))) {
		case 32:
			bemit8(0xD9); // flds
			bemit_mod_am(0, node);
			return;

		case 64:
			bemit8(0xDD); // fldl
			bemit_mod_am(0, node);
			return;

		case 80:
		case 96:
			bemit8(0xDB); // fldt
			bemit_mod_am(5, node);
			return;

		default:
			panic("invalid mode size");
	}
}

static void bemit_fld1(const ir_node *node)
{
	(void)node;
	bemit8(0xD9);
	bemit8(0xE8); // fld1
}

static void bemit_fldcw(const ir_node *node)
{
	bemit8(0xD9); // fldcw
	bemit_mod_am(5, node);
}

static void bemit_fldz(const ir_node *node)
{
	(void)node;
	bemit8(0xD9);
	bemit8(0xEE); // fldz
}

static void bemit_fmul(const ir_node *node)
{
	bemit_fbinop(node, 1, 1);
}

static void bemit_fpop(const ir_node *node)
{
	bemit_fop_reg(node, 0xDD, 0xD8);
}

static void bemit_fpush(const ir_node *node)
{
	bemit_fop_reg(node, 0xD9, 0xC0);
}

static void bemit_fpushcopy(const ir_node *node)
{
	bemit_fop_reg(node, 0xD9, 0xC0);
}

static void bemit_fst(const ir_node *node)
{
	unsigned       op;
	unsigned const size = get_mode_size_bits(get_ia32_ls_mode(node));
	switch (size) {
	case 32: bemit8(0xD9); op = 2; break; // fst[p]s
	case 64: bemit8(0xDD); op = 2; break; // fst[p]l
	case 80:
	case 96: bemit8(0xDB); op = 6; break; // fstpt
	default: panic("invalid mode size");
	}
	if (get_ia32_x87_attr_const(node)->pop)
		++op;
	// There is only a pop variant for long double store.
	assert(size < 80 || get_ia32_x87_attr_const(node)->pop);
	bemit_mod_am(op, node);
}

static void bemit_fsub(const ir_node *node)
{
	bemit_fbinop(node, 4, 5);
}

static void bemit_fnstcw(const ir_node *node)
{
	bemit8(0xD9); // fnstcw
	bemit_mod_am(7, node);
}

static void bemit_fnstsw(void)
{
	bemit8(0xDF); // fnstsw %ax
	bemit8(0xE0);
}

static void bemit_ftstfnstsw(const ir_node *node)
{
	(void)node;

	bemit8(0xD9); // ftst
	bemit8(0xE4);
	bemit_fnstsw();
}

static void bemit_fucomi(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(attr->pop ? 0xDF : 0xDB); // fucom[p]i
	bemit8(0xE8 + attr->x87[1]->index);
}

static void bemit_fucomfnstsw(const ir_node *node)
{
	const ia32_x87_attr_t *attr = get_ia32_x87_attr_const(node);
	bemit8(0xDD); // fucom[p]
	bemit8((attr->pop ? 0xE8 : 0xE0) + attr->x87[1]->index);
	bemit_fnstsw();
}

static void bemit_fucomppfnstsw(const ir_node *node)
{
	(void)node;

	bemit8(0xDA); // fucompp
	bemit8(0xE9);
	bemit_fnstsw();
}

static void bemit_fxch(const ir_node *node)
{
	bemit_fop_reg(node, 0xD9, 0xC8);
}

/**
 * The type of a emitter function.
 */
typedef void (*emit_func) (const ir_node *);

/**
 * Set a node emitter. Make it a bit more type safe.
 */
static void register_emitter(ir_op *op, emit_func func)
{
	op->ops.generic = (op_func) func;
}

static void ia32_register_binary_emitters(void)
{
	/* first clear the generic function pointer for all ops */
	ir_clear_opcodes_generic_func();

	/* benode emitter */
	register_emitter(op_be_Copy,            bemit_copy);
	register_emitter(op_be_CopyKeep,        bemit_copy);
	register_emitter(op_be_IncSP,           bemit_incsp);
	register_emitter(op_be_Perm,            bemit_perm);
	register_emitter(op_be_Return,          bemit_return);
	register_emitter(op_ia32_Adc,           bemit_adc);
	register_emitter(op_ia32_Add,           bemit_add);
	register_emitter(op_ia32_AddMem,        bemit_addmem);
	register_emitter(op_ia32_AddMem8Bit,    bemit_addmem8bit);
	register_emitter(op_ia32_And,           bemit_and);
	register_emitter(op_ia32_AndMem,        bemit_andmem);
	register_emitter(op_ia32_AndMem8Bit,    bemit_andmem8bit);
	register_emitter(op_ia32_Asm,           emit_ia32_Asm); // TODO implement binary emitter
	register_emitter(op_ia32_Breakpoint,    bemit_int3);
	register_emitter(op_ia32_Bsf,           bemit_bsf);
	register_emitter(op_ia32_Bsr,           bemit_bsr);
	register_emitter(op_ia32_Bswap,         bemit_bswap);
	register_emitter(op_ia32_Bt,            bemit_bt);
	register_emitter(op_ia32_CMovcc,        bemit_cmovcc);
	register_emitter(op_ia32_Call,          bemit_call);
	register_emitter(op_ia32_Cltd,          bemit_cltd);
	register_emitter(op_ia32_Cmc,           bemit_cmc);
	register_emitter(op_ia32_Cmp,           bemit_cmp);
	register_emitter(op_ia32_Cmp8Bit,       bemit_cmp8bit);
	register_emitter(op_ia32_Const,         bemit_mov_const);
	register_emitter(op_ia32_Conv_I2I,      bemit_conv_i2i);
	register_emitter(op_ia32_Conv_I2I8Bit,  bemit_conv_i2i);
	register_emitter(op_ia32_CopyB_i,       bemit_copybi);
	register_emitter(op_ia32_Cwtl,          bemit_cwtl);
	register_emitter(op_ia32_Dec,           bemit_dec);
	register_emitter(op_ia32_DecMem,        bemit_decmem);
	register_emitter(op_ia32_Div,           bemit_div);
	register_emitter(op_ia32_FldCW,         bemit_fldcw);
	register_emitter(op_ia32_FnstCW,        bemit_fnstcw);
	register_emitter(op_ia32_FtstFnstsw,    bemit_ftstfnstsw);
	register_emitter(op_ia32_FucomFnstsw,   bemit_fucomfnstsw);
	register_emitter(op_ia32_Fucomi,        bemit_fucomi);
	register_emitter(op_ia32_FucomppFnstsw, bemit_fucomppfnstsw);
	register_emitter(op_ia32_IDiv,          bemit_idiv);
	register_emitter(op_ia32_IJmp,          bemit_ijmp);
	register_emitter(op_ia32_IMul,          bemit_imul);
	register_emitter(op_ia32_IMul1OP,       bemit_imul1op);
	register_emitter(op_ia32_Inc,           bemit_inc);
	register_emitter(op_ia32_IncMem,        bemit_incmem);
	register_emitter(op_ia32_Jcc,           bemit_ia32_jcc);
	register_emitter(op_ia32_Jmp,           bemit_jump);
	register_emitter(op_ia32_LdTls,         bemit_ldtls);
	register_emitter(op_ia32_Lea,           bemit_lea);
	register_emitter(op_ia32_Leave,         bemit_leave);
	register_emitter(op_ia32_Load,          bemit_load);
	register_emitter(op_ia32_Minus64Bit,    bemit_minus64bit);
	register_emitter(op_ia32_Mul,           bemit_mul);
	register_emitter(op_ia32_Neg,           bemit_neg);
	register_emitter(op_ia32_NegMem,        bemit_negmem);
	register_emitter(op_ia32_Not,           bemit_not);
	register_emitter(op_ia32_NotMem,        bemit_notmem);
	register_emitter(op_ia32_Or,            bemit_or);
	register_emitter(op_ia32_OrMem,         bemit_ormem);
	register_emitter(op_ia32_OrMem8Bit,     bemit_ormem8bit);
	register_emitter(op_ia32_Pop,           bemit_pop);
	register_emitter(op_ia32_PopEbp,        bemit_pop);
	register_emitter(op_ia32_PopMem,        bemit_popmem);
	register_emitter(op_ia32_Popcnt,        bemit_popcnt);
	register_emitter(op_ia32_Push,          bemit_push);
	register_emitter(op_ia32_RepPrefix,     bemit_rep);
	register_emitter(op_ia32_Rol,           bemit_rol);
	register_emitter(op_ia32_RolMem,        bemit_rolmem);
	register_emitter(op_ia32_Ror,           bemit_ror);
	register_emitter(op_ia32_RorMem,        bemit_rormem);
	register_emitter(op_ia32_Sahf,          bemit_sahf);
	register_emitter(op_ia32_Sar,           bemit_sar);
	register_emitter(op_ia32_SarMem,        bemit_sarmem);
	register_emitter(op_ia32_Sbb,           bemit_sbb);
	register_emitter(op_ia32_Sbb0,          bemit_sbb0);
	register_emitter(op_ia32_Setcc,         bemit_setcc);
	register_emitter(op_ia32_Shl,           bemit_shl);
	register_emitter(op_ia32_ShlD,          bemit_shld);
	register_emitter(op_ia32_ShlMem,        bemit_shlmem);
	register_emitter(op_ia32_Shr,           bemit_shr);
	register_emitter(op_ia32_ShrD,          bemit_shrd);
	register_emitter(op_ia32_ShrMem,        bemit_shrmem);
	register_emitter(op_ia32_Stc,           bemit_stc);
	register_emitter(op_ia32_Store,         bemit_store);
	register_emitter(op_ia32_Store8Bit,     bemit_store);
	register_emitter(op_ia32_Sub,           bemit_sub);
	register_emitter(op_ia32_SubMem,        bemit_submem);
	register_emitter(op_ia32_SubMem8Bit,    bemit_submem8bit);
	register_emitter(op_ia32_SubSP,         bemit_subsp);
	register_emitter(op_ia32_SwitchJmp,     bemit_switchjmp);
	register_emitter(op_ia32_Test,          bemit_test);
	register_emitter(op_ia32_Test8Bit,      bemit_test8bit);
	register_emitter(op_ia32_Xor,           bemit_xor);
	register_emitter(op_ia32_Xor0,          bemit_xor0);
	register_emitter(op_ia32_XorMem,        bemit_xormem);
	register_emitter(op_ia32_XorMem8Bit,    bemit_xormem8bit);
	register_emitter(op_ia32_fabs,          bemit_fabs);
	register_emitter(op_ia32_fadd,          bemit_fadd);
	register_emitter(op_ia32_fchs,          bemit_fchs);
	register_emitter(op_ia32_fdiv,          bemit_fdiv);
	register_emitter(op_ia32_ffreep,        bemit_ffreep);
	register_emitter(op_ia32_fild,          bemit_fild);
	register_emitter(op_ia32_fist,          bemit_fist);
	register_emitter(op_ia32_fisttp,        bemit_fisttp);
	register_emitter(op_ia32_fld,           bemit_fld);
	register_emitter(op_ia32_fld1,          bemit_fld1);
	register_emitter(op_ia32_fldz,          bemit_fldz);
	register_emitter(op_ia32_fmul,          bemit_fmul);
	register_emitter(op_ia32_fpop,          bemit_fpop);
	register_emitter(op_ia32_fpush,         bemit_fpush);
	register_emitter(op_ia32_fpushCopy,     bemit_fpushcopy);
	register_emitter(op_ia32_fst,           bemit_fst);
	register_emitter(op_ia32_fsub,          bemit_fsub);
	register_emitter(op_ia32_fxch,          bemit_fxch);

	/* ignore the following nodes */
	register_emitter(op_ia32_ProduceVal,   emit_Nothing);
	register_emitter(op_ia32_Unknown,      emit_Nothing);
	register_emitter(op_be_Keep,           emit_Nothing);
	register_emitter(op_be_Start,          emit_Nothing);
	register_emitter(op_Phi,               emit_Nothing);
	register_emitter(op_Start,             emit_Nothing);
}

static void gen_binary_block(ir_node *block)
{
	ia32_emit_block_header(block);

	/* emit the contents of the block */
	sched_foreach(block, node) {
		ia32_emit_node(node);
	}
}

void ia32_gen_binary_routine(ir_graph *irg)
{
	ir_entity        *entity    = get_irg_entity(irg);
	const arch_env_t *arch_env  = be_get_irg_arch_env(irg);
	ia32_irg_data_t  *irg_data  = ia32_get_irg_data(irg);
	ir_node         **blk_sched = irg_data->blk_sched;
	size_t            i, n;
	parameter_dbg_info_t *infos;

	isa = (ia32_isa_t*) arch_env;

	ia32_register_binary_emitters();

	infos = construct_parameter_infos(irg);
	be_gas_emit_function_prolog(entity, ia32_cg_config.function_alignment,
	                            NULL);
	xfree(infos);

	/* we use links to point to target blocks */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irg_block_walk_graph(irg, ia32_gen_labels, NULL, NULL);

	/* initialize next block links */
	n = ARR_LEN(blk_sched);
	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		ir_node *prev  = i > 0 ? blk_sched[i-1] : NULL;

		set_irn_link(block, prev);
	}

	for (i = 0; i < n; ++i) {
		ir_node *block = blk_sched[i];
		gen_binary_block(block);
	}

	be_gas_emit_function_epilog(entity);

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
}


void ia32_init_emitter(void)
{
	lc_opt_entry_t *be_grp;
	lc_opt_entry_t *ia32_grp;

	be_grp   = lc_opt_get_grp(firm_opt_get_root(), "be");
	ia32_grp = lc_opt_get_grp(be_grp, "ia32");

	lc_opt_add_table(ia32_grp, ia32_emitter_options);

	build_reg_map();

	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.emitter");
}
