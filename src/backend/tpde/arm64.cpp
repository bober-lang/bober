// The AArch64 half of compiler.hpp: symbol addresses through ADRP pairs, and the ELF object.
#ifdef __aarch64__
#include "compiler.hpp"
#include "encode_template_arm64.hpp"
#include "tpde/ELF.hpp"
#include "tpde/arm64/CompilerA64.hpp"

namespace bober {

struct ConfigA64 : tpde::a64::PlatformConfig {
	static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

struct A64 : tpde::a64::CompilerA64<Adaptor, A64, bober::Base, ConfigA64>,
	tpde_encodegen::EncodeCompiler<Adaptor, A64, bober::Base, ConfigA64> {
	using Arch = tpde::a64::CompilerA64<Adaptor, A64, bober::Base, ConfigA64>;

	explicit A64(Adaptor *a) : Arch(a) {}

	/// As x64.cpp's, zeroing the frame above where the callee-saved registers are saved, up to the caller's stack
	/// pointer: in x17, where CompilerA64 has the stack arguments, and puts it when it knows the frame's size.
	void prologue_begin(tpde::CCAssigner *cc) {
		Arch::prologue_begin(cc);
		if (!this->adaptor->m->collected) {
			return;
		}
		this->register_file.allocatable &= ~cc->get_ccinfo().callee_saved_regs;
		if (this->adaptor->calls && !this->adaptor->refs.empty()) {
			this->stack.frame_used = true;
			this->func_arg_stack_add_off = text_writer.offset();
			this->func_arg_stack_add_reg = AsmReg::R17;
			ASM(ADDxi, DA_GP(17), DA_SP, 0);
			ASM(ADDxi, DA_GP(16), DA_GP(29), this->stack.frame_size);
			tpde::Label loop = text_writer.label_create(), test = text_writer.label_create();
			this->generate_raw_jump(Jump::jmp, test);
			this->label_place(loop);
			ASM(STPx_post, DA_ZR, DA_ZR, DA_GP(16), 16);
			this->label_place(test);
			ASM(CMPx, DA_GP(16), DA_GP(17));
			this->generate_raw_jump(Jump::Jlo, loop);
		}
	}

	void finish_func(u32 func_idx) {
		u32 start = func_start_off;
		Arch::finish_func(func_idx);
		this->stackmap(func_idx, start, tpde::dwarf::a64::DW_reg_fp);
	}

	/// As CompilerA64's; a call that passed arguments on the stack is followed by an add to the stack pointer.
	void generate_call(std::variant<SymRef, ValuePart> &&target, std::span<CallArg> args, ValueRef *result,
		bool = false) {
		tpde::a64::CCAssignerAAPCS a;
		CallBuilder cb{*this, a};
		for (auto &arg : args) {
			cb.add_arg(std::move(arg));
		}
		cb.call(std::move(target));
		this->returned = text_writer.offset() - (a.get_stack_size() ? 4 : 0);
		if (result) {
			cb.add_ret(*result);
		}
	}

	void reset() {
		Arch::reset();
		EncodeCompiler::reset();
	}

	/// A symbol defined here is reached relative to the code, others through the GOT, as in x64.cpp.
	void load_address_of_var_reference(AsmReg dst, tpde::AssignmentPartRef ap) {
		auto [sym, off, local] = address(ap.variable_ref_data());
		text_writer.ensure_space(8);
		if (local) {
			reloc_text(sym, tpde::elf::R_AARCH64_ADR_PREL_PG_HI21, text_writer.offset(), off);
			ASMNC(ADRP, dst, 0, 0);
			reloc_text(sym, tpde::elf::R_AARCH64_ADD_ABS_LO12_NC, text_writer.offset(), off);
			ASMNC(ADDxi, dst, dst, 0);
			return;
		}
		reloc_text(sym, tpde::elf::R_AARCH64_ADR_GOT_PAGE, text_writer.offset());
		ASMNC(ADRP, dst, 0, 0);
		reloc_text(sym, tpde::elf::R_AARCH64_LD64_GOT_LO12_NC, text_writer.offset());
		ASMNC(LDRxu, dst, dst, 0);
		if (off && !ASMIF(ADDxi, dst, dst, off)) {
			materialize_constant(off, ConfigA64::GP_BANK, 8, permanent_scratch_reg);
			ASM(ADDx, dst, dst, permanent_scratch_reg);
		}
	}

	void condBr(u32 cond, u32 yes, u32 no) {
		ValueRef c = val_ref(cond);
		ASM(TSTwi, c.part(0).load_to_reg(), 1);
		generate_cond_branch(Jump::Jne, yes, no);
	}

	/// Starts the va_list at ap as the AAPCS64 lays it out: the stack arguments, whose address the prologue saved
	/// after the register save area it fills, the ends of that area's general and float registers, and the negative
	/// offsets from those of the next arguments in them.
	void vaStart(u32 ap) {
		ValueRef list = val_ref(ap);
		ValuePartRef part = list.part(0);
		AsmReg p = part.load_to_reg(), r = permanent_scratch_reg;
		ASM(LDRxu, r, DA_GP(29), reg_save_frame_off + 192);
		ASM(STRxu, r, p, 0);
		ASM(ADDxi, r, DA_GP(29), reg_save_frame_off + 64);
		ASM(STRxu, r, p, 8);
		ASM(ADDxi, r, DA_GP(29), reg_save_frame_off + 192);
		ASM(STRxu, r, p, 16);
		u32 gr = -8 * (8 - scalar_arg_count), vr = -16 * (8 - vec_arg_count);
		materialize_constant(u64(vr) << 32 | gr, ConfigA64::GP_BANK, 8, r);
		ASM(STRxu, r, p, 24);
	}

	/// The frame pointer, which needs the function to have a frame.
	void frameAddress(u32 i) {
		this->stack.frame_used = true;
		ValuePartRef fp{this, ConfigA64::GP_BANK};
		ASM(MOVx, fp.alloc_reg(), DA_GP(29));
		this->result_ref(i).part(0).set_value(std::move(fp));
	}
};

bool compile(Module &m, std::vector<u8> &out) {
	Adaptor a(&m);
	A64 c(&a);
	if (!c.compile()) {
		return false;
	}
	out = c.assembler.build_object_file();
	return true;
}

} // namespace bober
#endif
