// tpde.h over the module of ir.hpp: building, folding constants, laying out types, and checking before compiling.
#include "tpde.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ir.hpp"

using namespace bober;

struct TpdeModule : Module {
	std::string msg;
};

namespace {

u64 alignUp(u64 n, u64 a) { return (n + a - 1) / a * a; }

u64 mask(u64 n, u64 bits) { return bits >= 64 ? n : n & ((u64{1} << bits) - 1); }

i64 sext(u64 n, u64 bits) { return bits >= 64 ? i64(n) : i64(n << (64 - bits)) >> (64 - bits); }

/// Appends first and then the n of rest to pool.
Run push(std::vector<u32> &pool, std::initializer_list<u32> first, const u64 *rest = nullptr, u64 n = 0) {
	Run r{u32(pool.size()), u32(first.size() + n)};
	pool.insert(pool.end(), first);
	pool.insert(pool.end(), rest, rest + n);
	return r;
}

/// A step of FNV-1a, over words rather than bytes.
u64 mix(u64 h, u64 x) { return (h ^ x) * 0x100000001b3; }

/// The type of that kind, n and elements, laid out when first asked for; elems were just pushed to the end of
/// Module::elemList, and are dropped again if the type exists.
u32 type(Module &m, Type::Kind kind, u64 n = 0, Run elems = {}) {
	if (kind == Type::Int && n > 64) {
		return m.fail("integers wider than 64 bits are not supported by TPDE");
	}
	std::span<const u32> es = std::span(m.elemList).subspan(elems.at, elems.n);
	u64 h = mix(mix(0xcbf29ce484222325, kind), n);
	for (u32 e : es) {
		h = mix(h, e);
	}
	for (;; h++) {
		auto [it, fresh] = m.typeIds.try_emplace(h, m.types.size());
		if (fresh) {
			break;
		}
		const Type &t = m.types[it->second];
		if (t.kind == kind && t.n == n && std::ranges::equal(m.elems(t), es)) {
			m.elemList.resize(m.elemList.size() - elems.n);
			return it->second;
		}
	}
	Type t{.kind = kind, .n = n, .elems = elems, .offs = u32(m.nums.size()), .parts = {u32(m.partList.size())}};
	auto scalar = [&](u64 size, bool fp) {
		t.size = t.align = size;
		m.partList.push_back({0, u8(size), u8(kind == Type::Int ? n : size * 8), fp, kind == Type::Ptr && n == 1});
	};
	auto add = [&](u32 e, u64 off) {
		Run r = m.types[e].parts;
		for (u32 k = r.at; k < r.at + r.n; k++) {
			Part p = m.partList[k];
			p.off += off;
			m.partList.push_back(p);
		}
	};
	switch (kind) {
	case Type::Int: scalar(n <= 8 ? 1 : n <= 16 ? 2 : n <= 32 ? 4 : 8, false); break;
	case Type::F32: scalar(4, true); break;
	case Type::F64: scalar(8, true); break;
	case Type::Ptr: scalar(8, false); break;
	case Type::Struct:
		for (u32 k = elems.at; k < elems.at + elems.n; k++) {
			const Type &e = m.types[m.elemList[k]];
			u32 align = n ? 1 : e.align;
			t.align = std::max(t.align, align);
			m.nums.push_back(alignUp(t.size, align));
			add(m.elemList[k], m.nums.back());
			t.size = m.nums.back() + e.size;
		}
		t.size = alignUp(t.size, t.align);
		break;
	case Type::Array: {
		const Type &e = m.types[m.elemList[elems.at]];
		t.align = e.align;
		t.size = e.size * n;
		for (u64 i = 0; i < n; i++) {
			add(m.elemList[elems.at], i * e.size);
		}
		break;
	}
	default: break;
	}
	t.parts.n = m.partList.size() - t.parts.at;
	m.types.push_back(t);
	return m.types.size() - 1;
}

u32 add(Module &m, Value v) {
	if (v.op == Op::Addr || v.op == Op::Function || v.op == Op::Global) {
		v.local = m.addrs.size();
		m.addrs.push_back(m.vals.size());
	}
	m.vals.push_back(v);
	return m.vals.size() - 1;
}

u32 constant(Module &m, u32 t, u64 bits) {
	const Type &ty = m.types[t];
	return add(m, {.op = Op::Const, .type = t, .imm = ty.kind == Type::Int ? mask(bits, ty.n) : bits});
}

/// The address off bytes past base, a function or global, as a value of type t.
u32 address(Module &m, u32 base, i64 off, u32 t) {
	if (off == 0 && t == type(m, Type::Ptr)) {
		return base;
	}
	if (m.types[t].size != 8) {
		return m.fail("a constant address does not fit in a type smaller than a pointer");
	}
	return add(m, {.op = Op::Addr, .type = t, .imm = u64(off), .ops = push(m.opList, {base})});
}

/// Appends an instruction to the current block, which jumps to succs if it is the terminator.
u32 inst(Module &m, Value v, Run succs = {}) {
	if (m.cur == 0) {
		return m.fail("an instruction was built outside a block");
	}
	Block &b = m.blocks[m.cur];
	v.block = m.cur;
	v.local = m.sym(b.fn).nlocal++;
	b.succs = succs;
	b.last = m.vals.size();
	m.vals.push_back(v);
	return b.last;
}

bool terminates(Op op) { return (op >= Op::Ret && op <= Op::Unreachable) || op == Op::Invoke; }

u32 symbol(Module &m, const char *name, Value v, Sym s) {
	s.val = m.vals.size();
	s.name = name;
	// Local symbols may share a name, as LLVM's would by renaming; the name then finds the first.
	if (!s.name.empty() && !m.names.emplace(s.name, s.val).second && !s.local) {
		return m.fail("the symbol " + s.name + " is defined twice");
	}
	v.type = type(m, Type::Ptr);
	v.imm = m.syms.size();
	m.syms.push_back(std::move(s));
	return add(m, v);
}

u32 named(Module &m, const char *name, Op op) {
	auto it = m.names.find(name);
	return it != m.names.end() && m.vals[it->second].op == op ? it->second : 0;
}

} // namespace

void bober::group(Module &m) {
	// Counting sorts: count each run's length, give each run its start, then fill them in the order built.
	for (Block &b : m.blocks) {
		b.insts = {};
	}
	for (u32 i = 1; i < m.vals.size(); i++) {
		m.blocks[m.vals[i].block].insts.n += m.vals[i].block != 0;
	}
	u32 at = 0;
	for (Block &b : m.blocks) {
		b.insts.at = at;
		at += b.insts.n;
		b.insts.n = 0;
	}
	m.instList.resize(at);
	for (u32 i = 1; i < m.vals.size(); i++) {
		if (Run &r = m.blocks[m.vals[i].block].insts; m.vals[i].block != 0) {
			m.instList[r.at + r.n++] = i;
		}
	}
	for (Sym &s : m.syms) {
		s.blocks = {};
	}
	for (u32 b = 1; b < m.blocks.size(); b++) {
		m.sym(m.blocks[b].fn).blocks.n++;
	}
	at = 0;
	for (Sym &s : m.syms) {
		s.blocks.at = at;
		at += s.blocks.n;
		s.blocks.n = 0;
	}
	m.blockList.resize(at);
	for (u32 b = 1; b < m.blocks.size(); b++) {
		Run &r = m.sym(m.blocks[b].fn).blocks;
		m.blockList[r.at + r.n++] = b;
	}
	std::stable_sort(m.attrs.begin(), m.attrs.end(), [](const Attr &a, const Attr &b) { return a.val < b.val; });
}

extern "C" {

TpdeModuleRef TpdeModuleCreate(void) { return new TpdeModule(); }
void TpdeModuleDispose(TpdeModuleRef m) { delete m; }

TpdeTypeRef TpdeVoidType(TpdeModuleRef m) { return type(*m, Type::Void); }
TpdeTypeRef TpdeIntType(TpdeModuleRef m, unsigned bits) { return type(*m, Type::Int, bits); }
TpdeTypeRef TpdeFloatType(TpdeModuleRef m) { return type(*m, Type::F32); }
TpdeTypeRef TpdeDoubleType(TpdeModuleRef m) { return type(*m, Type::F64); }
TpdeTypeRef TpdePointerType(TpdeModuleRef m) { return type(*m, Type::Ptr); }
TpdeTypeRef TpdeRefType(TpdeModuleRef m) { return type(*m, Type::Ptr, 1); }
TpdeTypeRef TpdeFunctionType(TpdeModuleRef m, TpdeTypeRef result, TpdeTypeRef *params, unsigned n, int variadic) {
	return type(*m, Type::Fn, variadic != 0, push(m->elemList, {u32(result)}, params, n));
}
TpdeTypeRef TpdeStructType(TpdeModuleRef m, TpdeTypeRef *fields, unsigned n, int packed) {
	return type(*m, Type::Struct, packed != 0, push(m->elemList, {}, fields, n));
}
TpdeTypeRef TpdeArrayType(TpdeModuleRef m, TpdeTypeRef elem, uint64_t n) {
	return type(*m, Type::Array, n, push(m->elemList, {u32(elem)}));
}

TpdeValueRef TpdeConstInt(TpdeModuleRef m, TpdeTypeRef t, uint64_t n) { return constant(*m, t, n); }
TpdeValueRef TpdeConstReal(TpdeModuleRef m, TpdeTypeRef t, double x) {
	float f = x;
	u64 bits = 0;
	m->types[t].kind == Type::F32 ? std::memcpy(&bits, &f, 4) : std::memcpy(&bits, &x, 8);
	return constant(*m, t, bits);
}
TpdeValueRef TpdeConstIntOfString(TpdeModuleRef m, TpdeTypeRef t, const char *digits, uint8_t radix) {
	bool negative = *digits == '-';
	u64 n = 0;
	for (const char *p = digits + negative; *p; p++) {
		n = n * radix + u64(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
	}
	return constant(*m, t, negative ? -n : n);
}
TpdeValueRef TpdeConstRealOfString(TpdeModuleRef m, TpdeTypeRef t, const char *text) {
	if (m->types[t].kind == Type::F32) {
		float f = std::strtof(text, nullptr);
		u64 bits = 0;
		std::memcpy(&bits, &f, 4);
		return constant(*m, t, bits);
	}
	return TpdeConstReal(m, t, std::strtod(text, nullptr));
}
TpdeValueRef TpdeConstNull(TpdeModuleRef m, TpdeTypeRef t) { return add(*m, {.op = Op::Zero, .type = u32(t)}); }
TpdeValueRef TpdeConstString(TpdeModuleRef m, const char *bytes, uint64_t n) {
	u32 t = type(*m, Type::Array, n, push(m->elemList, {type(*m, Type::Int, 8)}));
	m->bytes.append(bytes, n);
	return add(*m, {.op = Op::Bytes, .type = t, .imm = m->bytes.size() - n});
}
TpdeValueRef TpdeConstStruct(TpdeModuleRef m, TpdeValueRef *fields, unsigned n, int packed) {
	Run types{u32(m->elemList.size()), n};
	for (unsigned k = 0; k < n; k++) {
		m->elemList.push_back(m->vals[fields[k]].type);
	}
	u32 t = type(*m, Type::Struct, packed != 0, types);
	return add(*m, {.op = Op::Agg, .type = t, .ops = push(m->opList, {}, fields, n)});
}
TpdeValueRef TpdeConstArray(TpdeModuleRef m, TpdeTypeRef elem, TpdeValueRef *elems, uint64_t n) {
	u32 t = type(*m, Type::Array, n, push(m->elemList, {u32(elem)}));
	return add(*m, {.op = Op::Agg, .type = t, .ops = push(m->opList, {}, elems, n)});
}
TpdeValueRef TpdeSizeOf(TpdeModuleRef m, TpdeTypeRef t) {
	return constant(*m, type(*m, Type::Int, 64), m->types[t].size);
}
uint64_t TpdeABISizeOfType(TpdeModuleRef m, TpdeTypeRef t) { return m->types[t].size; }
TpdeValueRef TpdeConstGEP(TpdeModuleRef m, TpdeValueRef p, int64_t bytes) {
	const Value v = m->vals[p];
	if (v.op == Op::Addr) {
		return address(*m, m->ops(p)[0], i64(v.imm) + bytes, v.type);
	}
	if (m->isAddr(p)) {
		return address(*m, p, bytes, v.type);
	}
	return constant(*m, v.type, (v.op == Op::Const ? v.imm : 0) + bytes);
}
TpdeValueRef TpdeConstCast(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef p, TpdeTypeRef to) {
	const Value v = m->vals[p];
	if (v.op == Op::Const || v.op == Op::Zero) {
		return constant(*m, to, v.op == Op::Const ? v.imm : 0);
	}
	if (m->isAddr(p) && op != TpdeTrunc) {
		return v.op == Op::Addr ? address(*m, m->ops(p)[0], v.imm, to) : address(*m, p, 0, to);
	}
	return v.type == to ? p : m->fail("this constant cast is not supported by TPDE");
}

TpdeValueRef TpdeAddFunction(TpdeModuleRef m, const char *name, TpdeTypeRef t, int local) {
	u32 f = symbol(*m, name, {.op = Op::Function}, {.local = local != 0, .type = u32(t)});
	Run params = m->types[t].elems;
	for (u32 k = params.at + 1; f != 0 && k < params.at + params.n; k++) {
		m->vals.push_back({.op = Op::Param, .type = m->elemList[k], .local = m->sym(f).nlocal++});
	}
	return f;
}
TpdeValueRef TpdeGetNamedFunction(TpdeModuleRef m, const char *name) { return named(*m, name, Op::Function); }
TpdeValueRef TpdeGetNamedGlobal(TpdeModuleRef m, const char *name) { return named(*m, name, Op::Global); }
TpdeValueRef TpdeAddGlobal(TpdeModuleRef m, const char *name, TpdeValueRef init, int local, unsigned align) {
	bool constant = *name == 0;
	return symbol(*m, name, {.op = Op::Global},
		{.local = local || constant, .constant = constant, .init = u32(init), .align = align,
			.type = m->vals[init].type});
}
TpdeValueRef TpdeAddExternal(TpdeModuleRef m, const char *name, TpdeTypeRef t) {
	return symbol(*m, name, {.op = Op::Global}, {.type = u32(t)});
}
void TpdeAddAttributeAtIndex(TpdeModuleRef m, TpdeValueRef fn, int index, const char *name, TpdeTypeRef t) {
	static constexpr const char *names[] = {"byval", "sret", "zeroext", "signext"};
	for (u8 k = 0; k < std::size(names); k++) {
		if (std::strcmp(name, names[k]) == 0) {
			m->attrs.push_back({u32(fn), index, AttrKind(k), u32(t)});
		}
	}
}
int TpdeIsDeclaration(TpdeModuleRef m, TpdeValueRef g) {
	return m->vals[g].op == Op::Function ? m->sym(g).blocks.n == 0 : m->sym(g).init == 0;
}
void TpdeReplace(TpdeModuleRef m, TpdeValueRef old, TpdeValueRef by) {
	std::replace(m->opList.begin(), m->opList.end(), u32(old), u32(by));
	for (Sym &s : m->syms) {
		s.personality = s.personality == old ? by : s.personality;
		s.init = s.init == old ? by : s.init;
	}
	m->names.erase(m->sym(by).name);
	m->sym(by).name = m->sym(old).name;
	TpdeDeleteFunction(m, old);
	m->names[m->sym(by).name] = by;
}
void TpdeDeleteFunction(TpdeModuleRef m, TpdeValueRef fn) {
	m->sym(fn).removed = true;
	m->names.erase(m->sym(fn).name);
}
void TpdeSetWeak(TpdeModuleRef m, TpdeValueRef g) { m->sym(g).weak = true; }
void TpdeSetHidden(TpdeModuleRef m, TpdeValueRef g) { m->sym(g).hidden = true; }
void TpdeSetConstant(TpdeModuleRef m, TpdeValueRef g) { m->sym(g).constant = true; }
void TpdeSetInlineOnly(TpdeModuleRef m, TpdeValueRef fn) { m->sym(fn).inlineOnly = true; }
void TpdeSetPersonalityFn(TpdeModuleRef m, TpdeValueRef fn, TpdeValueRef p) { m->sym(fn).personality = p; }
TpdeValueRef TpdeGetParam(TpdeModuleRef, TpdeValueRef fn, unsigned i) { return fn + 1 + i; }
TpdeTypeRef TpdeTypeOf(TpdeModuleRef m, TpdeValueRef v) { return m->vals[v].type; }
TpdeTypeRef TpdeGetReturnType(TpdeModuleRef m, TpdeTypeRef t) { return m->elems(m->types[t])[0]; }
TpdeTypeRef TpdeGlobalGetValueType(TpdeModuleRef m, TpdeValueRef g) { return m->sym(g).type; }

TpdeBasicBlockRef TpdeAppendBasicBlock(TpdeModuleRef m, TpdeValueRef fn) {
	m->blocks.push_back({.fn = u32(fn)});
	m->sym(fn).blocks.n++;
	return m->blocks.size() - 1;
}
void TpdePositionAtEnd(TpdeModuleRef m, TpdeBasicBlockRef b) { m->cur = b; }
TpdeBasicBlockRef TpdeGetInsertBlock(TpdeModuleRef m) { return m->cur; }
TpdeValueRef TpdeGetBasicBlockTerminator(TpdeModuleRef m, TpdeBasicBlockRef b) {
	u32 last = m->blocks[b].last;
	return last && terminates(m->vals[last].op) ? last : 0;
}

TpdeValueRef TpdeBuildRet(TpdeModuleRef m, TpdeValueRef v) {
	return inst(*m, {.op = Op::Ret, .ops = push(m->opList, {u32(v)})});
}
TpdeValueRef TpdeBuildRetVoid(TpdeModuleRef m) { return inst(*m, {.op = Op::Ret}); }
TpdeValueRef TpdeBuildBr(TpdeModuleRef m, TpdeBasicBlockRef to) {
	return inst(*m, {.op = Op::Br}, push(m->succList, {u32(to)}));
}
TpdeValueRef TpdeBuildCondBr(TpdeModuleRef m, TpdeValueRef cond, TpdeBasicBlockRef yes, TpdeBasicBlockRef no) {
	return inst(*m, {.op = Op::CondBr, .ops = push(m->opList, {u32(cond)})}, push(m->succList, {u32(yes), u32(no)}));
}
TpdeValueRef TpdeBuildSwitch(TpdeModuleRef m, TpdeValueRef v, int64_t *cases, TpdeBasicBlockRef *targets, unsigned n,
	TpdeBasicBlockRef otherwise) {
	u64 at = m->nums.size();
	m->nums.insert(m->nums.end(), cases, cases + n);
	Value s{.op = Op::Switch, .imm = at, .ops = push(m->opList, {u32(v)})};
	return inst(*m, s, push(m->succList, {u32(otherwise)}, targets, n));
}
TpdeValueRef TpdeBuildUnreachable(TpdeModuleRef m) { return inst(*m, {.op = Op::Unreachable}); }
TpdeValueRef TpdeBuildBinOp(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef a, TpdeValueRef b) {
	return inst(*m, {.op = Op::Binary, .sub = u8(op), .type = m->vals[a].type, .ops = push(m->opList, {u32(a), u32(b)})});
}
TpdeValueRef TpdeBuildICmp(TpdeModuleRef m, TpdeIntPredicate pred, TpdeValueRef a, TpdeValueRef b) {
	u32 t = type(*m, Type::Int, 1);
	return inst(*m, {.op = Op::ICmp, .sub = u8(pred), .type = t, .ops = push(m->opList, {u32(a), u32(b)})});
}
TpdeValueRef TpdeBuildFCmp(TpdeModuleRef m, TpdeRealPredicate pred, TpdeValueRef a, TpdeValueRef b) {
	u32 t = type(*m, Type::Int, 1);
	return inst(*m, {.op = Op::FCmp, .sub = u8(pred), .type = t, .ops = push(m->opList, {u32(a), u32(b)})});
}
TpdeValueRef TpdeBuildCast(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef v, TpdeTypeRef to) {
	return inst(*m, {.op = Op::Cast, .sub = u8(op), .type = u32(to), .ops = push(m->opList, {u32(v)})});
}
TpdeValueRef TpdeBuildSelect(TpdeModuleRef m, TpdeValueRef cond, TpdeValueRef yes, TpdeValueRef no) {
	Run ops = push(m->opList, {u32(cond), u32(yes), u32(no)});
	return inst(*m, {.op = Op::Select, .type = m->vals[yes].type, .ops = ops});
}
TpdeValueRef TpdeBuildAlloca(TpdeModuleRef m, TpdeTypeRef t, unsigned align) {
	u32 a = align ? align : m->types[t].align;
	return inst(*m, {.op = Op::Alloca, .sub = u8(std::countr_zero(a)), .type = type(*m, Type::Ptr), .imm = t});
}
TpdeValueRef TpdeBuildStackAlloc(TpdeModuleRef m, TpdeValueRef bytes) {
	return inst(*m, {.op = Op::StackAlloc, .type = type(*m, Type::Ptr), .ops = push(m->opList, {u32(bytes)})});
}
TpdeValueRef TpdeBuildLoad(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, int) {
	return inst(*m, {.op = Op::Load, .type = u32(t), .ops = push(m->opList, {u32(p)})});
}
TpdeValueRef TpdeBuildStore(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef p, int) {
	return inst(*m, {.op = Op::Store, .ops = push(m->opList, {u32(v), u32(p)})});
}
TpdeValueRef TpdeBuildAtomicLoad(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p) {
	return inst(*m, {.op = Op::Load, .sub = 1, .type = u32(t), .ops = push(m->opList, {u32(p)})});
}
TpdeValueRef TpdeBuildAtomicStore(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef p) {
	return inst(*m, {.op = Op::Store, .sub = 1, .ops = push(m->opList, {u32(v), u32(p)})});
}
TpdeValueRef TpdeBuildFence(TpdeModuleRef m) { return inst(*m, {.op = Op::Fence}); }
TpdeValueRef TpdeBuildTrap(TpdeModuleRef m) { return inst(*m, {.op = Op::Trap}); }
TpdeValueRef TpdeBuildVaStart(TpdeModuleRef m, TpdeValueRef ap) {
	return inst(*m, {.op = Op::VaStart, .ops = push(m->opList, {u32(ap)})});
}
TpdeValueRef TpdeBuildFrameAddress(TpdeModuleRef m) {
	return inst(*m, {.op = Op::FrameAddress, .type = type(*m, Type::Ptr)});
}
TpdeValueRef TpdeBuildUnary(TpdeModuleRef m, TpdeUnaryOp op, TpdeValueRef v) {
	return inst(*m, {.op = Op::Unary, .sub = u8(op), .type = m->vals[v].type, .ops = push(m->opList, {u32(v)})});
}
TpdeValueRef TpdeBuildStructGEP(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, unsigned i) {
	u64 off = m->nums[m->types[t].offs + i];
	m->nums.push_back(off);
	Value v{.op = Op::Offset, .type = m->vals[p].type, .imm = m->nums.size() - 1, .ops = push(m->opList, {u32(p)})};
	return inst(*m, v);
}
TpdeValueRef TpdeBuildGEP(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, TpdeValueRef *indices, unsigned n) {
	Value v{.op = Op::Offset, .type = m->vals[p].type, .imm = m->nums.size(), .ops = push(m->opList, {u32(p)})};
	m->nums.push_back(0);
	u32 cur = t;
	for (unsigned k = 0; k < n; k++) {
		const Value &i = m->vals[indices[k]];
		const Type &ty = m->types[cur];
		u64 scale = ty.size;
		if (k > 0 && ty.kind == Type::Struct) {
			m->nums[v.imm] += m->nums[ty.offs + i.imm];
			cur = m->elems(ty)[i.imm];
			continue;
		}
		if (k > 0) {
			cur = m->elems(ty)[0];
			scale = m->types[cur].size;
		}
		if (i.op == Op::Const || i.op == Op::Zero) {
			m->nums[v.imm] += sext(i.op == Op::Const ? i.imm : 0, m->types[i.type].n) * scale;
		} else {
			m->opList.push_back(indices[k]);
			m->nums.push_back(scale);
			v.ops.n++;
		}
	}
	return inst(*m, v);
}
TpdeValueRef TpdeBuildExtractValue(TpdeModuleRef m, TpdeValueRef v, unsigned i) {
	const Type &t = m->types[m->vals[v].type];
	u32 e = m->elems(t)[t.kind == Type::Struct ? i : 0];
	return inst(*m, {.op = Op::Extract, .type = e, .imm = i, .ops = push(m->opList, {u32(v)})});
}
TpdeValueRef TpdeBuildInsertValue(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef e, unsigned i) {
	return inst(*m, {.op = Op::Insert, .type = m->vals[v].type, .imm = i, .ops = push(m->opList, {u32(v), u32(e)})});
}
TpdeValueRef TpdeBuildCall(TpdeModuleRef m, TpdeTypeRef fnType, TpdeValueRef fn, TpdeValueRef *args, unsigned n) {
	u32 t = m->elems(m->types[fnType])[0];
	return inst(*m, {.op = Op::Call, .type = t, .imm = fnType, .ops = push(m->opList, {u32(fn)}, args, n)});
}
TpdeValueRef TpdeBuildInvoke(TpdeModuleRef m, TpdeTypeRef fnType, TpdeValueRef fn, TpdeValueRef *args, unsigned n,
	TpdeBasicBlockRef then, TpdeBasicBlockRef unwind) {
	u32 t = m->elems(m->types[fnType])[0];
	Value v{.op = Op::Invoke, .type = t, .imm = fnType, .ops = push(m->opList, {u32(fn)}, args, n)};
	return inst(*m, v, push(m->succList, {u32(then), u32(unwind)}));
}
TpdeValueRef TpdeBuildLandingPad(TpdeModuleRef m, TpdeValueRef typeinfo) {
	return inst(*m, {.op = Op::LandingPad, .ops = push(m->opList, {u32(typeinfo)})});
}

const char *TpdeVerifyModule(TpdeModuleRef m) {
	group(*m);
	m->msg = m->error;
	for (const Sym &s : m->syms) {
		auto fail = [&](const std::string &what) { m->msg = m->msg.empty() ? what + " in " + s.name : m->msg; };
		for (u32 b : s.removed ? std::span<const u32>{} : m->blocksOf(s)) {
			std::span<const u32> insts = m->insts(b);
			if (insts.empty() || !terminates(m->vals[insts.back()].op)) {
				fail("a block does not end with a terminator");
			}
			for (size_t i = 0; i < insts.size(); i++) {
				const Value &v = m->vals[insts[i]];
				if (i + 1 < insts.size() && terminates(v.op)) {
					fail("a terminator is not at the end of its block");
				}
				if (v.op == Op::LandingPad && (i > 0 || s.personality == 0)) {
					fail("a landing pad is not first in its block, or has no personality");
				}
				if (v.op == Op::Invoke) {
					std::span<const u32> pad = m->insts(m->succs(b)[1]);
					if (pad.empty() || m->vals[pad[0]].op != Op::LandingPad) {
						fail("an invoke does not unwind to a landing pad");
					}
				}
			}
		}
	}
	return m->msg.empty() ? nullptr : m->msg.c_str();
}

void TpdeSetCollected(TpdeModuleRef m) { m->collected = true; }
int TpdeHoldsRef(TpdeModuleRef m, TpdeTypeRef t) { return std::ranges::any_of(m->parts(m->types[t]), &Part::ref); }
TpdeValueRef TpdeLayout(TpdeModuleRef m, TpdeTypeRef t) {
	if (u32 have = m->layouts[t]) {
		return have;
	}
	const Type &ty = m->types[t];
	u32 word = type(*m, Type::Int, 64);
	std::vector<u64> bits((ty.size / 8 + 63) / 64);
	for (const Part &p : m->parts(ty)) {
		bits[p.off / 512] |= u64(p.ref) << (p.off / 8 % 64);
	}
	for (u64 &b : bits) {
		b = constant(*m, word, b);
	}
	TpdeValueRef fields[] = {constant(*m, word, ty.size), TpdeConstArray(m, word, bits.data(), bits.size())};
	return m->layouts[t] = TpdeAddGlobal(m, "", TpdeConstStruct(m, fields, 2, 0), 1, 0);
}
void TpdeSetSection(TpdeModuleRef m, TpdeValueRef g, const char *name) { m->sym(g).section = name; }

const char *TpdeEmitToFile(TpdeModuleRef m, const char *path) {
	if (const char *err = TpdeVerifyModule(m)) {
		return err;
	}
	std::vector<u8> obj;
	if (!compile(*m, obj) || !m->error.empty()) {
		m->msg = m->error.empty() ? "TPDE could not compile the module" : m->error;
		return m->msg.c_str();
	}
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char *>(obj.data()), obj.size());
	m->msg = "cannot write " + std::string(path);
	return f ? nullptr : m->msg.c_str();
}

} // extern "C"
