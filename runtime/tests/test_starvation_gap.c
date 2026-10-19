/* Link the production watchdog; replace only unrelated emulator dependencies.
 * Windows test: passive accounting must distinguish CPU work from Sleep and
 * must neither wrap on a state rewind nor change the existing abort policy. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "starvation_ring.h"

uint32_t g_debug_current_func_addr = 0x1234;
uint32_t g_debug_last_store_pc = 0;
uint32_t i_stat = 0, i_mask = 0;
static uint64_t guest_cycles = 1000000;
uint64_t psx_get_cycle_count(void) { return guest_cycles; }
int psx_get_in_exception(void) { return 0; }
int psx_netplay_diag_snapshot(char *arch, size_t cap, int *max, int *count) {
    if (cap) snprintf(arch, cap, "off");
    *max = *count = 0;
    return 0;
}
void psx_crash_trace_set_exit_origin(const char *origin) { (void)origin; }

int main(int argc, char **argv) {
    if (argc != 2) return 3;
    starvation_ring_reset();
    starvation_watchdog_heartbeat();
    starvation_watchdog_check();
    if (strcmp(argv[1], "sleep-abort") == 0) {
        Sleep(4500);
        starvation_watchdog_check();
        return 4;  /* unchanged 4-second watchdog must have aborted */
    }
    if (strcmp(argv[1], "busy-abort") == 0) {
        ULONGLONG end = GetTickCount64() + 6000;
        while (GetTickCount64() < end) {
            guest_cycles += 65536;
            starvation_watchdog_check();
        }
        return 5;
    }
    if (strcmp(argv[1], "observe") != 0) return 6;
    ULONGLONG end = GetTickCount64() + 100;
    while (GetTickCount64() < end) guest_cycles++;
    starvation_watchdog_check();
    StarvationWatchdogGap busy, sleeping, reset;
    starvation_watchdog_get_max_gap(&busy);
    if (!busy.cpu_time_available || busy.wall_us < 50000 || busy.cpu_us < 15000)
        return 7;
    /* Observe waiting independently, then rewind as a savestate would. */
    starvation_ring_reset();
    starvation_watchdog_heartbeat();
    starvation_watchdog_check();
    guest_cycles = 1;
    uint64_t section_begin = starvation_watchdog_host_begin();
    Sleep(250);
    starvation_watchdog_host_end(SW_HOST_AUDIO, section_begin);
    starvation_watchdog_check();
    starvation_watchdog_get_max_gap(&sleeping);
    if (sleeping.wall_us < 200000 || !sleeping.cpu_time_available ||
        sleeping.cpu_us > sleeping.wall_us / 2 || sleeping.guest_cycles != 0)
        return 8;
    StarvationHostTiming section;
    starvation_watchdog_host_timing(SW_HOST_AUDIO, &section);
    if (section.calls != 1 || section.max_us < 200000 || section.total_us != section.max_us)
        return 10;
    starvation_watchdog_host_end(SW_HOST_SECTION_COUNT, section_begin);
    starvation_watchdog_host_timing(SW_HOST_SECTION_COUNT, &section);
    if (section.calls || section.total_us || section.max_us) return 11;
    starvation_ring_reset();
    starvation_watchdog_get_max_gap(&reset);
    if (reset.wall_us || reset.cpu_us || reset.guest_cycles || reset.cpu_time_available)
        return 9;
    starvation_watchdog_host_timing(SW_HOST_AUDIO, &section);
    if (section.calls || section.total_us || section.max_us) return 12;
    puts("production passive watchdog accounting PASS");
    return 0;
}
