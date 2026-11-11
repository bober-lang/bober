// musl's libm, for std/math's tests. bober.pkg compiles src/math/*[!l].c, leaving out the long double functions,
// whose files end in l.c, and with them ceil.c, which it adds back.
#define _GNU_SOURCE
#include <math.h>
