/* Pinned ChromiumOS Adaptive Charging model artifacts (ADAPTIVE_CHARGING_20230314).
 * Must match src/link/battery-adaptive-manifest.json exactly (checked by
 * test-battery-adaptive.c). */
#ifndef LOCACHY_BATTERY_MODEL_PINS_H
#define LOCACHY_BATTERY_MODEL_PINS_H

#include <stddef.h>

#define BAT_MODEL_ID           "ADAPTIVE_CHARGING_20230314"

#define BAT_MODEL_TFLITE_NAME  "mlservice-model-adaptive_charging-20230314.tflite"
#define BAT_MODEL_TFLITE_SIZE  ((size_t)124324u)
#define BAT_MODEL_TFLITE_SHA512 \
    "84c1ad16e06787c2c309380ed7a206be23aa9f4597d6492d52778b3347cf7032" \
    "d8097112aa88af60d9025f588023b5092fa0618cde1f860bf67265207f93b688"

#define BAT_MODEL_PREPROC_NAME "mlservice-model-adaptive_charging-20230314-preprocessor.pb"
#define BAT_MODEL_PREPROC_SIZE ((size_t)31082u)
#define BAT_MODEL_PREPROC_SHA512 \
    "e2e19b1d6fb5ae7f5223d5874968c1dc70035d2a77373e6a790127a6c88d29e2" \
    "6d79df72f15857cbbfb22e597c7894859751ae9ab1e193f384a62a28b0be7eff"

#endif
