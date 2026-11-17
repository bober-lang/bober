// LLD's ELF and Mach-O linkers as a C function, for bober code to link in its own process, and LLVM's C++ API that its
// C API lacks.
#pragma once

#include "llvm-c/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Links as the linker args[0] names, ld.lld or ld64.lld, does with the n arguments args; its errors go to stderr.
/// Returns the exit status that linker would.
int bober_lld(int n, const char **args);

/// Moves function f to the front of its module's functions.
void bober_first(LLVMValueRef f);

#ifdef __cplusplus
}
#endif
