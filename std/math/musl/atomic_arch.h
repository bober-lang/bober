// musl's fma.c includes atomic.h only for its portable a_clz_64; atomic.h wants an a_cas and a_cas_p, never called,
// and its a_crash, a store to address 0, crashes bobcc.
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s) { return *p; }
#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s) { return t; }
#define a_crash a_crash
static inline void a_crash(void) {}
