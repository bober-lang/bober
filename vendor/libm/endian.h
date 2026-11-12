// musl's libm.h includes endian.h for the byte order, and through it features.h for hidden: not every system has one.
// musl.h is here for musl's sources, which call what of a system's headers only it declares.
#include <features.h>
#include "musl.h"
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
