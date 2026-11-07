// The x86-64 half of compiler.hpp: symbol addresses through PC-relative relocations, and the ELF object.
#ifdef __x86_64__
#include "compiler.hpp"
#include "encode_template_x64.hpp"
#include "tpde/ELF.hpp"
#include "tpde/x64/CompilerX64.hpp"

namespace bober {

struct ConfigX64 : tpde::x64::PlatformConfig {
	static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

/// System V, but returning a struct by value as LLVM does where C would return it in memory: up to four integers in
/// AX, DX, CX and R8, and four floats in XMM0 to XMM3.
struct Assigner : tpde::x64::CCAssignerSysV {
	u32 gp = 0, fp = 0;

	using CCAssignerSysV::CCAssignerSysV;
	void reset() override {
		CCAssignerSysV::reset();
		gp = fp = 0;
	}
	void assign_ret(tpde::CCAssignment &a) override {
		static constexpr tpde::x64::AsmReg gps[] = {tpde::x64::AsmReg::AX, tpde::x64::AsmReg::DX,
			tpde::x64::AsmReg::CX, tpde::x64::AsmReg::R8};
		a.reg = a.bank == ConfigX64::GP_BANK ? tpde::Reg{gps[gp++]} : tpde::Reg{tpde::x64::AsmReg::XMM0 + fp++};
	}
};

struct X64 : tpde::x64::CompilerX64<Adaptor, X64, bober::Base, ConfigX64>,
	tpde_encodegen::EncodeCompiler<Adaptor, X64, bober::Base, ConfigX64> {
	using Arch = tpde::x64::CompilerX64<Adaptor, X64, bober::Base, ConfigX64>;
	Assigner assigner;

	explicit X64(Adaptor *a) : Arch(a) {}

	tpde::CCAssigner *cur_cc_assigner() { return &assigner; }

	/// A collected module keeps no value in a callee-saved register, so that each live across a call is in the frame.
	/// A function of it holding refs and calling zeroes its frame below where those registers are saved, as a ref's
	/// slot is reported from the first block it lives in to the last, though the path taken may not have set it yet.
	void prologue_begin(tpde::CCAssigner *cc) {
		Arch::prologue_begin(cc);
		if (!this->adaptor->m->collected) {
			return;
		}
		this->register_file.allocatable &= ~cc->get_ccinfo().callee_saved_regs;
		if (this->adaptor->calls && !this->adaptor->refs.empty()) {
			// The loop starts at the frame pointer, which without a frame would still be the caller's.
			this->stack.frame_used = true;
			ASM(LEA64rm, FE_R11, FE_MEM(FE_BP, 0, FE_NOREG, -(i32)this->stack.frame_size));
			tpde::Label loop = text_writer.label_create();
			this->label_place(loop);
			ASM(SUB64ri, FE_R11, 8);
			ASM(MOV64mi, FE_MEM(FE_R11, 0, FE_NOREG, 0), 0);
			ASM(CMP64rr, FE_R11, FE_SP);
			this->generate_raw_jump(Jump::ja, loop);
		}
	}

	void finish_func(u32 func_idx) {
		u32 start = func_start_off;
		Arch::finish_func(func_idx);
		this->stackmap(func_idx, start, tpde::dwarf::x64::DW_reg_rbp);
	}

	/// As CompilerX64's, through Assigner.
	void generate_call(std::variant<SymRef, ValuePart> &&target, std::span<CallArg> args, ValueRef *result,
		bool variadic = false) {
		Assigner a{variadic};
		CallBuilder cb{*this, a};
		for (auto &arg : args) {
			cb.add_arg(std::move(arg));
		}
		cb.call(std::move(target));
		// With a dynamic stack, a call that passed arguments on it is followed by an add to the stack pointer, of an
		// 8-bit immediate if it fits.
		u32 pushed = tpde::util::align_up(a.get_stack_size(), 16);
		this->returned = text_writer.offset() - (!this->stack.has_dynamic_alloca || !pushed ? 0 : pushed < 128 ? 4 : 7);
		if (result) {
			cb.add_ret(*result);
		}
	}

	void reset() {
		Arch::reset();
		EncodeCompiler::reset();
	}

	/// A symbol defined here is reached relative to the code, others through the GOT; so the object is for an
	/// executable, not a shared library, where those would need the GOT too.
	void load_address_of_var_reference(AsmReg dst, tpde::AssignmentPartRef ap) {
		auto [sym, off, local] = address(ap.variable_ref_data());
		if (local) {
			ASM(LEA64rm, dst, FE_MEM(FE_IP, 0, FE_NOREG, -1));
			reloc_text(sym, tpde::elf::R_X86_64_PC32, text_writer.offset() - 4, off - 4);
			return;
		}
		ASM(MOV64rm, dst, FE_MEM(FE_IP, 0, FE_NOREG, -1));
		reloc_text(sym, tpde::elf::R_X86_64_GOTPCREL, text_writer.offset() - 4, -4);
		if (off) {
			ASM(LEA64rm, dst, FE_MEM(dst, 0, FE_NOREG, i32(off)));
		}
	}

	void condBr(u32 cond, u32 yes, u32 no) {
		ValueRef c = val_ref(cond);
		ASM(TEST8ri, c.part(0).load_to_reg(), 1);
		generate_cond_branch(Jump::jne, yes, no);
	}

	/// Starts the va_list at ap as the System V ABI lays it out: the offsets of the next general and float
	/// register arguments into the register save area the prologue fills, the stack arguments, and that area.
	void vaStart(u32 ap) {
		ValueRef list = val_ref(ap);
		ValuePartRef part = list.part(0);
		AsmReg p = part.load_to_reg();
		ScratchReg scratch{this};
		AsmReg r = scratch.alloc_gp();
		ASM(MOV64ri, r, u64(vec_arg_count * 16 + 48) << 32 | scalar_arg_count * 8);
		ASM(MOV64mr, FE_MEM(p, 0, FE_NOREG, 0), r);
		ASM(LEA64rm, r, FE_MEM(FE_BP, 0, FE_NOREG, i32(var_arg_stack_off)));
		ASM(MOV64mr, FE_MEM(p, 0, FE_NOREG, 8), r);
		ASM(LEA64rm, r, FE_MEM(FE_BP, 0, FE_NOREG, -i32(reg_save_frame_off)));
		ASM(MOV64mr, FE_MEM(p, 0, FE_NOREG, 16), r);
	}

	/// The frame pointer, which needs the function to have a frame.
	void frameAddress(u32 i) {
		this->stack.frame_used = true;
		ValuePartRef fp{this, ConfigX64::GP_BANK};
		ASM(MOV64rr, fp.alloc_reg(), FE_BP);
		this->result_ref(i).part(0).set_value(std::move(fp));
	}
};

bool compile(Module &m, std::vector<u8> &out) {
	Adaptor a(&m);
	X64 c(&a);
	if (!c.compile()) {
		return false;
	}
	out = c.assembler.build_object_file();
	return true;
}

} // namespace bober
#endif
