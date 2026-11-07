// TPDE as a code generator behind a C API shaped like the part of LLVM-C that src/backend/llvm.bob uses: a module holds
// types, constants, functions, globals and blocks, all handles of its own, 0 for none, and appends instructions at
// the block it is positioned at. Nothing is checked as it is built; the first misuse or unsupported operation is
// kept and reported by TpdeVerifyModule and TpdeEmitToFile, which return a message, or NULL when all is well.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TpdeModule *TpdeModuleRef;
typedef uint64_t TpdeTypeRef;
typedef uint64_t TpdeValueRef;
typedef uint64_t TpdeBasicBlockRef;

typedef enum {
	TpdeAdd, TpdeSub, TpdeMul, TpdeUDiv, TpdeSDiv, TpdeURem, TpdeSRem, TpdeShl, TpdeLShr, TpdeAShr, TpdeAnd, TpdeOr,
	TpdeXor, TpdeFAdd, TpdeFSub, TpdeFMul, TpdeFDiv, TpdeFRem,
	TpdeTrunc, TpdeZExt, TpdeSExt, TpdeFPToUI, TpdeFPToSI, TpdeUIToFP, TpdeSIToFP, TpdeFPTrunc, TpdeFPExt, TpdePtrToInt,
	TpdeIntToPtr, TpdeBitCast,
} TpdeOpcode;

typedef enum { TpdeFNeg, TpdeFAbs, TpdeBSwap, TpdeCtlz, TpdeCttz, TpdeCtpop } TpdeUnaryOp;

typedef enum {
	TpdeIntEQ, TpdeIntNE, TpdeIntUGT, TpdeIntUGE, TpdeIntULT, TpdeIntULE, TpdeIntSGT, TpdeIntSGE, TpdeIntSLT,
	TpdeIntSLE,
} TpdeIntPredicate;

typedef enum {
	TpdeRealOEQ, TpdeRealOGT, TpdeRealOGE, TpdeRealOLT, TpdeRealOLE, TpdeRealONE, TpdeRealORD, TpdeRealUEQ, TpdeRealUGT,
	TpdeRealUGE, TpdeRealULT, TpdeRealULE, TpdeRealUNE, TpdeRealUNO,
} TpdeRealPredicate;

TpdeModuleRef TpdeModuleCreate(void);
void TpdeModuleDispose(TpdeModuleRef m);

TpdeTypeRef TpdeVoidType(TpdeModuleRef m);
TpdeTypeRef TpdeIntType(TpdeModuleRef m, unsigned bits);
TpdeTypeRef TpdeFloatType(TpdeModuleRef m);
TpdeTypeRef TpdeDoubleType(TpdeModuleRef m);
TpdeTypeRef TpdePointerType(TpdeModuleRef m);
/// A pointer the collector knows (src/rt.c), which a collected module keeps where its stack maps say across calls.
TpdeTypeRef TpdeRefType(TpdeModuleRef m);
TpdeTypeRef TpdeFunctionType(TpdeModuleRef m, TpdeTypeRef result, TpdeTypeRef *params, unsigned n, int variadic);
TpdeTypeRef TpdeStructType(TpdeModuleRef m, TpdeTypeRef *fields, unsigned n, int packed);
TpdeTypeRef TpdeArrayType(TpdeModuleRef m, TpdeTypeRef elem, uint64_t n);

TpdeValueRef TpdeConstInt(TpdeModuleRef m, TpdeTypeRef t, uint64_t n);
TpdeValueRef TpdeConstReal(TpdeModuleRef m, TpdeTypeRef t, double x);
TpdeValueRef TpdeConstIntOfString(TpdeModuleRef m, TpdeTypeRef t, const char *digits, uint8_t radix);
TpdeValueRef TpdeConstRealOfString(TpdeModuleRef m, TpdeTypeRef t, const char *text);
TpdeValueRef TpdeConstNull(TpdeModuleRef m, TpdeTypeRef t);
TpdeValueRef TpdeConstString(TpdeModuleRef m, const char *bytes, uint64_t n);
TpdeValueRef TpdeConstStruct(TpdeModuleRef m, TpdeValueRef *fields, unsigned n, int packed);
TpdeValueRef TpdeConstArray(TpdeModuleRef m, TpdeTypeRef elem, TpdeValueRef *elems, uint64_t n);
TpdeValueRef TpdeSizeOf(TpdeModuleRef m, TpdeTypeRef t);
uint64_t TpdeABISizeOfType(TpdeModuleRef m, TpdeTypeRef t);
/// The constant address bytes past the constant one p.
TpdeValueRef TpdeConstGEP(TpdeModuleRef m, TpdeValueRef p, int64_t bytes);
/// The constant v converted by op, which is TpdeTrunc, TpdePtrToInt, TpdeIntToPtr or TpdeBitCast.
TpdeValueRef TpdeConstCast(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef v, TpdeTypeRef to);

TpdeValueRef TpdeAddFunction(TpdeModuleRef m, const char *name, TpdeTypeRef t, int local);
TpdeValueRef TpdeGetNamedFunction(TpdeModuleRef m, const char *name);
TpdeValueRef TpdeGetNamedGlobal(TpdeModuleRef m, const char *name);
/// A global holding init, aligned to align bytes or as its type wants for 0; an empty name is a private constant.
TpdeValueRef TpdeAddGlobal(TpdeModuleRef m, const char *name, TpdeValueRef init, int local, unsigned align);
/// A global of type t defined elsewhere.
TpdeValueRef TpdeAddExternal(TpdeModuleRef m, const char *name, TpdeTypeRef t);
/// Attributes as LLVM names them; byval, sret, zeroext and signext change the code, others are ignored.
void TpdeAddAttributeAtIndex(TpdeModuleRef m, TpdeValueRef fn, int index, const char *name, TpdeTypeRef t);
int TpdeIsDeclaration(TpdeModuleRef m, TpdeValueRef g);
/// Puts the function or global by in old's place, with its uses and name, and deletes old.
void TpdeReplace(TpdeModuleRef m, TpdeValueRef old, TpdeValueRef by);
void TpdeDeleteFunction(TpdeModuleRef m, TpdeValueRef fn);
void TpdeSetWeak(TpdeModuleRef m, TpdeValueRef g);
void TpdeSetHidden(TpdeModuleRef m, TpdeValueRef g);
/// Puts the global in read-only data.
void TpdeSetConstant(TpdeModuleRef m, TpdeValueRef g);
/// Leaves the function's body out of the object; its symbol is defined elsewhere.
void TpdeSetInlineOnly(TpdeModuleRef m, TpdeValueRef fn);
void TpdeSetPersonalityFn(TpdeModuleRef m, TpdeValueRef fn, TpdeValueRef p);
TpdeValueRef TpdeGetParam(TpdeModuleRef m, TpdeValueRef fn, unsigned i);
TpdeTypeRef TpdeTypeOf(TpdeModuleRef m, TpdeValueRef v);
TpdeTypeRef TpdeGetReturnType(TpdeModuleRef m, TpdeTypeRef fnType);
TpdeTypeRef TpdeGlobalGetValueType(TpdeModuleRef m, TpdeValueRef g);

TpdeBasicBlockRef TpdeAppendBasicBlock(TpdeModuleRef m, TpdeValueRef fn);
void TpdePositionAtEnd(TpdeModuleRef m, TpdeBasicBlockRef b);
TpdeBasicBlockRef TpdeGetInsertBlock(TpdeModuleRef m);
TpdeValueRef TpdeGetBasicBlockTerminator(TpdeModuleRef m, TpdeBasicBlockRef b);

TpdeValueRef TpdeBuildRet(TpdeModuleRef m, TpdeValueRef v);
TpdeValueRef TpdeBuildRetVoid(TpdeModuleRef m);
TpdeValueRef TpdeBuildBr(TpdeModuleRef m, TpdeBasicBlockRef to);
TpdeValueRef TpdeBuildCondBr(TpdeModuleRef m, TpdeValueRef cond, TpdeBasicBlockRef yes, TpdeBasicBlockRef no);
/// Jumps to targets[k] if v is cases[k], or to otherwise.
TpdeValueRef TpdeBuildSwitch(TpdeModuleRef m, TpdeValueRef v, int64_t *cases, TpdeBasicBlockRef *targets, unsigned n,
	TpdeBasicBlockRef otherwise);
TpdeValueRef TpdeBuildUnreachable(TpdeModuleRef m);
TpdeValueRef TpdeBuildBinOp(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef a, TpdeValueRef b);
TpdeValueRef TpdeBuildICmp(TpdeModuleRef m, TpdeIntPredicate pred, TpdeValueRef a, TpdeValueRef b);
TpdeValueRef TpdeBuildFCmp(TpdeModuleRef m, TpdeRealPredicate pred, TpdeValueRef a, TpdeValueRef b);
TpdeValueRef TpdeBuildCast(TpdeModuleRef m, TpdeOpcode op, TpdeValueRef v, TpdeTypeRef to);
TpdeValueRef TpdeBuildSelect(TpdeModuleRef m, TpdeValueRef cond, TpdeValueRef yes, TpdeValueRef no);
/// A stack slot of type t; wherever it is built, it is allocated once on entry.
TpdeValueRef TpdeBuildAlloca(TpdeModuleRef m, TpdeTypeRef t, unsigned align);
/// bytes, an i64, of the stack, aligned to 16, from where it is built until the function returns.
TpdeValueRef TpdeBuildStackAlloc(TpdeModuleRef m, TpdeValueRef bytes);
/// Loads and stores are never removed or merged, so volatile ones are like the others.
TpdeValueRef TpdeBuildLoad(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, int isVolatile);
TpdeValueRef TpdeBuildStore(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef p, int isVolatile);
/// Sequentially consistent, aligned to the size of an integer or pointer.
TpdeValueRef TpdeBuildAtomicLoad(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p);
TpdeValueRef TpdeBuildAtomicStore(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef p);
TpdeValueRef TpdeBuildFence(TpdeModuleRef m);
TpdeValueRef TpdeBuildTrap(TpdeModuleRef m);
/// Starts the va_list at ap for the variadic function being built.
TpdeValueRef TpdeBuildVaStart(TpdeModuleRef m, TpdeValueRef ap);
/// The frame pointer of the function being built.
TpdeValueRef TpdeBuildFrameAddress(TpdeModuleRef m);
/// ctlz and cttz take zero too; bswap, ctlz and cttz integers of 8, 16, 32 or 64 bits.
TpdeValueRef TpdeBuildUnary(TpdeModuleRef m, TpdeUnaryOp op, TpdeValueRef v);
TpdeValueRef TpdeBuildStructGEP(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, unsigned i);
TpdeValueRef TpdeBuildGEP(TpdeModuleRef m, TpdeTypeRef t, TpdeValueRef p, TpdeValueRef *indices, unsigned n);
TpdeValueRef TpdeBuildExtractValue(TpdeModuleRef m, TpdeValueRef v, unsigned i);
TpdeValueRef TpdeBuildInsertValue(TpdeModuleRef m, TpdeValueRef v, TpdeValueRef e, unsigned i);
TpdeValueRef TpdeBuildCall(TpdeModuleRef m, TpdeTypeRef fnType, TpdeValueRef fn, TpdeValueRef *args, unsigned n);
TpdeValueRef TpdeBuildInvoke(TpdeModuleRef m, TpdeTypeRef fnType, TpdeValueRef fn, TpdeValueRef *args, unsigned n,
	TpdeBasicBlockRef then, TpdeBasicBlockRef unwind);
/// Where an invoke unwinds to, catching typeinfo's C++ exceptions, or all for a null pointer; it gives no value.
TpdeValueRef TpdeBuildLandingPad(TpdeModuleRef m, TpdeValueRef typeinfo);

/// Has the collector walk the frames of the module's functions: each ref live across a call is kept in the frame, which
/// the stack maps of section .llvm_stackmaps say where, in LLVM's format, as src/gc.c reads it.
void TpdeSetCollected(TpdeModuleRef m);
int TpdeHoldsRef(TpdeModuleRef m, TpdeTypeRef t);
/// Where the refs in a value of t are, the layout bober_alloc takes (src/rt.h): a private global, made once.
TpdeValueRef TpdeLayout(TpdeModuleRef m, TpdeTypeRef t);
void TpdeSetSection(TpdeModuleRef m, TpdeValueRef g, const char *name);

const char *TpdeVerifyModule(TpdeModuleRef m);
/// Writes an object file for this host.
const char *TpdeEmitToFile(TpdeModuleRef m, const char *path);

#ifdef __cplusplus
}
#endif
