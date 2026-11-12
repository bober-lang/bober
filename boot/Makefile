B = ../build
BISON = bison
FLEX = flex
CFLAGS = -std=gnu11 -O2 -g -Wall -I. -I$(B)
# The system's LLVM, of the version of the native bober's, which Linux's ld drops unless told, as only bober code
# calls it.
LLVM = $(shell llvm-config --libdir)
ifeq ($(shell uname),Linux)
KEEP = -Wl,--no-as-needed
endif

# With the run time's C, src/io.c, exported so that bober code finds it as C functions, and the host's LLVM, which
# src/backend/llvm.bob calls.
$(B)/boot: interp.c ast.h $(B)/parse.c $(B)/lex.c ../src/io.c ../src/rt.h ../src/net.h
	$(CC) $(CFLAGS) -rdynamic -o $@ interp.c $(B)/parse.c $(B)/lex.c ../src/io.c -lm -lffi -lgc \
		-L$(LLVM) -Wl,-rpath,$(LLVM) $(KEEP) -lLLVM

$(B)/parse.c: parse.y ast.h
	@mkdir -p $(B)
	$(BISON) -Wall -Werror -o $@ --header=$(B)/parse.h parse.y

$(B)/lex.c: lex.l ast.h $(B)/parse.c
	$(FLEX) -o $@ lex.l
