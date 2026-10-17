/* Execute the real dirty-RAM dispatcher, including nested JALR -> dispatch_call.
 * Synthetic instructions only: no BIOS/disc bytes or game/plugin dependency.
 * Unknown runtime seams abort via fixture_link.py rather than fake success. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dirty_ram_interp.h"

uint32_t g_psx_ram_size = 0x200000u, g_psx_ram_mask = 0x1FFFFFu;
uint32_t g_psx_mod_instruction_hooks, g_psx_mod_function_entry_hooks;
uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen, i_stat, i_mask;
int g_psx_dispatch_depth, g_psx_call_bail, g_rfe_escape_pending;
int g_exc_escape_reason, g_psx_draw_distance_clamp_live;
uint64_t s_frame_count, psx_cycle_count, g_dispatch_static_hits;
uint64_t g_psx_bail_first, g_psx_bail_resolved;
static uint8_t ram[0x200000u];
static int failures, nested_calls;

#define CHECK(c, msg) do { if (!(c)) { puts("FAIL: " msg); ++failures; } } while (0)

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) {
    uint32_t v; memcpy(&v, ram + (a & 0x1FFFFCu), 4); return v;
}
static void put(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFCu), &v, 4); }
static void store(uint32_t a, uint32_t v) { put(a, v); }
uint8_t psx_cyc_load_byte(CPUState *c, uint32_t a, uint32_t rt, uint32_t m) {
    (void)c; (void)rt; (void)m; return ram[a & 0x1FFFFFu];
}
int dirty_ram_is_dirty(uint32_t a) { (void)a; return 1; }
int psx_game_address_in_text(uint32_t a) { (void)a; return 0; }
int psx_game_text_native_ok(uint32_t a) { (void)a; return 0; }
int psx_game_text_native_ok_full(uint32_t a) { (void)a; return 0; }
int psx_game_is_function_entry(uint32_t a) { (void)a; return 0; }
int psx_kernel_bless_dispatchable(uint32_t a) { (void)a; return 0; }
int psx_kernel_patch_range_ends_at(uint32_t a) { (void)a; return 0; }
void fntrace_maybe_mark_game_started(CPUState *c, uint32_t a) { (void)c; (void)a; }
int psx_get_in_exception(void) { return 1; }
void psx_check_interrupts(CPUState *c) { (void)c; }
void psx_check_interrupts_at(CPUState *c, uint32_t pc) { (void)c; (void)pc; }
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 1; }
void psx_rfe_escape_check(CPUState *c) { (void)c; }
int overlay_fp_enabled(void) { return 0; }
int overlay_loader_dispatch(CPUState *c, uint32_t a) { (void)c; (void)a; return 0; }
int overlay_loader_call_native(CPUState *c, uint32_t a) { (void)c; (void)a; return 0; }
int overlay_loader_is_candidate(uint32_t a) { (void)a; return 0; }
uint32_t psx_overlay_resident_crc_at(uint32_t a, int *valid) {
    (void)a; *valid = 0; return 0;
}
void psx_pgxp_alu(CPUState *c, uint32_t i, uint32_t r, uint32_t a, uint32_t b) {
    (void)c; (void)i; (void)r; (void)a; (void)b;
}
void psx_pgxp_load(CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}
void psx_pgxp_store(CPUState *c, uint32_t i, uint32_t a, uint32_t v) {
    (void)c; (void)i; (void)a; (void)v;
}

/* Dispatcher seam: execute the nested callback, validate the same return-PC/SP
 * contract as psx_dispatch_call, then return to the suspended interpreter. */
void psx_dispatch_call(CPUState *c, uint32_t target, uint32_t ret) {
    uint32_t sp = c->gpr[29];
    ++nested_calls;
    if (nested_calls > 8) { puts("FAIL: unexpected recursive callback"); exit(1); }
    CHECK(dirty_ram_dispatch(c, target, ret) == 1, "nested callback handled");
    CHECK(c->pc == ret, "nested callback returned to walker");
    CHECK(c->gpr[29] == sp, "nested callback preserved stack");
    c->pc = 0;
}

static CPUState setup(uint32_t entry, uint32_t ret) {
    CPUState c;
    memset(ram, 0, sizeof ram);
    memset(&c, 0, sizeof c);
    dirty_ram_ld_delay_discard();
    c.read_word = psx_read_word;
    c.write_word = store;
    c.pc = entry;
    c.gpr[31] = ret;
    c.gpr[2] = 0x12345678u;
    for (int r = 16; r < 24; ++r) c.gpr[r] = 0xA000u + (uint32_t)r;
    c.gpr[28] = 0xB028u; c.gpr[29] = 0x8548u; c.gpr[30] = 0xB030u;
    c.cop0[12] = 0x40020404u;
    c.cop0[13] = 0x400u;
    c.cop0[14] = 0x80041E78u;
    put(0x2030u, 0xFC000000u); /* unsupported opcode after cleared slot */
    return c;
}

static void check_saved(const CPUState *c, const CPUState *before) {
    for (int r = 16; r < 24; ++r)
        CHECK(c->gpr[r] == before->gpr[r], "s0-s7 restored");
    for (int r = 28; r < 31; ++r)
        CHECK(c->gpr[r] == before->gpr[r], "gp/sp/fp restored");
}

static void recovery_case(uint32_t ret, int pending_load) {
    CPUState c = setup(0x1F6Cu, ret), before = c;
    if (pending_load) {
        /* One register-only instruction per saved register, then a final LB
         * whose delayed writeback must not overwrite the restored s0. */
        for (int r = 16; r < 24; ++r)
            put(0x1FE4u + 4u * (uint32_t)(r - 16), (uint32_t)r << 11); /* sll r,zero,0 */
        put(0x2004u, 28u << 11); /* sll gp,zero,0 */
        put(0x2008u, 29u << 11); /* sll sp,zero,0 */
        put(0x200Cu, 30u << 11); /* sll fp,zero,0 */
        put(0x202Cu, 0x80100000u); /* lb s0,0(zero), pending at RI */
        ram[0] = 0x7Fu;
    }
    uint64_t count = g_dirty_ram_empty_irq_callback_returns;
    int handled = dirty_ram_dispatch(&c, c.pc, ret);
    printf("recovery ret=%08X pending=%d handled=%d pc=%08X count=%llu s0=%08X\n",
           ret, pending_load, handled, c.pc,
           (unsigned long long)(g_dirty_ram_empty_irq_callback_returns - count), c.gpr[16]);
    CHECK(handled == 1, "empty callback handled");
    CHECK(g_dirty_ram_empty_irq_callback_returns == count + 1, "recovery counted once");
    CHECK(c.pc == ret && c.gpr[2] == 0, "empty callback returns zero");
    check_saved(&c, &before);
    CHECK(c.cop0[12] == before.cop0[12] && c.cop0[13] == before.cop0[13] &&
          c.cop0[14] == before.cop0[14], "recovery leaves COP0 unchanged");
}

static void nested_case(uint32_t site, int prefix) {
    uint32_t ret = site + 8u;
    CPUState c = setup(prefix ? site - 4u : site, 0x282Cu);
    c.gpr[17] = 0x1F6Cu;
    CPUState before = c;
    put(site, 0x0220F809u); /* jalr ra,s1; nop */
    uint64_t mid = g_dirty_ram_unsupported_midblock;
    uint64_t count = g_dirty_ram_empty_irq_callback_returns;
    int calls = nested_calls;
    CHECK(dirty_ram_dispatch(&c, c.pc, ret) == 1, "valid JALR caller handled");
    CHECK(nested_calls == calls + 1, "JALR executed nested callback");
    CHECK(g_dirty_ram_empty_irq_callback_returns == count + 1, "nested recovery counted");
    CHECK(g_dirty_ram_unsupported_midblock == mid, "caller did not inherit callee RI");
    CHECK(c.pc == ret, "caller resumes at JALR continuation");
    CHECK(c.cop0[14] == 0x80041E78u, "caller EPC was not replaced with callee RI");
    check_saved(&c, &before);
    printf("nested site=%08X prefix=%d pc=%08X recoveries=%llu\n", site, prefix, c.pc,
           (unsigned long long)(g_dirty_ram_empty_irq_callback_returns - count));
}

static void historical_word_case(void) {
    CPUState c = setup(0x2030u, 0x293Cu);
    uint64_t count = g_dirty_ram_empty_irq_callback_returns;
    /* The current decoder ignores undefined SPECIAL funct encodings. The old
     * incident's word is NOT a recovery trigger on this framework revision. */
    put(c.pc, 0x000900FFu);
    CHECK(dirty_ram_dispatch(&c, c.pc, 0x2034u) == 1, "historical word handled by current decoder");
    CHECK(c.pc == 0x2034u, "historical word advances under current SPECIAL policy");
    CHECK(g_dirty_ram_empty_irq_callback_returns == count, "historical word does not exercise recovery");
    puts("historical 000900FF: current SPECIAL policy, no recovery");
}

static void rejected_case(uint32_t entry, uint32_t ret, int writes) {
    CPUState c = setup(entry, ret);
    uint32_t invalid = entry + 8;
    put(entry, writes ? 0xAC000100u : 0u); /* sw zero,0x100(zero) or nop */
    put(invalid, 0xFC000000u);
    uint64_t count = g_dirty_ram_empty_irq_callback_returns;
    CHECK(dirty_ram_dispatch(&c, entry, 0) == 1, "ordinary midblock RI handled");
    CHECK(g_dirty_ram_empty_irq_callback_returns == count, "nonempty/nonwalker callback not recovered");
    CHECK(c.pc == 0x80000080u && c.cop0[14] == invalid, "ordinary RI vectors with exact EPC");
    CHECK((c.cop0[13] & 0x7Cu) == 40u, "ordinary RI ExcCode=10");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    recovery_case(0x293Cu, 0);
    recovery_case(0x2950u, 0);
    recovery_case(0x293Cu, 1);
    recovery_case(0x2950u, 1);
    nested_case(0x2934u, 0);
    nested_case(0x2934u, 1);
    nested_case(0x2948u, 0);
    nested_case(0x2948u, 1);
    historical_word_case();
    rejected_case(0x1F6Cu, 0x293Cu, 1);
    rejected_case(0x1F6Cu, 0x1234u, 0);
    rejected_case(0x10000u, 0x293Cu, 0);
    CPUState c = setup(0x1F6Cu, 0x293Cu);
    put(c.pc, 0xFC000000u);
    CHECK(dirty_ram_dispatch(&c, c.pc, 0) == 0, "first invalid instruction still falls back");
    if (failures) { printf("%d regression checks failed\n", failures); return 1; }
    puts("PASS: IRQ callback recovery, saved registers/load delay, nested JALR, RI exclusions");
    return 0;
}
