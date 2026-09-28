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
 * Experimental timer-backed 3-channel + noise PSG player.
 * Approach A: hardware square-wave frequency, channels time-multiplexed.
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

#define CHIPTUNE_DRUM_NONE          0
#define CHIPTUNE_DRUM_KICK          1
#define CHIPTUNE_DRUM_SNARE         2
#define CHIPTUNE_DRUM_HAT           3

#define CHIPTUNE_ARP_OFF            0
#define CHIPTUNE_ARP_MIN            1
#define CHIPTUNE_ARP_MAJ            2

#define CHIPTUNE_MUX_DEFAULT_HZ     1000
#define CHIPTUNE_MUX_MIN_HZ         100
#define CHIPTUNE_MUX_MAX_HZ         8000

#define CHIPTUNE_TEST_NOTE_A4       440
#define CHIPTUNE_TEST_NOTE_E5       659
#define CHIPTUNE_TEST_NOTE_A5       880
#define CHIPTUNE_TEST_PHASE_MS      2000
#define CHIPTUNE_TEST_GAP_MS        300
#define CHIPTUNE_TEST_BEEP_MS       120
#define CHIPTUNE_TEST_BEEP_GAP_MS   80

typedef enum {
    MODE_DEMO = 0,
    MODE_TEST,
} chiptuneMode_e;

typedef enum {
    TEST_MARK_BEEPS = 0,  // N short beeps = mux-rate index (1..4)
    TEST_GAP,
    TEST_ONE,             // A4 only
    TEST_TWO,             // A4 + E5
    TEST_THREE,           // A4 + E5 + A5
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

typedef struct {
    volatile uint16_t freqHz;
    volatile uint8_t volume;
} chiptuneOsc_t;

static chiptuneOsc_t oscillators[CHIPTUNE_CH_COUNT];
static volatile uint16_t noiseFreqHz;
static volatile uint8_t noiseVolume;
static volatile uint16_t lfsr = 0xACE1u;

static volatile bool playing;
static chiptuneMode_e mode;
static uint8_t muxIndex;
static uint16_t muxHz = CHIPTUNE_MUX_DEFAULT_HZ;

static uint8_t orderPos;
static uint8_t row;
static uint8_t tick;
static uint8_t arpStep;
static uint8_t noiseDecay;
static uint8_t currentPattern;
static timeUs_t nextEngineTimeUs;

// Test sequencer
static chiptuneTestPhase_e testPhase;
static uint8_t testRateIndex;
static uint8_t testRateCount;
static uint8_t testMarkBeepLeft;
static bool testMarkOn;
static timeUs_t testPhaseEndUs;
static uint16_t activeTestRates[4];
static const uint16_t defaultTestMuxRates[] = { 500, 1000, 2000, 4000 };

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
    pattern0,
    pattern1,
    pattern2,
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

static void clearChannels(void)
{
    for (int i = 0; i < CHIPTUNE_CH_COUNT; i++) {
        oscillators[i].freqHz = 0;
        oscillators[i].volume = 0;
    }
    noiseVolume = 0;
    noiseFreqHz = 0;
}

static void setChannel(uint8_t index, uint8_t note, uint8_t vol)
{
    if (index >= CHIPTUNE_CH_COUNT) {
        return;
    }
    if (note == 0 || vol == 0) {
        oscillators[index].freqHz = 0;
        oscillators[index].volume = 0;
        return;
    }
    oscillators[index].freqHz = noteToFreq(note);
    oscillators[index].volume = vol > 15 ? 15 : vol;
}

static void setChannelHz(uint8_t index, uint16_t hz, uint8_t vol)
{
    if (index >= CHIPTUNE_CH_COUNT) {
        return;
    }
    oscillators[index].freqHz = hz;
    oscillators[index].volume = (hz && vol) ? (vol > 15 ? 15 : vol) : 0;
}

static void applyMuxRate(uint16_t hz)
{
    muxHz = constrain(hz, CHIPTUNE_MUX_MIN_HZ, CHIPTUNE_MUX_MAX_HZ);
    rescheduleTask(TASK_CHIPTUNE, TASK_PERIOD_HZ(muxHz));
}

static void enableMuxTask(bool on)
{
    setTaskEnabled(TASK_CHIPTUNE, on);
}

static void triggerDrum(uint8_t drum)
{
    switch (drum) {
    case CHIPTUNE_DRUM_KICK:
        noiseFreqHz = 180;
        noiseVolume = 14;
        noiseDecay = 4;
        oscillators[0].freqHz = noteToFreq(36);
        oscillators[0].volume = 12;
        break;
    case CHIPTUNE_DRUM_SNARE:
        noiseFreqHz = 1200;
        noiseVolume = 13;
        noiseDecay = 3;
        break;
    case CHIPTUNE_DRUM_HAT:
        noiseFreqHz = 4000;
        noiseVolume = 8;
        noiseDecay = 1;
        break;
    default:
        break;
    }
}

static void applyRow(const chiptuneRow_t *r)
{
    setChannel(0, r->ch[0].note, r->ch[0].vol);
    setChannel(1, r->ch[1].note, r->ch[1].vol);

    if (r->arp == CHIPTUNE_ARP_OFF) {
        setChannel(2, r->ch[2].note, r->ch[2].vol);
    } else {
        const int8_t *intervals = (r->arp == CHIPTUNE_ARP_MAJ) ? arpMajor : arpMinor;
        const uint8_t root = r->ch[2].note;
        const uint8_t note = root ? (uint8_t)(root + intervals[arpStep % 3]) : 0;
        setChannel(2, note, r->ch[2].vol);
    }

    if (r->drum != CHIPTUNE_DRUM_NONE) {
        triggerDrum(r->drum);
    }
}

static void engineTickDemo(void)
{
    currentPattern = order[orderPos];
    const chiptuneRow_t *r = &patterns[currentPattern][row];

    if (tick == 0) {
        arpStep = 0;
        applyRow(r);
    } else if (r->arp != CHIPTUNE_ARP_OFF) {
        arpStep++;
        const int8_t *intervals = (r->arp == CHIPTUNE_ARP_MAJ) ? arpMajor : arpMinor;
        const uint8_t root = r->ch[2].note;
        const uint8_t note = root ? (uint8_t)(root + intervals[arpStep % 3]) : 0;
        setChannel(2, note, r->ch[2].vol);
    }

    if (noiseDecay && noiseVolume) {
        noiseDecay--;
        if (noiseVolume > 2) {
            noiseVolume -= 2;
        } else {
            noiseVolume = 0;
            noiseFreqHz = 0;
        }
        if (noiseVolume && noiseFreqHz >= 800) {
            const uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 1)) & 1u;
            lfsr = (lfsr >> 1) | (bit << 15);
            if (lfsr == 0) {
                lfsr = 0xACE1u;
            }
            noiseFreqHz = (uint16_t)(800 + (lfsr & 0x7FF));
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
    debug[2] = oscillators[1].freqHz;
    debug[3] = muxHz;
}

static void applyTestVoices(uint8_t voiceCount)
{
    clearChannels();
    if (voiceCount >= 1) {
        setChannelHz(0, CHIPTUNE_TEST_NOTE_A4, 15);
    }
    if (voiceCount >= 2) {
        setChannelHz(1, CHIPTUNE_TEST_NOTE_E5, 15);
    }
    if (voiceCount >= 3) {
        setChannelHz(2, CHIPTUNE_TEST_NOTE_A5, 15);
    }
}

static void beginTestRate(timeUs_t nowUs)
{
    applyMuxRate(activeTestRates[testRateIndex]);
    testPhase = TEST_MARK_BEEPS;
    testMarkBeepLeft = (uint8_t)(testRateIndex + 1);
    testMarkOn = true;
    clearChannels();
    setChannelHz(0, 880, 15);
    testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_MS * 1000);
}

static void engineTickTest(timeUs_t nowUs)
{
    if (cmpTimeUs(nowUs, testPhaseEndUs) < 0) {
        debug[0] = testPhase;
        debug[1] = testRateIndex;
        debug[2] = activeTestRates[testRateIndex];
        debug[3] = muxHz;
        return;
    }

    switch (testPhase) {
    case TEST_MARK_BEEPS:
        if (testMarkOn) {
            testMarkOn = false;
            clearChannels();
            testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_GAP_MS * 1000);
            if (testMarkBeepLeft > 1) {
                testMarkBeepLeft--;
            } else {
                testPhase = TEST_GAP;
            }
        } else {
            testMarkOn = true;
            setChannelHz(0, 880, 15);
            testPhaseEndUs = nowUs + (CHIPTUNE_TEST_BEEP_MS * 1000);
        }
        break;

    case TEST_GAP:
        clearChannels();
        testPhase = TEST_ONE;
        applyTestVoices(1);
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_PHASE_MS * 1000);
        break;

    case TEST_ONE:
        testPhase = TEST_TWO;
        applyTestVoices(2);
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_PHASE_MS * 1000);
        break;

    case TEST_TWO:
        testPhase = TEST_THREE;
        applyTestVoices(3);
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_PHASE_MS * 1000);
        break;

    case TEST_THREE:
        clearChannels();
        testPhase = TEST_END_GAP;
        testPhaseEndUs = nowUs + (CHIPTUNE_TEST_GAP_MS * 1000);
        break;

    case TEST_END_GAP:
        testRateIndex++;
        if (testRateIndex >= testRateCount) {
            chiptuneStop();
            return;
        }
        beginTestRate(nowUs);
        break;
    }

    debug[0] = testPhase;
    debug[1] = testRateIndex;
    debug[2] = activeTestRates[testRateIndex];
    debug[3] = muxHz;
}

static void muxOutput(void)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint8_t slot = muxIndex++ & 3u;

        if (slot == 3) {
            if (noiseVolume && noiseFreqHz) {
                beeperPwmSetTone(noiseFreqHz, noiseVolume);
                return;
            }
            continue;
        }

        if (oscillators[slot].volume && oscillators[slot].freqHz) {
            beeperPwmSetTone(oscillators[slot].freqHz, oscillators[slot].volume);
            return;
        }
    }

    beeperPwmSetTone(0, 0);
}

static bool chiptuneBegin(chiptuneMode_e startMode, uint16_t startMuxHz)
{
    if (playing) {
        chiptuneStop();
    }

    if (ARMING_FLAG(ARMED)) {
        return false;
    }

    if (!beeperPwmIsReady()) {
        return false;
    }

    beeperSilence();

    clearChannels();
    lfsr = 0xACE1u;
    muxIndex = 0;
    orderPos = 0;
    row = 0;
    tick = 0;
    arpStep = 0;
    noiseDecay = 0;
    currentPattern = order[0];
    nextEngineTimeUs = 0;
    mode = startMode;

    if (!beeperPwmAudioStart(0, 0, NULL)) {
        return false;
    }

    if (startMode == MODE_TEST) {
        if (startMuxHz == 0) {
            testRateCount = ARRAYLEN(defaultTestMuxRates);
            for (uint8_t i = 0; i < testRateCount; i++) {
                activeTestRates[i] = defaultTestMuxRates[i];
            }
        } else {
            testRateCount = 1;
            activeTestRates[0] = constrain(startMuxHz, CHIPTUNE_MUX_MIN_HZ, CHIPTUNE_MUX_MAX_HZ);
        }
        testRateIndex = 0;
        beginTestRate(micros());
    } else {
        applyMuxRate(startMuxHz ? startMuxHz : muxHz);
    }

    playing = true;
    enableMuxTask(true);
    return true;
}

bool chiptuneIsPlaying(void)
{
    return playing;
}

uint16_t chiptuneGetMuxHz(void)
{
    return muxHz;
}

bool chiptuneSetMuxHz(uint16_t hz)
{
    if (hz < CHIPTUNE_MUX_MIN_HZ || hz > CHIPTUNE_MUX_MAX_HZ) {
        return false;
    }
    applyMuxRate(hz);
    return true;
}

void chiptuneStop(void)
{
    if (!playing) {
        return;
    }

    playing = false;
    enableMuxTask(false);
    clearChannels();
    beeperPwmSetTone(0, 0);
    beeperPwmAudioStop();
}

bool chiptuneStart(void)
{
    return chiptuneBegin(MODE_DEMO, muxHz);
}

bool chiptuneStartTest(uint16_t requestedMuxHz)
{
    return chiptuneBegin(MODE_TEST, requestedMuxHz);
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

void chiptuneMuxUpdate(timeUs_t currentTimeUs)
{
    UNUSED(currentTimeUs);

    if (!playing) {
        return;
    }

    muxOutput();
}

#endif // USE_CHIPTUNE
