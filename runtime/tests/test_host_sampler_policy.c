/* Exercise the real Windows sampler in separate processes for default and
 * explicit diagnostic modes. No guest, generated code or game data required. */
#include "host_sampler.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int g_psx_render_pass_active;
uint64_t s_frame_count;

int main(int argc, char **argv) {
    assert(argc == 2);
    const int enabled = strcmp(argv[1], "enabled") == 0;
    if (enabled) {
        assert(_putenv("PSX_HOST_SAMPLER_SUSPEND=1") == 0);
    } else if (strcmp(argv[1], "invalid") == 0) {
        assert(_putenv("PSX_HOST_SAMPLER_SUSPEND=true") == 0);
    } else {
        assert(strcmp(argv[1], "default") == 0);
        assert(_putenv("PSX_HOST_SAMPLER_SUSPEND=") == 0);
    }
    host_sampler_start();
    host_sampler_start(); /* idempotence */
    assert(host_sampler_supported() == enabled);
    const ULONGLONG until = GetTickCount64() + 250;
    while (GetTickCount64() < until) Sleep(1);
    if (enabled) {
        assert(host_sampler_seq() > 0);
        HostSample sample;
        assert(host_sampler_get(host_sampler_seq() - 1, &sample));
    } else {
        assert(host_sampler_seq() == 0);
        assert(host_sampler_max_suspend_us() == 0);
        assert(host_sampler_max_suspend_hold_us() == 0);
        assert(host_sampler_max_suspend_wait_us() == 0);
        assert(host_sampler_suspend_age_us() == 0);
        HostSample sample;
        assert(!host_sampler_get(0, &sample));
    }
    return 0;
}
