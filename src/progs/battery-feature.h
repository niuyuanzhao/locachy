/* One feature of an adaptive charging example, as the ChromiumOS model's
 * preprocessor reads it (assist_ranker::RankerExample: int32 or float). Shared
 * by the controller (battery-controller.c, which fills them) and the model
 * (battery-tflite.c, which reads them). The name must stay valid for the call
 * that receives the array. */
#ifndef LOCACHY_BATTERY_FEATURE_H
#define LOCACHY_BATTERY_FEATURE_H

#include <stddef.h>

enum bat_feat_kind {
    BAT_FEAT_INT32,
    BAT_FEAT_FLOAT,
};

struct bat_feature {
    const char *name;
    enum bat_feat_kind kind;
    double value;
};

/* The model's output: probabilities that the charger is unplugged within each
 * hour 0..7 (index 0..7), and that it stays plugged for 8 hours or more (8). */
#define BAT_ADAPTIVE_OUTPUTS 9

#endif
