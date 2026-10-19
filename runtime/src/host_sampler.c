/* host_sampler.c - host CPU sampler for the emulation thread
 * (debug-tools builds; TCP host_profile, tools/host_profile.py).
 *
 * A background thread samples the emulation thread's instruction pointer
 * every millisecond into a ring, tagged with the guest frame and whether a
 * render pass was running. The ring covers the last ~65 seconds, so a probe
 * asks for a window that already happened instead of arming a profiler
 * before a workload. Addresses are stored relative to the executable's load
 * base; tools/host_profile.py maps them to functions with the image's own
 * symbol table (nm), so no debug info is needed.
 *
 * Windows: explicit diagnostic opt-in (PSX_HOST_SAMPLER_SUSPEND=1) only.
 * SuspendThread / GetThreadContext / ResumeThread can hold the emulation
 * thread indefinitely if the sampler itself is descheduled, so this must
 * never run as part of normal gameplay. POSIX: the process CPU-time timer
 * (SIGPROF) delivered to the
 * emulation thread, which reads its own interrupted PC from the ucontext.
 * Other hosts, and builds without debug tools, report the sampler as
 * unsupported. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* REG_RIP, dladdr */
#endif
#include "host_sampler.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>

extern int g_psx_render_pass_active;
extern uint64_t s_frame_count;

static HostSample s_ring[HOST_SAMPLER_CAP];
static volatile uint64_t s_seq;
static uintptr_t s_image_base;
static int s_supported;
static _Atomic uint64_t s_max_suspend_us;
static _Atomic uint64_t s_max_suspend_wait_us;
static _Atomic uint64_t s_max_suspend_hold_us;
static _Atomic uint64_t s_worst_hold_target_cpu_us;  /* CPU used during that hold */

#ifndef PSX_NO_DEBUG_TOOLS
static void record(uintptr_t pc) {
    const uint64_t n = s_seq;
    HostSample *e = &s_ring[n % HOST_SAMPLER_CAP];
    e->rva = (pc >= s_image_base) ? (uint64_t)(pc - s_image_base) : 0;
    e->frame = (uint32_t)s_frame_count;
    e->in_pass = g_psx_render_pass_active ? 1u : 0u;
    s_seq = n + 1;
}
#endif

#if defined(PSX_NO_DEBUG_TOOLS)
/* Production builds carry no sampler: it reports unsupported. */
void host_sampler_start(void) {}

#elif defined(_WIN32)
#include <windows.h>

static HANDLE s_target;
static LARGE_INTEGER s_counter_frequency;
static _Atomic uint64_t s_suspend_begin_us;   /* 0 = not suspending */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Microseconds from QPC. Frequency is set once in host_sampler_start before
 * the sampler thread exists. */
static uint64_t qpc_us_now(void) {
    LARGE_INTEGER c;
    const uint64_t f = (uint64_t)s_counter_frequency.QuadPart;
    if (!f || !QueryPerformanceCounter(&c)) return 0;
    const uint64_t t = (uint64_t)c.QuadPart;
    return t / f * 1000000u + t % f * 1000000u / f;
}

static void publish_max(_Atomic uint64_t *slot, uint64_t value) {
    if (value > atomic_load_explicit(slot, memory_order_relaxed))
        atomic_store_explicit(slot, value, memory_order_relaxed);
}

/* Kernel+user CPU microseconds consumed by the target so far. This is the
 * discriminator between "the sampler held the target suspended" (CPU time
 * flat across the hold) and "the target was inside a long kernel operation"
 * (CPU time advanced by about the hold duration). */
static uint64_t thread_cpu_us(HANDLE h) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(h, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) / 10u;   /* 100 ns -> us */
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    /* A 1 ms period needs a high-resolution timer; Sleep(1) would follow
     * whatever timer resolution the process last asked for. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms, relative */
        if (!SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
            CloseHandle(timer);
            timer = NULL;
        }
    }
    for (;;) {
        if (timer) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        const uint64_t t_before = qpc_us_now();
        /* Sentinel 1: a QPC failure must not read as "not suspending". */
        atomic_store_explicit(&s_suspend_begin_us, t_before ? t_before : 1u,
                              memory_order_relaxed);
        const uint64_t cpu_before = thread_cpu_us(s_target);
        if (SuspendThread(s_target) == (DWORD)-1) {
            atomic_store_explicit(&s_suspend_begin_us, 0, memory_order_relaxed);
            continue;
        }
        const uint64_t t_suspended = qpc_us_now();
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_target, &ctx)) {
#if defined(_M_X64) || defined(__x86_64__)
            record((uintptr_t)ctx.Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
            record((uintptr_t)ctx.Pc);
#endif
        }
        const uint64_t t_done = qpc_us_now();
        const uint64_t cpu_after = thread_cpu_us(s_target);
        /* Publish before ResumeThread: the target can immediately trip its
         * watchdog after a long host pause. No allocation/locks while stopped.
         * Splitting wait from hold tells a slow target kernel operation (wait)
         * from a delayed sampler thread (hold) without guessing; CPU time on
         * the worst hold then separates "target was busy" from "target was
         * suspended by us". */
        if (t_before && t_suspended && t_done) {
            const uint64_t wait = t_suspended - t_before;
            const uint64_t hold = t_done - t_suspended;
            const uint64_t total = t_done - t_before;
            publish_max(&s_max_suspend_wait_us, wait);
            publish_max(&s_max_suspend_us, total);
            if (hold > atomic_load_explicit(&s_max_suspend_hold_us,
                                            memory_order_relaxed)) {
                const uint64_t cpu = cpu_after > cpu_before
                                         ? cpu_after - cpu_before : 0;
                atomic_store_explicit(&s_max_suspend_hold_us, hold,
                                      memory_order_relaxed);
                atomic_store_explicit(&s_worst_hold_target_cpu_us, cpu,
                                      memory_order_relaxed);
            }
        }
        ResumeThread(s_target);
        atomic_store_explicit(&s_suspend_begin_us, 0, memory_order_relaxed);
    }
    return 0;
}

void host_sampler_start(void) {
    if (s_supported) return;
    /* Suspending another thread has no bounded latency guarantee. The ring is
     * a diagnostic, not part of emulation: do not create the suspending thread
     * at all unless the operator explicitly requested that diagnostic. Keep
     * the starvation watchdog independent and fully enabled. */
    const char *opt_in = getenv("PSX_HOST_SAMPLER_SUSPEND");
    if (!opt_in || strcmp(opt_in, "1") != 0) return;
    if (!QueryPerformanceFrequency(&s_counter_frequency) ||
        s_counter_frequency.QuadPart <= 0) return;
    s_image_base = (uintptr_t)GetModuleHandleW(NULL);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    HANDLE t = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!t) {
        CloseHandle(s_target);
        s_target = NULL;
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(t);
    s_supported = 1;
}

#elif defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>   /* <ucontext.h> errors without _XOPEN_SOURCE */
#else
#include <ucontext.h>
#endif
#include <pthread.h>
#include <dlfcn.h>

static pthread_t s_thread;

#if defined(__APPLE__)
/* The emulation thread's stack, [lo, hi). SIGPROF can land on a thread that
 * is still being created; pthread_self() there traps (arm64e pointer
 * authentication), so the handler tells threads apart by the interrupted
 * stack pointer instead, which needs no thread state at all. */
static uintptr_t s_stack_lo, s_stack_hi;
#endif

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
#if defined(__APPLE__)
    if (!uc || !uc->uc_mcontext) return;
#if defined(__aarch64__)
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
#else
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
#endif
    if (sp < s_stack_lo || sp >= s_stack_hi) return;
#else
    if (!pthread_equal(pthread_self(), s_thread)) return;
#endif
#if defined(__linux__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext.pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__)
    record((uintptr_t)uc->uc_mcontext->__ss.__pc);
#else
    (void)uc;
#endif
}

void host_sampler_start(void) {
    if (s_supported) return;
    Dl_info info;
    if (dladdr((void *)&host_sampler_start, &info) && info.dli_fbase)
        s_image_base = (uintptr_t)info.dli_fbase;
    s_thread = pthread_self();
#if defined(__APPLE__)
    s_stack_hi = (uintptr_t)pthread_get_stackaddr_np(s_thread);
    s_stack_lo = s_stack_hi - (uintptr_t)pthread_get_stacksize_np(s_thread);
#endif
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0) return;
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0) return;
    s_supported = 1;
}

#else
void host_sampler_start(void) {}
#endif

int host_sampler_supported(void) { return s_supported; }

uint64_t host_sampler_max_suspend_us(void) {
    return atomic_load_explicit(&s_max_suspend_us, memory_order_relaxed);
}

uint64_t host_sampler_max_suspend_wait_us(void) {
    return atomic_load_explicit(&s_max_suspend_wait_us, memory_order_relaxed);
}

uint64_t host_sampler_max_suspend_hold_us(void) {
    return atomic_load_explicit(&s_max_suspend_hold_us, memory_order_relaxed);
}

uint64_t host_sampler_worst_hold_target_cpu_us(void) {
    return atomic_load_explicit(&s_worst_hold_target_cpu_us,
                                memory_order_relaxed);
}

uint64_t host_sampler_suspend_age_us(void) {
#if defined(_WIN32) && !defined(PSX_NO_DEBUG_TOOLS)
    const uint64_t begin = atomic_load_explicit(&s_suspend_begin_us,
                                                memory_order_relaxed);
    return host_suspend_age_us_from(begin, qpc_us_now());
#else
    return 0;
#endif
}

uint64_t host_sampler_seq(void) { return s_seq; }

uint64_t host_sampler_image_base(void) { return (uint64_t)s_image_base; }

int host_sampler_get(uint64_t seq, HostSample *out) {
    const uint64_t now = s_seq;
    if (seq >= now || now - seq > HOST_SAMPLER_CAP) return 0;
    *out = s_ring[seq % HOST_SAMPLER_CAP];
    return 1;
}
