//! lwIP's port to the run time: the C library's types, and failed assertions abort.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LWIP_PLATFORM_DIAG(x) (void)0
#define LWIP_PLATFORM_ASSERT(x) (fprintf(stderr, "lwip: %s\n", x), abort())
#define LWIP_NO_CTYPE_H 1
