/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Divisor contributors */
/* Divisor's sequencer section (firmware/src/divseq.c) on the host, against a model of the Subharmonicon's logic written
 * here independently: every DIVISOR preset played from PLAY for 64 pulses; per pulse the rhythms that fire (p % RHY k),
 * the sequencers they advance (ASSIGN, OR), the step each sequencer is on, the VCO / sub offsets (ROUTE, RANGE), a
 * note on the root at each advance and none otherwise, the pulse's time (DIV at the tempo, to the block), the note
 * released within the pulse; a key while it runs moves the root and sounds nothing of its own; STOP ends the note and
 * clears the offsets. Renders each preset's run to build/divseq_demo/NN_NAME.wav for listening.
 *   cc -O1 -w -Ifirmware/hal -Ifirmware/bsp/chip -Ifirmware/bsp/boards/fm1 -Ifirmware/src -Ibuild/gen \
 *      -o build/host/divseq_test tests/divseq_test.c -lm && build/host/divseq_test */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <sys/stat.h>

static int fails;
static void check(const char *what, int ok)
{
    printf("divseq: %-92s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* the model: which sequencers pulse p advances (bit 0 SEQ 1, bit 1 SEQ 2) */
static uint32_t model_fire(const track_t *t, uint32_t p)
{
    uint32_t k, f = 0;
    for (k = 0; k < 4u; k++) {
        uint32_t r = (uint32_t)t->p[DV_R1 + k];
        if (r < 1u) r = 1u;
        if (r > 16u) r = 16u;
        if (p % r == 0u)
            f |= (uint32_t)t->p[DV_RA1 + k] & 3u;
    }
    return f;
}
static int32_t model_vco(const track_t *t, uint32_t s, uint32_t step)
{
    static const int32_t OCT[3] = {1, 2, 5};
    if (!((uint32_t)t->p[DV_SA1 + s] & 1u))
        return 0;
    return t->p[P_LN0 + s * 4u + step] * OCT[t->p[P_FTYPE] % 3] * 192 / 64;
}
static int32_t model_div(const track_t *t, uint32_t sub, uint32_t step)
{
    uint32_t s = sub / 2u, bit = 2u << (sub & 1u);
    return ((uint32_t)t->p[DV_SA1 + s] & bit) ? t->p[P_LN0 + s * 4u + step] / 8 : 0;
}
static int gated(const track_t *t, uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NVOICE; v++)
        if (t->v[v].active && t->v[v].gate && t->v[v].note == note)
            return 1;
    return 0;
}
static uint32_t ngated(const track_t *t)
{
    uint32_t v, n = 0;
    for (v = 0; v < NVOICE; v++)
        n += t->v[v].active && t->v[v].gate;
    return n;
}

static uint32_t demo_frames;
static FILE *wav_open(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f)
        wav_hdr(f, 0);
    demo_frames = 0;
    return f;
}
static void wav_close(FILE *f)
{
    if (!f)
        return;
    fseek(f, 0, SEEK_SET);
    wav_hdr(f, demo_frames);
    fclose(f);
}

static int32_t blk_out[2 * CTL];
static FILE *demo;
static uint32_t frames;
static void one_block(void)
{
    uint32_t i;
    mix_block(blk_out, CTL);
    if (demo) {
        for (i = 0; i < CTL; i++)
            wav_put(demo, blk_out[2 * i], blk_out[2 * i + 1]);
        demo_frames += CTL;
    }
    frames += CTL;
}

static void run_preset(uint32_t pi)
{
    track_t *t = &trk[0];
    dv_part_t *D;
    uint32_t p, last = 0, period, seen = 0, bad_fire = 0, bad_step = 0, bad_off = 0, bad_note = 0, bad_time = 0;
    uint32_t bad_gate = 0, step[2] = {3, 3}, start, guard;
    char path[96], name[64], what[160];
    host_tracks_init();
    host_preset(t, ENGI_DIVISOR, pi);
    song.sel = 0;
    D = dv_of(t);
    D->root = 45;                                     /* A2: the keys' last note */
    snprintf(name, sizeof name, "%02u_%s", pi, DIVISOR_PRESETS[pi].name);
    snprintf(path, sizeof path, "build/divseq_demo/%s.wav", name);
    demo = wav_open(path);
    frames = 0;
    period = div_samples((uint32_t)t->p[P_SDIV]);
    transport_req = 1;
    start = frames;
    for (guard = 0; seen < 64u && guard < 200000u; guard++) {
        one_block();
        if (!D->run)
            continue;
        while (last < D->pulse) {                     /* (one pulse a block at most: DIV is longer than a block) */
            uint32_t f = model_fire(t, last), s, k, ok;
            p = last++;
            seen++;
            bad_fire += D->fired != f;
            for (s = 0; s < 2u; s++)
                if ((f >> s) & 1u)
                    step[s] = (step[s] + 1u) & 3u;
            bad_step += D->step[0] != step[0] || D->step[1] != step[1];
            if (f) {
                ok = 1;
                for (s = 0; s < 2u; s++)
                    ok &= D->sq_vco[s] == model_vco(t, s, step[s]);
                for (k = 0; k < 4u; k++)
                    ok &= D->sq_div[k] == model_div(t, k, step[k / 2u]);
                bad_off += !ok;
                bad_note += !gated(t, 45) || ngated(t) != 1u;
            }
            /* the pulse's time: p periods after PLAY, within the block it falls in */
            if (frames - start < p * period || frames - start > p * period + 2u * CTL)
                bad_time++;
        }
        /* a note never outlives its pulse: gated only within half a pulse (and a block) of the last advance */
        if (D->gate && D->gate_left > period / 2u)
            bad_gate++;
    }
    snprintf(what, sizeof what, "%s: 64 pulses: rhythms fire as p %% RHY, ASSIGN OR'd (%u wrong)", name, bad_fire);
    if (seen != 64u)
        printf("  (seen %u pulses)\n", seen);
    check(what, seen == 64u && !bad_fire);
    snprintf(what, sizeof what, "%s: the steps advance on their sequencer's pulses only (%u wrong)", name, bad_step);
    check(what, !bad_step);
    snprintf(what, sizeof what, "%s: VCO and sub offsets from the step, ROUTE and RANGE (%u wrong)", name, bad_off);
    check(what, !bad_off);
    snprintf(what, sizeof what, "%s: a note on the root at each advance, one at a time (%u wrong)", name, bad_note);
    check(what, !bad_note);
    snprintf(what, sizeof what, "%s: pulse p at p x DIV from PLAY, gate under half a pulse (%u, %u wrong)", name,
             bad_time, bad_gate);
    check(what, !bad_time && !bad_gate);
    /* a key while it runs: the root moves, nothing sounds of its own; the next advance plays the new root */
    {
        uint32_t before = D->pulse, ok, o1, o2, o3, o4;
        input_on(t, 52, 100);
        o1 = D->root == 52u;
        one_block();
        o2 = ngated(t) <= 1u && !gated(t, 45);           /* (glided to 52, or nothing sounding) */
        for (guard = 0; guard < 20000u && !(D->pulse > before && D->fired); guard++)
            one_block();
        o3 = gated(t, 52) && ngated(t) == 1u;
        input_off(t, 52);
        one_block();
        o4 = D->run && (D->gate == (uint8_t)gated(t, 52));
        ok = o1 && o2 && o3 && o4;
        if (!ok)
            printf("  (root %d, after a block %d, next advance %d (gate %u note %u n %u), release %d)\n", o1, o2, o3,
                   D->gate, D->note, ngated(t), o4);
        snprintf(what, sizeof what, "%s: a key moves the root, the next advance plays it, its release ends nothing", name);
        check(what, ok);
    }
    /* STOP */
    {
        uint32_t k, ok;
        transport_req = 2;
        one_block();
        one_block();
        ok = !D->run && !D->gate && !ngated(t) && !D->sq_vco[0] && !D->sq_vco[1];
        for (k = 0; k < 4u; k++)
            ok &= !D->sq_div[k];
        snprintf(what, sizeof what, "%s: STOP ends the note, the oscillators back to their knobs", name);
        check(what, ok);
    }
    for (guard = 0; guard < FS / CTL; guard++)        /* the tail */
        one_block();
    wav_close(demo);
    demo = 0;
}

int main(void)
{
    uint32_t pi;
    mkdir("build", 0755);
    mkdir("build/divseq_demo", 0755);
    for (pi = 0; pi < NELEM(DIVISOR_PRESETS); pi++)
        run_preset(pi);
    /* the keys while stopped play as usual and set the root */
    {
        track_t *t = &trk[0];
        host_tracks_init();
        host_preset(t, ENGI_DIVISOR, 0);
        one_block();
        input_on(t, 50, 100);
        one_block();
        check("stopped: a key plays its own note and becomes the root", gated(t, 50) && dv_of(t)->root == 50u);
        input_off(t, 50);
        one_block();
        check("stopped: its release ends it", !ngated(t));
    }
    printf("divseq: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
