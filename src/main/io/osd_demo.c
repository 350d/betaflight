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
#include "drivers/sound_beeper.h"
#include "io/chiptune.h"
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
#define OSD_DEMO_FIRE_IGNITE_MS     1500
#define OSD_DEMO_FIRE_FALL_TICKS    4   // ticks per collapsed row (~1.1s for 16 rows @ 72Hz)
#define OSD_DEMO_FIRE_STEP_TICKS    3   // flame physics ~24Hz @ SCROLL_HZ (was every tick)
#define OSD_DEMO_FIRE_COLS          30
#define OSD_DEMO_FIRE_ROWS          16
#define OSD_DEMO_FIRE_SPARKS        4
#define OSD_DEMO_FIRE_CORE_HALF     5
#define OSD_DEMO_FIRE_CURVE_ROWS    2
#define OSD_DEMO_FIRE_CURVE_MAX_PX  OSD_DEMO_CELL_H // bend profile 0..18 on row 1

// Scene 7 (CLI-only, not in auto cycle): mid-raster Display SRAM rewrite probe.
#define OSD_DEMO_RASTER_DEFAULT_X       15
#define OSD_DEMO_RASTER_DEFAULT_Y       8
// Strict PAL field length on this F411 @ 108 MHz DWT. Never retune this to
// paper over scheduler lateness — use periodCorrectionTicks (fine) only.
#define OSD_DEMO_RASTER_NOMINAL_PERIOD_TICKS  2160000u
#define OSD_DEMO_RASTER_NOMINAL_FIELD_US      20000u
#define OSD_DEMO_RASTER_CORR_MAX_US           1000   // fine cal only (±1 ms)
#define OSD_DEMO_RASTER_CORR_DEFAULT_US       0      // VSYNC lock does not need period corr
#define OSD_DEMO_RASTER_DEFAULT_PHASE_US 16512  // user-tuned mid-glyph window
#define OSD_DEMO_RASTER_DELAY_MAX_US    20000  // ~1 PAL character-row window budget
#define OSD_DEMO_RASTER_SWEEP_STEP_US   1
#define OSD_DEMO_RASTER_ATTR_INV        0x01   // per-cell attribute bit0
#define OSD_DEMO_CHARS_PER_LINE         30
// If A is already this late, skip the field — collapsed A→B looks like blink.
#define OSD_DEMO_RASTER_LATE_SKIP_US    200
#define OSD_DEMO_RASTER_MARK_X          15
#define OSD_DEMO_RASTER_MARK_Y          0     // top row — impossible to miss
#define OSD_DEMO_RASTER_MARK_INTERVAL_DEFAULT 500
#define OSD_DEMO_RASTER_MARK_FLASH_HOLD 3     // keep WHITE this many fields (visible on 30fps capture)

typedef enum {
    OSD_DEMO_FX_SCROLLER = 0,
    OSD_DEMO_FX_PLASMA,
    OSD_DEMO_FX_FIRE,
    OSD_DEMO_FX_WIPE,
    OSD_DEMO_FX_TUNNEL,
    OSD_DEMO_FX_PLASMA2X2, // scene 7 — 2×2 mid-glyph plasma
    OSD_DEMO_FX_RASTER,     // experimental — enter via `osd_demo raster`, never auto-cycle
    OSD_DEMO_FX_HOSTEST,    // mid-scanline HOS poke — CLI `osd_demo hostest`
} osdDemoFx_e;

typedef enum {
    OSD_DEMO_RASTER_GLYPH = 0,  // switch character index mid-cell
    OSD_DEMO_RASTER_INVERT,     // switch per-cell INV attribute mid-cell
    OSD_DEMO_RASTER_SWEEP,      // glyph mode + auto-increment delay
    OSD_DEMO_RASTER_CAL,        // boot-button sync calibrator (mega-pixel UI)
    OSD_DEMO_RASTER_MARK,       // blink every N software fields (period self-check)
    OSD_DEMO_RASTER_VSYNC,      // lock A→B to MAX7456 STAT[4] VSYNC falling edge
    OSD_DEMO_RASTER_DIAG,       // VSYNC + cumulative diagonal hatch in one cell
    OSD_DEMO_RASTER_FILL,       // VSYNC + mid-glyph hatch across every character row
} osdDemoRasterMode_e;

#define OSD_DEMO_CAL_CENTER_INIT_US  16512
#define OSD_DEMO_CAL_RANGE_INIT_US   1000
#define OSD_DEMO_CAL_RANGE_MIN_US    50
#define OSD_DEMO_CAL_SWEEP_MS        8000   // slow full triangle ±range (~8s)
#define OSD_DEMO_CAL_TEXT_ROWS       3
#define OSD_DEMO_CAL_DIGIT_ADV       6     // 5×5 font @ 6×6 mega-pixels

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
static osdDemoFx_e fx;
static uint16_t prevBouncePhase;
static bool punchArmed;

static int16_t punchY;
static int16_t punchVel;
static uint8_t prevRowBright[OSD_DEMO_RB_ROWS];

static osdDemoStar_t stars[OSD_DEMO_STAR_LAYERS][OSD_DEMO_STARS_PER_LAYER];
static uint16_t starFine[OSD_DEMO_STAR_LAYERS]; // absolute leftward px; synced to HOS

// Raster timing probe (CLI) — Display SRAM only, no NVM/font writes.
static osdDemoRasterMode_e rasterMode;
static uint8_t rasterX = OSD_DEMO_RASTER_DEFAULT_X;
static uint8_t rasterY = OSD_DEMO_RASTER_DEFAULT_Y;
static uint16_t rasterDelayUs = 0;
static uint8_t rasterGlyphA = OSD_DEMO_FILL_BASE;     // full white 12x18
static uint8_t rasterGlyphB = OSD_DEMO_PIXEL_OFF;     // full black 12x18
static uint8_t rasterInvGlyph = (uint8_t)(OSD_DEMO_HFILL_BASE + 9); // asymmetric top-white
static timeUs_t rasterLastPulseUs;
static bool rasterStaticPainted;

// CAL: CLI-only sync calibrator (catch/commit/reset). Moving bar = delay within ±range.
static uint16_t calCenterUs;
static uint16_t calRangeUs;
static int16_t calOffsetUs;          // -range..+range
static int8_t calSweepDir;           // +1 / -1 triangle
static uint16_t calDynamicUs;        // center+offset (clamped), applied as pulse delay
static timeMs_t calSweepLastMs;
static uint32_t calSweepPhaseMs;     // 0 .. OSD_DEMO_CAL_SWEEP_MS (triangle phase)
static uint8_t calLastDynSlot;       // last painted mover column (skip redraw if unchanged)
static bool calUiForce;

// DWT CYCCNT software oscillator — ideal grid. SPI A/B fire inside a
// deterministic busy-wait on CYCCNT (not on the next OSD-task schedule).
static int32_t periodCorrectionTicks; // fine only; effective = NOMINAL + corr
static bool periodCorrSeeded;
static uint32_t rasterPhaseTicks;    // A→B offset from field epoch
static uint32_t rasterFieldEpoch;    // absolute DWT tick of current field start
static uint32_t rasterLateCount;
static uint32_t rasterMaxLateTicks;
static bool rasterEngineArmed;

// MARK: compare one short mid-glyph rewrite at event 0 vs event N (same phaseTicks).
static uint32_t rasterEventCounter;
static uint16_t markInterval = OSD_DEMO_RASTER_MARK_INTERVAL_DEFAULT;
static bool markRepeat;
static bool markTestActive;
static uint32_t markStartEvent;
static uint32_t markStartTick;
static uint32_t markEndEvent;
static uint32_t markEndTick;
static uint32_t markLateEvents;
static uint32_t markMaxLateTicks;
static bool markPairComplete;
static uint32_t markExpectedElapsedTicks;
static int32_t markElapsedErrorTicks;
static int32_t markErrorPerEventTicks;

// VSYNC lock via STAT[4] SPI poll (no physical VSYNC pin).
static uint32_t vsyncLockCount;
static uint32_t vsyncTimeoutCount;
static uint32_t vsyncLastEdgeTicks;
#define OSD_DEMO_RASTER_VSYNC_TIMEOUT_US  30000
#define OSD_DEMO_RASTER_LINE_US_DEFAULT   64    // PAL line ≈ 64 us
#define OSD_DEMO_RASTER_GLYPH_ROWS        18    // MAX7456 character height
// Small SPI lead kept for non-HSYNC paths; diag uses HSYNC edges instead.
#define OSD_DEMO_RASTER_SPI_LEAD_US_DEFAULT  24
// VSYNC→row0 top. Calibrated so Y=8, line=64 → 1504+8*18*64 = 10720 (user).
#define OSD_DEMO_RASTER_VBLANK_US         1504
#define OSD_DEMO_RASTER_DIAG_DEFAULT_PHASE_US  10720 // legacy fallback; prefer auto
// PAL field = 312.5 lines; lineTicks = fieldTicks * 2 / 625 (stats only).
#define OSD_DEMO_RASTER_PAL_FIELD_LINES_X2  625

static uint16_t rasterLineUs = OSD_DEMO_RASTER_LINE_US_DEFAULT;
static uint16_t rasterSpiLeadUs = OSD_DEMO_RASTER_SPI_LEAD_US_DEFAULT;
static bool rasterPhaseAuto; // recompute phase from cell Y + line_us
static uint32_t rasterMeasuredLineTicks; // from VSYNCΔ (stats); diag steps use line_us
static uint8_t rasterFieldParity; // PAL interlaced odd/even → ±½ line

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
#define OSD_DEMO_PLASMA2X2_BURST_US  36
#define OSD_DEMO_PLASMA2X2_ODD_PAD_US 4  // keep clear of odd/even edges
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
static uint8_t plasmaPrevPack[OSD_DEMO_CHECKER_COLS]; // dirty: last written mask 0..63

// Mid-scanline HOS probe: absolute register poke (no framebuffer touch after paint).
// Keep A left of centre so B=+16px does not clip the right-edge fiducial.
#define OSD_DEMO_HOSTEST_HOS_A          16  // left reference
#define OSD_DEMO_HOSTEST_HOS_B          32  // +16 px right
#define OSD_DEMO_HOSTEST_DEFAULT_CHAR_Y 8   // character row for the kink
#define OSD_DEMO_HOSTEST_DEFAULT_PY     9   // mid-glyph pixel row (0..17)
static bool hosTestArmed;
static uint8_t hosTestField; // phase counter for A-hold / B-hold / mid-poke
static uint16_t hosTestLine = (uint16_t)(OSD_DEMO_HOSTEST_DEFAULT_CHAR_Y * OSD_DEMO_CELL_H
                                         + OSD_DEMO_HOSTEST_DEFAULT_PY);

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

static bool osdDemoInstallFont(void)
{
    bool ok = true;

    // Undo older demos that stomped SYM_BLANK (0x20) — BF fills the screen with it.
    if (!max7456WriteNvm(0x20, osdDemoBlankNvm)) {
        ok = false;
    }
    for (uint16_t i = 0; ok && i < OSD_DEMO_GLYPH_COUNT; i++) {
        if (!max7456WriteNvm((uint8_t)(OSD_DEMO_GLYPH_BASE + i), osdDemoGlyphNvm[i])) {
            ok = false;
        }
    }
    for (uint8_t i = 0; ok && i < OSD_DEMO_HFILL_STEPS; i++) {
        if (!max7456WriteNvm((uint8_t)(OSD_DEMO_HFILL_BASE + i), osdDemoHFillNvm[i])) {
            ok = false;
        }
        if (ok && !max7456WriteNvm((uint8_t)(OSD_DEMO_HFILL_BOT_BASE + i), osdDemoHFillBotNvm[i])) {
            ok = false;
        }
    }
    for (uint8_t i = 0; ok && i < OSD_DEMO_STRIPE_COUNT; i++) {
        if (!max7456WriteNvm((uint8_t)(OSD_DEMO_STRIPE_BASE + i), osdDemoStripeNvm[i])) {
            ok = false;
        }
    }
    for (uint8_t i = 0; ok && i < OSD_DEMO_PX22_GLYPHS; i++) {
        if (!max7456WriteNvm((uint8_t)(OSD_DEMO_PX22_BASE + i), osdDemoPx22Nvm[i])) {
            ok = false;
        }
    }
    for (uint8_t i = 0; ok && i < OSD_DEMO_TUNNEL_GLYPHS; i++) {
        uint8_t addr = (uint8_t)(OSD_DEMO_TUNNEL_BASE + i);
        if (addr == OSD_DEMO_TUNNEL_BLANK_IDX) {
            addr = OSD_DEMO_TUNNEL_ALT; // keep SYM_BLANK transparent
        }
        if (!max7456WriteNvm(addr, osdDemoTunnelNvm[i])) {
            ok = false;
        }
    }
    // Tunnel loop may have written 0x00..0x3F; force SYM_BLANK back to transparent.
    if (ok && !max7456WriteNvm(OSD_DEMO_TUNNEL_BLANK_IDX, osdDemoBlankNvm)) {
        ok = false;
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
static void osdDemoHosTestEnginePoll(void);
static void osdDemoPaintHosTest(void);
static void osdDemoRasterWaitUntil(uint32_t deadlineTicks);
static void osdDemoPaintRaster(void);

// Drain SPI after the CPU shadow is ready. waitVsync=true: wait then flush
// (EnterFx / post-paint). Never put heavy paint BETWEEN vsync and flush —
// tunnel math alone burns blanking and the beam tears the text band.
static void osdDemoSyncFlush(bool waitVsync, bool applyHosEarly)
{
#ifdef USE_CHIPTUNE
    beeperPwmAyFifoFill();
#endif
    if (waitVsync) {
        (void)max7456WaitVsyncFallingEdge(NULL, OSD_DEMO_RASTER_VSYNC_TIMEOUT_US);
    }
    if (applyHosEarly && !hosWrappedThisStep) {
        max7456ApplyHudMotionNow();
    }
    while (max7456DrawScreen()) {
#ifdef USE_CHIPTUNE
        beeperPwmAyFifoFill();
#endif
    }
#ifdef USE_CHIPTUNE
    beeperPwmAyFifoFill();
#endif
}
static void osdDemoRasterEngineInit(void);
static void osdDemoRasterEnginePoll(void);
static void osdDemoRasterWriteA(void);
static void osdDemoRasterWriteB(void);
static void osdDemoPaintRasterCal(void);
static void osdDemoRasterCalReset(void);
static void osdDemoRasterCalCatch(void);
static void osdDemoRasterCalCommit(void);
static void osdDemoRasterCalAdvanceSweep(void);
static void osdDemoRasterMarkSetCell(uint8_t glyph);
static void osdDemoRasterMarkOnEvent(uint32_t eventIndex, uint32_t eventTick, uint32_t lateTicks);

static uint32_t osdDemoRasterEffectivePeriodTicks(void)
{
    return (uint32_t)((int32_t)OSD_DEMO_RASTER_NOMINAL_PERIOD_TICKS + periodCorrectionTicks);
}

static void osdDemoRasterClampPeriodCorrection(void)
{
    const int32_t maxc = (int32_t)clockMicrosToCycles(OSD_DEMO_RASTER_CORR_MAX_US);
    if (periodCorrectionTicks > maxc) {
        periodCorrectionTicks = maxc;
    } else if (periodCorrectionTicks < -maxc) {
        periodCorrectionTicks = -maxc;
    }
}

static void osdDemoRasterClampPhaseToPeriod(void)
{
    const uint32_t period = osdDemoRasterEffectivePeriodTicks();
    if (rasterPhaseTicks >= period) {
        rasterPhaseTicks = period - 1;
    }
}

// Shared soft-scroll phase: HOS + star parallax stay continuous across effects.
// Plasma/wipe: freeze scrollFine and clear HOS. Fire: stars only — paintFire owns HUD.
static void osdDemoAdvanceHosAndStars(void)
{
    hosSteps = osdDemoHosSteps();
    hosWrappedThisStep = false;
    if (fx == OSD_DEMO_FX_RASTER || fx == OSD_DEMO_FX_HOSTEST) {
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
#ifdef USE_CHIPTUNE
        beeperPwmAyFifoFill();
#endif
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
#ifdef USE_CHIPTUNE
        beeperPwmAyFifoFill();
#endif
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

// --- Fast 2×1 hatch renderer (F7/FD full-cell diagonal) ----------------

static uint8_t osdDemoHatch2x1Glyph(uint8_t row)
{
    return (row & 1u) ? (uint8_t)OSD_DEMO_FILL_GLYPH_P1 : (uint8_t)OSD_DEMO_FILL_GLYPH_P0;
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

static void osdDemoPaintHatch2x1Screen(uint8_t cols, uint8_t rows, bool cornerFiducials)
{
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > OSD_DEMO_CHARS_PER_LINE) {
        cols = OSD_DEMO_CHARS_PER_LINE;
    }
    for (uint8_t y = 0; y < rows; y++) {
        const uint16_t addr = (uint16_t)((uint16_t)y * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
        (void)max7456WriteDisplaySramRowFillEx(addr, osdDemoHatch2x1Glyph(y), cols, true);
    }
    if (cornerFiducials) {
        const uint8_t xr = (uint8_t)(cols - 1);
        const uint8_t yb = (uint8_t)(rows - 1);
        max7456WriteChar(0, 0, OSD_DEMO_FILL_BASE);
        max7456WriteChar(xr, 0, OSD_DEMO_FILL_BASE);
        max7456WriteChar(0, yb, OSD_DEMO_FILL_BASE);
        max7456WriteChar(xr, yb, OSD_DEMO_FILL_BASE);
    }
}

// Scene 7 — mid-glyph 2×2 plasma:
// Solid PX22 (all 18 rows) → both lines of each band lit. Early-odd SPI + prefetch
// so the burst stays ahead of the beam (avoids right-side 1px shear). No INV.

static int8_t plasmaSx[OSD_DEMO_CHECKER_COLS * OSD_DEMO_CHECKER_CELL_W2];
static int8_t plasmaSy[OSD_DEMO_CHECKER_ROWS * OSD_DEMO_PLASMA2X2_BANDS];
static int8_t plasmaSd[256];

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
    }
}

static void osdDemoPlasma2x2BuildMasks(uint8_t y, uint8_t b,
                                       uint8_t cols, uint8_t *glyphs, uint8_t *packs)
{
    const uint16_t my = (uint16_t)y * OSD_DEMO_PLASMA2X2_BANDS + b;
    for (uint8_t x = 0; x < cols; x++) {
        uint8_t pat = 0;
        const uint16_t x0 = (uint16_t)x * OSD_DEMO_CHECKER_CELL_W2;
        for (uint8_t bc = 0; bc < OSD_DEMO_CHECKER_CELL_W2; bc++) {
            const uint16_t mx = x0 + bc;
            int16_t v = (int16_t)plasmaSx[mx] + (int16_t)plasmaSy[my]
                + (int16_t)plasmaSd[(uint8_t)(mx + my)];
            int16_t s = (int16_t)(v + 384);
            if (s < 0) {
                s = 0;
            }
            if (s > 767) {
                s = 767;
            }
            if (((((uint16_t)s * OSD_DEMO_PLASMA_ZEBRA_BANDS) >> 9) & 1u) != 0u) {
                pat |= (uint8_t)(1u << bc);
            }
        }
        glyphs[x] = (uint8_t)(OSD_DEMO_PX22_BASE + pat);
        packs[x] = pat;
    }
}

static void osdDemoPlasma2x2WriteRow(uint16_t addr, const uint8_t *glyphs,
                                     const uint8_t *packs, uint8_t cols)
{
    if (memcmp(plasmaPrevPack, packs, cols) == 0) {
        return;
    }
    (void)max7456WriteDisplaySramRowGlyphs(addr, glyphs, cols, false);
    memcpy(plasmaPrevPack, packs, cols);
}

static void osdDemoPlasma2x2EnginePoll(void)
{
    if (!plasma2x2Armed || !demoDisplay) {
        return;
    }

    uint8_t cols = demoDisplay->cols;
    uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows == 0) {
        return;
    }
    if (cols > OSD_DEMO_CHECKER_COLS) {
        cols = OSD_DEMO_CHECKER_COLS;
    }
    if (rows > OSD_DEMO_CHECKER_ROWS) {
        rows = OSD_DEMO_CHECKER_ROWS;
    }

    const uint32_t burstTicks = clockMicrosToCycles(OSD_DEMO_PLASMA2X2_BURST_US);
    const uint32_t padTicks = clockMicrosToCycles(OSD_DEMO_PLASMA2X2_ODD_PAD_US);
    const uint32_t lineTicks = clockMicrosToCycles(rasterLineUs ? rasterLineUs : OSD_DEMO_RASTER_LINE_US_DEFAULT);
    const uint32_t row0Us = OSD_DEMO_RASTER_VBLANK_US;

    uint8_t glyphs[2][OSD_DEMO_CHECKER_COLS];
    uint8_t packs[2][OSD_DEMO_CHECKER_COLS];

    max7456MidGlyphSpiBegin();

    uint8_t vsyncFails = 0;
    while (active && plasma2x2Armed && !ARMING_FLAG(ARMED)) {
        if ((millis() - fxStartMs) >= OSD_DEMO_FX_PLASMA2X2_MS) {
            plasma2x2Armed = false;
            break;
        }

        uint32_t edgeTicks = 0;
        max7456MidGlyphSpiBoost(false);
        if (!max7456WaitVsyncFallingEdge(&edgeTicks, OSD_DEMO_RASTER_VSYNC_TIMEOUT_US)) {
            if (++vsyncFails >= 8) {
                plasma2x2Armed = false;
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

        const uint32_t row0 = edgeTicks + clockMicrosToCycles(row0Us);
        if (++plasmaPhaseDiv >= 2u) {
            plasmaPhaseDiv = 0;
            plasmaPhase++;
        }
        osdDemoPlasma2x2BuildLuts(cols, rows, plasmaPhase);

        max7456MidGlyphSpiBoost(true);
        const uint32_t blankDeadline = row0 - burstTicks;

        uint8_t yPre = 0;
        for (; yPre < rows; yPre++) {
            if ((int32_t)(getCycleCounter() - blankDeadline) > 0) {
                break;
            }
            osdDemoPlasma2x2BuildMasks(yPre, 0, cols, glyphs[0], packs[0]);
            const uint16_t addr = (uint16_t)((uint16_t)yPre * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
            memset(plasmaPrevPack, 0xFF, cols);
            osdDemoPlasma2x2WriteRow(addr, glyphs[0], packs[0], cols);
        }

        for (uint8_t y = 0; y < rows; y++) {
            const uint16_t addr = (uint16_t)((uint16_t)y * (uint16_t)OSD_DEMO_CHARS_PER_LINE);
            memset(plasmaPrevPack, 0xFF, cols);
            const uint8_t b0 = (y < yPre) ? 1u : 0u;
            if (b0 >= OSD_DEMO_PLASMA2X2_BANDS) {
                continue;
            }

            uint8_t cur = 0;
            osdDemoPlasma2x2BuildMasks(y, b0, cols, glyphs[cur], packs[cur]);

            for (uint8_t b = b0; b < OSD_DEMO_PLASMA2X2_BANDS; b++) {
                const uint8_t py = (uint8_t)(b * OSD_DEMO_PLASMA2X2_STEP);
                const uint32_t due = row0
                    + ((uint32_t)y * (uint32_t)OSD_DEMO_CHECKER_CELL_H + (uint32_t)py) * lineTicks;
                uint32_t wrAt = due - lineTicks + padTicks;
                const uint32_t finishBy = due - padTicks;
                if ((int32_t)(wrAt - edgeTicks) < 0) {
                    wrAt = edgeTicks;
                }

                bool nextBuilt = false;
                if ((int32_t)(getCycleCounter() - wrAt) < 0) {
                    if ((uint8_t)(b + 1u) < OSD_DEMO_PLASMA2X2_BANDS) {
                        osdDemoPlasma2x2BuildMasks(y, (uint8_t)(b + 1u), cols,
                                                   glyphs[cur ^ 1u], packs[cur ^ 1u]);
                        nextBuilt = true;
                    }
                    osdDemoRasterWaitUntil(wrAt);
                }

                const uint32_t tWrite = getCycleCounter();
                if ((int32_t)(finishBy - tWrite) >= (int32_t)burstTicks) {
                    osdDemoPlasma2x2WriteRow(addr, glyphs[cur], packs[cur], cols);
                }

                if ((uint8_t)(b + 1u) < OSD_DEMO_PLASMA2X2_BANDS) {
                    if (!nextBuilt) {
                        osdDemoPlasma2x2BuildMasks(y, (uint8_t)(b + 1u), cols,
                                                   glyphs[cur ^ 1u], packs[cur ^ 1u]);
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

    max7456MidGlyphSpiEnd();
}

// Mid-scanline HOS probe (display timing only — no Display SRAM rewrite after paint):
//   full-height white bar + corner fiducials
//   repeating 4-field cycle:
//     0: hold HOS=A all field     — bar LEFT  (proves write + latch)
//     1: hold HOS=B all field     — bar RIGHT (should jump every other field)
//     2/3: A at VSYNC, B at mid of hosTestLine — look for mid-line kink
// Readout:
//   - bar jumps L↔R every field on 0/1     → HOS writes work (field or faster)
//   - kink mid-line on phase 2/3           → HOS is realtime
//   - shift from next line only on 2/3     → line-latch
//   - phase 2/3 looks identical to hold-B  → field-latch (mid poke too late for this field)
static void osdDemoPaintHosTest(void)
{
    if (!demoDisplay) {
        return;
    }
    uint8_t cols = demoDisplay->cols;
    uint8_t rows = demoDisplay->rows;
    if (cols == 0 || rows == 0) {
        return;
    }
    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    // Slightly left of centre so +16 HOS still keeps the bar on-screen.
    const uint8_t x = (cols > 2u) ? (uint8_t)((cols / 2u) - 1u) : 0u;
    for (uint8_t y = 0; y < rows; y++) {
        max7456WriteChar(x, y, OSD_DEMO_FILL_BASE);
        max7456CommitShadowCell((uint16_t)((uint16_t)y * (uint16_t)OSD_DEMO_CHARS_PER_LINE + x),
                                OSD_DEMO_FILL_BASE, false);
    }
    // Corner fiducials — right edge must stay fully visible at HOS=B.
    max7456WriteChar(0, 0, OSD_DEMO_FILL_BASE);
    max7456WriteChar((uint8_t)(cols - 1), 0, OSD_DEMO_FILL_BASE);
    max7456WriteChar(0, (uint8_t)(rows - 1), OSD_DEMO_FILL_BASE);
    max7456WriteChar((uint8_t)(cols - 1), (uint8_t)(rows - 1), OSD_DEMO_FILL_BASE);
}

static void osdDemoHosTestEnginePoll(void)
{
    if (!hosTestArmed || !demoDisplay) {
        return;
    }

    const uint32_t lineTicks = clockMicrosToCycles(rasterLineUs ? rasterLineUs : OSD_DEMO_RASTER_LINE_US_DEFAULT);
    const uint32_t row0Us = OSD_DEMO_RASTER_VBLANK_US;
    const uint32_t midLineTicks = lineTicks / 2u;
    const uint16_t line = hosTestLine;

    max7456MidGlyphSpiBegin();

    uint8_t vsyncFails = 0;
    while (active && hosTestArmed && !ARMING_FLAG(ARMED)) {
        uint32_t edgeTicks = 0;
        if (!max7456WaitVsyncFallingEdge(&edgeTicks, OSD_DEMO_RASTER_VSYNC_TIMEOUT_US)) {
            if (++vsyncFails >= 8) {
                hosTestArmed = false;
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
        vsyncLockCount++;
        vsyncLastEdgeTicks = edgeTicks;

        const uint8_t phase = (uint8_t)(hosTestField++ & 3u);
        if (phase == 0u) {
            // Whole-field A — visible LEFT reference (~25% of time).
            max7456WriteHosNow(OSD_DEMO_HOSTEST_HOS_A);
        } else if (phase == 1u) {
            // Whole-field B — visible RIGHT reference; bar must jump vs phase 0.
            max7456WriteHosNow(OSD_DEMO_HOSTEST_HOS_B);
        } else {
            // Mid-line poke A→B on hosTestLine.
            max7456WriteHosNow(OSD_DEMO_HOSTEST_HOS_A);
            const uint32_t pokeAt = edgeTicks
                + clockMicrosToCycles(row0Us)
                + (uint32_t)line * lineTicks
                + midLineTicks;
            osdDemoRasterWaitUntil(pokeAt);
            max7456WriteHosNow(OSD_DEMO_HOSTEST_HOS_B);
        }

#ifdef USE_CLI
        if (cliMode) {
            (void)cliProcess();
        }
#endif
    }

    max7456ResetHudMotionOffset();
    max7456ApplyHudMotionNow();
    max7456MidGlyphSpiEnd();
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
    hosTestArmed = false;
    rasterEngineArmed = false;
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
        rasterEngineArmed = false;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        // Solid PX22 into NVM. EndFontWrite — WriteNvm leaves OSD off.
        for (uint8_t i = 0; i < OSD_DEMO_PX22_GLYPHS; i++) {
            (void)max7456WriteNvm((uint8_t)(OSD_DEMO_PX22_BASE + i), osdDemoPx22Nvm[i]);
        }
        max7456EndFontWrite();
        if (demoDisplay) {
            osdDemoFillRowsGlyphFast(demoDisplay->cols, demoDisplay->rows,
                                     OSD_DEMO_PX22_BASE, OSD_DEMO_PX22_BASE);
        } else {
            max7456FillScreen(OSD_DEMO_PX22_BASE);
        }
        plasma2x2Armed = true;
    } else if (next == OSD_DEMO_FX_RASTER) {
        rasterStaticPainted = false;
        rasterLastPulseUs = 0;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        osdDemoPaintRaster();
        max7456RefreshAll();
        rasterStaticPainted = true;
    } else if (next == OSD_DEMO_FX_HOSTEST) {
        rasterEngineArmed = false;
        plasma2x2Armed = false;
        max7456Osdm(0x1B);
        max7456Brightness(0, 3);
        max7456SetBackgroundType(DISPLAY_BACKGROUND_BLACK);
        max7456SetHudMotionOffset(0, 0);
        osdDemoPaintHosTest();
        max7456RefreshAll();
        hosTestField = 0;
        max7456WriteHosNow(OSD_DEMO_HOSTEST_HOS_A);
        hosTestArmed = true;
    } else {
        bounceStartMs = millis();
        prevBouncePhase = 0;
        scrollCol = 0;
        max7456Osdm(0x1B);
        max7456FillScreen(OSD_DEMO_PIXEL_OFF);
        osdDemoPaintScroller();
    }
    // First frame of every scene: same vsync+SPI drain as the steady Update path.
    // Raster / mid-glyph plasma / HOS probe own the chip directly — skip shadow flush.
    if (next != OSD_DEMO_FX_RASTER && next != OSD_DEMO_FX_PLASMA2X2
        && next != OSD_DEMO_FX_HOSTEST) {
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
#ifdef USE_CHIPTUNE
        beeperPwmAyFifoFill();
#endif
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


static uint16_t osdDemoRasterAddr(void)
{
    return (uint16_t)(rasterY * OSD_DEMO_CHARS_PER_LINE + rasterX);
}

// VSYNC falling edge → top scanline of probe glyph (µs, for CLI/stats).
// phase = VBLANK_US + Y * 18 * line_us  (VBLANK calibrated @ Y=8 → 10720).
uint16_t osdDemoRasterComputeCellTopPhaseUs(void)
{
    const uint32_t us = (uint32_t)OSD_DEMO_RASTER_VBLANK_US
        + (uint32_t)rasterY * (uint32_t)OSD_DEMO_RASTER_GLYPH_ROWS * (uint32_t)rasterLineUs;
    if (us > OSD_DEMO_RASTER_DELAY_MAX_US) {
        return OSD_DEMO_RASTER_DELAY_MAX_US;
    }
    return (uint16_t)us;
}

// Same instant as phase, but in HSYNC line counts (diag scheduling).
uint16_t osdDemoRasterComputeCellTopLine(void)
{
    if (rasterLineUs == 0) {
        return 0;
    }
    // Prefer current phase (auto or user-tuned) so `phase 10720` maps to lines.
    return (uint16_t)(rasterDelayUs / rasterLineUs);
}

uint16_t osdDemoRasterApplyAutoPhase(void)
{
    rasterPhaseAuto = true;
    rasterDelayUs = osdDemoRasterComputeCellTopPhaseUs();
    rasterPhaseTicks = clockMicrosToCycles(rasterDelayUs);
    osdDemoRasterClampPhaseToPeriod();
    return rasterDelayUs;
}

static uint16_t osdDemoCalClampDelay(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > (int32_t)OSD_DEMO_RASTER_DELAY_MAX_US) {
        return OSD_DEMO_RASTER_DELAY_MAX_US;
    }
    return (uint16_t)v;
}

static void osdDemoRasterCalSyncDynamic(void)
{
    calDynamicUs = osdDemoCalClampDelay((int32_t)calCenterUs + (int32_t)calOffsetUs);
    rasterDelayUs = calDynamicUs;
    // CAL sweeps phase; period stays independently tunable.
    rasterPhaseTicks = clockMicrosToCycles(calDynamicUs);
    osdDemoRasterClampPhaseToPeriod();
}

static void osdDemoRasterCalReset(void)
{
    calCenterUs = OSD_DEMO_CAL_CENTER_INIT_US;
    calRangeUs = OSD_DEMO_CAL_RANGE_INIT_US;
    calOffsetUs = 0;
    calSweepDir = 1;
    calSweepLastMs = millis();
    calSweepPhaseMs = OSD_DEMO_CAL_SWEEP_MS / 4u; // start at offset≈0 (mid up-ramp)
    calLastDynSlot = 0xFF;
    calUiForce = true;
    osdDemoRasterCalSyncDynamic();
}

static void osdDemoRasterCalCatch(void)
{
    // Visual match (probe == solid white refs) → lock centre, halve search window, keep sweeping.
    calCenterUs = calDynamicUs;
    uint16_t newRange = (uint16_t)(calRangeUs / 2u);
    if (newRange < OSD_DEMO_CAL_RANGE_MIN_US) {
        newRange = OSD_DEMO_CAL_RANGE_MIN_US;
    }
    calRangeUs = newRange;
    calOffsetUs = 0;
    calSweepDir = 1;
    calSweepLastMs = millis();
    // Keep phase at mid-ramp so we don't jump; sweep must keep moving even for small range.
    calSweepPhaseMs = OSD_DEMO_CAL_SWEEP_MS / 4u;
    calLastDynSlot = 0xFF;
    calUiForce = true;
    osdDemoRasterCalSyncDynamic();
    rasterStaticPainted = false;
}

static void osdDemoRasterCalCommit(void)
{
    // Lock delay and drop to plain GLYPH probe with tick engine.
    rasterDelayUs = calCenterUs;
    calDynamicUs = calCenterUs;
    calOffsetUs = 0;
    rasterPhaseTicks = clockMicrosToCycles(calCenterUs);
    osdDemoRasterClampPhaseToPeriod();
    rasterMode = OSD_DEMO_RASTER_GLYPH;
    rasterStaticPainted = false;
    osdDemoPaintRaster();
    max7456RefreshAll();
    rasterStaticPainted = true;
}


void osdDemoRasterCalGet(uint16_t *centerUs, uint16_t *rangeUs, uint16_t *dynamicUs)
{
    if (centerUs) {
        *centerUs = calCenterUs;
    }
    if (rangeUs) {
        *rangeUs = calRangeUs;
    }
    if (dynamicUs) {
        *dynamicUs = calDynamicUs;
    }
}

void osdDemoRasterCalEvent(uint8_t event)
{
    if (!active || fx != OSD_DEMO_FX_RASTER || rasterMode != OSD_DEMO_RASTER_CAL) {
        return;
    }
    if (event == 0) {
        osdDemoRasterCalCatch();
    } else if (event == 1) {
        osdDemoRasterCalCommit();
    } else {
        osdDemoRasterCalReset();
        rasterStaticPainted = false;
    }
}

static void osdDemoRasterCalAdvanceSweep(void)
{
    const timeMs_t now = millis();
    timeMs_t dt = (timeMs_t)(now - calSweepLastMs);
    if (dt == 0) {
        return;
    }
    calSweepLastMs = now;

    // Phase-based triangle — avoids integer step=0 when range is small (e.g. ±250).
    calSweepPhaseMs += (uint32_t)dt;
    while (calSweepPhaseMs >= OSD_DEMO_CAL_SWEEP_MS) {
        calSweepPhaseMs -= OSD_DEMO_CAL_SWEEP_MS;
    }

    const uint32_t half = OSD_DEMO_CAL_SWEEP_MS / 2u;
    const uint16_t range = (calRangeUs == 0) ? 1 : calRangeUs;
    int32_t offset;
    if (calSweepPhaseMs <= half) {
        // -range → +range over first half
        offset = -(int32_t)range
            + (int32_t)(((uint32_t)(2u * range) * calSweepPhaseMs) / half);
    } else {
        // +range → -range over second half
        const uint32_t t = calSweepPhaseMs - half;
        offset = (int32_t)range
            - (int32_t)(((uint32_t)(2u * range) * t) / half);
    }
    if (offset > (int32_t)range) {
        offset = (int32_t)range;
    }
    if (offset < -(int32_t)range) {
        offset = -(int32_t)range;
    }
    calOffsetUs = (int16_t)offset;
    calSweepDir = (calSweepPhaseMs <= half) ? 1 : -1;
    osdDemoRasterCalSyncDynamic();
}

// 5×5 font → 6×6 mega-pixels into tunnel 2×3 cell patterns (same packing as tunnel text).
static int8_t osdDemoCalTextFontRow(uint8_t cellY, uint8_t br, uint8_t textY0)
{
    if (cellY < textY0 || cellY >= (uint8_t)(textY0 + OSD_DEMO_CAL_TEXT_ROWS)) {
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

static void osdDemoPaintMpString(uint8_t textY0, const char *s, uint8_t cols, uint8_t rows)
{
    uint8_t len = 0;
    while (s[len] && len < 8) {
        len++;
    }
    const uint16_t totalMp = (uint16_t)(len * OSD_DEMO_CAL_DIGIT_ADV);
    const uint16_t gridMp = (uint16_t)(cols * 2u);
    int16_t startMx = (int16_t)((gridMp > totalMp) ? ((gridMp - totalMp) / 2) : 0);

    for (uint8_t cellY = textY0; cellY < (uint8_t)(textY0 + OSD_DEMO_CAL_TEXT_ROWS) && cellY < rows; cellY++) {
        for (uint8_t cellX = 0; cellX < cols; cellX++) {
            uint8_t pat = 0;
            for (uint8_t br = 0; br < 3; br++) {
                for (uint8_t bc = 0; bc < 2; bc++) {
                    const int8_t trow = osdDemoCalTextFontRow(cellY, br, textY0);
                    bool on = false;
                    if (trow >= 0) {
                        const int16_t mx = (int16_t)(cellX * 2u + bc);
                        const int16_t local = (int16_t)(mx - startMx);
                        if (local >= 0 && local < (int16_t)totalMp) {
                            const uint8_t gi = (uint8_t)(local / OSD_DEMO_CAL_DIGIT_ADV);
                            const uint8_t px = (uint8_t)(local % OSD_DEMO_CAL_DIGIT_ADV);
                            if (px < OSD_DEMO_FONT_W && gi < len) {
                                on = osdDemoFontPixel((uint8_t)s[gi], px, (uint8_t)(OSD_DEMO_TUNNEL_INK_Y0 + (uint8_t)trow));
                            }
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
}

static void osdDemoPaintCalScaleFixed(uint8_t y, uint8_t cols, uint8_t rows)
{
    if (y >= rows) {
        return;
    }
    for (uint8_t x = 0; x < cols; x++) {
        max7456WriteChar(x, y, OSD_DEMO_PIXEL_OFF);
    }
    // Reference caret only — nothing else on this row.
    max7456WriteChar((uint8_t)(cols / 2), y, OSD_DEMO_FILL_BASE);
}

static void osdDemoPaintCalScaleMoving(uint8_t y, uint8_t cols, uint8_t rows, int16_t markOffset, uint16_t range)
{
    if (y >= rows) {
        return;
    }
    for (uint8_t x = 0; x < cols; x++) {
        max7456WriteChar(x, y, OSD_DEMO_PIXEL_OFF);
    }
    if (range == 0) {
        range = 1;
    }
    const uint8_t mid = (uint8_t)(cols / 2);
    // Map ±range → full width; 3-cell-wide bright bar so travel is obvious.
    int32_t slot = (int32_t)mid + ((int32_t)markOffset * (int32_t)(cols / 2 - 2)) / (int32_t)range;
    if (slot < 1) {
        slot = 1;
    }
    if (slot > (int32_t)(cols - 2)) {
        slot = (int32_t)(cols - 2);
    }
    calLastDynSlot = (uint8_t)slot;
    max7456WriteChar((uint8_t)(slot - 1), y, OSD_DEMO_FILL_BASE);
    max7456WriteChar((uint8_t)slot, y, OSD_DEMO_FILL_BASE);
    max7456WriteChar((uint8_t)(slot + 1), y, OSD_DEMO_FILL_BASE);
}

static void osdDemoPaintRasterCal(void)
{
    if (!demoDisplay) {
        return;
    }
    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;
    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    max7456SetHudMotionOffset(0, 0);

    // Absolute delay (us) — mega-pixel 5×5@6×6
    char buf[6];
    uint16_t v = calDynamicUs;
    buf[0] = (char)('0' + (v / 10000) % 10);
    buf[1] = (char)('0' + (v / 1000) % 10);
    buf[2] = (char)('0' + (v / 100) % 10);
    buf[3] = (char)('0' + (v / 10) % 10);
    buf[4] = (char)('0' + (v % 10));
    buf[5] = 0;
    const uint8_t textY0 = 0;
    osdDemoPaintMpString(textY0, buf, cols, rows);

    // Scales directly under digits.
    const uint8_t scaleY0 = (uint8_t)(textY0 + OSD_DEMO_CAL_TEXT_ROWS + 1);
    osdDemoPaintCalScaleFixed(scaleY0, cols, rows);
    osdDemoPaintCalScaleMoving((uint8_t)(scaleY0 + 1), cols, rows, calOffsetUs, calRangeUs);

    // Probe row: [WHITE ref][gap][LIVE probe][gap][WHITE ref]
    // Target to catch = solid full white (no blink, no growing bar) — match the refs.
    rasterX = (uint8_t)(cols / 2);
    rasterY = (uint8_t)(scaleY0 + 3);
    if (rasterY >= rows) {
        rasterY = (uint8_t)(rows - 1);
    }
    const uint8_t refL = (uint8_t)((rasterX >= 3) ? (rasterX - 3) : 0);
    const uint8_t refR = (uint8_t)((rasterX + 3 < cols) ? (rasterX + 3) : (cols - 1));
    max7456WriteChar(refL, rasterY, OSD_DEMO_FILL_BASE);
    if (refL + 1 < rasterX) {
        max7456WriteChar((uint8_t)(refL + 1), rasterY, OSD_DEMO_FILL_BASE);
    }
    max7456WriteChar(refR, rasterY, OSD_DEMO_FILL_BASE);
    if (refR > 0 && refR - 1 > rasterX) {
        max7456WriteChar((uint8_t)(refR - 1), rasterY, OSD_DEMO_FILL_BASE);
    }

    const uint16_t addr = osdDemoRasterAddr();
    max7456WriteCharEx(rasterX, rasterY, rasterGlyphA, false);
    max7456CommitShadowCell(addr, rasterGlyphA, false);

    calUiForce = false;
}

// Static marker frame + resting glyph A. Layer/shadow stay on A so drawScreen
// will not fight the mid-pulse Display SRAM rewrite of B.
static void osdDemoPaintRaster(void)
{
    if (!demoDisplay) {
        return;
    }
    if (rasterMode == OSD_DEMO_RASTER_CAL) {
        osdDemoPaintRasterCal();
        return;
    }
    const uint8_t cols = demoDisplay->cols;
    const uint8_t rows = demoDisplay->rows;

    if (rasterX >= cols) {
        rasterX = (uint8_t)(cols / 2);
    }
    if (rasterY >= rows) {
        rasterY = (uint8_t)(rows / 2);
    }

    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    max7456SetHudMotionOffset(0, 0);

    if (rasterMode == OSD_DEMO_RASTER_MARK) {
        // Top-row marker cell only — no crosshair (keeps vertical compare clean).
        const uint16_t addr = (uint16_t)(OSD_DEMO_RASTER_MARK_Y * OSD_DEMO_CHARS_PER_LINE
                                         + OSD_DEMO_RASTER_MARK_X);
        max7456WriteCharEx(OSD_DEMO_RASTER_MARK_X, OSD_DEMO_RASTER_MARK_Y,
                           OSD_DEMO_PIXEL_OFF, false);
        max7456CommitShadowCell(addr, OSD_DEMO_PIXEL_OFF, false);
        return;
    }

    if (rasterMode == OSD_DEMO_RASTER_DIAG) {
        // Refs: growing dense hatch (phase for probe row).
        const uint8_t phase = (rasterY & 1u) ? (uint8_t)OSD_DEMO_DIAG_PHASE1 : 0;
        for (uint8_t i = 0; i < OSD_DEMO_DIAG_STEPS; i++) {
            const uint8_t gx = (uint8_t)(rasterX - OSD_DEMO_DIAG_STEPS - 1 + i);
            if (gx < cols) {
                max7456WriteCharEx(gx, rasterY,
                                   (uint8_t)(OSD_DEMO_STRIPE_BASE + phase + i), false);
            }
        }
        const uint16_t addr = osdDemoRasterAddr();
        max7456WriteCharEx(rasterX, rasterY, OSD_DEMO_PIXEL_OFF, false);
        max7456CommitShadowCell(addr, OSD_DEMO_PIXEL_OFF, false);
        return;
    }

    if (rasterMode == OSD_DEMO_RASTER_FILL) {
        // Fast 2×1 hatch (F7/FD row bursts) + white corner fiducials.
        osdDemoPaintHatch2x1Screen(cols, rows, true);
        max7456RefreshAll();
        return;
    }

    // Dim crosshair so the probe cell is obvious on USB capture.
    const uint8_t mark = (uint8_t)(OSD_DEMO_HFILL_BASE + 1); // 1px white tip
    for (uint8_t x = 0; x < cols; x++) {
        if (x == rasterX) {
            continue;
        }
        if ((x & 1) == 0) {
            max7456WriteChar(x, rasterY, mark);
        }
    }
    for (uint8_t y = 0; y < rows; y++) {
        if (y == rasterY) {
            continue;
        }
        if ((y & 1) == 0) {
            max7456WriteChar(rasterX, y, mark);
        }
    }

    const uint16_t addr = osdDemoRasterAddr();
    if (rasterMode == OSD_DEMO_RASTER_INVERT) {
        max7456WriteCharEx(rasterX, rasterY, rasterInvGlyph, false);
        max7456CommitShadowCell(addr, rasterInvGlyph, false);
    } else {
        max7456WriteCharEx(rasterX, rasterY, rasterGlyphA, false);
        max7456CommitShadowCell(addr, rasterGlyphA, false);
    }
}

// PAL period probe: full-cell WHITE blink every markInterval software fields.
// No mid-glyph strip — user measures real time between flashes on USB capture.
static void osdDemoRasterMarkSetCell(uint8_t glyph)
{
    const uint16_t addr = (uint16_t)(OSD_DEMO_RASTER_MARK_Y * OSD_DEMO_CHARS_PER_LINE
                                     + OSD_DEMO_RASTER_MARK_X);
    (void)max7456WriteDisplaySramChar(addr, glyph);
    max7456CommitShadowCell(addr, glyph, false);
}

static void osdDemoRasterMarkOnEvent(uint32_t eventIndex, uint32_t eventTick, uint32_t lateTicks)
{
    if (!markTestActive || markInterval == 0) {
        return;
    }
    if (lateTicks > markMaxLateTicks) {
        markMaxLateTicks = lateTicks;
    }
    if (lateTicks > 0) {
        markLateEvents++;
    }

    // Flash edge every N software fields (ideal grid).
    if ((eventIndex % markInterval) == 0) {
        if (markStartTick != 0 || markPairComplete) {
            // Previous flash → this flash = one measured interval.
            markEndEvent = eventIndex;
            markEndTick = eventTick;
            const uint32_t n = markInterval;
            markExpectedElapsedTicks = n * osdDemoRasterEffectivePeriodTicks();
            markElapsedErrorTicks = (int32_t)(markEndTick - markStartTick)
                - (int32_t)markExpectedElapsedTicks;
            markErrorPerEventTicks = (int32_t)markElapsedErrorTicks / (int32_t)n;
            markPairComplete = true;
            if (!markRepeat) {
                // Show this flash, then stop after hold.
            }
        }
        markStartEvent = eventIndex;
        markStartTick = eventTick;
    }

    const uint32_t phaseInInterval = eventIndex % markInterval;
    const bool lit = (phaseInInterval < OSD_DEMO_RASTER_MARK_FLASH_HOLD);
    osdDemoRasterMarkSetCell(lit ? rasterGlyphA : rasterGlyphB);

    if (!markRepeat && markPairComplete
        && eventIndex >= (markEndEvent + OSD_DEMO_RASTER_MARK_FLASH_HOLD)) {
        markTestActive = false;
    }
}

static void osdDemoRasterWriteA(void)
{
    const uint16_t addr = osdDemoRasterAddr();
    if (rasterMode == OSD_DEMO_RASTER_INVERT) {
        (void)max7456WriteDisplaySramChar(addr, rasterInvGlyph);
        (void)max7456WriteDisplaySramAttr(addr, 0x00);
        max7456CommitShadowCell(addr, rasterInvGlyph, false);
    } else {
        (void)max7456WriteDisplaySramChar(addr, rasterGlyphA);
        max7456CommitShadowCell(addr, rasterGlyphA, false);
    }
}

static void osdDemoRasterWriteB(void)
{
    const uint16_t addr = osdDemoRasterAddr();
    if (rasterMode == OSD_DEMO_RASTER_INVERT) {
        (void)max7456WriteDisplaySramAttr(addr, OSD_DEMO_RASTER_ATTR_INV);
        max7456CommitShadowCell(addr, rasterInvGlyph, false);
    } else {
        (void)max7456WriteDisplaySramChar(addr, rasterGlyphB);
        max7456CommitShadowCell(addr, rasterGlyphA, false);
    }
}

static void osdDemoRasterEngineInit(void)
{
    // DWT CYCCNT @ CPU clock — monotonic, independent of OSD scheduler.
    if (rasterDelayUs == 0) {
        rasterDelayUs = OSD_DEMO_RASTER_DEFAULT_PHASE_US;
    }
    if (!periodCorrSeeded) {
        periodCorrectionTicks = (int32_t)clockMicrosToCycles(OSD_DEMO_RASTER_CORR_DEFAULT_US);
        periodCorrSeeded = true;
    }
    osdDemoRasterClampPeriodCorrection();
    rasterPhaseTicks = clockMicrosToCycles(rasterDelayUs);
    osdDemoRasterClampPhaseToPeriod();
    // Ideal timeline only: next += period. Never rebase from CYCCNT.
    rasterFieldEpoch = getCycleCounter();
    rasterLateCount = 0;
    rasterMaxLateTicks = 0;
    vsyncLockCount = 0;
    vsyncTimeoutCount = 0;
    vsyncLastEdgeTicks = 0;
    rasterMeasuredLineTicks = 0;
    rasterFieldParity = 0;
    rasterEngineArmed = true;
}

static void osdDemoRasterWriteGlyphFast(uint8_t glyph)
{
    const uint16_t addr = osdDemoRasterAddr();
    (void)max7456WriteDisplaySramChar(addr, glyph);
    max7456CommitShadowCell(addr, glyph, false);
}

static void osdDemoRasterWaitUntil(uint32_t deadlineTicks)
{
    while ((int32_t)(getCycleCounter() - deadlineTicks) < 0) {
    }
}

// Resync every PAL field from MAX7456 STAT[4] — no long-term period drift.
static void osdDemoRasterVsyncEnginePoll(void)
{
    const bool diag = (rasterMode == OSD_DEMO_RASTER_DIAG);
    const bool fill = (rasterMode == OSD_DEMO_RASTER_FILL);
    const uint32_t leadTicks = clockMicrosToCycles(rasterSpiLeadUs);
    const uint32_t fieldMin = clockMicrosToCycles(15000);
    const uint32_t fieldMax = clockMicrosToCycles(25000);

    while (active && rasterEngineArmed && !ARMING_FLAG(ARMED)) {
        uint32_t edgeTicks = 0;
        if (!max7456WaitVsyncFallingEdge(&edgeTicks, OSD_DEMO_RASTER_VSYNC_TIMEOUT_US)) {
            vsyncTimeoutCount++;
#ifdef USE_CLI
            if (cliMode) {
                (void)cliProcess();
            }
#endif
            continue;
        }

        // Stats only — diag/fill step with nominal line_us (phase units).
        if (vsyncLastEdgeTicks != 0) {
            const uint32_t fieldTicks = edgeTicks - vsyncLastEdgeTicks;
            if (fieldTicks > fieldMin && fieldTicks < fieldMax) {
                rasterMeasuredLineTicks = (fieldTicks * 2u) / (uint32_t)OSD_DEMO_RASTER_PAL_FIELD_LINES_X2;
            }
        }

        vsyncLockCount++;
        vsyncLastEdgeTicks = edgeTicks;
        rasterEventCounter++;
        rasterFieldParity ^= 1;

        if (diag) {
            // Dense 2px diagonal hatch, cumulative mid-glyph in one probe cell.
            const uint32_t lineTicks = clockMicrosToCycles(rasterLineUs);
            const uint32_t stepTicks = lineTicks * (uint32_t)OSD_DEMO_DIAG_CUBE_PX;
            const uint32_t fieldBias = rasterFieldParity ? (lineTicks / 2u) : 0;
            const uint32_t band0 = edgeTicks + rasterPhaseTicks + fieldBias;
            const uint8_t phase = (rasterY & 1u) ? (uint8_t)OSD_DEMO_DIAG_PHASE1 : 0;
            for (uint8_t i = 0; i < OSD_DEMO_DIAG_STEPS; i++) {
                const uint32_t due = band0 + (uint32_t)i * stepTicks;
                uint32_t wrAt = due - leadTicks;
                if ((int32_t)(wrAt - edgeTicks) < 0) {
                    wrAt = edgeTicks;
                }
                osdDemoRasterWaitUntil(wrAt);
                osdDemoRasterWriteGlyphFast((uint8_t)(OSD_DEMO_STRIPE_BASE + phase + i));
            }
        } else if (fill) {
            // Hatch is static. RB0..15 is per character ROW only — cannot vary
            // brightness left↔right; live RB not useful for horizontal split.
        } else {
            // Classic two-step A→B mid-glyph probe (DWT phase).
            osdDemoRasterWriteA();
            osdDemoRasterWaitUntil(edgeTicks + rasterPhaseTicks);
            osdDemoRasterWriteB();
        }

#ifdef USE_CLI
        if (cliMode) {
            (void)cliProcess();
        }
#endif
    }
}

static void osdDemoRasterEnginePoll(void)
{
    if (!rasterEngineArmed) {
        return;
    }

    if (rasterMode == OSD_DEMO_RASTER_VSYNC || rasterMode == OSD_DEMO_RASTER_DIAG
        || rasterMode == OSD_DEMO_RASTER_FILL) {
        osdDemoRasterVsyncEnginePoll();
        return;
    }

    const uint32_t lateSkipTicks = clockMicrosToCycles(OSD_DEMO_RASTER_LATE_SKIP_US);

    // OSD task is ~12 Hz — never fire SPI from "next task tick".
    // Deterministic DWT busy-wait across fields; pump CLI so stop/tune work.
    while (active && rasterEngineArmed && !ARMING_FLAG(ARMED)) {
        while (max7456DmaInProgress()) {
            if (!active || ARMING_FLAG(ARMED)) {
                return;
            }
        }

        const uint32_t period = osdDemoRasterEffectivePeriodTicks();
        uint32_t now = getCycleCounter();

        uint8_t skipped = 0;
        while ((int32_t)(now - (rasterFieldEpoch + period)) >= 0) {
            rasterFieldEpoch += period;
            rasterLateCount++;
            rasterEventCounter++;
            if (markTestActive) {
                markLateEvents++;
                const uint32_t late = now - (rasterFieldEpoch - period);
                if (late > markMaxLateTicks) {
                    markMaxLateTicks = late;
                }
            }
            if (++skipped >= 8) {
                break;
            }
        }

        const uint32_t aAt = rasterFieldEpoch;
        const uint32_t bAt = rasterFieldEpoch + rasterPhaseTicks;

        if ((int32_t)(now - aAt) < 0) {
            while ((int32_t)(getCycleCounter() - aAt) < 0) {
            }
            now = getCycleCounter();
        }

        const uint32_t lateA = now - aAt;
        if (rasterMode == OSD_DEMO_RASTER_MARK) {
            // Blink needs only aAt — do not require the mid-glyph bAt window.
            if ((int32_t)(now - (aAt + period)) >= 0) {
                if (lateA > rasterMaxLateTicks) {
                    rasterMaxLateTicks = lateA;
                }
                rasterLateCount++;
                if (markTestActive) {
                    markLateEvents++;
                    if (lateA > markMaxLateTicks) {
                        markMaxLateTicks = lateA;
                    }
                }
                rasterFieldEpoch += period;
                rasterEventCounter++;
#ifdef USE_CLI
                if (cliMode) {
                    (void)cliProcess();
                }
#endif
                continue;
            }
        } else if ((int32_t)(now - bAt) >= 0 || lateA > lateSkipTicks) {
            if (lateA > rasterMaxLateTicks) {
                rasterMaxLateTicks = lateA;
            }
            rasterLateCount++;
            if (markTestActive) {
                markLateEvents++;
                if (lateA > markMaxLateTicks) {
                    markMaxLateTicks = lateA;
                }
            }
            // Keep ideal grid — do not rebase from now.
            rasterFieldEpoch += period;
            rasterEventCounter++;
#ifdef USE_CLI
            if (cliMode) {
                (void)cliProcess();
            }
#endif
            continue;
        }

        if (lateA > rasterMaxLateTicks) {
            rasterMaxLateTicks = lateA;
        }

        const uint32_t eventIndex = rasterEventCounter;

        if (rasterMode == OSD_DEMO_RASTER_MARK) {
            // Simple full-cell blink on the ideal grid — no mid-glyph phase wait.
            if (markTestActive) {
                osdDemoRasterMarkOnEvent(eventIndex, aAt, lateA);
            }
        } else {
            osdDemoRasterWriteA();
            while ((int32_t)(getCycleCounter() - bAt) < 0) {
            }
            now = getCycleCounter();
            {
                const uint32_t lateB = now - bAt;
                if (lateB > rasterMaxLateTicks) {
                    rasterMaxLateTicks = lateB;
                }
            }
            osdDemoRasterWriteB();

            if (rasterMode == OSD_DEMO_RASTER_SWEEP) {
                if (rasterDelayUs >= OSD_DEMO_RASTER_DELAY_MAX_US) {
                    rasterDelayUs = 0;
                } else {
                    rasterDelayUs = (uint16_t)(rasterDelayUs + OSD_DEMO_RASTER_SWEEP_STEP_US);
                }
                rasterPhaseTicks = clockMicrosToCycles(rasterDelayUs);
                osdDemoRasterClampPhaseToPeriod();
            }
        }

        rasterFieldEpoch += period;
        rasterEventCounter++;

#ifdef USE_CLI
        if (cliMode) {
            (void)cliProcess();
        }
#endif
        if (rasterMode == OSD_DEMO_RASTER_CAL) {
            const timeUs_t t = micros();
            const timeDelta_t uiUs = 1000000 / OSD_DEMO_SCROLL_HZ;
            if ((int32_t)(t - lastStepUs) >= uiUs) {
                lastStepUs = t;
                osdDemoRasterCalAdvanceSweep();
                osdDemoPaintRasterCal();
                rasterStaticPainted = true;
            }
        }
    }
}

bool osdDemoStartPlasma2x2(void)
{
    if (ARMING_FLAG(ARMED)) {
        return false;
    }
    if (!active) {
        if (!osdDemoStart()) {
            return false;
        }
    }
    {
        const osdDemoFx_e next = OSD_DEMO_FX_PLASMA2X2;
        osdDemoEnterFx(next);
    }
    return true;
}

bool osdDemoStartHosTest(void) __attribute__((noinline));

bool osdDemoStartHosTest(void)
{
    if (ARMING_FLAG(ARMED)) {
        return false;
    }
    if (!active) {
        if (!osdDemoStart()) {
            return false;
        }
    }
    // Already in the probe: allow `hostest line <n>` retarget without re-entering
    // (EnginePoll may be the caller via cliProcess — must not nest MidGlyph/EnterFx).
    if (fx == OSD_DEMO_FX_HOSTEST && hosTestArmed) {
        return true;
    }
    // Explicit local so LTO cannot feed EnterFx a stale r0 (seen on this tree before).
    const osdDemoFx_e next = OSD_DEMO_FX_HOSTEST;
    osdDemoEnterFx(next);
    return true;
}

void osdDemoHosTestSetLine(uint16_t lineFromRow0)
{
    // PAL active OSD ≈ 16*18 = 288 lines; clamp loosely for NTSC too.
    if (lineFromRow0 > 400) {
        lineFromRow0 = 400;
    }
    hosTestLine = lineFromRow0;
}

uint16_t osdDemoHosTestGetLine(void)
{
    return hosTestLine;
}

bool osdDemoStartRaster(uint8_t mode)
{
    if (mode > (uint8_t)OSD_DEMO_RASTER_FILL) {
        mode = (uint8_t)OSD_DEMO_RASTER_GLYPH;
    }
    rasterMode = (osdDemoRasterMode_e)mode;
    if (rasterMode == OSD_DEMO_RASTER_CAL) {
        osdDemoRasterCalReset();
    } else if (rasterMode == OSD_DEMO_RASTER_DIAG) {
        osdDemoRasterApplyAutoPhase();
    } else if (rasterMode == OSD_DEMO_RASTER_FILL) {
        // Phase = top of character row 0 (VBLANK), not centre-cell.
        // Extra SPI lead: full-row burst needs to start before the 2px band.
        rasterPhaseAuto = true;
        rasterDelayUs = OSD_DEMO_RASTER_VBLANK_US;
        if (rasterDelayUs > OSD_DEMO_RASTER_DELAY_MAX_US) {
            rasterDelayUs = OSD_DEMO_RASTER_DELAY_MAX_US;
        }
        rasterPhaseTicks = clockMicrosToCycles(rasterDelayUs);
        osdDemoRasterClampPhaseToPeriod();
        if (rasterSpiLeadUs < 40) {
            rasterSpiLeadUs = 40;
        }
    } else if (rasterDelayUs == 0) {
        rasterDelayUs = OSD_DEMO_RASTER_DEFAULT_PHASE_US;
        rasterPhaseAuto = false;
    }
    if (rasterMode != OSD_DEMO_RASTER_MARK) {
        markTestActive = false;
    }
    if (!active) {
        if (!osdDemoStart()) {
            return false;
        }
    }
    osdDemoEnterFx(OSD_DEMO_FX_RASTER);
    osdDemoRasterEngineInit();
    return true;
}

void osdDemoRasterSetDelayUs(uint16_t delayUs)
{
    if (delayUs > OSD_DEMO_RASTER_DELAY_MAX_US) {
        delayUs = OSD_DEMO_RASTER_DELAY_MAX_US;
    }
    rasterPhaseAuto = false;
    rasterDelayUs = delayUs;
    rasterPhaseTicks = clockMicrosToCycles(delayUs);
    osdDemoRasterClampPhaseToPeriod();
    if (rasterMode == OSD_DEMO_RASTER_SWEEP) {
        rasterMode = OSD_DEMO_RASTER_GLYPH;
    }
    if (rasterMode == OSD_DEMO_RASTER_CAL) {
        calCenterUs = delayUs;
        calOffsetUs = 0;
        osdDemoRasterCalSyncDynamic();
        rasterStaticPainted = false;
    }
}

void osdDemoRasterAdjustPeriodCorrectionTicks(int32_t deltaTicks)
{
    periodCorrectionTicks += deltaTicks;
    periodCorrSeeded = true;
    osdDemoRasterClampPeriodCorrection();
    osdDemoRasterClampPhaseToPeriod();
}

void osdDemoRasterSetPeriodCorrectionTicks(int32_t ticks)
{
    periodCorrectionTicks = ticks;
    periodCorrSeeded = true;
    osdDemoRasterClampPeriodCorrection();
    osdDemoRasterClampPhaseToPeriod();
}

void osdDemoRasterAdjustPhaseTicks(int32_t deltaTicks)
{
    rasterPhaseAuto = false;
    int32_t ph = (int32_t)rasterPhaseTicks + deltaTicks;
    if (ph < 0) {
        ph = 0;
    }
    rasterPhaseTicks = (uint32_t)ph;
    osdDemoRasterClampPhaseToPeriod();
    rasterDelayUs = (uint16_t)clockCyclesToMicros((int32_t)rasterPhaseTicks);
    if (rasterMode == OSD_DEMO_RASTER_CAL) {
        calCenterUs = rasterDelayUs;
        calOffsetUs = 0;
        calDynamicUs = rasterDelayUs;
    }
}

void osdDemoRasterSetPhaseTicks(uint32_t ticks)
{
    rasterPhaseTicks = ticks;
    rasterPhaseAuto = false;
    {
        // Clamp + sync delay_us without re-clearing auto via AdjustPhaseTicks path noise.
        osdDemoRasterClampPhaseToPeriod();
        rasterDelayUs = (uint16_t)clockCyclesToMicros((int32_t)rasterPhaseTicks);
        if (rasterMode == OSD_DEMO_RASTER_CAL) {
            calCenterUs = rasterDelayUs;
            calOffsetUs = 0;
            calDynamicUs = rasterDelayUs;
        }
    }
}

void osdDemoRasterGetTiming(uint32_t *periodTicks, uint32_t *phaseTicks,
                            uint32_t *timerHz, uint32_t *lateCount, uint32_t *maxLateTicks,
                            int32_t *corrTicks)
{
    if (periodTicks) {
        *periodTicks = osdDemoRasterEffectivePeriodTicks();
    }
    if (phaseTicks) {
        *phaseTicks = rasterPhaseTicks;
    }
    if (timerHz) {
        *timerHz = clockMicrosToCycles(1000000);
    }
    if (lateCount) {
        *lateCount = rasterLateCount;
    }
    if (maxLateTicks) {
        *maxLateTicks = rasterMaxLateTicks;
    }
    if (corrTicks) {
        *corrTicks = periodCorrectionTicks;
    }
}

void osdDemoRasterSetLineUs(uint16_t lineUs)
{
    if (lineUs < 50) {
        lineUs = 50;
    }
    if (lineUs > 80) {
        lineUs = 80;
    }
    rasterLineUs = lineUs;
    if (rasterPhaseAuto) {
        osdDemoRasterApplyAutoPhase();
    }
}

void osdDemoRasterSetSpiLeadUs(uint16_t leadUs)
{
    if (leadUs > 200) {
        leadUs = 200;
    }
    rasterSpiLeadUs = leadUs;
}

uint16_t osdDemoRasterGetSpiLeadUs(void)
{
    return rasterSpiLeadUs;
}

void osdDemoRasterGetVsyncStats(uint32_t *locks, uint32_t *timeouts, uint32_t *lastEdge)
{
    if (locks) {
        *locks = vsyncLockCount;
    }
    if (timeouts) {
        *timeouts = vsyncTimeoutCount;
    }
    if (lastEdge) {
        *lastEdge = vsyncLastEdgeTicks;
    }
}

uint32_t osdDemoRasterGetMeasuredLineTicks(void)
{
    return rasterMeasuredLineTicks;
}

void osdDemoRasterSetMarkInterval(uint16_t n)
{
    markInterval = n ? n : 1;
}

void osdDemoRasterSetMarkRepeat(bool enabled)
{
    markRepeat = enabled;
}

bool osdDemoRasterMarkStart(void)
{
    if (!osdDemoStartRaster((uint8_t)OSD_DEMO_RASTER_MARK)) {
        return false;
    }
    rasterEventCounter = 0;
    markLateEvents = 0;
    markMaxLateTicks = 0;
    markPairComplete = false;
    markStartEvent = 0;
    markEndEvent = 0;
    markStartTick = 0;
    markEndTick = 0;
    markExpectedElapsedTicks = 0;
    markElapsedErrorTicks = 0;
    markErrorPerEventTicks = 0;
    markTestActive = true;
    rasterStaticPainted = false;
    return true;
}

bool osdDemoRasterCalibrate(uint16_t n)
{
    osdDemoRasterSetMarkInterval(n ? n : OSD_DEMO_RASTER_MARK_INTERVAL_DEFAULT);
    markRepeat = true;
    return osdDemoRasterMarkStart();
}

void osdDemoRasterGetMarkStats(uint32_t *eventCount, uint16_t *interval, bool *repeat,
                               bool *activeTest, bool *pairDone,
                               uint32_t *startEvent, uint32_t *endEvent,
                               uint32_t *startTick, uint32_t *endTick,
                               uint32_t *expectedTicks, int32_t *elapsedErrorTicks,
                               int32_t *errorPerEventTicks,
                               uint32_t *markLate, uint32_t *markMaxLate)
{
    if (eventCount) {
        *eventCount = rasterEventCounter;
    }
    if (interval) {
        *interval = markInterval;
    }
    if (repeat) {
        *repeat = markRepeat;
    }
    if (activeTest) {
        *activeTest = markTestActive;
    }
    if (pairDone) {
        *pairDone = markPairComplete;
    }
    if (startEvent) {
        *startEvent = markStartEvent;
    }
    if (endEvent) {
        *endEvent = markEndEvent;
    }
    if (startTick) {
        *startTick = markStartTick;
    }
    if (endTick) {
        *endTick = markEndTick;
    }
    if (expectedTicks) {
        *expectedTicks = markExpectedElapsedTicks;
    }
    if (elapsedErrorTicks) {
        *elapsedErrorTicks = markElapsedErrorTicks;
    }
    if (errorPerEventTicks) {
        *errorPerEventTicks = markErrorPerEventTicks;
    }
    if (markLate) {
        *markLate = markLateEvents;
    }
    if (markMaxLate) {
        *markMaxLate = markMaxLateTicks;
    }
}

void osdDemoRasterSetCell(uint8_t x, uint8_t y)
{
    rasterX = x;
    rasterY = y;
    if (rasterPhaseAuto) {
        osdDemoRasterApplyAutoPhase();
    }
    if (active && fx == OSD_DEMO_FX_RASTER && rasterMode != OSD_DEMO_RASTER_CAL) {
        rasterStaticPainted = false;
        osdDemoPaintRaster();
        max7456RefreshAll();
        rasterStaticPainted = true;
    }
}

void osdDemoRasterSetGlyphs(uint8_t glyphA, uint8_t glyphB)
{
    rasterGlyphA = glyphA;
    rasterGlyphB = glyphB;
    if (active && fx == OSD_DEMO_FX_RASTER && rasterMode != OSD_DEMO_RASTER_INVERT) {
        rasterStaticPainted = false;
        osdDemoPaintRaster();
        max7456RefreshAll();
        rasterStaticPainted = true;
    }
}

void osdDemoRasterGetStatus(uint8_t *mode, uint8_t *x, uint8_t *y,
                            uint16_t *delayUs, uint8_t *glyphA, uint8_t *glyphB)
{
    if (mode) {
        *mode = (uint8_t)rasterMode;
    }
    if (x) {
        *x = rasterX;
    }
    if (y) {
        *y = rasterY;
    }
    if (delayUs) {
        *delayUs = (rasterMode == OSD_DEMO_RASTER_CAL) ? calDynamicUs : rasterDelayUs;
    }
    if (glyphA) {
        *glyphA = (rasterMode == OSD_DEMO_RASTER_INVERT) ? rasterInvGlyph : rasterGlyphA;
    }
    if (glyphB) {
        *glyphB = rasterGlyphB;
    }
}

bool osdDemoStart(void)
{
    startLastError = NULL;

    if (active) {
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
    // Explicit local — LTO has reused a stale r0 for EnterFx on this tree before.
    {
        const osdDemoFx_e next = OSD_DEMO_FX_SCROLLER;
        osdDemoEnterFx(next);
    }

    max7456FillScreen(OSD_DEMO_PIXEL_OFF);
    osdDemoPaintScroller();
    max7456RefreshAll();
#ifdef USE_CHIPTUNE
    chiptuneSchedulerPark();
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
    rasterEngineArmed = false;
    plasma2x2Armed = false;
    hosTestArmed = false;
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
#ifdef USE_CHIPTUNE
    chiptuneSchedulerUnpark();
#endif
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
    if (fx == OSD_DEMO_FX_SCROLLER) {
        if (elapsed >= OSD_DEMO_FX_SCROLLER_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_PLASMA;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_PLASMA) {
        if (elapsed >= OSD_DEMO_FX_PLASMA_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_FIRE;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_FIRE) {
        if (elapsed >= OSD_DEMO_FX_FIRE_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_WIPE;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_WIPE) {
        if (elapsed >= OSD_DEMO_FX_WIPE_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_TUNNEL;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_TUNNEL) {
        if (elapsed >= OSD_DEMO_FX_TUNNEL_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_PLASMA2X2;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_PLASMA2X2) {
        if (elapsed >= OSD_DEMO_FX_PLASMA2X2_MS) {
            const osdDemoFx_e next = OSD_DEMO_FX_SCROLLER;
            osdDemoEnterFx(next);
            return;
        }
    } else if (fx == OSD_DEMO_FX_RASTER || fx == OSD_DEMO_FX_HOSTEST) {
        // Hold forever until CLI stop / mode change — never auto-cycle.
    }

    // Raster / 2×2 plasma / HOS probe use DWT+VSYNC engines — poll every heartbeat.
    if (fx == OSD_DEMO_FX_RASTER) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoRasterEnginePoll();
        if (rasterMode == OSD_DEMO_RASTER_CAL) {
            // UI refresh ~SCROLL_HZ; engine already polled above.
            const timeDelta_t uiUs = 1000000 / OSD_DEMO_SCROLL_HZ;
            if ((int32_t)(currentTimeUs - lastStepUs) >= uiUs) {
                lastStepUs = currentTimeUs;
                osdDemoRasterCalAdvanceSweep();
                osdDemoPaintRasterCal();
                rasterStaticPainted = true;
            }
        } else if (!rasterStaticPainted) {
            osdDemoPaintRaster();
            rasterStaticPainted = true;
        }
        return;
    }
    if (fx == OSD_DEMO_FX_PLASMA2X2) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoPlasma2x2EnginePoll();
        return;
    }
    if (fx == OSD_DEMO_FX_HOSTEST) {
        if (ARMING_FLAG(ARMED)) {
            osdDemoStop();
            return;
        }
        osdDemoHosTestEnginePoll();
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
#ifdef USE_CHIPTUNE
    beeperPwmAyFifoFill();
#endif
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

#ifdef USE_CHIPTUNE
    beeperPwmAyFifoFill();
#endif
    // Soft-scroll (no wrap): early HOS. Wrap: HOS stays deferred until pass end.
    osdDemoSyncFlush(true, true);
}

#endif // USE_MAX7456
