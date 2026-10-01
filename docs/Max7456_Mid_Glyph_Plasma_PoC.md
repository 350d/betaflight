# PoC: Mid-glyph raster effects on MAX7456 / AT7456

**Status:** experimental proof-of-concept (disarmed OSD demoscene only)  
**Branch:** `poc/mid-glyph-plasma` on https://github.com/350d/betaflight  
**Platform tested:** Betaflight `2026.12.0-alpha`, HAKRCF411D (STM32F411 @ 108 MHz, OSD on SPI2), PAL FPV camera, AT7456-class OSD  
**Goal:** show that Display-SRAM character codes can be rewritten *during* an active video field, with beam-synced timing, to get logical resolution finer than the 30×16 character grid.

This is **not** a proposal to ship raster effects in flight builds. It is a timing / SPI / glyph technique demo.

Two scenes use the technique:

| Scene | CLI | What it shows |
|---|---|---|
| 7 | `osd_demo scene7` (`plasma2x2`) | Sine plasma on a full-screen **180×144** grid of 2×2 megapixels |
| 8 | `osd_demo scene8` (`twister`) | Classic vertical square-section twister: 1-px edges, white / dither faces, 2-line bands |

---

## One-line summary

Lock to PAL VSYNC → **measure** the real line period and per-field HSYNC phase → re-lock to HSYNC once per character row → per 2-line band, diff against a mirror of Display SRAM and send only the changed cells → write them **right behind the beam** (scene 7) or inside the previous line (scene 8).

---

## Naming (read this once)

| Name in code | Meaning |
|---|---|
| **2×2** | Logical megapixel: 2 OSD pixels wide × 2 video lines tall. Engine rewrites Display SRAM every 2 lines. |
| **PX22** | Font bank @ `0x40`: 64 solid 6-column masks (`BASE + mask`). Used by scene 7. |
| **XFILL** | Scene 8 bank @ `0x40` (installed on scene entry): 1-px-phase two-run cells (W/B, D/B, W/D …). |
| `OSD_DEMO_MG_*`, `mg*` | Shared mid-glyph layer: video timing model, SRAM mirror, burst planner, stats. |
| `PLASMA2X2_*` | Geometry / wave coefficients for scene 7. |
| `PLASMA_ZEBRA_BANDS` | Posterize levels for the sine field (8) — **not** the same as cell `PLASMA2X2_BANDS` (9). |

---

## Why this is interesting

MAX7456 OSD is normally a **character framebuffer**: 30×16 cells, each cell a fixed 12×18 bitmap from font NVM. Frame updates are usually done in VBLANK via the normal dirty/shadow path.

If you can change the **character index in Display SRAM while the beam is still in that cell**, the remaining pixel rows of the cell show a *different* glyph. That is “mid-glyph” rewrite.

With a small set of column-mask glyphs (**PX22**) and a rewrite every **2 video lines**, you get a 2×2 megapixel grid over the whole screen — enough for readable plasma contours without a framebuffer IC.

---

## Logical resolution (scene 7)

| Layer | Size | Notes |
|---|---|---|
| Character grid | 30 × 16 | standard PAL OSD |
| Pixel cell | 12 × 18 | MAX7456 glyph |
| Megapixel | **2 × 2** OSD pixels | rewrite every 2 lines; 6 columns per cell |
| Full-screen logical | **180 × 144** | 30×6 by 16×9 |

Each PX22 bit lights a 2px-wide column across **all 18** NVM rows, so both lines of a band stay solid. Diagonal contours therefore step by one megapixel (2 px / 2 lines) — that stair-step is the resolution of the effect, not a timing error (a host-side render of the exact masks shows the same pattern).

---

## Timing model (PAL)

```
VSYNC falling edge (STAT[4] poll @ nominal SPI)
  │
  ├─ line period   = IIR( VSYNC→VSYNC / 312.5 )          // measured, not 64 us
  ├─ line(n) start = pivot + (n − 144) · line             // pivot = calibrated top of char row 8
  ├─ field phase   = HSYNC edge − model, measured in VBLANK (13 lines before row 0)
  │                  → also tells field 1 / field 2 (½-line apart)
  ├─ VBLANK: preload band 0 of as many rows as fit
  │
  └─ per char row y:
        HSYNC re-lock in the 3 write-free lines before the row (band 0 is preloaded)
        per band b (2 lines):
            build next band (prefetch) while the beam is ahead
            plan burst  = diff(new band, SRAM mirror)         // only changed cells
            scene 7: start when the beam has passed the first changed cell in line due−1,
                     every cell must land before the beam reaches it in line due
            scene 8: finish inside line due−1 (short bursts)
            check deadline + send burst with IRQs masked (PRIMASK)
```

Engine entry points: `osdDemoPlasma2x2EnginePoll()` (scene 7), `osdDemoTwisterEnginePoll()` (scene 8). Shared helpers: `osdDemoMg*`.

### What each piece fixed (all found on hardware)

1. **Measure the line period — do not assume 64 us.** The test camera's field is **20.41–20.44 ms** (≈ 48.98 Hz): line ≈ **65.3 us** in DWT ticks. A fixed 64 us model drifted ~24 us per character row away from the calibration row; below mid-screen writes landed in lit lines and the twister grew square “corners” toward the bottom. A ±0.5 % sanity gate silently rejected every field and kept the nominal value — the gate is now ±5 % (still rejects a missed VSYNC). `twstat` then reports `line=65.3 us`, `rejected=0`.
2. **Re-lock to HSYNC once per character row.** Per-band STAT HSYNC polling is too slow / jittery, but once per row, in the 3 lines that carry no writes (band 0 comes from VBLANK), it is cheap. Residual drift on hardware: **≤ 3 us** on all 16 rows. STAT misses the short HSYNC pulse ~20 % of the time; the per-row IIR just keeps the previous value.
3. **Field phase per field.** PAL field 2's VSYNC starts mid-line (datasheet Fig. 6/7), so HSYNC vs VSYNC differs by **½ line**; measured: field 1 ≈ −14…−24 us, field 2 ≈ +8…+24 us (31–38 us apart). Averaging the two left race-the-beam ±16 us off in every field. Parity is also used to advance the animation once per frame on the field that starts it.
4. **Diff against a Display-SRAM mirror; send only changed cells.** The datasheet notes (p.40) that an SPI display-memory write colliding with the chip's internal fetch can break up a character for that field — fewer writes, fewer collisions, shorter bursts.
5. **Plan the burst framing per cluster of changed cells** (`osdDemoMgPlanRow`, `max7456EncodeDisplaySramRow`): one auto-increment run (`DMM|AI, DMAL, (DMDI,ca)×n, (DMDI,FF)` = 6 + 2n bytes) or addressed single writes (`DMAL, DMDI` = 4 bytes per cell, `DMM` once after an AI run). `DMAH` only when address bit 8 changes. Greedy is within 0.5 % of a DP optimum. Plasma: **44–46 B** average per band (vs 54 B AI-only, 68 B full row); twister: **14–20 B**.
6. **Race the beam (scene 7).** About half the plasma cells change per band; ~45 B can never finish inside the previous line, but each cell has a full line of window if written *behind* the beam: after the beam left it in line due−1, before it reaches it in line due. Both ends of every segment are checked (AI runs at ~1.3 us/cell are faster than the beam's ~1.78 us/cell; singles are slower).
7. **TX-only SPI burst (F4).** The generic polled transfer waits for RX after every byte through stdperiph flag calls: **1.25 us/byte** at 13.5 MHz. Feeding TXE back-to-back and dropping RX once at the end: **0.60 us/byte**. A pending divisor change is latched first with a harmless `DMAH` write (the bus layer applies divisors lazily), otherwise the first burst after a STAT poll would take the slow path.
8. **Atomic deadline check + burst (PRIMASK).** The audio PWM IRQ runs at the same top priority as gyro EXTI (BASEPRI cannot mask it) and does a 50 Hz tracker tick inside it. A burst stretched by that tick landed under the beam and — worse — taught the per-byte cost estimator a bogus value, mis-planning the next 4–5 bands (torn 1-px lines) or overflowing the start-time maths into a ~20 s wait. Bursts are now atomic (~30 us typical, ≤ 45 us), and IRQ-stretched samples are ignored by the estimator.
9. **First lit line from the same model.** `row0 = VSYNC + 1504 us` was ~200 us (3 lines) late with the measured line; the VBLANK preload ran into row 0 and made its band 1 miss the beam. `row0` is now derived from the pivot model.
10. **VBLANK preload of band 0, prefetch / double-buffered masks, no per-cell INV, uint8 phase** — unchanged from the first PoC.

### Measured on HAKRCF411D (`osd_demo twstat`)

| | Scene 7 (plasma) | Scene 8 (twister) |
|---|---|---|
| Skipped bands | 0 | 0 |
| Avg / max burst | 44–46 B / 70 B | 14–20 B / 30 B |
| SPI cost | ~0.61 us/byte | ~0.59 us/byte |
| Idle (waiting for the beam) | ~38 % of field | ~60 % of field |
| Row drift after HSYNC re-lock | ≤ 3 us | ≤ 2.5 us |

---

## Content

**Scene 7 — plasma**

```
v = sin(x·kx + t) + sin(y·ky + 2t) + sin((x+y)·kd + t)     // three int8 sines: v ∈ [−384, 381]
pixel = ((v + 384) >> 6) & 1                                // 8 zebra bands, no clamp needed
```

LUTs rebuilt once per field (`Sd` mirrored to 512 entries so the diagonal index needs no wrap); per band only 30×6 adds/shifts.

**Scene 8 — twister**

Rotating square cross-section → 4 projected edges → up to 2 front faces (white / dither) per scanline. Cells without an edge are solid; only the ≤ 4 edge cells per band run the 12-px encoder. Cells that contain the silhouette edge *and* a face edge (0.5 %) keep the silhouette exact and use the majority face — no 3-run glyphs exist, and blacking out the middle face showed as black notches. The sway is two fixed-wavelength sines: a time-varying wavelength moves the phase ∝ y (up to 5 px per field at the bottom), and woven fields with different geometry looked like notches.

---

## How to try

Disarmed FC, MAX7456/AT7456 OSD connected:

```text
osd_demo            # auto-cycle 1 → 2 → 3 → 4 → 5 → 7 → 8 → 1
osd_demo scene7     # hold one scene (also scene1..5, scene8, or by name: plasma2x2, twister, …)
osd_demo twstat     # mid-glyph stats for the running scene (resets counters)
```

Live tuning / diagnostics while a mid-glyph scene runs: `twhsync 0|1` (row HSYNC re-lock), `twpair 0|1|off` (which field starts a frame), `twfreeze 0|1` (scene 8 still frame), `twshift <us>`, `twlines 312|312.5|313`, `mgbeam <us>` (HSYNC → first OSD pixel for race-the-beam, default 10).

Relevant code:

- `src/main/io/osd_demo.c` — engines, `osdDemoMg*` shared timing / planner / writers
- `src/main/drivers/max7456.c` — `max7456EncodeDisplaySramRow`, `max7456SendEncodedDisplaySram`, TX-only burst, `max7456MidGlyphSpi*`, VSYNC/HSYNC STAT waits
- `src/main/osd/osd_demo_glyphs.inc`, `osd_demo_font.inc` — PX22 bank and defines

---

## Dead ends (so others do not repeat them)

| Approach | Result |
|---|---|
| Fixed 64 us line period | Drift grows ~24 us per char row on a real camera → corners / shear in the lower half |
| ±0.5 % gate on the measured field | Rejected every field of a 20.4 ms camera; looked like “measured 64.000” |
| Datasheet 8-bit data-only auto-increment (Fig. 21) inside one CS | **Hangs AT7456**; the chip keeps power across an FC reflash and then fails the OSDM probe (init now escapes + VM0 soft-resets) |
| Writing the whole row inside the previous line (scene 7) | ~45–70 B cannot fit in ~57 us; tail lands under the beam |
| Bursts with IRQs enabled | 50 Hz tracker tick inside the audio IRQ stretches bursts and corrupts the SPI cost estimate |
| Cross-field average of HSYNC phase | ½-line field difference → ±16 us error in every field |
| Per-cell INV for 32 glyphs (5-level gray) | AI run fragmentation → late bursts → shear / blocks |
| Even-lit / odd-black glyphs (tear hide) | No shear, but whole frame looks “every other line” |
| Late SPI start (`finishBy − burst`) | Most writes skipped → character-sized blocks |
| STAT HSYNC lock per band | Too slow / jittery; once per char row is enough |
| `uint8` phase via `>> 1` | Visible phase snap every ~5 s |

---

## Limits / honesty

- Busy-loops the CPU for the duration of the effect (demoscene only; **disarmed**).
- Masks IRQs for each burst (≤ ~45 us); fine for a demo, not acceptable next to flight control.
- **PAL only** so far; the model is in lines, but NTSC row count / VBLANK are not wired up.
- TX-only SPI path is STM32F4-only (others fall back to the generic polled transfer).
- `mgbeam` (HSYNC → first OSD pixel) is an estimate from the datasheet's 360 px / ~53 us, not a measurement; race-the-beam tolerates roughly ±15 us.
- The row-0 calibration (`VBLANK_US`, pivot at row 8) is still a constant and should be expressed in lines.
- Normal OSD `drawScreen` must not clobber Display SRAM during the effect.
- Not a replacement for canvas/framebuffer OSD chips; it is a clever abuse of character OSD timing.

---

## Ask to the community

Useful feedback:

- Has anyone shipped mid-field Display-SRAM rewrites on MAX/AT7456 in production features?
- Boards with HSYNC/VSYNC wired to an MCU timer input? Hardware timestamps would replace STAT polling.
- Interest in a minimal “raster canvas” API (band callbacks on top of `osdDemoMg*`) vs one-off demos?

Clips / captures of `osd_demo scene7` / `scene8` and `osd_demo twstat` output on other AT7456 boards are welcome — especially NTSC and F7/H7 SPI clocking.
