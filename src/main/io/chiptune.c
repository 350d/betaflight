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
 * Output architecture B: mixed PWM samples on the beeper timer.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#ifdef USE_CHIPTUNE

#include "build/debug.h"

#include "common/utils.h"

#include "drivers/sound_beeper.h"
#include "drivers/timer.h"

#include "fc/runtime_config.h"

#include "io/beeper.h"
#include "io/chiptune.h"

// 8 MHz timebase / 256 period = 31250 Hz PWM carrier == audio sample rate
#define CHIPTUNE_PWM_HZ         8000000u
#define CHIPTUNE_PWM_PERIOD     256u
#define CHIPTUNE_SAMPLE_RATE    (CHIPTUNE_PWM_HZ / CHIPTUNE_PWM_PERIOD)

#define CHIPTUNE_ENGINE_HZ      50
#define CHIPTUNE_TICKS_PER_ROW  6

#define CHIPTUNE_NOTE_MIN       36   // C2
#define CHIPTUNE_NOTE_MAX       96   // C7
#define CHIPTUNE_NOTE_COUNT     (CHIPTUNE_NOTE_MAX - CHIPTUNE_NOTE_MIN + 1)

#define CHIPTUNE_CH_COUNT       3
#define CHIPTUNE_PATTERN_LEN    16

#define CHIPTUNE_DRUM_NONE      0
#define CHIPTUNE_DRUM_KICK      1
#define CHIPTUNE_DRUM_SNARE     2
#define CHIPTUNE_DRUM_HAT       3

#define CHIPTUNE_ARP_OFF        0
#define CHIPTUNE_ARP_MIN        1
#define CHIPTUNE_ARP_MAJ        2

#define CHIPTUNE_MIX_MAX        (3 * 15 + 15)

typedef struct {
    uint8_t note;   // MIDI note, 0 = gate off
    uint8_t vol;    // 0..15
} chiptuneNote_t;

typedef struct {
    chiptuneNote_t ch[CHIPTUNE_CH_COUNT];
    uint8_t drum;   // CHIPTUNE_DRUM_*
    uint8_t arp;    // CHIPTUNE_ARP_* applied to channel C root
} chiptuneRow_t;

typedef struct {
    volatile uint32_t phase;
    volatile uint32_t step;
    volatile uint8_t volume;
} chiptuneOsc_t;

static chiptuneOsc_t oscillators[CHIPTUNE_CH_COUNT];
static volatile uint8_t noiseVolume;
static volatile uint16_t noisePeriod;   // samples between LFSR clocks
static volatile uint16_t noiseCounter;
static volatile uint16_t lfsr = 0xACE1u;

static volatile bool playing;
static timerOvrHandlerRec_t audioOverflowCb;
static uint16_t pwmPeriod;

static uint8_t orderPos;
static uint8_t row;
static uint8_t tick;
static uint8_t arpStep;
static uint8_t noiseDecay;
static uint8_t currentPattern;
static timeUs_t nextEngineTimeUs;

// phaseStep = round(freq * 2^32 / 31250), MIDI 36..96
static const uint32_t phaseSteps[CHIPTUNE_NOTE_COUNT] = {
    8989386u, 9523923u, 10090245u, 10690242u, 11325917u, 11999391u, 12712912u, 13468861u,
    14269761u, 15118285u, 16017265u, 16969701u, 17978772u, 19047845u, 20180489u, 21380484u,
    22651833u, 23998781u, 25425823u, 26937721u, 28539522u, 30236570u, 32034530u, 33939402u,
    35957544u, 38095691u, 40360978u, 42760967u, 45303666u, 47997563u, 50851646u, 53875442u,
    57079043u, 60473140u, 64069060u, 67878804u, 71915088u, 76191381u, 80721957u, 85521934u,
    90607333u, 95995125u, 101703292u, 107750885u, 114158086u, 120946279u, 128138119u, 135757608u,
    143830176u, 152382763u, 161443913u, 171043868u, 181214666u, 191990251u, 203406585u, 215501770u,
    228316172u, 241892558u, 256276238u, 271515216u, 287660351u
};

static const int8_t arpMinor[3] = { 0, 3, 7 };
static const int8_t arpMajor[3] = { 0, 4, 7 };

// Original demo tune (Am / F / C / G feeling). Newly authored; not from any game/demo.
#define NV(n, v) { (n), (v) }
#define ROW(a, av, b, bv, c, cv, drum, arp) { { NV(a, av), NV(b, bv), NV(c, cv) }, (drum), (arp) }

static const chiptuneRow_t pattern0[CHIPTUNE_PATTERN_LEN] = {
    // Am bar: bass A2/A3, lead motif, Am arp
    ROW(45, 12, 69, 10, 57, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 72, 11, 57, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 76, 12, 57, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MIN),
    ROW(57, 10, 74, 10, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    ROW(45, 12, 72, 11, 57, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MIN),
    ROW(57, 10,  0,  0, 57, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MIN),
    // F bar
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
    // C bar
    ROW(48, 12, 72, 11, 60, 8, CHIPTUNE_DRUM_KICK,  CHIPTUNE_ARP_MAJ),
    ROW(60, 10,  0,  0, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 76, 12, 60, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(60, 10, 74, 10, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 72, 11, 60, 8, CHIPTUNE_DRUM_SNARE, CHIPTUNE_ARP_MAJ),
    ROW(60, 10, 69, 10, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    ROW(48, 12, 67, 11, 60, 8, CHIPTUNE_DRUM_HAT,   CHIPTUNE_ARP_MAJ),
    ROW(60, 10,  0,  0, 60, 8, CHIPTUNE_DRUM_NONE,  CHIPTUNE_ARP_MAJ),
    // G bar
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
    // Am - G - F - E turnaround
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

// ~24 s loop at 50 Hz / 6 ticks/row
static const uint8_t order[] = {
    0, 1, 0, 2,
    0, 1, 0, 2,
    0, 1, 0, 2,
};

static uint32_t noteToStep(uint8_t note)
{
    if (note < CHIPTUNE_NOTE_MIN || note > CHIPTUNE_NOTE_MAX) {
        return 0;
    }
    return phaseSteps[note - CHIPTUNE_NOTE_MIN];
}

static void setChannel(uint8_t index, uint8_t note, uint8_t vol)
{
    if (index >= CHIPTUNE_CH_COUNT) {
        return;
    }
    if (note == 0 || vol == 0) {
        oscillators[index].step = 0;
        oscillators[index].volume = 0;
        return;
    }
    oscillators[index].step = noteToStep(note);
    oscillators[index].volume = vol > 15 ? 15 : vol;
}

static void triggerDrum(uint8_t drum)
{
    switch (drum) {
    case CHIPTUNE_DRUM_KICK:
        noisePeriod = 8;
        noiseVolume = 14;
        noiseDecay = 4;
        // soft low thump via bass duck into noise-ish pitch
        oscillators[0].step = noteToStep(36);
        oscillators[0].volume = 12;
        break;
    case CHIPTUNE_DRUM_SNARE:
        noisePeriod = 2;
        noiseVolume = 13;
        noiseDecay = 3;
        break;
    case CHIPTUNE_DRUM_HAT:
        noisePeriod = 1;
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

static void engineTick(void)
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
        // decay once per engine tick while drum is active
        noiseDecay--;
        if (noiseVolume > 2) {
            noiseVolume -= 2;
        } else {
            noiseVolume = 0;
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
    debug[2] = oscillators[1].step ? (int16_t)(oscillators[1].volume) : 0;
    debug[3] = noiseVolume;
}

static void FAST_CODE chiptuneAudioOverflow(timerOvrHandlerRec_t *cbRec, captureCompare_t capture)
{
    UNUSED(cbRec);
    UNUSED(capture);

    if (!playing) {
        return;
    }

    uint16_t mix = 0;

    for (int i = 0; i < CHIPTUNE_CH_COUNT; i++) {
        oscillators[i].phase += oscillators[i].step;
        if (oscillators[i].volume && (oscillators[i].phase & 0x80000000u)) {
            mix += oscillators[i].volume;
        }
    }

    if (noiseVolume) {
        if (noiseCounter) {
            noiseCounter--;
        } else {
            noiseCounter = noisePeriod ? noisePeriod : 1;
            // 16-bit Fibonacci LFSR (taps 0,1 -> maximal-ish sequence)
            const uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 1)) & 1u;
            lfsr = (lfsr >> 1) | (bit << 15);
            if (lfsr == 0) {
                lfsr = 0xACE1u;
            }
        }
        if (lfsr & 1u) {
            mix += noiseVolume;
        }
    }

    // Scale 0..60 into 0..period (avoid 100% duty)
    const uint32_t duty = ((uint32_t)mix * (pwmPeriod - 1u)) / CHIPTUNE_MIX_MAX;
    beeperPwmSetDuty((uint16_t)duty);
}

bool chiptuneIsPlaying(void)
{
    return playing;
}

void chiptuneStop(void)
{
    if (!playing) {
        return;
    }

    playing = false;

    for (int i = 0; i < CHIPTUNE_CH_COUNT; i++) {
        oscillators[i].step = 0;
        oscillators[i].volume = 0;
    }
    noiseVolume = 0;

    beeperPwmAudioStop();
}

bool chiptuneStart(void)
{
    if (playing) {
        return true;
    }

    if (ARMING_FLAG(ARMED)) {
        return false;
    }

    if (!beeperPwmIsReady()) {
        return false;
    }

    beeperSilence();

    memset((void *)oscillators, 0, sizeof(oscillators));
    noiseVolume = 0;
    noisePeriod = 1;
    noiseCounter = 0;
    lfsr = 0xACE1u;
    orderPos = 0;
    row = 0;
    tick = 0;
    arpStep = 0;
    noiseDecay = 0;
    currentPattern = order[0];
    nextEngineTimeUs = 0;

    timerChannelOverflowHandlerInit(&audioOverflowCb, chiptuneAudioOverflow);

    if (!beeperPwmAudioStart(CHIPTUNE_PWM_HZ, CHIPTUNE_PWM_PERIOD, &audioOverflowCb)) {
        return false;
    }

    pwmPeriod = beeperPwmGetPeriod();
    if (pwmPeriod < 2) {
        beeperPwmAudioStop();
        return false;
    }

    playing = true;
    return true;
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

    if (nextEngineTimeUs == 0) {
        nextEngineTimeUs = currentTimeUs;
    }

    while (cmpTimeUs(currentTimeUs, nextEngineTimeUs) >= 0) {
        engineTick();
        nextEngineTimeUs += (1000000 / CHIPTUNE_ENGINE_HZ);
    }
}

#endif // USE_CHIPTUNE
