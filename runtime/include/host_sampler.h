#ifndef PSX_HOST_SAMPLER_H
#define PSX_HOST_SAMPLER_H
/* Host CPU sampler for the emulation thread (debug-tools builds).
 * Windows requires PSX_HOST_SAMPLER_SUSPEND=1: external thread suspension is
 * intrusive and may stall the target for unbounded host time. Off by default.
 * See host_sampler.c; TCP host_profile; tools/host_profile.py. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_SAMPLER_CAP 65536u   /* ~65 s at 1 kHz */

typedef struct HostSample {
    uint64_t rva;      /* instruction pointer minus the executable's base */
    uint32_t frame;    /* guest frame (s_frame_count) */
    uint32_t in_pass;  /* 1 while a render pass ran */
} HostSample;

/* Start sampling the calling thread (the emulation thread). Idempotent.
 * An unsupported or non-opted-in sampler records no samples. */
void host_sampler_start(void);
int host_sampler_supported(void);
/* Total samples recorded; the ring holds the newest HOST_SAMPLER_CAP. */
uint64_t host_sampler_seq(void);
uint64_t host_sampler_image_base(void);
/* Sample `seq`, if it is still in the ring. */
int host_sampler_get(uint64_t seq, HostSample *out);

/* Host diagnostic only: longest completed Windows suspend/context interval.
 * Zero on platforms whose sampler does not suspend the emulation thread.
 *
 * The interval is split so a stall can be attributed without guessing:
 *   wait  = SuspendThread() blocked waiting for the target to become
 *           suspendable (the target was inside a long kernel operation, e.g.
 *           the image loader or a device wait);
 *   hold  = the target was actually suspended while the sampler read its
 *           context (the sampler thread itself was delayed).
 * max_suspend_us = wait + hold + the sampler's own bookkeeping. */
uint64_t host_sampler_max_suspend_us(void);
uint64_t host_sampler_max_suspend_wait_us(void);
uint64_t host_sampler_max_suspend_hold_us(void);

/* Target CPU time (kernel+user, us) consumed during the worst hold. Compared
 * with max_suspend_hold_us: ~equal means the target was busy inside the
 * kernel (loader/I/O); ~zero means the sampler held an idle target. */
uint64_t host_sampler_worst_hold_target_cpu_us(void);

/* Pure age arithmetic, exposed for deterministic tests. begin_us == 0 means
 * "no suspension in progress"; a clock that went backwards yields 0. */
static inline uint64_t host_suspend_age_us_from(uint64_t begin_us,
                                                uint64_t now_us) {
    if (begin_us == 0) return 0;
    return now_us > begin_us ? now_us - begin_us : 0;
}

/* Live age of the suspension the sampler is currently inside (0 when the
 * target is not suspended by the sampler). A guest heartbeat written while
 * this is large is evidence of a host suspension, not guest starvation. */
uint64_t host_sampler_suspend_age_us(void);

#ifdef __cplusplus
}
#endif
#endif
