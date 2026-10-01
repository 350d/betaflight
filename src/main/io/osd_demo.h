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

bool osdDemoStartPlasma2x2(void); // scene 7 — 2×2 mid-glyph plasma
// Mid-scanline HOS poke: vertical white bar, HOS=32 → HOS=40 at mid of one line.
// Tests whether HOS is realtime / next-line / next-field (Display timing only).
bool osdDemoStartHosTest(void);
void osdDemoHosTestSetLine(uint16_t lineFromRow0);
uint16_t osdDemoHosTestGetLine(void);
bool osdDemoStartRaster(uint8_t mode);
void osdDemoRasterSetDelayUs(uint16_t delayUs);
void osdDemoRasterSetCell(uint8_t x, uint8_t y);
void osdDemoRasterSetGlyphs(uint8_t glyphA, uint8_t glyphB);
void osdDemoRasterGetStatus(uint8_t *mode, uint8_t *x, uint8_t *y,
                            uint16_t *delayUs, uint8_t *glyphA, uint8_t *glyphB);
void osdDemoRasterCalEvent(uint8_t event);
void osdDemoRasterCalGet(uint16_t *centerUs, uint16_t *rangeUs, uint16_t *dynamicUs);

// Tick-based PAL software oscillator (DWT CYCCNT).
// Nominal period is fixed (2160000 ticks @ 108 MHz). Only fine periodCorrection
// is tunable. A/B SPI fires on deterministic DWT deadlines inside the engine loop.
void osdDemoRasterAdjustPeriodCorrectionTicks(int32_t deltaTicks);
void osdDemoRasterSetPeriodCorrectionTicks(int32_t ticks);
void osdDemoRasterAdjustPhaseTicks(int32_t deltaTicks);
void osdDemoRasterSetPhaseTicks(uint32_t ticks);
void osdDemoRasterGetTiming(uint32_t *periodTicks, uint32_t *phaseTicks,
                            uint32_t *timerHz, uint32_t *lateCount, uint32_t *maxLateTicks,
                            int32_t *corrTicks);

void osdDemoRasterSetMarkInterval(uint16_t n);
void osdDemoRasterSetMarkRepeat(bool enabled);
bool osdDemoRasterMarkStart(void);
bool osdDemoRasterCalibrate(uint16_t n);
void osdDemoRasterGetMarkStats(uint32_t *eventCount, uint16_t *interval, bool *repeat,
                               bool *activeTest, bool *pairDone,
                               uint32_t *startEvent, uint32_t *endEvent,
                               uint32_t *startTick, uint32_t *endTick,
                               uint32_t *expectedTicks, int32_t *elapsedErrorTicks,
                               int32_t *errorPerEventTicks,
                               uint32_t *markLate, uint32_t *markMaxLate);
void osdDemoRasterGetVsyncStats(uint32_t *locks, uint32_t *timeouts, uint32_t *lastEdge);
uint32_t osdDemoRasterGetMeasuredLineTicks(void);
void osdDemoRasterSetLineUs(uint16_t lineUs);
void osdDemoRasterSetSpiLeadUs(uint16_t leadUs);
uint16_t osdDemoRasterGetSpiLeadUs(void);
uint16_t osdDemoRasterComputeCellTopPhaseUs(void);
uint16_t osdDemoRasterComputeCellTopLine(void);
uint16_t osdDemoRasterApplyAutoPhase(void);

#endif
