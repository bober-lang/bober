#include <math.h>
#include "../cases.h"

const struct float_case fast_float_json_fmt[] = {
    {S("-0.2"), F64, OK, 4, -0.2},
    {S("0.02"), F64, OK, 4, 0.02},
    {S("0.002"), F64, OK, 5, 0.002},
    {S("1e+0000"), F64, OK, 7, 1.},
    {S("0e-2"), F64, OK, 4, 0.},
    {S("1e"), F64, OK, 1, 1.},
    {S("1e+"), F64, OK, 1, 1.},
    {S("inf"), F64, OK, 3, INFINITY},
    {S("-.2"), F64, SYNTAX, 1, 0},
    {S("00.02"), F64, OK, 5, 0.02},
    {S("0.e+1"), F64, OK, 1, 0.},
    {S("00.e+1"), F64, OK, 2, 0.},
    {S(".25"), F64, SYNTAX, 0, 0},
    {S("+0.25"), F64, SYNTAX, 0, 0},
    {S("nan(snan)"), F64, OK, 3, NAN},
    {S("+.1"), F64, SYNTAX, 0, 0},
};
