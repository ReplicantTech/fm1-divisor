/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Divisor contributors */
/* DIVISOR: a subharmonicon voice. Two VCOs; each drives two sub-oscillators that divide its frequency by
 * 1..16 (phase-locked dividers, as the hardware ones: they restart with their VCO every N cycles), so the subs
 * are exact subharmonics of the VCO, not detuned copies. All six go through one 4-pole ladder-style low-pass
 * with feedback resonance (saturating, self-oscillating towards RES 127) and the track's amplitude envelope;
 * the track's ENV -> FILTER / LFO -> FILTER (vmod_t cutoff) move the cutoff.
 *
 * Phase 0 (the CPU budget test): fixed mix (VCOs full, subs at ~0.7), the wave shared by all six, VCO 2 at an
 * interval from the note. The mixer, the EG amount knob, the four-step sequencers and the rhythm generators
 * come with the side state block (divseq.c) in phases 1 and 2.
 *
 * Mono (poly 1): one voice per part is the whole instrument. The oscillators and the filter live in the part's
 * engine memory (ENG_MEM), not the voice: like VCOs they run on across notes (no phase reset on a key; a note
 * sets the pitch) and the filter never restarts (no click).
 *
 * Parameters: WAVE (SAW / SQR, all six), VCO2 (semitones from the note, -24..24), DIV1 DIV2 (VCO 1's subs),
 * DIV3 DIV4 (VCO 2's), CUT, RES. HOME knobs: CUT RES ATK REL. */
static const char *const N_DIV_WAVE[] = {"SAW", "SQR"};
#define DV_NSUB 4u

typedef struct {
    uint32_t ph[2];              /* the VCOs' phases */
    uint32_t sph[DV_NSUB];       /* the subs' phases (sub k follows VCO k >> 1) */
    uint8_t cnt[DV_NSUB];        /* VCO cycles since the sub's last restart */
    int32_t y[4];                /* the ladder's four poles */
} dv_part_t;
ENG_MEM_FITS("DIVISOR", sizeof(dv_part_t));
static inline dv_part_t *dv_of(const track_t *t) { return (dv_part_t *)ENG_MEM((uint32_t)(t - trk) % NPART); }

static void divisor_note_on(track_t *t, voice_t *v)
{
    (void)t;
    (void)v;                     /* the oscillators and the filter run on: nothing to reset */
}

/* one-pole coefficient (Q12) for the cutoff cut (0..127 << 8): from the SVF's tan table, g1 = 2g / (1 + 2g),
 * a one-pole whose corner is at the cutoff; four in a row make the ladder (-24 dB / oct) */
static inline int32_t dv_pole_g(int32_t cut)
{
    int32_t i, g;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 127)
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    g *= 2;
    return clamp((g << 12) / (4096 + g), 1, 4000);
}

static __attribute__((noinline)) void divisor_render(track_t *t, voice_t *v, int32_t *out, uint32_t n,
                                                     const vmod_t *m)
{
    dv_part_t *D = dv_of(t);
    const int16_t *p = t->p;
    uint32_t sqr = p[P_E0] != 0, i, k;
    uint32_t inc[2], sinc[DV_NSUB], ph[2], sph[DV_NSUB], rcp[DV_NSUB];
    uint32_t div[DV_NSUB];
    uint8_t cnt[DV_NSUB];
    int32_t y0 = D->y[0], y1 = D->y[1], y2 = D->y[2], y3 = D->y[3];
    int32_t g = dv_pole_g((p[P_E6] << 8) + m->cutoff);
    int32_t kres = p[P_E7] * 140;                       /* feedback, Q12: 0 .. ~4.3 (self-oscillation above ~4) */
    (void)v;
    inc[0] = m->inc;
    inc[1] = pitch_inc(clamp(m->pitch16 + p[P_E1] * 16, 0, 2047));
    if (m->fine)
        inc[1] += (uint32_t)((int32_t)(inc[1] >> 12) * m->fine);
    for (k = 0; k < DV_NSUB; k++) {
        div[k] = (uint32_t)clamp(p[P_E2 + k], 1, 16);
        sinc[k] = inc[k >> 1] / div[k];
        rcp[k] = 65536u / div[k];                       /* for the restart: ph / div as (ph >> 16) * rcp */
        sph[k] = D->sph[k];
        cnt[k] = D->cnt[k];
    }
    ph[0] = D->ph[0];
    ph[1] = D->ph[1];
    for (i = 0; i < n; i++) {
        int32_t s, x;
        /* the VCOs */
        if (sqr)
            s = osc_pulse(ph[0], inc[0], 0x80000000u) + osc_pulse(ph[1], inc[1], 0x80000000u);
        else
            s = osc_saw(ph[0], inc[0]) + osc_saw(ph[1], inc[1]);
        /* the subs, at 0.7: 23 / 32 */
        x = 0;
        for (k = 0; k < DV_NSUB; k++)
            x += sqr ? osc_pulse(sph[k], sinc[k], 0x80000000u) : osc_saw(sph[k], sinc[k]);
        s += (x * 23) >> 5;
        /* advance; a VCO wrap counts a cycle on its subs, and restarts the one whose count is up (the drift of
         * sinc's rounding goes: at most a few phase units a cycle) */
        for (k = 0; k < 2u; k++) {
            ph[k] += inc[k];
            if (ph[k] < inc[k]) {
                uint32_t j = k * 2u;
                if (++cnt[j] >= div[j]) {
                    cnt[j] = 0;
                    sph[j] = (ph[k] >> 16) * rcp[j];
                }
                if (++cnt[j + 1u] >= div[j + 1u]) {
                    cnt[j + 1u] = 0;
                    sph[j + 1u] = (ph[k] >> 16) * rcp[j + 1u];
                }
            }
        }
        for (k = 0; k < DV_NSUB; k++)
            sph[k] += sinc[k];
        /* the ladder: six oscillators at up to 4.8 x full scale -> ~0.6, the feedback off the last pole,
         * the saturation at the input (the ladder's transistors), four one-poles */
        x = softclip((s >> 3) - ((kres * y3) >> 12));
        y0 += (g * (x - y0)) >> 12;
        y1 += (g * (y0 - y1)) >> 12;
        y2 += (g * (y1 - y2)) >> 12;
        y3 += (g * (y2 - y3)) >> 12;
        out[i] += voice_amp(y3, m, i) * 2;
    }
    D->ph[0] = ph[0];
    D->ph[1] = ph[1];
    for (k = 0; k < DV_NSUB; k++) {
        D->sph[k] = sph[k];
        D->cnt[k] = cnt[k];
    }
    D->y[0] = y0;
    D->y[1] = y1;
    D->y[2] = y2;
    D->y[3] = y3;
}

/* e: WAVE VCO2 DIV1 DIV2 DIV3 DIV4 CUT RES */
static const preset_t DIVISOR_PRESETS[] = {
    {"DIVISOR", {0, 0, 2, 3, 4, 5, 80, 30}, {0, 70, 100, 40}, 20, 1, FX(0, 0, 20, 30), PAT(8)},
    {"FIFTHS", {0, 7, 2, 4, 3, 6, 70, 40}, {0, 60, 90, 50}, 30, 1, FX(0, 10, 30, 40), PAT(5)},
    {"MOONDOG", {1, 0, 3, 5, 7, 2, 60, 50}, {0, 50, 60, 30}, 45, 1, FX(0, 0, 40, 20), PAT(8)},
    {"DEEP", {0, -12, 2, 4, 2, 8, 40, 20}, {10, 80, 110, 60}, 10, 1, FX(0, 0, 10, 50), PAT(5)},
    {"OCTAVES", {1, 12, 2, 4, 8, 16, 90, 10}, {0, 60, 100, 30}, 0, 1, FX(0, 20, 20, 30), PAT(2)},
    {"UNISON", {0, 0, 1, 1, 1, 1, 100, 60}, {0, 50, 80, 40}, 50, 1, FX(10, 0, 30, 20), PAT(1)},
};

static const engine_t ENG_DIVISOR = {
    .name = "DIVISOR",
    .page_title = {"VCO", "FLT"},
    .edit = {
        {"WAVE", F_ENUM, 0, 1, 0, N_DIV_WAVE, 0},
        {"VCO2", F_SEMI, -24, 24, 0, 0, 0},
        {"DIV1", F_INT, 1, 16, 2, 0, 0},
        {"DIV2", F_INT, 1, 16, 3, 0, 0},
        {"DIV3", F_INT, 1, 16, 4, 0, 0},
        {"DIV4", F_INT, 1, 16, 5, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = DIVISOR_PRESETS,
    .npresets = NELEM(DIVISOR_PRESETS),
    .note_on = divisor_note_on,
    .render = divisor_render,
    .knob = {P_E6, P_E7, P_ATK, P_REL},
    .poly = 1,                   /* one voice: the instrument */
    .mem = sizeof(dv_part_t),
};

#if !FELUCCA_ADPCM
/* engine 14 (ADPCM) when it is not built: a reserved number, never selectable (eng_ok), silent, so DIVISOR stays 15 */
static void dv_gone_note_on(struct track *t, voice_t *v) { (void)t; (void)v; }
static void dv_gone_render(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
static const engine_t ENG_ADPCM_GONE = {
    .name = "-",
    .page_title = {"-", "-"},
    .edit = {{"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0},
             {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0},
             {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0}},
    .note_on = dv_gone_note_on,
    .render = dv_gone_render,
    .knob = {P_E4, P_E5, P_E6, P_REL},
};
#endif
