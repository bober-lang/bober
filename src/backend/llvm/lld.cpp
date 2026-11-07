#include "lld.h"

#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

int bober_lld(int n, const char **args) {
	std::vector<const char *> argv(args, args + n);
	return lld::lldMain(argv, llvm::outs(), llvm::errs(), {{lld::Gnu, &lld::elf::link}, {lld::Darwin, &lld::macho::link}})
		.retCode;
}
