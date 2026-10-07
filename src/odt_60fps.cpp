/*
 * 60fps via render passes: each pass replays one task-scheduler round at
 * interpolated GTE transforms. Anatomy: NOTES.md "## 60fps".
 */

#include "mod_plugins.h"
#include "cpu_state.h"
#include "render_pass_replay.hpp"

namespace {

constexpr uint32_t kRoundStart  = 0x800932B8u;   /* yield wraps to the task list head */
constexpr uint32_t kRoundInsn   = 0x8F826D70u;
constexpr uint32_t kYield       = 0x80093230u;
constexpr uint32_t kSubmit      = 0x8009617Cu;
constexpr uint32_t kSubmitRet   = 0x800932B0u;
constexpr uint32_t kSentinel    = 0x800932A8u;   /* never a return address */
constexpr uint32_t kCdCommand   = 0x8009105Cu;   /* libcd: polls a CD that passes freeze */
constexpr uint32_t kPutDrawEnv  = 0x8008A0F8u;
constexpr uint32_t kClearImage  = 0x8008A26Cu;
constexpr uint32_t kDrawOTag    = 0x8008A468u;

constexpr uint32_t kFrameA      = 0x800B1B28u;
constexpr uint32_t kFrameB      = 0x800B1BA4u;
constexpr uint32_t kPublished   = 0x800B1B20u;   /* frame awaiting the VSync callback */
constexpr uint32_t kCurFrame    = 0x800B1C28u;
constexpr uint32_t kThreshold   = 0x800B1C2Cu;
constexpr uint32_t kClearColour = 0x800B1C34u;
constexpr uint32_t kOtLength    = 0x800B1C3Cu;

/* Written by the VSync IRQ mid-round; passes take no IRQs, so replays apply them. */
constexpr uint32_t kIrqWords[] = {0x8009F320u, 0x800A034Cu, 0x800A0368u, 0x800A036Cu, 0x800A05E0u,
                                  0x800AF6C0u, 0x800AF6C4u, 0x800AF6E0u, 0x800B1B1Cu};
constexpr uint32_t kIrqCount = sizeof kIrqWords / sizeof kIrqWords[0];

/* Flips are seen a present late, so frames show at phases 0.5 and 1 only. */
constexpr uint32_t kFramePhase  = 0x8000u;
constexpr uint32_t kTweenPhase  = 0xC000u;

/* PSXDrawReplay, but dispatching the whole round: tasks resume via saved $ra. */
class RoundReplay {
    CPUState entry{};
    std::vector<uint8_t> ram;
    uint8_t scratch[1024]{};
    uint32_t previous_tick = 0;
    bool captured = false, ready = false, have_tick = false;
    PSXProjectionHistory* history = nullptr;
public:
    bool reached = false;
    uint32_t outer_sp = 0;
    ~RoundReplay() { psx_projection_destroy(history); }
    void invalidate() {
        captured = ready = have_tick = false;
        psx_projection_invalidate(history);
    }
    void capture(CPUState* cpu) {
        if (captured) invalidate();
        if (!history && !(history = psx_projection_create(65536))) return;
        entry = *cpu;
        ram.assign(memory_get_ram_ptr(), memory_get_ram_ptr() + memory_get_ram_bytes());
        std::memcpy(scratch, memory_get_scratchpad_ptr(), sizeof scratch);
        captured = true; ready = false;
        psx_projection_capture_begin(history);
    }
    bool prepare(uint32_t tick) {
        if (!captured) { invalidate(); return false; }
        const uint32_t ticks = have_tick ? tick - previous_tick : 0;
        captured = false; previous_tick = tick; have_tick = true;
        psx_projection_capture_end(history, ticks, 512.0);
        PSXProjectionStats stats;
        psx_projection_stats(history, &stats);
        return ready = ticks && ticks <= 8 && stats.changed && !stats.overflow;
    }
    bool restore(CPUState* cpu) {
        if (!ready || ram.size() != memory_get_ram_bytes()) return false;
        std::memcpy(memory_get_ram_ptr(), ram.data(), ram.size());
        std::memcpy(memory_get_scratchpad_ptr(), scratch, sizeof scratch);
        pgxp_invalidate_all();
        *cpu = entry;
        psx_projection_replay_begin(history, 0x8000u);
        return true;
    }
    bool draw(CPUState* cpu) {
        reached = false;
        outer_sp = cpu->gpr[29];
        cpu->gpr[31] = kSentinel;
        /* A task switch left for the outermost dispatch to flatten lands here. */
        for (uint32_t pc = kRoundStart, i = 0; pc && i < 4096 && !reached; ++i) {
            cpu->pc = 0;
            psx_dispatch_call(cpu, pc, kSentinel);
            if (reached || !g_psx_call_bail) break;
            g_psx_call_bail = 0;
            pc = cpu->pc;
        }
        g_psx_call_bail = 0;
        psx_projection_replay_end(history);
        return reached;
    }
};

RoundReplay g_replay;
uint32_t g_vblanks = 0, g_yields = 0, g_irq_vblanks = 0;
int32_t g_irq_yield = -1;          /* yields before the round's VBlank */
uint32_t g_irq_vals[kIrqCount];

void on_vblank(void) {
    if (g_psx_render_pass_active) return;
    ++g_vblanks;
    if (g_irq_vblanks++ == 0) g_irq_yield = (int32_t)g_yields;
}

int on_yield(CPUState*, uint32_t) {
    if (++g_yields == (uint32_t)(g_irq_yield + 1) && g_psx_render_pass_active)
        for (uint32_t i = 0; i < kIrqCount; ++i) psx_mod_write_word(kIrqWords[i], g_irq_vals[i]);
    return 0;
}

void on_round_start(CPUState* cpu, uint32_t) {
    if (g_psx_render_pass_active) return;
    if (psx_mod_read_half(kThreshold) != 2) { g_replay.invalidate(); return; }
    g_replay.capture(cpu);
    g_yields = 0; g_irq_yield = -1; g_irq_vblanks = 0;
}

int on_cd_command(CPUState* cpu, uint32_t) {
    if (!g_psx_render_pass_active) return 0;
    cpu->gpr[2] = 0;
    return 1;
}

/* Draw frame struct f's OT into target's buffer, as the VSync callback would. */
void draw_frame(CPUState* cpu, uint32_t f, uint32_t target) {
    PSXDrawReplay::call(cpu, kPutDrawEnv, target + 8);
    if (const uint32_t c = psx_mod_read_word(kClearColour))
        PSXDrawReplay::call(cpu, kClearImage, target + 8, c);
    const uint32_t ot = psx_mod_read_word(f + 0x78);
    PSXDrawReplay::call(cpu, kDrawOTag, ot + (uint32_t)(int16_t)psx_mod_read_half(kOtLength) * 4 - 4);
}

int on_frame_pass(CPUState* cpu, void*, uint32_t) {
    const uint32_t f = psx_mod_read_word(kPublished);
    draw_frame(cpu, f, f);
    return 1;
}

int on_tween_pass(CPUState* cpu, void*, uint32_t) {
    if (!g_replay.restore(cpu)) return 0;
    g_yields = 0;
    return g_replay.draw(cpu) ? 1 : 0;
}

/* Frame k and the k->k+1 in-between, run in the game's idle spin before frame k is drawn. */
void run_passes(CPUState* cpu) {
    const uint32_t f = psx_mod_read_word(kPublished);
    uint32_t phases[8];
    if ((f != kFrameA && f != kFrameB) || !psx_mod_render_pass_plan(2, 1, phases, 8)) return;
    PSXModRenderPass pass{};
    pass.struct_size = sizeof pass;
    pass.x = (uint16_t)psx_mod_read_half(f + 8);   /* frame k's DRAWENV clip */
    pass.y = (uint16_t)psx_mod_read_half(f + 10);
    pass.w = (uint16_t)psx_mod_read_half(f + 12);
    pass.h = (uint16_t)psx_mod_read_half(f + 14);
    pass.alpha_q16 = kFramePhase;
    if (!pass.w || !pass.h || !psx_mod_render_pass(cpu, &pass, on_frame_pass, nullptr)) return;
    pass.alpha_q16 = kTweenPhase;
    psx_mod_render_pass(cpu, &pass, on_tween_pass, nullptr);
}

/* Outside a pass: run the passes. Inside: draw the rebuilt OT and end the replay. */
int on_submit(CPUState* cpu, uint32_t) {
    if (cpu->gpr[31] != kSubmitRet) return 0;
    if (!g_psx_render_pass_active) {
        for (uint32_t i = 0; i < kIrqCount; ++i) g_irq_vals[i] = psx_mod_read_word(kIrqWords[i]);
        if (g_replay.prepare(g_vblanks) && g_irq_vblanks <= 1) run_passes(cpu);
        return 0;
    }
    const uint32_t built = psx_mod_read_word(kCurFrame);
    draw_frame(cpu, built, built == kFrameA ? kFrameB : kFrameA);
    g_replay.reached = true;
    cpu->gpr[31] = kSentinel;
    cpu->gpr[29] = g_replay.outer_sp;
    return 1;
}

void reset(void) { g_replay.invalidate(); }

void activate(void) {
    reset();
    PSXDrawReplay::rate(60u, PSX_MOD_RENDER_PASS_FLIP_SHOWN);
}

}  // namespace

PSX_MOD_CONSTRUCTOR(odt_register_60fps_plugin) {
    (void)psx_mod_register_activation_plugin("odt.60fps", activate);
    (void)psx_mod_register_savestate_plugin("odt.60fps", reset);
    (void)psx_mod_register_vblank_plugin("odt.60fps", on_vblank);
    (void)psx_mod_register_instruction_plugin("odt.60fps", kRoundStart, kRoundInsn, on_round_start);
    (void)psx_mod_register_function_filter_plugin("odt.60fps", kYield, on_yield);
    (void)psx_mod_register_function_filter_plugin("odt.60fps", kSubmit, on_submit);
    (void)psx_mod_register_function_filter_plugin("odt.60fps", kCdCommand, on_cd_command);
}
