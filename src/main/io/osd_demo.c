/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * MAX7456 demoscene — uses the dedicated osd_demo font
 * (tmp/osd_demo.mcm / osd_demo_glyphs.inc from generate_osd_cellfont.py).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#ifdef USE_MAX7456

#include "common/maths.h"
#include "common/time.h"
#include "common/utils.h"

#include "drivers/display.h"
#include "drivers/max7456.h"
#include "drivers/system.h"
#include "drivers/time.h"
#include "drivers/timer.h"
#include "drivers/nvic.h"
#include "drivers/io.h"
#include "drivers/sound_beeper.h"

#include "osd/osd.h"

#include "config/config.h"
#include "config/feature.h"
#include "fc/runtime_config.h"

#include "pg/displayport_profiles.h"
#include "pg/max7456.h"
#include "pg/vcd.h"

#include "io/osd_demo.h"
#include "io/displayport_max7456.h"

#ifdef USE_CHIPTUNE
#include "io/chiptune.h"
#endif
// Demo ↔ music hooks (autostart, beat clock, AY register tap) need the extended chiptune
// API; with an older chiptune the demo runs silent with a fixed 120 BPM clock.
#if defined(USE_CHIPTUNE) && defined(CHIPTUNE_DEMO_API)
#define OSD_DEMO_CHIPTUNE
#endif

#ifdef USE_CLI
#include "cli/cli.h"
#endif

#include "osd/osd_demo_font.inc"
#include "osd/osd_demo_glyphs.inc"

#define OSD_DEMO_SCROLL_HZ          72  // fine px steps
#define OSD_DEMO_BOUNCE_PERIOD_MS   900
#define OSD_DEMO_CELL_W             12
#define OSD_DEMO_CELL_H             18
#define OSD_DEMO_HOS_SOFT_PX        6   // soft-scroll amplitude (halfway to a full cell)
#define OSD_DEMO_HOP_CELLS          3

#define OSD_DEMO_PUNCH_KICK         96
#define OSD_DEMO_PUNCH_SPRING       5
#define OSD_DEMO_PUNCH_DAMP_NUM     14
#define OSD_DEMO_PUNCH_DAMP_DEN     16
#define OSD_DEMO_PUNCH_MAX_PX       12

#define OSD_DEMO_SHIMMER_PERIOD_MS  1100
#define OSD_DEMO_STAR_BRIGHT_MS     90   // per-step for starfield row white wave
#define OSD_DEMO_RB_ROWS            16

#define OSD_DEMO_STAR_LAYERS        3
#define OSD_DEMO_STARS_PER_LAYER    12
#define OSD_DEMO_PLASMA_STAR_ROWS   3   // starfield band above/below plasma

#define OSD_DEMO_PLASMA_HZ          36
// Wall-clock effect durations (Craft plasma block ~8s).
#define OSD_DEMO_FX_SCROLLER_MS     8000
#define OSD_DEMO_FX_PLASMA_MS       10000
#define OSD_DEMO_PLASMA_FADE_MS     1200
#define OSD_DEMO_FX_FIRE_MS         12000
#define OSD_DEMO_FX_WIPE_MS         10000
#define OSD_DEMO_FX_TUNNEL_MS       20000
#define OSD_DEMO_FX_PLASMA2X2_MS   18000
#define OSD_DEMO_FX_TWISTER_MS     18000
#define OSD_DEMO_FX_SHOUT_MS       40000
#define OSD_DEMO_FX_PLASMA_IRQ_MS  18000
#define OSD_DEMO_FIRE_IGNITE_MS     1500
#define OSD_DEMO_FIRE_FALL_TICKS    4   // ticks per collapsed row (~1.1s for 16 rows @ 72Hz)
#define OSD_DEMO_FIRE_STEP_TICKS    3   // flame physics ~24Hz @ SCROLL_HZ (was every tick)
#define OSD_DEMO_FIRE_COLS          30
#define OSD_DEMO_FIRE_ROWS          16
#define OSD_DEMO_FIRE_SPARKS        4
#define OSD_DEMO_FIRE_CORE_HALF     5
#define OSD_DEMO_FIRE_CURVE_ROWS    2
#define OSD_DEMO_FIRE_CURVE_MAX_PX  OSD_DEMO_CELL_H // bend profile 0..18 on row 1

#define OSD_DEMO_CHARS_PER_LINE         30

typedef enum {
    OSD_DEMO_FX_SCROLLER = 0,
    OSD_DEMO_FX_PLASMA,
    OSD_DEMO_FX_FIRE,
    OSD_DEMO_FX_WIPE,
    OSD_DEMO_FX_TUNNEL,
    OSD_DEMO_FX_PLASMA2X2, // scene 7 — 2×2 mid-glyph plasma
    OSD_DEMO_FX_TWISTER,    // scene 8 — classic B/W ribbon twister (CLI scene8/twister)
    OSD_DEMO_FX_SHOUT,      // scene 9 — FPV community shoutouts (3×3-px raster text)
    OSD_DEMO_FX_PLASMA_IRQ, // scene 10 — scene 7's plasma on the TIM5 interrupt engine
} osdDemoFx_e;

typedef enum {
    OSD_DEMO_FIRE_COLLAPSE = 0,
    OSD_DEMO_FIRE_IGNITE,
    OSD_DEMO_FIRE_BURN,
} osdDemoFirePhase_e;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t size;
} osdDemoStar_t;

static bool active;
static displayPort_t *demoDisplay;
static const char *startLastError;
static uint16_t scrollCol;       // whole-cell text offset
static uint8_t scrollFine;       // 0..HOS_SOFT_PX-1 hardware HOS phase
static uint16_t textPixelCols;
static timeUs_t lastStepUs;
static timeMs_t bounceStartMs;
static timeMs_t fxStartMs;
// Started by scene name from the CLI: stay on this scene (no auto-cycle). Mid-glyph scenes
// (7, 8) just keep running; time-phased scenes restart themselves every period.
static bool fxHold;
#ifdef OSD_DEMO_CHIPTUNE
static bool demoStartedChiptune; // stop the music on osd_demo stop only if the demo started it
#endif
static osdDemoFx_e fx;
static uint16_t prevBouncePhase;
static bool punchArmed;

static int16_t punchY;
static int16_t punchVel;
static uint8_t prevRowBright[OSD_DEMO_RB_ROWS];

static osdDemoStar_t stars[OSD_DEMO_STAR_LAYERS][OSD_DEMO_STARS_PER_LAYER];
static uint16_t starFine[OSD_DEMO_STAR_LAYERS]; // absolute leftward px; synced to HOS

// PAL timing shared by the mid-glyph engines (scene 7 / scene 8).
#define OSD_DEMO_PAL_VSYNC_TIMEOUT_US  30000
#define OSD_DEMO_PAL_LINE_US           64    // PAL line ≈ 64 us
// VSYNC→row0 top (calibrated: Y=8, line=64 → 1504 + 8*18*64 = 10720 us).
#define OSD_DEMO_PAL_VBLANK_US         1504
// PAL field = 312.5 lines.
#define OSD_DEMO_PAL_FIELD_LINES_X2    625

static uint8_t hosSteps; // 1..HOS_SOFT_PX, right-bias soft-scroll range
static uint32_t rngState;
static uint8_t plasmaT; // Craft framecount>>1 analogue
static uint8_t wipePhase; // soft curtain phase
static uint8_t tunnelRot;  // angle scroll
static uint8_t tunnelZoom; // depth scroll
static uint8_t tunnelMw;
static uint8_t tunnelMh;

#define OSD_DEMO_TUNNEL_STATIC_MS  2500  // look forward before first look-back
#define OSD_DEMO_TUNNEL_FWD_MS     4000  // time facing forward
#define OSD_DEMO_TUNNEL_TURN_MS    4000  // 180° Y-rotation (Craft scene angle)
#define OSD_DEMO_TUNNEL_BACK_MS    2000  // time facing back
#define OSD_DEMO_TUNNEL_SWAY_AMP   10    // ±yaw during straight holds (~14°)
#define OSD_DEMO_TUNNEL_SWAY_MS    2200  // full left-right sway period
#define OSD_DEMO_TUNNEL_SWAY_FADE  500   // fade sway out before/after turns
#define OSD_DEMO_TUNNEL_Z         32     // Craft ray z (3.5 fixed forward)
#define OSD_DEMO_TUNNEL_HOLE2     81     // Craft r² black-centre threshold
#define OSD_DEMO_TUNNEL_ANIM_DIV  4      // depth fly (2=fast, 8=slow → mid)
#define OSD_DEMO_TUNNEL_SPIN_DIV  3      // wedge spin (1=fast, 6=slow → mid)
#define OSD_DEMO_TUNNEL_TEXT_DIV  4      // text mega-pixel scroll (keep)
#define OSD_DEMO_TUNNEL_UV_SHIFT  4      // Craft bit4 — same on U and V (even wedges)
#define OSD_DEMO_TUNNEL_DEP_MUL   2      // ×V before shift → depth rings half as long
#define OSD_DEMO_TUNNEL_TEXT_ROWS 3      // center band for 5×5 @ 6×6 mega-pixels
#define OSD_DEMO_TUNNEL_INK_Y0    1      // Visitor 5×5 ink starts at font row 1
#define OSD_DEMO_TUNNEL_ROW_WHITE 2      // labyrinth RB white (max is 3)

// Scene 7 — mid-glyph 2×2 plasma (6 cols × 9 bands per cell) via PAL VSYNC/DWT.
// 2×2 megapixels: rewrite every 2 video lines (half the SPI of full line rate).
#define OSD_DEMO_CHECKER_COLS     30
#define OSD_DEMO_CHECKER_ROWS     16
#define OSD_DEMO_CHECKER_CELL_H   18
#define OSD_DEMO_CHECKER_CELL_W2  6
#define OSD_DEMO_PLASMA2X2_STEP   2   // video lines per megapixel (DIAG-style)
#define OSD_DEMO_PLASMA2X2_BANDS  (OSD_DEMO_CHECKER_CELL_H / OSD_DEMO_PLASMA2X2_STEP) // 9
// Odd-line tear window: full 30-cell row @20 MHz ≈ 28–36 us. Budget must
// cover overhead so the burst FINISHES before the lit even line (due).
// Build the next band only if at least this long remains before the HSYNC of line due−1.
#define OSD_DEMO_RASTER_PREFETCH_US 40
// Scene 9 — FPV shoutouts on the raster engine: 3×3-px logical pixels.
// Scene 10 — scene 7's plasma on the TIM5 interrupt engine.
#define OSD_DEMO_IRQ_NOMINAL_FIELD_US 20000
#define OSD_DEMO_IRQ_VSYNC_LEAD_US  300   // open the STAT window this early before the prediction
#define OSD_DEMO_IRQ_VSYNC_WINDOW_US 1200 // past the predicted edge before giving up (dead-reckon)
#define OSD_DEMO_IRQ_VSYNC_LEAD_MAX_US 4000
#define OSD_DEMO_SHOUT_BAND_LINES   3
#define OSD_DEMO_SHOUT_BANDS        (OSD_DEMO_CELL_H / OSD_DEMO_SHOUT_BAND_LINES) // 6
#define OSD_DEMO_SHOUT_LP_PER_CELL  (OSD_DEMO_CELL_W / 3)                         // 4
#define OSD_DEMO_SHOUT_LP_COLS      (OSD_DEMO_CHARS_PER_LINE * OSD_DEMO_SHOUT_LP_PER_CELL) // 120
#define OSD_DEMO_SHOUT_LP_ROWS      (16 * OSD_DEMO_SHOUT_BANDS)                   // 96
#define OSD_DEMO_SHOUT_FONT_ROWS    5    // 5×5 ink → bands 0..4, band 5 = 3-line gap
#define OSD_DEMO_SHOUT_BASE         0x40 // scene bank (PX22 / XFILL reinstall on their entry)
#define OSD_DEMO_SHOUT_GLYPHS       39   // 16 text masks + 11 left + 11 right bar cells + full
#define OSD_DEMO_SHOUT_BAR_L        16   // bank offset: left bar "k px dither | black"
#define OSD_DEMO_SHOUT_BAR_R        27   // bank offset: right bar "black | k px dither"
#define OSD_DEMO_SHOUT_BAR_FULL     38   // bank offset: full dither cell
#define OSD_DEMO_SHOUT_LIFE_BEATS   8    // quarter notes before a name starts to crumble
#define OSD_DEMO_SHOUT_TRIES        48
#define OSD_DEMO_SHOUT_MAX_EVICT    4    // oldest names dropped per beat to make room
#define OSD_DEMO_SHOUT_ENV_LEVEL    13   // envelope-mode channels count as this volume
#define OSD_DEMO_SHOUT_NOISE_ROWS   4    // noise-only channels flicker in the top bands
#define OSD_DEMO_SHOUT_BAR_DECAY_Q8 256  // bar fall per frame (px << 8): full bar in ~0.5 s
#define OSD_DEMO_SHOUT_MAX_WORDS    64
#define OSD_DEMO_SHOUT_FALLBACK_BEAT_MS 500 // 120 BPM when no chiptune is playing
#define OSD_DEMO_SHOUT_BOUNCE_LINES 6    // VOS hop height (lines, up)
#define OSD_DEMO_SHOUT_BOUNCE_FIELDS 12  // hop duration
#define OSD_DEMO_SHOUT_REBOUND_LINES 2   // small second hop
#define OSD_DEMO_SHOUT_REBOUND_FIELDS 8
// Classic plasma wave coeffs — wide contour bands on 180×144 2×2 grid.
#define OSD_DEMO_PLASMA2X2_KX     3
#define OSD_DEMO_PLASMA2X2_KY     2
#define OSD_DEMO_PLASMA2X2_KD     2
#define OSD_DEMO_PLASMA_ZEBRA_BANDS  8

// Tunnel NVM is 0x00..0x3F — that range includes SYM_BLANK (0x20). Remap that
// pattern to a spare slot so wipe/fill with 0x20 stays fully transparent.
#define OSD_DEMO_TUNNEL_BLANK_IDX 0x20
#define OSD_DEMO_TUNNEL_ALT       0xA6   // free between HFILL_BOT and glyph base

static uint8_t tunnelAnim; // Craft framecount>>1 analogue
static uint8_t tunnelAnimDiv;
static uint8_t tunnelSpinDiv;
static uint8_t tunnelTextDiv;
static uint16_t tunnelScrollMx; // text offset in mega-pixels (1 font px = 1 MP)
static uint16_t tunnelTextCols; // len(tunnel text) * FONT_ADVANCE
static bool hosWrappedThisStep; // scrollFine wrap → defer HOS until SPI pass done

static uint8_t plasmaPhase;    // uint8 sine phase — wrap 255→0 is seamless
static uint8_t plasmaPhaseDiv; // advance phase every 2 fields (smooth, no >>1 jump)
static bool plasma2x2Armed; // mid-glyph PAL engine for scene 7

// Scene 8 — classic vertical twister: rotating square cross-section → 4 projected edges.
// Mid-glyph rewrite: 9 vertical logical px/cell (12×2). Bend is geometric (centerX), not
// mid-field HOS. Always rewrite the full silhouette window — trimming the burst leaves
// stale wide glyphs on the right and reads as torn corners at the end of each band.
#define OSD_DEMO_TWISTER_AMP_PX         56
#define OSD_DEMO_TWISTER_LINE_SPAN      288
#define OSD_DEMO_TWISTER_TWIST_NUM      256
#define OSD_DEMO_TWISTER_ROT_STEP       1
// Geometric sway = two fixed-wavelength sines drifting at different speeds (A + B ≤ AMP).
#define OSD_DEMO_TWISTER_BEND_AMP       12   // total sway px (±), sizes the SPI window margin
#define OSD_DEMO_TWISTER_BEND_AMP_A     8
#define OSD_DEMO_TWISTER_BEND_AMP_B     4
#define OSD_DEMO_TWISTER_BEND_WAVE_A    140  // lines per period
#define OSD_DEMO_TWISTER_BEND_WAVE_B    90
#define OSD_DEMO_TWISTER_BEND_ROT_STEP  3    // wave A phase per field
#define OSD_DEMO_TWISTER_BEND_FLOAT_STEP 2   // wave B phase per field (opposite direction)
#define OSD_DEMO_TWISTER_BAND_PX        2    // 12×2; smooth ribbon curves
#define OSD_DEMO_TWISTER_PAD_US         8
#define OSD_DEMO_TWISTER_WR_LEAD_US     2
#define OSD_DEMO_TWISTER_BURST_US       30
#define OSD_DEMO_TWISTER_WIN_MARGIN_PX  (12 + OSD_DEMO_TWISTER_BEND_AMP)
// PX22 0x40..0x7F during scene: k=1..11 partials (solids via FILL/OFF/plasma dither).
#define OSD_DEMO_TWISTER_XFILL_W_BASE   0x40 // 11: left k white | black
#define OSD_DEMO_TWISTER_XFILL_D_BASE   0x4B // 11: left k dither | black
#define OSD_DEMO_TWISTER_XFILL_DT_BASE  0x56 // 11: left k black | dither
#define OSD_DEMO_TWISTER_XFILL_WT_BASE  0x61 // 11: left k black | white (no INV)
#define OSD_DEMO_TWISTER_XFILL_WD_BASE  0x6C // 11: left k white | dither
#define OSD_DEMO_TWISTER_XFILL_DW_BASE  0x77 // 9:  left k dither | white (k=1..9)
#define OSD_DEMO_TWISTER_XFILL_DW_MAX_K 9
#define OSD_DEMO_TWISTER_GLYPH_W        OSD_DEMO_FILL_BASE
#define OSD_DEMO_TWISTER_GLYPH_B        OSD_DEMO_PIXEL_OFF
// Same TL=WHITE checker as our XFILL dither builders (plasma 0xEE).
#define OSD_DEMO_TWISTER_GLYPH_D        ((uint8_t)(OSD_DEMO_PLASMA_BASE + 3))
enum {
    OSD_DEMO_TWISTER_C_B = 0,
    OSD_DEMO_TWISTER_C_W = 1,
    OSD_DEMO_TWISTER_C_D = 2,
};
static bool twisterArmed;
static uint8_t twisterPhase;
static uint8_t twisterBendPhase;
static uint8_t twisterBendFloat;
static uint8_t twisterGlyphBuf[2][OSD_DEMO_CHARS_PER_LINE];
// Display-SRAM mirror for the twister rows: lets each band send only the changed cells.
#define OSD_DEMO_MG_ROWS_MAX       16
#define OSD_DEMO_MG_RUN_MERGE_GAP  2    // bridge ≤2 clean cells instead of a new run header
#define OSD_DEMO_MG_BURST_FIXED_US 2    // spiSequence + CS overhead per burst
#define OSD_DEMO_MG_LINE_IIR_SHIFT 5    // ~32-field time constant on the measured line period
static uint8_t mgSram[OSD_DEMO_MG_ROWS_MAX][OSD_DEMO_CHARS_PER_LINE];
static uint32_t mgLineQ16;       // measured PAL line period, DWT ticks << 16
// Lines per field ×2 used to turn VSYNC→VSYNC into a line period. 625 = interlaced PAL;
// many FPV cameras send progressive 313-line (626) or 312-line (624) fields: same 20 ms,
// different line → row timing drifts linearly away from the calibration row.
static uint16_t mgFieldHalfLines = OSD_DEMO_PAL_FIELD_LINES_X2;
static int16_t mgShiftUs;        // constant write-time shift (CLI twshift)
// Interlace: both fields of one frame must carry the same geometry, otherwise the monitor
// weaves two different ribbons line-by-line and edges show square notches. Advance the
// animation once per frame (2 fields, doubled step); pair parity selectable (CLI twpair).
#define OSD_DEMO_TWISTER_PAIR_OFF       2
static uint8_t twisterPairParity;     // 0/1 = advance on even/odd field, 2 = every field
static bool twisterFreeze;            // CLI twfreeze: still frame for diagnosis
// HSYNC lock: per char row, the measured beam phase relative to row 1 (DWT ticks, IIR).
// Row bands of row r are scheduled with mgRowCorr[r] added, so a beam-vs-model drift
// can never accumulate over more than one character row.
static bool mgHsyncLock = true;
static int32_t mgRowCorr[OSD_DEMO_MG_ROWS_MAX];
static uint8_t mgRowCorrN[OSD_DEMO_MG_ROWS_MAX];
static uint32_t mgHsyncMiss;
static uint32_t mgFieldLastTicks; // raw VSYNC→VSYNC, accepted or not
static uint32_t mgFieldReject;
static bool mgLineSeeded;
// VBLANK_US was calibrated at the top of char row 8 — pivot the line-period model there so
// changing the period does not move that calibrated row.
#define OSD_DEMO_MG_PIVOT_LINE     (8u * OSD_DEMO_CELL_H)
static uint32_t mgLastEdgeTicks;
static uint32_t mgByteTicksQ8;   // learned SPI cost per byte, DWT ticks << 8
static uint32_t mgBurstFixedTicks;
static uint32_t mgStatFields;
static uint32_t mgStatWrites;
static uint32_t mgStatSkips;
static uint16_t mgStatMaxBytes;
static uint16_t mgStatSkipRow[OSD_DEMO_MG_ROWS_MAX];
static uint64_t mgStatIdleTicks;
static timeMs_t mgStatSinceMs;   // stats window start (fields per second in twstat)
static uint32_t mgStatBytes;
static uint32_t mgStatSlowBursts;  // bursts stretched by an IRQ (not learned from)
static uint8_t mgSlowRun;
// Beam position model for race-the-beam writes (scene 7): HSYNC falling edge → first OSD
// pixel, and one 12-px cell. 360 OSD px span ~53 us of the active line (27 MHz / 4 pixel
// clock) → 1.778 us per cell. x0 is tunable live (CLI mgbeam); the window per cell is a
// whole line, so ±15 us of error here is tolerated.
#define OSD_DEMO_MG_BEAM_X0_US_DEFAULT  10
#define OSD_DEMO_MG_BEAM_CELL_NS        1778
#define OSD_DEMO_MG_CHASE_MARGIN_US     3
static int16_t mgBeamX0Us = OSD_DEMO_MG_BEAM_X0_US_DEFAULT;
static int32_t mgHsyncPhase;     // THIS field: HSYNC edge − model line start, wrapped ±½ line
static int32_t mgHsyncPhaseBy[2]; // last measured phase per field parity (fallback on a miss)
static bool mgHsyncPhaseByValid[2];
static bool mgFieldFirst;        // this field is field 1 (HSYNC aligned with the VSYNC edge)
#define OSD_DEMO_MG_FIELD_PHASE_LINES 13 // measure in VBLANK, after the equalizing pulses


static uint8_t fireHeat[OSD_DEMO_FIRE_ROWS][OSD_DEMO_FIRE_COLS];
static uint8_t fireTipFine[OSD_DEMO_FIRE_COLS]; // 0..STAR_FRAMES-1 pixel phase for tip stars
static uint8_t fireTipMask[OSD_DEMO_FIRE_COLS]; // row of tip star this frame (0xFF = none)
static osdDemoFirePhase_e firePhase;
static uint8_t fireFall;     // rows already collapsed toward bottom
static uint8_t fireTick;     // collapse / step sub-tick
static timeMs_t fireIgniteMs;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t fine;  // horizontal star frame
    uint8_t life;  // 0 = dead
    uint8_t size;  // 2 or 3
    uint8_t spd;   // rise period in ticks (1=fast .. 3=slow)
    uint8_t acc;   // ticks since last rise
} osdDemoFireSpark_t;

static osdDemoFireSpark_t fireSparks[OSD_DEMO_FIRE_SPARKS];
static uint8_t fireFlameTop;       // smoothed highest tongue row (for RB gradient)
static uint8_t fireFlameTopRaw;    // instantaneous highest tongue
static uint8_t fireTopHold;        // lag counter when tip falls
static uint8_t fireFuelAvg[OSD_DEMO_FIRE_COLS]; // smoothed base fuel per column
static uint8_t fireNext[OSD_DEMO_FIRE_ROWS][OSD_DEMO_FIRE_COLS]; // rise scratch
static uint8_t fireDrawn[OSD_DEMO_FIRE_ROWS][OSD_DEMO_FIRE_COLS]; // last painted cell code
static uint8_t fireCurveH[OSD_DEMO_FIRE_COLS]; // top-ribbon bend profile px 0..CELL_H
static uint8_t fireCurvePhase;
static uint8_t fireCurveBright; // RB white 0..3 for curve rows

// Craft data.S sine table (256 signed bytes). Stored raw; cast on lookup.
static const uint8_t osdDemoSineU8[256] = {
    0x00, 0x03, 0x06, 0x09, 0x0c, 0x10, 0x13, 0x16, 0x19, 0x1c, 0x1f, 0x22, 0x25, 0x28, 0x2b, 0x2e,
    0x31, 0x33, 0x36, 0x39, 0x3c, 0x3f, 0x41, 0x44, 0x47, 0x49, 0x4c, 0x4e, 0x51, 0x53, 0x55, 0x58,
    0x5a, 0x5c, 0x5e, 0x60, 0x62, 0x64, 0x66, 0x68, 0x6a, 0x6b, 0x6d, 0x6f, 0x70, 0x71, 0x73, 0x74,
    0x75, 0x76, 0x78, 0x79, 0x7a, 0x7a, 0x7b, 0x7c, 0x7d, 0x7d, 0x7e, 0x7e, 0x7e, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 0x7e, 0x7e, 0x7d, 0x7d, 0x7c, 0x7b, 0x7a, 0x7a, 0x79, 0x78, 0x76,
    0x75, 0x74, 0x73, 0x71, 0x70, 0x6f, 0x6d, 0x6b, 0x6a, 0x68, 0x66, 0x64, 0x62, 0x60, 0x5e, 0x5c,
    0x5a, 0x58, 0x55, 0x53, 0x51, 0x4e, 0x4c, 0x49, 0x47, 0x44, 0x41, 0x3f, 0x3c, 0x39, 0x36, 0x33,
    0x31, 0x2e, 0x2b, 0x28, 0x25, 0x22, 0x1f, 0x1c, 0x19, 0x16, 0x13, 0x10, 0x0c, 0x09, 0x06, 0x03,
    0x00, 0xfd, 0xfa, 0xf7, 0xf4, 0xf0, 0xed, 0xea, 0xe7, 0xe4, 0xe1, 0xde, 0xdb, 0xd8, 0xd5, 0xd2,
    0xcf, 0xcd, 0xca, 0xc7, 0xc4, 0xc1, 0xbf, 0xbc, 0xb9, 0xb7, 0xb4, 0xb2, 0xaf, 0xad, 0xab, 0xa8,
    0xa6, 0xa4, 0xa2, 0xa0, 0x9e, 0x9c, 0x9a, 0x98, 0x96, 0x95, 0x93, 0x91, 0x90, 0x8f, 0x8d, 0x8c,
    0x8b, 0x8a, 0x88, 0x87, 0x86, 0x86, 0x85, 0x84, 0x83, 0x83, 0x82, 0x82, 0x82, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x82, 0x82, 0x82, 0x83, 0x83, 0x84, 0x85, 0x86, 0x86, 0x87, 0x88, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8f, 0x90, 0x91, 0x93, 0x95, 0x96, 0x98, 0x9a, 0x9c, 0x9e, 0xa0, 0xa2, 0xa4,
    0xa6, 0xa8, 0xab, 0xad, 0xaf, 0xb2, 0xb4, 0xb7, 0xb9, 0xbc, 0xbf, 0xc1, 0xc4, 0xc7, 0xca, 0xcd,
    0xcf, 0xd2, 0xd5, 0xd8, 0xdb, 0xde, 0xe1, 0xe4, 0xe7, 0xea, 0xed, 0xf0, 0xf4, 0xf7, 0xfa, 0xfd,
};
// Total leftward px/tick (layer 0 matches scroller HOS rate = 1).
static const uint8_t starSpeedPx[OSD_DEMO_STAR_LAYERS] = { 1, 2, 3 };
static const uint8_t starLayerSize[OSD_DEMO_STAR_LAYERS] = { 2, 2, 3 };

static uint16_t osdDemoTextLen(void)
{
    return (uint16_t)strlen(osdDemoScrollText);
}

static uint16_t osdDemoTunnelTextLen(void)
{
    return (uint16_t)strlen(osdDemoTunnelScrollText);
}

static bool osdDemoFontPixel(uint8_t code, uint8_t px, uint8_t py)
{
    if (code >= OSD_DEMO_FONT_GLYPHS || px >= OSD_DEMO_FONT_W || py >= OSD_DEMO_FONT_H) {
        return false;
    }
    return (osdDemoFontBits[code][py] & (0x80 >> px)) != 0;
}

static bool osdDemoTextOnFrom(const char *text, uint16_t len, uint16_t pixelCol, uint8_t pixelRow)
{
    if (len == 0 || pixelRow >= OSD_DEMO_FONT_H) {
        return false;
    }
    const uint16_t advance = OSD_DEMO_FONT_ADVANCE;
    const uint8_t px = (uint8_t)(pixelCol % advance);
    if (px >= OSD_DEMO_FONT_W) {
        return false;
    }
    const uint8_t code = (uint8_t)text[(pixelCol / advance) % len];
    return osdDemoFontPixel(code, px, pixelRow);
}

static bool osdDemoTextOn(uint16_t pixelCol, uint8_t pixelRow)
{
    return osdDemoTextOnFrom(osdDemoScrollText, osdDemoTextLen(), pixelCol, pixelRow);
}

static bool osdDemoTunnelTextOn(uint16_t pixelCol, uint8_t pixelRow)
{
    return osdDemoTextOnFrom(osdDemoTunnelScrollText, osdDemoTunnelTextLen(), pixelCol, pixelRow);
}

static uint32_t osdDemoRand(void)
{
    uint32_t x = rngState ? rngState : 0xA5A5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

// Every demo glyph is a simple mask — build them at install time instead of
// storing 54-byte NVM payloads in flash. Pixels: 00 black, 10 white, 01 transparent.
#define OSD_DEMO_PX_B 0u
#define OSD_DEMO_PX_T 1u
#define OSD_DEMO_PX_W 2u

typedef enum {
    OSD_DEMO_GEN_BLANK = 0,
    OSD_DEMO_GEN_EFFECT,    // 0xC0.. : off, 18 fills, stars ×2 sizes, plasma dithers, streaks
    OSD_DEMO_GEN_HFILL,     // top k rows white over transparent
    OSD_DEMO_GEN_HFILL_BOT, // bottom k rows white over transparent
    OSD_DEMO_GEN_TUNNEL,    // 2×3 mega-pixels of 6×6, bit = 2*row + col
    OSD_DEMO_GEN_PX22,      // 6 columns of 2 px, all rows
} osdDemoGlyphGen_e;

static uint8_t osdDemoEffectPixel(uint8_t idx, uint8_t r, uint8_t c)
{
    if (idx == 0) {
        return OSD_DEMO_PX_B;                                   // PIXEL_OFF
    }
    if (idx <= OSD_DEMO_SOFT_STEPS) {
        return (r < idx - 1u) ? OSD_DEMO_PX_B : OSD_DEMO_PX_W;  // FILL_BASE + k: k black rows on top
    }
    const int8_t col = (int8_t)c;
    if (idx < OSD_DEMO_STAR3_BASE - OSD_DEMO_GLYPH_BASE) {      // 2×2 star, 12 frames
        const int8_t x0 = (int8_t)(10 - (idx - (OSD_DEMO_STAR2_BASE - OSD_DEMO_GLYPH_BASE)));
        return (r >= 8 && r <= 9 && col >= x0 && col < x0 + 2) ? OSD_DEMO_PX_W : OSD_DEMO_PX_B;
    }
    if (idx < OSD_DEMO_PLASMA_BASE - OSD_DEMO_GLYPH_BASE) {     // 3×3 star, 12 frames
        const int8_t x0 = (int8_t)(9 - (idx - (OSD_DEMO_STAR3_BASE - OSD_DEMO_GLYPH_BASE)));
        return (r >= 7 && r <= 9 && col >= x0 && col < x0 + 3) ? OSD_DEMO_PX_W : OSD_DEMO_PX_B;
    }
    bool on;
    switch (idx - (OSD_DEMO_PLASMA_BASE - OSD_DEMO_GLYPH_BASE)) {
    case 0: on = (r % 3u == 0) && (c % 3u == 0); break;        // plasma dither levels
    case 1: on = (r % 2u == 0) && (c % 3u == 0); break;
    case 2: on = (r % 2u == 0) && (c % 2u == 0); break;
    case 3: on = ((r + c) & 1u) == 0; break;
    case 4: return (r == 17) ? OSD_DEMO_PX_W : OSD_DEMO_PX_T; // STREAK_BOT1
    case 5: return (r >= 16) ? OSD_DEMO_PX_W : OSD_DEMO_PX_T; // STREAK_BOT2
    default: return (r == 0) ? OSD_DEMO_PX_W : OSD_DEMO_PX_T; // STREAK_TOP1
    }
    return on ? OSD_DEMO_PX_W : OSD_DEMO_PX_B;
}

static uint8_t osdDemoGenPixel(osdDemoGlyphGen_e kind, uint8_t idx, uint8_t r, uint8_t c)
{
    switch (kind) {
    case OSD_DEMO_GEN_EFFECT:
        return osdDemoEffectPixel(idx, r, c);
    case OSD_DEMO_GEN_HFILL:
        return (r < idx) ? OSD_DEMO_PX_W : OSD_DEMO_PX_T;
    case OSD_DEMO_GEN_HFILL_BOT:
        return (r >= OSD_DEMO_CELL_H - idx) ? OSD_DEMO_PX_W : OSD_DEMO_PX_T;
    case OSD_DEMO_GEN_TUNNEL:
        return ((idx >> (2u * (r / 6u) + c / 6u)) & 1u) ? OSD_DEMO_PX_W : OSD_DEMO_PX_B;
    case OSD_DEMO_GEN_PX22:
        return ((idx >> (c / 2u)) & 1u) ? OSD_DEMO_PX_W : OSD_DEMO_PX_B;
    default:
        return OSD_DEMO_PX_T;
    }
}

static bool osdDemoWriteGenGlyph(uint8_t addr, osdDemoGlyphGen_e kind, uint8_t idx)
{
    uint8_t nvm[OSD_DEMO_GLYPH_BYTES];
    uint8_t *dst = nvm;
    for (uint8_t r = 0; r < OSD_DEMO_CELL_H; r++) {
        for (uint8_t c = 0; c < 12; c += 4) {
            uint8_t v = 0;
            for (uint8_t k = 0; k < 4; k++) {
                v = (uint8_t)((v << 2) | osdDemoGenPixel(kind, idx, r, (uint8_t)(c + k)));
            }
            *dst++ = v;
        }
    }
    return max7456WriteNvm(addr, nvm);
}

static bool osdDemoWritePx22Glyphs(void)
{
    bool ok = true;
    for (uint8_t i = 0; ok && i < OSD_DEMO_PX22_GLYPHS; i++) {
        ok = osdDemoWriteGenGlyph((uint8_t)(OSD_DEMO_PX22_BASE + i), OSD_DEMO_GEN_PX22, i);
    }
    return ok;
}

static bool osdDemoInstallFont(void)
{
    // Undo older demos that stomped SYM_BLANK (0x20) — BF fills the screen with it.
    bool ok = osdDemoWriteGenGlyph(0x20, OSD_DEMO_GEN_BLANK, 0);
    for (uint8_t i = 0; ok && i < OSD_DEMO_GLYPH_COUNT; i++) {
        ok = osdDemoWriteGenGlyph((uint8_t)(OSD_DEMO_GLYPH_BASE + i), OSD_DEMO_GEN_EFFECT, i);
    }
    for (uint8_t i = 0; ok && i < OSD_DEMO_HFILL_STEPS; i++) {
        ok = osdDemoWriteGenGlyph((uint8_t)(OSD_DEMO_HFILL_BASE + i), OSD_DEMO_GEN_HFILL, i)
          && osdDemoWriteGenGlyph((uint8_t)(OSD_DEMO_HFILL_BOT_BASE + i), OSD_DEMO_GEN_HFILL_BOT, i);
    }
    ok = ok && osdDemoWritePx22Glyphs();
    for (uint8_t i = 0; ok && i < OSD_DEMO_TUNNEL_GLYPHS; i++) {
        uint8_t addr = (uint8_t)(OSD_DEMO_TUNNEL_BASE + i);
        if (addr == OSD_DEMO_TUNNEL_BLANK_IDX) {
            addr = OSD_DEMO_TUNNEL_ALT; // keep SYM_BLANK transparent
        }
        ok = osdDemoWriteGenGlyph(addr, OSD_DEMO_GEN_TUNNEL, i);
    }
    // Always clear fontIsLoading — WriteNvm leaves it set until EndFontWrite.
    max7456EndFontWrite();
    return ok;
}

static void osdDemoInitStars(uint8_t cols, uint8_t rows)
{
    for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
        starFine[layer] = 0;
        for (uint8_t i = 0; i < OSD_DEMO_STARS_PER_LAYER; i++) {
            stars[layer][i].x = (uint8_t)(osdDemoRand() % cols);
            stars[layer][i].y = (uint8_t)(osdDemoRand() % rows);
            stars[layer][i].size = starLayerSize[layer];
        }
    }
}

static uint16_t osdDemoBouncePhase(void)
{
    const timeMs_t elapsed = millis() - bounceStartMs;
    return (uint16_t)((elapsed % OSD_DEMO_BOUNCE_PERIOD_MS) * 256 / OSD_DEMO_BOUNCE_PERIOD_MS);
}

static uint16_t osdDemoBounceFine(uint16_t hopFine, uint16_t phase)
{
    const uint32_t h = (4u * (uint32_t)phase * (256u - phase) * hopFine) / (256u * 256u);
    return (uint16_t)constrain(h, 0, hopFine);
}

static void osdDemoUpdatePunch(uint16_t fine, uint16_t phase)
{
    // Re-arm once the soft-hop is clearly airborne.
    if (fine >= 3) {
        punchArmed = true;
    }

    // Floor impact: phase wraps 255→0 once per bounce. Also catch a descending
    // sample into the floor zone in case a paint lands exactly on the trough.
    const bool phaseWrapped = (phase < prevBouncePhase);
    const bool troughHit = punchArmed && (fine <= 1) && !phaseWrapped;
    if (punchArmed && (phaseWrapped || troughHit)) {
        punchVel += OSD_DEMO_PUNCH_KICK;
        punchArmed = false;
    }
    prevBouncePhase = phase;

    punchVel -= (int16_t)(punchY / OSD_DEMO_PUNCH_SPRING);
    punchVel = (int16_t)((punchVel * OSD_DEMO_PUNCH_DAMP_NUM) / OSD_DEMO_PUNCH_DAMP_DEN);
    punchY += punchVel;
    if (punchY < 0) {
        punchY = 0;
        punchVel = 0;
    }

    int8_t maxY = 0;
    max7456GetHudMotionYLimits(NULL, &maxY);
    const int maxPunch = (int)constrain(OSD_DEMO_PUNCH_MAX_PX, 0, (int)maxY) * 8;
    if (punchY > maxPunch) {
        punchY = (int16_t)maxPunch;
        if (punchVel > 0) {
            punchVel = 0;
        }
    }
}

// HOS soft-scroll: +6..+1 right bias (half-cell staging), ease toward +1, then
// cell-step + snap back to +6. Smaller than a full cell so the left margin stays sane.
// HOS is applied by max7456DrawScreen at end-of-pass (not immediately).
static uint8_t osdDemoHosSteps(void)
{
    int8_t maxX = 0;
    max7456GetHudMotionXLimits(NULL, &maxX);
    return (uint8_t)constrain((int)maxX, 1, (int)OSD_DEMO_HOS_SOFT_PX);
}

static void osdDemoApplyHudMotion(void)
{
    hosSteps = osdDemoHosSteps();
    const uint8_t fine = (scrollFine >= hosSteps) ? (uint8_t)(hosSteps - 1) : scrollFine;
    // fine=0 → hos=+hosSteps; fine→hosSteps-1 → hos=+1.
    const int hosX = (int)hosSteps - (int)fine;
    max7456SetHudMotionOffset((int8_t)hosX, (int8_t)(punchY / 8));
}

// Hardware per-row brightness: scroller ridge + starfield white breathing.
static void osdDemoApplyRowShimmer(uint8_t y0, uint8_t drawH, uint8_t rows)
{
    const uint8_t span = (drawH > 1) ? (uint8_t)(drawH - 1) : 0;
    const timeMs_t elapsed = millis() - bounceStartMs;
    uint8_t peak = 0;
    if (span > 0) {
        const uint16_t path = (uint16_t)(span * 2); // up then down
        uint16_t p = (uint16_t)((elapsed % OSD_DEMO_SHIMMER_PERIOD_MS) * path / OSD_DEMO_SHIMMER_PERIOD_MS);
        peak = (p <= span) ? (uint8_t)p : (uint8_t)(path - p);
    }

    const uint8_t limit = (rows < OSD_DEMO_RB_ROWS) ? rows : OSD_DEMO_RB_ROWS;
    for (uint8_t row = 0; row < limit; row++) {
        uint8_t black = 0;
        uint8_t white;
        if (row >= y0 && row < (uint8_t)(y0 + drawH)) {
            const uint8_t local = (uint8_t)(row - y0);
            const uint8_t dist = (local > peak) ? (uint8_t)(local - peak) : (uint8_t)(peak - local);
            if (dist == 0) {
                white = 3;
            } else if (dist == 1) {
                white = 2;
            } else if (dist == 2) {
                white = 1;
            } else {
                white = 0;
            }
        } else {
            // Starfield rows: white level waves independently per row.
            const uint8_t phase = (uint8_t)((elapsed / OSD_DEMO_STAR_BRIGHT_MS) + row * 3);
            static const uint8_t starWhiteWave[8] = { 0, 1, 2, 3, 2, 1, 0, 1 };
            white = starWhiteWave[phase & 7];
        }
        const uint8_t reg = (uint8_t)((black << 2) | (3 - white));
        if (prevRowBright[row] != reg) {
            prevRowBright[row] = reg;
            max7456BrightnessRow(row, black, white);
        }
    }
}

static void osdDemoWriteCell(uint8_t x, uint8_t y, uint8_t rows, uint8_t glyph, bool invert)
{
    if (y < rows) {
        max7456WriteCharEx(x, y, glyph, invert);
    }
}

static uint8_t osdDemoStarGlyph(uint8_t size, uint8_t frame)
{
    const uint8_t base = (size >= 3) ? OSD_DEMO_STAR3_BASE : OSD_DEMO_STAR2_BASE;
    return (uint8_t)(base + (frame % OSD_DEMO_STAR_FRAMES));
}

// Craft plasma.S calcpixel — 3 radial sines, (sum>>5)&3 expanded to 8 dither levels.
static uint8_t osdDemoSqHi(int8_t v)
{
    const int16_t p = (int16_t)v * (int16_t)v;
    return (uint8_t)((uint16_t)p >> 8);
}

static int8_t osdDemoSineAt(uint8_t idx)
{
    return (int8_t)osdDemoSineU8[idx];
}

static uint8_t osdDemoPlasmaLevel(int8_t x, int8_t y, uint8_t t)
{
    // Blob 1: (x-t, y-14), r²>>8 *4 → sine>>2
    uint8_t idx = (uint8_t)(osdDemoSqHi((int8_t)(x - (int8_t)t)) + osdDemoSqHi((int8_t)(y - 14)));
    idx = (uint8_t)(idx << 2);
    int16_t sum = (int16_t)(osdDemoSineAt(idx) >> 2);

    // Blob 2: (x-54, y-t), *8
    idx = (uint8_t)(osdDemoSqHi((int8_t)(x - 54)) + osdDemoSqHi((int8_t)(y - (int8_t)t)));
    idx = (uint8_t)(idx << 3);
    sum += (int16_t)(osdDemoSineAt(idx) >> 2);

    // Blob 3: (x-97, y-16), *8
    idx = (uint8_t)(osdDemoSqHi((int8_t)(x - 97)) + osdDemoSqHi((int8_t)(y - 16)));
    idx = (uint8_t)(idx << 3);
    sum += (int16_t)(osdDemoSineAt(idx) >> 2);

    // Craft: (sum>>5)&3 for 4 palette slots. Stretch onto 8 tileable dithers.
    int16_t v = sum + 96; // ~0..189
    if (v < 0) {
        v = 0;
    }
    if (v > 189) {
        v = 189;
    }
    return (uint8_t)((v * (OSD_DEMO_PLASMA_LEVELS - 1)) / 189);
}

static void osdDemoPaintPlasma(void);
static void osdDemoPaintScroller(void);
static void osdDemoPaintFire(void);
static void osdDemoPaintWipe(void);
static void osdDemoPaintTunnel(void);
static void osdDemoPlasma2x2EnginePoll(void);
static void osdDemoTwisterEnginePoll(void);
static void osdDemoShoutEnginePoll(void);
static void osdDemoShoutEnter(void);
static void osdDemoPlasmaIrqStop(void);
static bool osdDemoPlasmaIrqStart(void);
static void osdDemoPlasmaIrqTask(void);
static bool osdDemoFxIsMidGlyph(osdDemoFx_e f);
static void osdDemoPaintTwister(void);
static void osdDemoWaitCycles(uint32_t deadlineTicks);
// One planned race-the-beam burst and its start window (DWT ticks).
typedef struct osdDemoMgBurst_s {
    max7456SramSeg_t seg[OSD_DEMO_CHARS_PER_LINE];
    const uint8_t *glyphs;
    uint8_t row;
    uint8_t nSeg;
    uint16_t bytes;
    uint32_t startAt;
    uint32_t latest;
} osdDemoMgBurst_t;
static bool osdDemoMgChasePrepare(uint8_t row, const uint8_t *glyphs, uint8_t cols,
                                  uint32_t hsyncPrev, uint32_t lineTicks, osdDemoMgBurst_t *bu);
static bool osdDemoMgBurstSend(const osdDemoMgBurst_t *bu);
static void osdDemoMgResetStats(void);
static void osdDemoMgTrackLinePeriod(uint32_t edgeTicks, uint32_t nominalLineTicks);
static inline int32_t osdDemoMgLineOffset(uint16_t line, uint32_t lineQ16);
static bool osdDemoMgWriteRow(uint8_t row, const uint8_t *glyphs, uint8_t winL, uint8_t winR,
                              uint32_t finishBy, bool checkDeadline, bool maskIrq);
static bool osdDemoMgWriteRowChase(uint8_t row, const uint8_t *glyphs, uint8_t cols,
                                   uint32_t hsyncPrev, uint32_t lineTicks);
static void osdDemoMgHsyncMeasure(uint8_t row, uint32_t pivot, uint32_t lineQ16, int32_t *ref,
                                  bool *haveRef);
static uint32_t osdDemoMgHsyncAt(uint16_t line, uint32_t pivot, uint32_t lineQ16);
static void osdDemoMgMeasureFieldPhase(uint32_t vsyncEdge, uint32_t row0, uint32_t lineTicks);

// Drain SPI after the CPU shadow is ready. waitVsync=true: wait then flush
// (EnterFx / post-paint). Never put heavy paint BETWEEN vsync and flush —
// tunnel math alone burns blanking and the beam tears the text band.
static void osdDemoSyncFlush(bool waitVsync, bool applyHosEarly)
{
    if (waitVsync) {
        (void)max7456WaitVsyncFallingEdge(NULL, OSD_DEMO_PAL_VSYNC_TIMEOUT_US);
    }
    if (applyHosEarly && !hosWrappedThisStep) {
        max7456ApplyHudMotionNow();
    }
    while (max7456DrawScreen()) {
    }
}

// Shared soft-scroll phase: HOS + star parallax stay continuous across effects.
// Plasma/wipe: freeze scrollFine and clear HOS. Fire: stars only — paintFire owns HUD.
static void osdDemoAdvanceHosAndStars(void)
{
    hosSteps = osdDemoHosSteps();
    hosWrappedThisStep = false;
    if (fx == OSD_DEMO_FX_TWISTER) {
        max7456SetHudMotionOffset(0, 0);
        return;
    }
    if (fx == OSD_DEMO_FX_PLASMA || fx == OSD_DEMO_FX_WIPE || fx == OSD_DEMO_FX_TUNNEL
        || fx == OSD_DEMO_FX_PLASMA2X2) {
        for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
            starFine[layer] = (uint16_t)(starFine[layer] + starSpeedPx[layer]);
        }
        max7456SetHudMotionOffset(0, 0);
        return;
    }
    if (fx == OSD_DEMO_FX_FIRE) {
        for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
            starFine[layer] = (uint16_t)(starFine[layer] + starSpeedPx[layer]);
        }
        return;
    }
    scrollFine++;
    if (scrollFine >= hosSteps) {
        scrollFine = 0;
        scrollCol++;
        hosWrappedThisStep = true; // new cells must land before HOS snap
        if (textPixelCols && scrollCol >= textPixelCols) {
            scrollCol = 0;
        }
    }
    for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
        starFine[layer] = (uint16_t)(starFine[layer] + starSpeedPx[layer]);
    }
    osdDemoApplyHudMotion();
}

// Logical shade 0..7 → glyph + INV. Only 4 NVM dithers; upper half via invert.
static void osdDemoWritePlasmaCell(uint8_t x, uint8_t y, uint8_t rows, uint8_t level)
{
    uint8_t glyph = OSD_DEMO_PIXEL_OFF;
    bool inv = false;
    if (level == 0) {
        // black
    } else if (level >= 7) {
        inv = true; // INV black = white
    } else if (level <= 4) {
        glyph = (uint8_t)(OSD_DEMO_PLASMA_BASE + (level - 1));
    } else if (level == 5) {
        glyph = (uint8_t)(OSD_DEMO_PLASMA_BASE + 2); // INV 25% → 75%
        inv = true;
    } else {
        glyph = (uint8_t)(OSD_DEMO_PLASMA_BASE + 0); // INV 11% → 89%
        inv = true;
    }
    osdDemoWriteCell(x, y, rows, glyph, inv);
}

// Fade in only — exit is the fire collapse, not an RB fade-out.
static uint8_t osdDemoPlasmaFadeWhite(void)
{
    const timeMs_t elapsed = millis() - fxStartMs;
    const timeMs_t fade = OSD_DEMO_PLASMA_FADE_MS;

    if (elapsed < fade) {
        return (uint8_t)((elapsed * 3) / (fade ? fade : 1));
    }
    return 3;
}

static void osdDemoSnapshotToFire(uint8_t cols, uint8_t rows)
{
    memset(fireHeat, 0, sizeof(fireHeat));
    if (cols > OSD_DEMO_FIRE_COLS) {
        cols = OSD_DEMO_FIRE_COLS;
    }
    if (rows > OSD_DEMO_FIRE_ROWS) {
        rows = OSD_DEMO_FIRE_ROWS;
    }
    const uint8_t band = OSD_DEMO_PLASMA_STAR_ROWS;
    const uint8_t plasmaY0 = band;
    const uint8_t plasmaY1 = (rows > (uint8_t)(band * 2)) ? (uint8_t)(rows - band) : rows;
    const uint8_t plasmaH = (plasmaY1 > plasmaY0) ? (uint8_t)(plasmaY1 - plasmaY0) : 1;
    const uint8_t xDiv = (cols > 1) ? (uint8_t)(cols - 1) : 1;
    const uint8_t yDiv = (plasmaH > 1) ? (uint8_t)(plasmaH - 1) : 1;

    for (uint8_t y = plasmaY0; y < plasmaY1; y++) {
        const uint8_t local = (uint8_t)(y - plasmaY0);
        const int8_t cy = (int8_t)(((uint16_t)local * 31u) / yDiv);
        for (uint8_t x = 0; x < cols; x++) {
            const int8_t cx = (int8_t)(((uint16_t)x * 59u) / xDiv);
            uint8_t level = osdDemoPlasmaLevel(cx, cy, plasmaT);
            if (level >= OSD_DEMO_PLASMA_LEVELS) {
                level = OSD_DEMO_PLASMA_LEVELS - 1;
            }
            fireHeat[y][x] = level;
        }
    }

    // Fold visible stars into the heat map so the whole screen falls together.
    for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
        const uint16_t rel = (uint16_t)(starFine[layer] - scrollFine);
        const uint16_t cellShift = rel / OSD_DEMO_CELL_W;
        for (uint8_t i = 0; i < OSD_DEMO_STARS_PER_LAYER; i++) {
            const uint8_t y = stars[layer][i].y;
            if (y >= rows) {
                continue;
            }
            const int sx = (int)stars[layer][i].x - (int)cellShift;
            const uint8_t x = (uint8_t)(((sx % cols) + cols) % cols);
            const uint8_t spark = (stars[layer][i].size >= 3) ? 4 : 3;
            if (fireHeat[y][x] < spark) {
                fireHeat[y][x] = spark;
            }
        }
    }
}

static void osdDemoFireCollapseStep(uint8_t cols, uint8_t rows)
{
    for (uint8_t x = 0; x < cols; x++) {
        for (uint8_t y = (uint8_t)(rows - 1); y > 0; y--) {
            uint16_t sum = (uint16_t)fireHeat[y][x] + (uint16_t)fireHeat[y - 1][x];
            if (sum > (OSD_DEMO_PLASMA_LEVELS - 1)) {
                sum = OSD_DEMO_PLASMA_LEVELS - 1;
            }
            fireHeat[y][x] = (uint8_t)sum;
            fireHeat[y - 1][x] = 0;
        }
    }
}

static void osdDemoFireStep(uint8_t cols, uint8_t rows)
{
    memset(fireNext, 0, sizeof(fireNext));

    const uint8_t cx = (uint8_t)(cols / 2);
    const uint8_t maxDist = cx ? cx : 1;

    for (uint8_t y = 0; y + 1 < rows; y++) {
        for (uint8_t x = 0; x < cols; x++) {
            const uint8_t xl = (x > 0) ? (uint8_t)(x - 1) : 0;
            const uint8_t xr = (x + 1 < cols) ? (uint8_t)(x + 1) : (uint8_t)(cols - 1);
            const uint8_t c = fireHeat[y + 1][x];
            const uint16_t sum = (uint16_t)fireHeat[y + 1][xl] + (uint16_t)c * 2u
                + (uint16_t)fireHeat[y + 1][xr];
            uint8_t below = (uint8_t)(sum / 4u);
            if (c > 0 && below < (uint8_t)(c - 1)) {
                below = (uint8_t)(c - 1);
            }
            if ((osdDemoRand() % 10) == 0 && below > 0) {
                below--;
            }
            // Cool faster with height; edges die toward dark sooner than center.
            const uint8_t fromFloor = (uint8_t)(rows - 1 - y);
            const uint8_t dist = (x > cx) ? (uint8_t)(x - cx) : (uint8_t)(cx - x);
            const uint8_t edge = (uint8_t)((dist * 30u) / maxDist); // 0..30
            uint8_t die = 8;
            if (fromFloor >= 11) {
                die = 35;
            } else if (fromFloor >= 8) {
                die = 22;
            } else if (fromFloor >= 5) {
                die = 12;
            }
            die = (uint8_t)(die + edge);
            if (die > 90) {
                die = 90;
            }
            // Extra edge snuff above mid-tongue height.
            if (dist > OSD_DEMO_FIRE_CORE_HALF && fromFloor >= 4 && below > 0
                && (osdDemoRand() % 3) == 0) {
                below--;
            }
            if (below > 0 && (osdDemoRand() % 100) < die) {
                below--;
            }
            fireNext[y][x] = below;
        }
    }

    for (uint8_t x = 0; x < cols; x++) {
        const uint8_t dist = (x > cx) ? (uint8_t)(x - cx) : (uint8_t)(cx - x);
        uint8_t target;
        if (dist <= OSD_DEMO_FIRE_CORE_HALF) {
            target = (uint8_t)(6 + (osdDemoRand() % 2)); // 6..7
            if ((osdDemoRand() % 3) != 0) {
                target = 7; // mostly white-hot floor in the core
            }
        } else {
            const uint8_t roll = (uint8_t)(osdDemoRand() % 10);
            if (roll < 5) {
                target = (uint8_t)(osdDemoRand() % 3); // 0..2 more edge gaps
            } else if (roll < 8) {
                target = (uint8_t)(2 + (osdDemoRand() % 3)); // 2..4
            } else {
                target = (uint8_t)(4 + (osdDemoRand() % 2)); // 4..5
            }
        }
        fireFuelAvg[x] = (uint8_t)((fireFuelAvg[x] * 2u + target * 2u) / 4u);
        if (dist <= OSD_DEMO_FIRE_CORE_HALF && fireFuelAvg[x] + 1 < target) {
            fireFuelAvg[x] = (uint8_t)((fireFuelAvg[x] + target) / 2u);
        }
        fireNext[rows - 1][x] = fireFuelAvg[x];
        // Extra white flicker on the floor — mostly in the core.
        if (dist <= OSD_DEMO_FIRE_CORE_HALF) {
            if ((osdDemoRand() % 3) == 0) {
                fireNext[rows - 1][x] = 7;
            } else if ((osdDemoRand() & 1) != 0) {
                fireNext[rows - 1][x] = 7;
            }
        } else if ((osdDemoRand() % 6) == 0) {
            fireNext[rows - 1][x] = (uint8_t)(4 + (osdDemoRand() % 3));
        }

        if (dist <= OSD_DEMO_FIRE_CORE_HALF && rows >= 2 && (osdDemoRand() % 100) < 80) {
            const uint8_t v = (uint8_t)(fireFuelAvg[x] > 0 ? (fireFuelAvg[x] - 1) : 0);
            if (fireNext[rows - 2][x] < v) {
                fireNext[rows - 2][x] = v;
            }
        }
        if (dist <= OSD_DEMO_FIRE_CORE_HALF && rows >= 3 && (osdDemoRand() % 100) < 40) {
            const uint8_t burst = (uint8_t)(3 + (osdDemoRand() % 3)); // 3..5
            if (fireNext[rows - 3][x] < burst) {
                fireNext[rows - 3][x] = burst;
            }
        }

        const uint8_t mid = (uint8_t)(rows / 2);
        const uint8_t xl = (x > 0) ? (uint8_t)(x - 1) : 0;
        const uint8_t xr = (x + 1 < cols) ? (uint8_t)(x + 1) : (uint8_t)(cols - 1);
        if (fireHeat[mid][xl] > fireHeat[mid][xr]) {
            fireTipFine[x] = (uint8_t)((fireTipFine[x] + 1) % OSD_DEMO_STAR_FRAMES);
        } else if (fireHeat[mid][xr] > fireHeat[mid][xl]) {
            fireTipFine[x] = (uint8_t)((fireTipFine[x] + OSD_DEMO_STAR_FRAMES - 1) % OSD_DEMO_STAR_FRAMES);
        } else if ((osdDemoRand() & 7) == 0) {
            fireTipFine[x] = (uint8_t)((fireTipFine[x] + 1) % OSD_DEMO_STAR_FRAMES);
        }
    }

    for (uint8_t y = 0; y < rows; y++) {
        for (uint8_t x = 0; x < cols; x++) {
            const uint8_t n = fireNext[y][x];
            const uint8_t o = fireHeat[y][x];
            if (n >= o) {
                fireHeat[y][x] = n;
            } else {
                fireHeat[y][x] = (uint8_t)((o * 2u + n) / 3u);
            }
        }
    }

    for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
        if (fireSparks[i].life == 0) {
            continue;
        }
        fireSparks[i].acc++;
        if (fireSparks[i].acc < fireSparks[i].spd) {
            continue;
        }
        fireSparks[i].acc = 0;
        // Hide only once inside the ribbon (rows 0..CURVE_ROWS-1).
        if (fireSparks[i].y < OSD_DEMO_FIRE_CURVE_ROWS || fireSparks[i].y == 0) {
            fireSparks[i].life = 0;
            continue;
        }
        fireSparks[i].y--;
        if (fireSparks[i].y < OSD_DEMO_FIRE_CURVE_ROWS) {
            fireSparks[i].life = 0;
            continue;
        }
        if ((osdDemoRand() & 3) == 0) {
            fireSparks[i].fine = (uint8_t)((fireSparks[i].fine + 1) % OSD_DEMO_STAR_FRAMES);
        }
    }
    // Sparks peel off the tongue tip and fly up until the ribbon.
    if ((osdDemoRand() % 12) == 0 && cols > 0 && rows > 4) {
        const uint8_t x = (uint8_t)(osdDemoRand() % cols);
        uint8_t tipY = 0xFF;
        for (uint8_t y = 0; y < rows; y++) {
            if (fireHeat[y][x] != 0) {
                tipY = y;
                break;
            }
        }
        // Need a real climb path below the ribbon (at least a few rows).
        if (tipY != 0xFF && tipY > (uint8_t)(OSD_DEMO_FIRE_CURVE_ROWS + 2)) {
            const uint8_t yPick = tipY; // spawn on the tip, then rise
            bool rowTaken = false;
            for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
                if (fireSparks[i].life && fireSparks[i].x == x
                    && fireSparks[i].y <= yPick
                    && fireSparks[i].y + 2 >= yPick) {
                    rowTaken = true;
                    break;
                }
            }
            if (!rowTaken) {
                for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
                    if (fireSparks[i].life != 0) {
                        continue;
                    }
                    fireSparks[i].x = x;
                    fireSparks[i].y = yPick;
                    fireSparks[i].fine = fireTipFine[x];
                    // Survive the whole climb to the ribbon regardless of spd.
                    fireSparks[i].life = 255;
                    fireSparks[i].size = 2;
                    fireSparks[i].spd = (uint8_t)(2 + (osdDemoRand() % 3)); // 2..4 — readable rise
                    fireSparks[i].acc = 0;
                    break;
                }
            }
        }
    }
}

// (VOS soft-rise + cell shift-up removed — fire stays screen-locked.)

static void osdDemoFireFindTips(uint8_t cols, uint8_t rows)
{
    memset(fireTipMask, 0xFF, sizeof(fireTipMask));
    fireFlameTopRaw = (uint8_t)(rows - 1);
    if (firePhase == OSD_DEMO_FIRE_COLLAPSE) {
        fireFlameTop = fireFlameTopRaw;
        return;
    }
    uint8_t tipY[OSD_DEMO_FIRE_COLS];
    memset(tipY, 0xFF, sizeof(tipY));
    bool any = false;
    for (uint8_t x = 0; x < cols; x++) {
        for (uint8_t y = 0; y < rows; y++) {
            const uint8_t h = fireHeat[y][x];
            if (h == 0) {
                continue;
            }
            if (!any || y < fireFlameTopRaw) {
                fireFlameTopRaw = y;
                any = true;
            }
            const uint8_t flameH = (uint8_t)(rows - y);
            if (h <= 2 && flameH >= 4) {
                tipY[x] = y;
            }
            break;
        }
    }
    if (!any) {
        fireFlameTopRaw = (uint8_t)(rows - 1);
    }

    // Only local height maxima — breaks the flat mid-screen tip line.
    uint8_t placed = 0;
    for (uint8_t x = 0; x < cols && placed < 3; x++) {
        if (tipY[x] == 0xFF) {
            continue;
        }
        const uint8_t xl = (x > 0) ? (uint8_t)(x - 1) : x;
        const uint8_t xr = (x + 1 < cols) ? (uint8_t)(x + 1) : x;
        const uint8_t yl = tipY[xl];
        const uint8_t yr = tipY[xr];
        const bool tallerL = (yl == 0xFF) || (tipY[x] < yl);
        const bool tallerR = (yr == 0xFF) || (tipY[x] < yr);
        if (tallerL && tallerR && (osdDemoRand() & 1) != 0) {
            fireTipMask[x] = tipY[x];
            placed++;
        }
    }

    // Smooth RB boundary: follow rising tips immediately, lag when they fall.
    if (fireFlameTopRaw < fireFlameTop) {
        fireFlameTop = fireFlameTopRaw;
        fireTopHold = 0;
    } else if (fireFlameTopRaw > fireFlameTop) {
        fireTopHold++;
        if (fireTopHold >= 3) {
            fireTopHold = 0;
            if (fireFlameTop < (uint8_t)(rows - 1)) {
                fireFlameTop++;
            }
        }
    } else {
        fireTopHold = 0;
    }
}

// White-hot cores use the same dither ladder as plasma (no soft-fill glyphs).
static void osdDemoWriteFireCell(uint8_t x, uint8_t y, uint8_t rows, uint8_t level)
{
    if (level == 0) {
        return;
    }
    osdDemoWritePlasmaCell(x, y, rows, level);
}

static void osdDemoApplyFireRowBright(uint8_t rows)
{
    const uint8_t limit = (rows < OSD_DEMO_RB_ROWS) ? rows : OSD_DEMO_RB_ROWS;
    uint8_t sparkRow[OSD_DEMO_RB_ROWS];
    memset(sparkRow, 0, sizeof(sparkRow));
    for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
        if (fireSparks[i].life
            && fireSparks[i].y >= OSD_DEMO_FIRE_CURVE_ROWS
            && fireSparks[i].y < limit) {
            sparkRow[fireSparks[i].y] = 1;
        }
    }

    // Highest tongue tip → start RB; +1 white per row down to max (3).
    // Start at max-3 so the darkest band is only one row, not a tall void.
    const uint8_t top = (fireFlameTop < rows) ? fireFlameTop : (uint8_t)(rows - 1);
    const uint8_t rbMax = 3;
    const uint8_t rbStart = (uint8_t)(rbMax - 3); // tip row brightness

    for (uint8_t y = 0; y < limit; y++) {
        uint8_t white = rbMax;
        if (y < OSD_DEMO_FIRE_CURVE_ROWS && firePhase != OSD_DEMO_FIRE_COLLAPSE) {
            white = fireCurveBright;
        } else if (firePhase == OSD_DEMO_FIRE_COLLAPSE) {
            if (y < fireFall) {
                white = 0;
            } else if (y == (uint8_t)(rows - 1)) {
                white = rbMax;
            } else {
                // 1 step per collapsed row, same start as burn.
                const uint8_t pos = (uint8_t)(y - fireFall);
                white = (uint8_t)(rbStart + pos);
                if (white > rbMax) {
                    white = rbMax;
                }
            }
        } else if (y < top) {
            // Above the tallest tongue: dark, sparks still punch through brighter.
            white = sparkRow[y] ? rbMax : 0;
        } else {
            // 1 RB step per row: tip = max-3, then +1 each row → clamp at max.
            const uint8_t pos = (uint8_t)(y - top);
            white = (uint8_t)(rbStart + pos);
            if (white > rbMax) {
                white = rbMax;
            }
            if (sparkRow[y] && white < rbMax) {
                white++;
            }
        }
        const uint8_t reg = (uint8_t)((0 << 2) | (rbMax - white));
        if (prevRowBright[y] != reg) {
            prevRowBright[y] = reg;
            max7456BrightnessRow(y, 0, white);
        }
    }
}

// Cell codes for dirty redraw: 0=blank, 1..7=heat, 0x40|f tip2, 0x50|f tip3, 0x60|f spark.
static uint8_t osdDemoFireCellCode(uint8_t x, uint8_t y, uint8_t rows)
{
    // Never show sparks on/above the top ribbon.
    if (y >= OSD_DEMO_FIRE_CURVE_ROWS) {
        for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
            if (fireSparks[i].life && fireSparks[i].x == x && fireSparks[i].y == y) {
                return (uint8_t)(0x60 | (fireSparks[i].fine % OSD_DEMO_STAR_FRAMES));
            }
        }
    }
    if (fireTipMask[x] == y) {
        const uint8_t h = fireHeat[y][x];
        const uint8_t size = (h >= 3 || (y + 1 < rows && fireHeat[y + 1][x] >= 5)) ? 3 : 2;
        const uint8_t tag = (size >= 3) ? 0x50 : 0x40;
        return (uint8_t)(tag | (fireTipFine[x] % OSD_DEMO_STAR_FRAMES));
    }
    uint8_t level = fireHeat[y][x];
    if (level >= OSD_DEMO_PLASMA_LEVELS) {
        level = OSD_DEMO_PLASMA_LEVELS - 1;
    }
    return level;
}

static void osdDemoFireWriteCoded(uint8_t x, uint8_t y, uint8_t rows, uint8_t code)
{
    if (code == 0) {
        max7456WriteChar(x, y, OSD_DEMO_PIXEL_OFF);
        return;
    }
    if ((code & 0xF0) == 0x60) {
        max7456WriteChar(x, y, osdDemoStarGlyph(2, (uint8_t)(code & 0x0F)));
        return;
    }
    if ((code & 0xF0) == 0x50) {
        max7456WriteChar(x, y, osdDemoStarGlyph(3, (uint8_t)(code & 0x0F)));
        return;
    }
    if ((code & 0xF0) == 0x40) {
        max7456WriteChar(x, y, osdDemoStarGlyph(2, (uint8_t)(code & 0x0F)));
        return;
    }
    osdDemoWriteFireCell(x, y, rows, code);
}

static void osdDemoFireDrawDirty(uint8_t cols, uint8_t rows, bool force)
{
    // Rows 0..1 reserved for the top soft-fill curve.
    const uint8_t y0 = (rows > OSD_DEMO_FIRE_CURVE_ROWS) ? OSD_DEMO_FIRE_CURVE_ROWS : rows;
    for (uint8_t y = y0; y < rows; y++) {
        for (uint8_t x = 0; x < cols; x++) {
            const uint8_t code = osdDemoFireCellCode(x, y, rows);
            if (!force && fireDrawn[y][x] == code) {
                continue;
            }
            fireDrawn[y][x] = code;
            osdDemoFireWriteCoded(x, y, rows, code);
        }
    }
}

static uint8_t osdDemoFireTongueH(uint8_t x, uint8_t rows)
{
    for (uint8_t y = 0; y < rows; y++) {
        if (fireHeat[y][x] != 0) {
            return (uint8_t)(rows - y);
        }
    }
    return 0;
}

static void osdDemoFireUpdateCurve(uint8_t cols, uint8_t rows)
{
    fireCurvePhase = (uint8_t)(fireCurvePhase + 3);
    // Ribbon RB stays statically dark.
    fireCurveBright = 0;

    for (uint8_t x = 0; x < cols; x++) {
        const int8_t s = osdDemoSineAt((uint8_t)(fireCurvePhase + x * 11u));
        int16_t h = (int16_t)(5 + ((int16_t)s * 4) / 128); // base bend ~1..9px
        h += (int16_t)(osdDemoFireTongueH(x, rows) / 2); // flame lifts row-1 profile
        for (uint8_t i = 0; i < OSD_DEMO_FIRE_SPARKS; i++) {
            if (fireSparks[i].life == 0) {
                continue;
            }
            const uint8_t sx = fireSparks[i].x;
            const uint8_t sy = fireSparks[i].y;
            const uint8_t dx = (sx > x) ? (uint8_t)(sx - x) : (uint8_t)(x - sx);
            if (dx > 2 || sy >= 5) {
                continue;
            }
            h += (int16_t)(6 + (4 - (int)sy) * 2 - (int)dx * 2);
        }
        if (h < 0) {
            h = 0;
        }
        if (h > OSD_DEMO_FIRE_CURVE_MAX_PX) {
            h = OSD_DEMO_FIRE_CURVE_MAX_PX;
        }
        fireCurveH[x] = (uint8_t)((fireCurveH[x] * 3u + (uint8_t)h) / 4u);
    }
}

// Bottom-fill of k pixels: FILL(CELL_H - k) → white on [CELL_H-k .. CELL_H).
static void osdDemoFireWriteCurveBottom(uint8_t x, uint8_t y, uint8_t rows, uint8_t pxFromBottom)
{
    if (pxFromBottom == 0) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_PIXEL_OFF, false);
    } else if (pxFromBottom >= OSD_DEMO_CELL_H) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_FILL_BASE, false);
    } else {
        osdDemoWriteCell(x, y, rows,
            (uint8_t)(OSD_DEMO_FILL_BASE + (OSD_DEMO_CELL_H - pxFromBottom)), false);
    }
}

// Top-fill of k pixels: INV(FILL+k) → white on [0..k).
static void osdDemoFireWriteCurveTop(uint8_t x, uint8_t y, uint8_t rows, uint8_t pxFromTop)
{
    if (pxFromTop == 0) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_PIXEL_OFF, false);
    } else if (pxFromTop >= OSD_DEMO_CELL_H) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_FILL_BASE, false);
    } else {
        osdDemoWriteCell(x, y, rows, (uint8_t)(OSD_DEMO_FILL_BASE + pxFromTop), true);
    }
}

static void osdDemoFireDrawCurve(uint8_t cols, uint8_t rows)
{
    if (rows < OSD_DEMO_FIRE_CURVE_ROWS || cols == 0) {
        return;
    }
    // Row0: bottom-fill curveH[x]. Row1: top-fill curveH[mirror] (H-flip profile,
    // top-oriented so the lower band is not upside-down relative to the ribbon).
    for (uint8_t x = 0; x < cols; x++) {
        const uint8_t xm = (uint8_t)(cols - 1 - x);
        osdDemoFireWriteCurveBottom(x, 0, rows, fireCurveH[x]);
        osdDemoFireWriteCurveTop(x, 1, rows, fireCurveH[xm]);
        fireDrawn[0][x] = 0xFF;
        fireDrawn[1][x] = 0xFF;
    }
}

static void osdDemoPaintFire(void)
{
    if (!demoDisplay) {
        return;
    }
    uint8_t cols = demoDisplay->cols;
    uint8_t rows = demoDisplay->rows;
    if (cols > OSD_DEMO_FIRE_COLS) {
        cols = OSD_DEMO_FIRE_COLS;
    }
    if (rows > OSD_DEMO_FIRE_ROWS) {
        rows = OSD_DEMO_FIRE_ROWS;
    }

    bool redraw = false;

    if (firePhase == OSD_DEMO_FIRE_COLLAPSE) {
        max7456SetHudMotionOffset(0, 0);
        fireTick++;
        if (fireTick >= OSD_DEMO_FIRE_FALL_TICKS) {
            fireTick = 0;
            if (fireFall < rows) {
                osdDemoFireCollapseStep(cols, rows);
                fireFall++;
                redraw = true;
            }
            if (fireFall >= rows) {
                firePhase = OSD_DEMO_FIRE_IGNITE;
                fireIgniteMs = millis();
                redraw = true;
            }
        }
    } else {
        max7456SetHudMotionOffset(0, 0);
        fireTick++;
        if (fireTick >= OSD_DEMO_FIRE_STEP_TICKS) {
            fireTick = 0;
            osdDemoFireStep(cols, rows);
            redraw = true;
        }
        if (firePhase == OSD_DEMO_FIRE_IGNITE) {
            if ((millis() - fireIgniteMs) >= OSD_DEMO_FIRE_IGNITE_MS) {
                firePhase = OSD_DEMO_FIRE_BURN;
                memset(prevRowBright, 0xFF, sizeof(prevRowBright));
            }
        }
    }

    if (redraw) {
        osdDemoFireFindTips(cols, rows);
        if (firePhase != OSD_DEMO_FIRE_COLLAPSE) {
            osdDemoFireUpdateCurve(cols, rows);
        }
        osdDemoApplyFireRowBright(rows);
        osdDemoFireDrawDirty(cols, rows, false);
        if (firePhase != OSD_DEMO_FIRE_COLLAPSE) {
            osdDemoFireDrawCurve(cols, rows);
        }
    }
}

// Top-white HFILL; INV → top-black + transparent (gray-black into video).
static void osdDemoWriteHFillTop(uint8_t x, uint8_t y, uint8_t rows, uint8_t fill, bool inv)
{
    if (fill > OSD_DEMO_CELL_H) {
        fill = OSD_DEMO_CELL_H;
    }
    osdDemoWriteCell(x, y, rows, (uint8_t)(OSD_DEMO_HFILL_BASE + fill), inv);
}

// Bottom-white HFILL; INV → bottom-black + transparent.
static void osdDemoWriteHFillBot(uint8_t x, uint8_t y, uint8_t rows, uint8_t fill, bool inv)
{
    if (fill > OSD_DEMO_CELL_H) {
        fill = OSD_DEMO_CELL_H;
    }
    osdDemoWriteCell(x, y, rows, (uint8_t)(OSD_DEMO_HFILL_BOT_BASE + fill), inv);
}

// Soft silhouette into video: color amount `into`, growing from bottom or top.
static void osdDemoWriteRibbonSoftOuter(uint8_t x, uint8_t y, uint8_t rows,
    int16_t into, bool fromBottom, bool isBlack)
{
    if (into < 1) {
        into = 1;
    }
    if (into > OSD_DEMO_CELL_H) {
        into = OSD_DEMO_CELL_H;
    }
    const uint8_t fill = (uint8_t)into;
    if (fromBottom) {
        osdDemoWriteHFillBot(x, y, rows, fill, isBlack);
    } else {
        osdDemoWriteHFillTop(x, y, rows, fill, isBlack);
    }
}

// Soft B↔W seam — opaque FILL for the cell that straddles `edge`.
static void osdDemoWriteRibbonSoftBwCell(uint8_t x, uint8_t y, uint8_t rows,
    int16_t c0, int16_t edge, bool whiteBelow)
{
    int16_t whitePx;
    if (whiteBelow) {
        whitePx = (int16_t)(c0 + OSD_DEMO_CELL_H - edge);
    } else {
        whitePx = (int16_t)(edge - c0);
    }
    if (whitePx <= 0) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_PIXEL_OFF, false);
        return;
    }
    if (whitePx >= OSD_DEMO_CELL_H) {
        osdDemoWriteCell(x, y, rows, OSD_DEMO_FILL_BASE, false);
        return;
    }
    if (whiteBelow) {
        osdDemoFireWriteCurveBottom(x, y, rows, (uint8_t)whitePx);
    } else {
        osdDemoFireWriteCurveTop(x, y, rows, (uint8_t)whitePx);
    }
}

// Classic horizontal twister: ONE continuous bar, 4 edges @ 90°.
// Spatial frequency = half-turn across width → visible twist (2 face bands).
// Visible faces alternate black/white; SoftBw on shared edges; SoftOuter on silhouette.
static void osdDemoPaintWipe(void)
{
    if (!demoDisplay) {
        return;
    }
    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows < 4) {
        return;
    }

    wipePhase = (uint8_t)(wipePhase + 1);
    max7456SetHudMotionOffset(0, 0);
    // HFILL phase 0 = all-transparent (see-through to video). Do NOT use 0x20:
    // tunnel NVM lives in 0x00..0x3F and used to stomp SYM_BLANK with squares.
    max7456FillScreen(OSD_DEMO_HFILL_BASE);

    const int16_t screenH = (int16_t)((uint16_t)rows * OSD_DEMO_CELL_H);
    const int16_t midY = screenH / 2;
    const int16_t den = (cols > 1) ? (int16_t)(cols - 1) : 1;

    const int8_t slS = osdDemoSineAt(wipePhase);
    // Gentle tilt — strong slope made the bar look like it flies off-screen.
    const int16_t slope = (int16_t)((screenH / 28) + (((int16_t)(screenH / 48) * (int16_t)slS) / 128));

    const int8_t cvS = osdDemoSineAt((uint8_t)(wipePhase + 64));
    int16_t amp = (int16_t)((screenH / 3) + (((int16_t)(screenH / 16) * (int16_t)cvS) / 128));
    if (amp < (int16_t)(screenH / 5)) {
        amp = (int16_t)(screenH / 5);
    }
    const int16_t ampSpan = (amp > 0) ? (int16_t)(amp * 2) : 1;
    const uint8_t yBot = (uint8_t)(rows - 1);

    for (uint8_t x = 0; x < cols; x++) {
        const int16_t t = (int16_t)(((int32_t)x * 2 - den) * slope / den);
        const int16_t axis = midY + t;
        // Half-turn across width → one twist / two face bands.
        const uint8_t ang = (uint8_t)((uint16_t)((uint32_t)x * 128u / (uint16_t)den) + wipePhase);

        // Edge ribbons (top/bottom rows): same bend as twister axis+twist,
        // white-on-transparent HFILL; row brightness follows wipePhase.
        {
            const int8_t s0 = osdDemoSineAt(ang);
            const int16_t edge = (int16_t)(axis + ((amp * (int16_t)s0) / 128));
            int16_t fill = (int16_t)(((int32_t)(edge - (midY - amp)) * OSD_DEMO_CELL_H) / ampSpan);
            if (fill < 1) {
                fill = 1;
            }
            if (fill > OSD_DEMO_CELL_H) {
                fill = OSD_DEMO_CELL_H;
            }
            osdDemoWriteHFillTop(x, 0, rows, (uint8_t)fill, false);      // white+trans
            osdDemoWriteHFillBot(x, yBot, rows, (uint8_t)fill, false);   // white+trans
        }

        int16_t e[4];
        for (uint8_t k = 0; k < 4; k++) {
            const int8_t s = osdDemoSineAt((uint8_t)(ang + (uint8_t)(k * 64)));
            e[k] = (int16_t)(axis + ((amp * (int16_t)s) / 128));
        }

        int16_t fy0[4];
        int16_t fy1[4];
        bool fBlk[4];
        uint8_t nFace = 0;
        int16_t silLo = 32767;
        int16_t silHi = -32768;
        for (uint8_t i = 0; i < 4; i++) {
            const int16_t y0 = e[i];
            const int16_t y1 = e[(uint8_t)((i + 1) & 3)];
            if (y1 > y0) {
                fy0[nFace] = y0;
                fy1[nFace] = y1;
                fBlk[nFace] = ((i & 1) == 0);
                if (y0 < silLo) {
                    silLo = y0;
                }
                if (y1 > silHi) {
                    silHi = y1;
                }
                nFace++;
            }
        }
        if (nFace == 0 || silHi <= silLo) {
            continue;
        }

        for (uint8_t y = 0; y < rows; y++) {
            const int16_t c0 = (int16_t)((uint16_t)y * OSD_DEMO_CELL_H);
            const int16_t c1 = (int16_t)(c0 + OSD_DEMO_CELL_H);
            const int16_t cy = (int16_t)(c0 + (OSD_DEMO_CELL_H / 2));

            if (c1 <= silLo || c0 >= silHi) {
                continue;
            }

            if (c0 < silLo && silLo < c1) {
                bool blk = true;
                for (uint8_t i = 0; i < nFace; i++) {
                    if (fy0[i] == silLo) {
                        blk = fBlk[i];
                        break;
                    }
                }
                osdDemoWriteRibbonSoftOuter(x, y, rows, (int16_t)(c1 - silLo), true, blk);
                continue;
            }
            if (c0 < silHi && silHi < c1) {
                bool blk = true;
                for (uint8_t i = 0; i < nFace; i++) {
                    if (fy1[i] == silHi) {
                        blk = fBlk[i];
                        break;
                    }
                }
                osdDemoWriteRibbonSoftOuter(x, y, rows, (int16_t)(silHi - c0), false, blk);
                continue;
            }

            int8_t hit = -1;
            for (uint8_t i = 0; i < nFace; i++) {
                if (cy >= fy0[i] && cy <= fy1[i]) {
                    hit = (int8_t)i;
                    break;
                }
            }
            if (hit < 0) {
                continue;
            }

            const int16_t y0 = fy0[hit];
            const int16_t y1 = fy1[hit];
            const bool blk = fBlk[hit];

            bool loBw = false;
            bool hiBw = false;
            for (uint8_t j = 0; j < nFace; j++) {
                if (fBlk[j] == blk) {
                    continue;
                }
                if (fy0[j] == y0 || fy1[j] == y0) {
                    loBw = true;
                }
                if (fy0[j] == y1 || fy1[j] == y1) {
                    hiBw = true;
                }
            }
            if (loBw && c0 <= y0 && y0 < c1) {
                osdDemoWriteRibbonSoftBwCell(x, y, rows, c0, y0, !blk);
                continue;
            }
            if (hiBw && c0 <= y1 && y1 < c1) {
                osdDemoWriteRibbonSoftBwCell(x, y, rows, c0, y1, blk);
                continue;
            }

            if (blk) {
                osdDemoWriteCell(x, y, rows, OSD_DEMO_PIXEL_OFF, false);
            } else {
                osdDemoWriteCell(x, y, rows, OSD_DEMO_FILL_BASE, false);
            }
        }
    }

    // Edge ribbons: fixed minimum white. Twister: full.
    {
        for (uint8_t row = 0; row < rows; row++) {
            const uint8_t white = (row == 0 || row == (uint8_t)(rows - 1)) ? 0 : 3;
            max7456BrightnessRow(row, 0, white);
        }
    }
}

static uint16_t osdDemoIsqrt(uint32_t v)
{
    uint32_t op = v;
    uint32_t res = 0;
    uint32_t one = 1u << 30;
    while (one > op) {
        one >>= 2;
    }
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (uint16_t)res;
}

// True-angle LUT: idx = min*64/max → 0..32 for first octant (even wedges).
static const uint8_t osdDemoAtanLut[65] = {
    0,  1,  1,  2,  3,  3,  4,  4,  5,  6,  6,  7,  8,  8,  9,  9,
    10, 11, 11, 12, 12, 13, 13, 14, 15, 15, 16, 16, 17, 17, 18, 18,
    19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 25, 26,
    26, 27, 27, 27, 28, 28, 29, 29, 29, 30, 30, 30, 31, 31, 31, 32, 32
};

static uint8_t osdDemoAtan2u8(int16_t y, int16_t x)
{
    if (x == 0 && y == 0) {
        return 0;
    }
    const uint16_t ax = (x < 0) ? (uint16_t)(-x) : (uint16_t)x;
    const uint16_t ay = (y < 0) ? (uint16_t)(-y) : (uint16_t)y;
    uint8_t n;
    if (ax >= ay) {
        uint8_t idx = (ax == 0) ? 0u : (uint8_t)(((uint32_t)ay * 64u) / ax);
        if (idx > 64u) {
            idx = 64u;
        }
        n = osdDemoAtanLut[idx];
    } else {
        uint8_t idx = (ay == 0) ? 0u : (uint8_t)(((uint32_t)ax * 64u) / ay);
        if (idx > 64u) {
            idx = 64u;
        }
        n = (uint8_t)(64u - osdDemoAtanLut[idx]);
    }
    if (x >= 0) {
        return (y >= 0) ? n : (uint8_t)(256u - n);
    }
    return (y >= 0) ? (uint8_t)(128u - n) : (uint8_t)(128u + n);
}

static uint8_t osdDemoTunnelGlyph(uint8_t pat)
{
    uint8_t addr = (uint8_t)(OSD_DEMO_TUNNEL_BASE + (pat & 0x3F));
    if (addr == OSD_DEMO_TUNNEL_BLANK_IDX) {
        addr = OSD_DEMO_TUNNEL_ALT;
    }
    return addr;
}

static void osdDemoWriteTunnelPat(uint8_t x, uint8_t y, uint8_t rows, uint8_t pat)
{
    osdDemoWriteCell(x, y, rows, osdDemoTunnelGlyph(pat), false);
}

// Cosine ease 0→128 over dur (soft start/stop for look-back turns).
static uint8_t osdDemoTunnelEase128(timeMs_t t, timeMs_t dur)
{
    if (dur == 0) {
        return 128;
    }
    if (t >= dur) {
        return 128;
    }
    const uint8_t a = (uint8_t)((t * 128u) / dur); // 0..127 → half-turn of cos
    const int8_t c = osdDemoSineAt((uint8_t)(a + 64)); // cos: +127 → -127
    return (uint8_t)((((int16_t)127 - (int16_t)c) * 128) / 254);
}

// Light L/R yaw while holding a straight heading; fades at hold edges so turns connect cleanly.
static int8_t osdDemoTunnelSway(timeMs_t elapsed, timeMs_t holdPos, timeMs_t holdLen)
{
    uint16_t amp = OSD_DEMO_TUNNEL_SWAY_AMP;
    const timeMs_t fade = OSD_DEMO_TUNNEL_SWAY_FADE;
    if (holdLen <= (timeMs_t)(2u * fade)) {
        amp = (uint16_t)((amp * holdLen) / (2u * fade + 1u));
    } else if (holdPos < fade) {
        amp = (uint16_t)((amp * holdPos) / fade);
    } else if (holdPos > (timeMs_t)(holdLen - fade)) {
        amp = (uint16_t)((amp * (holdLen - holdPos)) / fade);
    }
    const uint8_t ph = (uint8_t)((elapsed * 256u) / OSD_DEMO_TUNNEL_SWAY_MS);
    return (int8_t)(((int16_t)osdDemoSineAt(ph) * (int16_t)amp) >> 7);
}

// Craft tunnel.S scene angle: 0=forward, 128=back, 256=forward (full Y spin).
// Straight holds get a soft L/R sway; 180° turns use cosine ease (no hard kick).
static uint8_t osdDemoTunnelSceneAngle(timeMs_t elapsed)
{
    if (elapsed < OSD_DEMO_TUNNEL_STATIC_MS) {
        return (uint8_t)osdDemoTunnelSway(elapsed, elapsed, OSD_DEMO_TUNNEL_STATIC_MS);
    }
    const timeMs_t after = (timeMs_t)(elapsed - OSD_DEMO_TUNNEL_STATIC_MS);
    const timeMs_t turn = OSD_DEMO_TUNNEL_TURN_MS;
    const timeMs_t fwd = OSD_DEMO_TUNNEL_FWD_MS;
    const timeMs_t back = OSD_DEMO_TUNNEL_BACK_MS;
    const timeMs_t cycle = (timeMs_t)(fwd + turn + back + turn);
    const timeMs_t phase = (timeMs_t)(after % cycle);

    if (phase < fwd) {
        return (uint8_t)osdDemoTunnelSway(elapsed, phase, fwd);
    }
    if (phase < (fwd + turn)) {
        return osdDemoTunnelEase128((timeMs_t)(phase - fwd), turn); // 0→128 eased
    }
    if (phase < (fwd + turn + back)) {
        const timeMs_t hp = (timeMs_t)(phase - fwd - turn);
        return (uint8_t)(128 + osdDemoTunnelSway(elapsed, hp, back));
    }
    return (uint8_t)(128u + osdDemoTunnelEase128((timeMs_t)(phase - fwd - turn - back), turn));
}

// Map a mega-pixel inside the 3-row text band → font row 0..4, -1=black rule, -2=tunnel.
// Stack font rows contiguously (no black gaps through the glyph):
//   top:    black | black | font[0]
//   middle: font[1] | font[2] | font[3]
//   bottom: font[4] | black | black
static int8_t osdDemoTunnelTextFontRow(uint8_t cellY, uint8_t br, uint8_t textY0)
{
    if (cellY < textY0 || cellY >= (uint8_t)(textY0 + OSD_DEMO_TUNNEL_TEXT_ROWS)) {
        return -2;
    }
    const uint8_t band = (uint8_t)(cellY - textY0);
    if (band == 0) {
        return (br == 2) ? 0 : -1;
    }
    if (band == 1) {
        return (int8_t)(1 + br);
    }
    return (br == 0) ? 4 : -1;
}

// Craft tunnel.S: rotate ray (x,z) around Y, then polar-map to checker.
// Angle drives look-around; at 90° the tunnel streams past sideways (true 3D yaw).
// Center 3 rows overlay the 5×5 font at 6×6 mega-pixel resolution.
static void osdDemoPaintTunnel(void)
{
    if (!demoDisplay) {
        return;
    }
    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows == 0) {
        return;
    }
    tunnelMw = (uint8_t)(cols * 2);
    tunnelMh = (uint8_t)(rows * 3);

    const timeMs_t elapsed = (timeMs_t)(millis() - fxStartMs);
    // Fly (V), spin (U), and text scroll at different rates — lockstep freezes wedges.
    tunnelSpinDiv++;
    if (tunnelSpinDiv >= OSD_DEMO_TUNNEL_SPIN_DIV) {
        tunnelSpinDiv = 0;
        tunnelRot++;
    }
    tunnelAnimDiv++;
    if (tunnelAnimDiv >= OSD_DEMO_TUNNEL_ANIM_DIV) {
        tunnelAnimDiv = 0;
        tunnelAnim++;
    }
    tunnelTextDiv++;
    if (tunnelTextDiv >= OSD_DEMO_TUNNEL_TEXT_DIV) {
        tunnelTextDiv = 0;
        tunnelScrollMx++;
        if (tunnelTextCols && tunnelScrollMx >= tunnelTextCols) {
            tunnelScrollMx = 0;
        }
    }
    const uint8_t anim = tunnelAnim;
    const uint8_t spin = tunnelRot;
    // Craft: a = (framecount>>3)+offset → sin/cos. We drive a for look-back cycle.
    const uint8_t a = osdDemoTunnelSceneAngle(elapsed);
    const int8_t sn = osdDemoSineAt(a);
    const int8_t cs = osdDemoSineAt((uint8_t)(a + 64));

    max7456SetHudMotionOffset(0, 0);

    const int16_t midX = (int16_t)(tunnelMw / 2);
    const int16_t midY = (int16_t)(tunnelMh / 2);
    const int16_t xDen = (midX > 0) ? midX : 1;
    const uint8_t textY0 = (rows >= OSD_DEMO_TUNNEL_TEXT_ROWS)
        ? (uint8_t)((rows - OSD_DEMO_TUNNEL_TEXT_ROWS) / 2)
        : 0;

    for (uint8_t cellY = 0; cellY < rows; cellY++) {
        for (uint8_t cellX = 0; cellX < cols; cellX++) {
            uint8_t pat = 0;
            for (uint8_t br = 0; br < 3; br++) {
                for (uint8_t bc = 0; bc < 2; bc++) {
                    const int8_t trow = osdDemoTunnelTextFontRow(cellY, br, textY0);
                    bool on = false;
                    if (trow == -1) {
                        // black rule — leave bit clear
                    } else if (trow >= 0) {
                        const uint16_t pcol = (uint16_t)(tunnelScrollMx + (uint16_t)(cellX * 2u + bc));
                        on = osdDemoTunnelTextOn(pcol, (uint8_t)(OSD_DEMO_TUNNEL_INK_Y0 + (uint8_t)trow));
                    } else {
                        const int16_t mx = (int16_t)(cellX * 2u + bc);
                        const int16_t my = (int16_t)(cellY * 3u + br);
                        // Craft 3.5-fixed span ±32; same scale on X/Y so the hole stays round
                        const int16_t x = (int16_t)(((int32_t)(mx - midX) * 32) / xDen);
                        const int16_t y = (int16_t)(((int32_t)(my - midY) * 32) / xDen);
                        const int16_t z = OSD_DEMO_TUNNEL_Z;

                        const int16_t xv = (int16_t)(((x * (int16_t)cs) - (z * (int16_t)sn)) >> 7);
                        const int16_t zv = (int16_t)(((x * (int16_t)sn) + (z * (int16_t)cs)) >> 7);

                        const uint32_t r2 = (uint32_t)((int32_t)xv * xv + (int32_t)y * y);
                        if (r2 >= OSD_DEMO_TUNNEL_HOLE2) {
                            const uint16_t dist = osdDemoIsqrt(r2);
                            uint16_t invr16 = (dist > 0) ? (uint16_t)(512u / dist) : 255u;
                            if (invr16 > 255u) {
                                invr16 = 255u;
                            }
                            const uint8_t invr = (uint8_t)invr16;
                            const int8_t dtex = (int8_t)(((int16_t)zv * (int16_t)invr) >> 7);
                            const uint8_t ang = osdDemoAtan2u8(y, xv);
                            const uint8_t u = (uint8_t)(ang + spin);
                            const uint8_t v = (uint8_t)(((uint8_t)dtex + anim) * OSD_DEMO_TUNNEL_DEP_MUL);
                            on = ((((u >> OSD_DEMO_TUNNEL_UV_SHIFT) ^ (v >> OSD_DEMO_TUNNEL_UV_SHIFT)) & 1) != 0);
                        }
                    }
                    if (on) {
                        pat |= (uint8_t)(1u << (br * 2u + bc));
                    }
                }
            }
            osdDemoWriteTunnelPat(cellX, cellY, rows, pat);
        }
    }

    // Tunnel field below max white; text band stays full bright for readability.
    for (uint8_t y = 0; y < rows && y < OSD_DEMO_RB_ROWS; y++) {
        uint8_t white = OSD_DEMO_TUNNEL_ROW_WHITE;
        if (y >= textY0 && y < (uint8_t)(textY0 + OSD_DEMO_TUNNEL_TEXT_ROWS)) {
            white = 3;
        }
        const uint8_t reg = (uint8_t)((0 << 2) | (3 - white));
        if (prevRowBright[y] != reg) {
            prevRowBright[y] = reg;
            max7456BrightnessRow(y, 0, white);
        }
    }
}


// Row-burst Display SRAM fill — much fewer SPI transactions than per-cell WriteChar.
static void osdDemoFillRowsGlyphFast(uint8_t cols, uint8_t rows, uint8_t glyphEven, uint8_t glyphOdd)
{
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > OSD_DEMO_CHARS_PER_LINE) {
        cols = OSD_DEMO_CHARS_PER_LINE;
    }
    for (uint8_t y = 0; y < rows; y++) {
        const uint16_t addr = (uint16_t)((uint16_t)y * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
        const uint8_t g = (y & 1u) ? glyphOdd : glyphEven;
        (void)max7456WriteDisplaySramRowFillEx(addr, g, cols, true);
    }
}


// Scene 7 — mid-glyph 2×2 plasma:
// Solid PX22 (all 18 rows) → both lines of each band lit. Early-odd SPI + prefetch
// so the burst stays ahead of the beam (avoids right-side 1px shear). No INV.

static int8_t plasmaSx[OSD_DEMO_CHECKER_COLS * OSD_DEMO_CHECKER_CELL_W2];
static int8_t plasmaSy[OSD_DEMO_CHECKER_ROWS * OSD_DEMO_PLASMA2X2_BANDS];
static int8_t plasmaSd[512]; // second half mirrors the first: index my+mx without & 255

static void osdDemoPlasma2x2BuildLuts(uint8_t cols, uint8_t rows, uint8_t t)
{
    const uint16_t mxMax = (uint16_t)cols * OSD_DEMO_CHECKER_CELL_W2;
    const uint16_t myMax = (uint16_t)rows * OSD_DEMO_PLASMA2X2_BANDS;
    for (uint16_t mx = 0; mx < mxMax; mx++) {
        plasmaSx[mx] = osdDemoSineAt((uint8_t)(mx * OSD_DEMO_PLASMA2X2_KX + t));
    }
    for (uint16_t my = 0; my < myMax; my++) {
        plasmaSy[my] = osdDemoSineAt((uint8_t)(my * OSD_DEMO_PLASMA2X2_KY + (uint8_t)(t << 1)));
    }
    for (uint16_t i = 0; i < 256u; i++) {
        plasmaSd[i] = osdDemoSineAt((uint8_t)(i * OSD_DEMO_PLASMA2X2_KD + t));
        plasmaSd[i + 256u] = plasmaSd[i];
    }
}

// v = Sx + Sy + Sd ∈ [−384, 381] (three int8 sines) → s = v + 384 ∈ [0, 765]: the old
// 0..767 clamp never fired, and (s·ZEBRA) >> 9 with ZEBRA = 8 is s >> 6. Sd is mirrored to
// 512 entries so the diagonal index needs no wrap. Same masks, ~half the inner-loop work.
#if OSD_DEMO_PLASMA_ZEBRA_BANDS != 8
#error "osdDemoPlasma2x2BuildMasks assumes 8 zebra bands (s >> 6)"
#endif
static void osdDemoPlasma2x2BuildMasks(uint8_t y, uint8_t b,
                                       uint8_t cols, uint8_t *glyphs, uint8_t *packs)
{
    const uint16_t my = (uint16_t)y * OSD_DEMO_PLASMA2X2_BANDS + b;
    const int16_t base = (int16_t)plasmaSy[my] + 384;
    const int8_t *sx = plasmaSx;
    const int8_t *sd = &plasmaSd[my & 0xFFu];
    for (uint8_t x = 0; x < cols; x++) {
        uint8_t pat = 0;
        for (uint8_t bc = 0; bc < OSD_DEMO_CHECKER_CELL_W2; bc++) {
            const int16_t s6 = (int16_t)(base + (int16_t)sx[bc] + (int16_t)sd[bc]);
            pat |= (uint8_t)((((uint16_t)s6 >> 6) & 1u) << bc);
        }
        sx += OSD_DEMO_CHECKER_CELL_W2;
        sd += OSD_DEMO_CHECKER_CELL_W2;
        glyphs[x] = (uint8_t)(OSD_DEMO_PX22_BASE + pat);
        packs[x] = pat;
    }
}

// Universal mid-glyph raster engine (scenes 7 and 9). Same video model as scene 8 (measured
// line period, per-field HSYNC phase, HSYNC re-lock per char row, SRAM mirror + planned
// bursts) with writes racing the beam, so every changed cell has a whole line of window.
// A scene supplies the band geometry (bandLines × bands = 18), a per-field hook (animation +
// VOS motion for the next field) and a per-band glyph builder.
typedef struct osdDemoRasterMotion_s {
    int8_t vos; // whole-OSD vertical offset, lines (+ = down)
} osdDemoRasterMotion_t;

typedef struct osdDemoRaster_s {
    uint8_t bandLines;   // video lines per band
    uint8_t bands;       // bands per character row
    bool *armed;
    timeMs_t durationMs; // auto-cycle length (ignored when the scene is held)
    // Per field, in VBLANK: animate; fill the motion applied from the NEXT field on.
    void (*field)(uint8_t cols, uint8_t rows, bool frameStart, osdDemoRasterMotion_t *next);
    void (*band)(uint8_t y, uint8_t b, uint8_t cols, uint8_t *glyphs);
} osdDemoRaster_t;

static uint8_t rasterFieldDiv; // frame grouping when twpair is off (free-running ÷2)

static void osdDemoRasterEnginePoll(const osdDemoRaster_t *rs)
{
    if (!*rs->armed || !demoDisplay) {
        return;
    }

    uint8_t cols = demoDisplay->cols;
    uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > OSD_DEMO_CHARS_PER_LINE) {
        cols = OSD_DEMO_CHARS_PER_LINE;
    }
    if (rows > OSD_DEMO_MG_ROWS_MAX) {
        rows = OSD_DEMO_MG_ROWS_MAX;
    }

    const uint32_t nominalLineTicks = clockMicrosToCycles(OSD_DEMO_PAL_LINE_US);
    const uint32_t pivotTicks = clockMicrosToCycles(OSD_DEMO_PAL_VBLANK_US
        + OSD_DEMO_MG_PIVOT_LINE * OSD_DEMO_PAL_LINE_US);
    const uint32_t prefetchLead = clockMicrosToCycles(OSD_DEMO_RASTER_PREFETCH_US);
    if (mgBurstFixedTicks == 0) {
        mgBurstFixedTicks = clockMicrosToCycles(OSD_DEMO_MG_BURST_FIXED_US);
    }
    if (mgByteTicksQ8 == 0) {
        mgByteTicksQ8 = (clockMicrosToCycles(30) << 8) / 36u;
    }

    uint8_t glyphs[2][OSD_DEMO_CHARS_PER_LINE];
    osdDemoRasterMotion_t motion = { 0 };

    max7456MidGlyphSpiBegin();

    uint8_t vsyncFails = 0;
    while (active && *rs->armed && !ARMING_FLAG(ARMED)) {
        if (!fxHold && (millis() - fxStartMs) >= rs->durationMs) {
            *rs->armed = false;
            break;
        }

        uint32_t edgeTicks = 0;
        max7456MidGlyphSpiBoost(false);
        if (!max7456WaitVsyncFallingEdge(&edgeTicks, OSD_DEMO_PAL_VSYNC_TIMEOUT_US)) {
            mgLastEdgeTicks = 0;
            if (++vsyncFails >= 8) {
                *rs->armed = false;
                break;
            }
#ifdef USE_CLI
            if (cliMode) {
                (void)cliProcess();
            }
#endif
            continue;
        }
        vsyncFails = 0;
        mgStatFields++;

        osdDemoMgTrackLinePeriod(edgeTicks, nominalLineTicks);
        const uint32_t lineQ16 = mgLineQ16;
        const uint32_t lineTicks = lineQ16 >> 16;
        // VOS moves the whole OSD by whole lines: move the schedule with it (written in VBLANK,
        // before the first OSD line of this field).
        const int8_t vos = max7456WriteVosOffsetNow(motion.vos);

        const uint32_t pivot = edgeTicks + pivotTicks
            + (uint32_t)(int32_t)((int32_t)mgShiftUs * (int32_t)clockMicrosToCycles(1))
            + (uint32_t)((int32_t)vos * (int32_t)lineTicks);
        // First lit line from the same measured model as every band (not VSYNC + 1504 us:
        // with a 65.4 us line that is ~200 us = 3 lines too late, and the VBLANK preload
        // then ran into row 0 and made its band 1 miss the beam).
        const uint32_t row0 = pivot + (uint32_t)osdDemoMgLineOffset(0, lineQ16);
        osdDemoMgMeasureFieldPhase(edgeTicks, row0, lineTicks);

        // Animation steps once per frame, on the field that starts it, so both woven fields
        // carry the same picture (twpair 1 flips the guess, twpair off = free-running ÷2).
        bool frameStart;
        if (twisterPairParity == OSD_DEMO_TWISTER_PAIR_OFF) {
            rasterFieldDiv ^= 1u;
            frameStart = rasterFieldDiv != 0u;
        } else {
            frameStart = mgFieldFirst == (twisterPairParity == 0u);
        }
        rs->field(cols, rows, frameStart, &motion);

        max7456MidGlyphSpiBoost(true);

        // VBLANK preload of band 0 for as many rows as fit before the first lit line.
        const uint32_t fullRowTicks = mgBurstFixedTicks
            + (((10u + 2u * (uint32_t)cols) * mgByteTicksQ8) >> 8); // worst case: one AI run
        const uint32_t blankDeadline = row0 - fullRowTicks;
        uint8_t yPre = 0;
        for (; yPre < rows; yPre++) {
            if ((int32_t)(getCycleCounter() - blankDeadline) > 0) {
                break;
            }
            rs->band(yPre, 0, cols, glyphs[0]);
            (void)osdDemoMgWriteRow(yPre, glyphs[0], 0, cols, 0, false, false);
        }

        int32_t hsyncRef = 0;
        bool hsyncHaveRef = false;

        for (uint8_t y = 0; y < rows; y++) {
            const uint8_t b0 = (y < yPre) ? 1u : 0u;
            // Write-free lines before a preloaded row: re-lock to the real HSYNC.
            if (y < yPre && mgHsyncLock) {
                osdDemoMgHsyncMeasure(y, pivot, lineQ16, &hsyncRef, &hsyncHaveRef);
            }

            uint8_t cur = 0;
            rs->band(y, b0, cols, glyphs[cur]);

            for (uint8_t b = b0; b < rs->bands; b++) {
                const uint16_t dueLine = (uint16_t)((uint16_t)y * OSD_DEMO_CELL_H
                                                    + (uint16_t)b * rs->bandLines);
                const uint32_t hsyncPrev = osdDemoMgHsyncAt((uint16_t)(dueLine - 1u), pivot, lineQ16);
                const bool hasNext = (uint8_t)(b + 1u) < rs->bands;

                // Build the next band while the beam is still ahead of us.
                bool nextBuilt = false;
                if (hasNext && (int32_t)(hsyncPrev - prefetchLead - getCycleCounter()) > 0) {
                    rs->band(y, (uint8_t)(b + 1u), cols, glyphs[cur ^ 1u]);
                    nextBuilt = true;
                }

                if (!osdDemoMgWriteRowChase(y, glyphs[cur], cols, hsyncPrev, lineTicks)) {
                    mgStatSkips++;
                    if (y < OSD_DEMO_MG_ROWS_MAX && mgStatSkipRow[y] < UINT16_MAX) {
                        mgStatSkipRow[y]++;
                    }
                }

                if (hasNext) {
                    if (!nextBuilt) {
                        rs->band(y, (uint8_t)(b + 1u), cols, glyphs[cur ^ 1u]);
                    }
                    cur ^= 1u;
                }
            }
        }

#ifdef USE_CLI
        if (cliMode) {
            (void)cliProcess();
        }
#endif
        max7456MidGlyphSpiBoost(false);
    }

    (void)max7456WriteVosOffsetNow(0);
    max7456MidGlyphSpiEnd();
}

// --- Scene 7: 2×2 plasma on the raster engine ---------------------------------------------

static void osdDemoPlasma2x2Field(uint8_t cols, uint8_t rows, bool frameStart,
                                  osdDemoRasterMotion_t *next)
{
    if (frameStart) {
        plasmaPhase++;
    }
    osdDemoPlasma2x2BuildLuts(cols, rows, plasmaPhase);
    next->vos = 0;
}

static void osdDemoPlasma2x2Band(uint8_t y, uint8_t b, uint8_t cols, uint8_t *glyphs)
{
    uint8_t packs[OSD_DEMO_CHECKER_COLS];
    osdDemoPlasma2x2BuildMasks(y, b, cols, glyphs, packs);
}

static void osdDemoPlasma2x2EnginePoll(void)
{
    static const osdDemoRaster_t plasmaRaster = {
        .bandLines = OSD_DEMO_PLASMA2X2_STEP,
        .bands = OSD_DEMO_PLASMA2X2_BANDS,
        .armed = &plasma2x2Armed,
        .durationMs = OSD_DEMO_FX_PLASMA2X2_MS,
        .field = osdDemoPlasma2x2Field,
        .band = osdDemoPlasma2x2Band,
    };
    osdDemoRasterEnginePoll(&plasmaRaster);
}

// --- Scene 9: FPV shoutouts ----------------------------------------------------------------
// 3×3-px logical pixels: 4 per cell × 6 bands of 3 lines → 120×96 over the screen. The 5×5
// font fills 5 bands of a character row and leaves exactly one 3-line band between rows.
// A new name appears on every quarter note, blinks white/black every frame until the next one,
// then stays white; every beat bounces the whole OSD up via VOS (hop + small rebound).

static const char *const osdDemoShoutNames[] = {
    "CHARPU", "MR STEELE", "SKITZO", "LE DRIB", "UMMAGAWD", "FINALGLIDEAUS", "STINGERSWARM",
    "MATTYSTUNTZ", "JOHNNYFPV", "NURKFPV", "CRICKETFPV", "BOTGRINDER", "ZOEFPV", "VORT3X",
    "BARDWELL", "VANOVER", "HEADSUP", "MCKFPV", "NYTFURY", "JET", "BMSTHOMAS", "PHATKID",
    "WILDWILLY", "NUBB", "GAB707", "JBOX", "PAWELOSFPV", "YUKI FPV", "DARKEX",
    "QUADMOVR", "J-TRUE", "VIKFPV", "NOICAL", "SKYWAKKA", "WESTPYSDE", "HIFLITE",
    "NATHANLOOPZ", "ERODYO", "RECKLESS_FPV", "SLATTFPV", "JACUZZI JAY", "MARIUSFPV",
    "FENIXFPV", "AUXPLUMES", "TINE_XD", "LUMPYFPV", "PDEVX", "BUBBYFPV", "YOUDONTKNOWME",
    "PATRICK WATKINS", "MAGIC CARPET", "FPVEGAN", "CIOTTIFPV", "INFINITYLOOPS", "NICK BURNS",
    "OSCAR LIANG", "CHRIS ROSSER", "UAV TECH", "MAD'S TECH",
    "BORISBSTYLE", "HYDRA", "TIMECOP", "BLCKMN", "MIKELLER", "CTZSNOOZE", "LEDVINAP",
    "JFLYPER", "MARTINBUDDEN", "SKAMAN82", "STEVECEVANS", "HASLINGHUIS", "CAPNBRY",
    "DIGITALENTITY", "ALEXINPARIS", "KILRAH",
    "ALYXFPV", "BORODA", "MARTINOSFPV", "BOGDAN", "ANIKFPV", "RECOPTER",
};
#define OSD_DEMO_SHOUT_COUNT      ARRAYLEN(osdDemoShoutNames)

// Names go to random free spots of the text area (columns 1..28, ≥ 1 character of air) on
// every quarter note; after LIFE_BEATS beats a name crumbles — every frame one random letter
// steps letter → '-' → '.' → gone — and is removed once empty. Columns 0 and 29 carry a 16-band AY
// "spectrum": per character row, a horizontal bar with 1-px resolution growing from the
// screen edge (XFILL-style partial cells, as in scene 8).
typedef struct {
    uint8_t row;
    uint8_t x;      // logical px, absolute (text area starts at column 1)
    uint8_t w;      // logical px
    uint8_t name;   // index into osdDemoShoutNames
    uint16_t born;  // beat number it appeared on
    uint32_t decay; // 2 bits per letter: 0 letter, 1 '-', 2 '.', 3 gone (crumbling once expired)
} osdDemoShoutWord_t;

static bool shoutArmed;
static uint8_t shoutFb[OSD_DEMO_SHOUT_LP_ROWS][OSD_DEMO_SHOUT_LP_COLS / 8u];
static osdDemoShoutWord_t shoutWords[OSD_DEMO_SHOUT_MAX_WORDS];
static uint8_t shoutWordCount;
static uint16_t shoutBeatNo;
static uint8_t shoutOrder[OSD_DEMO_SHOUT_COUNT];
static uint8_t shoutNext;
static int8_t shoutNewest = -1;  // word index that blinks
static bool shoutBlinkBlack;
#ifdef OSD_DEMO_CHIPTUNE
static uint8_t shoutLastPulse; // tracker line counter seen last (beat clock)
static uint8_t shoutLineAcc;
#endif
static timeMs_t shoutLastBeatMs;
static uint8_t shoutBounceT; // fields since the last beat (hop + rebound)
static bool shoutBeatPending;
static uint8_t shoutRowWhite[OSD_DEMO_MG_ROWS_MAX]; // RB white level last written per row
static uint16_t shoutBarQ8[2][OSD_DEMO_MG_ROWS_MAX]; // spectrum bars, px << 8 (left, right)

// Glyph bank @ SHOUT_BASE: 0..15 white 4-column masks on black (mask 0 = blink-off/black);
// 16..26 left bar "k px dither | black" (k = 1..11); 27..37 right bar "black | k px dither";
// 38 full dither. Checker phase from the glyph row, so 3-line bands tile seamlessly.
static void osdDemoShoutBuildGlyph(uint8_t *nvm, uint8_t idx)
{
    for (uint8_t r = 0; r < OSD_DEMO_CELL_H; r++) {
        for (uint8_t c = 0; c < OSD_DEMO_CELL_W; c += 4u) {
            uint8_t v = 0;
            for (uint8_t k = 0; k < 4u; k++) {
                const uint8_t px = (uint8_t)(c + k);
                bool on;
                if (idx < OSD_DEMO_SHOUT_BAR_L) {
                    const uint8_t lp = (uint8_t)(px / 3u); // 0..3, leftmost = mask bit 3
                    on = ((idx >> (3u - lp)) & 1u) != 0u;
                } else {
                    bool inBar;
                    if (idx < OSD_DEMO_SHOUT_BAR_R) {
                        inBar = px < (uint8_t)(idx - OSD_DEMO_SHOUT_BAR_L + 1u);
                    } else if (idx < OSD_DEMO_SHOUT_BAR_FULL) {
                        inBar = px >= (uint8_t)(OSD_DEMO_CELL_W - (idx - OSD_DEMO_SHOUT_BAR_R + 1u));
                    } else {
                        inBar = true;
                    }
                    on = inBar && ((px ^ r) & 1u) == 0u;
                }
                v = (uint8_t)((v << 2) | (on ? OSD_DEMO_PX_W : OSD_DEMO_PX_B));
            }
            nvm[(uint16_t)r * 3u + c / 4u] = v;
        }
    }
}

static void osdDemoShoutInstallGlyphs(void)
{
    uint8_t nvm[OSD_DEMO_GLYPH_BYTES];
    for (uint8_t i = 0; i < OSD_DEMO_SHOUT_GLYPHS; i++) {
        osdDemoShoutBuildGlyph(nvm, i);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_SHOUT_BASE + i), nvm);
    }
    max7456EndFontWrite();
}

static uint8_t osdDemoShoutWidth(uint8_t name)
{
    return (uint8_t)(strlen(osdDemoShoutNames[name]) * OSD_DEMO_FONT_ADVANCE - 1u);
}

static void osdDemoShoutShuffle(void)
{
    for (uint8_t i = 0; i < OSD_DEMO_SHOUT_COUNT; i++) {
        shoutOrder[i] = i;
    }
    for (uint8_t i = (uint8_t)(OSD_DEMO_SHOUT_COUNT - 1u); i > 0u; i--) {
        const uint8_t j = (uint8_t)(osdDemoRand() % (uint32_t)(i + 1u));
        const uint8_t t = shoutOrder[i];
        shoutOrder[i] = shoutOrder[j];
        shoutOrder[j] = t;
    }
    shoutNext = 0;
}

static void osdDemoShoutPaint(const osdDemoShoutWord_t *wd, bool on)
{
    const uint16_t lpRow0 = (uint16_t)wd->row * OSD_DEMO_SHOUT_BANDS;
    if (!on) {
        for (uint8_t r = 0; r < OSD_DEMO_SHOUT_FONT_ROWS; r++) {
            for (uint16_t x = wd->x; x < (uint16_t)wd->x + wd->w; x++) {
                shoutFb[lpRow0 + r][x >> 3] &= (uint8_t)~(0x80u >> (x & 7u));
            }
        }
        return;
    }
    static const char stageChar[4] = { 0, '-', '.', ' ' };
    const char *s = osdDemoShoutNames[wd->name];
    for (uint8_t ci = 0; s[ci]; ci++) {
        const uint8_t stage = (uint8_t)((wd->decay >> (2u * ci)) & 3u);
        const uint8_t code = stage ? (uint8_t)stageChar[stage] : (uint8_t)s[ci];
        if (code >= OSD_DEMO_FONT_GLYPHS) {
            continue;
        }
        for (uint8_t r = 0; r < OSD_DEMO_SHOUT_FONT_ROWS; r++) {
            const uint8_t bits = osdDemoFontBits[code][OSD_DEMO_TUNNEL_INK_Y0 + r];
            for (uint8_t px = 0; px < OSD_DEMO_FONT_W; px++) {
                if (bits & (0x80u >> px)) {
                    const uint16_t x = (uint16_t)(wd->x + ci * OSD_DEMO_FONT_ADVANCE + px);
                    shoutFb[lpRow0 + r][x >> 3] |= (uint8_t)(0x80u >> (x & 7u));
                }
            }
        }
    }
}

static bool osdDemoShoutOnScreen(uint8_t name)
{
    for (uint8_t i = 0; i < shoutWordCount; i++) {
        if (shoutWords[i].name == name) {
            return true;
        }
    }
    return false;
}

static bool osdDemoShoutFits(uint8_t row, uint8_t x, uint8_t w)
{
    for (uint8_t i = 0; i < shoutWordCount; i++) {
        const osdDemoShoutWord_t *o = &shoutWords[i];
        if (o->row != row) {
            continue;
        }
        // At least one character (6 logical px) of air between names.
        const bool leftOk = (uint16_t)x + w + OSD_DEMO_FONT_ADVANCE <= o->x;
        const bool rightOk = (uint16_t)o->x + o->w + OSD_DEMO_FONT_ADVANCE <= x;
        if (!leftOk && !rightOk) {
            return false;
        }
    }
    return true;
}

static void osdDemoShoutRemove(uint8_t i)
{
    osdDemoShoutPaint(&shoutWords[i], false);
    if (shoutNewest == (int8_t)i) {
        shoutNewest = -1;
    } else if (shoutNewest == (int8_t)(shoutWordCount - 1u)) {
        shoutNewest = (int8_t)i; // the last entry moves into the hole
    }
    shoutWords[i] = shoutWords[--shoutWordCount];
}

// One quarter note: expire old names, then place the next one at a random free spot.
static void osdDemoShoutBeat(uint8_t cols, uint8_t rows)
{
    shoutBeatNo++;
    if (shoutWordCount >= OSD_DEMO_SHOUT_MAX_WORDS) {
        osdDemoShoutRemove(0); // table full (cannot happen on a 30×16 screen, but be exact)
    }
    // Next name not already on screen.
    uint8_t name = shoutOrder[shoutNext];
    for (uint8_t n = 0; n < OSD_DEMO_SHOUT_COUNT && osdDemoShoutOnScreen(name); n++) {
        if (++shoutNext >= OSD_DEMO_SHOUT_COUNT) {
            osdDemoShoutShuffle();
        }
        name = shoutOrder[shoutNext];
    }
    const uint8_t w = osdDemoShoutWidth(name);
    const uint8_t x0 = OSD_DEMO_SHOUT_LP_PER_CELL;                                     // column 1
    const uint8_t x1 = (uint8_t)((cols - 1u) * OSD_DEMO_SHOUT_LP_PER_CELL);          // column cols-1
    if (w > (uint8_t)(x1 - x0)) {
        return;
    }
    osdDemoShoutWord_t wd = { 0, 0, w, name, shoutBeatNo, 0 };
    // No room → drop the oldest name and try again, so a new name appears on every beat.
    for (uint8_t evict = 0; evict <= OSD_DEMO_SHOUT_MAX_EVICT; evict++) {
        for (uint8_t tries = 0; tries < OSD_DEMO_SHOUT_TRIES; tries++) {
            wd.row = (uint8_t)(osdDemoRand() % rows);
            wd.x = (uint8_t)(x0 + osdDemoRand() % (uint32_t)(x1 - x0 - w + 1u));
            if (osdDemoShoutFits(wd.row, wd.x, w)) {
                if (++shoutNext >= OSD_DEMO_SHOUT_COUNT) {
                    osdDemoShoutShuffle();
                }
                shoutWords[shoutWordCount] = wd;
                shoutNewest = (int8_t)shoutWordCount;
                shoutWordCount++;
                osdDemoShoutPaint(&wd, true);
                shoutBlinkBlack = false;
                shoutBounceT = 0;
                return;
            }
        }
        if (shoutWordCount == 0u) {
            return;
        }
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < shoutWordCount; i++) {
            if ((uint16_t)(shoutBeatNo - shoutWords[i].born) > (uint16_t)(shoutBeatNo - shoutWords[oldest].born)) {
                oldest = i;
            }
        }
        osdDemoShoutRemove(oldest);
    }
}

// Beats from the tracker: PT3 lines at 50/speed Hz, a quarter ≈ 0.5 s → 2/4/8/16 lines.
// Without music: a fixed 120 BPM clock.
static uint8_t osdDemoShoutBeats(void)
{
#ifdef OSD_DEMO_CHIPTUNE
    const uint8_t speed = chiptuneGetSpeed();
    if (speed > 0u) {
        uint8_t lpb = 2;
        while (lpb < 16u && (uint16_t)lpb * speed * 2u < 50u) {
            lpb = (uint8_t)(lpb * 2u);
        }
        const uint8_t pulse = chiptuneGetLinePulse();
        shoutLineAcc = (uint8_t)(shoutLineAcc + (uint8_t)(pulse - shoutLastPulse));
        shoutLastPulse = pulse;
        uint8_t beats = 0;
        while (shoutLineAcc >= lpb) {
            shoutLineAcc = (uint8_t)(shoutLineAcc - lpb);
            beats++;
        }
        return beats;
    }
#endif
    const timeMs_t now = millis();
    if ((timeDelta_t)(now - shoutLastBeatMs) >= OSD_DEMO_SHOUT_FALLBACK_BEAT_MS) {
        shoutLastBeatMs = now;
        return 1;
    }
    return 0;
}

// AY "spectrum" from the live register frame (no FFT): each tone channel lands in a
// half-octave band by its period (row 0 = highest pitch), its volume sets the bar; noise
// lights the top bands. Stereo like the Spectrum's ABC: left = A + ½B, right = C + ½B.
static void osdDemoShoutUpdateBars(uint8_t rows)
{
    uint8_t target[2][OSD_DEMO_MG_ROWS_MAX];
    memset(target, 0, sizeof(target));
#ifdef OSD_DEMO_CHIPTUNE
    uint8_t regs[16];
    if (chiptuneGetAyRegs(regs)) {
        const uint8_t mixer = regs[7];
        for (uint8_t ch = 0; ch < 3u; ch++) {
            const uint8_t volReg = regs[8u + ch];
            const uint8_t vol = (volReg & 0x10u) ? (uint8_t)OSD_DEMO_SHOUT_ENV_LEVEL : (uint8_t)(volReg & 0x0Fu);
            if (vol == 0u) {
                continue;
            }
            const uint8_t len = (uint8_t)((vol * OSD_DEMO_CELL_W + 7u) / 15u); // 0..12 px
            const bool tone = (mixer & (1u << ch)) == 0u;
            const bool noise = (mixer & (8u << ch)) == 0u;
            uint8_t row = 0xFF;
            if (tone) {
                const uint16_t period = (uint16_t)(regs[ch * 2u] | ((regs[ch * 2u + 1u] & 0x0Fu) << 8));
                if (period > 0u) {
                    uint8_t msb = 0;
                    while ((uint16_t)(period >> (msb + 1u)) != 0u) {
                        msb++;
                    }
                    const uint8_t half = (msb > 0u) ? (uint8_t)((period >> (msb - 1u)) & 1u) : 0u;
                    // period 16 (≈ 6.9 kHz) → row 0 … period 4095 (≈ 27 Hz) → row 15.
                    const int16_t r = (int16_t)((int16_t)msb - 4) * 2 + half;
                    row = (uint8_t)constrain(r, 0, (int16_t)rows - 1);
                }
            } else if (noise) {
                row = (uint8_t)(osdDemoRand() % OSD_DEMO_SHOUT_NOISE_ROWS);
            }
            if (row == 0xFF) {
                continue;
            }
            const uint8_t side[3][2] = { { 2, 0 }, { 1, 1 }, { 0, 2 } }; // A, B, C weights /2
            for (uint8_t sd = 0; sd < 2u; sd++) {
                const uint8_t l = (uint8_t)((len * side[ch][sd]) / 2u);
                for (int8_t dr = -1; dr <= 1; dr++) { // soft neighbours
                    const int8_t rr = (int8_t)(row + dr);
                    if (rr < 0 || rr >= (int8_t)rows) {
                        continue;
                    }
                    const uint8_t v = dr ? (uint8_t)(l / 2u) : l;
                    if (v > target[sd][rr]) {
                        target[sd][rr] = v;
                    }
                }
            }
        }
    }
#endif
    for (uint8_t sd = 0; sd < 2u; sd++) {
        for (uint8_t y = 0; y < rows && y < OSD_DEMO_MG_ROWS_MAX; y++) {
            const uint16_t t = (uint16_t)target[sd][y] << 8;
            uint16_t *b = &shoutBarQ8[sd][y];
            // Attack at once, fall back slowly (classic analyser feel).
            if (t >= *b) {
                *b = t;
            } else {
                *b = (*b - t > OSD_DEMO_SHOUT_BAR_DECAY_Q8) ? (uint16_t)(*b - OSD_DEMO_SHOUT_BAR_DECAY_Q8) : t;
            }
        }
    }
}

// Once per frame: every expired name moves one random letter a step towards gone.
static void osdDemoShoutCrumble(void)
{
    for (uint8_t i = 0; i < shoutWordCount;) {
        osdDemoShoutWord_t *wd = &shoutWords[i];
        if ((uint16_t)(shoutBeatNo - wd->born) < OSD_DEMO_SHOUT_LIFE_BEATS) {
            i++;
            continue;
        }
        const uint8_t len = (uint8_t)strlen(osdDemoShoutNames[wd->name]);
        uint8_t alive = 0;
        for (uint8_t c = 0; c < len; c++) {
            alive = (uint8_t)(alive + (((wd->decay >> (2u * c)) & 3u) != 3u));
        }
        if (alive == 0u) {
            osdDemoShoutRemove(i); // fully crumbled (clears its pixels); re-check slot i
            continue;
        }
        uint8_t pick = (uint8_t)(osdDemoRand() % alive);
        for (uint8_t c = 0; c < len; c++) {
            if (((wd->decay >> (2u * c)) & 3u) != 3u && pick-- == 0u) {
                wd->decay += 1u << (2u * c);
                break;
            }
        }
        osdDemoShoutPaint(wd, false);
        osdDemoShoutPaint(wd, true);
        i++;
    }
}

// Bounce after each beat: a main hop and a small rebound (two parabolas), in lines up.
static int8_t osdDemoShoutBounce(void)
{
    static const uint8_t hopFields[2] = { OSD_DEMO_SHOUT_BOUNCE_FIELDS, OSD_DEMO_SHOUT_REBOUND_FIELDS };
    static const uint8_t hopLines[2] = { OSD_DEMO_SHOUT_BOUNCE_LINES, OSD_DEMO_SHOUT_REBOUND_LINES };
    uint8_t t = shoutBounceT;
    for (uint8_t h = 0; h < 2u; h++) {
        if (t < hopFields[h]) {
            const int32_t n = hopFields[h];
            shoutBounceT++;
            return (int8_t)-((4 * (int32_t)hopLines[h] * t * (n - t) + n * n / 2) / (n * n));
        }
        t = (uint8_t)(t - hopFields[h]);
    }
    return 0;
}

// Row brightness gradient from the active (newest) row: RB white 120 / 100 / 90 / 80 %.
static void osdDemoShoutApplyRowBrightness(uint8_t rows)
{
    const int8_t active = (shoutNewest >= 0) ? (int8_t)shoutWords[shoutNewest].row : -1;
    for (uint8_t y = 0; y < rows && y < OSD_DEMO_MG_ROWS_MAX; y++) {
        uint8_t white = 3;
        if (active >= 0) {
            const uint8_t d = (uint8_t)((y > active) ? (y - active) : (active - y));
            white = (d >= 3u) ? 0u : (uint8_t)(3u - d);
        }
        if (white != shoutRowWhite[y]) {
            shoutRowWhite[y] = white;
            max7456BrightnessRow(y, 0, white);
        }
    }
}

static void osdDemoShoutField(uint8_t cols, uint8_t rows, bool frameStart,
                              osdDemoRasterMotion_t *next)
{
    // Picture changes only on the field that starts a frame (both woven fields identical):
    // a beat that lands on the second field waits for the next frame start.
    if (osdDemoShoutBeats() > 0u) {
        shoutBeatPending = true;
    }
    if (frameStart) {
        if (shoutBeatPending) {
            shoutBeatPending = false;
            osdDemoShoutBeat(cols, rows);
        } else if (shoutNewest >= 0) {
            shoutBlinkBlack = !shoutBlinkBlack; // newest name: white/black every frame
        }
        osdDemoShoutCrumble();
        osdDemoShoutUpdateBars(rows);
    }
    osdDemoShoutApplyRowBrightness(rows); // RB registers, in VBLANK, only rows that change
    next->vos = osdDemoShoutBounce();
}

static uint8_t osdDemoShoutBarGlyph(uint16_t q8, bool right)
{
    const uint8_t px = (uint8_t)MIN(q8 >> 8, OSD_DEMO_CELL_W);
    if (px == 0u) {
        return OSD_DEMO_SHOUT_BASE;          // mask 0: black
    }
    if (px >= OSD_DEMO_CELL_W) {
        return (uint8_t)(OSD_DEMO_SHOUT_BASE + OSD_DEMO_SHOUT_BAR_FULL);
    }
    return (uint8_t)(OSD_DEMO_SHOUT_BASE + (right ? OSD_DEMO_SHOUT_BAR_R : OSD_DEMO_SHOUT_BAR_L) + px - 1u);
}

static void osdDemoShoutBand(uint8_t y, uint8_t b, uint8_t cols, uint8_t *glyphs)
{
    if (b >= OSD_DEMO_SHOUT_FONT_ROWS) {
        memset(glyphs, OSD_DEMO_SHOUT_BASE, cols); // the 3-line gap band (text and bars)
        return;
    }
    const uint8_t *line = shoutFb[(uint16_t)y * OSD_DEMO_SHOUT_BANDS + b];
    uint8_t blackL = 0xFF;
    uint8_t blackR = 0;
    if (shoutNewest >= 0 && shoutBlinkBlack && shoutWords[shoutNewest].row == y) {
        const osdDemoShoutWord_t *wd = &shoutWords[shoutNewest];
        blackL = (uint8_t)(wd->x / OSD_DEMO_SHOUT_LP_PER_CELL);
        blackR = (uint8_t)((wd->x + wd->w - 1u) / OSD_DEMO_SHOUT_LP_PER_CELL);
    }
    for (uint8_t x = 1; x + 1u < cols; x++) {
        const uint8_t byte = line[x >> 1];
        const uint8_t nib = (x & 1u) ? (uint8_t)(byte & 0x0Fu) : (uint8_t)(byte >> 4);
        const bool black = x >= blackL && x <= blackR; // whole cell belongs to this name
        glyphs[x] = (uint8_t)(OSD_DEMO_SHOUT_BASE + (black ? 0u : nib));
    }
    const uint8_t row = (y < OSD_DEMO_MG_ROWS_MAX) ? y : (uint8_t)(OSD_DEMO_MG_ROWS_MAX - 1u);
    glyphs[0] = osdDemoShoutBarGlyph(shoutBarQ8[0][row], false);
    glyphs[cols - 1u] = osdDemoShoutBarGlyph(shoutBarQ8[1][row], true);
}

static void osdDemoShoutEnginePoll(void)
{
    static const osdDemoRaster_t shoutRaster = {
        .bandLines = OSD_DEMO_SHOUT_BAND_LINES,
        .bands = OSD_DEMO_SHOUT_BANDS,
        .armed = &shoutArmed,
        .durationMs = OSD_DEMO_FX_SHOUT_MS,
        .field = osdDemoShoutField,
        .band = osdDemoShoutBand,
    };
    osdDemoRasterEnginePoll(&shoutRaster);
}

static void osdDemoShoutEnter(void)
{
    max7456Osdm(0x1B);
    max7456Brightness(0, 3);
    max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
    max7456SetHudMotionOffset(0, 0);
    osdDemoShoutInstallGlyphs(); // 39 × NVM write, once per scene entry
    if (demoDisplay) {
        osdDemoFillRowsGlyphFast(demoDisplay->cols, demoDisplay->rows,
                                 OSD_DEMO_SHOUT_BASE, OSD_DEMO_SHOUT_BASE);
    } else {
        max7456FillScreen(OSD_DEMO_SHOUT_BASE);
    }
    memset(mgSram, 0xFF, sizeof(mgSram)); // 0xFF never in a row → first field rewrites all
    mgLastEdgeTicks = 0;
    osdDemoMgResetStats();
    osdDemoShoutShuffle();
    memset(shoutFb, 0, sizeof(shoutFb));
    shoutWordCount = 0;
    shoutBeatNo = 0;
    shoutNewest = -1;
    memset(shoutBarQ8, 0, sizeof(shoutBarQ8));
#ifdef OSD_DEMO_CHIPTUNE
    shoutLastPulse = chiptuneGetLinePulse();
#endif
#ifdef OSD_DEMO_CHIPTUNE
    shoutLineAcc = 0;
#endif
    shoutLastBeatMs = millis();
    shoutBeatPending = true; // first name right away
    shoutBounceT = OSD_DEMO_SHOUT_BOUNCE_FIELDS + OSD_DEMO_SHOUT_REBOUND_FIELDS;
    shoutBlinkBlack = false;
    memset(shoutRowWhite, 0xFF, sizeof(shoutRowWhite)); // force the first gradient write
    shoutArmed = true;
}

// --- Scene 10: scene 7's plasma on an interrupt engine (no busy-wait) ----------------------
// Same video model, planner, encoder and race-the-beam windows as scene 7, but nothing spins:
//  - TIM5 compare interrupts fire at each event (VSYNC window, field phase, every band start);
//  - each band's burst is planned + encoded at the end of the previous band's interrupt and
//    sent from its own interrupt (TX-only polled SPI, IRQs masked for the burst only);
//  - the plasma masks for a whole frame are built by the normal OSD task into a double buffer;
//  - VSYNC is predicted from the measured field period and polled only in a short window.
// The scheduler, CLI and USB keep running in between. SPI2 TX DMA is not usable on this board
// (DMA1 Stream4 belongs to motor 1 DShot), hence a polled burst inside the interrupt.
#if defined(STM32F4)
#define OSD_DEMO_IRQ_ENGINE
#endif

#ifdef OSD_DEMO_IRQ_ENGINE

typedef enum {
    IRQ_EV_VSYNC = 0, // poll STAT for the VSYNC edge (window opened just before the prediction)
    IRQ_EV_PHASE,     // HSYNC phase + parity in VBLANK, frame swap, band-0 preload, first band
    IRQ_EV_BAND,      // send the prepared band, prepare + schedule the next
} osdDemoIrqEvent_e;

static const timerHardware_t osdDemoIrqTimHw = {
    .tim = (timerResource_t *)TIM5,
    .tag = IO_TAG_NONE,
    .channel = TIM_Channel_1,
};
static timerEdgeHandlerRec_t osdDemoIrqEdgeRec;
static bool plasmaIrqArmed;          // scene 10 running (ISR chain alive)
static volatile uint8_t irqEvent;
static uint8_t irqMasks[2][OSD_DEMO_CHECKER_ROWS][OSD_DEMO_PLASMA2X2_BANDS][OSD_DEMO_CHECKER_COLS];
static volatile uint8_t irqPlay;     // mask buffer being displayed
static volatile bool irqNextReady;   // the other buffer holds a complete new frame
static uint8_t irqCols;
static uint8_t irqRows;
static uint8_t irqY;
static uint8_t irqB;
static uint8_t irqYPre;
static uint32_t irqEdge;
static uint32_t irqPivot;
static uint32_t irqRow0;
static uint32_t irqLineQ16;
static uint32_t irqLineTicks;
static uint32_t irqTimRatioQ16;      // TIM5 ticks per DWT tick, Q16
static bool irqHaveBurst;
static osdDemoMgBurst_t irqBurst;
// Stats (twstat): CPU time inside the ISR and in the frame-building task, late wake-ups.
static uint64_t irqIsrTicks;  // 64-bit: 40 % of 108 MHz overflows 32 bits in ~100 s
static uint64_t irqTaskTicks;
static uint32_t irqLateUs;           // worst wake-up lateness vs the planned start
static uint32_t irqVsyncMiss;
static uint16_t irqVsyncLeadUs = OSD_DEMO_IRQ_VSYNC_LEAD_US; // widened after a miss

static void osdDemoIrqArm(uint32_t dwtAt)
{
    TIM_TypeDef *tim = TIM5;
    int32_t d = (int32_t)(dwtAt - getCycleCounter());
    const int32_t minLead = (int32_t)clockMicrosToCycles(1);
    if (d < minLead) {
        d = minLead; // already due: fire as soon as possible
    }
    const uint32_t dt = (uint32_t)(((uint64_t)(uint32_t)d * irqTimRatioQ16) >> 16);
    tim->SR = (uint16_t)~TIM_IT_CC1;
    tim->CCR1 = tim->CNT + dt;
}

static void osdDemoIrqScheduleVsync(void)
{
    // Open the STAT window a little before the predicted edge (field period from TrackLine).
    const uint32_t field = (mgFieldLastTicks != 0u)
        ? mgFieldLastTicks : clockMicrosToCycles(OSD_DEMO_IRQ_NOMINAL_FIELD_US);
    irqEvent = IRQ_EV_VSYNC;
    osdDemoIrqArm(irqEdge + field - clockMicrosToCycles(irqVsyncLeadUs));
}

// Prepare the burst for (irqY, irqB) and arm its start; advances past empty / infeasible bands.
static void osdDemoIrqPrepareNext(void)
{
    for (;;) {
        if (irqY >= irqRows) {
            irqHaveBurst = false;
            osdDemoIrqScheduleVsync();
            return;
        }
        const uint16_t dueLine = (uint16_t)((uint16_t)irqY * OSD_DEMO_CELL_H
                                            + (uint16_t)irqB * OSD_DEMO_PLASMA2X2_STEP);
        const uint32_t hsyncPrev = irqPivot + (uint32_t)(osdDemoMgLineOffset((uint16_t)(dueLine - 1u), irqLineQ16)
                                                         + mgHsyncPhase);
        const uint8_t *glyphs = irqMasks[irqPlay][irqY][irqB];
        const uint8_t y = irqY;
        // Advance the cursor to the following band now.
        if (++irqB >= OSD_DEMO_PLASMA2X2_BANDS) {
            irqY++;
            irqB = (irqY < irqYPre) ? 1u : 0u;
        }
        if (!osdDemoMgChasePrepare(y, glyphs, irqCols, hsyncPrev, irqLineTicks, &irqBurst)) {
            continue; // nothing changed in this band
        }
        if ((int32_t)(irqBurst.latest - irqBurst.startAt) < 0) {
            mgStatSkips++;
            continue;
        }
        irqHaveBurst = true;
        irqEvent = IRQ_EV_BAND;
        osdDemoIrqArm(irqBurst.startAt);
        return;
    }
}

static void osdDemoIrqVsync(void)
{
    uint32_t edge = 0;
    max7456MidGlyphSpiBoost(false);
    if (max7456WaitVsyncFallingEdge(&edge, (timeUs_t)irqVsyncLeadUs + OSD_DEMO_IRQ_VSYNC_WINDOW_US)) {
        osdDemoMgTrackLinePeriod(edge, clockMicrosToCycles(OSD_DEMO_PAL_LINE_US));
        irqVsyncLeadUs = OSD_DEMO_IRQ_VSYNC_LEAD_US;
    } else {
        // Lost it: dead-reckon one field and look again next time with a wider window
        // (doubling up to ~4 ms) so the lock comes back in a field or two.
        irqVsyncMiss++;
        irqVsyncLeadUs = (uint16_t)MIN((uint32_t)irqVsyncLeadUs * 2u, (uint32_t)OSD_DEMO_IRQ_VSYNC_LEAD_MAX_US);
        edge = irqEdge + ((mgFieldLastTicks != 0u)
            ? mgFieldLastTicks : clockMicrosToCycles(OSD_DEMO_IRQ_NOMINAL_FIELD_US));
        mgLastEdgeTicks = 0;
    }
    irqEdge = edge;
    mgStatFields++;
    irqLineQ16 = mgLineQ16;
    irqLineTicks = irqLineQ16 >> 16;
    irqPivot = edge + clockMicrosToCycles(OSD_DEMO_PAL_VBLANK_US + OSD_DEMO_MG_PIVOT_LINE * OSD_DEMO_PAL_LINE_US)
        + (uint32_t)(int32_t)((int32_t)mgShiftUs * (int32_t)clockMicrosToCycles(1));
    irqRow0 = irqPivot + (uint32_t)osdDemoMgLineOffset(0, irqLineQ16);
    irqEvent = IRQ_EV_PHASE;
    osdDemoIrqArm(irqRow0 - (uint32_t)OSD_DEMO_MG_FIELD_PHASE_LINES * irqLineTicks - irqLineTicks * 3u / 8u);
}

static void osdDemoIrqPhase(void)
{
    // MeasureFieldPhase waits until its own window (we are already there) and polls HSYNC.
    osdDemoMgMeasureFieldPhase(irqEdge, irqRow0, irqLineTicks);
    const bool frameStart = (twisterPairParity == OSD_DEMO_TWISTER_PAIR_OFF)
        || (mgFieldFirst == (twisterPairParity == 0u));
    if (frameStart && irqNextReady) {
        irqPlay ^= 1u;   // a new frame on the field that starts it (both fields identical)
        irqNextReady = false;
    }
    max7456MidGlyphSpiBoost(true);

    // VBLANK preload of band 0 for as many rows as fit.
    const uint32_t fullRowTicks = mgBurstFixedTicks
        + (((10u + 2u * (uint32_t)irqCols) * mgByteTicksQ8) >> 8);
    irqYPre = 0;
    for (; irqYPre < irqRows; irqYPre++) {
        if ((int32_t)(getCycleCounter() - (irqRow0 - fullRowTicks)) > 0) {
            break;
        }
        (void)osdDemoMgWriteRow(irqYPre, irqMasks[irqPlay][irqYPre][0], 0, irqCols, 0, false, true);
    }
    irqY = 0;
    irqB = (irqYPre > 0u) ? 1u : 0u;
    osdDemoIrqPrepareNext();
}

static void osdDemoIrqBand(void)
{
    if (irqHaveBurst) {
        // Timer wake-up lands a few us before/after startAt; never start early.
        const int32_t late = (int32_t)(getCycleCounter() - irqBurst.startAt);
        if (late > 0) {
            const uint32_t us = (uint32_t)late / clockMicrosToCycles(1);
            if (us > irqLateUs) {
                irqLateUs = us;
            }
        }
        osdDemoWaitCycles(irqBurst.startAt);
        if (!osdDemoMgBurstSend(&irqBurst)) {
            mgStatSkips++;
            if (irqBurst.row < OSD_DEMO_MG_ROWS_MAX && mgStatSkipRow[irqBurst.row] < UINT16_MAX) {
                mgStatSkipRow[irqBurst.row]++;
            }
        }
    }
    osdDemoIrqPrepareNext();
}

static void osdDemoIrqIsr(timerEdgeHandlerRec_t *cbRec, captureCompare_t capture)
{
    UNUSED(cbRec);
    UNUSED(capture);
    if (!plasmaIrqArmed) {
        return;
    }
    const uint32_t t0 = getCycleCounter();
    switch (irqEvent) {
    case IRQ_EV_VSYNC:
        osdDemoIrqVsync();
        break;
    case IRQ_EV_PHASE:
        osdDemoIrqPhase();
        break;
    default:
        osdDemoIrqBand();
        break;
    }
    irqIsrTicks += getCycleCounter() - t0;
}

static void osdDemoPlasmaIrqBuild(uint8_t buf)
{
    plasmaPhase++;
    osdDemoPlasma2x2BuildLuts(irqCols, irqRows, plasmaPhase);
    uint8_t packs[OSD_DEMO_CHECKER_COLS];
    for (uint8_t y = 0; y < irqRows; y++) {
        for (uint8_t b = 0; b < OSD_DEMO_PLASMA2X2_BANDS; b++) {
            osdDemoPlasma2x2BuildMasks(y, b, irqCols, irqMasks[buf][y][b], packs);
        }
    }
}

// OSD task side: build the next frame's masks whenever the ISR has taken the previous one.
static void osdDemoPlasmaIrqTask(void)
{
    if (!plasmaIrqArmed || irqNextReady) {
        return;
    }
    const uint32_t t0 = getCycleCounter();
    osdDemoPlasmaIrqBuild((uint8_t)(irqPlay ^ 1u));
    __DSB();
    irqNextReady = true;
    irqTaskTicks += getCycleCounter() - t0;
}

static void osdDemoPlasmaIrqStop(void)
{
    if (!plasmaIrqArmed) {
        return;
    }
    plasmaIrqArmed = false;
    TIM_ITConfig(TIM5, TIM_IT_CC1, DISABLE);
    timerChannelConfigCallbacks(&osdDemoIrqTimHw, NULL, NULL);
    (void)max7456WriteVosOffsetNow(0);
    max7456MidGlyphSpiEnd();
#ifdef OSD_DEMO_CHIPTUNE
    beeperPwmAySetIrqBoost(chiptuneSchedulerIsParked());
#endif
}

static bool osdDemoPlasmaIrqStart(void)
{
    if (!demoDisplay) {
        return false;
    }
    irqCols = MIN(demoDisplay->cols, OSD_DEMO_CHECKER_COLS);
    irqRows = MIN(demoDisplay->rows, OSD_DEMO_CHECKER_ROWS);
    if (mgBurstFixedTicks == 0) {
        mgBurstFixedTicks = clockMicrosToCycles(OSD_DEMO_MG_BURST_FIXED_US);
    }
    if (mgByteTicksQ8 == 0) {
        mgByteTicksQ8 = (clockMicrosToCycles(30) << 8) / 36u;
    }

    // First frame into buffer 0.
    irqPlay = 0;
    irqNextReady = false;
    osdDemoPlasmaIrqBuild(0);

    // TIM5: free-running 32-bit at the timer clock; CC1 = event time.
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);
    TIM_TimeBaseInitTypeDef tb;
    TIM_TimeBaseStructInit(&tb);
    tb.TIM_Prescaler = 0;
    tb.TIM_Period = 0xFFFFFFFFu;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM5, &tb);
    TIM_OCInitTypeDef oc;
    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_Timing;
    TIM_OC1Init(TIM5, &oc);
    TIM_Cmd(TIM5, ENABLE);
    irqTimRatioQ16 = (uint32_t)(((uint64_t)timerClock(&osdDemoIrqTimHw) << 16)
                                / ((uint64_t)clockMicrosToCycles(1) * 1000000u));

    // Above the audio IRQ (dropped to timer priority while we run) and gyro-level otherwise.
#ifdef OSD_DEMO_CHIPTUNE
    beeperPwmAySetIrqBoost(false);
#endif
    NVIC_InitTypeDef nvic;
    nvic.NVIC_IRQChannel = TIM5_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = NVIC_PRIORITY_BASE(NVIC_PRIO_MAX);
    nvic.NVIC_IRQChannelSubPriority = NVIC_PRIORITY_SUB(NVIC_PRIO_MAX);
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    max7456MidGlyphSpiBegin();
    // One blocking VSYNC lock to seed the prediction; from then on only short windows.
    uint32_t edge = 0;
    if (!max7456WaitVsyncFallingEdge(&edge, OSD_DEMO_PAL_VSYNC_TIMEOUT_US)) {
        max7456MidGlyphSpiEnd();
        return false;
    }
    mgLastEdgeTicks = 0;
    osdDemoMgTrackLinePeriod(edge, clockMicrosToCycles(OSD_DEMO_PAL_LINE_US));
    irqEdge = edge;
    irqIsrTicks = 0;
    irqTaskTicks = 0;
    irqLateUs = 0;
    irqVsyncMiss = 0;
    irqVsyncLeadUs = OSD_DEMO_IRQ_VSYNC_LEAD_US;

    plasmaIrqArmed = true;
    osdDemoIrqScheduleVsync(); // arm CC1 first, then enable its interrupt (no stale match)
    timerChannelEdgeHandlerInit(&osdDemoIrqEdgeRec, osdDemoIrqIsr);
    timerChannelConfigCallbacks(&osdDemoIrqTimHw, &osdDemoIrqEdgeRec, NULL);
    return true;
}

void osdDemoPlasmaIrqGetStats(uint64_t *isrTicks, uint64_t *taskTicks, uint32_t *lateUs,
                              uint32_t *vsyncMiss, bool reset)
{
    *isrTicks = irqIsrTicks;
    *taskTicks = irqTaskTicks;
    *lateUs = irqLateUs;
    *vsyncMiss = irqVsyncMiss;
    if (reset) {
        irqIsrTicks = 0;
        irqTaskTicks = 0;
        irqLateUs = 0;
        irqVsyncMiss = 0;
    }
}

#else // !OSD_DEMO_IRQ_ENGINE

static bool plasmaIrqArmed;
static void osdDemoPlasmaIrqTask(void) {}
static void osdDemoPlasmaIrqStop(void) { plasmaIrqArmed = false; }
static bool osdDemoPlasmaIrqStart(void) { return false; }
void osdDemoPlasmaIrqGetStats(uint64_t *isrTicks, uint64_t *taskTicks, uint32_t *lateUs,
                              uint32_t *vsyncMiss, bool reset)
{
    UNUSED(reset);
    *isrTicks = *taskTicks = *lateUs = *vsyncMiss = 0;
}

#endif // OSD_DEMO_IRQ_ENGINE



// ---------------------------------------------------------------------------
// 6px logical-block edge glyphs (1 physical px phases). Preloaded at InstallFont.
// INV supplies reverse directions — no second inverted glyph bank.
// ---------------------------------------------------------------------------

// Scene 8 — classic vertical twister (rotating square cross-section).
// 4 projected edges → front-facing intervals; widths from geometry, not cells.
// Paint faces into a per-px color buffer, then encode cells so WHITE↔DITHER
// shares a 1px phase (WD/DW) instead of the second face clobbering the first.

enum {
    OSD_DEMO_TWISTER_PIX_BLACK = 0, // 00
    OSD_DEMO_TWISTER_PIX_WHITE = 2, // 10
};

// One checker for every dither glyph (D/DT/WD/DW/full-D). Phase matches plasma 0xEE /
// EDGE_BD: top-left (0,0) = WHITE. Even cell width → seamless across character joins.
static uint8_t osdDemoTwisterDitherPix(uint8_t x, uint8_t row)
{
    return (((x ^ row) & 1u) == 0u) ? (uint8_t)OSD_DEMO_TWISTER_PIX_WHITE
                                    : (uint8_t)OSD_DEMO_TWISTER_PIX_BLACK;
}

static void osdDemoTwisterPackRow(uint8_t *dst3, const uint8_t pix12[12])
{
    for (uint8_t b = 0; b < 3; b++) {
        uint8_t v = 0;
        for (uint8_t i = 0; i < 4; i++) {
            v = (uint8_t)((v << 2) | (pix12[b * 4u + i] & 3u));
        }
        dst3[b] = v;
    }
}

static void osdDemoTwisterBuildXFillWhite(uint8_t *nvm54, uint8_t leftWhitePx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            pix[x] = (x < leftWhitePx) ? (uint8_t)OSD_DEMO_TWISTER_PIX_WHITE
                                       : (uint8_t)OSD_DEMO_TWISTER_PIX_BLACK;
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

static void osdDemoTwisterBuildXFillDitherLeft(uint8_t *nvm54, uint8_t leftDitherPx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            if (x >= leftDitherPx) {
                pix[x] = (uint8_t)OSD_DEMO_TWISTER_PIX_BLACK;
            } else {
                pix[x] = osdDemoTwisterDitherPix(x, row);
            }
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

static void osdDemoTwisterBuildXFillDitherTail(uint8_t *nvm54, uint8_t leftBlackPx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            if (x < leftBlackPx) {
                pix[x] = (uint8_t)OSD_DEMO_TWISTER_PIX_BLACK;
            } else {
                pix[x] = osdDemoTwisterDitherPix(x, row);
            }
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

static void osdDemoTwisterBuildXFillWhiteDither(uint8_t *nvm54, uint8_t leftWhitePx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            if (x < leftWhitePx) {
                pix[x] = (uint8_t)OSD_DEMO_TWISTER_PIX_WHITE;
            } else {
                pix[x] = osdDemoTwisterDitherPix(x, row);
            }
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

static void osdDemoTwisterBuildXFillDitherWhite(uint8_t *nvm54, uint8_t leftDitherPx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            if (x < leftDitherPx) {
                pix[x] = osdDemoTwisterDitherPix(x, row);
            } else {
                pix[x] = (uint8_t)OSD_DEMO_TWISTER_PIX_WHITE;
            }
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

static void osdDemoTwisterBuildXFillBlackWhite(uint8_t *nvm54, uint8_t leftBlackPx)
{
    uint8_t pix[12];
    for (uint8_t row = 0; row < OSD_DEMO_CELL_H; row++) {
        for (uint8_t x = 0; x < OSD_DEMO_CELL_W; x++) {
            pix[x] = (x < leftBlackPx) ? (uint8_t)OSD_DEMO_TWISTER_PIX_BLACK
                                       : (uint8_t)OSD_DEMO_TWISTER_PIX_WHITE;
        }
        osdDemoTwisterPackRow(&nvm54[(uint16_t)row * 3u], pix);
    }
}

// One-time NVM install (EnterFx only — never during animation).
// PX22 0x40..0x7F; no INV glyphs — WT covers black|white. Leaves HFILL @ 0x80.
static void osdDemoTwisterInstallXFillGlyphs(void)
{
    uint8_t nvm[OSD_DEMO_GLYPH_BYTES];
    for (uint8_t k = 1; k < OSD_DEMO_CELL_W; k++) {
        const uint8_t idx = (uint8_t)(k - 1u);
        osdDemoTwisterBuildXFillWhite(nvm, k);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_W_BASE + idx), nvm);
        osdDemoTwisterBuildXFillDitherLeft(nvm, k);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_D_BASE + idx), nvm);
        osdDemoTwisterBuildXFillDitherTail(nvm, k);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_DT_BASE + idx), nvm);
        osdDemoTwisterBuildXFillBlackWhite(nvm, k);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_WT_BASE + idx), nvm);
        osdDemoTwisterBuildXFillWhiteDither(nvm, k);
        (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_WD_BASE + idx), nvm);
        if (k <= OSD_DEMO_TWISTER_XFILL_DW_MAX_K) {
            osdDemoTwisterBuildXFillDitherWhite(nvm, k);
            (void)max7456WriteNvm((uint8_t)(OSD_DEMO_TWISTER_XFILL_DW_BASE + idx), nvm);
        }
    }
    max7456EndFontWrite();
}

static void osdDemoTwisterSolidGlyph(uint8_t color, uint8_t *glyph, uint8_t *inv)
{
    *inv = 0;
    if (color == OSD_DEMO_TWISTER_C_W) {
        *glyph = OSD_DEMO_TWISTER_GLYPH_W;
    } else if (color == OSD_DEMO_TWISTER_C_D) {
        *glyph = OSD_DEMO_TWISTER_GLYPH_D;
    } else {
        *glyph = OSD_DEMO_TWISTER_GLYPH_B;
    }
}

// Encode a clean 2-run cell [0,t)=c0, [t,12)=c1. Never sets INV (single SPI AI run).
static void osdDemoTwisterEncodeTwoRun(uint8_t c0, uint8_t c1, uint8_t t,
                                       uint8_t *glyph, uint8_t *inv)
{
    *inv = 0;
    if (t == 0) {
        osdDemoTwisterSolidGlyph(c1, glyph, inv);
        return;
    }
    if (t >= OSD_DEMO_CELL_W) {
        osdDemoTwisterSolidGlyph(c0, glyph, inv);
        return;
    }

    const uint8_t idx = (uint8_t)(t - 1u);
    if (c0 == OSD_DEMO_TWISTER_C_W && c1 == OSD_DEMO_TWISTER_C_B) {
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_W_BASE + idx);
        return;
    }
    if (c0 == OSD_DEMO_TWISTER_C_B && c1 == OSD_DEMO_TWISTER_C_W) {
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_WT_BASE + idx);
        return;
    }
    if (c0 == OSD_DEMO_TWISTER_C_D && c1 == OSD_DEMO_TWISTER_C_B) {
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_D_BASE + idx);
        return;
    }
    if (c0 == OSD_DEMO_TWISTER_C_B && c1 == OSD_DEMO_TWISTER_C_D) {
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_DT_BASE + idx);
        return;
    }
    if (c0 == OSD_DEMO_TWISTER_C_W && c1 == OSD_DEMO_TWISTER_C_D) {
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_WD_BASE + idx);
        return;
    }
    if (c0 == OSD_DEMO_TWISTER_C_D && c1 == OSD_DEMO_TWISTER_C_W) {
        // DW bank only has k=1..9. For t=10..11 under-draw to solid D — never clamp
        // down to k=9 (that over-draws white and flashes a wider face).
        if (t > OSD_DEMO_TWISTER_XFILL_DW_MAX_K) {
            osdDemoTwisterSolidGlyph(OSD_DEMO_TWISTER_C_D, glyph, inv);
            return;
        }
        *glyph = (uint8_t)(OSD_DEMO_TWISTER_XFILL_DW_BASE + idx);
        return;
    }
    osdDemoTwisterSolidGlyph(c0, glyph, inv);
}

// Classify one 12px cell. Extreme X (thin faces / 3+ runs) must NEVER expand a color
// past its true span — that picks a wider XFILL and flashes a fat ribbon.
static void osdDemoTwisterEncodeCell(const uint8_t pix[12], uint8_t *glyph, uint8_t *inv)
{
    *inv = 0;

    uint8_t runC[4];
    uint8_t runS[5];
    uint8_t n = 1;
    runS[0] = 0;
    runC[0] = pix[0];
    for (uint8_t i = 1; i < OSD_DEMO_CELL_W; i++) {
        if (pix[i] != runC[n - 1u]) {
            if (n >= 4u) {
                break;
            }
            runS[n] = i;
            runC[n] = pix[i];
            n++;
        }
    }
    runS[n] = OSD_DEMO_CELL_W;

    if (n == 1u) {
        osdDemoTwisterSolidGlyph(runC[0], glyph, inv);
        return;
    }
    if (n == 2u) {
        osdDemoTwisterEncodeTwoRun(runC[0], runC[1], runS[1], glyph, inv);
        return;
    }

    const uint8_t leftC = pix[0];
    const uint8_t rightC = pix[OSD_DEMO_CELL_W - 1u];

    // B|C|B islands: snap to nearest cell edge when close (smooth ribbon), else suppress.
    // Never use B|C-to-end — that flashes a full-cell-wide square.
    if (leftC == OSD_DEMO_TWISTER_C_B && rightC == OSD_DEMO_TWISTER_C_B) {
        if (n >= 3u && runC[1] != OSD_DEMO_TWISTER_C_B) {
            const uint8_t a = runS[1];
            const uint8_t b = runS[2];
            const uint8_t midC = runC[1];
            if (a <= 2u) {
                // Near left: C|B ending at b (at most +2px left expand).
                osdDemoTwisterEncodeTwoRun(midC, OSD_DEMO_TWISTER_C_B, b, glyph, inv);
                return;
            }
            if (b >= (uint8_t)(OSD_DEMO_CELL_W - 2u)) {
                // Near right: B|C starting at a (at most +2px right expand).
                osdDemoTwisterEncodeTwoRun(OSD_DEMO_TWISTER_C_B, midC, a, glyph, inv);
                return;
            }
        }
        osdDemoTwisterSolidGlyph(OSD_DEMO_TWISTER_C_B, glyph, inv);
        return;
    }

    // Silhouette edge + face edge in one cell (B|W|D, B|D|W, W|D|B, D|W|B — 0.5% of cells).
    // No 3-run glyphs exist, so keep the SILHOUETTE edge exact and paint the ribbon part with
    // whichever face covers more of it. The old rule kept the face edge and blacked out the
    // middle face — a black notch "cutting" the ribbon at every B/W/D junction.
    if (leftC == OSD_DEMO_TWISTER_C_B || rightC == OSD_DEMO_TWISTER_C_B) {
        uint8_t a = 0;
        uint8_t b = OSD_DEMO_CELL_W;
        if (leftC == OSD_DEMO_TWISTER_C_B) {
            while (a < OSD_DEMO_CELL_W && pix[a] == OSD_DEMO_TWISTER_C_B) {
                a++;
            }
        } else {
            while (b > 0u && pix[b - 1u] == OSD_DEMO_TWISTER_C_B) {
                b--;
            }
        }
        uint8_t nW = 0;
        uint8_t nD = 0;
        bool solidRibbon = true;
        for (uint8_t i = a; i < b; i++) {
            if (pix[i] == OSD_DEMO_TWISTER_C_W) {
                nW++;
            } else if (pix[i] == OSD_DEMO_TWISTER_C_D) {
                nD++;
            } else {
                solidRibbon = false; // inner black seam: leave it to the rules below
            }
        }
        if (solidRibbon && a < b) {
            const uint8_t face = (nW >= nD) ? OSD_DEMO_TWISTER_C_W : OSD_DEMO_TWISTER_C_D;
            if (leftC == OSD_DEMO_TWISTER_C_B) {
                osdDemoTwisterEncodeTwoRun(OSD_DEMO_TWISTER_C_B, face, a, glyph, inv);
            } else {
                osdDemoTwisterEncodeTwoRun(face, OSD_DEMO_TWISTER_C_B, b, glyph, inv);
            }
            return;
        }
    }

    // B|…|R where R is a pure suffix: encode B|R at suffix start (left under-drawn).
    if (leftC == OSD_DEMO_TWISTER_C_B) {
        uint8_t t = OSD_DEMO_CELL_W;
        while (t > 0u && pix[t - 1u] == rightC) {
            t--;
        }
        osdDemoTwisterEncodeTwoRun(OSD_DEMO_TWISTER_C_B, rightC, t, glyph, inv);
        return;
    }

    // L|…|B where L is a pure prefix: encode L|B (right under-drawn).
    if (rightC == OSD_DEMO_TWISTER_C_B) {
        uint8_t t = 0;
        while (t < OSD_DEMO_CELL_W && pix[t] == leftC) {
            t++;
        }
        osdDemoTwisterEncodeTwoRun(leftC, OSD_DEMO_TWISTER_C_B, t, glyph, inv);
        return;
    }

    // Both edges colored (W|B|D, W|D, …). Right color must stay a suffix.
    {
        uint8_t tR = OSD_DEMO_CELL_W;
        while (tR > 0u && pix[tR - 1u] == rightC) {
            tR--;
        }
        uint8_t tL = 0;
        while (tL < OSD_DEMO_CELL_W && pix[tL] == leftC) {
            tL++;
        }

        bool leftPureToJoin = true;
        for (uint8_t i = 0; i < tR; i++) {
            if (pix[i] != leftC) {
                leftPureToJoin = false;
                break;
            }
        }
        if (leftPureToJoin && leftC != rightC) {
            osdDemoTwisterEncodeTwoRun(leftC, rightC, tR, glyph, inv);
            return;
        }

        // Thin black seam between two faces (W|B|D, ≤2px): absorb into leftC|rightC
        // so the join stays 1px-smooth instead of a square black notch.
        if (leftC != rightC && tL < tR) {
            bool thinBlackSeam = true;
            for (uint8_t i = tL; i < tR; i++) {
                if (pix[i] != OSD_DEMO_TWISTER_C_B) {
                    thinBlackSeam = false;
                    break;
                }
            }
            if (thinBlackSeam && (uint8_t)(tR - tL) <= 2u) {
                osdDemoTwisterEncodeTwoRun(leftC, rightC, tR, glyph, inv);
                return;
            }
        }

        // Mixed middle: keep the wider true edge, under-draw the other.
        const uint8_t leftSpan = tL;
        const uint8_t rightSpan = (uint8_t)(OSD_DEMO_CELL_W - tR);
        if (rightSpan >= leftSpan) {
            osdDemoTwisterEncodeTwoRun(OSD_DEMO_TWISTER_C_B, rightC, tR, glyph, inv);
        } else {
            osdDemoTwisterEncodeTwoRun(leftC, OSD_DEMO_TWISTER_C_B, leftSpan, glyph, inv);
        }
    }
}

// Fast scanline: only the silhouette window, no full-width color buffer / INV.
static void osdDemoTwisterBuildScanline(uint8_t *glyphs, uint8_t cols,
                                        uint8_t phase, int16_t centerX,
                                        uint8_t winL, uint8_t winR)
{
    if (winR > cols) {
        winR = cols;
    }
    if (winL >= winR) {
        return;
    }

    int16_t e[4];
    for (uint8_t k = 0; k < 4; k++) {
        const int8_t s = osdDemoSineAt((uint8_t)(phase + (uint8_t)(k * 64u)));
        e[k] = (int16_t)(centerX + (((int16_t)OSD_DEMO_TWISTER_AMP_PX * (int16_t)s) / 128));
    }

    int16_t fL[2];
    int16_t fR[2];
    uint8_t fC[2];
    uint8_t nFace = 0;
    for (uint8_t i = 0; i < 4; i++) {
        const int16_t x0 = e[i];
        const int16_t x1 = e[(uint8_t)((i + 1u) & 3u)];
        if (x1 > x0 && nFace < 2u) {
            fL[nFace] = x0;
            fR[nFace] = x1;
            fC[nFace] = ((i & 1u) == 0u) ? OSD_DEMO_TWISTER_C_W : OSD_DEMO_TWISTER_C_D;
            nFace++;
        }
    }
    // Left-to-right paint order — stable shared edge, no right-face flash over left.
    if (nFace == 2u && fL[1] < fL[0]) {
        const int16_t sl = fL[0];
        const int16_t sr = fR[0];
        const uint8_t sc = fC[0];
        fL[0] = fL[1];
        fR[0] = fR[1];
        fC[0] = fC[1];
        fL[1] = sl;
        fR[1] = sr;
        fC[1] = sc;
    }

    for (uint8_t x = winL; x < winR; x++) {
        const int16_t cell0 = (int16_t)((uint16_t)x * (uint16_t)OSD_DEMO_CELL_W);
        const int16_t cell1 = (int16_t)(cell0 + (int16_t)OSD_DEMO_CELL_W);

        // Fast path: no face edge strictly inside this cell → the cell is one solid
        // color (same result as the per-pixel path). Only the ≤4 edge cells per band
        // pay for the 12px buffer + run encoder, which keeps the build well under the
        // band period even with audio/USB IRQs stealing time.
        bool hasEdge = false;
        uint8_t solid = OSD_DEMO_TWISTER_C_B;
        for (uint8_t f = 0; f < nFace; f++) {
            if ((fL[f] > cell0 && fL[f] < cell1) || (fR[f] > cell0 && fR[f] < cell1)) {
                hasEdge = true;
                break;
            }
            if (fL[f] <= cell0 && fR[f] >= cell1) {
                solid = fC[f]; // later face wins, as in the paint loop below
            }
        }
        if (!hasEdge) {
            uint8_t inv = 0;
            osdDemoTwisterSolidGlyph(solid, &glyphs[x], &inv);
            continue;
        }

        uint8_t pix[OSD_DEMO_CELL_W];
        for (uint8_t p = 0; p < OSD_DEMO_CELL_W; p++) {
            pix[p] = OSD_DEMO_TWISTER_C_B;
        }
        for (uint8_t f = 0; f < nFace; f++) {
            int16_t a = (int16_t)(fL[f] - cell0);
            int16_t b = (int16_t)(fR[f] - cell0);
            if (a < 0) {
                a = 0;
            }
            if (b > (int16_t)OSD_DEMO_CELL_W) {
                b = (int16_t)OSD_DEMO_CELL_W;
            }
            for (int16_t p = a; p < b; p++) {
                pix[p] = fC[f];
            }
        }
        uint8_t inv = 0;
        osdDemoTwisterEncodeCell(pix, &glyphs[x], &inv);
    }
}

static void osdDemoPaintTwister(void)
{
    if (!demoDisplay) {
        return;
    }
    max7456FillScreen(OSD_DEMO_TWISTER_GLYPH_B);
}

// uint32 mul — (uint16)y*256 overflows for y≥256 and warps the lower third of the twist.
static uint8_t osdDemoTwisterTwistPhase(uint8_t rot, uint16_t sampleY, uint16_t span)
{
    if (span == 0) {
        return rot;
    }
    return (uint8_t)(rot + (uint8_t)(((uint32_t)sampleY * (uint32_t)OSD_DEMO_TWISTER_TWIST_NUM) / (uint32_t)span));
}

// Fixed wavelengths → the phase velocity is the same on every line. The old single sine
// with a time-varying integer period moved ∝ y (up to 4–5 px per field at the bottom vs
// 2 px at the top); consecutive interlaced fields with different geometry then read as
// square notches that grow toward the bottom of the screen.
static int8_t osdDemoTwisterBendAt(uint8_t rotA, uint16_t sampleY, uint8_t rotB)
{
    const uint8_t phA = (uint8_t)(rotA
        + (uint8_t)(((uint32_t)sampleY * 256u) / OSD_DEMO_TWISTER_BEND_WAVE_A));
    const uint8_t phB = (uint8_t)(rotB
        - (uint8_t)(((uint32_t)sampleY * 256u) / OSD_DEMO_TWISTER_BEND_WAVE_B));
    return (int8_t)(((int16_t)osdDemoSineAt(phA) * (int16_t)OSD_DEMO_TWISTER_BEND_AMP_A
                     + (int16_t)osdDemoSineAt(phB) * (int16_t)OSD_DEMO_TWISTER_BEND_AMP_B) / 128);
}

// VSYNC→VSYNC is exactly 312.5 PAL lines. Track the real line period in DWT ticks (Q16)
// instead of trusting a 64 us integer: any MCU-vs-video clock error accumulates linearly
// down the field and pushes the bottom rows' bursts into lit lines. Heavy IIR because
// each edge carries STAT-poll jitter (a few us).
static void osdDemoMgTrackLinePeriod(uint32_t edgeTicks, uint32_t nominalLineTicks)
{
    const uint32_t nominalQ16 = nominalLineTicks << 16;
    if (mgLineQ16 == 0) {
        mgLineQ16 = nominalQ16;
    }
    if (mgLastEdgeTicks != 0) {
        const uint32_t fieldTicks = edgeTicks - mgLastEdgeTicks;
        const uint32_t nominalField = (nominalLineTicks * (uint32_t)OSD_DEMO_PAL_FIELD_LINES_X2) / 2u;
        // ±5%: still rejects missed VSYNCs (2×) and glitches, but accepts real off-nominal
        // sources — on HAKRCF411D + this camera VSYNC→VSYNC is ~20.43 ms in DWT ticks
        // (line ≈ 65.4 "us"): a ±0.5% gate rejected every field and left the model at
        // 64 us, ~24 us of drift per char row = the growing corners in the lower half.
        const uint32_t tol = nominalField / 20u;
        mgFieldLastTicks = fieldTicks;
        if (!(fieldTicks > nominalField - tol && fieldTicks < nominalField + tol)) {
            mgFieldReject++;
        }
        if (fieldTicks > nominalField - tol && fieldTicks < nominalField + tol) {
            const uint32_t measQ16 = (uint32_t)(((uint64_t)fieldTicks << 17)
                                                / (uint64_t)mgFieldHalfLines);
            if (mgLineSeeded) {
                mgLineQ16 = (uint32_t)((int32_t)mgLineQ16
                    + (((int32_t)measQ16 - (int32_t)mgLineQ16) >> OSD_DEMO_MG_LINE_IIR_SHIFT));
            } else {
                mgLineQ16 = measQ16; // jump straight to the first real measurement
                mgLineSeeded = true;
            }
        }
    }
    mgLastEdgeTicks = edgeTicks;
}

// Signed offset of `line` from the pivot row (calibrated at nominal 64 us/line).
static inline int32_t osdDemoMgLineOffset(uint16_t line, uint32_t lineQ16)
{
    const int32_t dl = (int32_t)line - (int32_t)OSD_DEMO_MG_PIVOT_LINE;
    return (int32_t)(((int64_t)dl * (int64_t)lineQ16) >> 16);
}

// Diff a built band against what Display SRAM already holds for that row and write only
// the changed cells. Per the datasheet (p.40) every SPI display-memory write can collide
// with the chip's own fetch and break up a character for the field — fewer writes, fewer
// collisions, and a much shorter burst (typically 3–6 cells instead of the whole window).
// checkDeadline: skip (leave SRAM as-is) when the burst cannot finish before finishBy.
// Plan one row burst: cluster changed cells (clean gaps ≤ MERGE_GAP), then per cluster pick
// the cheaper framing — one auto-increment run (DMM, DMAL, 2/cell, END = 6 + 2·span) or
// addressed single writes (DMAL+DMDI = 4/changed cell, + DMM once after an AI run). Greedy
// is within 0.5% of the DP optimum on the plasma; −18% bytes vs "AI runs only", and no
// unchanged cell is ever rewritten by a single-write cluster. Returns the segment count.
static uint8_t osdDemoMgPlanRow(uint8_t row, const uint8_t *glyphs, uint8_t winL, uint8_t winR,
                                max7456SramSeg_t *seg)
{
    const uint8_t *sram = mgSram[row];
    uint8_t nSeg = 0;
    bool afterAi = true; // DMM state unknown at burst start
    uint8_t x = winL;
    while (x < winR) {
        if (glyphs[x] == sram[x]) {
            x++;
            continue;
        }
        const uint8_t first = x;
        uint8_t last = x;
        uint8_t changed = 1;
        x++;
        while (x < winR) {
            if (glyphs[x] != sram[x]) {
                last = x;
                changed++;
                x++;
                continue;
            }
            uint8_t z = x;
            while (z < winR && glyphs[z] == sram[z]) {
                z++;
            }
            if (z < winR && (uint8_t)(z - x) <= OSD_DEMO_MG_RUN_MERGE_GAP) {
                x = z;
            } else {
                break;
            }
        }
        const uint8_t span = (uint8_t)(last - first + 1u);
        const uint16_t aiCost = (uint16_t)(6u + 2u * span);
        const uint16_t singleCost = (uint16_t)((afterAi ? 2u : 0u) + 4u * changed);
        if (span > 1u && aiCost < singleCost) {
            seg[nSeg].col = first;
            seg[nSeg].len = span;
            seg[nSeg].autoInc = true;
            nSeg++;
            afterAi = true;
        } else {
            for (uint8_t c = first; c <= last; c++) {
                if (glyphs[c] == sram[c]) {
                    continue;
                }
                // Merge adjacent singles into one segment (still one DMAL per cell).
                if (nSeg > 0 && !seg[nSeg - 1u].autoInc
                    && (uint8_t)(seg[nSeg - 1u].col + seg[nSeg - 1u].len) == c) {
                    seg[nSeg - 1u].len++;
                } else {
                    seg[nSeg].col = c;
                    seg[nSeg].len = 1;
                    seg[nSeg].autoInc = false;
                    nSeg++;
                }
            }
            afterAi = false;
        }
        x = (uint8_t)(last + 1u);
    }
    return nSeg;
}

static void osdDemoMgCommit(uint8_t row, const uint8_t *glyphs, const max7456SramSeg_t *seg,
                            uint8_t nSeg, uint16_t bytes, uint32_t spent)
{
    for (uint8_t i = 0; i < nSeg; i++) {
        memcpy(&mgSram[row][seg[i].col], &glyphs[seg[i].col], seg[i].len);
    }
    // Learn the real SPI cost per byte (clock divider, polled-loop gaps, CS overhead).
    // A burst stretched by an IRQ (the 50 Hz PT3 tick runs inside the audio IRQ) is not an
    // SPI measurement: one such sample used to inflate the estimate, mis-plan the next 4–5
    // bands (torn 1-px lines) and, when large enough, overflowed the start-time maths into a
    // ~20 s wait (the scene "hang"). Accept faster/near samples; take a slow one only if it
    // keeps repeating.
    if (spent > mgBurstFixedTicks) {
        const uint32_t estTicks = mgBurstFixedTicks + (((uint32_t)bytes * mgByteTicksQ8) >> 8);
        const bool slow = spent > estTicks + estTicks / 2u;
        if (!slow || ++mgSlowRun >= 16u) {
            const uint32_t perByteQ8 = ((spent - mgBurstFixedTicks) << 8) / bytes;
            mgByteTicksQ8 = (uint32_t)((int32_t)mgByteTicksQ8
                + (((int32_t)perByteQ8 - (int32_t)mgByteTicksQ8) >> 3));
            mgSlowRun = 0;
        } else {
            mgStatSlowBursts++;
        }
    }
    if (bytes > mgStatMaxBytes) {
        mgStatMaxBytes = bytes;
    }
    mgStatBytes += bytes;
    mgStatWrites++;
}

// Diff a built band against what Display SRAM already holds for that row and write only
// the changed cells. Per the datasheet (p.40) every SPI display-memory write can collide
// with the chip's own fetch and break up a character for the field — fewer writes, fewer
// collisions, and a much shorter burst (typically 3–6 cells instead of the whole window).
// checkDeadline: skip (leave SRAM as-is) when the burst cannot finish before finishBy.
static bool osdDemoMgWriteRow(uint8_t row, const uint8_t *glyphs, uint8_t winL, uint8_t winR,
                              uint32_t finishBy, bool checkDeadline, bool maskIrq)
{
    max7456SramSeg_t seg[OSD_DEMO_CHARS_PER_LINE];
    uint16_t segLast[OSD_DEMO_CHARS_PER_LINE];
    const uint8_t nSeg = osdDemoMgPlanRow(row, glyphs, winL, winR, seg);
    if (nSeg == 0) {
        return true;
    }
    const uint16_t addr = (uint16_t)((uint16_t)row * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
    const uint16_t bytes = max7456EncodeDisplaySramRow(addr, glyphs, seg, nSeg, segLast);
    if (bytes == 0) {
        return false;
    }
    const uint32_t estTicks = mgBurstFixedTicks + ((uint32_t)bytes * mgByteTicksQ8 >> 8);

    // Deadline check and burst are atomic: the 16 kHz AY IRQ (same top priority as gyro
    // EXTI) or its 50 Hz tracker tick must not land between "fits" and "done" and push
    // the tail of the burst into the lit line. PRIMASK — BASEPRI cannot mask prio 0.
#if defined(__CORTEX_M) // CMSIS core present
    const uint32_t primask = __get_PRIMASK();
    if (maskIrq) {
        __disable_irq();
    }
#else
    UNUSED(maskIrq);
#endif
    const uint32_t t0 = getCycleCounter();
    bool ok = false;
    if (!checkDeadline || (int32_t)(finishBy - t0) >= (int32_t)estTicks) {
        ok = max7456SendEncodedDisplaySram(bytes);
    }
    const uint32_t t1 = getCycleCounter();
#if defined(__CORTEX_M) // CMSIS core present
    __set_PRIMASK(primask);
#endif

    if (!ok) {
        return false;
    }
    osdDemoMgCommit(row, glyphs, seg, nSeg, bytes, t1 - t0);
    return true;
}

// Race the beam: start right behind the beam in line (due−1) and require every cell to land
// before the beam reaches it in line `due`. Per cell that is a whole line of window no matter
// how long the burst is — a full 30-cell row fits, while "finish the burst inside the previous
// line" (≈57 us) never can. Segment landing times come from the exact encoded byte offsets.
// Every cell must land after the beam left it in line due−1 and before the beam reaches it in
// line `due`; within one segment the writer moves at a constant rate (AI ~2 B/cell can be
// faster than the beam, singles ~4 B/cell slower), so its two end cells bound it. Result: a
// start window [startAt, latest] for the burst.

// Plan + encode one band (into the driver's burst buffer) and compute its start window.
// Returns false when nothing changed (no burst).
static bool osdDemoMgChasePrepare(uint8_t row, const uint8_t *glyphs, uint8_t cols,
                                  uint32_t hsyncPrev, uint32_t lineTicks, osdDemoMgBurst_t *bu)
{
    uint16_t segLast[OSD_DEMO_CHARS_PER_LINE];
    bu->row = row;
    bu->glyphs = glyphs;
    bu->nSeg = osdDemoMgPlanRow(row, glyphs, 0, cols, bu->seg);
    if (bu->nSeg == 0) {
        return false;
    }
    const uint16_t addr = (uint16_t)((uint16_t)row * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
    bu->bytes = max7456EncodeDisplaySramRow(addr, glyphs, bu->seg, bu->nSeg, segLast);

    const uint32_t cpu = clockMicrosToCycles(1);
    const uint32_t cellQ8 = (cpu * (uint32_t)OSD_DEMO_MG_BEAM_CELL_NS * 256u) / 1000u;
    const int32_t x0 = (int32_t)mgBeamX0Us * (int32_t)cpu;
    const uint32_t margin = clockMicrosToCycles(OSD_DEMO_MG_CHASE_MARGIN_US);
    const uint32_t byteT = mgByteTicksQ8; // Q8
    const uint32_t lineBeam = hsyncPrev + (uint32_t)x0; // first OSD pixel, line due−1

    bool set = false;
    for (uint8_t i = 0; i < bu->nSeg; i++) {
        const uint8_t step = bu->seg[i].autoInc ? 2u : 4u;
        const uint16_t lastOff = segLast[i];
        const uint16_t firstOff = (uint16_t)(lastOff - (uint16_t)step * (bu->seg[i].len - 1u));
        const uint8_t xs[2] = { bu->seg[i].col, (uint8_t)(bu->seg[i].col + bu->seg[i].len - 1u) };
        const uint16_t offs[2] = { firstOff, lastOff };
        for (uint8_t e = 0; e < 2u; e++) {
            const uint32_t land = mgBurstFixedTicks + (((uint32_t)offs[e] * byteT) >> 8);
            const uint32_t pass = lineBeam + (((uint32_t)(xs[e] + 1u) * cellQ8) >> 8) + margin;
            const uint32_t reach = lineBeam + lineTicks + (((uint32_t)xs[e] * cellQ8) >> 8) - margin;
            const uint32_t need = pass - land;    // earliest start for this cell
            const uint32_t limit = reach - land;  // latest start for this cell
            if (!set || (int32_t)(need - bu->startAt) > 0) {
                bu->startAt = need;
            }
            if (!set || (int32_t)(limit - bu->latest) < 0) {
                bu->latest = limit;
            }
            set = true;
        }
    }
    return bu->bytes != 0u;
}

// Send a prepared burst if it can still start inside its window (call at/after startAt).
// Check + burst are atomic: an IRQ inside the burst would push its tail past the beam.
static bool osdDemoMgBurstSend(const osdDemoMgBurst_t *bu)
{
#if defined(__CORTEX_M) // CMSIS core present
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
#endif
    const uint32_t t0 = getCycleCounter();
    const bool ok = (int32_t)(bu->latest - t0) >= 0 && max7456SendEncodedDisplaySram(bu->bytes);
    const uint32_t t1 = getCycleCounter();
#if defined(__CORTEX_M) // CMSIS core present
    __set_PRIMASK(primask);
#endif
    if (ok) {
        osdDemoMgCommit(bu->row, bu->glyphs, bu->seg, bu->nSeg, bu->bytes, t1 - t0);
    }
    return ok;
}

// Blocking engines (scenes 7, 9): prepare, wait for the beam, send.
static bool osdDemoMgWriteRowChase(uint8_t row, const uint8_t *glyphs, uint8_t cols,
                                   uint32_t hsyncPrev, uint32_t lineTicks)
{
    osdDemoMgBurst_t bu;
    if (!osdDemoMgChasePrepare(row, glyphs, cols, hsyncPrev, lineTicks, &bu)) {
        return true;
    }
    if ((int32_t)(bu.latest - bu.startAt) < 0) {
        return false;
    }
    osdDemoWaitCycles(bu.startAt);
    return osdDemoMgBurstSend(&bu);
}

// Once per field, in VBLANK (13 lines before the first OSD line, after the equalizing
// pulses): the absolute HSYNC phase of THIS field and which field it is. PAL field 2's VSYNC
// starts mid-line (datasheet Fig. 6/7), so HSYNC vs VSYNC differs by ½ line (~32 us) between
// fields. The old cross-field IIR averaged the two and left race-the-beam ~±16 us off in
// every field — at the edge of its tolerance: in one field cells landed a line early, and the
// monitor weaving both fields showed a ±1-line comb ("grid"). Parity also lets animations
// advance once per frame on the right field.
static void osdDemoMgMeasureFieldPhase(uint32_t vsyncEdge, uint32_t row0, uint32_t lineTicks)
{
    const uint32_t model = row0 - (uint32_t)OSD_DEMO_MG_FIELD_PHASE_LINES * lineTicks;
    osdDemoWaitCycles(model - lineTicks * 3u / 8u);

    uint32_t edge = 0;
    max7456MidGlyphSpiBoost(false); // STAT polling is unreliable at the boosted clock
    const bool ok = max7456WaitHsyncFallingEdge(&edge, 150); // ≥2 chances at a short pulse
    max7456MidGlyphSpiBoost(true);
    if (!ok) {
        mgHsyncMiss++;
        mgFieldFirst = !mgFieldFirst; // fields alternate
        if (mgHsyncPhaseByValid[mgFieldFirst]) {
            mgHsyncPhase = mgHsyncPhaseBy[mgFieldFirst];
        }
        return;
    }
    int32_t ph = (int32_t)(edge - model);
    const int32_t half = (int32_t)lineTicks / 2;
    while (ph > half) {
        ph -= (int32_t)lineTicks;
    }
    while (ph < -half) {
        ph += (int32_t)lineTicks;
    }
    const uint32_t pv = (edge - vsyncEdge) % lineTicks;
    mgFieldFirst = (pv < lineTicks / 4u) || (pv > lineTicks - lineTicks / 4u);
    mgHsyncPhase = ph;
    mgHsyncPhaseBy[mgFieldFirst] = ph;
    mgHsyncPhaseByValid[mgFieldFirst] = true;
}

// Absolute DWT time of the HSYNC edge that starts `line` (model + HSYNC phase + row lock).
static uint32_t osdDemoMgHsyncAt(uint16_t line, uint32_t pivot, uint32_t lineQ16)
{
    uint8_t row = (uint8_t)(line / (uint16_t)OSD_DEMO_CELL_H);
    if (row >= OSD_DEMO_MG_ROWS_MAX) {
        row = OSD_DEMO_MG_ROWS_MAX - 1u;
    }
    const int32_t corr = mgHsyncLock ? mgRowCorr[row] : 0;
    return pivot + (uint32_t)(osdDemoMgLineOffset(line, lineQ16) + mgHsyncPhase + corr);
}

// Catch the real HSYNC that starts line (18·row − 1) during the 3 write-free lines before
// a preloaded row (band 0 came from VBLANK) and learn this row's phase vs row 1.
static void osdDemoMgHsyncMeasure(uint8_t row, uint32_t pivot, uint32_t lineQ16, int32_t *ref,
                                       bool *haveRef)
{
    if (row == 0 || row >= OSD_DEMO_MG_ROWS_MAX) {
        return;
    }
    const uint16_t line = (uint16_t)((uint16_t)row * (uint16_t)OSD_DEMO_CELL_H - 1u);
    const uint32_t model = pivot + (uint32_t)osdDemoMgLineOffset(line, lineQ16);
    const int32_t lineTicks = (int32_t)(lineQ16 >> 16);
    const int32_t corrPrev = mgRowCorr[row - 1u];
    osdDemoWaitCycles(model + (uint32_t)corrPrev - (uint32_t)(lineTicks * 3 / 8));

    uint32_t edge = 0;
    max7456MidGlyphSpiBoost(false); // STAT polling is unreliable at the boosted clock
    const bool ok = max7456WaitHsyncFallingEdge(&edge, 90);
    max7456MidGlyphSpiBoost(true);
    if (!ok) {
        mgHsyncMiss++;
        return;
    }
    const int32_t raw = (int32_t)(edge - model);
    if (row == 1u) {
        *ref = raw;
        *haveRef = true;
        return;
    }
    if (!*haveRef) {
        return;
    }
    int32_t d = raw - *ref;
    while (d > lineTicks / 2) {
        d -= lineTicks;
    }
    while (d < -lineTicks / 2) {
        d += lineTicks;
    }
    // Reject IRQ-delayed polls once the row has converged.
    const int32_t tol = (int32_t)clockMicrosToCycles(12);
    if (mgRowCorrN[row] >= 8u && (d - mgRowCorr[row] > tol || mgRowCorr[row] - d > tol)) {
        return;
    }
    if (mgRowCorrN[row] < 8u) {
        mgRowCorrN[row]++;
        mgRowCorr[row] += (d - mgRowCorr[row]) / (int32_t)mgRowCorrN[row];
    } else {
        mgRowCorr[row] += (d - mgRowCorr[row]) / 4;
    }
}

static void osdDemoTwisterBuildBand(uint8_t *glyphs, uint8_t cols, uint16_t line, uint8_t rot,
                                    uint8_t bendRot, uint8_t bendFloat, uint16_t span,
                                    int16_t centerX, uint8_t winL, uint8_t winR)
{
    const uint16_t sampleY = (uint16_t)(line + (OSD_DEMO_TWISTER_BAND_PX / 2u));
    const int16_t cx = (int16_t)(centerX + osdDemoTwisterBendAt(bendRot, sampleY, bendFloat));
    osdDemoTwisterBuildScanline(glyphs, cols, osdDemoTwisterTwistPhase(rot, sampleY, span), cx, winL, winR);
}

static void osdDemoTwisterEnginePoll(void)
{
    if (!twisterArmed || !demoDisplay) {
        return;
    }

    uint8_t cols = demoDisplay->cols;
    uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > OSD_DEMO_CHARS_PER_LINE) {
        cols = OSD_DEMO_CHARS_PER_LINE;
    }
    if (rows > OSD_DEMO_MG_ROWS_MAX) {
        rows = OSD_DEMO_MG_ROWS_MAX;
    }

    const int16_t centerX = (int16_t)(((uint16_t)cols * (uint16_t)OSD_DEMO_CELL_W) / 2u);

    // Fixed build window covers silhouette + geometric bend; the SPI burst is only the
    // cells inside it that actually changed (see osdDemoMgWriteRow).
    int16_t winPx0 = (int16_t)(centerX - (int16_t)OSD_DEMO_TWISTER_AMP_PX
                               - (int16_t)OSD_DEMO_TWISTER_WIN_MARGIN_PX);
    int16_t winPx1 = (int16_t)(centerX + (int16_t)OSD_DEMO_TWISTER_AMP_PX
                               + (int16_t)OSD_DEMO_TWISTER_WIN_MARGIN_PX);
    if (winPx0 < 0) {
        winPx0 = 0;
    }
    const int16_t screenW = (int16_t)((uint16_t)cols * (uint16_t)OSD_DEMO_CELL_W);
    if (winPx1 > screenW) {
        winPx1 = screenW;
    }
    uint8_t winL = (uint8_t)(winPx0 / (int16_t)OSD_DEMO_CELL_W);
    uint8_t winR = (uint8_t)((winPx1 + (int16_t)OSD_DEMO_CELL_W - 1) / (int16_t)OSD_DEMO_CELL_W);
    if (winR > cols) {
        winR = cols;
    }
    if (winL >= winR) {
        winL = 0;
        winR = cols;
    }

    const uint32_t burstTicks = clockMicrosToCycles(OSD_DEMO_TWISTER_BURST_US);
    const uint32_t padTicks = clockMicrosToCycles(OSD_DEMO_TWISTER_PAD_US);
    const uint32_t wrLeadTicks = clockMicrosToCycles(OSD_DEMO_TWISTER_WR_LEAD_US);
    const uint32_t nominalLineTicks = clockMicrosToCycles(OSD_DEMO_PAL_LINE_US);
    const uint32_t pivotTicks = clockMicrosToCycles(OSD_DEMO_PAL_VBLANK_US
        + OSD_DEMO_MG_PIVOT_LINE * OSD_DEMO_PAL_LINE_US);
    if (mgBurstFixedTicks == 0) {
        mgBurstFixedTicks = clockMicrosToCycles(OSD_DEMO_MG_BURST_FIXED_US);
    }
    if (mgByteTicksQ8 == 0) {
        // Seed: the old fixed 30 us budget covered a full 14-cell 16-bit burst (36 bytes).
        mgByteTicksQ8 = (burstTicks << 8) / 36u;
    }
    uint16_t span = OSD_DEMO_TWISTER_LINE_SPAN;
    {
        const uint16_t byRows = (uint16_t)rows * (uint16_t)OSD_DEMO_CELL_H;
        if (byRows < span) {
            span = byRows;
        }
    }

    max7456MidGlyphSpiBegin();
    max7456WriteHosSigned(0); // neutral — bend is geometric only

    uint8_t vsyncFails = 0;
    while (active && twisterArmed && !ARMING_FLAG(ARMED)) {
        if (!fxHold && (millis() - fxStartMs) >= OSD_DEMO_FX_TWISTER_MS) {
            twisterArmed = false;
            break;
        }
        uint32_t edgeTicks = 0;
        max7456MidGlyphSpiBoost(false);
        if (!max7456WaitVsyncFallingEdge(&edgeTicks, OSD_DEMO_PAL_VSYNC_TIMEOUT_US)) {
            mgLastEdgeTicks = 0;
            if (++vsyncFails >= 8) {
                twisterArmed = false;
                break;
            }
#ifdef USE_CLI
            if (cliMode) {
                (void)cliProcess();
            }
#endif
            continue;
        }
        vsyncFails = 0;
        mgStatFields++;

        osdDemoMgTrackLinePeriod(edgeTicks, nominalLineTicks);
        const uint32_t lineQ16 = mgLineQ16;
        const uint32_t lineTicks = lineQ16 >> 16;

        const uint32_t pivot = edgeTicks + pivotTicks
            + (uint32_t)(int32_t)((int32_t)mgShiftUs * (int32_t)clockMicrosToCycles(1));
        // First lit line from the same measured model as every band (not VSYNC + 1504 us:
        // with a 65.4 us line that is ~200 us = 3 lines too late, and the VBLANK preload
        // then ran into row 0 and made its band 1 miss the beam).
        const uint32_t row0 = pivot + (uint32_t)osdDemoMgLineOffset(0, lineQ16);
        osdDemoMgMeasureFieldPhase(edgeTicks, row0, lineTicks);

        const uint8_t rot = twisterPhase;
        const uint8_t bendRot = twisterBendPhase;
        const uint8_t bendFloat = twisterBendFloat;
        if (!twisterFreeze) {
            const bool perField = (twisterPairParity == OSD_DEMO_TWISTER_PAIR_OFF);
            // New frame starts on field 1 (twpair 0) or field 2 (twpair 1, if the guess is off).
            if (perField || mgFieldFirst == (twisterPairParity == 0u)) {
                const uint8_t k = perField ? 1u : 2u;
                twisterPhase = (uint8_t)(twisterPhase + k * OSD_DEMO_TWISTER_ROT_STEP);
                twisterBendPhase = (uint8_t)(twisterBendPhase + k * OSD_DEMO_TWISTER_BEND_ROT_STEP);
                twisterBendFloat = (uint8_t)(twisterBendFloat + k * OSD_DEMO_TWISTER_BEND_FLOAT_STEP);
            }
        }
        max7456MidGlyphSpiBoost(true);

        // VBLANK preload: band-0 of as many character rows as fit (plasma top-shear fix).
        // Only band 0 — later bands must rewrite mid-glyph or the whole cell sticks on the
        // last preloaded geometry.
        const uint32_t blankDeadline = row0 - burstTicks;
        uint8_t rowsPre = 0;
        for (; rowsPre < rows; rowsPre++) {
            if ((int32_t)(getCycleCounter() - blankDeadline) > 0) {
                break;
            }
            const uint16_t line0 = (uint16_t)((uint16_t)rowsPre * (uint16_t)OSD_DEMO_CELL_H);
            if (line0 >= span) {
                break;
            }
            osdDemoTwisterBuildBand(twisterGlyphBuf[0], cols, line0, rot, bendRot, bendFloat,
                                    span, centerX, winL, winR);
            (void)osdDemoMgWriteRow(rowsPre, twisterGlyphBuf[0], winL, winR, 0, false, true);
        }

        int32_t hsyncRef = 0;
        bool hsyncHaveRef = false;

        uint8_t cur = 0;
        bool have = false;
        // First mid-field band: row0 band1 if preloaded, else row0 band0.
        uint16_t line = 0;
        if (rowsPre > 0) {
            line = OSD_DEMO_TWISTER_BAND_PX; // skip preloaded band0 of row 0
        }
        if (line < span) {
            osdDemoTwisterBuildBand(twisterGlyphBuf[cur], cols, line, rot, bendRot, bendFloat,
                                    span, centerX, winL, winR);
            have = true;
        }

        for (; line < span; line = (uint16_t)(line + OSD_DEMO_TWISTER_BAND_PX)) {
            const uint8_t row = (uint8_t)(line / (uint16_t)OSD_DEMO_CELL_H);
            const uint8_t bandInRow = (uint8_t)((line % (uint16_t)OSD_DEMO_CELL_H)
                                                / (uint16_t)OSD_DEMO_TWISTER_BAND_PX);
            // Band 0 already written in VBLANK for the first rowsPre rows.
            if (row < rowsPre && bandInRow == 0u) {
                if (mgHsyncLock) {
                    osdDemoMgHsyncMeasure(row, pivot, lineQ16, &hsyncRef, &hsyncHaveRef);
                }
                continue;
            }

            const int32_t rowCorr = mgHsyncLock ? mgRowCorr[row] : 0;
            const uint32_t due = pivot + (uint32_t)(osdDemoMgLineOffset(line, lineQ16) + rowCorr);
            uint32_t wrAt = due - lineTicks + wrLeadTicks;
            const uint32_t finishBy = due - padTicks;
            if ((int32_t)(wrAt - edgeTicks) < 0) {
                wrAt = edgeTicks;
            }

            const uint16_t nextLine = (uint16_t)(line + OSD_DEMO_TWISTER_BAND_PX);
            uint16_t prefetchLine = nextLine;
            while (prefetchLine < span) {
                const uint8_t nr = (uint8_t)(prefetchLine / (uint16_t)OSD_DEMO_CELL_H);
                const uint8_t nb = (uint8_t)((prefetchLine % (uint16_t)OSD_DEMO_CELL_H)
                                             / (uint16_t)OSD_DEMO_TWISTER_BAND_PX);
                if (!(nr < rowsPre && nb == 0u)) {
                    break;
                }
                prefetchLine = (uint16_t)(prefetchLine + OSD_DEMO_TWISTER_BAND_PX);
            }

            bool builtNext = false;
            if ((int32_t)(getCycleCounter() - wrAt) < 0) {
                if (prefetchLine < span) {
                    osdDemoTwisterBuildBand(twisterGlyphBuf[cur ^ 1u], cols, prefetchLine, rot,
                                            bendRot, bendFloat, span, centerX, winL, winR);
                    builtNext = true;
                }
                osdDemoWaitCycles(wrAt);
            }

            if (!have) {
                if (prefetchLine < span && !builtNext) {
                    osdDemoTwisterBuildBand(twisterGlyphBuf[cur ^ 1u], cols, prefetchLine, rot,
                                            bendRot, bendFloat, span, centerX, winL, winR);
                    builtNext = true;
                }
                if (builtNext) {
                    cur ^= 1u;
                    have = true;
                }
                continue;
            }

            if (!osdDemoMgWriteRow(row, twisterGlyphBuf[cur], winL, winR, finishBy, true, true)) {
                mgStatSkips++;
                if (mgStatSkipRow[row] < UINT16_MAX) {
                    mgStatSkipRow[row]++;
                }
            }

            if (prefetchLine < span) {
                if (!builtNext) {
                    osdDemoTwisterBuildBand(twisterGlyphBuf[cur ^ 1u], cols, prefetchLine, rot,
                                            bendRot, bendFloat, span, centerX, winL, winR);
                }
                cur ^= 1u;
                have = true;
            } else {
                have = false;
            }
        }

#ifdef USE_CLI
        if (cliMode) {
            (void)cliProcess();
        }
#endif
        max7456MidGlyphSpiBoost(false);
    }

    max7456WriteHosSigned(0);
    max7456ResetHudMotionOffset();
    max7456ApplyHudMotionNow();
    max7456MidGlyphSpiEnd();
}

void osdDemoTwisterSetFieldHalfLines(uint16_t halfLines)
{
    if (halfLines >= 620u && halfLines <= 630u && halfLines != mgFieldHalfLines) {
        mgFieldHalfLines = halfLines;
        mgLineQ16 = 0; // re-seed from the next real measurement
        mgLineSeeded = false;
    }
}

uint16_t osdDemoTwisterGetFieldHalfLines(void)
{
    return mgFieldHalfLines;
}

void osdDemoTwisterSetHsyncLock(bool enable)
{
    mgHsyncLock = enable;
    memset(mgRowCorr, 0, sizeof(mgRowCorr));
    memset(mgRowCorrN, 0, sizeof(mgRowCorrN));
}

bool osdDemoTwisterGetHsyncLock(void)
{
    return mgHsyncLock;
}

void osdDemoTwisterSetPair(uint8_t mode)
{
    twisterPairParity = (mode > OSD_DEMO_TWISTER_PAIR_OFF) ? OSD_DEMO_TWISTER_PAIR_OFF : mode;
}

uint8_t osdDemoTwisterGetPair(void)
{
    return twisterPairParity;
}

void osdDemoTwisterSetFreeze(bool freeze)
{
    twisterFreeze = freeze;
}

bool osdDemoTwisterGetFreeze(void)
{
    return twisterFreeze;
}

void osdDemoTwisterGetPhases(uint8_t *rot, uint8_t *bendA, uint8_t *bendB)
{
    *rot = twisterPhase;
    *bendA = twisterBendPhase;
    *bendB = twisterBendFloat;
}

void osdDemoMgSetBeamX0Us(int16_t us)
{
    mgBeamX0Us = constrain(us, -20, 40);
}

int16_t osdDemoMgGetBeamX0Us(void)
{
    return mgBeamX0Us;
}

void osdDemoTwisterSetShiftUs(int16_t us)
{
    mgShiftUs = constrain(us, -200, 200);
}

int16_t osdDemoTwisterGetShiftUs(void)
{
    return mgShiftUs;
}

static void osdDemoMgResetStats(void)
{
    osdDemoTwisterStats_t st;
    osdDemoTwisterGetStats(&st, true);
}

void osdDemoTwisterGetStats(osdDemoTwisterStats_t *st, bool reset)
{
    if (!st) {
        return;
    }
    st->fields = mgStatFields;
    st->writes = mgStatWrites;
    st->skips = mgStatSkips;
    st->maxBytes = mgStatMaxBytes;
    st->lineQ16 = mgLineQ16;
    st->byteTicksQ8 = mgByteTicksQ8;
    st->cyclesPerUs = clockMicrosToCycles(1);
    st->fieldTicks = mgFieldLastTicks;
    st->idleTicks = mgStatIdleTicks;
    st->windowMs = (uint32_t)(millis() - mgStatSinceMs);
    st->bytes = mgStatBytes;
    st->phaseField1 = mgHsyncPhaseBy[1];
    st->phaseField2 = mgHsyncPhaseBy[0];
    st->slowBursts = mgStatSlowBursts;

    st->fieldReject = mgFieldReject;
    st->hsyncMiss = mgHsyncMiss;
    st->hsyncLock = mgHsyncLock;
    for (uint8_t r = 0; r < OSD_DEMO_MG_ROWS_MAX; r++) {
        st->rowCorrTicks[r] = mgRowCorr[r];
    }
    memcpy(st->skipRow, mgStatSkipRow, sizeof(st->skipRow));
    if (reset) {
        mgStatFields = 0;
        mgStatWrites = 0;
        mgStatSkips = 0;
        mgStatMaxBytes = 0;
        memset(mgStatSkipRow, 0, sizeof(mgStatSkipRow));
        mgFieldReject = 0;
        mgStatIdleTicks = 0;
        mgStatSinceMs = millis();
        mgStatBytes = 0;
        mgStatSlowBursts = 0;

        mgHsyncMiss = 0;
    }
}

static void osdDemoEnterFx(osdDemoFx_e next) __attribute__((noinline));

static void osdDemoEnterFx(osdDemoFx_e next)
{
    // Drop any mid-glyph SPI boost before scene setup (NVM writes / RefreshAll).
    max7456MidGlyphSpiEnd();
    fx = next;
    fxStartMs = millis();
    punchY = 0;
    punchVel = 0;
    punchArmed = false;
    plasma2x2Armed = false;
    twisterArmed = false;
    shoutArmed = false;
    osdDemoPlasmaIrqStop();
    memset(prevRowBright, 0xFF, sizeof(prevRowBright));
    max7456Invalidate();
    if (next == OSD_DEMO_FX_PLASMA) {
        plasmaT = 0;
        max7456Brightness(0, 3);
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        osdDemoPaintPlasma();
    } else if (next == OSD_DEMO_FX_FIRE) {
        firePhase = OSD_DEMO_FIRE_COLLAPSE;
        fireFall = 0;
        fireTick = 0;
        memset(fireTipFine, 0, sizeof(fireTipFine));
        memset(fireTipMask, 0xFF, sizeof(fireTipMask));
        memset(fireSparks, 0, sizeof(fireSparks));
        memset(fireFuelAvg, 6, sizeof(fireFuelAvg));
        memset(fireDrawn, 0xFF, sizeof(fireDrawn)); // force dirty first paint
        memset(fireCurveH, 8, sizeof(fireCurveH));
        fireCurvePhase = 0;
        fireCurveBright = 0;
        fireFlameTop = OSD_DEMO_FIRE_ROWS - 1;
        fireFlameTopRaw = fireFlameTop;
        fireTopHold = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetHudMotionOffset(0, 0);
        if (demoDisplay) {
            osdDemoSnapshotToFire(demoDisplay->cols, demoDisplay->rows);
        } else {
            memset(fireHeat, 0, sizeof(fireHeat));
        }
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        if (demoDisplay) {
            uint8_t cols = demoDisplay->cols;
            uint8_t rows = demoDisplay->rows;
            if (cols > OSD_DEMO_FIRE_COLS) {
                cols = OSD_DEMO_FIRE_COLS;
            }
            if (rows > OSD_DEMO_FIRE_ROWS) {
                rows = OSD_DEMO_FIRE_ROWS;
            }
            osdDemoFireFindTips(cols, rows);
            osdDemoApplyFireRowBright(rows);
            osdDemoFireDrawDirty(cols, rows, true);
        }
    } else if (next == OSD_DEMO_FX_WIPE) {
        wipePhase = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_TRANSPARENT);
        max7456SetHudMotionOffset(0, 0);
        osdDemoPaintWipe();
    } else if (next == OSD_DEMO_FX_TUNNEL) {
        tunnelRot = 0;
        tunnelZoom = 0;
        tunnelAnim = 0;
        tunnelAnimDiv = 0;
        tunnelSpinDiv = 0;
        tunnelTextDiv = 0;
        tunnelScrollMx = 0;
        tunnelMw = 0;
        tunnelMh = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        osdDemoPaintTunnel();
    } else if (next == OSD_DEMO_FX_PLASMA2X2) {
        plasmaPhase = 0;
        plasmaPhaseDiv = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        // Solid PX22 into NVM. EndFontWrite — WriteNvm leaves OSD off.
        (void)osdDemoWritePx22Glyphs();
        max7456EndFontWrite();
        if (demoDisplay) {
            osdDemoFillRowsGlyphFast(demoDisplay->cols, demoDisplay->rows,
                                     OSD_DEMO_PX22_BASE, OSD_DEMO_PX22_BASE);
        } else {
            max7456FillScreen(OSD_DEMO_PX22_BASE);
        }
        // 0xFF never appears in a PX22 row (it is the auto-increment escape), so the first
        // field rewrites every cell and the mirror is exact from then on.
        memset(mgSram, 0xFF, sizeof(mgSram));
        mgLastEdgeTicks = 0;
        osdDemoMgResetStats();
        plasma2x2Armed = true;
    } else if (next == OSD_DEMO_FX_TWISTER) {
        plasma2x2Armed = false;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        // X-fill edge glyphs once; never rewrite NVM during animation.
        osdDemoTwisterInstallXFillGlyphs();
        osdDemoPaintTwister();
        max7456RefreshAll();
        // RefreshAll leaves every twister cell black — seed the SRAM mirror to match.
        memset(mgSram, OSD_DEMO_TWISTER_GLYPH_B, sizeof(mgSram));
        mgLastEdgeTicks = 0;
        osdDemoMgResetStats();
        twisterPhase = 0;
        twisterBendPhase = 0;
        twisterBendFloat = 0;
        max7456WriteHosSigned(0);
        twisterArmed = true;
    } else if (next == OSD_DEMO_FX_SHOUT) {
        osdDemoShoutEnter();
    } else if (next == OSD_DEMO_FX_PLASMA_IRQ) {
        plasmaPhase = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        (void)osdDemoWritePx22Glyphs();
        max7456EndFontWrite();
        if (demoDisplay) {
            osdDemoFillRowsGlyphFast(demoDisplay->cols, demoDisplay->rows,
                                     OSD_DEMO_PX22_BASE, OSD_DEMO_PX22_BASE);
        }
        memset(mgSram, 0xFF, sizeof(mgSram));
        mgLastEdgeTicks = 0;
        osdDemoMgResetStats();
        if (!osdDemoPlasmaIrqStart()) {
            startLastError = "scene10: interrupt engine unavailable on this MCU";
        }
    } else {
        bounceStartMs = millis();
        prevBouncePhase = 0;
        scrollCol = 0;
        max7456Osdm(0x1B);
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        osdDemoPaintScroller();
    }
    // First frame of every scene: same vsync+SPI drain as the steady Update path.
    // Mid-glyph scenes own the chip directly — skip shadow flush.
    if (!osdDemoFxIsMidGlyph(next)) {
        hosWrappedThisStep = false;
        osdDemoSyncFlush(true, true);
    }
}

static void osdDemoPaintPlasma(void)
{
    if (!demoDisplay) {
        return;
    }
    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;
    const uint8_t band = OSD_DEMO_PLASMA_STAR_ROWS;
    // Top stars + ≥1 plasma row + bottom stars.
    if (rows < (uint8_t)(band * 2 + 1)) {
        return;
    }
    const uint8_t plasmaY0 = band;
    const uint8_t plasmaY1 = (uint8_t)(rows - band); // exclusive
    const uint8_t plasmaH = (uint8_t)(plasmaY1 - plasmaY0);
    const uint8_t xDiv = (cols > 1) ? (uint8_t)(cols - 1) : 1;
    const uint8_t yDiv = (plasmaH > 1) ? (uint8_t)(plasmaH - 1) : 1;

    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    // No HOS on plasma — full-width field (right-bias soft-scroll leaves a left gap).
    max7456SetHudMotionOffset(0, 0);

    // Field fade via RB white; plasma outer rows slightly darker (1st −2, 2nd −1).
    const uint8_t white = osdDemoPlasmaFadeWhite();
    const timeMs_t twinkleT = millis() - fxStartMs;
    for (uint8_t y = 0; y < rows && y < OSD_DEMO_RB_ROWS; y++) {
        uint8_t rowWhite = white;
        if (y >= plasmaY0 && y < plasmaY1) {
            const uint8_t local = (uint8_t)(y - plasmaY0);
            uint8_t drop = 0;
            if (local == 0 || local == (uint8_t)(plasmaH - 1)) {
                drop = 2; // outermost plasma rows
            } else if (plasmaH >= 3 && (local == 1 || local == (uint8_t)(plasmaH - 2))) {
                drop = 1; // next-in rows, still below center
            }
            rowWhite = (white > drop) ? (uint8_t)(white - drop) : 0;
        }
        const uint8_t reg = (uint8_t)((0 << 2) | (3 - rowWhite));
        if (prevRowBright[y] != reg) {
            prevRowBright[y] = reg;
            max7456BrightnessRow(y, 0, rowWhite);
        }
    }

    // Top / bottom starfields — same rel phase as scroller (starFine keeps moving).
    for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
        const uint16_t rel = (uint16_t)(starFine[layer] - scrollFine);
        const uint16_t cellShift = rel / OSD_DEMO_CELL_W;
        const uint8_t frame = (uint8_t)(rel % OSD_DEMO_STAR_FRAMES);
        for (uint8_t i = 0; i < OSD_DEMO_STARS_PER_LAYER; i++) {
            const uint8_t y = stars[layer][i].y;
            if (!((y < band) || (y >= (uint8_t)(rows - band)))) {
                continue;
            }
            const uint8_t tw = (uint8_t)((twinkleT / 50) + (layer * 19) + (i * 37) + y * 5);
            if ((tw & 0x0F) < 3) {
                continue;
            }
            const int sx = (int)stars[layer][i].x - (int)cellShift;
            const uint8_t x = (uint8_t)(((sx % cols) + cols) % cols);
            max7456WriteChar(x, y, osdDemoStarGlyph(stars[layer][i].size, frame));
        }
    }

    // Plasma fills the whole centre band (no border stripes).
    // Accumulate mean dither level → drive OSDM sharpness (dark=sharp, bright=soft).
    uint16_t levelSum = 0;
    uint16_t levelCount = 0;
    for (uint8_t y = plasmaY0; y < plasmaY1; y++) {
        const uint8_t local = (uint8_t)(y - plasmaY0);
        const int8_t cy = (int8_t)(((uint16_t)local * 31u) / yDiv);
        for (uint8_t x = 0; x < cols; x++) {
            const int8_t cx = (int8_t)(((uint16_t)x * 59u) / xDiv);
            uint8_t level = osdDemoPlasmaLevel(cx, cy, plasmaT);
            if (level >= OSD_DEMO_PLASMA_LEVELS) {
                level = OSD_DEMO_PLASMA_LEVELS - 1;
            }
            levelSum = (uint16_t)(levelSum + level);
            levelCount++;
            osdDemoWritePlasmaCell(x, y, rows, level);
        }
    }

    // OSDM: fold fade white into perceived brightness so soft edges track the fade too.
    if (levelCount) {
        const uint8_t avg = (uint8_t)(levelSum / levelCount); // 0..7
        const uint8_t perceived = (uint8_t)((avg * (white + 1)) / 4); // 0..7
        const uint8_t soft = (uint8_t)((perceived * 5u) / (OSD_DEMO_PLASMA_LEVELS - 1)); // 0..5
        max7456Osdm((uint8_t)((soft << 3) | soft));
    }
}

static void osdDemoPaintScroller(void)
{
    if (!demoDisplay) {
        return;
    }

    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;
    const uint8_t drawH = (rows < OSD_DEMO_FONT_H) ? rows : OSD_DEMO_FONT_H;

    uint8_t floorY = (rows > drawH + 1) ? (uint8_t)(rows - drawH - 1) : 0;
    if (floorY + drawH > rows) {
        floorY = (rows > drawH) ? (uint8_t)(rows - drawH) : 0;
    }
    uint16_t hopCells = OSD_DEMO_HOP_CELLS;
    if (hopCells > floorY) {
        hopCells = floorY;
    }
    const uint16_t hopFine = (uint16_t)(hopCells * OSD_DEMO_CELL_H);
    const uint16_t phase = osdDemoBouncePhase();
    const uint16_t fine = osdDemoBounceFine(hopFine, phase);
    const uint8_t liftRows = (uint8_t)(fine / OSD_DEMO_CELL_H);
    const uint8_t soft = (uint8_t)(fine % OSD_DEMO_CELL_H);
    const uint8_t y0 = (uint8_t)(floorY - liftRows);

    osdDemoUpdatePunch(fine, phase);
    osdDemoApplyHudMotion();
    osdDemoApplyRowShimmer(y0, drawH, rows);

    max7456FillScreen(OSD_DEMO_PIXEL_OFF);

    // Stars: HOS carries the shared fine phase; glyph frame + cell hold the rest
    // so total leftward motion = starFine (parallax), continuous across HOS wrap.
    // Twinkle: briefly skip drawing so stars flicker against the row-brightness wave.
    const timeMs_t twinkleT = millis() - bounceStartMs;
    for (uint8_t layer = 0; layer < OSD_DEMO_STAR_LAYERS; layer++) {
        const uint16_t rel = (uint16_t)(starFine[layer] - scrollFine);
        const uint16_t cellShift = rel / OSD_DEMO_CELL_W;
        const uint8_t frame = (uint8_t)(rel % OSD_DEMO_STAR_FRAMES);
        for (uint8_t i = 0; i < OSD_DEMO_STARS_PER_LAYER; i++) {
            const uint8_t y = stars[layer][i].y;
            // Leave the scroller band to the text; stars live in the field around it.
            if (y >= y0 && y < (uint8_t)(y0 + drawH)) {
                continue;
            }
            // Per-star twinkle phase — mostly on, short off blinks.
            const uint8_t tw = (uint8_t)((twinkleT / 50) + (layer * 19) + (i * 37) + y * 5);
            if ((tw & 0x0F) < 3) {
                continue;
            }
            const int sx = (int)stars[layer][i].x - (int)cellShift;
            const uint8_t x = (uint8_t)(((sx % cols) + cols) % cols);
            if (y < rows) {
                max7456WriteChar(x, y, osdDemoStarGlyph(stars[layer][i].size, frame));
            }
        }
    }

    for (uint8_t x = 0; x < cols; x++) {
        const uint16_t pixelCol = (uint16_t)(scrollCol + x);
        for (uint8_t py = 0; py < drawH; py++) {
            if (!osdDemoTextOn(pixelCol, py)) {
                continue;
            }
            const uint8_t sy = (uint8_t)(y0 + py);
            if (soft == 0) {
                // Full white cell (fill offset 0).
                osdDemoWriteCell(x, sy, rows, OSD_DEMO_FILL_BASE, false);
                continue;
            }
            // Glyph g: white on rows [gOff..17]. INV flips to white on [0..gOff).
            // Shift-UP by `soft`: remainder = INV(g), spill above = g, with gOff = 18-soft.
            const uint8_t g = (uint8_t)(OSD_DEMO_FILL_BASE + (OSD_DEMO_SOFT_STEPS - soft));
            osdDemoWriteCell(x, sy, rows, g, true);
            if (sy > 0) {
                if (py > 0 && osdDemoTextOn(pixelCol, (uint8_t)(py - 1))) {
                    osdDemoWriteCell(x, (uint8_t)(sy - 1), rows, OSD_DEMO_FILL_BASE, false);
                } else {
                    osdDemoWriteCell(x, (uint8_t)(sy - 1), rows, g, false);
                }
            }
        }
    }
}



static void osdDemoWaitCycles(uint32_t deadlineTicks)
{
    const int32_t ahead = (int32_t)(deadlineTicks - getCycleCounter());
    if (ahead > 0) {
        mgStatIdleTicks += (uint32_t)ahead; // time spent waiting for the beam (load stat)
    }
    while ((int32_t)(getCycleCounter() - deadlineTicks) < 0) {
    }
}

bool osdDemoStartPlasma2x2(void)
{
    return osdDemoStartScene(7);
}

bool osdDemoStartTwister(void) __attribute__((noinline));

bool osdDemoStartTwister(void)
{
    return osdDemoStartScene(8);
}

bool osdDemoStart(void)
{
    startLastError = NULL;

    if (active) {
        fxHold = false; // `osd_demo` / `play` on a running demo: resume the auto-cycle
        return true;
    }

    // Recover from a prior mid-glyph session that left SPI hot.
    max7456MidGlyphSpiEnd();

    // Demo needs a bound OSD displayport. Boot only creates one when FEATURE_OSD
    // is on; after a wipe / failed AUTO bind osdDisplayPort stays NULL even if
    // the chip is present. Force-enable + bind MAX7456 for this session.
    if (!featureIsEnabled(FEATURE_OSD)) {
        featureEnableImmediate(FEATURE_OSD);
    }

    osdDisplayPortDevice_e type;
    displayPort_t *dp = osdGetDisplayPort(&type);

    if (!dp) {
        displayPort_t *created = NULL;
        // DisplayPortInit returns false on NOT_FOUND but still hands back a port.
        (void)max7456DisplayPortInit(vcdProfile(), &created);
        if (!created) {
            startLastError = "MAX7456 displayport init failed (CS/SPI not configured?)";
            return false;
        }
        osdInit(created, OSD_DISPLAYPORT_DEVICE_MAX7456);
        dp = created;
    }

    // Boot may leave detected=false (OSDM probe before chip ready). Rescan via
    // the existing port — do NOT call bare max7456Init() again: CS is already
    // owned and a second Init returns NOT_CONFIGURED permanently.
    if (!max7456IsDeviceDetected()) {
        (void)displayCheckReady(dp, true);
    }
    if (!max7456IsDeviceDetected()) {
        startLastError = "MAX7456 not detected (SPI/OSDM) — check wiring / power cycle";
        return false;
    }

    demoDisplay = osdGetDisplayPort(&type);
    if (!demoDisplay) {
        startLastError = "no OSD displayport after bind";
        return false;
    }

    const uint16_t len = osdDemoTextLen();
    if (len == 0) {
        startLastError = "scroll text empty";
        return false;
    }
    textPixelCols = (uint16_t)(len * OSD_DEMO_FONT_ADVANCE);
    const uint16_t tlen = osdDemoTunnelTextLen();
    tunnelTextCols = (uint16_t)(tlen * OSD_DEMO_FONT_ADVANCE);

    displayGrab(demoDisplay);

    max7456ResetHudMotionOffset();
    max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
    max7456Brightness(0, 3);
    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    max7456RefreshAll();

    if (!osdDemoInstallFont()) {
        max7456SetBackgroundType(DISPLAY_BACKGROUND_TRANSPARENT);
        max7456Brightness(displayPortProfileMax7456()->blackBrightness,
                          displayPortProfileMax7456()->whiteBrightness);
        displayReleaseAll(demoDisplay);
        demoDisplay = NULL;
        startLastError = "font NVM install failed";
        return false;
    }

    rngState = micros() ^ ((uint32_t)millis() << 16);
    osdDemoInitStars(demoDisplay->cols, demoDisplay->rows);

    scrollCol = 0;
    scrollFine = 0;
    hosSteps = osdDemoHosSteps();
    lastStepUs = micros();
    bounceStartMs = millis();
    prevBouncePhase = osdDemoBouncePhase();
    punchArmed = false;
    punchY = 0;
    punchVel = 0;
    plasmaT = 0;
    memset(prevRowBright, 0xFF, sizeof(prevRowBright)); // force first shimmer write
    active = true;
    fxHold = false;
    // Explicit local — LTO has reused a stale r0 for EnterFx on this tree before.
    {
        const osdDemoFx_e next = OSD_DEMO_FX_SCROLLER;
        osdDemoEnterFx(next);
    }

    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    osdDemoPaintScroller();
    max7456RefreshAll();
#ifdef OSD_DEMO_CHIPTUNE
    chiptuneSchedulerPark();
    // Demo comes with music (scene 9 also takes its quarter notes from the tracker).
    demoStartedChiptune = !chiptuneIsPlaying() && chiptuneStart(0);
#endif
    return true;
}

const char *osdDemoStartLastError(void)
{
    return startLastError ? startLastError : "unknown";
}

void osdDemoStop(void)
{
    if (!active) {
        return;
    }
    active = false;
    plasma2x2Armed = false;
    twisterArmed = false;
    shoutArmed = false;
    osdDemoPlasmaIrqStop();
    punchY = 0;
    punchVel = 0;
    max7456MidGlyphSpiEnd();
    max7456ResetHudMotionOffset();
    max7456SetBackgroundType(DISPLAY_BACKGROUND_TRANSPARENT);
    max7456Osdm(0x1B);
    max7456Brightness(displayPortProfileMax7456()->blackBrightness,
                      displayPortProfileMax7456()->whiteBrightness);
    if (demoDisplay) {
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        max7456RefreshAll();
        displayReleaseAll(demoDisplay);
        displayClearScreen(demoDisplay, DISPLAY_CLEAR_WAIT);
        demoDisplay = NULL;
    }
#ifdef OSD_DEMO_CHIPTUNE
    if (demoStartedChiptune) {
        chiptuneStop();
        demoStartedChiptune = false;
    }
    chiptuneSchedulerUnpark();
#endif
}

// Scenes that own the chip through a DWT/VSYNC mid-glyph engine.
static bool osdDemoFxIsMidGlyph(osdDemoFx_e f)
{
    return f == OSD_DEMO_FX_PLASMA2X2 || f == OSD_DEMO_FX_TWISTER || f == OSD_DEMO_FX_SHOUT
        || f == OSD_DEMO_FX_PLASMA_IRQ;
}

static timeMs_t osdDemoFxDurationMs(osdDemoFx_e f)
{
    switch (f) {
    case OSD_DEMO_FX_SCROLLER:  return OSD_DEMO_FX_SCROLLER_MS;
    case OSD_DEMO_FX_PLASMA:    return OSD_DEMO_FX_PLASMA_MS;
    case OSD_DEMO_FX_FIRE:      return OSD_DEMO_FX_FIRE_MS;
    case OSD_DEMO_FX_WIPE:      return OSD_DEMO_FX_WIPE_MS;
    case OSD_DEMO_FX_TUNNEL:    return OSD_DEMO_FX_TUNNEL_MS;
    case OSD_DEMO_FX_PLASMA2X2: return OSD_DEMO_FX_PLASMA2X2_MS;
    case OSD_DEMO_FX_TWISTER:   return OSD_DEMO_FX_TWISTER_MS;
    case OSD_DEMO_FX_SHOUT:     return OSD_DEMO_FX_SHOUT_MS;
    case OSD_DEMO_FX_PLASMA_IRQ: return OSD_DEMO_FX_PLASMA_IRQ_MS;
    default:                    return OSD_DEMO_FX_SCROLLER_MS;
    }
}

// Auto-cycle order: 1 scroller → 2 plasma → 3 fire → 4 wipe → 5 tunnel → 7 plasma 2×2 →
// 8 twister → 9 shoutouts → back to 1.
static osdDemoFx_e osdDemoFxNext(osdDemoFx_e f)
{
    switch (f) {
    case OSD_DEMO_FX_SCROLLER:  return OSD_DEMO_FX_PLASMA;
    case OSD_DEMO_FX_PLASMA:    return OSD_DEMO_FX_FIRE;
    case OSD_DEMO_FX_FIRE:      return OSD_DEMO_FX_WIPE;
    case OSD_DEMO_FX_WIPE:      return OSD_DEMO_FX_TUNNEL;
    case OSD_DEMO_FX_TUNNEL:    return OSD_DEMO_FX_PLASMA2X2;
    case OSD_DEMO_FX_PLASMA2X2: return OSD_DEMO_FX_TWISTER;
    case OSD_DEMO_FX_TWISTER:   return OSD_DEMO_FX_SHOUT;
    default:                    return OSD_DEMO_FX_SCROLLER;
    }
}

// CLI: run one scene permanently (1..5, 7, 8, 9). Starts the demo if needed.
bool osdDemoStartScene(uint8_t scene)
{
    static const int8_t sceneFx[11] = {
        -1, OSD_DEMO_FX_SCROLLER, OSD_DEMO_FX_PLASMA, OSD_DEMO_FX_FIRE, OSD_DEMO_FX_WIPE,
        OSD_DEMO_FX_TUNNEL, -1, OSD_DEMO_FX_PLASMA2X2, OSD_DEMO_FX_TWISTER, OSD_DEMO_FX_SHOUT,
        OSD_DEMO_FX_PLASMA_IRQ,
    };
    if (scene >= ARRAYLEN(sceneFx) || sceneFx[scene] < 0) {
        startLastError = "unknown scene (1..5, 7..10)";
        return false;
    }
    if (ARMING_FLAG(ARMED)) {
        startLastError = "disarm first";
        return false;
    }
    if (!active && !osdDemoStart()) {
        return false;
    }
    fxHold = true;
    const osdDemoFx_e next = (osdDemoFx_e)sceneFx[scene];
    // Already running (re-issued from cliProcess inside the engine loop): just hold it.
    if (fx == next && ((next == OSD_DEMO_FX_TWISTER && twisterArmed)
                       || (next == OSD_DEMO_FX_PLASMA2X2 && plasma2x2Armed)
                       || (next == OSD_DEMO_FX_SHOUT && shoutArmed)
                       || (next == OSD_DEMO_FX_PLASMA_IRQ && plasmaIrqArmed))) {
        return true;
    }
    osdDemoEnterFx(next);
    return true;
}

bool osdDemoIsActive(void)
{
    return active;
}

void osdDemoUpdate(timeUs_t currentTimeUs)
{
    if (!active || !demoDisplay) {
        return;
    }

    // Switch effects on wall-clock first (independent of SPI idle / step gates).
    // Keep EnterFx args in locals so LTO cannot reuse a stale r0 (seen passing
    // the heartbeat "reinited" flag instead of OSD_DEMO_FX_SCROLLER).
    const timeMs_t nowMs = millis();
    const timeMs_t elapsed = nowMs - fxStartMs;
    const timeMs_t fxMs = osdDemoFxDurationMs(fx);
    if (elapsed >= fxMs) {
        if (!fxHold) {
            const osdDemoFx_e next = osdDemoFxNext(fx);
            osdDemoEnterFx(next);
            return;
        }
        // Held: mid-glyph engines run continuously; time-phased scenes loop on themselves.
        if (!osdDemoFxIsMidGlyph(fx)) {
            const osdDemoFx_e same = fx;
            osdDemoEnterFx(same);
            return;
        }
    }

    // 2×2 plasma / twister use DWT+VSYNC engines — poll every heartbeat.
    if (fx == OSD_DEMO_FX_PLASMA2X2) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoPlasma2x2EnginePoll();
        return;
    }
    if (fx == OSD_DEMO_FX_TWISTER) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoTwisterEnginePoll();
        return;
    }
    if (fx == OSD_DEMO_FX_SHOUT) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoShoutEnginePoll();
        return;
    }
    if (fx == OSD_DEMO_FX_PLASMA_IRQ) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        // Non-blocking: the ISR chain draws; the task only builds the next frame.
        osdDemoPlasmaIrqTask();
        return;
    }

    // Same step rate for all effects so HOS/star phase never drifts.
    const timeDelta_t stepUs = 1000000 / OSD_DEMO_SCROLL_HZ;
    if ((int32_t)(currentTimeUs - lastStepUs) < stepUs) {
        return;
    }

    // Do not require buffersSynced — while animating it is almost never true on
    // real hardware, which starved paints and made FX switches look stuck.
    if (max7456DmaInProgress()) {
        return;
    }

    lastStepUs += stepUs;
    if ((int32_t)(currentTimeUs - lastStepUs) > stepUs) {
        lastStepUs = currentTimeUs;
    }

    // 1) Build the next frame in CPU shadow first (tunnel/plasma are heavy).
    // 2) Then VSYNC + SPI immediately — never burn blanking on paint math, or
    //    the beam slices the tunnel text band (strobe / pixel jitter).
    osdDemoAdvanceHosAndStars();

    if (fx == OSD_DEMO_FX_PLASMA) {
        plasmaT++;
        osdDemoPaintPlasma();
    } else if (fx == OSD_DEMO_FX_FIRE) {
        osdDemoPaintFire();
    } else if (fx == OSD_DEMO_FX_WIPE) {
        osdDemoPaintWipe();
    } else if (fx == OSD_DEMO_FX_TUNNEL) {
        osdDemoPaintTunnel();
    } else {
        osdDemoPaintScroller();
    }

    // Soft-scroll (no wrap): early HOS. Wrap: HOS stays deferred until pass end.
    osdDemoSyncFlush(true, true);
}

#endif // USE_MAX7456
