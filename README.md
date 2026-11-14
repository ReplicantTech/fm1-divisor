# Divisor

Subharmonicon-style firmware for the M-VAVE FM-1: two VCOs, four subharmonic oscillators,
two four-step sequencers, four polyrhythm generators, quantize modes, a ladder-style filter and
two envelopes. A fork of [Felucca](https://github.com/hugelton/Felucca) 1.8.2.

Status: planning and Phase 0 (CPU budget test). Nothing to install yet.

- [Plan](docs/PLAN.md)
- Building: as Felucca, see [BUILDING.md](BUILDING.md). The original Felucca README is kept as
  [FELUCCA_README.md](FELUCCA_README.md).

Divisor begins as an extra engine inside Felucca and is meant to end as a dedicated instrument
with its own front panel. Installing any firmware on an FM-1 is at your own risk; keep the
official V15 firmware and a recovery tool at hand.

Licence: GPL-3.0-only, as Felucca. The engines (MIT) and the hardware layer (MPL-2.0) keep
their own licences; see [LICENSING.md](LICENSING.md). Divisor is not affiliated with M-VAVE,
Moog or Hügelton Instruments.

You can also try FM-1 Divisor in the browser, no flashing needed, in the free community [FM-1 Emulator](https://fm1-emulator.atst.tools/).
