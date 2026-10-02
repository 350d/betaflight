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
#include <string.h>

#include "platform.h"

#ifdef USE_MAX7456

#include "build/debug.h"
#include "common/maths.h"

#include "pg/max7456.h"
#include "pg/vcd.h"

#include "drivers/bus_spi.h"
#include "drivers/dma.h"
#include "drivers/io.h"
#include "drivers/light_led.h"
#include "drivers/max7456.h"
#include "drivers/nvic.h"
#include "drivers/osd.h"
#include "drivers/osd_symbols.h"
#include "drivers/system.h"
#include "drivers/time.h"

// 10 MHz max SPI frequency
#define MAX7456_MAX_SPI_CLK_HZ 10000000
#define MAX7456_INIT_MAX_SPI_CLK_HZ 5000000

// DEBUG_MAX7456_SIGNAL
#define DEBUG_MAX7456_SIGNAL_MODEREG       0
#define DEBUG_MAX7456_SIGNAL_SENSE         1
#define DEBUG_MAX7456_SIGNAL_REINIT        2
#define DEBUG_MAX7456_SIGNAL_ROWS          3

// DEBUG_MAX7456_SPICLOCK
#define DEBUG_MAX7456_SPICLOCK_OVERCLOCK   0
#define DEBUG_MAX7456_SPICLOCK_DEVTYPE     1
#define DEBUG_MAX7456_SPICLOCK_DIVISOR     2
#define DEBUG_MAX7456_SPICLOCK_X100        3

// VM0 bits
#define VIDEO_BUFFER_DISABLE        0x01
#define MAX7456_RESET               0x02
#define VERTICAL_SYNC_NEXT_VSYNC    0x04
#define OSD_ENABLE                  0x08

#define SYNC_MODE_AUTO              0x00
#define SYNC_MODE_INTERNAL          0x30
#define SYNC_MODE_EXTERNAL          0x20

#define VIDEO_MODE_PAL              0x40
#define VIDEO_MODE_NTSC             0x00
#define VIDEO_MODE_MASK             0x40
#define VIDEO_MODE_IS_PAL(val)      (((val) & VIDEO_MODE_MASK) == VIDEO_MODE_PAL)
#define VIDEO_MODE_IS_NTSC(val)     (((val) & VIDEO_MODE_MASK) == VIDEO_MODE_NTSC)

#define VIDEO_SIGNAL_DEBOUNCE_MS    100 // Time to wait for input to stabilize

// VM1 bits

// duty cycle is on_off
#define BLINK_DUTY_CYCLE_50_50 0x00
#define BLINK_DUTY_CYCLE_33_66 0x01
#define BLINK_DUTY_CYCLE_25_75 0x02
#define BLINK_DUTY_CYCLE_75_25 0x03

// blinking time
#define BLINK_TIME_0 0x00
#define BLINK_TIME_1 0x04
#define BLINK_TIME_2 0x08
#define BLINK_TIME_3 0x0C

// background mode brightness (percent)
#define BACKGROUND_BRIGHTNESS_0 0x00
#define BACKGROUND_BRIGHTNESS_7 0x01
#define BACKGROUND_BRIGHTNESS_14 0x02
#define BACKGROUND_BRIGHTNESS_21 0x03
#define BACKGROUND_BRIGHTNESS_28 0x04
#define BACKGROUND_BRIGHTNESS_35 0x05
#define BACKGROUND_BRIGHTNESS_42 0x06
#define BACKGROUND_BRIGHTNESS_49 0x07

#define BACKGROUND_MODE_GRAY 0x80

// STAT register bits

#define STAT_PAL      0x01
#define STAT_NTSC     0x02
#define STAT_LOS      0x04
#define STAT_HSYNC    0x08  // STAT[3] — HSYNC output level (~HSYNC)
#define STAT_VSYNC    0x10  // STAT[4] — VSYNC output level (~VSYNC, falls at VSYNC start)
#define STAT_NVR_BUSY 0x20

#define STAT_IS_PAL(val)  ((val) & STAT_PAL)
#define STAT_IS_NTSC(val) ((val) & STAT_NTSC)
#define STAT_IS_LOS(val)  ((val) & STAT_LOS)
#define STAT_IS_VSYNC_HIGH(val)  (((val) & STAT_VSYNC) != 0)
#define STAT_IS_HSYNC_HIGH(val)  (((val) & STAT_HSYNC) != 0)

#define VIN_IS_PAL(val)  (!STAT_IS_LOS(val) && STAT_IS_PAL(val))
#define VIN_IS_NTSC(val)  (!STAT_IS_LOS(val) && STAT_IS_NTSC(val))

// DMM register bits
#define DMM_AUTO_INC 0x01

// Kluege warning!
// There are occasions that NTSC is not detected even with !LOS (AB7456 specific?)
// When this happens, lower 3 bits of STAT register is read as zero.
// To cope with this case, this macro defines !LOS && !PAL as NTSC.
// Should be compatible with MAX7456 and non-problematic case.

#define VIN_IS_NTSC_alt(val)  (!STAT_IS_LOS(val) && !STAT_IS_PAL(val))

#define MAX7456_SIGNAL_CHECK_INTERVAL_MS 1000 // msec
#define MAX7456_STALL_CHECK_INTERVAL_MS  1000 // msec

// DMM special bits
#define CLEAR_DISPLAY 0x04
#define CLEAR_DISPLAY_VERT 0x06
#define INVERT_PIXEL_COLOR 0x08
#define DMM_8BIT_MODE 0x40          // DMM[6]: 1 = 8-bit display-memory ops
#define DMAH_ATTR_SELECT 0x02       // DMAH[1]: 1 = attribute byte (8-bit mode only)
#define CHAR_ATTR_INV 0x01          // per-cell attribute bit0

// Special address for terminating incremental write
#define END_STRING 0xff

#define MAX7456ADD_READ         0x80
#define MAX7456ADD_VM0          0x00  //0b0011100// 00 // 00             ,0011100
#define MAX7456ADD_VM1          0x01
#define MAX7456ADD_HOS          0x02
#define MAX7456ADD_VOS          0x03
#define MAX7456ADD_DMM          0x04
#define MAX7456ADD_DMAH         0x05
#define MAX7456ADD_DMAL         0x06
#define MAX7456ADD_DMDI         0x07
#define MAX7456ADD_CMM          0x08
#define MAX7456ADD_CMAH         0x09
#define MAX7456ADD_CMAL         0x0a
#define MAX7456ADD_CMDI         0x0b
#define MAX7456ADD_OSDM         0x0c
#define MAX7456ADD_RB0          0x10
#define MAX7456ADD_RB1          0x11
#define MAX7456ADD_RB2          0x12
#define MAX7456ADD_RB3          0x13
#define MAX7456ADD_RB4          0x14
#define MAX7456ADD_RB5          0x15
#define MAX7456ADD_RB6          0x16
#define MAX7456ADD_RB7          0x17
#define MAX7456ADD_RB8          0x18
#define MAX7456ADD_RB9          0x19
#define MAX7456ADD_RB10         0x1a
#define MAX7456ADD_RB11         0x1b
#define MAX7456ADD_RB12         0x1c
#define MAX7456ADD_RB13         0x1d
#define MAX7456ADD_RB14         0x1e
#define MAX7456ADD_RB15         0x1f
#define MAX7456ADD_OSDBL        0x6c
#define MAX7456ADD_STAT         0xA0

#define NVM_RAM_SIZE            54
#define WRITE_NVR               0xA0

// Device type. An enum rather than two #defines so that the debug annotation
// on DEBUG_MAX7456_SPICLOCK_DEVTYPE can name it, and a consumer reads the
// device name instead of the number.
typedef enum {
    MAX7456_DEVICE_TYPE_MAX = 0,
    MAX7456_DEVICE_TYPE_AT
} max7456DeviceType_e;

#define CHARS_PER_LINE      30 // XXX Should be related to VIDEO_BUFFER_CHARS_*?

#define MAX7456_SUPPORTED_LAYER_COUNT (DISPLAYPORT_LAYER_BACKGROUND + 1)

typedef struct max7456Layer_s {
    uint8_t buffer[VIDEO_BUFFER_CHARS_PAL];
} max7456Layer_t;

static max7456Layer_t displayLayers[MAX7456_SUPPORTED_LAYER_COUNT];
static displayPortLayer_e activeLayer = DISPLAYPORT_LAYER_FOREGROUND;

extDevice_t max7456Device;
extDevice_t *dev = &max7456Device;

static bool max7456DeviceDetected = false;
static uint16_t max7456SpiClockDiv;
static volatile bool max7456ActiveDma = false;

uint16_t maxScreenSize = VIDEO_BUFFER_CHARS_PAL;

// We write everything to the active layer and then compare
// it with shadowBuffer to update only changed chars.
// This solution is faster then redrawing entire screen.

static uint8_t shadowBuffer[VIDEO_BUFFER_CHARS_PAL];
// Per-cell INV attribute (MAX7456 DMM[3] / char attr). Parallel to char buffers.
static uint8_t invertLayer[VIDEO_BUFFER_CHARS_PAL];
static uint8_t shadowInvert[VIDEO_BUFFER_CHARS_PAL];

//Max bytes to update in one call to max7456DrawScreen()

#define MAX_BYTES2SEND          250
#define MAX_BYTES2SEND_POLLED   12
#define MAX_ENCODE_US           20
#define MAX_ENCODE_US_POLLED    10

static DMA_DATA uint8_t spiBuf[MAX_BYTES2SEND];

// Screen-draw cursor; exposed via max7456IsFrameIdle().
static uint16_t max7456DrawPos = 0;

static uint8_t  videoSignalCfg;
static uint8_t  videoSignalReg  = OSD_ENABLE; // OSD_ENABLE required to trigger first ReInit
static uint8_t  displayMemoryModeReg = 0;

static uint8_t  hosRegValue; // HOS (Horizontal offset register) value
static bool midGlyphSpiHot; // a mid-glyph engine owns chip + bus (defined with its helpers below)
static uint8_t  vosRegValue; // VOS (Vertical offset register) value

// Transient HUD inertia offset in screen pixels (+x right, +y down). Not part of vcd base.
static int8_t hudMotionOffsetX;
static int8_t hudMotionOffsetY;

static bool fontIsLoading       = false;

static uint8_t max7456DeviceType;

static displayPortBackground_e deviceBackgroundType = DISPLAY_BACKGROUND_TRANSPARENT;

// previous states initialized outside the valid range to force update on first call
#define INVALID_PREVIOUS_REGISTER_STATE 255
static uint8_t previousBlackWhiteRegister = INVALID_PREVIOUS_REGISTER_STATE;
static uint8_t previousInvertRegister = INVALID_PREVIOUS_REGISTER_STATE;
static uint8_t previousHosRegister = INVALID_PREVIOUS_REGISTER_STATE;
static uint8_t previousVosRegister = INVALID_PREVIOUS_REGISTER_STATE;
static uint8_t previousOsdmRegister = INVALID_PREVIOUS_REGISTER_STATE;

static uint8_t *getLayerBuffer(displayPortLayer_e layer)
{
    return displayLayers[layer].buffer;
}

static uint8_t *getActiveLayerBuffer(void)
{
    return getLayerBuffer(activeLayer);
}

// HOS 0..63, VOS 0..31. Higher HOS/VOS shifts OSD right/down on screen.
static void max7456ApplyHosVos(void)
{
    const int hos = constrain((int)hosRegValue + hudMotionOffsetX, 0, 63);
    const int vos = constrain((int)vosRegValue + hudMotionOffsetY, 0, 31);

    if (hos != previousHosRegister) {
        previousHosRegister = hos;
        spiWriteReg(dev, MAX7456ADD_HOS, hos);
    }
    if (vos != previousVosRegister) {
        previousVosRegister = vos;
        spiWriteReg(dev, MAX7456ADD_VOS, vos);
    }
}

void max7456SetHudMotionOffset(int8_t x, int8_t y)
{
    hudMotionOffsetX = x;
    hudMotionOffsetY = y;
}

void max7456ResetHudMotionOffset(void)
{
    hudMotionOffsetX = 0;
    hudMotionOffsetY = 0;
}

void max7456GetHudMotionYLimits(int8_t *minY, int8_t *maxY)
{
    // Final VOS = vosRegValue + offset, clamped to 0..31.
    // +offset shifts OSD downward on screen.
    if (minY) {
        *minY = (int8_t)(-vosRegValue);           // most upward (smallest VOS)
    }
    if (maxY) {
        *maxY = (int8_t)(31 - vosRegValue);        // most downward (largest VOS) = floor
    }
}

void max7456GetHudMotionXLimits(int8_t *minX, int8_t *maxX)
{
    // Final HOS = hosRegValue + offset, clamped to 0..63.
    // +offset shifts OSD rightward on screen.
    if (minX) {
        *minX = (int8_t)(-hosRegValue);           // most leftward
    }
    if (maxX) {
        *maxX = (int8_t)(63 - hosRegValue);       // most rightward
    }
}

static void max7456SetRegisterVM1(void)
{
    uint8_t backgroundGray = BACKGROUND_BRIGHTNESS_28; // this is the device default background gray level
    uint8_t vm1Register = BLINK_TIME_1 | BLINK_DUTY_CYCLE_75_25; // device defaults
    if (deviceBackgroundType != DISPLAY_BACKGROUND_TRANSPARENT) {
        vm1Register |= BACKGROUND_MODE_GRAY;
        switch (deviceBackgroundType) {
        case DISPLAY_BACKGROUND_BLACK:
            backgroundGray = BACKGROUND_BRIGHTNESS_0;
            break;
        case DISPLAY_BACKGROUND_LTGRAY:
            backgroundGray = BACKGROUND_BRIGHTNESS_49;
            break;
        case DISPLAY_BACKGROUND_GRAY:
        default:
            backgroundGray = BACKGROUND_BRIGHTNESS_28;
            break;
        }
    }
    vm1Register |= (backgroundGray << 4);
    spiWriteReg(dev, MAX7456ADD_VM1, vm1Register);
}

uint8_t max7456GetRowsCount(void)
{
    return (videoSignalReg & VIDEO_MODE_PAL) ? VIDEO_LINES_PAL : VIDEO_LINES_NTSC;
}

// When clearing the shadow buffer we fill with 0 so that the characters will
// be flagged as changed when compared to the 0x20 used in the layer buffers.
static void max7456ClearShadowBuffer(void)
{
    memset(shadowBuffer, 0, maxScreenSize);
    memset(shadowInvert, 0, maxScreenSize);
}

// Buffer is filled with the whitespace character (0x20)
static void max7456ClearLayer(displayPortLayer_e layer)
{
    memset(getLayerBuffer(layer), 0x20, VIDEO_BUFFER_CHARS_PAL);
    if (layer == activeLayer) {
        memset(invertLayer, 0, VIDEO_BUFFER_CHARS_PAL);
    }
}

static void max7456ReInit(void)
{
    uint8_t srdata = 0;

    switch (videoSignalCfg) {
    case VIDEO_SYSTEM_PAL:
        videoSignalReg = VIDEO_MODE_PAL | OSD_ENABLE;
        break;

    case VIDEO_SYSTEM_NTSC:
        videoSignalReg = VIDEO_MODE_NTSC | OSD_ENABLE;
        break;

    case VIDEO_SYSTEM_AUTO:
        srdata = spiReadRegMsk(dev, MAX7456ADD_STAT);

        if (VIN_IS_NTSC(srdata)) {
            videoSignalReg = VIDEO_MODE_NTSC | OSD_ENABLE;
        } else if (VIN_IS_PAL(srdata)) {
            videoSignalReg = VIDEO_MODE_PAL | OSD_ENABLE;
        } else {
            // No valid input signal, fallback to default (PAL)
            videoSignalReg = VIDEO_MODE_PAL | OSD_ENABLE;
        }
        break;
    }

    if (videoSignalReg & VIDEO_MODE_PAL) { //PAL
        maxScreenSize = VIDEO_BUFFER_CHARS_PAL;
    } else {              // NTSC
        maxScreenSize = VIDEO_BUFFER_CHARS_NTSC;
    }

    // Set all rows to same charactor black/white level
    previousBlackWhiteRegister = INVALID_PREVIOUS_REGISTER_STATE;
    max7456Brightness(0, 2);
    // Re-enable MAX7456 (last function call disables it)

    // Make sure the Max7456 is enabled
    spiWriteReg(dev, MAX7456ADD_VM0, videoSignalReg);
    // Reinit restores configured base position; drop any transient HUD offset.
    max7456ResetHudMotionOffset();
    previousHosRegister = INVALID_PREVIOUS_REGISTER_STATE;
    previousVosRegister = INVALID_PREVIOUS_REGISTER_STATE;
    previousOsdmRegister = INVALID_PREVIOUS_REGISTER_STATE;
    max7456ApplyHosVos();

    max7456SetRegisterVM1();

    // Clear shadow to force redraw all screen
    max7456ClearShadowBuffer();
}

void max7456Preinit(const max7456Config_t *max7456Config)
{
    ioPreinitByTag(max7456Config->csTag, max7456Config->preInitOPU ? IOCFG_OUT_PP : IOCFG_IPU, PREINIT_PIN_STATE_HIGH);
}

// Here we init only CS and try to init MAX for first time.
// Also detect device type (MAX v.s. AT)

max7456InitStatus_e max7456Init(const max7456Config_t *max7456Config, const vcdProfile_t *pVcdProfile, bool cpuOverclock)
{
    max7456DeviceDetected = false;
    deviceBackgroundType = DISPLAY_BACKGROUND_TRANSPARENT;

    // initialize all layers
    for (unsigned i = 0; i < MAX7456_SUPPORTED_LAYER_COUNT; i++) {
        max7456ClearLayer(i);
    }

    max7456HardwareReset();

    if (!max7456Config->csTag || !spiSetBusInstance(dev, max7456Config->spiDevice)) {
        return MAX7456_INIT_NOT_CONFIGURED;
    }

    dev->busType_u.spi.csnPin = IOGetByTag(max7456Config->csTag);

    // Allow re-probe when a prior NOT_FOUND already claimed CS (checkReady rescan).
    // Any other owner means the pin is not ours — abort.
    if (!IOIsFreeOrPreinit(dev->busType_u.spi.csnPin)
        && IOGetOwner(dev->busType_u.spi.csnPin) != OWNER_OSD_CS) {
        return MAX7456_INIT_NOT_CONFIGURED;
    }

    IOInit(dev->busType_u.spi.csnPin, OWNER_OSD_CS, 0);
    IOConfigGPIO(dev->busType_u.spi.csnPin, SPI_IO_CS_CFG);
    IOHi(dev->busType_u.spi.csnPin);

    // Detect MAX7456 / AT7456 by an OSDM write/read-back.
    // Do NOT assume power-on default 0x1B: osd_demo (and max7456Osdm) change OSDM
    // at runtime, and a soft FC reboot does not power-cycle the OSD chip — a stale
    // non-0x1B value made the old "read default only" probe fail forever.

    spiSetClkDivisor(dev, spiCalculateDivider(MAX7456_INIT_MAX_SPI_CLK_HZ));

    // Write 0xff to conclude any current SPI transaction the MAX7456 is expecting
    spiWrite(dev, END_STRING);

    spiWriteReg(dev, MAX7456ADD_OSDM, 0x1B);
    previousOsdmRegister = 0x1B;
    uint8_t osdm = spiReadRegMsk(dev, MAX7456ADD_OSDM);

    if (osdm != 0x1B) {
        // The chip keeps power across FC reflash / soft reboot, so it can still be inside
        // an auto-increment session (16-bit framed or 8-bit data-only, datasheet p.29) and
        // eat every register write. Escape in both framings, then VM0[1] software reset
        // (all registers to defaults, display memory cleared, ~100 us; p.25).
        spiWrite(dev, END_STRING);
        spiWriteReg(dev, MAX7456ADD_DMDI, END_STRING);
        spiWrite(dev, END_STRING);
        spiWriteReg(dev, MAX7456ADD_VM0, MAX7456_RESET);
        delayMicroseconds(1000);
        spiWriteReg(dev, MAX7456ADD_OSDM, 0x1B);
        osdm = spiReadRegMsk(dev, MAX7456ADD_OSDM);
    }

    if (osdm != 0x1B) {
        IOConfigGPIO(dev->busType_u.spi.csnPin, IOCFG_IPU);
        IORelease(dev->busType_u.spi.csnPin);
        return MAX7456_INIT_NOT_FOUND;
    }

    // At this point, we can claim the ownership of the CS pin
    max7456DeviceDetected = true;
    IOInit(dev->busType_u.spi.csnPin, OWNER_OSD_CS, 0);

    // Detect device type by writing and reading CA[8] bit at CMAL[6].
    // This is a bit for accessing second half of character glyph storage, supported only by AT variant.

    spiWriteReg(dev, MAX7456ADD_CMAL, (1 << 6)); // CA[8] bit

    if (spiReadRegMsk(dev, MAX7456ADD_CMAL) & (1 << 6)) {
        max7456DeviceType = MAX7456_DEVICE_TYPE_AT;
    } else {
        max7456DeviceType = MAX7456_DEVICE_TYPE_MAX;
    }

#if defined(USE_OVERCLOCK)
    // Determine SPI clock divisor based on config and the device type.

    switch (max7456Config->clockConfig) {
    case MAX7456_CLOCK_CONFIG_HALF:
        max7456SpiClockDiv = spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ / 2);
        break;

    case MAX7456_CLOCK_CONFIG_NOMINAL:
    default:
        max7456SpiClockDiv = spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ);
        break;

    case MAX7456_CLOCK_CONFIG_DOUBLE:
        max7456SpiClockDiv = spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ * 2);
        break;
    }

    DEBUG_SET(DEBUG_MAX7456_SPICLOCK, DEBUG_MAX7456_SPICLOCK_OVERCLOCK, cpuOverclock);                              //!< CPU Overclocked
    DEBUG_SET(DEBUG_MAX7456_SPICLOCK, DEBUG_MAX7456_SPICLOCK_DEVTYPE, max7456DeviceType);                           //!< Device Type [enum:max7456DeviceType_e]
    DEBUG_SET(DEBUG_MAX7456_SPICLOCK, DEBUG_MAX7456_SPICLOCK_DIVISOR, max7456SpiClockDiv);                          //!< SPI Clock Divisor
    DEBUG_SET(DEBUG_MAX7456_SPICLOCK, DEBUG_MAX7456_SPICLOCK_X100, spiCalculateClock(max7456SpiClockDiv) / 10000);  //!< SPI Clock [unit:0.01MHz]
#else
    UNUSED(max7456Config);
    UNUSED(cpuOverclock);
    max7456SpiClockDiv = spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ);
#endif

    spiSetClkDivisor(dev, max7456SpiClockDiv);

    // force soft reset on Max7456
    spiWriteReg(dev, MAX7456ADD_VM0, MAX7456_RESET);

    // Wait for 200us before polling for completion of reset
    delayMicroseconds(200);

    // Wait for reset to complete
    while ((spiReadRegMsk(dev, MAX7456ADD_VM0) & MAX7456_RESET) != 0x00);

    // Setup values to write to registers
    videoSignalCfg = pVcdProfile->video_system;
    hosRegValue = 32 - pVcdProfile->h_offset;
    vosRegValue = 16 - pVcdProfile->v_offset;

    // Real init will be made later when driver detect idle.
    return MAX7456_INIT_OK;
}

/**
 * Sets inversion of black and white pixels.
 */
void max7456Invert(bool invert)
{
    if (invert) {
        displayMemoryModeReg |= INVERT_PIXEL_COLOR;
    } else {
        displayMemoryModeReg &= ~INVERT_PIXEL_COLOR;
    }

    if (displayMemoryModeReg != previousInvertRegister) {
        // clear the shadow buffer so all characters will be
        // redrawn with the proper invert state
        max7456ClearShadowBuffer();
        previousInvertRegister = displayMemoryModeReg;
        spiWriteReg(dev, MAX7456ADD_DMM, displayMemoryModeReg);
    }
}

/**
 * Sets the brightness of black and white pixels.
 *
 * @param black Black brightness (0-3, 0 is darkest)
 * @param white White brightness (0-3, 0 is darkest)
 */
void max7456Brightness(uint8_t black, uint8_t white)
{
    const uint8_t reg = (black << 2) | (3 - white);

    if (reg != previousBlackWhiteRegister) {
        previousBlackWhiteRegister = reg;
        STATIC_DMA_DATA_AUTO uint8_t buf[32];
        for (int i = MAX7456ADD_RB0, j = 0; i <= MAX7456ADD_RB15; i++) {
            buf[j++] = i;
            buf[j++] = reg;
        }
        spiReadWriteBuf(dev, buf, NULL, sizeof(buf));
    }
}

/**
 * Per-row black/white brightness (MAX7456 RB0..RB15).
 * black/white: 0 = darkest, 3 = brightest.
 */
void max7456BrightnessRow(uint8_t row, uint8_t black, uint8_t white)
{
    if (row > 15) {
        return;
    }
    const uint8_t reg = (uint8_t)(((black & 3) << 2) | (3 - (white & 3)));
    // Invalidate the "all rows equal" cache so a later max7456Brightness() rewrites.
    previousBlackWhiteRegister = INVALID_PREVIOUS_REGISTER_STATE;
    spiWriteReg(dev, (uint8_t)(MAX7456ADD_RB0 + row), reg);
}

/**
 * OSD Insertion Mux (OSDM): trade sharpness vs cross-color/cross-luma.
 * value bits[5:3] = OSD pixel rise/fall (0=20ns .. 5=110ns),
 *       bits[2:0] = video↔OSD mux switch (0=30ns .. 5=120ns).
 * Datasheet default is 0x1B (both fields = 3).
 */
void max7456Osdm(uint8_t value)
{
    // Only codes 0..5 are valid in each field; clamp nibble-wise.
    const uint8_t rise = (uint8_t)MIN(value >> 3, 5);
    const uint8_t mux = (uint8_t)MIN(value & 7, 5);
    const uint8_t reg = (uint8_t)((rise << 3) | mux);
    if (reg != previousOsdmRegister) {
        previousOsdmRegister = reg;
        spiWriteReg(dev, MAX7456ADD_OSDM, reg);
    }
}

void max7456ClearScreen(void)
{
    max7456ClearLayer(activeLayer);
}

void max7456FillScreen(uint8_t c)
{
    memset(getActiveLayerBuffer(), c, maxScreenSize);
    memset(invertLayer, 0, maxScreenSize);
}

void max7456Invalidate(void)
{
    // Force every cell dirty vs shadow so drawScreen rewrites the whole screen.
    memset(shadowBuffer, 0xFF, maxScreenSize);
    memset(shadowInvert, 0xFF, maxScreenSize);
    max7456DrawPos = 0;
}

void max7456WriteCharEx(uint8_t x, uint8_t y, uint8_t c, bool invert)
{
    if (x < CHARS_PER_LINE && y < VIDEO_LINES_PAL) {
        const uint16_t pos = (uint16_t)(y * CHARS_PER_LINE + x);
        getActiveLayerBuffer()[pos] = c;
        invertLayer[pos] = invert ? 1 : 0;
    }
}

void max7456WriteChar(uint8_t x, uint8_t y, uint8_t c)
{
    max7456WriteCharEx(x, y, c, false);
}

void max7456Write(uint8_t x, uint8_t y, const char *text)
{
    if (y < VIDEO_LINES_PAL) {
        uint8_t *buffer = getActiveLayerBuffer();
        const uint32_t bufferYOffset = y * CHARS_PER_LINE;
        for (int i = 0, bufferXOffset = x; text[i] && bufferXOffset < CHARS_PER_LINE; i++, bufferXOffset++) {
            const uint16_t pos = (uint16_t)(bufferYOffset + bufferXOffset);
            buffer[pos] = text[i];
            invertLayer[pos] = 0;
        }
    }
}

bool max7456LayerSupported(displayPortLayer_e layer)
{
    if (layer == DISPLAYPORT_LAYER_FOREGROUND) {
        return true;
    } else {
        return false;
    }
}

bool max7456LayerSelect(displayPortLayer_e layer)
{
    if (max7456LayerSupported(layer)) {
        activeLayer = layer;
        return true;
    } else {
        return false;
    }
}

bool max7456LayerCopy(displayPortLayer_e destLayer, displayPortLayer_e sourceLayer)
{
    if ((sourceLayer != destLayer) && max7456LayerSupported(sourceLayer) && max7456LayerSupported(destLayer)) {
        memcpy(getLayerBuffer(destLayer), getLayerBuffer(sourceLayer), VIDEO_BUFFER_CHARS_PAL);
        return true;
    } else {
        return false;
    }
}

bool max7456DmaInProgress(void)
{
    return max7456ActiveDma;
}

bool max7456BuffersSynced(void)
{
    for (int i = 0; i < maxScreenSize; i++) {
        if (displayLayers[DISPLAYPORT_LAYER_FOREGROUND].buffer[i] != shadowBuffer[i]
            || invertLayer[i] != shadowInvert[i]) {
            return false;
        }
    }
    return true;
}

bool max7456ReInitIfRequired(bool forceStallCheck)
{
    static timeMs_t lastSigCheckMs = 0;
    static timeMs_t videoDetectTimeMs = 0;
    static uint16_t reInitCount = 0;
    static timeMs_t lastStallCheckMs = MAX7456_STALL_CHECK_INTERVAL_MS / 2; // offset so that it doesn't coincide with the signal check

    // A mid-glyph engine owns the chip and the bus (scene 10 bursts from a TIM5 interrupt while
    // this runs in the OSD task): a VM0 read here could interleave with a raster burst, read
    // garbage, "detect a stall" and re-init the chip mid-scene. Skip checks while hot.
    if (midGlyphSpiHot) {
        return false;
    }

    const timeMs_t nowMs = millis();

    bool stalled = false;
    if (forceStallCheck || (lastStallCheckMs + MAX7456_STALL_CHECK_INTERVAL_MS < nowMs)) {
        lastStallCheckMs = nowMs;

        // Write 0xff to conclude any current SPI transaction the MAX7456 is expecting
        spiWrite(dev, END_STRING);

        stalled = (spiReadRegMsk(dev, MAX7456ADD_VM0) != videoSignalReg);
    }

    if (stalled) {
        max7456ReInit();
    } else if ((videoSignalCfg == VIDEO_SYSTEM_AUTO)
              && ((nowMs - lastSigCheckMs) > MAX7456_SIGNAL_CHECK_INTERVAL_MS)) {

        // Write 0xff to conclude any current SPI transaction the MAX7456 is expecting
        spiWrite(dev, END_STRING);

        // Adjust output format based on the current input format.

        const uint8_t videoSense = spiReadRegMsk(dev, MAX7456ADD_STAT);

        DEBUG_SET(DEBUG_MAX7456_SIGNAL, DEBUG_MAX7456_SIGNAL_MODEREG, videoSignalReg & VIDEO_MODE_MASK);  //!< Video Mode Register
        DEBUG_SET(DEBUG_MAX7456_SIGNAL, DEBUG_MAX7456_SIGNAL_SENSE, videoSense & 0x7);                    //!< Video Sense
        DEBUG_SET(DEBUG_MAX7456_SIGNAL, DEBUG_MAX7456_SIGNAL_ROWS, max7456GetRowsCount());                //!< Row Count

        if (videoSense & STAT_LOS) {
            videoDetectTimeMs = 0;
        } else {
            if ((VIN_IS_PAL(videoSense) && VIDEO_MODE_IS_NTSC(videoSignalReg))
              || (VIN_IS_NTSC_alt(videoSense) && VIDEO_MODE_IS_PAL(videoSignalReg))) {
                if (videoDetectTimeMs) {
                    if (millis() - videoDetectTimeMs > VIDEO_SIGNAL_DEBOUNCE_MS) {
                        max7456ReInit();
                        DEBUG_SET(DEBUG_MAX7456_SIGNAL, DEBUG_MAX7456_SIGNAL_REINIT, ++reInitCount);  //!< Reinit Count
                    }
                } else {
                    // Wait for signal to stabilize
                    videoDetectTimeMs = millis();
                }
            }
        }

        lastSigCheckMs = nowMs;
    }

    return stalled;
}

// Called in ISR context
static busStatus_e max7456_callbackReady(uintptr_t arg)
{
    UNUSED(arg);

    max7456ActiveDma = false;

    return BUS_READY;
}

// Return true if screen still being transferred
bool max7456DrawScreen(void)
{
    // This routine doesn't block so need to use static data
    static busSegment_t segments[] = {
            {.u.link = {NULL, NULL}, 0, true, max7456_callbackReady},
            {.u.link = {NULL, NULL}, 0, true, NULL},
    };

    if (!fontIsLoading) {
        uint8_t *buffer = getActiveLayerBuffer();
        int spiBufIndex = 0;
        int maxSpiBufStartIndex;
        timeDelta_t maxEncodeTime;
        bool setAddress = true;
        bool autoInc = false;
        int posLimit = max7456DrawPos + (maxScreenSize / 2);

#ifdef USE_DMA
        const bool useDma = spiUseSDO_DMA(dev);
#else
        const bool useDma = false;
#endif
        maxSpiBufStartIndex = useDma ? MAX_BYTES2SEND : MAX_BYTES2SEND_POLLED;
        maxEncodeTime = useDma ? MAX_ENCODE_US : MAX_ENCODE_US_POLLED;

        // Abort for now if the bus is still busy
        if (spiIsBusy(dev)) {
            // Not finished yet
            return true;
        }

        // NOTE: Do NOT apply HOS/VOS here at pass start. On soft-scroll wrap
        // (HOS → 0 + cell advance) that would snap the old character grid right
        // before the new cells are written. Apply after the pass completes.

        timeUs_t startTime = micros();

        // Allow for an ESCAPE, a reset of DMM and a two byte MAX7456ADD_DMM command at end of buffer
        // Extra headroom: DMM updates when per-char INV attribute changes mid-pass.
        maxSpiBufStartIndex -= 8;

        // 16-bit mode copies DMM[5:3] (LBC/BLK/INV) into each written character's attribute.
        uint8_t drawDmm = displayMemoryModeReg & (uint8_t)~INVERT_PIXEL_COLOR;

        // Initialise the transfer buffer
        while ((spiBufIndex < maxSpiBufStartIndex) && (max7456DrawPos < posLimit) && (cmpTimeUs(micros(), startTime) < maxEncodeTime)) {
            const bool charDirty = buffer[max7456DrawPos] != shadowBuffer[max7456DrawPos];
            const bool invDirty = invertLayer[max7456DrawPos] != shadowInvert[max7456DrawPos];
            if (charDirty || invDirty) {
                if (buffer[max7456DrawPos] == 0xff) {
                    buffer[max7456DrawPos] = ' ';
                }

                const uint8_t wantDmm = invertLayer[max7456DrawPos]
                    ? (uint8_t)(displayMemoryModeReg | INVERT_PIXEL_COLOR)
                    : (uint8_t)(displayMemoryModeReg & (uint8_t)~INVERT_PIXEL_COLOR);

                if (setAddress || !autoInc || wantDmm != drawDmm) {
                    if (autoInc && !setAddress) {
                        spiBuf[spiBufIndex++] = MAX7456ADD_DMDI;
                        spiBuf[spiBufIndex++] = END_STRING;
                    }

                    // Peek ahead: auto-inc only when next cell shares char+inv dirty streak and same INV.
                    bool nextDirty = false;
                    bool nextSameInv = false;
                    if (max7456DrawPos + 1 < maxScreenSize) {
                        nextDirty = (buffer[max7456DrawPos + 1] != shadowBuffer[max7456DrawPos + 1])
                            || (invertLayer[max7456DrawPos + 1] != shadowInvert[max7456DrawPos + 1]);
                        nextSameInv = invertLayer[max7456DrawPos + 1] == invertLayer[max7456DrawPos];
                    }

                    drawDmm = wantDmm;
                    if (nextDirty && nextSameInv) {
                        spiBuf[spiBufIndex++] = MAX7456ADD_DMM;
                        spiBuf[spiBufIndex++] = drawDmm | DMM_AUTO_INC;
                        autoInc = true;
                    } else {
                        spiBuf[spiBufIndex++] = MAX7456ADD_DMM;
                        spiBuf[spiBufIndex++] = drawDmm;
                        autoInc = false;
                    }

                    spiBuf[spiBufIndex++] = MAX7456ADD_DMAH;
                    spiBuf[spiBufIndex++] = max7456DrawPos >> 8;
                    spiBuf[spiBufIndex++] = MAX7456ADD_DMAL;
                    spiBuf[spiBufIndex++] = max7456DrawPos & 0xff;

                    setAddress = false;
                }

                spiBuf[spiBufIndex++] = MAX7456ADD_DMDI;
                spiBuf[spiBufIndex++] = buffer[max7456DrawPos];

                shadowBuffer[max7456DrawPos] = buffer[max7456DrawPos];
                shadowInvert[max7456DrawPos] = invertLayer[max7456DrawPos];
            } else {
                if (!setAddress) {
                    setAddress = true;
                    if (autoInc) {
                        spiBuf[spiBufIndex++] = MAX7456ADD_DMDI;
                        spiBuf[spiBufIndex++] = END_STRING;
                    }
                }
            }

            if (++max7456DrawPos >= maxScreenSize) {
                max7456DrawPos = 0;
                // All dirty cells for this buffer are now queued/sent — apply HUD
                // motion so HOS wrap lands with the matching character grid.
                max7456ApplyHosVos();
                break;
            }
        }

        if (autoInc) {
            if (!setAddress) {
                spiBuf[spiBufIndex++] = MAX7456ADD_DMDI;
                spiBuf[spiBufIndex++] = END_STRING;
            }

            spiBuf[spiBufIndex++] = MAX7456ADD_DMM;
            spiBuf[spiBufIndex++] = displayMemoryModeReg;
        }

        if (spiBufIndex) {
            segments[0].u.buffers.txData = spiBuf;
            segments[0].len = spiBufIndex;

            max7456ActiveDma = true;

            spiSequence(dev, &segments[0]);

            // Non-blocking, so transfer still in progress if using DMA
        }
    }

    return (max7456DrawPos != 0);
}

bool max7456IsFrameIdle(void)
{
    return !fontIsLoading
        && !max7456ActiveDma
        && !spiIsBusy(dev)
        && max7456DrawPos == 0
        && max7456BuffersSynced();
}

// should not be used when armed
void max7456RefreshAll(void)
{
    max7456ReInitIfRequired(true);
    while (max7456DrawScreen());
}

bool max7456WriteNvm(uint8_t char_address, const uint8_t *font_data)
{
    if (!max7456DeviceDetected) {
        return false;
    }

    // Block pending completion of any prior SPI access
    spiWait(dev);

    // disable display
    fontIsLoading = true;
    spiWriteReg(dev, MAX7456ADD_VM0, 0);

    spiWriteReg(dev, MAX7456ADD_CMAH, char_address); // set start address high

    for (int x = 0; x < 54; x++) {
        spiWriteReg(dev, MAX7456ADD_CMAL, x); //set start address low
        spiWriteReg(dev, MAX7456ADD_CMDI, font_data[x]);
#ifdef LED0_TOGGLE
        LED0_TOGGLE;
#else
        LED1_TOGGLE;
#endif
    }

    // Transfer 54 bytes from shadow ram to NVM

    spiWriteReg(dev, MAX7456ADD_CMM, WRITE_NVR);

    // Wait until bit 5 in the status register returns to 0 (12ms)

    while ((spiReadRegMsk(dev, MAX7456ADD_STAT) & STAT_NVR_BUSY) != 0x00);

    return true;
}

void max7456EndFontWrite(void)
{
    // WriteNvm leaves OSD disabled (VM0=0) and blocks drawScreen via fontIsLoading.
    fontIsLoading = false;
    max7456ReInit();
}

#ifdef MAX7456_NRST_PIN
static IO_t max7456ResetPin        = IO_NONE;
#endif

void max7456HardwareReset(void)
{
#ifdef MAX7456_NRST_PIN
#define IO_RESET_CFG      IO_CONFIG(GPIO_Mode_OUT, GPIO_Speed_2MHz, GPIO_OType_PP, GPIO_PuPd_DOWN)

    max7456ResetPin = IOGetByTag(IO_TAG(MAX7456_NRST_PIN));
    IOInit(max7456ResetPin, OWNER_OSD, 0);
    IOConfigGPIO(max7456ResetPin, IO_RESET_CFG);

    // RESET 50ms long pulse, followed by 100us pause
    IOLo(max7456ResetPin);
    delay(50);
    IOHi(max7456ResetPin);
    delayMicroseconds(100);
#else
    // Allow device 50ms to powerup
    delay(50);
#endif
}

bool max7456IsDeviceDetected(void)
{
    return max7456DeviceDetected;
}

void max7456SetBackgroundType(displayPortBackground_e backgroundType)
{
    deviceBackgroundType = backgroundType;

    max7456SetRegisterVM1();
}

// Wait until the draw DMA/SPI path is idle enough for a polled register poke.
static bool max7456WaitSpiIdle(void)
{
    if (!max7456DeviceDetected || fontIsLoading) {
        return false;
    }
    // Finish any in-flight DMA segment before stealing the bus. DWT timeout: this also runs
    // inside the scene-10 TIM5 interrupt, where micros() does not advance past 1 ms.
    const uint32_t spinStart = getCycleCounter();
    const uint32_t spinTicks = clockMicrosToCycles(5000);
    while (max7456ActiveDma || spiIsBusy(dev)) {
        if (getCycleCounter() - spinStart > spinTicks) {
            return false;
        }
    }
    // Conclude any dangling auto-increment transaction the chip may expect.
    spiWrite(dev, END_STRING);
    return true;
}

void max7456ApplyHudMotionNow(void)
{
    if (!max7456WaitSpiIdle()) {
        return;
    }
    max7456ApplyHosVos();
}

static void max7456RestoreDisplayMemoryMode(void)
{
    spiWriteReg(dev, MAX7456ADD_DMM, displayMemoryModeReg);
    previousInvertRegister = displayMemoryModeReg;
}

bool max7456WriteDisplaySramChar(uint16_t addr, uint8_t glyph)
{
    if (addr >= maxScreenSize) {
        return false;
    }
    if (!max7456WaitSpiIdle()) {
        return false;
    }

    // 8-bit mode: DMDI writes Character Address only (DMAH[1]=0). Attribute untouched.
    spiWriteReg(dev, MAX7456ADD_DMM, DMM_8BIT_MODE);
    spiWriteReg(dev, MAX7456ADD_DMAH, (uint8_t)((addr >> 8) & 0x01));
    spiWriteReg(dev, MAX7456ADD_DMAL, (uint8_t)(addr & 0xff));
    spiWriteReg(dev, MAX7456ADD_DMDI, glyph);
    max7456RestoreDisplayMemoryMode();
    return true;
}

bool max7456WriteDisplaySramAttr(uint16_t addr, uint8_t attr)
{
    if (addr >= maxScreenSize) {
        return false;
    }
    if (!max7456WaitSpiIdle()) {
        return false;
    }

    // 8-bit mode: DMAH[1]=1 directs DMDI to the per-cell attribute byte (INV/BLK/LBC).
    spiWriteReg(dev, MAX7456ADD_DMM, DMM_8BIT_MODE);
    spiWriteReg(dev, MAX7456ADD_DMAH, (uint8_t)(((addr >> 8) & 0x01) | DMAH_ATTR_SELECT));
    spiWriteReg(dev, MAX7456ADD_DMAL, (uint8_t)(addr & 0xff));
    spiWriteReg(dev, MAX7456ADD_DMDI, attr);
    max7456RestoreDisplayMemoryMode();
    return true;
}

// Char + attr in one 8-bit DMM session — half the SPI overhead of separate calls.
bool max7456WriteDisplaySramCharAttr(uint16_t addr, uint8_t glyph, uint8_t attr)
{
    if (addr >= maxScreenSize) {
        return false;
    }
    if (!max7456WaitSpiIdle()) {
        return false;
    }

    spiWriteReg(dev, MAX7456ADD_DMM, DMM_8BIT_MODE);
    spiWriteReg(dev, MAX7456ADD_DMAH, (uint8_t)((addr >> 8) & 0x01));
    spiWriteReg(dev, MAX7456ADD_DMAL, (uint8_t)(addr & 0xff));
    spiWriteReg(dev, MAX7456ADD_DMDI, glyph);
    spiWriteReg(dev, MAX7456ADD_DMAH, (uint8_t)(((addr >> 8) & 0x01) | DMAH_ATTR_SELECT));
    spiWriteReg(dev, MAX7456ADD_DMAL, (uint8_t)(addr & 0xff));
    spiWriteReg(dev, MAX7456ADD_DMDI, attr);
    max7456RestoreDisplayMemoryMode();
    return true;
}

// Burst-fill `count` consecutive character cells with the same glyph.
// 16-bit auto-inc like max7456DrawScreen: address → DMM|AI → DMDI writes → END.
// Per-register spiWriteReg (not a long bare-byte DMA) — reliable on polled SPI.
// commitShadow=false: raster hot path skips the shadow walk.
bool max7456WriteDisplaySramRowFillEx(uint16_t addr, uint8_t glyph, uint8_t count, bool commitShadow)
{
    if (count == 0 || addr >= maxScreenSize) {
        return false;
    }
    if ((uint16_t)(addr + count) > maxScreenSize) {
        count = (uint8_t)(maxScreenSize - addr);
    }
    if (count > CHARS_PER_LINE) {
        count = CHARS_PER_LINE;
    }
    if (commitShadow) {
        if (!max7456WaitSpiIdle()) {
            return false;
        }
    } else if (max7456ActiveDma || spiIsBusy(dev)) {
        if (!max7456WaitSpiIdle()) {
            return false;
        }
    }

    const uint8_t drawDmm = (uint8_t)((displayMemoryModeReg & (uint8_t)~INVERT_PIXEL_COLOR) | DMM_AUTO_INC);
    spiWriteReg(dev, MAX7456ADD_DMAH, (uint8_t)((addr >> 8) & 0x01));
    spiWriteReg(dev, MAX7456ADD_DMAL, (uint8_t)(addr & 0xff));
    spiWriteReg(dev, MAX7456ADD_DMM, drawDmm);
    for (uint8_t i = 0; i < count; i++) {
        spiWriteReg(dev, MAX7456ADD_DMDI, glyph);
    }
    spiWriteReg(dev, MAX7456ADD_DMDI, END_STRING);
    spiWriteReg(dev, MAX7456ADD_DMM, displayMemoryModeReg);
    previousInvertRegister = displayMemoryModeReg;

    if (commitShadow) {
        for (uint8_t i = 0; i < count; i++) {
            max7456CommitShadowCell((uint16_t)(addr + i), glyph, false);
        }
    }
    return true;
}

bool max7456WriteDisplaySramRowFill(uint16_t addr, uint8_t glyph, uint8_t count)
{
    return max7456WriteDisplaySramRowFillEx(addr, glyph, count, true);
}

static bool midGlyphSpiHot;
static uint16_t midGlyphSavedDiv;
#ifdef USE_DMA
static bool midGlyphSavedDma;
#endif

// Burst-write `count` consecutive cells with per-cell glyphs (+ optional INV).
// Hot mode (after MidGlyphSpiBegin): no per-call clock/DMA juggling, no DMM restore
// after END — keeps the odd-line tear window under ~24 us @20 MHz / 30 cells.
// invs==NULL → all cells non-inverted. Otherwise invs[i]!=0 sets per-cell INV via DMM[3]
// (16-bit mode copies DMM INV into each written character). Runs are grouped by INV.
// hosAbsOrNull: when non-NULL, prepend MAX7456ADD_HOS|*hos in the SAME spiSequence and
// update hosRegValue/previousHosRegister (one CS, no second wait).
static bool max7456WriteDisplaySramRowGlyphsInvEx(uint16_t addr, const uint8_t *glyphs, const uint8_t *invs,
                                                  uint8_t count, bool commitShadow, const uint8_t *hosAbsOrNull)
{
    if (!glyphs || count == 0 || addr >= maxScreenSize) {
        return false;
    }
    if ((uint16_t)(addr + count) > maxScreenSize) {
        count = (uint8_t)(maxScreenSize - addr);
    }
    if (count > CHARS_PER_LINE) {
        count = CHARS_PER_LINE;
    }
    if (!midGlyphSpiHot) {
        if (commitShadow) {
            if (!max7456WaitSpiIdle()) {
                return false;
            }
        } else if (max7456ActiveDma || spiIsBusy(dev)) {
            if (!max7456WaitSpiIdle()) {
                return false;
            }
        }
    } else if (max7456ActiveDma || spiIsBusy(dev)) {
        if (!max7456WaitSpiIdle()) {
            return false;
        }
    }

    // Worst case: INV alternates every cell → count short runs. ~10 bytes/run. +2 for HOS.
    static DMA_DATA uint8_t rowGlyphBuf[512];
    uint16_t idx = 0;
    if (hosAbsOrNull) {
        uint8_t hos = *hosAbsOrNull;
        if (hos > 63) {
            hos = 63;
        }
        hosRegValue = hos;
        previousHosRegister = hos;
        rowGlyphBuf[idx++] = MAX7456ADD_HOS;
        rowGlyphBuf[idx++] = hos;
    }
    uint8_t i = 0;
    while (i < count) {
        const bool runInv = invs ? (invs[i] != 0) : false;
        const uint8_t runStart = i;
        do {
            i++;
        } while (i < count && (invs ? (invs[i] != 0) : false) == runInv);

        const uint8_t runLen = (uint8_t)(i - runStart);
        const uint16_t runAddr = (uint16_t)(addr + runStart);
        uint8_t drawDmm = (uint8_t)((displayMemoryModeReg & (uint8_t)~INVERT_PIXEL_COLOR) | DMM_AUTO_INC);
        if (runInv) {
            drawDmm = (uint8_t)(drawDmm | INVERT_PIXEL_COLOR);
        }
        rowGlyphBuf[idx++] = MAX7456ADD_DMM;
        rowGlyphBuf[idx++] = drawDmm;
        rowGlyphBuf[idx++] = MAX7456ADD_DMAH;
        rowGlyphBuf[idx++] = (uint8_t)((runAddr >> 8) & 0x01);
        rowGlyphBuf[idx++] = MAX7456ADD_DMAL;
        rowGlyphBuf[idx++] = (uint8_t)(runAddr & 0xff);
        for (uint8_t k = 0; k < runLen; k++) {
            rowGlyphBuf[idx++] = MAX7456ADD_DMDI;
            rowGlyphBuf[idx++] = glyphs[runStart + k];
        }
        rowGlyphBuf[idx++] = MAX7456ADD_DMDI;
        rowGlyphBuf[idx++] = END_STRING;
    }
    if (!midGlyphSpiHot) {
        rowGlyphBuf[idx++] = MAX7456ADD_DMM;
        rowGlyphBuf[idx++] = displayMemoryModeReg;
    }

    busSegment_t segments[] = {
        {.u.buffers = {rowGlyphBuf, NULL}, idx, true, NULL},
        {.u.link = {NULL, NULL}, 0, true, NULL},
    };

    if (!midGlyphSpiHot) {
        const uint16_t savedDiv = max7456SpiClockDiv;
#ifdef USE_DMA
        const bool savedDma = dev->useDMA;
        spiDmaEnable(dev, false);
#endif
        if (max7456DeviceType == MAX7456_DEVICE_TYPE_AT) {
            spiSetClkDivisor(dev, spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ * 2));
        }
        spiSequence(dev, &segments[0]);
        spiWait(dev);
        if (max7456DeviceType == MAX7456_DEVICE_TYPE_AT) {
            spiSetClkDivisor(dev, savedDiv);
        }
#ifdef USE_DMA
        spiDmaEnable(dev, savedDma);
#endif
        previousInvertRegister = displayMemoryModeReg;
    } else {
        spiSequence(dev, &segments[0]);
        spiWait(dev);
    }

    if (commitShadow) {
        for (uint8_t c = 0; c < count; c++) {
            max7456CommitShadowCell((uint16_t)(addr + c), glyphs[c], invs ? (invs[c] != 0) : false);
        }
    }
    return true;
}

bool max7456WriteDisplaySramRowGlyphsInv(uint16_t addr, const uint8_t *glyphs, const uint8_t *invs,
                                         uint8_t count, bool commitShadow)
{
    return max7456WriteDisplaySramRowGlyphsInvEx(addr, glyphs, invs, count, commitShadow, NULL);
}

bool max7456WriteDisplaySramRowGlyphs(uint16_t addr, const uint8_t *glyphs, uint8_t count, bool commitShadow)
{
    return max7456WriteDisplaySramRowGlyphsInvEx(addr, glyphs, NULL, count, commitShadow, NULL);
}

bool max7456WriteHosSignedAndRowGlyphs(int8_t offsetPx, uint16_t addr, const uint8_t *glyphs,
                                       uint8_t count, bool commitShadow)
{
    int hos = 32 + (int)offsetPx;
    if (hos < 0) {
        hos = 0;
    }
    if (hos > 63) {
        hos = 63;
    }
    const uint8_t hosAbs = (uint8_t)hos;
    return max7456WriteDisplaySramRowGlyphsInvEx(addr, glyphs, NULL, count, commitShadow, &hosAbs);
}

#if defined(STM32F4)
// Mid-glyph hot path, write-only. The generic polled transfer sends a byte, waits for its
// RX byte, reads it, and does it through stdperiph flag calls — ~1.25 us/byte at 13.5 MHz
// where the wire needs 0.59 us. Writes to the MAX7456 return nothing useful, so keep TXE
// fed back-to-back and drop RX once at the end (DR then SR read clears OVR).
// Only used when the bus already carries this device's clock/mode (the generic path
// applies a pending divisor change lazily) — otherwise fall back.
static bool max7456SpiTxOnlyBurst(const uint8_t *buf, uint16_t len)
{
    busDevice_t *bus = dev->bus;
    if (dev->busType_u.spi.speed != bus->busType_u.spi.speed
        || dev->busType_u.spi.leadingEdge != bus->busType_u.spi.leadingEdge) {
        return false;
    }
    SPI_TypeDef *spi = (SPI_TypeDef *)bus->busType_u.spi.instance;
    IOLo(dev->busType_u.spi.csnPin);
    for (uint16_t i = 0; i < len; i++) {
        while (!(spi->SR & SPI_SR_TXE)) {
        }
        *(volatile uint8_t *)&spi->DR = buf[i];
    }
    while (!(spi->SR & SPI_SR_TXE)) {
    }
    while (spi->SR & SPI_SR_BSY) {
    }
    (void)spi->DR;
    (void)spi->SR;
    IOHi(dev->busType_u.spi.csnPin);
    return true;
}
#endif

static DMA_DATA uint8_t midGlyphRowBuf[256];

// Mid-glyph hot path: encode one row burst from a segment plan. Two framings, both 16-bit:
//  - autoInc: DMM|AI, [DMAH], DMAL, (DMDI,ca)×n, (DMDI,0xFF)   = 6 (+2) + 2n bytes
//  - single:  [DMM non-AI], [DMAH], DMAL, (DMDI,ca) per cell  = 4 (+2) bytes per cell
// Isolated changed cells are far cheaper as plain addressed writes than as an AI run, and
// nothing unchanged gets rewritten (fewer SRAM writes → fewer internal-read collisions,
// datasheet p.40). DMAH is only sent when address bit 8 changes. segLastByte[i] = offset just
// past the last character byte of segment i (when that cell lands) for beam racing.
// The datasheet's 8-bit data-only AI framing (Fig. 21) hangs AT7456 clones — not used.
uint16_t max7456EncodeDisplaySramRow(uint16_t rowAddr, const uint8_t *glyphs,
                                     const max7456SramSeg_t *seg, uint8_t nSeg, uint16_t *segLastByte)
{
    const uint8_t dmmAi = (uint8_t)((displayMemoryModeReg & (uint8_t)~INVERT_PIXEL_COLOR) | DMM_AUTO_INC);
    const uint8_t dmmSingle = (uint8_t)(dmmAi & (uint8_t)~DMM_AUTO_INC);
    uint16_t idx = 0;
    bool dmmSingleKnown = false;
    int16_t high = -1; // DMAH bit 8 currently in the chip, -1 = unknown
    for (uint8_t i = 0; i < nSeg; i++) {
        const uint16_t addr = (uint16_t)(rowAddr + seg[i].col);
        uint8_t len = seg[i].len;
        if (len == 0 || addr >= maxScreenSize) {
            segLastByte[i] = idx;
            continue;
        }
        if ((uint16_t)(addr + len) > maxScreenSize) {
            len = (uint8_t)(maxScreenSize - addr);
        }
        const uint16_t need = (uint16_t)(seg[i].autoInc ? 10u + 2u * len : 8u + 6u * len);
        if ((uint16_t)(idx + need) > sizeof(midGlyphRowBuf)) {
            return 0;
        }
        if (seg[i].autoInc) {
            midGlyphRowBuf[idx++] = MAX7456ADD_DMM;
            midGlyphRowBuf[idx++] = dmmAi;
            if (high != (int16_t)(addr >> 8)) {
                midGlyphRowBuf[idx++] = MAX7456ADD_DMAH;
                midGlyphRowBuf[idx++] = (uint8_t)((addr >> 8) & 0x01);
                high = (int16_t)(addr >> 8);
            }
            midGlyphRowBuf[idx++] = MAX7456ADD_DMAL;
            midGlyphRowBuf[idx++] = (uint8_t)(addr & 0xff);
            for (uint8_t k = 0; k < len; k++) {
                midGlyphRowBuf[idx++] = MAX7456ADD_DMDI;
                midGlyphRowBuf[idx++] = glyphs[seg[i].col + k];
            }
            segLastByte[i] = idx;
            midGlyphRowBuf[idx++] = MAX7456ADD_DMDI;
            midGlyphRowBuf[idx++] = END_STRING;
            dmmSingleKnown = false;
            if ((addr >> 8) != ((addr + len - 1u) >> 8)) {
                high = -1; // the AI counter crossed 255→256; chip DMAH state unknown
            }
        } else {
            if (!dmmSingleKnown) {
                midGlyphRowBuf[idx++] = MAX7456ADD_DMM;
                midGlyphRowBuf[idx++] = dmmSingle;
                dmmSingleKnown = true;
            }
            for (uint8_t k = 0; k < len; k++) {
                const uint16_t a = (uint16_t)(addr + k);
                if (high != (int16_t)(a >> 8)) {
                    midGlyphRowBuf[idx++] = MAX7456ADD_DMAH;
                    midGlyphRowBuf[idx++] = (uint8_t)((a >> 8) & 0x01);
                    high = (int16_t)(a >> 8);
                }
                midGlyphRowBuf[idx++] = MAX7456ADD_DMAL;
                midGlyphRowBuf[idx++] = (uint8_t)(a & 0xff);
                midGlyphRowBuf[idx++] = MAX7456ADD_DMDI;
                midGlyphRowBuf[idx++] = glyphs[seg[i].col + k];
            }
            segLastByte[i] = idx;
        }
    }
    return idx;
}

// Send what max7456EncodeDisplaySramRow() prepared. Only valid between MidGlyphSpiBegin/End.
bool max7456SendEncodedDisplaySram(uint16_t len)
{
    if (!midGlyphSpiHot || len == 0 || len > sizeof(midGlyphRowBuf)) {
        return false;
    }
    if (max7456ActiveDma || spiIsBusy(dev)) {
        if (!max7456WaitSpiIdle()) {
            return false;
        }
    }
#if defined(STM32F4)
    if (max7456SpiTxOnlyBurst(midGlyphRowBuf, len)) {
        return true;
    }
#endif
    busSegment_t segments[] = {
        {.u.buffers = {midGlyphRowBuf, NULL}, len, true, NULL},
        {.u.link = {NULL, NULL}, 0, true, NULL},
    };
    spiSequence(dev, &segments[0]);
    spiWait(dev);
    return true;
}

void max7456MidGlyphSpiBegin(void)
{
    (void)max7456WaitSpiIdle();
    midGlyphSavedDiv = max7456SpiClockDiv;
#ifdef USE_DMA
    midGlyphSavedDma = dev->useDMA;
    spiDmaEnable(dev, false);
#endif
    // Keep nominal SPI clock here — WaitVsync STAT polling breaks at 20 MHz on many boards.
    midGlyphSpiHot = true;
}

void max7456MidGlyphSpiEnd(void)
{
    if (!midGlyphSpiHot) {
        return;
    }
    midGlyphSpiHot = false;
    (void)max7456WaitSpiIdle();
    spiWriteReg(dev, MAX7456ADD_DMM, displayMemoryModeReg);
    previousInvertRegister = displayMemoryModeReg;
    spiSetClkDivisor(dev, midGlyphSavedDiv);
#ifdef USE_DMA
    spiDmaEnable(dev, midGlyphSavedDma);
#endif
}

// Boost only around Display-SRAM bursts (call after VSYNC, unboost before next VSYNC).
void max7456MidGlyphSpiBoost(bool enable)
{
    if (!midGlyphSpiHot) {
        return;
    }
    if (enable) {
        // 20 MHz for both AT and MAX — needed to finish a row rewrite inside a PAL line.
        spiSetClkDivisor(dev, spiCalculateDivider(MAX7456_MAX_SPI_CLK_HZ * 2));
        // The bus layer applies a divisor lazily on the next spiSequence. Latch it now with a
        // write that cannot touch the picture (~1 us) so the first hot burst after a STAT poll
        // takes the TX-only path instead of the 2× slower generic one. DMAH only feeds the
        // next display-memory address, and every hot burst rewrites it first — unlike HOS,
        // which re-latches the horizontal position mid-line.
        if (!max7456ActiveDma && !spiIsBusy(dev)) {
            spiWriteReg(dev, MAX7456ADD_DMAH, 0);
        }
    } else {
        spiSetClkDivisor(dev, midGlyphSavedDiv);
    }
}

void max7456WriteHosNow(uint8_t hos)
{
    if (hos > 63) {
        hos = 63;
    }
    if (!midGlyphSpiHot) {
        if (!max7456WaitSpiIdle()) {
            return;
        }
    } else if (spiIsBusy(dev)) {
        spiWait(dev);
    }
    // Keep hosRegValue in sync so a later ApplyHosVos cannot snap by ~1 cell.
    hosRegValue = hos;
    previousHosRegister = hos;
    spiWriteReg(dev, MAX7456ADD_HOS, hos);
}

// Mid-glyph hot path: vertical shift relative to the configured base VOS, now (+ = down).
// Returns the offset actually applied after clamping to the VOS range, so a raster engine
// can move its line schedule by exactly that many lines.
int8_t max7456WriteVosOffsetNow(int8_t offsetPx)
{
    const int vos = constrain((int)vosRegValue + (int)offsetPx, 0, 31);
    if (!midGlyphSpiHot) {
        if (!max7456WaitSpiIdle()) {
            return (int8_t)(vos - (int)vosRegValue);
        }
    } else if (spiIsBusy(dev)) {
        spiWait(dev);
    }
    if (vos != previousVosRegister) {
        previousVosRegister = (uint8_t)vos;
        spiWriteReg(dev, MAX7456ADD_VOS, (uint8_t)vos);
    }
    return (int8_t)(vos - (int)vosRegValue);
}

void max7456WriteHosSigned(int8_t offsetPx)
{
    // Register center 32 == neutral. Clamp BEFORE encode — never wrap 0↔63.
    int hos = 32 + (int)offsetPx;
    if (hos < 0) {
        hos = 0;
    }
    if (hos > 63) {
        hos = 63;
    }
    max7456WriteHosNow((uint8_t)hos);
}

void max7456CommitShadowCell(uint16_t addr, uint8_t glyph, bool invert)
{
    if (addr >= maxScreenSize) {
        return;
    }
    getActiveLayerBuffer()[addr] = glyph;
    invertLayer[addr] = invert ? 1 : 0;
    shadowBuffer[addr] = glyph;
    shadowInvert[addr] = invert ? 1 : 0;
}

uint8_t max7456ReadStat(void)
{
    if (!max7456WaitSpiIdle()) {
        return 0;
    }
    return spiReadRegMsk(dev, MAX7456ADD_STAT);
}

bool max7456WaitVsyncFallingEdge(uint32_t *edgeTicks, timeUs_t timeoutUs)
{
    if (!max7456WaitSpiIdle()) {
        return false;
    }

    // DWT, not micros(): these run inside the scene-10 TIM5 interrupt, where SysTick cannot
    // fire — micros() then wraps back every 1 ms and a >1 ms timeout never expired (hang).
    const uint32_t t0 = getCycleCounter();
    const uint32_t timeoutTicks = clockMicrosToCycles(timeoutUs);
    uint8_t prev = spiReadRegMsk(dev, MAX7456ADD_STAT);

    // Ensure we start from VSYNC high so the next 1→0 is a real edge.
    while (!STAT_IS_VSYNC_HIGH(prev)) {
        if (getCycleCounter() - t0 > timeoutTicks) {
            return false;
        }
        prev = spiReadRegMsk(dev, MAX7456ADD_STAT);
    }

    for (;;) {
        const uint8_t s = spiReadRegMsk(dev, MAX7456ADD_STAT);
        if (STAT_IS_VSYNC_HIGH(prev) && !STAT_IS_VSYNC_HIGH(s)) {
            // Falling edge of STAT[4] ≈ start of VSYNC (active-low ~VSYNC).
            if (edgeTicks) {
                *edgeTicks = getCycleCounter();
            }
            return true;
        }
        prev = s;
        if (getCycleCounter() - t0 > timeoutTicks) {
            return false;
        }
    }
}

bool max7456WaitHsyncFallingEdge(uint32_t *edgeTicks, timeUs_t timeoutUs)
{
    if (!max7456WaitSpiIdle()) {
        return false;
    }

    // DWT, not micros(): these run inside the scene-10 TIM5 interrupt, where SysTick cannot
    // fire — micros() then wraps back every 1 ms and a >1 ms timeout never expired (hang).
    const uint32_t t0 = getCycleCounter();
    const uint32_t timeoutTicks = clockMicrosToCycles(timeoutUs);
    uint8_t prev = spiReadRegMsk(dev, MAX7456ADD_STAT);

    // Start from HSYNC high so the next 1→0 is a real line edge.
    while (!STAT_IS_HSYNC_HIGH(prev)) {
        if (getCycleCounter() - t0 > timeoutTicks) {
            return false;
        }
        prev = spiReadRegMsk(dev, MAX7456ADD_STAT);
    }

    for (;;) {
        const uint8_t s = spiReadRegMsk(dev, MAX7456ADD_STAT);
        if (STAT_IS_HSYNC_HIGH(prev) && !STAT_IS_HSYNC_HIGH(s)) {
            if (edgeTicks) {
                *edgeTicks = getCycleCounter();
            }
            return true;
        }
        prev = s;
        if (getCycleCounter() - t0 > timeoutTicks) {
            return false;
        }
    }
}

bool max7456SkipHsyncFallingEdges(uint16_t count, timeUs_t timeoutUs)
{
    const timeUs_t t0 = micros();
    for (uint16_t i = 0; i < count; i++) {
        const timeDelta_t left = (timeDelta_t)timeoutUs - cmpTimeUs(micros(), t0);
        if (left <= 0) {
            return false;
        }
        // Per-edge budget: never less than ~2 line periods.
        const timeUs_t edgeBudget = (left < 200) ? (timeUs_t)left : 200;
        if (!max7456WaitHsyncFallingEdge(NULL, edgeBudget)) {
            return false;
        }
    }
    return true;
}

#endif // USE_MAX7456
