#include "lld.h"

#include "lld/Common/Driver.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

int bober_lld(int n, const char **args) {
	std::vector<const char *> argv(args, args + n);
	return lld::lldMain(argv, llvm::outs(), llvm::errs(), {{lld::Gnu, &lld::elf::link}, {lld::Darwin, &lld::macho::link}})
		.retCode;
}

void bober_first(LLVMValueRef f) {
	auto *fn = llvm::unwrap<llvm::Function>(f);
	auto &fs = fn->getParent()->getFunctionList();
	fs.splice(fs.begin(), fs, fn->getIterator());
}
