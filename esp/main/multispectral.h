#pragma once

#include "extract_hues.h"

struct MsFlashColor {
    uint8_t r, g, b;
};

constexpr MsFlashColor MS_FLASH_COLORS[3] = {
    {255, 0, 0},
    {0, 255, 0},
    {0, 0, 255},
};

/**
 * @brief Active Multispectral Imaging capture (R -> G -> B sequential flash).
 *
 * Locks AWB/AGC/AEC, flashes the WS2812 pure red/green/blue, captures one
 * JPEG frame per flash, decodes each to RGB565 (TJpgDec, downscaled), then
 * synthesizes a single white-balanced RGB565 image (red channel from the red
 * flash, green from the green flash, blue from the blue flash) and runs the
 * standard HueExtractor pipeline on it.
 *
 * @param out Receives the synthesized ColorFeatures on success.
 * @return true on success, false if capture/decode/synthesis failed.
 */
bool multispectral_capture(ColorFeatures& out);

/**
 * @brief Overrides the runtime per-channel gains, locked AEC value and
 *        ambient-subtraction enable, then persists them to NVS.
 *
 * Used by the BLE ms_config command for white-card calibration without
 * reflashing. Missing NVS keys fall back to the compile-time constants.
 */
void multispectral_set_config(float gain_r, float gain_g, float gain_b, int aec, bool ambient);

/**
 * @brief Loads calibration overrides (gains/AEC/ambient) from NVS, falling
 *        back to the compile-time constants when keys are absent.
 */
void multispectral_load_calibration();

/**
 * @brief Locks AWB/AGC/AEC to the fixed values used for multispectral captures.
 */
void multispectral_lock_sensor();

/**
 * @brief Restores AWB/AGC/AEC to automatic after a multispectral capture.
 */
void multispectral_restore_sensor();

/**
 * @brief Grabs one camera frame exposed under the given flash color.
 *
 * Turns on the WS2812 to the requested color and discards enough frames for
 * the sensor exposure to synchronize with the LED, guaranteeing the returned
 * frame was exposed while the LED was active. The LED is left ON for the
 * caller to turn off / change color.
 *
 * @return camera_fb_t* to release with release_camera_frame(), or nullptr.
 */
camera_fb_t* multispectral_grab_flash_frame(uint8_t r, uint8_t g, uint8_t b);
