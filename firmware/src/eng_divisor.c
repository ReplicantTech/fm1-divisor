/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Divisor contributors */
/* DIVISOR: a subharmonicon voice. Two VCOs; each drives two sub-oscillators that divide its frequency by
 * 1..16 (phase-locked dividers, as the hardware ones: they restart with their VCO every N cycles), so the subs
 * are exact subharmonics of the VCO, not detuned copies. A six-channel mixer, then one 4-pole ladder-style
 * low-pass with feedback resonance (saturating, self-oscillating towards RES 127), its own attack / decay
 * envelope (VCF EG, a bipolar amount) and the track's amplitude envelope (the VCA EG); the track's
 * ENV -> FILTER / LFO -> FILTER (vmod_t cutoff) move the cutoff too.
 *
 * Mono (poly 1): one voice per part is the whole instrument. The oscillators, the filter and the VCF EG live
 * in the part's engine memory (ENG_MEM), not the voice: like VCOs they run on across notes (no phase reset on
 * a key; a note sets the pitch) and the filter never restarts (no click).
 *
 * Parameters. The engine's eight (EDIT 1 / 2): WAVE (SAW / SQR, all six), VCO2 (semitones from the note,
 * -24..24, through the quantizer), DIV1 DIV2 (VCO 1's subs), DIV3 DIV4 (VCO 2's), CUT, RES. HOME knobs:
 * CUT RES ATK REL. Ten more in the track's P_FM1_ATK .. P_FM2_LEVEL slots (DIGITAL's retired operator
 * envelopes: stored in projects, user presets and motion like any parameter; params.c param_desc_of names
 * them on a DIVISOR track, params.c PAGES shows them as SUB MIX, SUB MIX 2 and VCF EG): the six levels, EG AMT,
 * F ATK, F DEC and QNT, the quantizer (OFF, ET12, JI8, ET8: the Subharmonicon's four). P_FM3_ATK ..
 * P_FM4_LEVEL (ten) are reserved for the sequencer (divseq.c, phase 2).
 *
 * No favourites: favorites.c keeps a row per engine 0..13 and the settings in rows 14 and 15, all of whose bytes
 * are taken, so FAV on a DIVISOR sound changes nothing (favorite_set refuses). The dedicated front panel (plan,
 * option B) gets its own store. */
static const char *const N_DIV_WAVE[] = {"SAW", "SQR"};
static const char *const N_DIV_QNT[] = {"OFF", "ET12", "JI8", "ET8"};
#define DV_NSUB 4u
#ifndef DV_DRIVE
#define DV_DRIVE 9               /* the mix into the ladder: >> 9 (Q15 x Q7 levels, six channels) */
#endif
#ifndef DV_GAIN
#define DV_GAIN 4                /* the ladder's output to the voice level (Felucca's engines' loudness) */
#endif
#define DV_NX 10u                /* the extra parameters, P_FM1_ATK .. */
enum { DV_LV1 = P_FM1_ATK, DV_LV2, DV_LS1, DV_LS2, DV_LS3, DV_LS4, DV_EGA, DV_FATK, DV_FDEC, DV_QNT };
_Static_assert(DV_QNT == P_FM2_LEVEL && DV_QNT - DV_LV1 + 1 == DV_NX, "DIVISOR: ten slots in a row");

typedef struct {
    uint32_t ph[2];              /* the VCOs' phases */
    uint32_t sph[DV_NSUB];       /* the subs' phases (sub k follows VCO k >> 1) */
    uint8_t cnt[DV_NSUB];        /* VCO cycles since the sub's last restart */
    uint8_t fstage;              /* VCF EG: 0 idle, 1 attack, 2 decay */
    int32_t fenv;                /* VCF EG level, Q24 */
    int32_t y[4];                /* the ladder's four poles */
} dv_part_t;
ENG_MEM_FITS("DIVISOR", sizeof(dv_part_t));
static inline dv_part_t *dv_of(const track_t *t) { return (dv_part_t *)ENG_MEM((uint32_t)(t - trk) % NPART); }

static const param_desc_t DV_XDESC[DV_NX] = {
    {"VCO1", F_PCT, 0, 127, 100, 0, 0},
    {"VCO2", F_PCT, 0, 127, 100, 0, 0},
    {"SUB1", F_PCT, 0, 127, 80, 0, 0},
    {"SUB2", F_PCT, 0, 127, 80, 0, 0},
    {"SUB3", F_PCT, 0, 127, 80, 0, 0},
    {"SUB4", F_PCT, 0, 127, 80, 0, 0},
    {"EG", F_BIPCT, -64, 63, 30, 0, 0},
    {"FATK", F_TIME, 0, 127, 0, 0, 0},
    {"FDEC", F_TIME, 0, 127, 70, 0, 0},
    {"QNT", F_ENUM, 0, 3, 1, N_DIV_QNT, 0},
};
static const param_desc_t DV_XNONE = {"-", F_INT, 0, 0, 0, 0, 0};
/* the descriptor of a P_FM1_ATK .. P_FM4_LEVEL slot on a DIVISOR track (params.c param_desc_of) */
static const param_desc_t *dv_xdesc(uint32_t id)
{
    return id >= DV_LV1 && id < DV_LV1 + DV_NX ? &DV_XDESC[id - DV_LV1] : &DV_XNONE;
}

/* the quantizer: an offset in 1/16 semitones -> the nearest pitch of the mode. OFF as it is; ET12 whole semitones;
 * JI8 eight just ratios in the octave (1/1 9/8 5/4 4/3 3/2 5/3 7/4 15/8); ET8 eight equal steps (1.5 semitones) */
static const uint8_t DV_JI8[8] = {0, 33, 62, 80, 112, 142, 155, 174};   /* cents x 16 / 100 */
static int32_t dv_quant(int32_t q16, uint32_t mode)
{
    int32_t oct, in, best, k, d, bd;
    if (mode == 1u)
        return ((q16 + (q16 < 0 ? -8 : 8)) / 16) * 16;
    if (mode == 3u)
        return ((q16 + (q16 < 0 ? -12 : 12)) / 24) * 24;
    if (mode != 2u)
        return q16;
    oct = q16 / 192;                                    /* towards 0 */
    in = q16 - oct * 192;
    if (in < 0) {
        in += 192;
        oct--;
    }
    best = 192;                                          /* the octave above: the 9th candidate */
    bd = 192 - in;
    for (k = 0; k < 8; k++) {
        d = (int32_t)DV_JI8[k] - in;
        if (d < 0)
            d = -d;
        if (d < bd) {
            bd = d;
            best = DV_JI8[k];
        }
    }
    return oct * 192 + best;
}

static void divisor_note_on(track_t *t, voice_t *v)
{
    dv_part_t *D = dv_of(t);
    (void)v;                     /* the oscillators and the filter run on: nothing to reset */
    D->fstage = 1;               /* the VCF EG restarts from where it is */
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

/* the VCF EG, once a block (render runs once a block: one voice): linear attack (ENV_LIN, as the VCA's), exponential
 * decay to 0 (ENV_EXP); returns its level, Q15 */
static inline int32_t dv_feg(dv_part_t *D, const int16_t *p)
{
    if (D->fstage == 1u) {
        D->fenv += (int32_t)ENV_LIN[p[DV_FATK] & 127];
        if (D->fenv >= (1 << 24)) {
            D->fenv = 1 << 24;
            D->fstage = 2;
        }
    } else if (D->fstage == 2u) {
        D->fenv -= mulq16(D->fenv, ENV_EXP[p[DV_FDEC] & 127]);
        if (D->fenv < (1 << 10)) {
            D->fenv = 0;
            D->fstage = 0;
        }
    }
    return D->fenv >> 9;
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
    int32_t lv[2], slv[DV_NSUB];
    int32_t y0 = D->y[0], y1 = D->y[1], y2 = D->y[2], y3 = D->y[3];
    int32_t feg = dv_feg(D, p);
    int32_t cut = (p[P_E6] << 8) + m->cutoff + ((p[DV_EGA] * feg) >> 5);   /* EG AMT 63: up to twice the range */
    int32_t g = dv_pole_g(cut);
    int32_t kres = p[P_E7] * 140;                       /* feedback, Q12: 0 .. ~4.3 (self-oscillation above ~4) */
    int32_t vco2 = dv_quant(p[P_E1] * 16, (uint32_t)p[DV_QNT] & 3u);
    (void)v;
    inc[0] = m->inc;
    inc[1] = pitch_inc(clamp(m->pitch16 + vco2, 0, 2047));
    if (m->fine)
        inc[1] += (uint32_t)((int32_t)(inc[1] >> 12) * m->fine);
    lv[0] = clamp(p[DV_LV1], 0, 127);
    lv[1] = clamp(p[DV_LV2], 0, 127);
    for (k = 0; k < DV_NSUB; k++) {
        div[k] = (uint32_t)clamp(p[P_E2 + k], 1, 16);
        sinc[k] = inc[k >> 1] / div[k];
        rcp[k] = 65536u / div[k];                       /* for the restart: ph / div as (ph >> 16) * rcp */
        sph[k] = D->sph[k];
        cnt[k] = D->cnt[k];
        slv[k] = clamp(p[DV_LS1 + k], 0, 127);
    }
    ph[0] = D->ph[0];
    ph[1] = D->ph[1];
    for (i = 0; i < n; i++) {
        int32_t s, x;
        /* the VCOs and the subs into the mixer (Q15 x Q7) */
        if (sqr) {
            s = osc_pulse(ph[0], inc[0], 0x80000000u) * lv[0] + osc_pulse(ph[1], inc[1], 0x80000000u) * lv[1];
            for (k = 0; k < DV_NSUB; k++)
                s += osc_pulse(sph[k], sinc[k], 0x80000000u) * slv[k];
        } else {
            s = osc_saw(ph[0], inc[0]) * lv[0] + osc_saw(ph[1], inc[1]) * lv[1];
            for (k = 0; k < DV_NSUB; k++)
                s += osc_saw(sph[k], sinc[k]) * slv[k];
        }
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
        /* the ladder: the mix (six at full level: 6 x 127 x full scale) to ~1.5 into the input saturation (the
         * ladder's transistors: driven, as the hardware's mixer drives it), the feedback off the last pole, four
         * one-poles */
        x = softclip((s >> DV_DRIVE) - ((kres * y3) >> 12));
        y0 += (g * (x - y0)) >> 12;
        y1 += (g * (y0 - y1)) >> 12;
        y2 += (g * (y1 - y2)) >> 12;
        y3 += (g * (y2 - y3)) >> 12;
        out[i] += voice_amp(y3, m, i) * DV_GAIN;
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

/* e: WAVE VCO2 DIV1 DIV2 DIV3 DIV4 CUT RES; DV_PRESET_X: VCO1 VCO2 SUB1 SUB2 SUB3 SUB4 EG FATK FDEC QNT */
static const preset_t DIVISOR_PRESETS[] = {
    {"DIVISOR", {0, 0, 2, 3, 4, 5, 70, 30}, {0, 70, 100, 40}, 0, 1, FX(0, 0, 20, 30), PAT(8)},
    {"FIFTHS", {0, 7, 2, 4, 3, 6, 60, 40}, {0, 60, 90, 50}, 0, 1, FX(0, 10, 30, 40), PAT(5)},
    {"MOONDOG", {1, 0, 3, 5, 7, 2, 68, 40}, {0, 60, 80, 30}, 0, 1, FX(0, 0, 40, 20), PAT(8)},
    {"DEEP", {0, -12, 2, 4, 2, 8, 36, 20}, {10, 80, 110, 60}, 0, 1, FX(0, 0, 10, 50), PAT(5)},
    {"OCTAVES", {1, 12, 2, 4, 8, 16, 85, 10}, {0, 60, 100, 30}, 0, 1, FX(0, 20, 20, 30), PAT(2)},
    {"UNISON", {0, 0, 1, 1, 1, 1, 90, 60}, {0, 50, 80, 40}, 0, 1, FX(10, 0, 30, 20), PAT(1)},
    {"JUST", {0, 4, 2, 3, 5, 6, 64, 45}, {0, 60, 100, 50}, 0, 1, FX(0, 15, 25, 35), PAT(5)},
    {"PLUCK", {1, 0, 2, 4, 3, 6, 58, 55}, {0, 88, 0, 60}, 0, 1, FX(0, 0, 35, 25), PAT(3)},
};
static const int8_t DV_PRESET_X[NELEM(DIVISOR_PRESETS)][DV_NX] = {
    {100, 100, 80, 80, 80, 80, 30, 0, 70, 1},          /* DIVISOR */
    {100, 90, 70, 70, 70, 70, 25, 0, 80, 1},           /* FIFTHS */
    {100, 100, 110, 110, 110, 110, 40, 0, 60, 1},      /* MOONDOG */
    {100, 80, 90, 60, 90, 60, 20, 30, 100, 1},         /* DEEP */
    {100, 100, 60, 60, 60, 60, 35, 0, 50, 1},          /* OCTAVES */
    {100, 100, 100, 100, 100, 100, 50, 0, 60, 1},      /* UNISON */
    {100, 100, 80, 80, 80, 80, 30, 0, 75, 2},          /* JUST: JI8 */
    {110, 110, 100, 100, 100, 100, 63, 0, 60, 1},      /* PLUCK: the EG does the note */
};

/* after a preset's values (ui.c apply_preset_to, tests/hostsim.c): a DIVISOR track's ten extra parameters from the
 * preset's row (the common ones were just reset to TP's defaults, DIGITAL's; the reserved ten to 0) */
static void divisor_track_loaded(track_t *t)
{
    uint32_t i, pi = t->preset;
    if (t->eng_req % NENGINES != ENGI_DIVISOR)
        return;
    if (pi >= NELEM(DIVISOR_PRESETS))
        pi = 0;
    for (i = 0; i < DV_NX; i++)
        t->p[DV_LV1 + i] = DV_PRESET_X[pi][i];
    for (i = DV_LV1 + DV_NX; i <= P_FM4_LEVEL; i++)
        t->p[i] = 0;
}

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
        {"CUT", F_CUTOFF, 0, 127, 70, 0, 0},
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
