/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "platform.h"

#ifdef USE_MAX7456

#include "common/maths.h"
#include "common/time.h"
#include "common/utils.h"

#include "drivers/display.h"
#include "drivers/max7456.h"
#include "drivers/osd.h"
#include "drivers/time.h"

#include "config/config.h"

#include "fc/core.h"
#include "fc/runtime_config.h"

#include "flight/position.h"

#include "io/displayport_max7456.h"
#ifdef USE_MAX7456
#include "io/osd_demo.h"
#endif

#include "osd/osd.h"

#include "pg/displayport_profiles.h"
#include "pg/max7456.h"
#include "pg/vcd.h"

#include "sensors/acceleration.h"
#include "sensors/gyro.h"

static displayPort_t max7456DisplayPort;
static vcdProfile_t const *max7456VcdProfile;

// Experimental HUD inertia via MAX7456 HOS/VOS.
// Screen: +x right, +y down.
// +roll (right) -> HUD lags left (-x); +pitch (nose up) -> HUD lags down (+y).
//
// Model: high-pass gyro rate sets a moving lag target (вираж entry/exit),
// soft spring follows that target. Impacts kick POSITION immediately (not only velocity),
// otherwise a stiff damper eats the hit before it is visible.
#define HUD_MOTION_RATE_GAIN        0.16f    // px per (deg/s) of HP rate
#define HUD_MOTION_RATE_DEADZONE    2.0f
#define HUD_MOTION_LP_HZ            0.8f     // lower = longer visible lag while turning
#define HUD_MOTION_WN               5.5f     // soft spring — must not kill impacts
#define HUD_MOTION_ZETA             0.95f
#define HUD_MOTION_SNAP_GAIN        0.07f
#define HUD_MOTION_SNAP_DEADZONE    12.0f
#define HUD_MOTION_MAX_PX           24.0f
#define HUD_MOTION_IMPACT_THRESHOLD 5.0f     // g/s
#define HUD_MOTION_IMPACT_AXIS_GAIN 0.14f
#define HUD_MOTION_IMPACT_PUNCH     0.22f
#define HUD_MOTION_IMPACT_POS       0.55f    // fraction of impact applied straight to position
#define HUD_MOTION_IMPACT_MAX       48.0f
// Throttle focus: shrinks maneuver lag more than impacts.
#define HUD_MOTION_THROTTLE_FOCUS   0.55f
#define HUD_MOTION_THROTTLE_IMPACT  0.25f
#define HUD_MOTION_THROTTLE_STIFFEN 0.35f
#define HUD_MOTION_VARIO_GAIN       0.008f
#define HUD_MOTION_VARIO_DEADZONE   40.0f

static float hudPosX, hudPosY;
static float hudVelX, hudVelY;
static float hudRollLp, hudPitchLp;
static float hudPrevGyroRoll, hudPrevGyroPitch;
static timeUs_t hudMotionLastUs;
static bool hudMotionPrevValid;

static void max7456HudMotionReset(void)
{
    hudPosX = 0.0f;
    hudPosY = 0.0f;
    hudVelX = 0.0f;
    hudVelY = 0.0f;
    hudRollLp = 0.0f;
    hudPitchLp = 0.0f;
    hudMotionPrevValid = false;
    hudMotionLastUs = 0;
    max7456ResetHudMotionOffset();
}

static void max7456HudMotionUpdate(void)
{
    if (!osdConfig()->hud_motion) {
        if (hudMotionPrevValid || hudPosX != 0.0f || hudPosY != 0.0f || hudVelX != 0.0f || hudVelY != 0.0f) {
            max7456HudMotionReset();
        }
        return;
    }

    // Allow motion while disarmed for bench testing (no arm required).
    // To restore armed-only behavior, use: if (!ARMING_FLAG(ARMED)) {
    if (false) {
        max7456HudMotionReset();
        return;
    }

    const timeUs_t nowUs = micros();
    float dt = hudMotionPrevValid ? (nowUs - hudMotionLastUs) * 1e-6f : (1.0f / 60.0f);
    dt = constrainf(dt, 0.005f, 0.05f);
    hudMotionLastUs = nowUs;

    const float roll = gyro.gyroADCf[FD_ROLL];
    const float pitch = gyro.gyroADCf[FD_PITCH];

    // First-order HP: sustained rate fades, вираж entry/exit remains.
    const float alphaLp = constrainf(2.0f * M_PIf * HUD_MOTION_LP_HZ * dt, 0.0f, 1.0f);
    if (!hudMotionPrevValid) {
        hudRollLp = roll;
        hudPitchLp = pitch;
    } else {
        hudRollLp += (roll - hudRollLp) * alphaLp;
        hudPitchLp += (pitch - hudPitchLp) * alphaLp;
    }

    float rollHp = roll - hudRollLp;
    float pitchHp = pitch - hudPitchLp;
    if (fabsf(rollHp) < HUD_MOTION_RATE_DEADZONE) {
        rollHp = 0.0f;
    }
    if (fabsf(pitchHp) < HUD_MOTION_RATE_DEADZONE) {
        pitchHp = 0.0f;
    }

    // Lag target from transient rate (accurate on banked turns / pitch pulls).
    float targetX = -rollHp * HUD_MOTION_RATE_GAIN;
    float targetY = pitchHp * HUD_MOTION_RATE_GAIN;

#ifdef USE_VARIO
    // Climb -> HUD slightly down; fall/dive -> HUD slightly up (lag vs vertical speed).
    float varioCms = getEstimatedVario();
    if (fabsf(varioCms) < HUD_MOTION_VARIO_DEADZONE) {
        varioCms = 0.0f;
    }
    targetY += varioCms * HUD_MOTION_VARIO_GAIN;
#endif

    // Snap / impact from angular acceleration between samples.
    float dRoll = 0.0f;
    float dPitch = 0.0f;
    if (hudMotionPrevValid) {
        dRoll = roll - hudPrevGyroRoll;
        dPitch = pitch - hudPrevGyroPitch;
    }
    hudPrevGyroRoll = roll;
    hudPrevGyroPitch = pitch;
    hudMotionPrevValid = true;

    if (fabsf(dRoll) < HUD_MOTION_SNAP_DEADZONE) {
        dRoll = 0.0f;
    }
    if (fabsf(dPitch) < HUD_MOTION_SNAP_DEADZONE) {
        dPitch = 0.0f;
    }

    float impulseX = -dRoll * HUD_MOTION_SNAP_GAIN;
    float impulseY = dPitch * HUD_MOTION_SNAP_GAIN;
    float impactX = 0.0f;
    float impactY = 0.0f;

#ifdef USE_ACC
    // Falls / collisions: drive HUD from accelerometer jerk (g/s).
    // Body: X forward, Y right, Z up. HUD lags opposite the shove.
    if (acc.jerkMagnitude > HUD_MOTION_IMPACT_THRESHOLD) {
        const float inv1G = acc.dev.acc_1G_rec;
        const float jx = acc.jerk.v[X] * inv1G;
        const float jy = acc.jerk.v[Y] * inv1G;
        const float jz = acc.jerk.v[Z] * inv1G;
        const float excess = acc.jerkMagnitude - HUD_MOTION_IMPACT_THRESHOLD;

        impactX = -jy * HUD_MOTION_IMPACT_AXIS_GAIN;
        impactY = (jx - jz) * HUD_MOTION_IMPACT_AXIS_GAIN;

        const float punch = excess * HUD_MOTION_IMPACT_PUNCH;
        if (acc.jerkMagnitude > 1e-3f) {
            impactX += (-jy / acc.jerkMagnitude) * punch;
            impactY += ((jx - jz) / acc.jerkMagnitude) * punch;
        }

        impactX = constrainf(impactX, -HUD_MOTION_IMPACT_MAX, HUD_MOTION_IMPACT_MAX);
        impactY = constrainf(impactY, -HUD_MOTION_IMPACT_MAX, HUD_MOTION_IMPACT_MAX);
    }
#endif

    // Focus/speed: throttle reduces maneuver lag; impacts stay more visible.
    const float thr = constrainf(calculateThrottlePercentAbs() * 0.01f, 0.0f, 1.0f);
    const float motionScale = 1.0f - thr * HUD_MOTION_THROTTLE_FOCUS;
    const float impactScale = 1.0f - thr * HUD_MOTION_THROTTLE_IMPACT;
    targetX *= motionScale;
    targetY *= motionScale;
    impulseX *= motionScale;
    impulseY *= motionScale;
    impactX *= impactScale;
    impactY *= impactScale;

    // Impacts must move pixels immediately — velocity-only kicks die in a stiff damper.
    hudPosX += impactX * HUD_MOTION_IMPACT_POS;
    hudPosY += impactY * HUD_MOTION_IMPACT_POS;
    hudVelX += impulseX + impactX;
    hudVelY += impulseY + impactY;

    const float wn = HUD_MOTION_WN * (1.0f + thr * HUD_MOTION_THROTTLE_STIFFEN);
    const float damp = 2.0f * HUD_MOTION_ZETA * wn;
    const float accelX = (targetX - hudPosX) * (wn * wn) - hudVelX * damp;
    const float accelY = (targetY - hudPosY) * (wn * wn) - hudVelY * damp;
    hudVelX += accelX * dt;
    hudVelY += accelY * dt;
    hudPosX += hudVelX * dt;
    hudPosY += hudVelY * dt;

    hudPosX = constrainf(hudPosX, -HUD_MOTION_MAX_PX, HUD_MOTION_MAX_PX);
    hudPosY = constrainf(hudPosY, -HUD_MOTION_MAX_PX, HUD_MOTION_MAX_PX);

    // Settle exactly to zero when nearly still (avoids 1px chatter from rounding).
    if (fabsf(hudPosX) < 0.4f && fabsf(hudVelX) < 0.4f && fabsf(targetX) < 0.01f) {
        hudPosX = 0.0f;
        hudVelX = 0.0f;
    }
    if (fabsf(hudPosY) < 0.4f && fabsf(hudVelY) < 0.4f && fabsf(targetY) < 0.01f) {
        hudPosY = 0.0f;
        hudVelY = 0.0f;
    }

    max7456SetHudMotionOffset((int8_t)lrintf(hudPosX), (int8_t)lrintf(hudPosY));
}

static int grab(displayPort_t *displayPort)
{
    UNUSED(displayPort);

    return 0;
}

static int release(displayPort_t *displayPort)
{
    UNUSED(displayPort);

    return 0;
}

static int clearScreen(displayPort_t *displayPort, displayClearOption_e options)
{
    UNUSED(displayPort);
    UNUSED(options);

    max7456Invert(displayPortProfileMax7456()->invert);
    max7456Brightness(displayPortProfileMax7456()->blackBrightness, displayPortProfileMax7456()->whiteBrightness);

    max7456ClearScreen();

    return 0;
}

// Return true if screen still being transferred
static bool drawScreen(displayPort_t *displayPort)
{
    UNUSED(displayPort);
    // Demo (esp. mid-glyph plasma/raster) owns Display SRAM. OSD TRANSFER still
    // runs while grabCount>0 and would sync the stale layer buffer over our
    // per-line rewrites → character-sized blocks instead of 2×1. SyncFlush /
    // RefreshAll call max7456DrawScreen() directly when the demo wants a push.
    if (osdDemoIsActive()) {
        return false;
    }
    return max7456DrawScreen();
}

static int screenSize(const displayPort_t *displayPort)
{
    UNUSED(displayPort);
    return maxScreenSize;
}

static int writeString(displayPort_t *displayPort, uint8_t x, uint8_t y, uint8_t attr, const char *text)
{
    UNUSED(displayPort);
    UNUSED(attr);

    max7456Write(x, y, text);

    return 0;
}

static int writeChar(displayPort_t *displayPort, uint8_t x, uint8_t y, uint8_t attr, uint8_t c)
{
    UNUSED(displayPort);
    UNUSED(attr);

    max7456WriteChar(x, y, c);

    return 0;
}

static bool isTransferInProgress(const displayPort_t *displayPort)
{
    UNUSED(displayPort);
    return max7456DmaInProgress();
}

static bool isSynced(const displayPort_t *displayPort)
{
    UNUSED(displayPort);
    return max7456BuffersSynced();
}

static void redraw(displayPort_t *displayPort)
{
    UNUSED(displayPort);

    if (!ARMING_FLAG(ARMED)) {
        max7456RefreshAll();
    }
}

static int heartbeat(displayPort_t *displayPort)
{
    UNUSED(displayPort);

    // (Re)Initialize MAX7456 at startup or stall is detected.
    const bool reinited = max7456ReInitIfRequired(false);
    if (reinited) {
        max7456HudMotionReset();
    }
    // Demo must tick even after a stall reinit — otherwise FX switches stall forever
    // while the first scene stays on the glass from the initial RefreshAll.
    if (osdDemoIsActive()) {
        osdDemoUpdate(micros());
    } else if (!reinited) {
        max7456HudMotionUpdate();
    }

    return reinited;
}

static uint32_t txBytesFree(const displayPort_t *displayPort)
{
    UNUSED(displayPort);
    return UINT32_MAX;
}

static bool layerSupported(displayPort_t *displayPort, displayPortLayer_e layer)
{
    UNUSED(displayPort);
    return max7456LayerSupported(layer);
}

static bool layerSelect(displayPort_t *displayPort, displayPortLayer_e layer)
{
    UNUSED(displayPort);
    return max7456LayerSelect(layer);
}

static bool layerCopy(displayPort_t *displayPort, displayPortLayer_e destLayer, displayPortLayer_e sourceLayer)
{
    UNUSED(displayPort);
    return max7456LayerCopy(destLayer, sourceLayer);
}

static bool writeFontCharacter(displayPort_t *displayPort, uint16_t addr, const osdCharacter_t *chr)
{
    UNUSED(displayPort);

    return max7456WriteNvm(addr, (const uint8_t *)chr);
}

static bool checkReady(displayPort_t *displayPort, bool rescan)
{
    UNUSED(displayPort);
    if (!max7456IsDeviceDetected()) {
        if (!rescan) {
            return false;
        } else {
            // Try to initialize the device
            if (max7456Init(max7456Config(), max7456VcdProfile, systemConfig()->cpu_overclock) != MAX7456_INIT_OK) {
                return false;
            }
            // At this point the device has been initialized and detected
            redraw(&max7456DisplayPort);
        }
    }

    return true;
}

static void setBackgroundType(displayPort_t *displayPort, displayPortBackground_e backgroundType)
{
    UNUSED(displayPort);
    max7456SetBackgroundType(backgroundType);
}

static const displayPortVTable_t max7456VTable = {
    .grab = grab,
    .release = release,
    .clearScreen = clearScreen,
    .drawScreen = drawScreen,
    .screenSize = screenSize,
    .writeString = writeString,
    .writeChar = writeChar,
    .isTransferInProgress = isTransferInProgress,
    .heartbeat = heartbeat,
    .redraw = redraw,
    .isSynced = isSynced,
    .txBytesFree = txBytesFree,
    .layerSupported = layerSupported,
    .layerSelect = layerSelect,
    .layerCopy = layerCopy,
    .writeFontCharacter = writeFontCharacter,
    .checkReady = checkReady,
    .setBackgroundType = setBackgroundType,
};

bool max7456DisplayPortInit(const vcdProfile_t *vcdProfile, displayPort_t **displayPort)
{
    max7456VcdProfile = vcdProfile;

    switch (max7456Init(max7456Config(), max7456VcdProfile, systemConfig()->cpu_overclock)) {
    case MAX7456_INIT_NOT_CONFIGURED:
        // MAX7456 IO pins are not defined. We either don't have
        // it on board or either the configuration for it has
        // not been set.
        *displayPort = NULL;

        return false;

        break;
    case MAX7456_INIT_NOT_FOUND:
        // MAX7456 IO pins are defined, but we could not get a reply
        // from it at this time. Delay full initialization to
        // checkReady() with 'rescan' enabled
        displayInit(&max7456DisplayPort, &max7456VTable, DISPLAYPORT_DEVICE_TYPE_MAX7456);
        *displayPort = &max7456DisplayPort;

        return false;

        break;
    case MAX7456_INIT_OK:
        // MAX7456 configured and detected
        displayInit(&max7456DisplayPort, &max7456VTable, DISPLAYPORT_DEVICE_TYPE_MAX7456);
        *displayPort = &max7456DisplayPort;

        break;
    }

    uint8_t displayRows;

    switch(vcdProfile->video_system) {
    default:
    case VIDEO_SYSTEM_PAL:
        displayRows = VIDEO_LINES_PAL;
        break;

    case VIDEO_SYSTEM_NTSC:
        displayRows = VIDEO_LINES_NTSC;
        break;

    case VIDEO_SYSTEM_AUTO:
        displayRows = max7456GetRowsCount();
        break;
    }

    max7456DisplayPort.rows = displayRows + displayPortProfileMax7456()->rowAdjust;
    max7456DisplayPort.cols = 30 + displayPortProfileMax7456()->colAdjust;

    return true;
}
#endif // USE_MAX7456
