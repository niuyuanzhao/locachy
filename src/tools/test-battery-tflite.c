/* Unit tests for the pure-C ChromiumOS Adaptive Charging TFLite + preprocessor
 * engine (battery-tflite.c).
 *
 * Usage: test-battery-tflite <artifact-dir>
 * where <artifact-dir> contains:
 *   mlservice-model-adaptive_charging-20230314.tflite
 *   mlservice-model-adaptive_charging-20230314-preprocessor.pb
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../progs/battery-tflite.h"

#define MODEL_FILE "mlservice-model-adaptive_charging-20230314.tflite"
#define PREPROC_FILE "mlservice-model-adaptive_charging-20230314-preprocessor.pb"

static int g_failures;

static void check(int cond, const char *desc)
{
    if (cond) {
        printf("PASS %s\n", desc);
    } else {
        printf("FAIL %s\n", desc);
        g_failures++;
    }
}

static uint8_t *read_file(const char *dir, const char *name, size_t *out_len)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    char *path = malloc(dlen + 1 + nlen + 1);
    if (!path)
        return NULL;
    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, name, nlen + 1);

    FILE *fp = fopen(path, "rb");
    free(path);
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (nread != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = 0;
    *out_len = (size_t)sz;
    return buf;
}

static double max_abs_diff(const double a[BAT_ADAPTIVE_OUTPUTS],
                           const double b[BAT_ADAPTIVE_OUTPUTS])
{
    double m = 0.0;
    for (size_t i = 0; i < BAT_ADAPTIVE_OUTPUTS; i++) {
        double d = fabs(a[i] - b[i]);
        if (d > m)
            m = d;
    }
    return m;
}

static double sum_probs(const double a[BAT_ADAPTIVE_OUTPUTS])
{
    double s = 0.0;
    for (size_t i = 0; i < BAT_ADAPTIVE_OUTPUTS; i++)
        s += a[i];
    return s;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <artifact-dir>\n", argv[0]);
        return 2;
    }

    size_t tfl_len = 0, pb_len = 0;
    uint8_t *tfl = read_file(argv[1], MODEL_FILE, &tfl_len);
    uint8_t *pb = read_file(argv[1], PREPROC_FILE, &pb_len);
    check(tfl != NULL && tfl_len == 124324, "read tflite artifact");
    check(pb != NULL && pb_len == 31082, "read preprocessor artifact");
    if (!tfl || !pb) {
        free(tfl);
        free(pb);
        return 1;
    }

    /* Load real artifacts; verify caller buffers can be freed/mutated afterwards
     * because bat_model_load copies everything it needs. */
    struct bat_model *m = NULL;
    uint8_t *tfl_copy = malloc(tfl_len);
    uint8_t *pb_copy = malloc(pb_len);
    if (!tfl_copy || !pb_copy) {
        free(tfl_copy);
        free(pb_copy);
        free(tfl);
        free(pb);
        return 1;
    }
    memcpy(tfl_copy, tfl, tfl_len);
    memcpy(pb_copy, pb, pb_len);

    int rc = bat_model_load(&m, tfl_copy, tfl_len, pb_copy, pb_len);
    memset(tfl_copy, 0xA5, tfl_len);
    memset(pb_copy, 0x5A, pb_len);
    free(tfl_copy);
    free(pb_copy);
    check(rc == 0 && m != NULL, "bat_model_load real artifacts (independent of caller buffers)");

    if (!m) {
        free(tfl);
        free(pb);
        return 1;
    }

    /* 1. Upstream ChromiumOS golden empty-input vector from
     *    platform2/ml/dbus_service/tf_model_graph_executor_test.cc
     *    (TEST(TfModelGraphExecutorTest, ExecuteAdaptiveCharging20230314)). */
    {
        static const double golden[BAT_ADAPTIVE_OUTPUTS] = {
            0.207927, 0.187088, 0.121717, 0.090703, 0.065328,
            0.045972, 0.026243, 0.018707, 0.236313
        };
        double out[BAT_ADAPTIVE_OUTPUTS] = {0};
        int r = bat_model_predict(m, NULL, 0, out);
        double diff = max_abs_diff(out, golden);
        double sum = sum_probs(out);
        check(r == 0 && diff < 1e-5 && fabs(sum - 1.0) < 1e-5,
              "golden empty input matches ChromiumOS tf_model_graph_executor_test.cc within 1e-5");
    }

    /* 2. Non-empty Case A: exact float bucket boundaries (checks strict `<` rule).
     *    NOTE: Expected outputs for Cases A, B, and C below were computed by an
     *    independent Python reference port of ChromiumOS example_preprocessing.cc
     *    and TFLite inference; they are NOT upstream ChromiumOS golden vectors.
     *
     *    Boundary checks:
     *      BatteryPercentage boundaries[5] == 48.5  -> value 48.5 lands in bucket 6 (not 5)
     *      TimeOfTheDay boundaries[72] == 1417.5    -> value 1417.5 lands in bucket 73 (past end)
     *      ScreenBrightnessPercent boundaries[0] == 3.0 -> value 3.0 lands in bucket 1 (not 0) */
    {
        static const struct bat_feature at_boundary[] = {
            { "BatteryPercentage", BAT_FEAT_FLOAT, 48.5 },
            { "TimeOfTheDay", BAT_FEAT_FLOAT, 1417.5 },
            { "ScreenBrightnessPercent", BAT_FEAT_FLOAT, 3.0 },
        };
        static const double expected_at[BAT_ADAPTIVE_OUTPUTS] = {
            0.082114032, 0.049305293, 0.013285050, 0.005455235, 0.004565053,
            0.003814103, 0.016476430, 0.035998679, 0.788986125
        };
        static const struct bat_feature below_boundary[] = {
            { "BatteryPercentage", BAT_FEAT_FLOAT, 48.49 },
            { "TimeOfTheDay", BAT_FEAT_FLOAT, 1417.49 },
            { "ScreenBrightnessPercent", BAT_FEAT_FLOAT, 2.99 },
        };
        static const double expected_below[BAT_ADAPTIVE_OUTPUTS] = {
            0.100513110, 0.047906207, 0.012754123, 0.005222848, 0.003993602,
            0.005289382, 0.013559257, 0.024271121, 0.786490349
        };
        double out_at[BAT_ADAPTIVE_OUTPUTS] = {0};
        double out_below[BAT_ADAPTIVE_OUTPUTS] = {0};
        int r1 = bat_model_predict(m, at_boundary, 3, out_at);
        int r2 = bat_model_predict(m, below_boundary, 3, out_below);
        check(r1 == 0 && max_abs_diff(out_at, expected_at) < 1e-6 &&
              r2 == 0 && max_abs_diff(out_below, expected_below) < 1e-6 &&
              max_abs_diff(out_at, out_below) > 1e-2,
              "bucket boundary strict '<' rule (reference port, non-golden)");
    }

    /* 3. Non-empty Case B: normalizer clamping above divisor, below -divisor,
     *    unclamped interior, and unnormalized scalar RatioTimeFullCharge.
     *    NOTE: Expected outputs are from the independent Python reference port,
     *    NOT upstream golden vectors. */
    {
        static const struct bat_feature clamped[] = {
            { "ChargeEventHistoryDuration0", BAT_FEAT_INT32, 180 }, /* divisor 60 -> clamp +1.0 */
            { "ChargeEventHistorySize", BAT_FEAT_FLOAT, -100.0 },   /* divisor 50 -> clamp -1.0 */
            { "DailySummaryTimeOnAc0", BAT_FEAT_INT32, 420 },       /* divisor 840 -> 0.5 */
            { "RatioTimeFullCharge", BAT_FEAT_FLOAT, 0.75 },        /* unnormalized float */
        };
        static const struct bat_feature exact_limit[] = {
            { "ChargeEventHistoryDuration0", BAT_FEAT_INT32, 60 },
            { "ChargeEventHistorySize", BAT_FEAT_FLOAT, -50.0 },
            { "DailySummaryTimeOnAc0", BAT_FEAT_INT32, 420 },
            { "RatioTimeFullCharge", BAT_FEAT_FLOAT, 0.75 },
        };
        static const double expected[BAT_ADAPTIVE_OUTPUTS] = {
            0.161551174, 0.187391159, 0.133803907, 0.100165001, 0.070966790,
            0.057205295, 0.044352304, 0.032949097, 0.211615271
        };
        double out1[BAT_ADAPTIVE_OUTPUTS] = {0};
        double out2[BAT_ADAPTIVE_OUTPUTS] = {0};
        int r1 = bat_model_predict(m, clamped, 4, out1);
        int r2 = bat_model_predict(m, exact_limit, 4, out2);
        check(r1 == 0 && r2 == 0 &&
              max_abs_diff(out1, expected) < 1e-6 &&
              max_abs_diff(out1, out2) < 1e-7,
              "normalizer clamp to [-1, 1] and unnormalized float (reference port, non-golden)");
    }

    /* 4. Non-empty Case C: int32 convert-to-string features (DayOfWeek, DayOfMonth,
     *    DeviceMode, Reason, IsCharging) combined with bucketizers & normalizers.
     *    NOTE: Expected outputs are from the independent Python reference port,
     *    NOT upstream golden vectors. */
    {
        static const struct bat_feature mixed[] = {
            { "TimeOfTheDay", BAT_FEAT_INT32, 1380 },
            { "DayOfWeek", BAT_FEAT_INT32, 3 },
            { "DayOfMonth", BAT_FEAT_INT32, 15 },
            { "DeviceMode", BAT_FEAT_INT32, 1 },
            { "Reason", BAT_FEAT_INT32, 1 },
            { "IsCharging", BAT_FEAT_INT32, 1 },
            { "BatteryPercentage", BAT_FEAT_INT32, 80 },
            { "ScreenBrightnessPercent", BAT_FEAT_INT32, 50 },
            { "ChargeEventHistorySize", BAT_FEAT_INT32, 10 },
            { "ChargeEventHistoryDuration0", BAT_FEAT_INT32, 480 },
            { "DailySummarySize", BAT_FEAT_INT32, 7 },
            { "DailySummaryTimeOnAc0", BAT_FEAT_INT32, 600 },
            { "DailySummaryTimeFullOnAc0", BAT_FEAT_INT32, 300 },
            { "RatioTimeFullCharge", BAT_FEAT_FLOAT, 0.5 },
        };
        static const double expected[BAT_ADAPTIVE_OUTPUTS] = {
            0.634043055, 0.018030555, 0.001121382, 0.000272256, 0.000667998,
            0.000273199, 0.000773391, 0.003130678, 0.341687485
        };
        double out[BAT_ADAPTIVE_OUTPUTS] = {0};
        int r = bat_model_predict(m, mixed, sizeof mixed / sizeof mixed[0], out);
        check(r == 0 && max_abs_diff(out, expected) < 1e-6 && fabs(sum_probs(out) - 1.0) < 1e-5,
              "convert-to-string DayOfWeek and mixed features (reference port, non-golden)");
    }

    /* 5. Runtime error checks on bat_model_predict. */
    {
        double out[BAT_ADAPTIVE_OUTPUTS];
        /* Float passed for convert_to_string feature DayOfWeek -> rejected per upstream. */
        static const struct bat_feature bad_c2s[] = {
            { "DayOfWeek", BAT_FEAT_FLOAT, 3.0 },
        };
        static const struct bat_feature bad_nan[] = {
            { "BatteryPercentage", BAT_FEAT_FLOAT, NAN },
        };
        static const struct bat_feature bad_null[] = {
            { NULL, BAT_FEAT_INT32, 1 },
        };
        int ok = 1;
        if (bat_model_predict(m, bad_c2s, 1, out) != -1) ok = 0;
        if (bat_model_predict(m, bad_nan, 1, out) != -1) ok = 0;
        if (bat_model_predict(m, bad_null, 1, out) != -1) ok = 0;
        if (bat_model_predict(m, NULL, 1, out) != -1) ok = 0;
        if (bat_model_predict(NULL, NULL, 0, out) != -1) ok = 0;
        if (bat_model_predict(m, NULL, 0, NULL) != -1) ok = 0;
        check(ok, "predict rejects invalid args, NaN, and float convert_to_string");
    }

    bat_model_free(m);
    bat_model_free(NULL);

    /* 6. Malformed inputs must return -1 without crashing or leaking. */
    {
        struct bat_model *bad = (struct bat_model *)0x1;
        int ok = 1;

        /* Empty / NULL buffers. */
        if (bat_model_load(&bad, "", 0, pb, pb_len) != -1 || bad != NULL) ok = 0;
        if (bat_model_load(&bad, tfl, tfl_len, "", 0) != -1 || bad != NULL) ok = 0;
        if (bat_model_load(&bad, NULL, tfl_len, pb, pb_len) != -1 || bad != NULL) ok = 0;
        if (bat_model_load(&bad, tfl, tfl_len, NULL, pb_len) != -1 || bad != NULL) ok = 0;
        if (bat_model_load(NULL, tfl, tfl_len, pb, pb_len) != -1) ok = 0;
        check(ok, "reject empty and NULL buffers");
    }

    {
        struct bat_model *bad = NULL;
        int ok = 1;
        uint8_t *zeros = calloc(1, tfl_len);
        if (!zeros) {
            free(tfl);
            free(pb);
            return 1;
        }
        if (bat_model_load(&bad, zeros, tfl_len, pb, pb_len) != -1 || bad != NULL) ok = 0;
        if (bat_model_load(&bad, tfl, tfl_len, zeros, pb_len) != -1 || bad != NULL) ok = 0;
        free(zeros);
        check(ok, "reject zeroed tflite and zeroed preprocessor");
    }

    {
        struct bat_model *bad = NULL;
        int ok = 1;
        /* Sample truncation lengths across the entire tflite buffer. */
        static const size_t tfl_cuts[] = {
            1, 4, 7, 8, 12, 16, 28, 32, 64, 512, 4096, 30000, 120000,
            122500, 124000, 124320, 124323
        };
        for (size_t i = 0; i < sizeof tfl_cuts / sizeof tfl_cuts[0]; i++) {
            if (bat_model_load(&bad, tfl, tfl_cuts[i], pb, pb_len) != -1 || bad != NULL)
                ok = 0;
        }
        check(ok, "reject truncated tflite at multiple offsets");
    }

    {
        struct bat_model *bad = NULL;
        int ok = 1;
        static const size_t pb_cuts[] = {
            1, 2, 5, 16, 128, 1024, 15000, 31000, 31080, 31081
        };
        for (size_t i = 0; i < sizeof pb_cuts / sizeof pb_cuts[0]; i++) {
            if (bat_model_load(&bad, tfl, tfl_len, pb, pb_cuts[i]) != -1 || bad != NULL)
                ok = 0;
        }
        check(ok, "reject truncated preprocessor at multiple offsets");
    }

    {
        struct bat_model *bad = NULL;
        int ok = 1;
        uint32_t rng = 0x12345678u;
        uint8_t *rnd = malloc(4096);
        if (!rnd) {
            free(tfl);
            free(pb);
            return 1;
        }
        for (int trial = 0; trial < 32; trial++) {
            size_t rlen = 8 + (xorshift32(&rng) % 4088);
            for (size_t i = 0; i < rlen; i++)
                rnd[i] = (uint8_t)xorshift32(&rng);
            /* Half the trials keep TFL3 magic so the FlatBuffer parser exercises
             * its offset and vtable bounds checks. */
            if (trial & 1)
                memcpy(rnd + 4, "TFL3", 4);
            if (bat_model_load(&bad, rnd, rlen, pb, pb_len) != -1 || bad != NULL)
                ok = 0;
            if (bat_model_load(&bad, tfl, tfl_len, rnd, rlen) != -1 || bad != NULL)
                ok = 0;
        }
        free(rnd);
        check(ok, "reject random bytes (including TFL3-prefixed fuzz inputs)");
    }

    {
        /* Mutate specific fields in a valid tflite buffer to ensure unsupported
         * activations, tensor types, and opcodes are rejected cleanly. */
        struct bat_model *bad = NULL;
        uint8_t *mut = malloc(tfl_len);
        if (!mut) {
            free(tfl);
            free(pb);
            return 1;
        }
        int ok = 1;

        /* Mutate op[0] fused_activation_function (at verified offset 122803) from
         * RELU (1) to RELU6 (3): must be rejected. */
        memcpy(mut, tfl, tfl_len);
        if (mut[122803] != 1) {
            ok = 0;
        } else {
            mut[122803] = 3;
            if (bat_model_load(&bad, mut, tfl_len, pb, pb_len) != -1 || bad != NULL)
                ok = 0;
        }

        /* Mutate OperatorCode[0] deprecated_builtin_code (124323) and builtin_code
         * (124316) from FULLY_CONNECTED (9) to ADD (0): must be rejected. */
        memcpy(mut, tfl, tfl_len);
        if (mut[124323] != 9 || mut[124316] != 9) {
            ok = 0;
        } else {
            mut[124323] = 0;
            mut[124316] = 0;
            if (bat_model_load(&bad, mut, tfl_len, pb, pb_len) != -1 || bad != NULL)
                ok = 0;
        }

        /* Corrupt root offset out of bounds. */
        memcpy(mut, tfl, tfl_len);
        mut[0] = 0xff;
        mut[1] = 0xff;
        mut[2] = 0xff;
        mut[3] = 0x7f;
        if (bat_model_load(&bad, mut, tfl_len, pb, pb_len) != -1 || bad != NULL)
            ok = 0;

        free(mut);
        check(ok, "reject unsupported activation (RELU6), unsupported opcode, and bad root offset");
    }

    free(tfl);
    free(pb);
    return g_failures ? 1 : 0;
}
