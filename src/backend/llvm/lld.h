// LLD's ELF and Mach-O linkers as a C function, for bober code to link in its own process.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Links as the linker args[0] names, ld.lld or ld64.lld, does with the n arguments args; its errors go to stderr.
/// Returns the exit status that linker would.
int bober_lld(int n, const char **args);

#ifdef __cplusplus
}
#endif
