/* Minimal, dependency-free TFLite + ExamplePreprocessor inference engine for
 * the ChromiumOS Adaptive Charging model. */
#ifndef LOCACHY_BATTERY_TFLITE_H
#define LOCACHY_BATTERY_TFLITE_H

#include "battery-feature.h"

struct bat_model;

/* Parse and keep copies of both artifacts (TFLite FlatBuffer graph/weights and
 * ExamplePreprocessorConfig protobuf) in memory; caller buffers need not stay
 * alive after this call returns. Supports only the subset used by the adaptive
 * charging model: a single subgraph, FLOAT32 tensors, FULLY_CONNECTED with
 * NONE or RELU fused activation, and SOFTMAX. Returns 0 on success, -1 on any
 * malformed or unsupported input (setting *out = NULL). */
int bat_model_load(struct bat_model **out, const void *tflite, size_t tflite_len,
                   const void *preproc, size_t preproc_len);

/* Preprocess the features according to the loaded ExamplePreprocessorConfig
 * and run the TFLite graph. feats may be NULL when n == 0. Writes
 * BAT_ADAPTIVE_OUTPUTS probabilities to out. Returns 0 on success, -1 on error
 * (invalid arguments, non-finite inputs, or unsupported feature type conversion). */
int bat_model_predict(const struct bat_model *m, const struct bat_feature *feats,
                      size_t n, double out[BAT_ADAPTIVE_OUTPUTS]);

/* Free a model allocated by bat_model_load(). NULL is a no-op. */
void bat_model_free(struct bat_model *m);

#endif
