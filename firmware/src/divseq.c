/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Divisor contributors */
/* DIVSEQ: the Subharmonicon's sequencer section for a DIVISOR track (eng_divisor.c), run from seq.c seq_tick in the
 * audio ISR, once a block, in place of Felucca's step sequencer on that track.
 *
 *   clock     the track's DIV (P_SDIV, SEQ > ROUTE): one pulse per DIV at the song tempo, or the external clock's
 *             grid (seq_tick hands the samples over after midi_grid_advance). PLAY starts it, every rhythm firing on
 *             the first pulse; STOP releases the note and puts the oscillators back.
 *   rhythms   RHY1..RHY4 divide the pulse by 1..16; RHY k fires on every pulse p with p % RHY k == 0. ASSIGN routes
 *             each to SEQ 1, SEQ 2, both or neither: a sequencer advances once on a pulse where any rhythm routed to it
 *             fires (OR, as the Subharmonicon's default logic).
 *   sequencers  SEQ 1 and SEQ 2, four steps each (ST1..ST4, -64..63). ROUTE sends SEQ 1 to VCO 1 and / or its subs
 *             (SUB 1, SUB 2), SEQ 2 to VCO 2 / SUB 3 / SUB 4. On a VCO a step is a pitch offset of +-RANGE octaves
 *             (OCT: 1, 2 or 5) at full scale, through the quantizer (QNT); on a sub it adds step / 8 (-8..7) to its
 *             division number (clamped to 1..16: a higher number, a lower sub).
 *   envelopes  any advance retriggers both: a note on the track (the VCA's ADSR, the VCF EG: divisor_note_on) at the
 *             root, held for half a pulse.
 *   root      the last key or MIDI note on the track (C3 at first); while the sequencer runs a key only moves the root
 *             (a sounding note glides there as LEGATO does), it never sounds a note of its own.
 * Its state is in the part's engine memory with the oscillators' (dv_part_t: zeroed when the engine is claimed). */
static const uint8_t DV_OCT[3] = {1, 2, 5};

/* the sequencers' current steps -> the oscillators' offsets (read by divisor_render) */
static void divseq_apply(const track_t *t, dv_part_t *D)
{
    const int16_t *p = t->p;
    uint32_t s, oct = DV_OCT[(uint32_t)p[P_FTYPE] % 3u];
    for (s = 0; s < 2u; s++) {
        uint32_t route = (uint32_t)p[DV_SA1 + s] & 7u;
        int32_t v = clamp(p[P_LN0 + s * 4u + (D->step[s] & 3u)], -64, 63);
        D->sq_vco[s] = (int16_t)((route & 1u) ? v * (int32_t)oct * 192 / 64 : 0);
        D->sq_div[s * 2u] = (int8_t)((route & 2u) ? v / 8 : 0);
        D->sq_div[s * 2u + 1u] = (int8_t)((route & 4u) ? v / 8 : 0);
    }
}

static void divseq_clear(dv_part_t *D)
{
    uint32_t k;
    D->sq_vco[0] = D->sq_vco[1] = 0;
    for (k = 0; k < DV_NSUB; k++)
        D->sq_div[k] = 0;
}

static void divseq_gate_off(track_t *t, dv_part_t *D)
{
    if (D->gate) {
        trk_note_off(t, D->note);
        D->gate = 0;
    }
}

/* one pulse: the rhythms, the sequencers they advance, the envelopes */
static void divseq_pulse(track_t *t, dv_part_t *D, uint32_t period)
{
    const int16_t *p = t->p;
    uint32_t k, fire = 0, root = D->root ? D->root : 48u;
    for (k = 0; k < 4u; k++)
        if (D->pulse % (uint32_t)clamp(p[DV_R1 + k], 1, 16) == 0u)
            fire |= (uint32_t)p[DV_RA1 + k] & 3u;
    D->fired = (uint8_t)fire;
    if (!fire)
        return;
    if (fire & 1u)
        D->step[0] = (uint8_t)((D->step[0] + 1u) & 3u);
    if (fire & 2u)
        D->step[1] = (uint8_t)((D->step[1] + 1u) & 3u);
    divseq_apply(t, D);
    divseq_gate_off(t, D);
    trk_note_on(t, root, 100);
    D->note = (uint8_t)root;
    D->gate = 1;
    D->gate_left = period / 2u > CTL ? period / 2u : CTL;
}

/* a DIVISOR track's sequencer for n samples (seq.c seq_tick); 0 = not a DIVISOR track (Felucca's sequencer runs) */
static int divseq_tick(track_t *t, uint32_t n)
{
    dv_part_t *D;
    uint32_t period;
    if (eng_idx(t->engine) != ENGI_DIVISOR || eng_idx(t->eng_req) != ENGI_DIVISOR)
        return 0;
    D = dv_of(t);
    if (D->gate) {
        if (D->gate_left <= n)
            divseq_gate_off(t, D);
        else
            D->gate_left -= n;
    }
    if (!song.playing) {
        if (D->run) {                                   /* STOP: the note ends, the oscillators back to the knobs */
            D->run = 0;
            divseq_gate_off(t, D);
            divseq_clear(D);
        }
        return 1;
    }
    period = div_samples((uint32_t)t->p[P_SDIV]);
    if (!period)
        period = CTL;
    if (!D->run) {                                      /* PLAY: every rhythm on the first pulse, both from step 1 */
        trk_all_off(t);                                 /* (a key held: the sequencer plays now) */
        D->run = 1;
        D->pulse = 0;
        D->step[0] = D->step[1] = 3;
        D->pos = period;
    } else {
        D->pos += n;
    }
    while (D->pos >= period) {
        D->pos -= period;
        divseq_pulse(t, D, period);
        D->pulse++;
    }
    return 1;
}

/* a key or MIDI note into a DIVISOR track (seq.c input_on / input_off): 1 = taken (the sequencer runs: the root moves,
 * a sounding note glides there), 0 = play it as usual (the root follows it too) */
static int divseq_input(track_t *t, uint32_t note, int on)
{
    dv_part_t *D;
    if (eng_idx(t->engine) != ENGI_DIVISOR || eng_idx(t->eng_req) != ENGI_DIVISOR)
        return 0;
    D = dv_of(t);
    if (on)
        D->root = (uint8_t)note;
    if (!D->run)
        return 0;
    if (on && D->gate && D->note != note) {             /* legato to the new root (no retrigger: LEG / MONO) */
        trk_note_on(t, note, 100);
        trk_note_off(t, D->note);
        D->note = (uint8_t)note;
    }
    return 1;
}
