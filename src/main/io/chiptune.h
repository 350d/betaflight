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

#ifdef USE_CHIPTUNE

bool chiptuneStart(void);
// dwellMs: 0 = sweep 2/4/8/12 ms; else fixed dwell for A4-C5-E5 arpeggio test
bool chiptuneStartTest(uint16_t dwellMs);
void chiptuneStop(void);
bool chiptuneIsPlaying(void);
bool chiptuneSetDwellMs(uint16_t dwellMs);
uint16_t chiptuneGetDwellMs(void);

// Tracker / test sequencer (TASK_BEEPER ~100 Hz)
void chiptuneUpdate(timeUs_t currentTimeUs);
// Monophonic arpeggio stepper (TASK_CHIPTUNE, fast)
void chiptuneArpUpdate(timeUs_t currentTimeUs);

#endif // USE_CHIPTUNE
