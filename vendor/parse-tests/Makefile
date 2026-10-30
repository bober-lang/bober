B = ../../build

$(B)/libparsetests.dylib: cases.h $(wildcard */*.c)
	$(CC) -std=c11 -O1 -Wall -Wextra -dynamiclib -o $@ $(wildcard */*.c)
