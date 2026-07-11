#ifndef CAMERA_HANDLER_H
#define CAMERA_HANDLER_H

#include <Arduino.h>

// Camera pin definitions — adjust to match your board wiring
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    40
#define CAM_PIN_SIOD    17
#define CAM_PIN_SIOC    18
#define CAM_PIN_Y9      39
#define CAM_PIN_Y8      41
#define CAM_PIN_Y7      42
#define CAM_PIN_Y6      12
#define CAM_PIN_Y5       3
#define CAM_PIN_Y4      14
#define CAM_PIN_Y3      47
#define CAM_PIN_Y2      13
#define CAM_PIN_VSYNC   21
#define CAM_PIN_HREF    38
#define CAM_PIN_PCLK    11

#define LED_PIN         48   // Status LED — change if it conflicts

bool    initCamera();
String  captureBase64();

#endif
