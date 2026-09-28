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

#include <stdbool.h>
#include <stdint.h>
#include "platform.h"

#if defined(USE_BEEPER) && defined(USE_PWM_OUTPUT)

#include "drivers/io.h"
#include "drivers/pwm_output.h"
#include "drivers/sound_beeper.h"
#include "drivers/timer.h"

static pwmOutputPort_t beeperPwm;
static uint16_t freqBeep = 0;

#ifdef USE_CHIPTUNE
static const timerHardware_t *beeperTimer = NULL;
static bool beeperAudioMode = false;
#endif

void pwmWriteBeeper(bool on)
{
#ifdef USE_CHIPTUNE
    if (beeperAudioMode) {
        return;
    }
#endif
    if (!beeperPwm.io || freqBeep == 0) {
        return;
    }

    if (on) {
        *beeperPwm.channel.ccr = (PWM_TIMER_1MHZ / freqBeep) / 2;
        beeperPwm.enabled = true;
    } else {
        *beeperPwm.channel.ccr = 0;
        beeperPwm.enabled = false;
    }
}

void pwmToggleBeeper(void)
{
#ifdef USE_CHIPTUNE
    if (beeperAudioMode) {
        return;
    }
#endif
    pwmWriteBeeper(!beeperPwm.enabled);
}

void beeperPwmInit(const ioTag_t tag, uint16_t frequency)
{
    const timerHardware_t *timer = timerAllocate(tag, OWNER_BEEPER, 0);
    IO_t beeperIO = IOGetByTag(tag);

    if (beeperIO && timer && frequency) {
        beeperPwm.io = beeperIO;
        IOInit(beeperPwm.io, OWNER_BEEPER, 0);
        IOConfigGPIOAF(beeperPwm.io, IOCFG_AF_PP, timer->alternateFunction);
        freqBeep = frequency;
#ifdef USE_CHIPTUNE
        beeperTimer = timer;
        beeperAudioMode = false;
#endif
        pwmOutputConfig(&beeperPwm.channel, timer, PWM_TIMER_1MHZ, PWM_TIMER_1MHZ / freqBeep, (PWM_TIMER_1MHZ / freqBeep) / 2, 0);

        *beeperPwm.channel.ccr = 0;
        beeperPwm.enabled = false;
    }
}

#ifdef USE_CHIPTUNE
bool beeperPwmIsReady(void)
{
    return beeperPwm.io && beeperTimer && freqBeep && beeperPwm.channel.ccr;
}

void beeperPwmSetDuty(uint16_t duty)
{
    if (beeperPwm.channel.ccr) {
        *beeperPwm.channel.ccr = duty;
    }
}

uint16_t beeperPwmGetPeriod(void)
{
    if (!beeperTimer) {
        return 0;
    }
    // timerGetPeriod returns ARR; PWM period counts are ARR+1
    return (uint16_t)(timerGetPeriod(beeperTimer) + 1);
}

bool beeperPwmAudioStart(uint32_t hz, uint16_t period, timerOvrHandlerRec_t *overflowCb)
{
    if (!beeperPwmIsReady() || !overflowCb || hz == 0 || period < 2) {
        return false;
    }

    beeperAudioMode = true;

    // Fixed-rate PWM carrier; duty carries mixed audio samples.
    pwmOutputConfig(&beeperPwm.channel, beeperTimer, hz, period, 0, 0);

    // Ensure update IRQ NVIC is enabled (needed for TIM1 UP on F4).
    timerConfigure(beeperTimer, period, hz);
    pwmOutputConfig(&beeperPwm.channel, beeperTimer, hz, period, 0, 0);

    timerConfigUpdateCallback(beeperTimer, overflowCb);

    *beeperPwm.channel.ccr = 0;
    beeperPwm.enabled = true;
    return true;
}

void beeperPwmAudioStop(void)
{
    if (!beeperTimer || !freqBeep) {
        beeperAudioMode = false;
        return;
    }

    timerConfigUpdateCallback(beeperTimer, NULL);

    pwmOutputConfig(&beeperPwm.channel, beeperTimer, PWM_TIMER_1MHZ, PWM_TIMER_1MHZ / freqBeep, (PWM_TIMER_1MHZ / freqBeep) / 2, 0);
    *beeperPwm.channel.ccr = 0;
    beeperPwm.enabled = false;
    beeperAudioMode = false;
}
#endif // USE_CHIPTUNE

#endif
