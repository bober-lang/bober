// The module tpde.h builds: types, values and blocks in vectors of small fixed records, their handles the indices,
// 0 for none. What varies in length, operands, type elements, parts and bytes, lives in pools the records point
// into, so building allocates only when a pool grows. Instructions are values too, numbered within their function
// so TPDE can keep per-value state in arrays, and kept in the order built; group sorts them into their blocks.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bober {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

/// The n entries from at in one of the module's pools.
struct Run {
	u32 at = 0, n = 0;
};

/// A register-sized piece of a value at offset off in its memory; aggregates have one per scalar leaf. A ref's piece
/// is one the collector finds and moves.
struct Part {
	u32 off;
	u8 size, bits;
	bool fp, ref;
};

struct Type {
	enum Kind : u8 { Void, Int, F32, F64, LongDouble, Ptr, Fn, Struct, Array } kind;
	/// The int's bits, the array's length, or 1 for a ref, a variadic function or a packed struct.
	u64 n = 0;
	u64 size = 0;
	u32 align = 1;
	/// Struct fields, array element, or function result and then params, in Module::elemList.
	Run elems;
	/// Where the byte offsets of struct fields start in Module::nums.
	u32 offs = 0;
	/// In Module::partList.
	Run parts;
};

enum class Op : u8 {
	// Constants. Function, Global and Addr are addresses, which only the linker knows.
	Const, Zero, Bytes, Agg, Addr, Function, Global,
	Param,
	// Instructions; block targets are their block's successors.
	Ret, Br, CondBr, Switch, Unreachable, Binary, ICmp, FCmp, Cast, Select, Alloca, StackAlloc, Load, Store, Offset, Extract,
	Insert, Unary, Fence, Trap, VaStart, FrameAddress, Call, Invoke, LandingPad,
};

/// 32 bytes.
struct Value {
	Op op;
	/// The opcode or predicate of Binary, Unary, Cast and the compares; 1 for an atomic Load or Store; the log2 of
	/// Alloca's align.
	u8 sub = 0;
	u32 type = 0;
	/// The index among its function's parameters and instructions, or for an address among the module's addresses.
	u32 local = 0;
	/// The block an instruction was built in.
	u32 block = 0;
	/// Const's bits, Addr's byte offset, Alloca's type, Extract's and Insert's index, Function's and Global's index
	/// in Module::syms, Call's and Invoke's function type, or where Bytes' bytes start in Module::bytes, and
	/// Offset's byte offset, then a scale per index, and Switch's cases in Module::nums.
	u64 imm = 0;
	/// In Module::opList: Agg's elements, Addr's base, Offset's base and indices, Call's callee and args,
	/// LandingPad's typeinfo, and the instructions' operands.
	Run ops;
};

/// The attributes that change the code; the others are dropped.
enum class AttrKind : u8 { ByVal, SRet, ZeroExt, SignExt };

/// An attribute of the function or call val, on its result for index 0, else on parameter index - 1.
struct Attr {
	u32 val;
	int index;
	AttrKind kind;
	u32 type;
};

/// A function or global variable.
struct Sym {
	/// Its value, which a function's parameters follow.
	u32 val;
	std::string name;
	bool local = false, constant = false, removed = false, weak = false, hidden = false, inlineOnly = false;
	/// A global's initializer, or 0 if declared only.
	u32 init = 0;
	/// A global's section, or "" for the one its kind goes to.
	std::string section;
	u32 align = 0;
	/// The function's type, or the global's value type.
	u32 type = 0;
	/// In Module::blockList, once grouped; n counts them as they are appended.
	Run blocks;
	u32 personality = 0;
	/// Parameters and instructions so far.
	u32 nlocal = 0;
};

/// 32 bytes.
struct Block {
	u32 fn;
	/// The instruction built last, so the terminator once there is one.
	u32 last = 0;
	/// In Module::instList, once grouped.
	Run insts;
	/// The targets of its terminator, in Module::succList.
	Run succs;
	/// Kept for TPDE.
	u32 info = 0, info2 = 0;
};

struct Module {
	std::vector<Type> types{1};
	std::vector<Value> vals{1};
	std::vector<Block> blocks{1};
	std::vector<Sym> syms;
	/// The pools records point into.
	std::vector<u32> opList, elemList, succList, instList, blockList;
	std::vector<Part> partList;
	std::vector<u64> nums;
	std::string bytes;
	/// The address constants, which TPDE numbers after each function's own values.
	std::vector<u32> addrs;
	/// Type ids by a hash of kind, n and elements; a colliding type takes the next free hash.
	std::unordered_map<u64, u32> typeIds;
	std::unordered_map<std::string, u32> names;
	/// Sorted by val once grouped.
	std::vector<Attr> attrs;
	u32 cur = 0;
	/// Whether the collector walks the frames of its functions, which then keep the refs live across a call in the
	/// frame, where the stack maps say.
	bool collected = false;
	/// The layout global made for each type.
	std::unordered_map<u32, u32> layouts;
	/// The first thing that went wrong.
	std::string error;

	std::span<u32> ops(u32 v) { return {opList.data() + vals[v].ops.at, vals[v].ops.n}; }
	std::span<const u32> elems(const Type &t) const { return {elemList.data() + t.elems.at, t.elems.n}; }
	std::span<const Part> parts(const Type &t) const { return {partList.data() + t.parts.at, t.parts.n}; }
	std::span<const u32> succs(u32 b) const { return {succList.data() + blocks[b].succs.at, blocks[b].succs.n}; }
	std::span<const u32> insts(u32 b) const { return {instList.data() + blocks[b].insts.at, blocks[b].insts.n}; }
	std::span<const u32> blocksOf(const Sym &s) const { return {blockList.data() + s.blocks.at, s.blocks.n}; }
	Sym &sym(u32 v) { return syms[vals[v].imm]; }
	/// Whether the function or global v is defined in this object.
	bool defined(u32 v) { return sym(v).init != 0 || (sym(v).blocks.n != 0 && !sym(v).inlineOnly); }
	bool isConst(u32 v) const { return vals[v].op <= Op::Global; }
	bool isAddr(u32 v) const { return vals[v].op >= Op::Addr && vals[v].op <= Op::Global; }
	/// The attribute kind on index of the function or call v, or null.
	const Attr *attr(u32 v, int index, AttrKind kind) const {
		auto it = std::lower_bound(attrs.begin(), attrs.end(), v, [](const Attr &a, u32 v) { return a.val < v; });
		for (; it != attrs.end() && it->val == v; it++) {
			if (it->index == index && it->kind == kind) {
				return &*it;
			}
		}
		return nullptr;
	}
	/// Keeps msg if it is the first error; returns 0 to stand for the value that could not be made.
	u32 fail(const std::string &msg) {
		if (error.empty()) {
			error = msg;
		}
		return 0;
	}
};

/// Sorts the instructions into their blocks, the blocks into their functions, and the attributes by value.
void group(Module &m);

/// Compiles m for the host, in x64.cpp or arm64.cpp, into an ELF object; false if TPDE could not.
bool compile(Module &m, std::vector<u8> &out);

} // namespace bober
