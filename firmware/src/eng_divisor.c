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
 * F ATK, F DEC and QNT, the quantizer (OFF, ET12, JI8, ET8). The sequencer section (divseq.c) keeps its values the
 * same way: RHY1..RHY4, the rhythm ASSIGNs and the two ROUTEs in P_FM3_ATK .. P_FM4_LEVEL, the eight steps in
 * DRUM's lane levels P_LN0 .. P_LN7, the RANGE in ANALOG's filter TYPE P_FTYPE; its clock is the track's DIV.
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
enum { DV_LV1 = P_FM1_ATK, DV_LV2, DV_LS1, DV_LS2, DV_LS3, DV_LS4, DV_EGA, DV_FATK, DV_FDEC, DV_QNT,
       DV_R1, DV_R2, DV_R3, DV_R4, DV_RA1, DV_RA2, DV_RA3, DV_RA4, DV_SA1, DV_SA2 };
#define DV_NY 10u                /* the sequencer's: DV_R1 .. DV_SA2 */
_Static_assert((int)DV_QNT == (int)P_FM2_LEVEL && (int)DV_QNT - (int)DV_LV1 + 1 == (int)DV_NX, "DIVISOR: ten slots in a row");
_Static_assert((int)DV_SA2 == (int)P_FM4_LEVEL, "DIVISOR: the sequencer's ten after them");

typedef struct {
    uint32_t ph[2];              /* the VCOs' phases */
    uint32_t sph[DV_NSUB];       /* the subs' phases (sub k follows VCO k >> 1) */
    uint8_t cnt[DV_NSUB];        /* VCO cycles since the sub's last restart */
    uint8_t fstage;              /* VCF EG: 0 idle, 1 attack, 2 decay */
    int32_t fenv;                /* VCF EG level, Q24 */
    int32_t y[4];                /* the ladder's four poles */
    /* the sequencer (divseq.c) */
    int16_t sq_vco[2];           /* the VCOs' offsets, 1/16 semitone (before the quantizer) */
    int8_t sq_div[DV_NSUB];      /* the subs' division offsets */
    uint8_t run, gate, note, root;   /* running; a note on (its number); the root */
    uint8_t step[2], fired;      /* the sequencers' steps (0..3); the last pulse's advances (bit 0 SEQ 1, bit 1 SEQ 2) */
    uint32_t pos, pulse, gate_left;  /* samples into the pulse, pulses since PLAY, samples the note still holds */
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
static const char *const N_DV_RA[] = {"OFF", "SEQ1", "SEQ2", "BOTH"};
static const char *const N_DV_SA[] = {"OFF", "VCO", "SUB A", "VCO+A", "SUB B", "VCO+B", "A+B", "ALL"};
static const char *const N_DV_OCT[] = {"1 OCT", "2 OCT", "5 OCT"};
static const param_desc_t DV_YDESC[DV_NY] = {
    {"RHY1", F_INT, 1, 16, 1, 0, 0},
    {"RHY2", F_INT, 1, 16, 2, 0, 0},
    {"RHY3", F_INT, 1, 16, 3, 0, 0},
    {"RHY4", F_INT, 1, 16, 4, 0, 0},
    {"R1>", F_ENUM, 0, 3, 1, N_DV_RA, 0},
    {"R2>", F_ENUM, 0, 3, 2, N_DV_RA, 0},
    {"R3>", F_ENUM, 0, 3, 0, N_DV_RA, 0},
    {"R4>", F_ENUM, 0, 3, 0, N_DV_RA, 0},
    {"SEQ1>", F_ENUM, 0, 7, 1, N_DV_SA, 0},
    {"SEQ2>", F_ENUM, 0, 7, 1, N_DV_SA, 0},
};
static const param_desc_t DV_STEP[8] = {
    {"S1.1", F_BIPCT, -64, 63, 0, 0, 0}, {"S1.2", F_BIPCT, -64, 63, 0, 0, 0},
    {"S1.3", F_BIPCT, -64, 63, 0, 0, 0}, {"S1.4", F_BIPCT, -64, 63, 0, 0, 0},
    {"S2.1", F_BIPCT, -64, 63, 0, 0, 0}, {"S2.2", F_BIPCT, -64, 63, 0, 0, 0},
    {"S2.3", F_BIPCT, -64, 63, 0, 0, 0}, {"S2.4", F_BIPCT, -64, 63, 0, 0, 0},
};
static const param_desc_t DV_RANGE = {"RANGE", F_ENUM, 0, 2, 0, N_DV_OCT, 0};
/* the descriptor of a common parameter DIVISOR uses as its own (params.c track_desc / param_desc_of), 0 = TP's */
static const param_desc_t *dv_xdesc(uint32_t id)
{
    if (id >= DV_LV1 && id < DV_LV1 + DV_NX)
        return &DV_XDESC[id - DV_LV1];
    if (id >= DV_R1 && id <= DV_SA2)
        return &DV_YDESC[id - DV_R1];
    if (id >= P_LN0 && id <= P_LN7)
        return &DV_STEP[id - P_LN0];
    if (id == P_FTYPE)
        return &DV_RANGE;
    return 0;
}
static int dv_own(uint32_t id)
{
    return (id >= P_FM1_ATK && id <= P_FM4_LEVEL) || (id >= P_LN0 && id <= P_LN7) || id == P_FTYPE;
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
    uint32_t qm = (uint32_t)p[DV_QNT] & 3u;
    int32_t vco1 = D->sq_vco[0] ? dv_quant(D->sq_vco[0], qm) : 0;
    int32_t vco2 = dv_quant(p[P_E1] * 16 + D->sq_vco[1], qm);
    (void)v;
    inc[0] = vco1 ? pitch_inc(clamp(m->pitch16 + vco1, 0, 2047)) : m->inc;
    inc[1] = pitch_inc(clamp(m->pitch16 + vco2, 0, 2047));
    if (m->fine) {
        if (vco1)
            inc[0] += (uint32_t)((int32_t)(inc[0] >> 12) * m->fine);
        inc[1] += (uint32_t)((int32_t)(inc[1] >> 12) * m->fine);
    }
    lv[0] = clamp(p[DV_LV1], 0, 127);
    lv[1] = clamp(p[DV_LV2], 0, 127);
    for (k = 0; k < DV_NSUB; k++) {
        div[k] = (uint32_t)clamp(p[P_E2 + k] + D->sq_div[k], 1, 16);
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

/* the presets' sequencer: steps S1.1..S2.4 (at RANGE 1 OCT a semitone is ~5.3: 37 a fifth, 64 an octave), RHY1..4,
 * R1>..R4> (0 OFF 1 SEQ1 2 SEQ2 3 BOTH), SEQ1> SEQ2> (bit 0 VCO, 1 SUB A, 2 SUB B), RANGE. (The clock, the track's DIV,
 * stays as the track has it: Felucca keeps a track's DIV over a sound load) */
typedef struct {
    int8_t st[8];
    uint8_t r[4], ra[4], sa[2], range;
} dv_seq_t;
static const dv_seq_t DV_PRESET_SEQ[NELEM(DIVISOR_PRESETS)] = {
    {{0, 37, 16, 63, 0, -27, 27, -64}, {4, 3, 5, 7}, {1, 2, 0, 0}, {1, 1}, 0},        /* DIVISOR: 4 against 3 */
    {{0, 37, 0, 27, 0, 16, 37, 0}, {2, 3, 4, 6}, {1, 2, 0, 0}, {1, 1}, 0},           /* FIFTHS */
    {{0, 40, -24, 16, 8, -16, 24, 0}, {3, 4, 5, 7}, {1, 2, 1, 2}, {7, 7}, 0},        /* MOONDOG: every route */
    {{0, -27, 0, 11, 0, 0, -32, 16}, {8, 6, 16, 12}, {1, 2, 0, 0}, {1, 1}, 0},       /* DEEP: slow */
    {{0, 63, 0, -64, 0, 32, -32, 63}, {2, 4, 3, 8}, {1, 2, 3, 0}, {1, 1}, 0},        /* OCTAVES */
    {{0, 0, 16, 0, 0, 0, -16, 0}, {3, 5, 4, 7}, {1, 2, 0, 0}, {3, 3}, 0},            /* UNISON: subs move */
    {{0, 21, 37, 58, 0, 27, -21, 0}, {2, 3, 5, 4}, {1, 2, 0, 0}, {1, 1}, 0},         /* JUST: JI8 steps */
    {{0, 37, 21, 48, 0, 16, 27, -11}, {1, 2, 3, 5}, {1, 2, 3, 0}, {1, 1}, 0},        /* PLUCK: 1/16 pulses */
};

/* after a preset's values (ui.c apply_preset_to, tests/hostsim.c): a DIVISOR track's own parameters from the preset's
 * rows (the common ones were just reset to TP's defaults: DIGITAL's, DRUM's, ANALOG's) */
static void divisor_track_loaded(track_t *t)
{
    uint32_t i, pi = t->preset;
    const dv_seq_t *q;
    if (t->eng_req % NENGINES != ENGI_DIVISOR)
        return;
    if (pi >= NELEM(DIVISOR_PRESETS))
        pi = 0;
    q = &DV_PRESET_SEQ[pi];
    for (i = 0; i < DV_NX; i++)
        t->p[DV_LV1 + i] = DV_PRESET_X[pi][i];
    for (i = 0; i < 4u; i++) {
        t->p[DV_R1 + i] = q->r[i];
        t->p[DV_RA1 + i] = q->ra[i];
    }
    t->p[DV_SA1] = q->sa[0];
    t->p[DV_SA2] = q->sa[1];
    for (i = 0; i < 8u; i++)
        t->p[P_LN0 + i] = q->st[i];
    t->p[P_FTYPE] = q->range;
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
