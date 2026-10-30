#include <stdint.h>

enum { OK, SYNTAX, RANGE };
enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

struct int_case {
  const void *in;
  int32_t len;
  uint8_t unit;
  uint8_t type;
  uint8_t radix;
  uint8_t err;
  int32_t at;
  uint64_t value;
};

struct float_case {
  const void *in;
  int32_t len;
  uint8_t unit;
  uint8_t type;
  uint8_t err;
  int32_t at;
  double value;
};

#define S(s) s, sizeof(s) - 1, 1
#define U16(s) s, sizeof(s) / 2 - 1, 2
#define U32(s) s, sizeof(s) / 4 - 1, 4
