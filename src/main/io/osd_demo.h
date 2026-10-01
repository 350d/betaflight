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
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "common/time.h"

#ifdef USE_MAX7456

bool osdDemoStart(void);
const char *osdDemoStartLastError(void); // set when Start returns false
void osdDemoStop(void);
bool osdDemoIsActive(void);
void osdDemoUpdate(timeUs_t currentTimeUs);

// Run one scene permanently, no auto-cycle: 1 scroller, 2 plasma, 3 fire, 4 wipe, 5 tunnel,
// 7 plasma 2×2, 8 twister. `osd_demo` / `play` returns to the cycle.
bool osdDemoStartScene(uint8_t scene);
bool osdDemoStartPlasma2x2(void); // scene 7 — 2×2 mid-glyph plasma
// Scene 8 — classic vertical B/W ribbon twister (glyph width + HOS sway).
bool osdDemoStartTwister(void);
typedef struct osdDemoTwisterStats_s {
    uint32_t fields;
    uint32_t writes;       // bands actually sent (dirty cells only)
    uint32_t skips;        // bands dropped because the burst could not finish before the beam
    uint16_t maxBytes;     // largest single SPI burst
    uint32_t lineQ16;      // measured line period, DWT ticks << 16
    uint32_t byteTicksQ8;  // learned SPI cost per byte, DWT ticks << 8
    uint32_t cyclesPerUs;
    uint16_t skipRow[16];  // skips per character row — shows where on screen bands are lost
    uint32_t fieldTicks;   // last raw VSYNC→VSYNC
    uint32_t idleTicks;    // total wait-for-beam ticks since last reset
    uint32_t bytes;        // total SPI bytes of all bursts since last reset
    int32_t phaseField1;   // HSYNC phase vs model, field 1 (ticks)
    int32_t phaseField2;   // same, field 2 — expect ~½ line apart
    uint32_t slowBursts;   // bursts stretched by an IRQ (ignored by the SPI cost learner)
    uint32_t fieldReject;  // fields outside ±0.5% of 20 ms (not used for line period)
    uint32_t hsyncMiss;
    bool hsyncLock;
    int32_t rowCorrTicks[16]; // measured beam phase per char row vs row 1
} osdDemoTwisterStats_t;
void osdDemoTwisterGetStats(osdDemoTwisterStats_t *st, bool reset);
// Scene 8 timing model: lines per field ×2 (624/625/626) and a constant write shift (us).
void osdDemoTwisterSetFieldHalfLines(uint16_t halfLines);
uint16_t osdDemoTwisterGetFieldHalfLines(void);
// Interlace pairing: 0/1 = advance once per frame on even/odd field, 2 = every field.
// HSYNC re-lock once per char row (default on).
void osdDemoTwisterSetHsyncLock(bool enable);
bool osdDemoTwisterGetHsyncLock(void);
void osdDemoTwisterSetPair(uint8_t mode);
uint8_t osdDemoTwisterGetPair(void);
// Timing ruler instead of the twister (zig-zag bar, 1.67 px/line).
void osdDemoTwisterSetTest(bool zigzag);
bool osdDemoTwisterGetTest(void);
void osdDemoTwisterSetFreeze(bool freeze);
bool osdDemoTwisterGetFreeze(void);
void osdDemoTwisterGetPhases(uint8_t *rot, uint8_t *bendA, uint8_t *bendB);
// Scene 7 race-the-beam: HSYNC edge → first OSD pixel (us), tunable live.
void osdDemoMgSetBeamX0Us(int16_t us);
int16_t osdDemoMgGetBeamX0Us(void);
void osdDemoTwisterSetShiftUs(int16_t us);
int16_t osdDemoTwisterGetShiftUs(void);
#endif
