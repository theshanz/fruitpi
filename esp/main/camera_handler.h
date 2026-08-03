#pragma once

#include <Arduino.h>
#include "esp_camera.h"

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
