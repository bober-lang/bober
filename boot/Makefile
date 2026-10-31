B = ../build
BISON = bison
FLEX = flex
CFLAGS = -std=gnu11 -O2 -g -Wall -I. -I$(B)
LLVM = $(shell llvm-config --libdir)

# With the run time's C, src/io.c and std/crypto's vendored C, exported so that bober code finds it as C functions, and
# the host's LLVM, which src/backend/llvm.bob calls.
$(B)/bober: interp.c ast.h $(B)/parse.c $(B)/lex.c ../src/io.c ../src/rt.h $(B)/crypto.a
	$(CC) $(CFLAGS) -rdynamic -o $@ interp.c $(B)/parse.c $(B)/lex.c ../src/io.c \
		-Wl,--whole-archive $(B)/crypto.a -Wl,--no-whole-archive -lm -lffi \
		-L$(LLVM) -Wl,-rpath,$(LLVM),--no-as-needed -lLLVM

$(B)/crypto.a: FORCE
	$(MAKE) -s -C .. -f std/crypto/Makefile

FORCE:

$(B)/parse.c: parse.y ast.h
	@mkdir -p $(B)
	$(BISON) -Wall -Werror -o $@ --header=$(B)/parse.h parse.y

$(B)/lex.c: lex.l ast.h $(B)/parse.c
	$(FLEX) -o $@ lex.l
