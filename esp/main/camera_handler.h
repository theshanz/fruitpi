#pragma once

#include <Arduino.h>
#include "esp_camera.h"

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
 * @brief Checks if camera hardware is ready.
 */
bool is_camera_ready();
