// musl's libm, for std/math's tests. bober.pkg compiles src/math/*[!l].c, leaving out the long double functions,
// whose files end in l.c, and with them ceil.c, which it adds back.
#include <math.h>

// What musl has beyond C's math.h, which not every system's declares: lgamma_r by the name musl defines it under, as
// lgamma_r is a weak alias of it, which Mach-O has not.
double j0(double), j1(double), jn(int, double), y0(double), y1(double), yn(int, double);
float j0f(float), j1f(float), jnf(int, float), y0f(float), y1f(float), ynf(int, float);
double __lgamma_r(double, int *);
float __lgammaf_r(float, int *);
void sincos(double, double *, double *), sincosf(float, float *, float *);
