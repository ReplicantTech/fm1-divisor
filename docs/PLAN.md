# FM-1 Subharmonicon firmware: plan

Goal: a Felucca fork for the M-VAVE FM-1 (old-chip units, Puya flash) that reproduces the
full Moog Subharmonicon architecture: 2 VCOs, 4 subharmonic oscillators, 2 four-step
sequencers, 4 polyrhythm generators, quantize modes, ladder-style VCF, two EGs.

Name: Divisor (repo ReplicantTech/fm1-divisor). Engine id in code: DIVISOR (eng_divisor.c).

## What we found (2026-11-14)

- Felucca 1.8.2 is GPL-3.0-only, C99, fixed point, 44.1 kHz, 32-sample blocks (CTL).
  4 parts, 8 shared voices, 64-step per-track sequencer with LEN/DIV polymeter.
- Engines are plug-in structs (engine_t): name, 8 parameters (P_E0..P_E7), presets,
  4 HOME knobs, hooks note_on / render / block / amp / post. Adding an engine =
  one eng_*.c file + one line in firmware/src/engines.c ENGINES[] (append only).
- Hardware: 4 encoder knobs, 27 velocity/pressure keys, octave buttons, a few function
  buttons, LEDs per key, colour TFT, USB MIDI + TRS MIDI in, USB audio out.
- Host-side tooling is excellent: tests/hostsim.c renders to WAV on a PC with the exact
  firmware sources, tests/regress.c measures instructions per sample per preset
  (tests/cpu_baseline.txt), tests/target_budget.py counts loop cost in the real pi32v2
  disassembly. This is our CPU test harness, no hardware needed.
- Cost reference: ANALOG (2 polyBLEP oscs + sub + SVF) is about 1200 instr/sample per
  voice, and 8 ANALOG voices on one part cost about 17 % CPU. One Subharmonicon voice
  (6 oscillators + 4-pole filter + 2 EGs) should land near 3000-4500 instr/sample,
  roughly 5-8 % CPU. Analog-style modelling is affordable. Phase 0 confirms this.
- Prior art worth reading: DUODRONE (two-voice drone with -1/-2 octave subs, foma),
  DriftSeq (polymetric sequencer spec), FM-106 (dual-core DSP). Nobody has done a
  subharmonicon yet.
- Build: JieLi clang 4.0.1 for pi32v2, Linux x86-64 native or Docker on macOS, plus three
  files from the JieLi AC79 SDK. tools/get_toolchain.sh fetches it. The cloud container
  is currently blocked from pkgman.jieliapp.com by the network policy; host tests (gcc)
  run here fine. Device builds happen on your machine, or we allow that host later.
- Licence: a Felucca fork stays GPL-3.0-only. Source must be published with any release.

## Architecture mapping

Subharmonicon            -> DIVISOR firmware
VCO1, VCO2 (saw/square)  -> one voice_t with two master phase accumulators, polyBLEP saw/square
SUB1..SUB4 (div 1..16)   -> phase-locked dividers off each VCO's cycle counter, saw output
                            (the original's subs are divider-derived saws)
VCO/SUB mixer            -> 6 level parameters
VCF ladder + EG          -> 4-pole ladder (Huovilainen-lite or cascaded one-poles with
                            feedback, fixed point), AD envelope, EG amount
VCA EG                   -> AD envelope on the voice amp (Felucca's env, or our own)
SEQ1, SEQ2 (4 steps)     -> subseq.c: two 4-step pitch sequencers, each step -2..+2 oct
SEQ ASSIGN buttons       -> SEQ1 -> VCO1 / SUB1 / SUB2 toggles, SEQ2 -> VCO2 / SUB3 / SUB4
RHYTHM1..4 (div 1..16)   -> four clock dividers off the master clock
RHYTHM ASSIGN buttons    -> each rhythm OR-ed into SEQ1 and/or SEQ2 advance + trigger
QUANTIZE (4 modes)       -> none / 12-TET / just / equal-interval, applied to VCO and SEQ pitch
Tempo                    -> Felucca transport + MIDI clock (already there)
Keyboard (no original)   -> 27 keys set the root pitch of VCO1/VCO2 (bonus), hold = drone
MIDI (original has it)   -> notes set root pitch, CCs for knobs via Felucca's MIDI learn

Everything audio-rate and sequencer-rate runs in the audio ISR per block, via the
engine's block hook, so the polyrhythms stay sample-accurate to the block (0.7 ms).

## UI (4 knobs per page, keys as buttons where noted)

HOME     : VCO1 freq, VCO2 freq, CUTOFF, RESONANCE
VCO 1    : wave, SUB1 div, SUB2 div, level
VCO 2    : wave, SUB3 div, SUB4 div, level
MIX      : SUB1, SUB2, SUB3, SUB4 levels
VCF      : cutoff, reso, EG amount, (VCF) attack/decay toggle knob
EG       : VCF attack, VCF decay, VCA attack, VCA decay
SEQ 1    : step 1..4 (quantized display)
SEQ 2    : step 1..4
RHYTHM   : div 1..4
ASSIGN   : black keys toggle the 6 SEQ assigns and 8 RHYTHM assigns, LEDs show state
QUANT    : mode, VCO1 octave, VCO2 octave, tempo

Screen: an oscilloscope-style strip (Felucca's ui_graph.c) plus the 4 knob cards per page,
and a rhythm/seq activity row of 8 dots.

## Phases

### Phase 0: repo + CPU budget test (1-2 sessions)
1. Fork hugelton/Felucca with submodules into a new repo. Keep upstream as a remote.
2. Build Felucca unchanged on your machine, flash one FM-1, confirm it runs. Baseline.
3. Write eng_divisor.c as a bare oscillator bank: 2 VCOs + 4 subs + ladder filter, no
   sequencer, playable from keys. Run tests/regress.c and target_budget.py.
   Decision point: if cost < 12 % CPU, keep the analog model. If not, drop to
   cheaper oscillators (naive saw + one-pole smoothing) or a 2-pole SVF.
4. Host render via hostsim to WAV, listen.

### Phase 1: sound (2-3 sessions)
- Polish the ladder filter (self-oscillation, drive), the two AD envelopes, EG routing.
- Quantize modes. Divider phase-locking so subs do not drift.
- 8 engine parameters are too few for ~30 controls: use a second param block of our
  own (DIVISOR has its own state struct in ENG_MEM, saved in projects via a new store
  record, mirrors how SLICE and FM6 keep extra state).
- Presets: 12 or so.

### Phase 2: polyrhythm sequencer (2 sessions)
- subseq.c: 4 rhythm dividers, 2 four-step sequencers, OR-logic assigns, retrigger
  of both EGs on any assigned rhythm hit, reset on transport start.
- Honour Felucca tempo, MIDI clock in, swing off.
- Host test: render 8 bars, assert the trigger timing against a Python model.

### Phase 3: UI (2-3 sessions)
- Pages above, LEDs for assigns, activity dots, scope.
- Key behaviour: root pitch, hold.
- MIDI learn mapping for every knob.

### Phase 4: device bring-up and iteration
- Flash one unit, keep a second on stock V15 as reference and a third for Felucca.
- Recovery kit ready before the first flash: FM-1-transporter, the official V15 .fwsc.

### Phase 5: release
- Web installer (web/make_site.py), browser emulator if we want one (several forks did),
  manual, GPL source archive with submodules.

## Risks and mitigations
- Brick on flash: old-chip units are the safe case. Always have V15 + transporter ready.
- 8-param engine limit: solved by a side state block, as above. Decide early.
- CPU on one core: budget test in Phase 0 settles it; dual core exists as a fallback.
- Upstream drift: pin to Felucca 1.8.2, merge upstream only at phase boundaries.
- GPL: publish the repo from day one.

## Open questions for you
1. Repo: new GitHub repo under your account? Name?
2. Build machine: Linux, macOS (Docker), or Windows? Determines which build notes I write.
3. Should the DIVISOR part coexist with Felucca's other engines (DIVISOR as engine 15 on a
   normal Felucca) or should the firmware boot straight into subharmonicon mode with
   the other engines removed? Coexist is less work and keeps your other units' habits.
4. Any must-have beyond the original: MIDI out of the sequencer notes, USB audio out
   recording, per-VCO filters?

## Decisions so far
- 2026-11-14: name Divisor; start as a Felucca engine (option A), dedicated front panel (option B) is the end goal,
  so the engine (eng_divisor.c) and the sequencer (divseq.c) stay independent of Felucca's UI.
- Base: Felucca v1.8.2 exactly, upstream kept as the `upstream` git remote. Merge upstream only at phase boundaries.
- Build machine: Arch Linux, native JieLi toolchain 20260730.1, AC79 SDK V1.2.1. Stock Felucca builds and all tests pass.

## Phase 0 results (2026-11-14, host)
- eng_divisor.c: 2 VCOs (polyBLEP saw or square), 4 phase-locked sub dividers (1..16), 4-pole ladder with
  saturating feedback, the track's envelope. Registered as engine 15 (14 stays reserved for ADPCM).
- Spectrum of preset DIVISOR at G4 (392.5 Hz): peaks at f, f/2, f/3, f/4, f/5. The dividers are exact.
- Cost (callgrind, host x86-64, same 4 s mono phrase): divisor_render 20.9 M instructions against ANALOG
  SUB BASS's analog_osc 13.0 M, so one Divisor voice is about 1.6 ANALOG voices. Felucca measured 8
  ANALOG voices at 17 % of the device CPU, so the Divisor voice should take about 3.5 %. Decision: keep
  the analog-style model (saw/square oscillators and a ladder filter). No need for the second core.
- Levels: peaks 3300 to 8200 of 32767 at the voice level, no clipping. MOONDOG (square, deep divisors) is
  quiet; the mixer in Phase 1 fixes the balance.
- Still to measure on the device: tests/target_budget.py's loop count of divisor_render in the pi32v2
  disassembly (BUDGET_UPDATE=1 tests/run_tests.sh after ./build.sh adds it).

## Phase 1 results (2026-11-14, host)
- Sound: six-channel mixer (VCO1 VCO2 SUB1..SUB4), VCF EG (attack, decay, bipolar amount into the ladder cutoff),
  quantizer QNT (OFF, ET12, JI8, ET8) on VCO2's interval, eight presets. Verified on the host: the JUST preset's
  VCO2 at +4 semitones lands on 5/4 (489 Hz over 391 Hz).
- Storage: the ten extra parameters live in the track's P_FM1_ATK..P_FM2_LEVEL slots (DIGITAL's retired operator
  envelopes), so projects, user presets, motion and the editor protocol carry them unchanged. params.c names them
  on a DIVISOR track; PAGES shows them as SUB MIX, SUB MIX 2, VCF EG. P_FM3_ATK..P_FM4_LEVEL (ten) are reserved
  for Phase 2 (rhythm dividers and assigns). The sequencer steps will need a store of their own.
- Known limitation: no favourites for DIVISOR sounds (favorites.c has no free row or byte). Fixed by option B's
  own store.
- Tests: regress (golden renders, health, voices), union (engine memory), editor, ui_test (default, ADPCM and
  CPU2=0 builds), ui_render lint (0 findings), project, upreset, motion, backup: all pass on the host.
- JI8 ratio set (1/1 9/8 5/4 4/3 3/2 5/3 7/4 15/8) is a reasonable guess at Moog's; to check against the
  Subharmonicon manual before Phase 2 uses it for the sequencer steps.

## Level fix (2026-11-14, after device tests 1-7)
- Device tests 1 to 6 passed: exact dividers, lock under vibrato and glide, clean self-oscillation, VCF EG,
  quantizer, mono playing. Test 7: everything was quieter than Felucca, MOONDOG more so, PLUCK very quiet.
- Fix: the mix drives the ladder input twice as hard and the output gain is 4. Presets now measure in ANALOG's
  range on the host. MOONDOG and PLUCK reworked.

## Phase 2 results (2026-11-14, host)
- divseq.c: the Subharmonicon's sequencer section, run from seq.c in place of Felucca's step sequencer on a
  DIVISOR track. Clock = the track's DIV at the song tempo or the external MIDI clock. RHY1..RHY4 divide it by
  1..16; ASSIGN routes each to SEQ 1, SEQ 2, both or off (OR logic). Two 4-step sequencers; ROUTE sends SEQ 1
  to VCO1 / SUB1 / SUB2 and SEQ 2 to VCO2 / SUB3 / SUB4 in any combination. On a VCO a step is a pitch offset
  (RANGE 1, 2 or 5 octaves at full scale) through the quantizer; on a sub it adds step/8 to the division.
  Each advance retriggers both envelopes. Keys and MIDI move the root while it runs.
- Pages under SEQ on a DIVISOR track: SEQ 1, SEQ 2, RHYTHM, ASSIGN, ROUTE (STEP and DETAIL hidden there).
- Storage: steps in P_LN0..P_LN7 (DRUM's lane levels), rhythms and routes in P_FM3_ATK..P_FM4_LEVEL, RANGE in
  P_FTYPE. All saved by projects and user presets, recordable as automation.
- Each preset has its own sequence and polyrhythm.
- tests/divseq_test.c checks 64 pulses of every preset against an independent model: rhythm firing, step
  advance, offsets, note per advance, pulse timing, gate length, keys moving the root, STOP. All pass, as do
  regress, union, ui_test (3 builds), ui_render lint (every engine x page x value), editor, project, upreset,
  motion, backup and chord tests.
- Assumptions to check against the Subharmonicon manual: OR logic only (no XOR), sub step = step/8, JI8 ratios.

## Operability: why option B next
On option A every Subharmonicon control is a knob on some page, reached by tapping SEQ or EDIT several times.
That proves the sound and the logic but is not how the instrument should feel. Option B gives Divisor the whole
panel. Proposal to discuss before building:
- Keys: the 27 keys become buttons. 8 for SEQ 1 / SEQ 2 steps (press and turn a knob to set, LED = playing step),
  4 for the rhythms (LED blinks on each fire), 8 for the rhythm-to-sequencer assigns (lit = on), 6 for the
  sequencer-to-oscillator routes (lit = on), 1 for play / root hold.
- Knobs: four fixed sets selected by the existing page buttons (OSC, MIX, FILTER, SEQ), always one press away.
- Screen: a live view of both sequencers, the four rhythm clocks and the six oscillator pitches.
