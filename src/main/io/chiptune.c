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
 * Experimental timer-backed PSG-style player for passive piezo.
 *
 * Monophonic hardware square wave: one frequency at a time.
 * "Polyphony" is tracker-style rapid arpeggio (change ARR only at
 * note boundaries; tone runs continuously during each dwell slot).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#ifdef USE_CHIPTUNE

#include "build/debug.h"

#include "common/maths.h"
#include "common/utils.h"

#include "drivers/sound_beeper.h"
#include "drivers/time.h"

#include "fc/runtime_config.h"

#include "io/beeper.h"
#include "io/chiptune.h"

#include "scheduler/scheduler.h"

#define CHIPTUNE_ENGINE_HZ          50
#define CHIPTUNE_TICKS_PER_ROW      6

#define CHIPTUNE_NOTE_MIN           36
#define CHIPTUNE_NOTE_MAX           96
#define CHIPTUNE_NOTE_COUNT         (CHIPTUNE_NOTE_MAX - CHIPTUNE_NOTE_MIN + 1)

#define CHIPTUNE_CH_COUNT           3
#define CHIPTUNE_PATTERN_LEN        16
#define CHIPTUNE_ARP_MAX_NOTES      4

#define CHIPTUNE_DRUM_NONE          0
#define CHIPTUNE_DRUM_KICK          1
#define CHIPTUNE_DRUM_SNARE         2
#define CHIPTUNE_DRUM_HAT           3

#define CHIPTUNE_ARP_OFF            0
#define CHIPTUNE_ARP_MIN            1
#define CHIPTUNE_ARP_MAJ            2

#define CHIPTUNE_DWELL_DEFAULT_MS   20
#define CHIPTUNE_DWELL_MIN_MS       1
#define CHIPTUNE_DWELL_MAX_MS       100

#define CHIPTUNE_TEST_A4            440
#define CHIPTUNE_TEST_C5            523
#define CHIPTUNE_TEST_E5            659
#define CHIPTUNE_TEST_SECTION_MS    3000
#define CHIPTUNE_TEST_GAP_MS        400
#define CHIPTUNE_TEST_BEEP_MS       120
#define CHIPTUNE_TEST_BEEP_GAP_MS   80

typedef enum {
    MODE_DEMO = 0,
    MODE_TEST,
} chiptuneMode_e;

typedef enum {
    TEST_MARK_BEEPS = 0,
    TEST_GAP,
    TEST_ARP,
    TEST_END_GAP,
} chiptuneTestPhase_e;

typedef struct {
    uint8_t note;
    uint8_t vol;
} chiptuneNote_t;

typedef struct {
    chiptuneNote_t ch[CHIPTUNE_CH_COUNT];
    uint8_t drum;
    uint8_t arp;
} chiptuneRow_t;

static volatile bool playing;
static chiptuneMode_e mode;

static uint16_t dwellMs = CHIPTUNE_DWELL_DEFAULT_MS;
static uint16_t arpFreqs[CHIPTUNE_ARP_MAX_NOTES];
static uint8_t arpVols[CHIPTUNE_ARP_MAX_NOTES];
static uint8_t arpCount;
static uint8_t arpIndex;
static uint16_t currentToneHz;
static timeUs_t nextArpStepUs;

static uint8_t orderPos;
static uint8_t row;
static uint8_t tick;
static uint8_t arpChordStep;
static uint8_t noiseDecay;
static uint8_t currentPattern;
static timeUs_t nextEngineTimeUs;
static uint16_t drumFreqHz;
static uint8_t drumVol;
static uint16_t lfsr = 0xACE1u;

static chiptuneTestPhase_e testPhase;
static uint8_t testDwellIndex;
static uint8_t testDwellCount;
static uint8_t testMarkBeepLeft;
static bool testMarkOn;
static timeUs_t testPhaseEndUs;
static uint16_t activeTestDwells[4];
static const uint16_t defaultTestDwells[] = { 12, 16, 20, 24 };

static const uint16_t noteFreqHz[CHIPTUNE_NOTE_COUNT] = {
    65, 69, 73, 78, 82, 87, 92, 98, 104, 110, 117, 123,
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
    2093
};

static const int8_t arpMinor[3] = { 0, 3, 7 };
static const int8_t arpMajor[3] = { 0, 4, 7 };

#define NV(n, v) { (n), (v) }
#define ROW(a, av, b, bv, c, cv, drum, arp) { { NV(a, av), NV(b, bv), NV(c, cv) }, (drum), (arp) }

static const chiptuneRow_t pattern0[CHIPTUNE_PATTERN_LEN] = {
    ROW(45, 12, 69, 10, 57, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 72, 11, 57, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 76, 12, 57, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MIN),
    ROW(57, 10, 74, 10, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 72, 11, 57, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(41, 12, 69, 10, 53, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(53, 10,  0,  0, 53, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(41, 12, 72, 11, 53, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(53, 10, 67, 10, 53, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(41, 12, 69, 12, 53, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(53, 10, 65, 10, 53, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(41, 12, 64, 11, 53, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(53, 10,  0,  0, 53, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
};

static const chiptuneRow_t pattern1[CHIPTUNE_PATTERN_LEN] = {
    ROW(48, 12, 72, 11, 60, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(60, 10,  0,  0, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 76, 12, 60, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(60, 10, 74, 10, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 72, 11, 60, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(60, 10, 69, 10, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 67, 11, 60, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(60, 10,  0,  0, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(43, 12, 71, 11, 55, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(55, 10,  0,  0, 55, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(43, 12, 74, 12, 55, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(55, 10, 72, 10, 55, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(43, 12, 71, 11, 55, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(55, 10, 69, 10, 55, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(43, 12, 67, 12, 55, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(55, 10, 64, 10, 55, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
};

static const chiptuneRow_t pattern2[CHIPTUNE_PATTERN_LEN] = {
    ROW(45, 13, 69, 12, 57, 9, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MIN),
    ROW(57, 11, 72, 11, 57, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(45, 13, 76, 12, 57, 9, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MIN),
    ROW(57, 11, 74, 10, 57, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(43, 13, 71, 12, 55, 9, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(55, 11, 74, 11, 55, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(43, 13, 79, 12, 55, 9, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(55, 11, 76, 10, 55, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(41, 13, 69, 12, 53, 9, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(53, 11, 72, 11, 53, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(41, 13, 77, 12, 53, 9, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(53, 11, 72, 10, 53, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(40, 13, 64, 12, 52, 9, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(52, 11, 67, 11, 52, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(40, 13, 71, 13, 52, 9, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(52, 11, 69, 12, 52, 9, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
};

static const chiptuneRow_t * const patterns[] = {
    pattern0, pattern1, pattern2,
};

static const uint8_t order[] = {
    0, 1, 0, 2,
    0, 1, 0, 2,
    0, 1, 0, 2,
};

static uint16_t noteToFreq(uint8_t note)
{
    if (note < CHIPTUNE_NOTE_MIN || note > CHIPTUNE_NOTE_MAX) {
        return 0;
    }
    return noteFreqHz[note - CHIPTUNE_NOTE_MIN];
}

static void arpClear(void)
{
    arpCount = 0;
    arpIndex = 0;
    memset(arpFreqs, 0, sizeof(arpFreqs));
    memset(arpVols, 0, sizeof(arpVols));
}

static void arpAdd(uint16_t hz, uint8_t vol)
{
    if (!hz || !vol || arpCount >= CHIPTUNE_ARP_MAX_NOTES) {
        return;
    }
    arpFreqs[arpCount] = hz;
    arpVols[arpCount] = vol > 15 ? 15 : vol;
    arpCount++;
}

static void applyTone(uint16_t hz, uint8_t vol)
{
    // Only touch the timer when the note changes — keep continuous square during dwell.
    if (hz == 0) {
        if (currentToneHz != 0) {
            currentToneHz = 0;
            beeperPwmSetTone(0, 0);
        }
        return;
    }
    if (hz == currentToneHz) {
        return;
    }
    currentToneHz = hz;
    beeperPwmSetTone(hz, vol);
}

static void arpSilence(void)
{
    currentToneHz = 0;
    beeperPwmSetTone(0, 0);
}

static void enableArpTask(bool on)
{
    setTaskEnabled(TASK_CHIPTUNE, on);
}

static void applyDwell(uint16_t ms)
{
    dwellMs = constrain(ms, CHIPTUNE_DWELL_MIN_MS, CHIPTUNE_DWELL_MAX_MS);
    // Poll a bit faster than the shortest dwell so boundaries stay tight.
    const uint16_t pollHz = (uint16_t)constrain(1000 / dwellMs * 2, 100, 2000);
    rescheduleTask(TASK_CHIPTUNE, TASK_PERIOD_HZ(pollHz));
}

static void setTestChordArp(void)
{
    arpClear();
    arpAdd(CHIPTUNE_TEST_A4, 15);
    arpAdd(CHIPTUNE_TEST_C5, 15);
    arpAdd(CHIPTUNE_TEST_E5, 15);
    arpIndex = 0;
    nextArpStepUs = 0;
}

static void triggerDrum(uint8_t drum)
{
    switch (drum) {
    case CHIPTUNE_DRUM_KICK:
        drumFreqHz = 180;
        drumVol = 14;
        noiseDecay = 3;
        break;
    case CHIPTUNE_DRUM_SNARE:
        drumFreqHz = 1400;
        drumVol = 12;
        noiseDecay = 2;
        break;
    case CHIPTUNE_DRUM_HAT:
        drumFreqHz = 3200;
        drumVol = 8;
        noiseDecay = 1;
        break;
    default:
        break;
    }
}

static void rebuildDemoArp(const chiptuneRow_t *r)
{
    arpClear();

    // Monophonic voice priority queue for this row: lead, bass, chord tones.
    if (r->ch[1].note && r->ch[1].vol) {
        arpAdd(noteToFreq(r->ch[1].note), r->ch[1].vol);
    }
    if (r->ch[0].note && r->ch[0].vol) {
        arpAdd(noteToFreq(r->ch[0].note), r->ch[0].vol);
    }

    if (r->arp != CHIPTUNE_ARP_OFF && r->ch[2].note && r->ch[2].vol) {
        const int8_t *intervals = (r->arp == CHIPTUNE_ARP_MAJ) ? arpMajor : arpMinor;
        const uint8_t root = r->ch[2].note;
        for (int i = 0; i < 3 && arpCount < CHIPTUNE_ARP_MAX_NOTES; i++) {
            const uint8_t n = (uint8_t)(root + intervals[(arpChordStep + i) % 3]);
            arpAdd(noteToFreq(n), r->ch[2].vol);
        }
    } else if (r->ch[2].note && r->ch[2].vol) {
        arpAdd(noteToFreq(r->ch[2].note), r->ch[2].vol);
    }

    if (drumVol && drumFreqHz) {
        // Insert drum hit as a short dedicated step at the front for this rebuild.
        if (arpCount < CHIPTUNE_ARP_MAX_NOTES) {
            // Shift up to make room
            for (int i = arpCount; i > 0; i--) {
                arpFreqs[i] = arpFreqs[i - 1];
                arpVols[i] = arpVols[i - 1];
            }
            arpFreqs[0] = drumFreqHz;
            arpVols[0] = drumVol;
            arpCount++;
        }
    }

    arpIndex = 0;
}

static void engineTickDemo(void)
{
    currentPattern = order[orderPos];
    const chiptuneRow_t *r = &patterns[currentPattern][row];

    if (tick == 0) {
        arpChordStep = 0;
        if (r->drum != CHIPTUNE_DRUM_NONE) {
            triggerDrum(r->drum);
        }
        rebuildDemoArp(r);
    } else {
        arpChordStep++;
        rebuildDemoArp(r);
        if (noiseDecay) {
            noiseDecay--;
            if (noiseDecay == 0) {
                drumVol = 0;
                drumFreqHz = 0;
            } else if (drumFreqHz >= 800) {
                const uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 1)) & 1u;
                lfsr = (lfsr >> 1) | (bit << 15);
                if (lfsr == 0) {
                    lfsr = 0xACE1u;
                }
                drumFreqHz = (uint16_t)(800 + (lfsr & 0x7FF));
            }
        }
    }

    tick++;
    if (tick >= CHIPTUNE_TICKS_PER_ROW) {
        tick = 0;
        row++;
        if (row >= CHIPTUNE_PATTERN_LEN) {
            row = 0;
            orderPos++;
            if (orderPos >= ARRAYLEN(order)) {
                orderPos = 0;
            }
        }
    }

    debug[0] = currentPattern;
    debug[1] = row;
    debug[2] = currentToneHz;
    debug[3] = dwellMs;
}

static void beginTestDwell(timeUs_t nowUs)
{
    applyDwell(activeTestDwells[testDwellIndex]);
    testPhase = TEST_MARK_BEEPS;
    testMarkBeepLeft = (uint8_t)(testDwellIndex + 1);
    testMarkOn = true;
    arpClear();
    arpAdd(880, 15);
    arpIndex = 0;
    nextArpStepUs = 0;
    applyTone(880, 15);
    testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_MS * 1000);
}

static void engineTickTest(timeUs_t nowUs)
{
    if (cmpTimeUs(nowUs, testPhaseEndUs) < 0) {
        debug[0] = testPhase;
        debug[1] = testDwellIndex;
        debug[2] = activeTestDwells[testDwellIndex];
        debug[3] = currentToneHz;
        return;
    }

    switch (testPhase) {
    case TEST_MARK_BEEPS:
        if (testMarkOn) {
            testMarkOn = false;
            arpSilence();
            arpClear();
            testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_GAP_MS * 1000);
            if (testMarkBeepLeft > 1) {
                testMarkBeepLeft--;
            } else {
                testPhase = TEST_GAP;
            }
        } else {
            testMarkOn = true;
            arpClear();
            arpAdd(880, 15);
            applyTone(880, 15);
            testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_MS * 1000);
        }
        break;

    case TEST_GAP:
        arpSilence();
        testPhase = TEST_ARP;
        setTestChordArp();
        applyTone(arpFreqs[0], arpVols[0]);
        arpIndex = 0;
        nextArpStepUs = nowUs + (dwellMs * 1000);
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_SECTION_MS * 1000);
        break;

    case TEST_ARP:
        arpSilence();
        arpClear();
        testPhase = TEST_END_GAP;
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_GAP_MS * 1000);
        break;

    case TEST_END_GAP:
        testDwellIndex++;
        if (testDwellIndex >= testDwellCount) {
            chiptuneStop();
            return;
        }
        beginTestDwell(nowUs);
        break;
    }

    debug[0] = testPhase;
    debug[1] = testDwellIndex;
    debug[2] = activeTestDwells[testDwellIndex];
    debug[3] = currentToneHz;
}

static bool chiptuneBegin(chiptuneMode_e startMode, uint16_t dwellOrZero)
{
    if (playing) {
        chiptuneStop();
    }

    if (ARMING_FLAG(ARMED) || !beeperPwmIsReady()) {
        return false;
    }

    beeperSilence();

    arpClear();
    currentToneHz = 0;
    orderPos = 0;
    row = 0;
    tick = 0;
    arpChordStep = 0;
    noiseDecay = 0;
    drumFreqHz = 0;
    drumVol = 0;
    lfsr = 0xACE1u;
    currentPattern = order[0];
    nextEngineTimeUs = 0;
    nextArpStepUs = 0;
    mode = startMode;

    if (!beeperPwmAudioStart(0, 0, NULL)) {
        return false;
    }

    if (startMode == MODE_TEST) {
        if (dwellOrZero == 0) {
            testDwellCount = ARRAYLEN(defaultTestDwells);
            for (uint8_t i = 0; i < testDwellCount; i++) {
                activeTestDwells[i] = defaultTestDwells[i];
            }
        } else {
            testDwellCount = 1;
            activeTestDwells[0] = constrain(dwellOrZero, CHIPTUNE_DWELL_MIN_MS, CHIPTUNE_DWELL_MAX_MS);
        }
        testDwellIndex = 0;
        beginTestDwell(micros());
    } else {
        applyDwell(dwellOrZero ? dwellOrZero : dwellMs);
    }

    playing = true;
    enableArpTask(true);
    return true;
}

bool chiptuneIsPlaying(void)
{
    return playing;
}

uint16_t chiptuneGetDwellMs(void)
{
    return dwellMs;
}

bool chiptuneSetDwellMs(uint16_t ms)
{
    if (ms < CHIPTUNE_DWELL_MIN_MS || ms > CHIPTUNE_DWELL_MAX_MS) {
        return false;
    }
    applyDwell(ms);
    return true;
}

void chiptuneStop(void)
{
    if (!playing) {
        return;
    }

    playing = false;
    enableArpTask(false);
    arpClear();
    arpSilence();
    beeperPwmAudioStop();
}

bool chiptuneStart(void)
{
    return chiptuneBegin(MODE_DEMO, dwellMs);
}

bool chiptuneStartTest(uint16_t dwellMsOrZero)
{
    return chiptuneBegin(MODE_TEST, dwellMsOrZero);
}

void chiptuneUpdate(timeUs_t currentTimeUs)
{
    if (!playing) {
        return;
    }

    if (ARMING_FLAG(ARMED)) {
        chiptuneStop();
        return;
    }

    if (mode == MODE_TEST) {
        engineTickTest(currentTimeUs);
        return;
    }

    if (nextEngineTimeUs == 0) {
        nextEngineTimeUs = currentTimeUs;
    }

    while (cmpTimeUs(currentTimeUs, nextEngineTimeUs) >= 0) {
        engineTickDemo();
        nextEngineTimeUs += (1000000 / CHIPTUNE_ENGINE_HZ);
    }
}

void chiptuneArpUpdate(timeUs_t currentTimeUs)
{
    if (!playing) {
        return;
    }

    if (arpCount == 0) {
        applyTone(0, 0);
        return;
    }

    if (nextArpStepUs == 0) {
        applyTone(arpFreqs[arpIndex], arpVols[arpIndex]);
        nextArpStepUs = currentTimeUs + (dwellMs * 1000);
        return;
    }

    if (cmpTimeUs(currentTimeUs, nextArpStepUs) < 0) {
        return;
    }

    // Dwell elapsed: step to the next note and retune ARR once.
    arpIndex++;
    if (arpIndex >= arpCount) {
        arpIndex = 0;
    }
    applyTone(arpFreqs[arpIndex], arpVols[arpIndex]);

    nextArpStepUs += (dwellMs * 1000);
    if (cmpTimeUs(currentTimeUs, nextArpStepUs) > 20000) {
        nextArpStepUs = currentTimeUs + (dwellMs * 1000);
    }

    debug[2] = currentToneHz;
    debug[3] = dwellMs;
}

#endif // USE_CHIPTUNE
