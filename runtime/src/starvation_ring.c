/* starvation_ring.c — diagnostic-only.
 *
 * Always-on ring of the last 16K SIO/MMIO events with full SIO state
 * snapshot at each event. Watchdog flushes the ring to a file and
 * aborts cleanly when host wall-clock since the last
 * debug_server_poll exceeds the threshold (TCP listener has stopped
 * draining the connection — symptom of dispatch-loop starvation).
 *
 * The dumped file is plain JSON-lines (one event per line) so it can
 * be inspected with grep/jq even after the process terminated. */

#include "starvation_ring.h"
#include "psx_bss.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "crash_trace.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t host_us_now(void) {
    LARGE_INTEGER c;
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / freq.QuadPart);
}
#else
#include <sys/time.h>
static uint64_t host_us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint32_t i_stat;
extern uint32_t i_mask;
extern int psx_get_in_exception(void);

const char *starvation_watchdog_host_name(StarvationHostSection section) {
    static const char *names[SW_HOST_SECTION_COUNT] = {
        "debug_poll", "frame_record", "autocapture", "provider_poll", "audio",
        "mod_hooks", "widescreen", "image_capture"
    };
    return (unsigned)section < SW_HOST_SECTION_COUNT ? names[section] : "unknown";
}

#if STARVATION_RING_ENABLED

static PSX_BSS StarvationEntry s_ring[STARVATION_RING_CAP];
static uint64_t        s_seq = 0;
/* Written by the emu thread AND the debug-server IO thread
 * (send_all_blocking); read by the emu thread's watchdog. Atomic so the
 * cross-thread store/load is single-copy atomic on every target. */
static _Atomic uint64_t s_last_heartbeat_us = 0;
static int             s_dump_done = 0;
static StarvationWatchdogGap s_max_gap;
static uint64_t s_previous_check_us, s_previous_cpu_us, s_previous_guest_cycles;
static uint32_t s_previous_func;
static int s_previous_cpu_valid;
static StarvationHostTiming s_host_timing[SW_HOST_SECTION_COUNT];

uint64_t starvation_watchdog_host_begin(void) { return host_us_now(); }
void starvation_watchdog_host_end(StarvationHostSection section, uint64_t begin) {
    if ((unsigned)section >= SW_HOST_SECTION_COUNT || !begin) return;
    uint64_t end = host_us_now();
    uint64_t elapsed = end >= begin ? end - begin : 0;
    StarvationHostTiming *s = &s_host_timing[section];
    s->calls++;
    s->total_us += elapsed;
    if (elapsed > s->max_us) s->max_us = elapsed;
}
void starvation_watchdog_host_timing(StarvationHostSection section, StarvationHostTiming *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if ((unsigned)section < SW_HOST_SECTION_COUNT) *out = s_host_timing[section];
}

static int thread_cpu_us_now(uint64_t *out) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    *out = (k.QuadPart + u.QuadPart) / 10u;
    return 1;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    *out = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return 1;
#else
    (void)out;
    return 0;
#endif
}

static void record_check_gap(uint64_t now) {
    uint64_t cpu = 0, guest = psx_get_cycle_count();
    int valid = thread_cpu_us_now(&cpu);
    if (s_previous_check_us && now > s_previous_check_us &&
        now - s_previous_check_us > s_max_gap.wall_us) {
        s_max_gap.wall_us = now - s_previous_check_us;
        s_max_gap.cpu_time_available = valid && s_previous_cpu_valid &&
                                      cpu >= s_previous_cpu_us;
        s_max_gap.cpu_us = s_max_gap.cpu_time_available ? cpu - s_previous_cpu_us : 0;
        /* A state restore can rewind guest time; never wrap that subtraction. */
        s_max_gap.guest_cycles = guest >= s_previous_guest_cycles ?
                                guest - s_previous_guest_cycles : 0;
        s_max_gap.func_before = s_previous_func;
        s_max_gap.func_after = g_debug_current_func_addr;
    }
    s_previous_check_us = now;
    s_previous_cpu_us = cpu;
    s_previous_guest_cycles = guest;
    s_previous_cpu_valid = valid;
    s_previous_func = g_debug_current_func_addr;
}

void starvation_watchdog_get_max_gap(StarvationWatchdogGap *out) {
    if (out) *out = s_max_gap;
}

static uint64_t heartbeat_load(void) {
    return atomic_load_explicit(&s_last_heartbeat_us, memory_order_relaxed);
}

/* Watchdog threshold: 4 seconds of wall-clock without a heartbeat is
 * "TCP died, BIOS still running" territory. Real BIOS pad polling
 * fires VBlank ~60Hz so debug_server_poll runs at ~60Hz. 4 seconds
 * is well past any normal stall.
 *
 * Override with env PSX_STARVATION_TIMEOUT_US (microseconds; 0 disables the
 * watchdog) — needed while bringing up a renderer whose per-op submit model is
 * deliberately slow (a single heavy frame can legitimately exceed 4 s). */
#define STARVATION_TIMEOUT_US_DEFAULT (4 * 1000000ULL)
static uint64_t starvation_timeout_us(void) {
    static int resolved = 0;
    static uint64_t timeout = STARVATION_TIMEOUT_US_DEFAULT;
    if (!resolved) {
        const char *e = getenv("PSX_STARVATION_TIMEOUT_US");
        if (e && *e) timeout = strtoull(e, NULL, 10);
        resolved = 1;
    }
    return timeout;
}

void starvation_watchdog_heartbeat(void) {
    atomic_store_explicit(&s_last_heartbeat_us, host_us_now(),
                          memory_order_relaxed);
}

void starvation_ring_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_seq = 0;
    atomic_store_explicit(&s_last_heartbeat_us, 0, memory_order_relaxed);
    s_dump_done = 0;
    memset(&s_max_gap, 0, sizeof(s_max_gap));
    s_previous_check_us = s_previous_cpu_us = s_previous_guest_cycles = 0;
    s_previous_func = 0;
    s_previous_cpu_valid = 0;
    memset(s_host_timing, 0, sizeof(s_host_timing));
}

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = ctrl;
    e->sio_stat             = stat;
    e->tx_data              = tx;
    e->rx_data              = rx;
    e->kind                 = kind;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = (uint8_t)shift_active;
    e->tx_buffered          = (uint8_t)tx_buffered;
    e->pending_ack          = (uint8_t)pending_ack;
    e->bus_owner            = bus_owner;
    e->active_device        = active_device;
    e->mc_state             = mc_state;
    e->pad_state            = pad_state;
    e->selected_slot        = selected_slot;
    e->g_sio_timing_active  = (uint8_t)g_sio_timing_active;
    e->tx_rdy_visible       = (uint8_t)((stat >> 0) & 1);
    e->tx_em_visible        = (uint8_t)((stat >> 2) & 1);
    e->shift_remaining      = shift_remaining;
    e->ack_remaining        = ack_remaining;
    e->bus_byte_index       = bus_byte_index;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

static const char *kind_name(uint8_t k) {
    switch (k) {
    case SR_EVT_TX_DATA_WRITE: return "TX_WRITE";
    case SR_EVT_RX_DATA_READ:  return "RX_READ";
    case SR_EVT_STAT_READ:     return "STAT_READ";
    case SR_EVT_CTRL_WRITE:    return "CTRL_WRITE";
    case SR_EVT_MODE_WRITE:    return "MODE_WRITE";
    case SR_EVT_BAUD_WRITE:    return "BAUD_WRITE";
    case SR_EVT_SHIFT_START:   return "SHIFT_START";
    case SR_EVT_BUFFER_LOAD:   return "BUFFER_LOAD";
    case SR_EVT_TX_DROPPED:    return "TX_DROPPED";
    case SR_EVT_SHIFT_DONE:    return "SHIFT_DONE";
    case SR_EVT_ACK_FIRE:      return "ACK_FIRE";
    case SR_EVT_SELECT_ASSERT: return "SELECT_ASSERT";
    case SR_EVT_SELECT_DEASS:  return "SELECT_DEASSERT";
    case SR_EVT_RESET:         return "RESET";
    case SR_EVT_PC_SAMPLE:     return "PC_SAMPLE";
    default: return "?";
    }
}

/* PC sampler. Records current PC + in_exc + i_stat/i_mask without
 * touching SIO state — used to localize busy-wait loops that don't
 * touch MMIO. Caller responsible for throttling (~every 1M cycles). */
void starvation_ring_pc_sample(void) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = 0;
    e->sio_stat             = 0;
    e->tx_data              = 0;
    e->rx_data              = 0;
    e->kind                 = SR_EVT_PC_SAMPLE;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = 0;
    e->tx_buffered          = 0;
    e->pending_ack          = 0;
    e->bus_owner            = 0;
    e->active_device        = 0;
    e->mc_state             = 0;
    e->pad_state            = 0;
    e->selected_slot        = 0;
    e->g_sio_timing_active  = 0;
    e->tx_rdy_visible       = 0;
    e->tx_em_visible        = 0;
    e->shift_remaining      = 0;
    e->ack_remaining        = 0;
    e->bus_byte_index       = 0;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

uint64_t starvation_ring_total(void) { return s_seq; }

int starvation_ring_get(uint64_t seq, StarvationEntry *out) {
    if (seq >= s_seq) return 0;                                /* not written yet */
    if (s_seq - seq > STARVATION_RING_CAP) return 0;           /* evicted */
    *out = s_ring[seq & (STARVATION_RING_CAP - 1)];
    if (out->seq != seq) return 0;                             /* raced an eviction */
    return 1;
}

void starvation_ring_dump(const char *path) {
    if (s_dump_done) return;  /* dump exactly once */
    s_dump_done = 1;
    const char *file = path ? path : "starvation_dump.jsonl";
    FILE *f = fopen(file, "w");
    if (!f) return;
    uint64_t total = s_seq;
    uint64_t avail = total < STARVATION_RING_CAP ? total : STARVATION_RING_CAP;
    {
        char net_arch[24];
        int max_players = 0, player_count = 0;
        (void)psx_netplay_diag_snapshot(net_arch, sizeof(net_arch),
                                        &max_players, &player_count);
        fprintf(f, "{\"meta\":{\"total\":%llu,\"shown\":%llu,"
                "\"last_heartbeat_us\":%llu,\"now_us\":%llu,"
                "\"psx_cycle_count\":%llu,"
                "\"current_func\":\"0x%08X\",\"last_store_pc\":\"0x%08X\","
                "\"in_exception\":%u,\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
                "\"net_arch\":\"%s\",\"max_players\":%d,\"player_count\":%d,"
                "\"check_gap_wall_us\":%llu,\"check_gap_cpu_us\":%llu,"
                "\"check_gap_guest_cycles\":%llu,\"check_gap_cpu_available\":%d,"
                "\"check_gap_func_before\":\"0x%08X\","
                "\"check_gap_func_after\":\"0x%08X\"}}\n",
                (unsigned long long)total, (unsigned long long)avail,
                (unsigned long long)heartbeat_load(),
                (unsigned long long)host_us_now(),
                (unsigned long long)psx_get_cycle_count(),
                g_debug_current_func_addr, g_debug_last_store_pc,
                (unsigned)psx_get_in_exception(),
                i_stat, i_mask,
                net_arch, max_players, player_count,
                (unsigned long long)s_max_gap.wall_us,
                (unsigned long long)s_max_gap.cpu_us,
                (unsigned long long)s_max_gap.guest_cycles,
                s_max_gap.cpu_time_available, s_max_gap.func_before,
                s_max_gap.func_after);
    }
    uint64_t start = total > avail ? total - avail : 0;
    for (uint64_t i = start; i < total; i++) {
        StarvationEntry *e = &s_ring[i & (STARVATION_RING_CAP - 1)];
        fprintf(f,
                "{\"seq\":%llu,\"kind\":\"%s\",\"cyc\":%llu,\"us\":%llu,"
                "\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                "\"ctrl\":\"0x%04X\",\"stat\":\"0x%04X\","
                "\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                "\"in_exc\":%u,\"shift_act\":%u,\"shift_rem\":%d,"
                "\"buf\":%u,\"ack_pend\":%u,\"ack_rem\":%d,"
                "\"owner\":%u,\"bbidx\":%u,"
                "\"dev\":%u,\"mc\":%u,\"pad\":%u,\"slot\":%u,"
                "\"gact\":%u,\"tx_rdy\":%u,\"tx_em\":%u,"
                "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\"}\n",
                (unsigned long long)e->seq, kind_name(e->kind),
                (unsigned long long)e->psx_cycle_count,
                (unsigned long long)e->host_us,
                e->current_func, e->last_store_pc,
                e->sio_ctrl, e->sio_stat,
                e->tx_data, e->rx_data,
                e->in_exception, e->shift_active, e->shift_remaining,
                e->tx_buffered, e->pending_ack, e->ack_remaining,
                e->bus_owner, e->bus_byte_index,
                e->active_device, e->mc_state, e->pad_state, e->selected_slot,
                e->g_sio_timing_active, e->tx_rdy_visible, e->tx_em_visible,
                e->i_stat, e->i_mask);
    }
    fclose(f);
}

void starvation_watchdog_check(void) {
    if (s_dump_done) return;
    /* Load the heartbeat BEFORE sampling the clock. The IO thread stamps it
     * concurrently; with the reads in the other order a stamp landing in
     * between made now < last and the unsigned gap wrapped past any
     * threshold (see starvation_watchdog_stale). Re-read AFTER the clock so
     * a concurrent refresh during a host suspension invalidates stale evidence
     * from the first load. */
    uint64_t last = heartbeat_load();
    if (last == 0) return;                 /* not initialized yet */
    uint64_t timeout = starvation_timeout_us();
    if (timeout == 0) return;              /* watchdog disabled (renderer bring-up) */
    uint64_t now = host_us_now();
    record_check_gap(now);
    /* Keep the final refresh check after passive OS accounting as well: a
     * diagnostic system call must not widen the stale-evidence window. */
    uint64_t confirmed_last = heartbeat_load();
    if (starvation_watchdog_confirmed_stale(last, now, confirmed_last, timeout)) {
        starvation_ring_dump(NULL);
        /* Abort cleanly so the dump file is preserved and the user knows
         * the runtime starved. Tag the exit so psx_last_run_report.json
         * says "starvation_watchdog" instead of atexit/unknown. */
        fprintf(stderr, "starvation_watchdog: %llu us without heartbeat — "
                "ring dumped to starvation_dump.jsonl, aborting\n",
                (unsigned long long)(now - confirmed_last));
        fflush(stderr);
        psx_crash_trace_set_exit_origin("starvation_watchdog");
        exit(2);
    }
}

#else

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    (void)kind; (void)tx; (void)rx; (void)ctrl; (void)stat;
    (void)shift_active; (void)shift_remaining;
    (void)tx_buffered; (void)pending_ack; (void)ack_remaining;
    (void)bus_owner; (void)bus_byte_index;
    (void)active_device; (void)mc_state; (void)pad_state;
    (void)selected_slot; (void)g_sio_timing_active;
}
void starvation_watchdog_heartbeat(void) {}
void starvation_watchdog_check(void) {}
uint64_t starvation_watchdog_host_begin(void) { return 0; }
void starvation_watchdog_host_end(StarvationHostSection section, uint64_t begin) {
    (void)section; (void)begin;
}
void starvation_watchdog_host_timing(StarvationHostSection section, StarvationHostTiming *out) {
    (void)section;
    if (out) memset(out, 0, sizeof(*out));
}
void starvation_watchdog_get_max_gap(StarvationWatchdogGap *out) {
    if (out) memset(out, 0, sizeof(*out));
}
void starvation_ring_dump(const char *path) { (void)path; }
void starvation_ring_reset(void) {}
void starvation_ring_pc_sample(void) {}
uint64_t starvation_ring_total(void) { return 0; }
int starvation_ring_get(uint64_t seq, StarvationEntry *out) { (void)seq; (void)out; return 0; }

#endif /* STARVATION_RING_ENABLED */
