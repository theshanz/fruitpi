#pragma once

#include <Arduino.h>
#include "esp_camera.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
// Camera Pin Definitions — ESP32-S3 EYE / WROOM-CAM (N16R8)
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_Y9      16
#define CAM_PIN_Y8      17
#define CAM_PIN_Y7      18
#define CAM_PIN_Y6      12
#define CAM_PIN_Y5      10
#define CAM_PIN_Y4       8
#define CAM_PIN_Y3       9
#define CAM_PIN_Y2      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13
#else
// Camera Pin Definitions — AI-Thinker ESP32-CAM (OV2640)
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_Y9      35
#define CAM_PIN_Y8      34
#define CAM_PIN_Y7      39
#define CAM_PIN_Y6      36
#define CAM_PIN_Y5      21
#define CAM_PIN_Y4      19
#define CAM_PIN_Y3      18
#define CAM_PIN_Y2      5
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK    22
#endif

/**
 * @brief Initializes the camera hardware using PSRAM configuration.
 * @return true if successful, false if hardware failed to initialize.
 */
bool init_camera_subsystem();

/**
 * @brief Safely captures the latest camera framebuffer.
 * @return camera_fb_t* pointer, or nullptr on failure.
 */
camera_fb_t* capture_camera_frame();

/**
 * @brief Releases the framebuffer memory back to the driver.
 */
void release_camera_frame(camera_fb_t* fb);

/**
 * @brief Copies a raw JPEG framebuffer into PSRAM so the driver buffer can be
 * returned immediately.
 *
 * The single-buffer driver (fb_count=1) cannot hold a frame across another
 * fb_get. When the caller needs to keep a JPEG past the capture (e.g. the
 * dual-shot-debug stream), copy it here and return the driver frame right away.
 *
 * @param src The live framebuffer to copy (may be null).
 * @param dst_len Out: copied byte length (0 on failure).
 * @return Pointer to the PSRAM copy (caller owns / free()s with
 *         free_copied_frame), or nullptr on allocation/copy failure.
 */
camera_fb_t* copy_camera_frame(const camera_fb_t* src, size_t& dst_len);

/**
 * @brief Frees a copy produced by copy_camera_frame.
 */
void free_copied_frame(camera_fb_t* fb);

/**
 * @brief Checks if camera hardware is ready.
 */
bool is_camera_ready();

/**
 * @brief Brings the sensor out of idle (restores XCLK) so a capture works.
 *
 * Idempotent: safe to call before every grab even mid-session.
 */
void camera_wake();

/**
 * @brief Drops the sensor to idle by stopping XCLK (LEDC channel 0 duty -> 0).
 *
 * The OV3660 streams continuously between captures otherwise, which is what
 * makes it hot at idle; with no clock it drops to a quiescent state. This
 * SDK's esp32-camera has no sleep API and the S3-EYE has no PWDN pin, so
 * gating XCLK is the only safe lever (de-init/re-init is broken on the S3).
 */
void camera_sleep_idle();

/**
 * @brief Turns the target-appropriate white flash on/off.
 *
 * S3 EYE: WS2812 all-white. AI-Thinker ESP32-CAM: on-board white LED (GPIO4).
 *
 * @param on true = flash lit, false = flash off.
 */
void camera_set_flash(bool on);
