// TPDE over the module of ir.hpp: the adaptor through which TPDE walks it, and the instruction selection common to
// the architectures, which leans on the snippet encoders TPDE generates from tpde-llvm's encode_template.c. An
// architecture adds the few things only it can say: addresses of symbols, the conditional branch, the calling
// convention and the frame.
#pragma once
#include <algorithm>
#include <bit>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>

#include "ir.hpp"
#include "tpde.h"
#include "tpde/CompilerBase.hpp"

namespace bober {

using tpde::SymRef;

struct Adaptor {
	Module *m;
	u32 cur = 0;
	std::vector<u32> fns, allocas;
	/// If collected, the current function's parameters, instructions and allocas that hold refs.
	std::vector<u32> refs;
	/// Whether the current function calls, so that refs may need to be found in its frame.
	bool calls = false;
	/// Whether the current function has invokes or stack allocations, which need the frame pointer.
	bool dynamic = false;

	using IRValueRef = u32;
	using IRInstRef = u32;
	using IRBlockRef = u32;
	using IRFuncRef = u32;
	static constexpr u32 INVALID_VALUE_REF = 0, INVALID_BLOCK_REF = 0, INVALID_FUNC_REF = 0;
	static constexpr bool TPDE_PROVIDES_HIGHEST_VAL_IDX = true;
	static constexpr bool TPDE_LIVENESS_VISIT_ARGS = true;

	explicit Adaptor(Module *m) : m(m) {
		for (const Sym &s : m->syms) {
			if (m->vals[s.val].op == Op::Function && !s.removed) {
				fns.push_back(s.val);
			}
		}
	}
	Sym &sym() const { return m->sym(cur); }

	u32 func_count() const { return fns.size(); }
	const std::vector<u32> &funcs() const { return fns; }
	const std::vector<u32> &funcs_to_compile() const { return fns; }
	std::string_view func_link_name(u32 f) const { return m->sym(f).name; }
	bool func_extern(u32 f) const { return !m->defined(f); }
	bool func_only_local(u32 f) const { return m->sym(f).local; }
	bool func_has_weak_linkage(u32 f) const { return m->sym(f).weak; }

	bool cur_needs_unwind_info() const { return true; }
	bool cur_is_vararg() const { return m->types[sym().type].n == 1; }
	/// Addresses come after the function's own values.
	u32 cur_highest_val_idx() const { return sym().nlocal + m->addrs.size(); }
	/// The parameters follow their function's value.
	auto cur_args() const { return std::views::iota(cur + 1, cur + m->types[sym().type].elems.n); }
	bool cur_arg_is_byval(u32 i) const { return m->attr(cur, i + 1, AttrKind::ByVal); }
	u32 cur_arg_byval_size(u32 i) const { return m->types[m->attr(cur, i + 1, AttrKind::ByVal)->type].size; }
	u32 cur_arg_byval_align(u32 i) const { return m->types[m->attr(cur, i + 1, AttrKind::ByVal)->type].align; }
	bool cur_arg_is_sret(u32 i) const { return m->attr(cur, i + 1, AttrKind::SRet); }
	/// Every alloca, wherever it stands, is a slot of the frame.
	const std::vector<u32> &cur_static_allocas() const { return allocas; }
	/// Also TPDE's way of keeping the stack pointer right in landing pads, as tpde-llvm does.
	bool cur_has_dynamic_alloca() const { return dynamic; }
	u32 cur_entry_block() const { return m->blocksOf(sym())[0]; }
	std::span<const u32> cur_blocks() const { return m->blocksOf(sym()); }

	/// A view, as TPDE copies it and keeps iterators into the copy.
	std::span<const u32> block_succs(u32 b) const { return m->succs(b); }
	std::span<const u32> block_insts(u32 b) const { return m->insts(b); }
	std::ranges::empty_view<u32> block_phis(u32) const { return {}; }
	u32 block_info(u32 b) const { return m->blocks[b].info; }
	void block_set_info(u32 b, u32 info) { m->blocks[b].info = info; }
	u32 block_info2(u32 b) const { return m->blocks[b].info2; }
	void block_set_info2(u32 b, u32 info) { m->blocks[b].info2 = info; }
	u32 block_fmt_ref(u32 b) const { return b; }

	tpde::ValLocalIdx val_local_idx(u32 v) const { return tpde::ValLocalIdx(m->vals[v].local); }
	bool val_ignore_in_liveness_analysis(u32 v) const { return m->isConst(v); }
	bool val_is_phi(u32) const { return false; }
	struct NoPhi {
		u32 incoming_count() const { return 0; }
		u32 incoming_val_for_slot(u32) const { return 0; }
		u32 incoming_block_for_slot(u32) const { return 0; }
		u32 incoming_val_for_block(u32) const { return 0; }
	};
	NoPhi val_as_phi(u32) const { return {}; }
	u32 val_alloca_size(u32 v) const { return m->types[m->vals[v].imm].size; }
	u32 val_alloca_align(u32 v) const { return 1u << m->vals[v].sub; }
	u32 value_fmt_ref(u32 v) const { return v; }

	std::span<u32> inst_operands(u32 i) const { return m->ops(i); }
	auto inst_results(u32 i) const {
		return std::views::single(i) | std::views::drop(m->types[m->vals[i].type].parts.n == 0);
	}
	bool inst_fused(u32) const { return false; }
	u32 inst_fmt_ref(u32 i) const { return i; }

	void start_compile() {}
	void end_compile() {}
	bool switch_func(u32 f) {
		cur = f;
		allocas.clear();
		refs.clear();
		dynamic = calls = false;
		auto holdsRef = [&](u32 t) { return std::ranges::any_of(m->parts(m->types[t]), &Part::ref); };
		for (u32 a : cur_args()) {
			if (m->collected && holdsRef(m->vals[a].type)) {
				refs.push_back(a);
			}
		}
		for (u32 b : m->blocksOf(sym())) {
			for (u32 i : m->insts(b)) {
				const Value &v = m->vals[i];
				if (v.op == Op::Alloca) {
					allocas.push_back(i);
				}
				if (m->collected && holdsRef(v.op == Op::Alloca ? v.imm : v.type)) {
					refs.push_back(i);
				}
				dynamic |= v.op == Op::Invoke || v.op == Op::StackAlloc;
				calls |= v.op == Op::Call || v.op == Op::Invoke;
			}
		}
		return true;
	}
	void reset() {}
};

template <typename A, typename Derived, typename Config>
struct Base : tpde::CompilerBase<Adaptor, Derived, Config> {
	using B = tpde::CompilerBase<Adaptor, Derived, Config>;
	using typename B::CallArg;
	using typename B::GenericValuePart;
	using typename B::InstRange;
	using typename B::ValuePart;
	using typename B::ValuePartRef;
	using typename B::ValueRef;
	using Expr = typename GenericValuePart::Expr;
	using Encode1 = bool (Derived::*)(GenericValuePart &&, ValuePart &&);
	using Encode2 = bool (Derived::*)(GenericValuePart &&, GenericValuePart &&, ValuePart &&);

	struct ValRefSpecial {
		uint8_t mode = 4;
		u32 val;
	};

	/// Symbols of Module::syms, and of the library functions and typeinfo addresses used.
	std::vector<SymRef> syms;
	std::unordered_map<std::string, SymRef> libs;
	std::unordered_map<u32, SymRef> typeinfos;
	std::unordered_map<std::string, tpde::SecRef> sections;
	/// The calls of the function being compiled that refs live across: where each returns to, and the offsets from
	/// the frame pointer where those refs are.
	std::vector<std::pair<tpde::Label, std::vector<i32>>> sites;
	/// Where the last call made returns to, which the code restoring the stack pointer after it may follow.
	u32 returned = 0;

	explicit Base(Adaptor *a) : B(a), syms(a->m->syms.size()) {}

	Derived *derived() { return static_cast<Derived *>(this); }
	Module &m() { return *this->adaptor->m; }
	const Type &type(u32 v) { return m().types[m().vals[v].type]; }
	u64 bits(u32 v) { return type(v).kind == Type::Int ? type(v).n : 64; }
	bool fails(const std::string &what) { return m().fail(what + " is not supported by TPDE"), false; }

	bool cur_func_may_emit_calls() const { return true; }
	SymRef cur_personality_func() {
		u32 p = this->adaptor->sym().personality;
		return p ? syms[m().vals[p].imm] : SymRef();
	}
	bool try_force_fixed_assignment(u32) const { return false; }
	void setup_var_ref_assignments() {}
	void define_func_idx(u32 f, u32 idx) { syms[m().vals[f].imm] = this->func_syms[idx]; }

	struct Parts {
		std::span<const Part> parts;
		u32 count() const { return parts.size(); }
		u32 size_bytes(u32 i) const { return parts[i].size; }
		tpde::RegBank reg_bank(u32 i) const { return parts[i].fp ? Config::FP_BANK : Config::GP_BANK; }
	};
	std::span<const Part> parts(u32 v) { return m().parts(type(v)); }
	Parts val_parts(u32 v) { return {parts(v)}; }

	std::optional<ValRefSpecial> val_ref_special(u32 v) {
		return m().isConst(v) ? std::optional(ValRefSpecial{.val = v}) : std::nullopt;
	}
	/// The constant scalar at part of a constant; an address is a variable TPDE recomputes where it is used.
	ValuePart val_part_ref_special(ValRefSpecial &s, u32 part) {
		u32 v = s.val;
		while (m().vals[v].op == Op::Agg) {
			for (u32 e : m().ops(v)) {
				if (part < type(e).parts.n) {
					v = e;
					break;
				}
				part -= type(e).parts.n;
			}
		}
		const Value &c = m().vals[v];
		Part p = parts(v)[part];
		tpde::RegBank bank = p.fp ? Config::FP_BANK : Config::GP_BANK;
		if (c.op == Op::Const || c.op == Op::Zero || c.op == Op::Bytes) {
			u64 n = c.op == Op::Const ? c.imm : c.op == Op::Bytes ? u8(m().bytes[c.imm + part]) : 0;
			return ValuePart(n, p.size, bank);
		}
		auto idx = tpde::ValLocalIdx(this->adaptor->sym().nlocal + c.local);
		if (!this->val_assignment(idx)) {
			this->init_variable_ref(idx, v);
		}
		return ValuePart(idx, this->val_assignment(idx), 0, false);
	}

	/// The symbol and addend of an address constant, and whether it is defined here.
	std::tuple<SymRef, i64, bool> address(u32 v) {
		const Value &c = m().vals[v];
		u32 base = c.op == Op::Addr ? m().ops(v)[0] : v;
		return {syms[m().vals[base].imm], c.op == Op::Addr ? i64(c.imm) : 0, m().defined(base)};
	}

	SymRef lib(const std::string &name) {
		SymRef &s = libs[name];
		return s.valid() ? s : s = this->assembler.sym_add_undef(name, tpde::Assembler::SymBinding::GLOBAL);
	}

	/// Defines the globals, now that every symbol exists to be relocated against.
	bool hook_post_func_sym_init() {
		using enum tpde::SectionKind;
		using enum tpde::Assembler::SymBinding;
		auto &as = this->assembler;
		for (const Sym &s : m().syms) {
			if (m().vals[s.val].op == Op::Global && !s.removed) {
				auto binding = s.local ? LOCAL : s.weak ? WEAK : GLOBAL;
				SymRef &sym = syms[m().vals[s.val].imm];
				sym = s.init ? as.sym_predef_data(s.name, binding) : as.sym_add_undef(s.name, binding);
			}
		}
		for (const Sym &s : m().syms) {
			if (s.hidden && !s.removed) {
				as.sym_set_visibility(syms[m().vals[s.val].imm], std::remove_reference_t<decltype(as)>::SymVisibility::HIDDEN);
			}
		}
		for (const Sym &s : m().syms) {
			if (m().vals[s.val].op != Op::Global || s.init == 0 || s.removed) {
				continue;
			}
			std::vector<u8> data(type(s.init).size);
			std::vector<std::pair<u64, u32>> relocs;
			flatten(s.init, 0, data, relocs);
			SymRef sym = syms[m().vals[s.val].imm];
			u32 align = s.align ? s.align : type(s.init).align;
			if (m().vals[s.init].op == Op::Zero && !s.constant) {
				as.sym_def_predef_zero(as.get_default_section(BSS), sym, data.size(), align);
				continue;
			}
			auto kind = !s.constant ? Data : relocs.empty() ? ReadOnly : DataRelRO;
			tpde::SecRef sec = s.section.empty() ? as.get_default_section(kind) : section(s.section, kind);
			u32 off;
			as.sym_def_predef_data(sec, sym, data, align, &off);
			for (auto [at, v] : relocs) {
				auto [target, addend, _] = address(v);
				as.reloc_abs(sec, target, off + at, addend);
			}
		}
		return true;
	}

	/// The section of that name, made of kind.
	tpde::SecRef section(const std::string &name, tpde::SectionKind kind) {
		auto [it, fresh] = sections.try_emplace(name);
		if (fresh) {
			it->second = this->assembler.create_section(kind);
			this->assembler.rename_section(it->second, name);
		}
		return it->second;
	}

	/// Notes where the refs live across call are, which was just made: in the slots of the allocas, and those the
	/// values were spilled to, as no register is callee-saved in a collected module.
	void live(u32 call) {
		std::vector<i32> at;
		for (u32 v : this->adaptor->refs) {
			tpde::ValueAssignment *va = this->val_assignment(this->val_idx(v));
			// One past its last use but not yet freed is still reported, as its block may loop back to a use.
			if (!va || v == call) {
				continue;
			}
			if (va->variable_ref) {
				for (const Part &p : m().parts(m().types[m().vals[v].imm])) {
					if (p.ref && va->stack_variable) {
						at.push_back(va->frame_off + p.off);
					}
				}
				continue;
			}
			for (u32 k = 0; k < va->part_count; k++) {
				tpde::AssignmentPartRef ap{va, k};
				if (!parts(v)[k].ref) {
					continue;
				}
				if (!ap.stack_valid()) {
					m().fail("a ref in a register across a call");
				}
				at.push_back(ap.frame_off());
			}
		}
		if (!at.empty()) {
			tpde::Label ret = this->text_writer.label_create();
			this->text_writer.label_place(ret, returned);
			sites.emplace_back(ret, std::move(at));
		}
	}

	/// Writes the stack map of the function just compiled, which starts at start, into section .llvm_stackmaps, listed
	/// by its address in section bober_stackmaps, in
	/// LLVM's format, version 3 (llvm.org/docs/StackMaps.html): a record per call in sites, each a statepoint's three
	/// constants, the last no deopts, and then its refs as pairs of a base, at an offset from the frame pointer, the
	/// DWARF register fp, and a constant in place of a derived ref.
	void stackmap(u32 func_idx, u32 start, u16 fp) {
		if (sites.empty()) {
			return;
		}
		std::vector<u8> d;
		auto put = [&](auto x) { d.insert(d.end(), (const u8 *)&x, (const u8 *)&x + sizeof x); };
		auto loc = [&](u8 kind, u16 reg, i32 off) {
			put(u16(kind)), put(u16(8)), put(reg), put(u16(0)), put(off);
		};
		auto align8 = [&] { d.resize((d.size() + 7) & ~size_t(7)); };
		put(u32(3)), put(u32(1)), put(u32(0)), put(u32(sites.size()));
		put(u64(0)), put(u64(0)), put(u64(sites.size()));
		for (auto &[ret, at] : sites) {
			put(u64(0)), put(u32(this->text_writer.label_offset(ret) - start)), put(u16(0)), put(u16(3 + 2 * at.size()));
			for (int k = 0; k < 3; k++) {
				loc(4, 0, 0);
			}
			for (i32 off : at) {
				loc(3, fp, off);
				loc(4, 0, 0);
			}
			align8();
			put(u32(0));
			align8();
		}
		sites.clear();
		u32 off, at;
		u8 word[8] = {};
		auto sec = section(".llvm_stackmaps", tpde::SectionKind::DataRelRO);
		auto map = this->assembler.sym_def_data(sec, {}, d, 8, tpde::Assembler::SymBinding::LOCAL, &off);
		this->assembler.reloc_abs(sec, this->func_syms[func_idx], off + 16, 0);
		auto list = section("bober_stackmaps", tpde::SectionKind::DataRelRO);
		(void)this->assembler.sym_def_data(list, {}, word, 8, tpde::Assembler::SymBinding::LOCAL, &at);
		this->assembler.reloc_abs(list, map, at, 0);
	}

	/// Writes the bytes of the constant v at off, and notes where addresses go.
	void flatten(u32 v, u64 off, std::vector<u8> &data, std::vector<std::pair<u64, u32>> &relocs) {
		const Value &c = m().vals[v];
		const Type &t = type(v);
		switch (c.op) {
		case Op::Zero: break;
		case Op::Const: std::memcpy(&data[off], &c.imm, t.size); break;
		case Op::Bytes: std::memcpy(&data[off], m().bytes.data() + c.imm, t.n); break;
		case Op::Agg:
			for (u32 i = 0; i < c.ops.n; i++) {
				u64 at = t.kind == Type::Struct ? m().nums[t.offs + i] : i * m().types[m().elems(t)[0]].size;
				flatten(m().ops(v)[i], off + at, data, relocs);
			}
			break;
		default: relocs.push_back({off, v});
		}
	}

	bool compile_inst(u32 i, InstRange) {
		const Value &v = m().vals[i];
		switch (v.op) {
		case Op::Alloca: return true;
		case Op::StackAlloc: return stackAlloc(i);
		case Op::Ret: return ret(i);
		case Op::Br: return this->generate_uncond_branch(succ(0)), true;
		case Op::CondBr: return derived()->condBr(m().ops(i)[0], succ(0), succ(1)), true;
		case Op::Switch: return table(i, v);
		case Op::Unreachable: return derived()->encode_trap(), this->release_regs_after_return(), true;
		case Op::Binary: return v.sub >= TpdeFAdd ? floatBinary(i, v) : intBinary(i, v);
		case Op::ICmp: return icmp(i, v);
		case Op::FCmp: return fcmp(i, v);
		case Op::Cast: return cast(i, v);
		case Op::Select: return select(i, v);
		case Op::Load: return load(i, v);
		case Op::Store: return store(i, v);
		case Op::Offset: return offset(i, v);
		case Op::Extract:
		case Op::Insert: return element(i, v);
		case Op::Unary: return unary(i, v);
		case Op::Fence: return derived()->encode_fence_seqcst();
		case Op::Trap: return derived()->encode_trap();
		case Op::VaStart: return derived()->vaStart(m().ops(i)[0]), true;
		case Op::FrameAddress: return derived()->frameAddress(i), true;
		case Op::Call: return call(i, v);
		case Op::Invoke: return invoke(i, v);
		case Op::LandingPad: return true;
		default: return fails("this instruction");
		}
	}

	bool stackAlloc(u32 i) {
		ValueRef bytes = this->val_ref(m().ops(i)[0]);
		ValueRef res = this->result_ref(i);
		ValuePartRef p = res.part(0);
		derived()->alloca_dynamic(1, bytes.part(0), 16, p);
		return true;
	}

	/// Successor k of the block being compiled.
	u32 succ(u32 k) { return m().succs(this->analyzer.block_ref(this->cur_block_idx))[k]; }

	bool ret(u32 i) {
		typename B::RetBuilder rb{*derived(), *derived()->cur_cc_assigner()};
		u32 f = this->adaptor->cur;
		bool sign = m().attr(f, 0, AttrKind::SignExt);
		std::span<u32> ops = m().ops(i);
		if (!ops.empty() && (sign || m().attr(f, 0, AttrKind::ZeroExt)) && bits(ops[0]) < 32) {
			auto [vr, vp] = this->val_ref_single(ops[0]);
			rb.add(std::move(vp).into_extended(sign, bits(ops[0]), 32), {});
		} else if (!ops.empty() && !parts(ops[0]).empty()) {
			rb.add(ops[0]);
		}
		rb.ret();
		return true;
	}

	bool table(u32 i, const Value &v) {
		std::span<const u32> succs = m().succs(this->analyzer.block_ref(this->cur_block_idx));
		u32 key = m().ops(i)[0];
		u64 n = bits(key);
		std::vector<std::pair<u64, u32>> cases;
		for (u32 k = 1; k < succs.size(); k++) {
			u64 c = m().nums[v.imm + k - 1];
			cases.push_back({n < 64 ? c & ((u64{1} << n) - 1) : c, succs[k]});
		}
		std::sort(cases.begin(), cases.end());
		ValueRef x = this->val_ref(key);
		this->generate_switch(widened(x.part(0), n, false).into_scratch(), n <= 32 ? 32 : 64, succs[0], cases);
		return true;
	}

	/// a extended for op to the width its encoder takes, 32 or 64 bits.
	ValuePartRef widened(ValuePartRef &&a, u64 n, bool sign) {
		return n % 32 ? std::move(a).into_extended(sign, n, n < 32 ? 32 : 64) : std::move(a);
	}

	bool intBinary(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode2 fns[][2] = {
			{&Derived::encode_addi32, &Derived::encode_addi64}, {&Derived::encode_subi32, &Derived::encode_subi64},
			{&Derived::encode_muli32, &Derived::encode_muli64}, {&Derived::encode_udivi32, &Derived::encode_udivi64},
			{&Derived::encode_sdivi32, &Derived::encode_sdivi64}, {&Derived::encode_uremi32, &Derived::encode_uremi64},
			{&Derived::encode_sremi32, &Derived::encode_sremi64}, {&Derived::encode_shli32, &Derived::encode_shli64},
			{&Derived::encode_shri32, &Derived::encode_shri64}, {&Derived::encode_ashri32, &Derived::encode_ashri64},
			{&Derived::encode_landi32, &Derived::encode_landi64}, {&Derived::encode_lori32, &Derived::encode_lori64},
			{&Derived::encode_lxori32, &Derived::encode_lxori64},
		};
		u64 n = bits(i);
		bool sign = v.sub == TpdeSDiv || v.sub == TpdeSRem || v.sub == TpdeAShr;
		bool extLhs = v.sub >= TpdeUDiv && v.sub <= TpdeSRem || v.sub == TpdeLShr || v.sub == TpdeAShr;
		bool extRhs = v.sub >= TpdeUDiv && v.sub <= TpdeSRem;
		ValueRef a = this->val_ref(ops[0]), b = this->val_ref(ops[1]);
		ValuePartRef pa = a.part(0), pb = b.part(0);
		if (pa.is_const() && !pb.is_const() && (v.sub <= TpdeMul && v.sub != TpdeSub || v.sub >= TpdeAnd)) {
			std::swap(pa, pb); // the encoders take an immediate only on the right
		}
		pa = extLhs ? widened(std::move(pa), n, sign) : std::move(pa);
		pb = extRhs ? widened(std::move(pb), n, sign) : std::move(pb);
		return (derived()->*fns[v.sub][n > 32])(std::move(pa), std::move(pb), this->result_ref(i).part(0));
	}

	bool floatBinary(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode2 fns[][2] = {
			{&Derived::encode_addf32, &Derived::encode_addf64}, {&Derived::encode_subf32, &Derived::encode_subf64},
			{&Derived::encode_mulf32, &Derived::encode_mulf64}, {&Derived::encode_divf32, &Derived::encode_divf64},
		};
		bool f64 = type(i).kind == Type::F64;
		if (v.sub == TpdeFRem) {
			ValueRef res = this->result_ref(i);
			CallArg args[] = {CallArg(ops[0]), CallArg(ops[1])};
			derived()->generate_call(lib(f64 ? "fmod" : "fmodf"), args, &res);
			return true;
		}
		ValueRef a = this->val_ref(ops[0]), b = this->val_ref(ops[1]);
		return (derived()->*fns[v.sub - TpdeFAdd][f64])(a.part(0), b.part(0), this->result_ref(i).part(0));
	}

	bool icmp(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode2 fns[][2] = {
			{&Derived::encode_icmp_eqi32, &Derived::encode_icmp_eqi64},
			{&Derived::encode_icmp_nei32, &Derived::encode_icmp_nei64},
			{&Derived::encode_icmp_ugti32, &Derived::encode_icmp_ugti64},
			{&Derived::encode_icmp_ugei32, &Derived::encode_icmp_ugei64},
			{&Derived::encode_icmp_ulti32, &Derived::encode_icmp_ulti64},
			{&Derived::encode_icmp_ulei32, &Derived::encode_icmp_ulei64},
			{&Derived::encode_icmp_sgti32, &Derived::encode_icmp_sgti64},
			{&Derived::encode_icmp_sgei32, &Derived::encode_icmp_sgei64},
			{&Derived::encode_icmp_slti32, &Derived::encode_icmp_slti64},
			{&Derived::encode_icmp_slei32, &Derived::encode_icmp_slei64},
		};
		u64 n = bits(ops[0]);
		bool sign = v.sub >= TpdeIntSGT;
		ValueRef a = this->val_ref(ops[0]), b = this->val_ref(ops[1]);
		return (derived()->*fns[v.sub][n > 32])(widened(a.part(0), n, sign), widened(b.part(0), n, sign),
			this->result_ref(i).part(0));
	}

	bool fcmp(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
#define FCMP(p) {&Derived::encode_fcmp_##p##_float, &Derived::encode_fcmp_##p##_double}
		static constexpr Encode2 fns[][2] = {FCMP(oeq), FCMP(ogt), FCMP(oge), FCMP(olt), FCMP(ole), FCMP(one),
			FCMP(ord), FCMP(ueq), FCMP(ugt), FCMP(uge), FCMP(ult), FCMP(ule), FCMP(une), FCMP(uno)};
#undef FCMP
		ValueRef a = this->val_ref(ops[0]), b = this->val_ref(ops[1]);
		return (derived()->*fns[v.sub][type(ops[0]).kind == Type::F64])(a.part(0), b.part(0),
			this->result_ref(i).part(0));
	}

	bool cast(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode1 toInt[][2][2] = {
			{{&Derived::encode_f32tou32, &Derived::encode_f32tou64},
				{&Derived::encode_f32toi32, &Derived::encode_f32toi64}},
			{{&Derived::encode_f64tou32, &Derived::encode_f64tou64},
				{&Derived::encode_f64toi32, &Derived::encode_f64toi64}},
		};
		static constexpr Encode1 toFloat[][2][2] = {
			{{&Derived::encode_u32tof32, &Derived::encode_u64tof32},
				{&Derived::encode_i32tof32, &Derived::encode_i64tof32}},
			{{&Derived::encode_u32tof64, &Derived::encode_u64tof64},
				{&Derived::encode_i32tof64, &Derived::encode_i64tof64}},
		};
		ValueRef a = this->val_ref(ops[0]);
		ValueRef res = this->result_ref(i);
		ValuePartRef src = a.part(0), dst = res.part(0);
		u64 from = bits(ops[0]), to = bits(i);
		switch (v.sub) {
		case TpdeZExt:
		case TpdeSExt:
		case TpdeIntToPtr:
			dst.set_value(from < to ? std::move(src).into_extended(v.sub == TpdeSExt, from, to) : std::move(src));
			return true;
		case TpdeFPToUI:
		case TpdeFPToSI:
			return (derived()->*toInt[type(ops[0]).kind == Type::F64][v.sub == TpdeFPToSI][to > 32])(std::move(src),
				std::move(dst));
		case TpdeUIToFP:
		case TpdeSIToFP:
			return (derived()->*toFloat[type(i).kind == Type::F64][v.sub == TpdeSIToFP][from > 32])(
				widened(std::move(src), from, v.sub == TpdeSIToFP), std::move(dst));
		case TpdeFPTrunc: return derived()->encode_f64tof32(std::move(src), std::move(dst));
		case TpdeFPExt: return derived()->encode_f32tof64(std::move(src), std::move(dst));
		default: // trunc, ptrtoint and bitcast keep the bits; a bitcast may move them to the other bank
			if (src.bank() == dst.bank()) {
				dst.set_value(std::move(src));
			} else {
				derived()->mov(dst.alloc_reg(), src.load_to_reg(), dst.part_size());
			}
			return true;
		}
	}

	bool select(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		if (parts(i).size() != 1) {
			return fails("select of an aggregate");
		}
		using Encode3 = bool (Derived::*)(GenericValuePart &&, GenericValuePart &&, GenericValuePart &&, ValuePart &&);
		static constexpr Encode3 fns[] = {&Derived::encode_select_i32, &Derived::encode_select_i64,
			&Derived::encode_select_f32, &Derived::encode_select_f64};
		Part p = parts(i)[0];
		ValueRef c = this->val_ref(ops[0]), a = this->val_ref(ops[1]), b = this->val_ref(ops[2]);
		Encode3 fn = fns[p.fp * 2 + (p.size == 8)];
		return (derived()->*fn)(c.part(0), a.part(0), b.part(0), this->result_ref(i).part(0));
	}

	/// Where part's encoder stands in the tables of load and store: i8, i16, i32, i64, f32, f64.
	static u32 kind(Part p) { return p.fp ? 4 + (p.size == 8) : std::countr_zero(p.size); }

	/// Loads each part of i from p, plus its offset.
	bool load(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode1 fns[] = {&Derived::encode_loadi8_zext, &Derived::encode_loadi16_zext,
			&Derived::encode_loadi32_zext, &Derived::encode_loadi64, &Derived::encode_loadf32,
			&Derived::encode_loadf64};
		static constexpr Encode1 atomics[] = {&Derived::encode_atomic_load_u8_seqcst,
			&Derived::encode_atomic_load_u16_seqcst, &Derived::encode_atomic_load_u32_seqcst,
			&Derived::encode_atomic_load_u64_seqcst};
		std::span<const Part> ps = parts(i);
		if (ps.empty()) {
			this->val_ref(ops[0]); // an empty struct moves nothing, but uses its pointer
			return true;
		}
		if (v.sub && (ps.size() != 1 || ps[0].fp)) {
			return fails("an atomic load of this type");
		}
		ValueRef p = this->val_ref(ops[0]);
		ValuePartRef base = p.part(0);
		ValueRef res = this->result_ref(i);
		if (v.sub) {
			return (derived()->*atomics[kind(ps[0])])(std::move(base), res.part(0));
		}
		auto reg = base.load_to_reg();
		for (u32 k = 0; k < ps.size(); k++) {
			(derived()->*fns[kind(ps[k])])(Expr(reg, ps[k].off), res.part(k));
		}
		return true;
	}

	bool store(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		using Store = bool (Derived::*)(GenericValuePart &&, GenericValuePart &&);
		static constexpr Store fns[] = {&Derived::encode_storei8, &Derived::encode_storei16,
			&Derived::encode_storei32, &Derived::encode_storei64, &Derived::encode_storef32, &Derived::encode_storef64};
		static constexpr Store atomics[] = {&Derived::encode_atomic_store_u8_seqcst,
			&Derived::encode_atomic_store_u16_seqcst, &Derived::encode_atomic_store_u32_seqcst,
			&Derived::encode_atomic_store_u64_seqcst};
		std::span<const Part> ps = parts(ops[0]);
		if (ps.empty()) {
			this->val_ref(ops[1]); // an empty struct moves nothing, but uses its pointer
			return true;
		}
		if (v.sub && (ps.size() != 1 || ps[0].fp)) {
			return fails("an atomic store of this type");
		}
		ValueRef val = this->val_ref(ops[0]), p = this->val_ref(ops[1]);
		ValuePartRef base = p.part(0);
		auto part = [&](u32 k) { // an i1 is stored as 0 or 1
			u8 n = ps[k].bits;
			return n % 8 ? val.part(k).into_extended(false, n, 32) : val.part(k);
		};
		if (v.sub) {
			return (derived()->*atomics[kind(ps[0])])(std::move(base), part(0));
		}
		auto reg = base.load_to_reg();
		for (u32 k = 0; k < ps.size(); k++) {
			(derived()->*fns[kind(ps[k])])(Expr(reg, ps[k].off), part(k));
		}
		return true;
	}

	/// The base plus each index times its scale plus the constant offset.
	bool offset(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		const u64 *nums = &m().nums[v.imm]; // the constant offset, then the scale of each index
		ValueRef p = this->val_ref(ops[0]);
		ValuePartRef acc = p.part(0);
		if (ops.size() == 1 && acc.has_assignment() && acc.assignment().is_stack_variable()) {
			this->result_ref_stack_slot(i, acc.assignment(), i32(nums[0]));
			return true;
		}
		auto imm = [&](u64 n) { return ValuePartRef(this, n, 8, Config::GP_BANK); };
		for (size_t k = 1; k < ops.size(); k++) {
			ValueRef x = this->val_ref(ops[k]);
			u64 n = bits(ops[k]);
			ValuePartRef ix = n < 64 ? x.part(0).into_extended(true, n, 64) : x.part(0);
			if (nums[k] != 1) {
				ValuePartRef scaled(this, Config::GP_BANK);
				derived()->encode_muli64(std::move(ix), imm(nums[k]), scaled);
				ix = std::move(scaled);
			}
			ValuePartRef sum(this, Config::GP_BANK);
			derived()->encode_addi64(std::move(acc), std::move(ix), sum);
			acc = std::move(sum);
		}
		if (nums[0] != 0) {
			ValuePartRef sum(this, Config::GP_BANK);
			derived()->encode_addi64(std::move(acc), imm(nums[0]), sum);
			acc = std::move(sum);
		}
		this->result_ref(i).part(0).set_value(std::move(acc));
		return true;
	}

	/// Extract's element of an aggregate, or the aggregate with Insert's element in its place.
	bool element(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		const Type &t = type(ops[0]);
		std::span<const u32> elems = m().elems(t);
		u32 first = 0, n = m().types[elems[t.kind == Type::Struct ? v.imm : 0]].parts.n;
		for (u64 k = 0; k < v.imm; k++) {
			first += m().types[elems[t.kind == Type::Struct ? k : 0]].parts.n;
		}
		ValueRef a = this->val_ref(ops[0]);
		ValueRef res = this->result_ref(i);
		if (v.op == Op::Extract) {
			for (u32 k = 0; k < n; k++) {
				res.part(k).set_value(a.part(first + k));
			}
			return true;
		}
		ValueRef e = this->val_ref(ops[1]);
		for (u32 k = 0; k < t.parts.n; k++) {
			res.part(k).set_value(k >= first && k < first + n ? e.part(k - first) : a.part(k));
		}
		return true;
	}

	bool unary(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		static constexpr Encode1 floats[][2] = {{&Derived::encode_fnegf32, &Derived::encode_fnegf64},
			{&Derived::encode_fabsf32, &Derived::encode_fabsf64}};
		static constexpr Encode1 ints[][4] = {
			{nullptr, &Derived::encode_bswapi16, &Derived::encode_bswapi32, &Derived::encode_bswapi64},
			{&Derived::encode_ctlzi8, &Derived::encode_ctlzi16, &Derived::encode_ctlzi32, &Derived::encode_ctlzi64},
			{&Derived::encode_cttzi8, &Derived::encode_cttzi16, &Derived::encode_cttzi32, &Derived::encode_cttzi64},
			{&Derived::encode_ctpopi32, &Derived::encode_ctpopi32, &Derived::encode_ctpopi32,
				&Derived::encode_ctpopi64},
		};
		ValueRef a = this->val_ref(ops[0]);
		Part p = parts(i)[0];
		if (v.sub <= TpdeFAbs) {
			return (derived()->*floats[v.sub][p.size == 8])(a.part(0), this->result_ref(i).part(0));
		}
		u64 n = bits(i);
		Encode1 fn = ints[v.sub - TpdeBSwap][kind(p)];
		if (v.sub == TpdeCtpop) {
			return (derived()->*fn)(widened(a.part(0), n, false), this->result_ref(i).part(0));
		}
		return n == p.size * 8u && fn ? (derived()->*fn)(a.part(0), this->result_ref(i).part(0))
			: fails("this bit operation on an i" + std::to_string(n));
	}

	/// A parameter's registers or stack slot; an empty struct has none. A struct's fields go each where a scalar would,
	/// some in the last registers and the rest on the stack, as LLVM passes it; an array's all in registers or all on
	/// the stack, as C's on arm64 (src/backend/abi.bob, which passes a C struct that does not fit through memory).
	void prologue_assign_arg(tpde::CCAssigner *cc, u32 k, u32 arg, u32 align = 1, bool split = false) {
		if (type(arg).parts.n > 0) {
			B::prologue_assign_arg(cc, k, arg, align, split || type(arg).kind == Type::Struct);
		}
	}

	bool call(u32 i, const Value &v) {
		std::span<u32> ops = m().ops(i);
		u32 callee = ops[0];
		tpde::util::SmallVector<CallArg, 8> args;
		for (u32 k = 1; k < ops.size(); k++) {
			if (type(ops[k]).parts.n == 0) {
				continue; // An empty struct, which takes no register.
			}
			CallArg &a = args.emplace_back(ops[k]);
			auto attr = [&](AttrKind kind) {
				const Attr *at = m().attr(i, k, kind);
				return at ? at : m().attr(callee, k, kind);
			};
			if (const Attr *at = attr(AttrKind::ByVal)) {
				a.flag = CallArg::Flag::byval;
				a.byval_size = m().types[at->type].size;
				a.byval_align = m().types[at->type].align;
			} else if (attr(AttrKind::SRet)) {
				a.flag = CallArg::Flag::sret;
			} else if (bool zext = attr(AttrKind::ZeroExt); zext || attr(AttrKind::SignExt)) {
				a.flag = zext ? CallArg::Flag::zext : CallArg::Flag::sext;
				a.ext_bits = bits(ops[k]);
			} else if (type(ops[k]).kind == Type::Struct) {
				a.flag = CallArg::Flag::allow_split; // as prologue_assign_arg takes it
			}
		}
		std::optional<ValueRef> res;
		if (type(i).parts.n != 0) {
			res.emplace(this->result_ref(i));
		}
		bool variadic = m().types[v.imm].n;
		if (m().vals[callee].op == Op::Function) {
			derived()->generate_call(syms[m().vals[callee].imm], args, res ? &*res : nullptr, variadic);
		} else {
			ValueRef target = this->val_ref(callee);
			derived()->generate_call(target.part(0), args, res ? &*res : nullptr, variadic);
		}
		if (m().collected) {
			live(i);
		}
		return true;
	}

	/// A call whose call site the unwinder maps to the landing pad, as tpde-llvm's compile_invoke does.
	bool invoke(u32 i, const Value &v) {
		auto spilled = this->spill_before_branch();
		u32 start = this->text_writer.offset();
		call(i, v);
		u32 end = this->text_writer.offset();
		u32 then = succ(0), unwind = succ(1);
		auto pad = this->block_labels[u32(this->analyzer.block_idx(unwind))];
		spilled |= this->spill_before_branch(true);
		derived()->generate_branch_to_block(Derived::Jump::jmp, then, false, true);
		this->release_spilled_regs(spilled);
		this->text_writer.except_add_call_site(start, end - start, pad, false);
		u32 typeinfo = m().ops(m().insts(unwind)[0])[0];
		this->text_writer.except_add_action(true, m().isAddr(typeinfo) ? typeinfoSym(typeinfo) : SymRef());
		return true;
	}

	/// A local word holding the address of typeinfo, which the exception table points at.
	SymRef typeinfoSym(u32 typeinfo) {
		SymRef &s = typeinfos[typeinfo];
		if (!s.valid()) {
			u32 off;
			u8 word[8] = {};
			auto sec = this->assembler.get_default_section(tpde::SectionKind::DataRelRO);
			s = this->assembler.sym_def_data(sec, {}, word, 8, tpde::Assembler::SymBinding::LOCAL, &off);
			auto [target, addend, _] = address(typeinfo);
			this->assembler.reloc_abs(sec, target, off, addend);
		}
		return s;
	}
};

} // namespace bober
