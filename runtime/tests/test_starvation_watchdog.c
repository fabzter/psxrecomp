/*
 * test_starvation_watchdog.c — pin the race-free staleness decision behind
 * starvation_watchdog_check().
 *
 * Regression test for the false watchdog trip seen on Breath of Fire III
 * (2026-09-02): the debug-server IO thread stamps the heartbeat on every
 * send() chunk, the emu thread's check sampled the clock first and the
 * heartbeat second, so a stamp landing between the two reads made
 * `last > now`; `now - last` wrapped to ~1.8e19 us, beat the 4 s threshold,
 * and a healthy 60 fps runtime exited with a dump whose meta line showed the
 * heartbeat was 402 us old. starvation_watchdog_stale() refuses a negative gap
 * by construction; this test pins that property. The two-read helper also
 * pins deterministic interleavings around a suspension of the checking thread.
 *
 * Build: gcc -I../include -o test_starvation_watchdog test_starvation_watchdog.c
 */
#include <stdio.h>
#include <stdint.h>

#include "starvation_ring.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    const uint64_t TIMEOUT = 4 * 1000000ull;   /* runtime default: 4 s */
    const uint64_t T       = 322798613022ull;  /* the 2026-09-02 heartbeat */

    /* 1. The false-trip scenario: a heartbeat stamped by the IO thread AFTER
     *    the emu thread sampled the clock. Old code wrapped here. */
    CHECK(starvation_watchdog_stale(T + 1, T, TIMEOUT) == 0,
          "heartbeat 1 us newer than the clock sample does not fire");
    CHECK(starvation_watchdog_stale(T + 5000, T, TIMEOUT) == 0,
          "heartbeat 5 ms newer than the clock sample does not fire");
    CHECK(starvation_watchdog_stale(T, T, TIMEOUT) == 0,
          "heartbeat equal to the clock sample does not fire");

    /* 2. Fresh heartbeats never fire (the 402 us case from the dump). */
    CHECK(starvation_watchdog_stale(T, T + 402, TIMEOUT) == 0,
          "402 us staleness does not fire a 4 s watchdog");
    CHECK(starvation_watchdog_stale(T, T + TIMEOUT, TIMEOUT) == 0,
          "staleness exactly at the threshold does not fire");

    /* 3. A genuine stall still fires. */
    CHECK(starvation_watchdog_stale(T, T + TIMEOUT + 1, TIMEOUT) == 1,
          "staleness one past the threshold fires");
    CHECK(starvation_watchdog_stale(T, T + 60 * 1000000ull, TIMEOUT) == 1,
          "60 s staleness fires");

    /* 4. Disable / not-yet-initialised guards. */
    CHECK(starvation_watchdog_stale(T, T + 60 * 1000000ull, 0) == 0,
          "timeout 0 (PSX_STARVATION_TIMEOUT_US=0) never fires");
    CHECK(starvation_watchdog_stale(0, T, TIMEOUT) == 0,
          "no heartbeat yet never fires");

    /* 5. Wrap is impossible for any pair, not just the observed one. */
    CHECK(starvation_watchdog_stale(UINT64_MAX, 1, TIMEOUT) == 0,
          "max heartbeat vs tiny clock does not fire");
    CHECK(starvation_watchdog_stale(1, UINT64_MAX, TIMEOUT) == 1,
          "tiny heartbeat vs max clock fires");

    /* 6. Deterministic read / clock / read interleavings, without sleeps or
     * threads. At N the original stamp T really is expired. */
    const uint64_t N = T + 3 * TIMEOUT;
    CHECK(starvation_watchdog_confirmed_stale(N - 402, N, N - 402, TIMEOUT) == 0,
          "heartbeat refreshed before the first read stays fresh");
    CHECK(starvation_watchdog_stale(T, N, TIMEOUT) == 1,
          "pre-suspension first read alone would fire");
    CHECK(starvation_watchdog_confirmed_stale(T, N, N - 402, TIMEOUT) == 0,
          "refresh during suspension before the clock prevents false abort");
    CHECK(starvation_watchdog_confirmed_stale(T, N, N, TIMEOUT) == 0,
          "refresh equal to the clock prevents abort");
    CHECK(starvation_watchdog_confirmed_stale(T, N, N + 1, TIMEOUT) == 0,
          "refresh after the clock (future stamp) prevents abort");
    CHECK(starvation_watchdog_confirmed_stale(N + 1, N, N + 1, TIMEOUT) == 0,
          "future first stamp cannot wrap the confirmed gap");
    CHECK(starvation_watchdog_confirmed_stale(T, N, T, TIMEOUT) == 1,
          "unchanged expired heartbeat still detects a real stall");
    CHECK(starvation_watchdog_confirmed_stale(T, N, T + 1, TIMEOUT) == 1,
          "changed but still expired heartbeat still fires");
    CHECK(starvation_watchdog_confirmed_stale(T, N, N - TIMEOUT, TIMEOUT) == 0,
          "confirmed age exactly at the unchanged threshold does not fire");
    CHECK(starvation_watchdog_confirmed_stale(T, N, N - TIMEOUT - 1, TIMEOUT) == 1,
          "confirmed age one past the unchanged threshold fires");
    CHECK(starvation_watchdog_confirmed_stale(T, N, T, 0) == 0,
          "zero timeout disables the two-read decision");
    CHECK(starvation_watchdog_confirmed_stale(0, N, T, TIMEOUT) == 0,
          "uninitialized first heartbeat does not fire");
    CHECK(starvation_watchdog_confirmed_stale(T, N, 0, TIMEOUT) == 0,
          "heartbeat reset before confirmation does not fire");
    CHECK(starvation_watchdog_confirmed_stale(UINT64_MAX, 1, UINT64_MAX, TIMEOUT) == 0,
          "two-read decision rejects negative gaps at integer extremes");
    CHECK(starvation_watchdog_confirmed_stale(1, UINT64_MAX, 1, TIMEOUT) == 1,
          "two-read decision preserves real stalls at integer extremes");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all starvation watchdog checks passed\n");
    return 0;
}
