# Compressor

A dedicated, zero-latency compressor audio FX for
[Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. Built for
live sources on the line input (vocals, acoustic guitar), and general enough
for anything in a Signal Chain or the Master FX.

- **Threshold, Ratio, Attack, Release, Knee, Makeup, Mix** on the knobs
- **Gain-reduction meter** in the knob grid, refreshed every UI frame
- **Curve page**: the transfer curve with the signal's position riding it
- **Sidechain high-pass** so lows and plosives don't pump a vocal
- **Peak or RMS** detection, **auto makeup**, input gain
- **Bypass** switch for A/B comparison, click-free
- **11 factory presets**, plus Schwung's own *My Presets*
- **No lookahead, no added latency**: safe for monitoring a live mic

## Using it

Put **Line In** in a slot, add **Compressor** as an Audio FX after it, pick a
preset, then lower **Threshold** while watching **GR**.

### Pages

Jog through them left to right:

| Page | Knobs |
|---|---|
| **Main** | Thrsh · Ratio · Atk · Rel / Knee · Gain · Mix · **GR** |
| **Curve** | the same eight knobs, drawn as the transfer curve |
| **Presets** | factory presets (jog to browse, click to load) |
| **Setup** | In · HPF · Det · Auto / Byp · **GR** |

| Control | Range | Notes |
|---|---|---|
| Threshold | -60 … 0 dB | where compression starts |
| Ratio | 1 … 20 :1 | 3 means 3 dB over comes out as 1 dB over |
| Attack | 0.1 … 100 ms | time constant (63% of the way) |
| Release | 10 … 2000 ms | time constant |
| Knee | 0 … 24 dB | full width of the soft bend around the threshold; 0 is hard |
| Makeup | -12 … +24 dB | gain after compression |
| Mix | 0 … 100% | dry/wet; below 100% is parallel compression |
| Input | -24 … +24 dB | before everything; presets do not change it |
| Sidechain HPF | Off / 60 / 100 / 150 / 250 Hz | 12 dB/oct, **detector only**; the audio keeps its lows |
| Detector | Peak / RMS | RMS is calibrated so a sine reads the same level in both |
| Auto Makeup | Off / On | sets makeup to half the reduction a 0 dBFS signal would get; **replaces** the Makeup knob while on |
| Bypass | Off / On | passes the input through untouched (no Input, Makeup or Mix either), crossfaded over ~10 ms; presets leave it alone |

### The meter

**GR** is a read-only knob. Its cell shows the current gain change in dB and a
bar filling from the right, with ticks at 3, 6 and 12 dB. Touch knob 8 and the
header reads e.g. `-6.2 dB in -9`: 6.2 dB of reduction, with the detector
seeing -9 dBFS. The **Curve** page draws the same reading as a dot on the
transfer curve, beside a vertical reduction meter.

While **Bypass** is on the compressor keeps listening, so the meter goes on
showing what it *would* be doing: the cell reads `BYP` over a dithered bar,
and the Curve page says BYPASS with a hollow dot. Switch it back and you hear
exactly that, with no settling. For a fair A/B, set Makeup so both positions
sound equally loud first: louder always sounds better.

### Factory presets

| Preset | Threshold | Ratio | Attack | Release | Knee | Makeup | Mix | HPF | Detector |
|---|---|---|---|---|---|---|---|---|---|
| Default | -18 | 3 | 10 | 150 | 6 | 0 | 100% | Off | Peak |
| Vocal Leveler | -20 | 2.5 | 12 | 160 | 8 | +4 | 100% | 100 Hz | RMS |
| Vocal Upfront | -24 | 4 | 4 | 90 | 5 | +7 | 100% | 100 Hz | Peak |
| Vocal Gentle | -16 | 2 | 20 | 250 | 10 | +3 | 100% | 100 Hz | RMS |
| Spoken Word | -26 | 3.5 | 8 | 180 | 8 | +8 | 100% | 150 Hz | RMS |
| Acoustic Strum | -20 | 3 | 25 | 140 | 6 | +4 | 100% | 100 Hz | Peak |
| Fingerpicked | -24 | 2.5 | 15 | 220 | 8 | +5 | 100% | 60 Hz | RMS |
| Parallel Crush | -36 | 10 | 1 | 80 | 0 | +12 | 35% | Off | Peak |
| Peak Catcher | -8 | 12 | 0.5 | 60 | 2 | 0 | 100% | Off | Peak |
| Bus Glue | -16 | 2 | 30 | 300 | 6 | +2 | 100% | 60 Hz | RMS |
| Drum Punch | -20 | 4 | 25 | 70 | 3 | +4 | 100% | Off | Peak |

These are starting points: the right threshold depends on how hot your source
is. Aim for 3-6 dB of GR on the louder phrases of a vocal.

## How it works

A feed-forward, log-domain design (Giannoulis, Massberg & Reiss, *Digital
Dynamic Range Compressor Design — A Tutorial and Analysis*, JAES 2012):

```
in -> Input -+-> sidechain HPF -> Peak|RMS detector -> dB -> gain computer
             |                    (threshold, ratio, soft knee) -> attack/release
             |                    smoothing of the reduction, in dB
             +-> x gain(makeup - reduction) -> dry/wet Mix -> out
```

Stereo-linked (one gain for both channels), no lookahead. Below threshold at
unity settings the output is bit-identical to the input. Input, Makeup and Mix
changes are smoothed (~10 ms) so turning them does not click.

### Notes for Schwung developers

- **The meter is one key.** `gr` answers `"<gain change> dB in <level>"`,
  with ` byp` appended while bypassed. The
  knob grid refreshes `live` params one read per tick *shared between them*,
  so a second meter key would halve the rate of both. The string is also what
  the header shows when the knob is touched, so it is written to be read.
- **The plugin serves its own `chain_params` and `ui_hierarchy`.** The chain
  host's fallback re-serialises module.json's `chain_params` and drops `viz`,
  `live`, `access` and `step` — which would cost the meter its widget, its
  refresh and its read-only frame. `scripts/gen_contract.py` generates the C
  strings from `src/module.json` at build time, so there is one source of
  truth, and `tests/check_contract.py` checks the two agree.
- **The meter cannot freeze.** Schwung stops calling an FX on a slot that has
  been silent for ~1 s, and a bypassed FX is not called at all. After 24
  reads with no audio in between, the meter reports `0.0 dB in --` instead of
  holding the last value of a release.
- Everything runs on the SPI audio callback: no allocation, locks, logging or
  file I/O outside create/destroy.

## Build, test, install

```bash
bash tests/run_tests.sh   # offline: DSP, contract, help text, canvas (needs cc, python3, node)
scripts/build.sh          # cross-compile via Docker -> dist/compressor-module.tar.gz
scripts/install.sh        # scp to move.local (MOVE_HOST=... to override)
```

`CROSS_PREFIX=aarch64-linux-gnu- scripts/build.sh` builds without Docker if
you have the cross-compiler installed.

To see the pages without a Move, render them with a Schwung checkout's own
renderer:

```bash
SCHWUNG_DIR=../schwung node tools/preview.mjs --png build/preview --gr "-6.2 dB in -9"
```

Releases: bump `version` in `src/module.json`, commit, then
`git tag v0.1.0 && git push --tags`. The release workflow builds, tests,
attaches `compressor-module.tar.gz` and updates `release.json`.

## Credits

MIT (see `LICENSE`). `src/dsp/plugin_api_v1.h` and `src/dsp/audio_fx_api_v2.h`
are copied from Schwung (MIT, Charles Vestal), and `tests/font_widths.json`
is derived from Schwung's font table.
