// C calling bober's functions tagged @C, which tests/callback.bob runs.
#include <stdbool.h>
#include <string.h>

typedef struct { int x, y; } point;
typedef struct { point from; point to; double weight; } edge;

long cb_sum(int a, long b, unsigned char c);
bool cb_odd(int n);
double cb_length(edge *e);
point cb_mid(edge *e);
const char *cb_unit(int n);
int cb_scale(const char *unit);

/// What bober's functions give, called from C.
long numbers(void) { return cb_sum(-2, 5000000000, 200) + (cb_odd(3) && !cb_odd(4)); }

/// The length of an edge, which bober reads and doubles.
double length(void) {
	edge e = {{1, 2}, {4, 6}, 0.5};
	double l = cb_length(&e);
	return l + e.weight;
}

/// The middle of an edge, a struct bober returns.
int middle(void) {
	edge e = {{0, 0}, {4, 8}, 1};
	point m = cb_mid(&e);
	return m.x * 100 + m.y;
}

/// Literals: a string bober gives, the same pointer each time, and one C gives.
int units(void) {
	const char *m = cb_unit(1);
	return strcmp(m, "m") == 0 && m == cb_unit(1) && strcmp(cb_unit(2), "kilometre") == 0
		? cb_scale("cm") + cb_scale(cb_unit(2)) : -1;
}
