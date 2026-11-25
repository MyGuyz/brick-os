/* The outline-atomics helpers PPSSPP's prebuilt ffmpeg (built with GCC >= 10) calls; the Brick
   toolchain's GCC 8 libgcc has none. Plain LL/SC versions (the A53 has no LSE atomics anyway). */
#include <stdint.h>

int32_t __aarch64_ldadd4_acq_rel(int32_t v, int32_t *p) {
	return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

int64_t __aarch64_cas8_acq_rel(int64_t expected, int64_t desired, int64_t *p) {
	__atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return expected;   /* the value that was in memory */
}
