// C11's atomics, which tests/atomic.bob runs.
#include <stdatomic.h>

static atomic_int n;
static _Atomic(long *) p;
static long xs[2];

/// n changed by each of stdatomic.h's functions, then by a plain load and store.
int count(void) {
	atomic_init(&n, 1);
	atomic_fetch_add(&n, 4);
	atomic_fetch_sub_explicit(&n, 1, memory_order_relaxed);
	atomic_fetch_or(&n, 3);
	atomic_fetch_and(&n, 6);
	atomic_fetch_xor(&n, 1);
	int was = atomic_exchange(&n, 10);
	int want = 9;
	if (atomic_compare_exchange_strong(&n, &want, 20) || want != 10) {
		return -1;
	}
	if (!atomic_compare_exchange_weak(&n, &want, 20)) {
		return -2;
	}
	n = n + was;
	return atomic_load(&n);
}

/// Whether a flag is set once and a pointer swapped as it should be.
int flagged(void) {
	atomic_flag f = ATOMIC_FLAG_INIT;
	atomic_store(&p, &xs[0]);
	long *old = atomic_exchange(&p, &xs[1]);
	return !atomic_flag_test_and_set(&f) && atomic_flag_test_and_set(&f) && old == &xs[0] && p == &xs[1];
}
