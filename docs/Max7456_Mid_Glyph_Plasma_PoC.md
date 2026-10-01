# PoC: Mid-glyph 2×2 plasma on MAX7456 / AT7456

**Status:** experimental proof-of-concept (disarmed OSD demoscene only)  
**Branch:** `poc/mid-glyph-plasma` on https://github.com/350d/betaflight  
**Platform tested:** Betaflight `2026.12.0-alpha`, HAKRCF411D (STM32F411), PAL, AT7456-class OSD  
**Goal:** show that Display-SRAM character codes can be rewritten *during* an active video field, with beam-synced timing, to get logical resolution finer than the 30×16 character grid.

This is **not** a proposal to ship plasma in flight builds. It is a timing / SPI / glyph technique demo.

---

## One-line summary

Lock to PAL VSYNC → open-loop DWT deadlines per video line → rewrite a full OSD row’s Display SRAM in the previous line → finish before the lit band → classic sine plasma at **180×144** logical **2×2** pixels (30×16 cells × 6×9 megapixels).

---

## Naming (read this once)

| Name in code | Meaning |
|---|---|
| **2×2** | Logical megapixel: 2 OSD pixels wide × 2 video lines tall. Engine rewrites Display SRAM every 2 lines. |
| **PX22** | Font bank @ `0x40`: 64 solid 6-column masks (`BASE + mask`). Used by the 2×2 plasma path. |
| `PLASMA2X2_*` | Timing / geometry for the 2×2 engine (`STEP`, cell `BANDS`, SPI `BURST`, wave coeffs). |
| `PLASMA_ZEBRA_BANDS` | Posterize levels for the sine field (8) — **not** the same as cell `PLASMA2X2_BANDS` (9). |
| Hatch **2×1** (F7/FD) | Unrelated DIAG/FILL diagonal grain; not the plasma engine. |

Older drafts called this path “2×1”; the working PoC is **2×2 only**.

---

## Why this is interesting

MAX7456 OSD is normally a **character framebuffer**: 30×16 cells, each cell a fixed 12×18 bitmap from font NVM. Frame updates are usually done in VBLANK via the normal dirty/shadow path.

If you can change the **character index in Display SRAM while the beam is still in that cell**, the remaining pixel rows of the cell show a *different* glyph. That is “mid-glyph” rewrite.

With a small set of column-mask glyphs (**PX22**) and a rewrite every **2 video lines**, you get a 2×2 megapixel grid over the whole screen — enough for readable plasma contours without a framebuffer IC.

---

## Logical resolution

| Layer | Size | Notes |
|---|---|---|
| Character grid | 30 × 16 | standard PAL OSD |
| Pixel cell | 12 × 18 | MAX7456 glyph |
| Megapixel (this PoC) | **2 × 2** OSD pixels | rewrite every 2 lines; 6 columns per cell |
| Full-screen logical | **180 × 144** | 30×6 by 16×9 |

**PX22** glyph bank: 64 solid patterns at font index `0x40` (`OSD_DEMO_PX22_BASE + 6-bit column mask`). Each bit lights a 2px-wide column across **all 18** NVM rows so both lines of a 2×2 band stay solid (not every-other-line).

---

## Timing model (PAL)

```
VSYNC falling (STAT poll @ nominal SPI)
    │
    ├─ row0 = edge + VBLANK_US          // first active OSD line
    ├─ VBLANK: preload band 0 for as many rows as fit
    │
    └─ for each char row y, band b in 0..8:          // PLASMA2X2_BANDS == 9
           due      = row0 + (y*18 + b*2) * line_us  // STEP == 2
           wrAt     = due - line_us + pad            // early in previous line
           finishBy = due - pad
           wait until wrAt  (prefetch next band into double-buffer while waiting)
           if (now + burst ≤ finishBy)  SPI AI write 30 cells
```

Engine entry point: `osdDemoPlasma2x2EnginePoll()`.

Key rules that made it stable:

1. **Finish-before-due** — never start a burst that cannot finish before the lit even line. Late SPI into a lit line → horizontal shear (often 1px high, right side).
2. **Early write in the previous line** — start near the beginning of that line so SPI stays *ahead* of the beam. “Latest safe start” (`finishBy − burst`) looked clever but skipped most writes → huge character blocks.
3. **Open-loop DWT from VSYNC** — per-band STAT HSYNC polling was too jittery and sheared the whole frame.
4. **No half-line field bias** — interlaced ±½ line offset put bursts into lit lines.
5. **VBLANK preload of band 0** — avoids burning the first active odd windows on LUT/SPI setup (top-half shear).
6. **Prefetch / double-buffer masks** — build the next band while waiting for `wrAt`, so the hot path is not late because of CPU work.
7. **No per-cell INV in the hot path** — mixed INV fragments the AI burst into many DMM runs; under budget that becomes late/shear. Use 64 direct **PX22** glyphs instead of 32+INV.
8. **Phase as `uint8` ++ every 2 fields** — do **not** animate with `(uint8_t)t >> 1`; wrap jumps 127→0 and snaps the plasma.

SPI: mid-glyph “hot” mode boosts clock for the row burst; STAT VSYNC wait stays at nominal clock (20 MHz STAT polling was unreliable on this board).

---

## Content (plasma)

Cheap classic field:

```
v = sin(x·kx + t) + sin(y·ky + 2t) + sin((x+y)·kd + t)
band = posterize(v)  // PLASMA_ZEBRA_BANDS (8)
pixel = band & 1     // zebra contours
```

Axis LUTs rebuilt once per field; per-band work is only 30×6 bit tests + one AI row write (dirty-skipped if unchanged).

---

## How to try

Disarmed FC, MAX7456/AT7456 OSD connected:

```text
osd_demo scene7
```

or

```text
osd_demo plasma2x2
```

Also appears in the auto demoscene cycle after the Craft-style tunnel scene.

Relevant code:

- `src/main/io/osd_demo.c` — `osdDemoPlasma2x2EnginePoll` and helpers
- `src/main/osd/osd_demo_glyphs.inc` — `osdDemoPx22Nvm[64]` @ `0x40`
- `src/main/osd/osd_demo_font.inc` — `OSD_DEMO_PX22_*` defines
- `src/main/drivers/max7456.c` — `max7456WriteDisplaySramRowGlyphs*`, `max7456MidGlyphSpi*`, VSYNC wait

---

## Dead ends (so others do not repeat them)

| Approach | Result |
|---|---|
| Per-cell INV for 32 glyphs (5-level gray) | AI run fragmentation → late bursts → shear / blocks |
| Even-lit / odd-black glyphs (tear hide) | No shear, but whole frame looks “every other line” (not solid 2×2) |
| Late SPI start (`finishBy − burst`) | Most writes skipped → character-sized blocks |
| STAT HSYNC lock per band | Full-frame shear / jitter |
| `uint8` phase via `>> 1` | Visible phase snap every ~5 s |

---

## Limits / honesty

- Busy-loops the CPU for the duration of the effect (demoscene only; **disarmed**).
- Tuned for **PAL** line timing on F411 + this OSD path; NTSC / other MCUs need re-budgeting.
- Normal OSD `drawScreen` must not clobber Display SRAM during the effect.
- Not a replacement for canvas/framebuffer OSD chips; it is a clever abuse of character OSD timing.
- Gray / still-image experiments were tried and removed from this PoC cycle; binary zebra plasma is the stable demo.

---

## Ask to the community

Useful feedback:

- Has anyone shipped mid-field Display-SRAM rewrites on MAX/AT7456 in production features?
- Cleaner VSYNC/HSYNC source than STAT polling on F4?
- Interest in a minimal “raster canvas” API (band callbacks) vs one-off demos?

Clips / captures of `osd_demo scene7` on other AT7456 boards welcome — especially NTSC and F7/H7 SPI clocking.
