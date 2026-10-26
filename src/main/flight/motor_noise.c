/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Betaflight. If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdbool.h>

#include "platform.h"

#ifdef USE_MOTOR_NOISE

#include "build/debug.h"
#include "common/filter.h"
#include "common/maths.h"
#include "drivers/motor.h"
#include "flight/mixer.h"

#include "motor_noise.h"

#define MOTOR_NOISE_HP_HZ    80.0f    // below this the prop still follows the command: not ripple
#define MOTOR_NOISE_AVG_S     0.5f    // averaging time constant

static pt1Filter_t lowpass[MAX_SUPPORTED_MOTORS];   // highpass = input - lowpass
static float meanSq[MAX_SUPPORTED_MOTORS];         // EMA of the squared highpassed command
static bool seeded;                                 // the lowpass starts at the first sample: no start-up transient
static float kAvg;
static float scaleToExternal;

void motorNoiseInit(float looptimeUs)
{
    const float dt = looptimeUs * 1e-6f;
    for (int i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
        pt1FilterInit(&lowpass[i], pt1FilterGain(MOTOR_NOISE_HP_HZ, dt));
        meanSq[i] = 0.0f;
    }
    seeded = false;
    kAvg = dt / MOTOR_NOISE_AVG_S;
    // internal motor units -> external 1000..2000 units, measured in the linear region
    scaleToExternal = 200.0f / (motorConvertFromExternal(1600) - motorConvertFromExternal(1400));
}

// Runs every PID loop. Kept out of the speed-optimised mixTable(), where it is inlined at several times this size.
NOINLINE void motorNoiseUpdate(const float *motorOutput, int motorCount)
{
    for (int i = 0; i < motorCount && i < MAX_SUPPORTED_MOTORS; i++) {
        const float x = motorOutput[i] * scaleToExternal;
        if (!seeded) {
            lowpass[i].state = x;
        }
        const float hp = x - pt1FilterApply(&lowpass[i], x);
        meanSq[i] += kAvg * (hp * hp - meanSq[i]);
    }
    seeded = true;
    if (debugMode == DEBUG_MOTOR_NOISE) {
        for (int i = 0; i < 4; i++) {
            DEBUG_SET(DEBUG_MOTOR_NOISE, i, lrintf(10.0f * motorNoiseRms(i)));  //!< [index:0..3] Motor {1|2|3|4} Command Noise RMS [unit:0.1]
        }
    }
}

float motorNoiseMsq(int motor)
{
    return (motor >= 0 && motor < MAX_SUPPORTED_MOTORS) ? meanSq[motor] : 0.0f;
}

float motorNoiseRms(int motor)
{
    return sqrtf(motorNoiseMsq(motor));
}

#endif // USE_MOTOR_NOISE
