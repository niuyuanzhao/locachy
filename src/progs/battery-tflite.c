/* Dependency-free pure-C TFLite FlatBuffer + ChromiumOS ExamplePreprocessor
 * inference engine for the Adaptive Charging model.
 *
 * Preprocessor semantics follow ChromiumOS platform2:
 *   ml/proto/example_preprocessor.proto
 *   ml/example_preprocessor/example_preprocessing.cc
 *   ml/example_preprocessor/ranker_example_util.cc
 *   ml/dbus_service/tf_model_graph_executor_util.cc
 *
 * TFLite FlatBuffer layout follows tensorflow/lite/schema/schema.fbs (v3).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "battery-tflite.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants and limits
 * -------------------------------------------------------------------------- */

#define TFL_MAX_TENSORS        1024u
#define TFL_MAX_OPS            1024u
#define TFL_MAX_BUFFERS        4096u
#define TFL_MAX_OPCODES        256u
#define TFL_MAX_TENSOR_ELEMS   (16u * 1024u * 1024u)
#define PB_MAX_STRINGS         65536u
#define PB_MAX_BOUNDARIES      4096u
#define PB_MAX_MAP_ENTRIES     65536u

/* TFLite schema.fbs enums */
enum {
    TFL_TENSOR_FLOAT32 = 0,
};

enum {
    TFL_OP_FULLY_CONNECTED = 9,
    TFL_OP_SOFTMAX = 25,
};

enum {
    TFL_ACT_NONE = 0,
    TFL_ACT_RELU = 1,
};

enum {
    TFL_OPTS_NONE = 0,
    TFL_OPTS_FULLY_CONNECTED = 8,
    TFL_OPTS_SOFTMAX = 9,
};

enum {
    TFL_FC_WEIGHTS_DEFAULT = 0,
};

/* --------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

struct pb_bucketizer {
    char *name;
    float *boundaries;
    size_t count;
    size_t seq;
};

struct pb_index_entry {
    char *name;
    int32_t index;
    size_t seq;
};

struct pb_normalizer {
    char *name;
    float divisor;
    size_t seq;
};

struct preproc_cfg {
    char **missing_features;
    size_t n_missing;

    struct pb_bucketizer *bucketizers;
    size_t n_bucketizers;

    struct pb_index_entry *feature_indices;
    size_t n_feature_indices;
    size_t vec_size;

    struct pb_normalizer *normalizers;
    size_t n_normalizers;

    char **convert_to_string;
    size_t n_convert_to_string;
};

struct tfl_tensor {
    int32_t dims[4];
    size_t ndims;
    size_t num_elems;
    float *const_data; /* non-NULL if initialized from a non-empty buffer */
};

enum tfl_op_kind {
    OP_FULLY_CONNECTED,
    OP_SOFTMAX,
};

struct tfl_op {
    enum tfl_op_kind kind;
    int32_t in_tensor;
    int32_t weights_tensor;
    int32_t bias_tensor; /* -1 if absent */
    int32_t out_tensor;
    int32_t in_dim;
    int32_t out_dim;
    int32_t batch_dim;
    int fused_act;
    float beta;
};

struct tfl_graph {
    struct tfl_tensor *tensors;
    size_t n_tensors;
    struct tfl_op *ops;
    size_t n_ops;
    int32_t input_tensor;
    int32_t output_tensor;
};

struct bat_model {
    struct tfl_graph graph;
    struct preproc_cfg preproc;
};

/* --------------------------------------------------------------------------
 * Safe little-endian readers
 * -------------------------------------------------------------------------- */

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t read_i32_le(const uint8_t *p)
{
    uint32_t u = read_u32_le(p);
    int32_t s;
    memcpy(&s, &u, sizeof s);
    return s;
}

static float read_f32_le(const uint8_t *p)
{
    uint32_t u = read_u32_le(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static int mul_size_overflow(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return -1;
    *out = a * b;
    return 0;
}

/* --------------------------------------------------------------------------
 * Safe FlatBuffer reader
 * -------------------------------------------------------------------------- */

struct fb_buf {
    const uint8_t *data;
    size_t len;
};

struct fb_table {
    size_t pos;
    size_t vtable_pos;
    uint16_t vtable_len;
    uint16_t table_len;
};

static int fb_in_bounds(const struct fb_buf *fb, size_t off, size_t n)
{
    return off <= fb->len && n <= fb->len - off;
}

static int fb_read_u16(const struct fb_buf *fb, size_t off, uint16_t *out)
{
    if (!fb_in_bounds(fb, off, 2))
        return -1;
    *out = read_u16_le(fb->data + off);
    return 0;
}

static int fb_read_u32(const struct fb_buf *fb, size_t off, uint32_t *out)
{
    if (!fb_in_bounds(fb, off, 4))
        return -1;
    *out = read_u32_le(fb->data + off);
    return 0;
}

static int fb_read_i32(const struct fb_buf *fb, size_t off, int32_t *out)
{
    if (!fb_in_bounds(fb, off, 4))
        return -1;
    *out = read_i32_le(fb->data + off);
    return 0;
}

static int fb_read_table(const struct fb_buf *fb, size_t pos, struct fb_table *t)
{
    int32_t voff;
    uint16_t vlen, tlen;
    size_t vpos;

    if (fb_read_i32(fb, pos, &voff) < 0)
        return -1;
    if (voff >= 0) {
        if ((size_t)voff > pos)
            return -1;
        vpos = pos - (size_t)voff;
    } else {
        uint32_t neg = (uint32_t)(-(int64_t)voff);
        if ((size_t)neg > fb->len - pos)
            return -1;
        vpos = pos + (size_t)neg;
    }
    if (fb_read_u16(fb, vpos, &vlen) < 0 || fb_read_u16(fb, vpos + 2, &tlen) < 0)
        return -1;
    if (vlen < 4 || (vlen & 1u) != 0 || tlen < 4)
        return -1;
    if (!fb_in_bounds(fb, vpos, vlen))
        return -1;
    if (!fb_in_bounds(fb, pos, tlen))
        return -1;

    t->pos = pos;
    t->vtable_pos = vpos;
    t->vtable_len = vlen;
    t->table_len = tlen;
    return 0;
}

/* Returns 1 and writes field's absolute offset if present, 0 if absent/default,
 * -1 if out of bounds. */
static int fb_field_pos(const struct fb_buf *fb, const struct fb_table *t,
                        size_t field_idx, size_t field_size, size_t *out_pos)
{
    size_t vslot;
    uint16_t foff;

    if (field_idx > (SIZE_MAX - 4) / 2)
        return -1;
    vslot = 4 + 2 * field_idx;
    if (vslot + 2 > t->vtable_len)
        return 0;
    if (fb_read_u16(fb, t->vtable_pos + vslot, &foff) < 0)
        return -1;
    if (foff == 0)
        return 0;
    if (foff < 4)
        return -1;
    if ((size_t)foff > t->table_len || field_size > t->table_len - (size_t)foff)
        return -1;
    if (!fb_in_bounds(fb, t->pos + (size_t)foff, field_size))
        return -1;
    *out_pos = t->pos + (size_t)foff;
    return 1;
}

/* Dereference a uoffset32 stored at `slot_pos`. */
static int fb_follow_uoff(const struct fb_buf *fb, size_t slot_pos, size_t *target)
{
    uint32_t rel;
    if (fb_read_u32(fb, slot_pos, &rel) < 0)
        return -1;
    if (rel == 0 || (size_t)rel > fb->len - slot_pos)
        return -1;
    *target = slot_pos + (size_t)rel;
    return 0;
}

/* Read a vector field: returns 1 if present (setting data_pos and count),
 * 0 if absent, -1 if malformed/out of bounds. */
static int fb_field_vector(const struct fb_buf *fb, const struct fb_table *t,
                           size_t field_idx, size_t elem_size,
                           size_t *data_pos, uint32_t *count)
{
    size_t slot, vec_pos, total_bytes;
    uint32_t n;
    int r = fb_field_pos(fb, t, field_idx, 4, &slot);
    if (r <= 0)
        return r;
    if (fb_follow_uoff(fb, slot, &vec_pos) < 0)
        return -1;
    if (fb_read_u32(fb, vec_pos, &n) < 0)
        return -1;
    if (mul_size_overflow((size_t)n, elem_size, &total_bytes) < 0)
        return -1;
    if (!fb_in_bounds(fb, vec_pos + 4, total_bytes))
        return -1;
    *data_pos = vec_pos + 4;
    *count = n;
    return 1;
}

static int fb_field_i8(const struct fb_buf *fb, const struct fb_table *t,
                       size_t field_idx, int8_t defval, int8_t *out)
{
    size_t pos;
    int r = fb_field_pos(fb, t, field_idx, 1, &pos);
    if (r < 0)
        return -1;
    *out = r ? (int8_t)fb->data[pos] : defval;
    return 0;
}

static int fb_field_u8(const struct fb_buf *fb, const struct fb_table *t,
                       size_t field_idx, uint8_t defval, uint8_t *out)
{
    size_t pos;
    int r = fb_field_pos(fb, t, field_idx, 1, &pos);
    if (r < 0)
        return -1;
    *out = r ? fb->data[pos] : defval;
    return 0;
}

static int fb_field_u32(const struct fb_buf *fb, const struct fb_table *t,
                        size_t field_idx, uint32_t defval, uint32_t *out)
{
    size_t pos;
    int r = fb_field_pos(fb, t, field_idx, 4, &pos);
    if (r < 0)
        return -1;
    if (!r) {
        *out = defval;
        return 0;
    }
    return fb_read_u32(fb, pos, out);
}

static int fb_field_i32(const struct fb_buf *fb, const struct fb_table *t,
                        size_t field_idx, int32_t defval, int32_t *out)
{
    size_t pos;
    int r = fb_field_pos(fb, t, field_idx, 4, &pos);
    if (r < 0)
        return -1;
    if (!r) {
        *out = defval;
        return 0;
    }
    return fb_read_i32(fb, pos, out);
}

static int fb_field_f32(const struct fb_buf *fb, const struct fb_table *t,
                        size_t field_idx, float defval, float *out)
{
    size_t pos;
    int r = fb_field_pos(fb, t, field_idx, 4, &pos);
    if (r < 0)
        return -1;
    if (!r) {
        *out = defval;
        return 0;
    }
    *out = read_f32_le(fb->data + pos);
    return 0;
}

static int fb_field_table(const struct fb_buf *fb, const struct fb_table *t,
                          size_t field_idx, struct fb_table *out)
{
    size_t slot, target;
    int r = fb_field_pos(fb, t, field_idx, 4, &slot);
    if (r <= 0)
        return r;
    if (fb_follow_uoff(fb, slot, &target) < 0)
        return -1;
    if (fb_read_table(fb, target, out) < 0)
        return -1;
    return 1;
}

/* --------------------------------------------------------------------------
 * TFLite graph parsing and validation
 * -------------------------------------------------------------------------- */

static void tfl_graph_free(struct tfl_graph *g)
{
    if (!g)
        return;
    if (g->tensors) {
        for (size_t i = 0; i < g->n_tensors; i++)
            free(g->tensors[i].const_data);
        free(g->tensors);
    }
    free(g->ops);
    memset(g, 0, sizeof *g);
}

struct raw_buffer_ref {
    size_t data_pos;
    uint32_t data_len;
};

static int parse_tflite(struct tfl_graph *g, const uint8_t *buf, size_t len)
{
    struct fb_buf fb = { buf, len };
    struct fb_table root, sg;
    uint32_t root_off, version;
    size_t opcodes_pos = 0, sgs_pos = 0, bufs_pos = 0;
    uint32_t n_opcodes = 0, n_sgs = 0, n_bufs = 0;
    int32_t *opcode_builtins = NULL;
    struct raw_buffer_ref *raw_bufs = NULL;
    uint8_t *tensor_written = NULL;
    int ret = -1;

    memset(g, 0, sizeof *g);
    g->input_tensor = -1;
    g->output_tensor = -1;

    if (!buf || len < 12)
        return -1;
    /* File identifier "TFL3" at offset 4..7 */
    if (memcmp(buf + 4, "TFL3", 4) != 0)
        return -1;
    if (fb_read_u32(&fb, 0, &root_off) < 0)
        return -1;
    if (fb_read_table(&fb, (size_t)root_off, &root) < 0)
        return -1;

    /* Model.version (field 0) */
    if (fb_field_u32(&fb, &root, 0, 0, &version) < 0 || version != 3)
        return -1;

    /* Model.operator_codes (field 1) */
    if (fb_field_vector(&fb, &root, 1, 4, &opcodes_pos, &n_opcodes) <= 0 ||
        n_opcodes == 0 || n_opcodes > TFL_MAX_OPCODES)
        goto fail;

    opcode_builtins = calloc(n_opcodes, sizeof *opcode_builtins);
    if (!opcode_builtins)
        goto fail;

    for (uint32_t i = 0; i < n_opcodes; i++) {
        size_t elem_slot = opcodes_pos + (size_t)i * 4;
        size_t oc_pos;
        struct fb_table oc;
        int8_t dep_code = 0;
        int32_t ext_code = 0;
        size_t custom_pos = 0;
        int r_custom;

        if (fb_follow_uoff(&fb, elem_slot, &oc_pos) < 0 ||
            fb_read_table(&fb, oc_pos, &oc) < 0)
            goto fail;
        /* Reject custom ops (field 1: custom_code string). */
        r_custom = fb_field_pos(&fb, &oc, 1, 4, &custom_pos);
        if (r_custom != 0)
            goto fail;
        if (fb_field_i8(&fb, &oc, 0, 0, &dep_code) < 0 ||
            fb_field_i32(&fb, &oc, 3, 0, &ext_code) < 0)
            goto fail;
        /* Per TFLite schema.fbs: deprecated_builtin_code is used unless it is
         * 127 (PLACEHOLDER_FOR_GREATER_OP_CODES) or builtin_code is populated. */
        int32_t code = (ext_code != 0) ? ext_code : (int32_t)(uint8_t)dep_code;
        if (dep_code != 0 && ext_code != 0 && ext_code != (int32_t)(uint8_t)dep_code)
            goto fail;
        if (code != TFL_OP_FULLY_CONNECTED && code != TFL_OP_SOFTMAX)
            goto fail;
        opcode_builtins[i] = code;
    }

    /* Model.buffers (field 4) */
    if (fb_field_vector(&fb, &root, 4, 4, &bufs_pos, &n_bufs) <= 0 ||
        n_bufs == 0 || n_bufs > TFL_MAX_BUFFERS)
        goto fail;

    raw_bufs = calloc(n_bufs, sizeof *raw_bufs);
    if (!raw_bufs)
        goto fail;

    for (uint32_t i = 0; i < n_bufs; i++) {
        size_t elem_slot = bufs_pos + (size_t)i * 4;
        size_t bpos;
        struct fb_table bt;
        size_t dpos = 0, dummy = 0;
        uint32_t dlen = 0;
        int r;

        if (fb_follow_uoff(&fb, elem_slot, &bpos) < 0 ||
            fb_read_table(&fb, bpos, &bt) < 0)
            goto fail;
        /* Reject external-offset buffers (fields 1/2: offset/size). */
        if (fb_field_pos(&fb, &bt, 1, 8, &dummy) != 0 ||
            fb_field_pos(&fb, &bt, 2, 8, &dummy) != 0)
            goto fail;
        r = fb_field_vector(&fb, &bt, 0, 1, &dpos, &dlen);
        if (r < 0)
            goto fail;
        if (r > 0) {
            raw_bufs[i].data_pos = dpos;
            raw_bufs[i].data_len = dlen;
        }
    }

    /* Model.subgraphs (field 2): require exactly one subgraph. */
    if (fb_field_vector(&fb, &root, 2, 4, &sgs_pos, &n_sgs) <= 0 || n_sgs != 1)
        goto fail;
    {
        size_t sg_pos;
        if (fb_follow_uoff(&fb, sgs_pos, &sg_pos) < 0 ||
            fb_read_table(&fb, sg_pos, &sg) < 0)
            goto fail;
    }

    /* SubGraph.tensors (field 0) */
    {
        size_t tvec_pos = 0;
        uint32_t n_tensors = 0;
        if (fb_field_vector(&fb, &sg, 0, 4, &tvec_pos, &n_tensors) <= 0 ||
            n_tensors == 0 || n_tensors > TFL_MAX_TENSORS)
            goto fail;

        g->tensors = calloc(n_tensors, sizeof *g->tensors);
        if (!g->tensors)
            goto fail;
        g->n_tensors = n_tensors;

        for (uint32_t i = 0; i < n_tensors; i++) {
            size_t tslot = tvec_pos + (size_t)i * 4;
            size_t tpos, shape_pos = 0;
            uint32_t shape_n = 0, buf_idx = 0;
            int8_t ttype = 0;
            uint8_t is_var = 0;
            struct fb_table tt, qt;
            size_t elems = 1;
            int rq;

            if (fb_follow_uoff(&fb, tslot, &tpos) < 0 ||
                fb_read_table(&fb, tpos, &tt) < 0)
                goto fail;

            /* Tensor.type (field 1): must be FLOAT32 (0). */
            if (fb_field_i8(&fb, &tt, 1, 0, &ttype) < 0 ||
                ttype != TFL_TENSOR_FLOAT32)
                goto fail;

            /* Tensor.is_variable (field 5): unsupported. */
            if (fb_field_u8(&fb, &tt, 5, 0, &is_var) < 0 || is_var != 0)
                goto fail;

            /* Tensor.quantization (field 4): allow absent or empty table,
             * reject non-empty scale/zero_point/details. */
            rq = fb_field_table(&fb, &tt, 4, &qt);
            if (rq < 0)
                goto fail;
            if (rq > 0) {
                size_t dp = 0;
                uint32_t dn = 0;
                uint8_t qdt = 0;
                int r2 = fb_field_vector(&fb, &qt, 2, 4, &dp, &dn);
                if (r2 < 0 || (r2 > 0 && dn > 0))
                    goto fail;
                int r3 = fb_field_vector(&fb, &qt, 3, 8, &dp, &dn);
                if (r3 < 0 || (r3 > 0 && dn > 0))
                    goto fail;
                if (fb_field_u8(&fb, &qt, 4, 0, &qdt) < 0 || qdt != 0)
                    goto fail;
            }

            /* Tensor.shape (field 0): require 1..4 positive dims. */
            if (fb_field_vector(&fb, &tt, 0, 4, &shape_pos, &shape_n) <= 0 ||
                shape_n == 0 || shape_n > 4)
                goto fail;
            g->tensors[i].ndims = shape_n;
            for (uint32_t d = 0; d < shape_n; d++) {
                int32_t dv;
                if (fb_read_i32(&fb, shape_pos + (size_t)d * 4, &dv) < 0 || dv <= 0)
                    goto fail;
                g->tensors[i].dims[d] = dv;
                if (mul_size_overflow(elems, (size_t)dv, &elems) < 0 ||
                    elems > TFL_MAX_TENSOR_ELEMS)
                    goto fail;
            }
            g->tensors[i].num_elems = elems;

            /* Tensor.buffer (field 2) */
            if (fb_field_u32(&fb, &tt, 2, 0, &buf_idx) < 0 || buf_idx >= n_bufs)
                goto fail;

            if (raw_bufs[buf_idx].data_len > 0) {
                size_t expected_bytes;
                if (mul_size_overflow(elems, 4, &expected_bytes) < 0 ||
                    raw_bufs[buf_idx].data_len != expected_bytes)
                    goto fail;
                g->tensors[i].const_data = malloc(expected_bytes);
                if (!g->tensors[i].const_data)
                    goto fail;
                for (size_t e = 0; e < elems; e++) {
                    float v = read_f32_le(buf + raw_bufs[buf_idx].data_pos + e * 4);
                    if (!isfinite(v))
                        goto fail;
                    g->tensors[i].const_data[e] = v;
                }
            }
        }
    }

    /* SubGraph.inputs (field 1) and outputs (field 2): require 1 input, 1 output. */
    {
        size_t in_pos = 0, out_pos = 0;
        uint32_t n_in = 0, n_out = 0;
        int32_t in_idx, out_idx;

        if (fb_field_vector(&fb, &sg, 1, 4, &in_pos, &n_in) <= 0 || n_in != 1)
            goto fail;
        if (fb_field_vector(&fb, &sg, 2, 4, &out_pos, &n_out) <= 0 || n_out != 1)
            goto fail;
        if (fb_read_i32(&fb, in_pos, &in_idx) < 0 ||
            fb_read_i32(&fb, out_pos, &out_idx) < 0)
            goto fail;
        if (in_idx < 0 || (size_t)in_idx >= g->n_tensors ||
            out_idx < 0 || (size_t)out_idx >= g->n_tensors)
            goto fail;
        if (g->tensors[in_idx].const_data != NULL ||
            g->tensors[out_idx].const_data != NULL)
            goto fail;

        /* Require output shape [1, BAT_ADAPTIVE_OUTPUTS] or [BAT_ADAPTIVE_OUTPUTS]. */
        const struct tfl_tensor *ot = &g->tensors[out_idx];
        if (!((ot->ndims == 2 && ot->dims[0] == 1 && ot->dims[1] == BAT_ADAPTIVE_OUTPUTS) ||
              (ot->ndims == 1 && ot->dims[0] == BAT_ADAPTIVE_OUTPUTS)))
            goto fail;

        /* Require input shape [1, N] or [N]. */
        const struct tfl_tensor *it = &g->tensors[in_idx];
        if (!((it->ndims == 2 && it->dims[0] == 1) || it->ndims == 1))
            goto fail;

        g->input_tensor = in_idx;
        g->output_tensor = out_idx;
    }

    tensor_written = calloc(g->n_tensors, 1);
    if (!tensor_written)
        goto fail;
    for (size_t i = 0; i < g->n_tensors; i++) {
        if (g->tensors[i].const_data != NULL)
            tensor_written[i] = 1;
    }
    tensor_written[g->input_tensor] = 1;

    /* SubGraph.operators (field 3) */
    {
        size_t opvec_pos = 0;
        uint32_t n_ops = 0;
        if (fb_field_vector(&fb, &sg, 3, 4, &opvec_pos, &n_ops) <= 0 ||
            n_ops == 0 || n_ops > TFL_MAX_OPS)
            goto fail;

        g->ops = calloc(n_ops, sizeof *g->ops);
        if (!g->ops)
            goto fail;
        g->n_ops = n_ops;

        for (uint32_t i = 0; i < n_ops; i++) {
            size_t opslot = opvec_pos + (size_t)i * 4;
            size_t op_pos, ins_pos = 0, outs_pos = 0;
            uint32_t n_ins = 0, n_outs = 0, oc_idx = 0;
            uint8_t opts_type = 0;
            struct fb_table opt, bot;
            int has_opts;
            int32_t builtin;

            if (fb_follow_uoff(&fb, opslot, &op_pos) < 0 ||
                fb_read_table(&fb, op_pos, &opt) < 0)
                goto fail;

            if (fb_field_u32(&fb, &opt, 0, 0, &oc_idx) < 0 || oc_idx >= n_opcodes)
                goto fail;
            builtin = opcode_builtins[oc_idx];

            if (fb_field_vector(&fb, &opt, 1, 4, &ins_pos, &n_ins) <= 0 ||
                fb_field_vector(&fb, &opt, 2, 4, &outs_pos, &n_outs) <= 0 ||
                n_outs != 1)
                goto fail;

            int32_t out_t;
            if (fb_read_i32(&fb, outs_pos, &out_t) < 0 ||
                out_t < 0 || (size_t)out_t >= g->n_tensors)
                goto fail;
            /* Output tensor cannot be a constant weight buffer. */
            if (g->tensors[out_t].const_data != NULL)
                goto fail;

            if (fb_field_u8(&fb, &opt, 3, 0, &opts_type) < 0)
                goto fail;
            has_opts = fb_field_table(&fb, &opt, 4, &bot);
            if (has_opts < 0)
                goto fail;
            if ((opts_type == TFL_OPTS_NONE) != (has_opts == 0))
                goto fail;
            /* Reject custom_options (field 5) and builtin_options_2 (field 11). */
            {
                size_t cpos = 0;
                uint32_t clen = 0;
                uint8_t opts2_type = 0;
                int rc = fb_field_vector(&fb, &opt, 5, 1, &cpos, &clen);
                if (rc < 0 || (rc > 0 && clen > 0))
                    goto fail;
                if (fb_field_u8(&fb, &opt, 11, 0, &opts2_type) < 0 || opts2_type != 0)
                    goto fail;
            }

            struct tfl_op *dst = &g->ops[i];
            dst->out_tensor = out_t;
            dst->bias_tensor = -1;

            if (builtin == TFL_OP_FULLY_CONNECTED) {
                int32_t in_t, w_t, b_t = -1;
                int8_t act = TFL_ACT_NONE, wfmt = TFL_FC_WEIGHTS_DEFAULT;
                uint8_t asym = 0;

                if (n_ins != 2 && n_ins != 3)
                    goto fail;
                if (fb_read_i32(&fb, ins_pos, &in_t) < 0 ||
                    fb_read_i32(&fb, ins_pos + 4, &w_t) < 0)
                    goto fail;
                if (n_ins == 3 && fb_read_i32(&fb, ins_pos + 8, &b_t) < 0)
                    goto fail;

                if (in_t < 0 || (size_t)in_t >= g->n_tensors || !tensor_written[in_t])
                    goto fail;
                if (w_t < 0 || (size_t)w_t >= g->n_tensors || !g->tensors[w_t].const_data)
                    goto fail;
                if (b_t != -1) {
                    if (b_t < 0 || (size_t)b_t >= g->n_tensors || !g->tensors[b_t].const_data)
                        goto fail;
                }
                if (in_t == out_t)
                    goto fail;

                if (has_opts) {
                    if (opts_type != TFL_OPTS_FULLY_CONNECTED)
                        goto fail;
                    if (fb_field_i8(&fb, &bot, 0, TFL_ACT_NONE, &act) < 0 ||
                        fb_field_i8(&fb, &bot, 1, TFL_FC_WEIGHTS_DEFAULT, &wfmt) < 0 ||
                        fb_field_u8(&fb, &bot, 3, 0, &asym) < 0)
                        goto fail;
                }
                if ((act != TFL_ACT_NONE && act != TFL_ACT_RELU) ||
                    wfmt != TFL_FC_WEIGHTS_DEFAULT || asym != 0)
                    goto fail;

                /* Validate shapes: weights [out_dim, in_dim], input [batch, in_dim],
                 * output [batch, out_dim], optional bias [out_dim]. */
                const struct tfl_tensor *wt = &g->tensors[w_t];
                const struct tfl_tensor *it = &g->tensors[in_t];
                const struct tfl_tensor *ot = &g->tensors[out_t];
                if (wt->ndims != 2)
                    goto fail;
                int32_t out_dim = wt->dims[0];
                int32_t in_dim = wt->dims[1];
                if (it->num_elems % (size_t)in_dim != 0)
                    goto fail;
                size_t batch = it->num_elems / (size_t)in_dim;
                if (batch == 0 || batch > INT32_MAX)
                    goto fail;
                size_t expected_out;
                if (mul_size_overflow(batch, (size_t)out_dim, &expected_out) < 0 ||
                    ot->num_elems != expected_out)
                    goto fail;
                if (b_t != -1) {
                    const struct tfl_tensor *bt = &g->tensors[b_t];
                    if (bt->ndims != 1 || bt->dims[0] != out_dim)
                        goto fail;
                }

                dst->kind = OP_FULLY_CONNECTED;
                dst->in_tensor = in_t;
                dst->weights_tensor = w_t;
                dst->bias_tensor = b_t;
                dst->in_dim = in_dim;
                dst->out_dim = out_dim;
                dst->batch_dim = (int32_t)batch;
                dst->fused_act = act;
            } else if (builtin == TFL_OP_SOFTMAX) {
                int32_t in_t;
                float beta = 1.0f;

                if (n_ins != 1)
                    goto fail;
                if (fb_read_i32(&fb, ins_pos, &in_t) < 0 ||
                    in_t < 0 || (size_t)in_t >= g->n_tensors || !tensor_written[in_t])
                    goto fail;
                if (in_t == out_t)
                    goto fail;

                if (!has_opts || opts_type != TFL_OPTS_SOFTMAX)
                    goto fail;
                if (fb_field_f32(&fb, &bot, 0, 0.0f, &beta) < 0)
                    goto fail;
                if (!isfinite(beta) || beta <= 0.0f)
                    goto fail;

                const struct tfl_tensor *it = &g->tensors[in_t];
                const struct tfl_tensor *ot = &g->tensors[out_t];
                if (it->ndims != ot->ndims || it->num_elems != ot->num_elems)
                    goto fail;
                for (size_t d = 0; d < it->ndims; d++) {
                    if (it->dims[d] != ot->dims[d])
                        goto fail;
                }
                int32_t last_dim = it->dims[it->ndims - 1];
                size_t batch = it->num_elems / (size_t)last_dim;
                if (batch == 0 || batch > INT32_MAX)
                    goto fail;

                dst->kind = OP_SOFTMAX;
                dst->in_tensor = in_t;
                dst->weights_tensor = -1;
                dst->in_dim = last_dim;
                dst->out_dim = last_dim;
                dst->batch_dim = (int32_t)batch;
                dst->beta = beta;
            } else {
                /* Any other operator is unsupported. */
                goto fail;
            }

            tensor_written[out_t] = 1;
        }
    }

    if (!tensor_written[g->output_tensor])
        goto fail;

    ret = 0;

fail:
    free(tensor_written);
    free(raw_bufs);
    free(opcode_builtins);
    if (ret < 0)
        tfl_graph_free(g);
    return ret;
}

/* --------------------------------------------------------------------------
 * Safe Protobuf wire decoder for ExamplePreprocessorConfig
 * -------------------------------------------------------------------------- */

struct pb_slice {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

static int pb_read_varint(struct pb_slice *s, uint64_t *out)
{
    uint64_t val = 0;
    unsigned shift = 0;
    while (s->pos < s->len) {
        uint8_t b = s->data[s->pos++];
        if (shift == 63 && b > 1)
            return -1;
        val |= (uint64_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *out = val;
            return 0;
        }
        shift += 7;
        if (shift > 63)
            return -1;
    }
    return -1;
}

static int pb_read_bytes(struct pb_slice *s, struct pb_slice *sub)
{
    uint64_t n;
    if (pb_read_varint(s, &n) < 0)
        return -1;
    if (n > s->len - s->pos)
        return -1;
    sub->data = s->data + s->pos;
    sub->len = (size_t)n;
    sub->pos = 0;
    s->pos += (size_t)n;
    return 0;
}

static int pb_read_fixed32(struct pb_slice *s, float *out)
{
    if (s->len - s->pos < 4)
        return -1;
    *out = read_f32_le(s->data + s->pos);
    s->pos += 4;
    return 0;
}

static int pb_skip_field(struct pb_slice *s, uint32_t wire_type)
{
    uint64_t dummy;
    struct pb_slice sub;
    switch (wire_type) {
    case 0:
        return pb_read_varint(s, &dummy);
    case 1:
        if (s->len - s->pos < 8)
            return -1;
        s->pos += 8;
        return 0;
    case 2:
        return pb_read_bytes(s, &sub);
    case 5:
        if (s->len - s->pos < 4)
            return -1;
        s->pos += 4;
        return 0;
    default:
        return -1;
    }
}

static char *pb_dup_string(const struct pb_slice *sub)
{
    /* Reject embedded NUL bytes so C-string lookups stay exact. */
    if (memchr(sub->data, '\0', sub->len) != NULL)
        return NULL;
    char *p = malloc(sub->len + 1);
    if (!p)
        return NULL;
    if (sub->len > 0)
        memcpy(p, sub->data, sub->len);
    p[sub->len] = '\0';
    return p;
}

static void preproc_free(struct preproc_cfg *p)
{
    if (!p)
        return;
    for (size_t i = 0; i < p->n_missing; i++)
        free(p->missing_features[i]);
    free(p->missing_features);

    for (size_t i = 0; i < p->n_bucketizers; i++) {
        free(p->bucketizers[i].name);
        free(p->bucketizers[i].boundaries);
    }
    free(p->bucketizers);

    for (size_t i = 0; i < p->n_feature_indices; i++)
        free(p->feature_indices[i].name);
    free(p->feature_indices);

    for (size_t i = 0; i < p->n_normalizers; i++)
        free(p->normalizers[i].name);
    free(p->normalizers);

    for (size_t i = 0; i < p->n_convert_to_string; i++)
        free(p->convert_to_string[i]);
    free(p->convert_to_string);

    memset(p, 0, sizeof *p);
}

static int grow_array(void **ptr, size_t *cap, size_t elem_size, size_t max_elems)
{
    size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
    if (new_cap > max_elems)
        new_cap = max_elems;
    if (new_cap <= *cap)
        return -1;
    size_t bytes;
    if (mul_size_overflow(new_cap, elem_size, &bytes) < 0)
        return -1;
    void *np = realloc(*ptr, bytes);
    if (!np)
        return -1;
    *ptr = np;
    *cap = new_cap;
    return 0;
}

static int cmp_str_ptr(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

static int cmp_index_entry(const void *a, const void *b)
{
    const struct pb_index_entry *ea = a;
    const struct pb_index_entry *eb = b;
    int c = strcmp(ea->name, eb->name);
    if (c != 0)
        return c;
    return (ea->seq > eb->seq) - (ea->seq < eb->seq);
}

static int cmp_normalizer(const void *a, const void *b)
{
    const struct pb_normalizer *na = a;
    const struct pb_normalizer *nb = b;
    int c = strcmp(na->name, nb->name);
    if (c != 0)
        return c;
    return (na->seq > nb->seq) - (na->seq < nb->seq);
}

static int cmp_bucketizer(const void *a, const void *b)
{
    const struct pb_bucketizer *ba = a;
    const struct pb_bucketizer *bb = b;
    int c = strcmp(ba->name, bb->name);
    if (c != 0)
        return c;
    return (ba->seq > bb->seq) - (ba->seq < bb->seq);
}

static int parse_boundaries(struct pb_slice s, float **out_vals, size_t *out_n)
{
    float *vals = NULL;
    size_t n = 0, cap = 0;

    while (s.pos < s.len) {
        uint64_t tag;
        if (pb_read_varint(&s, &tag) < 0)
            goto fail;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (fn == 0)
            goto fail;
        if (fn == 1 && wt == 5) {
            float v;
            if (pb_read_fixed32(&s, &v) < 0 || !isfinite(v))
                goto fail;
            if (n == cap && grow_array((void **)&vals, &cap, sizeof *vals, PB_MAX_BOUNDARIES) < 0)
                goto fail;
            vals[n++] = v;
        } else if (fn == 1 && wt == 2) {
            struct pb_slice packed;
            if (pb_read_bytes(&s, &packed) < 0 || (packed.len % 4) != 0)
                goto fail;
            while (packed.pos < packed.len) {
                float v;
                if (pb_read_fixed32(&packed, &v) < 0 || !isfinite(v))
                    goto fail;
                if (n == cap && grow_array((void **)&vals, &cap, sizeof *vals, PB_MAX_BOUNDARIES) < 0)
                    goto fail;
                vals[n++] = v;
            }
        } else {
            if (pb_skip_field(&s, wt) < 0)
                goto fail;
        }
    }

    *out_vals = vals;
    *out_n = n;
    return 0;

fail:
    free(vals);
    return -1;
}

static int parse_bucketizer_entry(struct pb_slice s, struct pb_bucketizer *out)
{
    char *key = NULL;
    float *bounds = NULL;
    size_t n_bounds = 0;

    while (s.pos < s.len) {
        uint64_t tag;
        if (pb_read_varint(&s, &tag) < 0)
            goto fail;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (fn == 0)
            goto fail;
        if (fn == 1 && wt == 2) {
            struct pb_slice ksub;
            if (pb_read_bytes(&s, &ksub) < 0)
                goto fail;
            free(key);
            key = pb_dup_string(&ksub);
            if (!key)
                goto fail;
        } else if (fn == 2 && wt == 2) {
            struct pb_slice bsub;
            float *nb = NULL;
            size_t nn = 0;
            if (pb_read_bytes(&s, &bsub) < 0)
                goto fail;
            if (parse_boundaries(bsub, &nb, &nn) < 0)
                goto fail;
            /* Protobuf merges repeated fields if submessage appears multiple times. */
            if (nn > 0) {
                if (n_bounds > PB_MAX_BOUNDARIES - nn) {
                    free(nb);
                    goto fail;
                }
                float *merged = realloc(bounds, (n_bounds + nn) * sizeof *merged);
                if (!merged) {
                    free(nb);
                    goto fail;
                }
                memcpy(merged + n_bounds, nb, nn * sizeof *merged);
                bounds = merged;
                n_bounds += nn;
            }
            free(nb);
        } else {
            if (pb_skip_field(&s, wt) < 0)
                goto fail;
        }
    }

    if (!key)
        goto fail;
    out->name = key;
    out->boundaries = bounds;
    out->count = n_bounds;
    return 0;

fail:
    free(key);
    free(bounds);
    return -1;
}

static int parse_index_entry(struct pb_slice s, struct pb_index_entry *out)
{
    char *key = NULL;
    int32_t val = 0;

    while (s.pos < s.len) {
        uint64_t tag;
        if (pb_read_varint(&s, &tag) < 0)
            goto fail;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (fn == 0)
            goto fail;
        if (fn == 1 && wt == 2) {
            struct pb_slice ksub;
            if (pb_read_bytes(&s, &ksub) < 0)
                goto fail;
            free(key);
            key = pb_dup_string(&ksub);
            if (!key)
                goto fail;
        } else if (fn == 2 && wt == 0) {
            uint64_t raw;
            if (pb_read_varint(&s, &raw) < 0)
                goto fail;
            uint32_t lo = (uint32_t)raw;
            memcpy(&val, &lo, sizeof val);
        } else {
            if (pb_skip_field(&s, wt) < 0)
                goto fail;
        }
    }

    if (!key)
        goto fail;
    out->name = key;
    out->index = val;
    return 0;

fail:
    free(key);
    return -1;
}

static int parse_normalizer_entry(struct pb_slice s, struct pb_normalizer *out)
{
    char *key = NULL;
    float div = 0.0f;

    while (s.pos < s.len) {
        uint64_t tag;
        if (pb_read_varint(&s, &tag) < 0)
            goto fail;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (fn == 0)
            goto fail;
        if (fn == 1 && wt == 2) {
            struct pb_slice ksub;
            if (pb_read_bytes(&s, &ksub) < 0)
                goto fail;
            free(key);
            key = pb_dup_string(&ksub);
            if (!key)
                goto fail;
        } else if (fn == 2 && wt == 5) {
            if (pb_read_fixed32(&s, &div) < 0 || !isfinite(div))
                goto fail;
        } else {
            if (pb_skip_field(&s, wt) < 0)
                goto fail;
        }
    }

    if (!key)
        goto fail;
    out->name = key;
    out->divisor = div;
    return 0;

fail:
    free(key);
    return -1;
}

static int parse_preproc(struct preproc_cfg *p, const uint8_t *buf, size_t len)
{
    struct pb_slice s = { buf, len, 0 };
    size_t cap_missing = 0, cap_buck = 0, cap_idx = 0, cap_norm = 0, cap_c2s = 0;

    memset(p, 0, sizeof *p);
    if (!buf || len == 0)
        return -1;

    while (s.pos < s.len) {
        uint64_t tag;
        if (pb_read_varint(&s, &tag) < 0)
            goto fail;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (fn == 0)
            goto fail;

        if (fn == 1 && wt == 2) {
            struct pb_slice sub;
            if (pb_read_bytes(&s, &sub) < 0)
                goto fail;
            char *str = pb_dup_string(&sub);
            if (!str)
                goto fail;
            if (p->n_missing == cap_missing &&
                grow_array((void **)&p->missing_features, &cap_missing,
                           sizeof *p->missing_features, PB_MAX_STRINGS) < 0) {
                free(str);
                goto fail;
            }
            p->missing_features[p->n_missing++] = str;
        } else if (fn == 2 && wt == 2) {
            struct pb_slice sub;
            struct pb_bucketizer b = {0};
            if (pb_read_bytes(&s, &sub) < 0 ||
                parse_bucketizer_entry(sub, &b) < 0)
                goto fail;
            b.seq = p->n_bucketizers;
            if (p->n_bucketizers == cap_buck &&
                grow_array((void **)&p->bucketizers, &cap_buck,
                           sizeof *p->bucketizers, PB_MAX_MAP_ENTRIES) < 0) {
                free(b.name);
                free(b.boundaries);
                goto fail;
            }
            p->bucketizers[p->n_bucketizers++] = b;
        } else if (fn == 3 && wt == 2) {
            struct pb_slice sub;
            struct pb_index_entry e = {0};
            if (pb_read_bytes(&s, &sub) < 0 ||
                parse_index_entry(sub, &e) < 0)
                goto fail;
            e.seq = p->n_feature_indices;
            if (p->n_feature_indices == cap_idx &&
                grow_array((void **)&p->feature_indices, &cap_idx,
                           sizeof *p->feature_indices, PB_MAX_MAP_ENTRIES) < 0) {
                free(e.name);
                goto fail;
            }
            p->feature_indices[p->n_feature_indices++] = e;
        } else if (fn == 4 && wt == 2) {
            struct pb_slice sub;
            struct pb_normalizer nr = {0};
            if (pb_read_bytes(&s, &sub) < 0 ||
                parse_normalizer_entry(sub, &nr) < 0)
                goto fail;
            nr.seq = p->n_normalizers;
            if (p->n_normalizers == cap_norm &&
                grow_array((void **)&p->normalizers, &cap_norm,
                           sizeof *p->normalizers, PB_MAX_MAP_ENTRIES) < 0) {
                free(nr.name);
                goto fail;
            }
            p->normalizers[p->n_normalizers++] = nr;
        } else if (fn == 5 && wt == 2) {
            struct pb_slice sub;
            if (pb_read_bytes(&s, &sub) < 0)
                goto fail;
            char *str = pb_dup_string(&sub);
            if (!str)
                goto fail;
            if (p->n_convert_to_string == cap_c2s &&
                grow_array((void **)&p->convert_to_string, &cap_c2s,
                           sizeof *p->convert_to_string, PB_MAX_STRINGS) < 0) {
                free(str);
                goto fail;
            }
            p->convert_to_string[p->n_convert_to_string++] = str;
        } else {
            /* Strict: reject unexpected field numbers or wrong wire types for
             * ExamplePreprocessorConfig so random/corrupt inputs fail cleanly. */
            goto fail;
        }
    }

    if (p->n_feature_indices == 0)
        goto fail;

    /* Sort map/set tables for O(log N) lookup and deduplicate map keys
     * using protobuf map semantics (last entry in wire order wins). */
    if (p->n_bucketizers > 1) {
        qsort(p->bucketizers, p->n_bucketizers, sizeof *p->bucketizers, cmp_bucketizer);
        size_t w = 0;
        for (size_t i = 0; i < p->n_bucketizers; i++) {
            if (w > 0 && strcmp(p->bucketizers[w - 1].name, p->bucketizers[i].name) == 0) {
                free(p->bucketizers[w - 1].name);
                free(p->bucketizers[w - 1].boundaries);
                p->bucketizers[w - 1] = p->bucketizers[i];
            } else {
                p->bucketizers[w++] = p->bucketizers[i];
            }
        }
        p->n_bucketizers = w;
    }

    if (p->n_feature_indices > 1) {
        qsort(p->feature_indices, p->n_feature_indices,
              sizeof *p->feature_indices, cmp_index_entry);
        size_t w = 0;
        for (size_t i = 0; i < p->n_feature_indices; i++) {
            if (w > 0 && strcmp(p->feature_indices[w - 1].name,
                                p->feature_indices[i].name) == 0) {
                free(p->feature_indices[w - 1].name);
                p->feature_indices[w - 1] = p->feature_indices[i];
            } else {
                p->feature_indices[w++] = p->feature_indices[i];
            }
        }
        p->n_feature_indices = w;
    }

    if (p->n_normalizers > 1) {
        qsort(p->normalizers, p->n_normalizers,
              sizeof *p->normalizers, cmp_normalizer);
        size_t w = 0;
        for (size_t i = 0; i < p->n_normalizers; i++) {
            if (w > 0 && strcmp(p->normalizers[w - 1].name, p->normalizers[i].name) == 0) {
                free(p->normalizers[w - 1].name);
                p->normalizers[w - 1] = p->normalizers[i];
            } else {
                p->normalizers[w++] = p->normalizers[i];
            }
        }
        p->n_normalizers = w;
    }

    if (p->n_convert_to_string > 1) {
        qsort(p->convert_to_string, p->n_convert_to_string,
              sizeof *p->convert_to_string, cmp_str_ptr);
    }

    /* Upstream ExamplePreprocessor::Vectorization sizes the vector to
     * config.feature_indices().size() and writes at find_index->second; every
     * index must therefore lie in [0, n_feature_indices). */
    p->vec_size = p->n_feature_indices;
    for (size_t i = 0; i < p->n_feature_indices; i++) {
        if (p->feature_indices[i].index < 0 ||
            (size_t)p->feature_indices[i].index >= p->vec_size)
            goto fail;
    }

    return 0;

fail:
    preproc_free(p);
    return -1;
}

/* --------------------------------------------------------------------------
 * Lookup helpers
 * -------------------------------------------------------------------------- */

static const struct pb_index_entry *find_feature_index(const struct preproc_cfg *p,
                                                       const char *key)
{
    size_t lo = 0, hi = p->n_feature_indices;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(p->feature_indices[mid].name, key);
        if (c == 0)
            return &p->feature_indices[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static const struct pb_normalizer *find_normalizer(const struct preproc_cfg *p,
                                                   const char *name)
{
    size_t lo = 0, hi = p->n_normalizers;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(p->normalizers[mid].name, name);
        if (c == 0)
            return &p->normalizers[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static const struct pb_bucketizer *find_bucketizer(const struct preproc_cfg *p,
                                                   const char *name)
{
    size_t lo = 0, hi = p->n_bucketizers;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(p->bucketizers[mid].name, name);
        if (c == 0)
            return &p->bucketizers[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static int in_convert_to_string(const struct preproc_cfg *p, const char *name)
{
    size_t lo = 0, hi = p->n_convert_to_string;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(p->convert_to_string[mid], name);
        if (c == 0)
            return 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * ExamplePreprocessor::Process execution
 * -------------------------------------------------------------------------- */

enum work_feat_kind {
    WF_INT32,
    WF_FLOAT,
    WF_STRING,
};

struct work_feat {
    const char *name;
    enum work_feat_kind kind;
    int32_t i32;
    float f32;
    char str[32];
};

static int set_vector_key(const struct preproc_cfg *p, float *vec,
                          const char *prefix, const char *suffix, float value)
{
    char stack_buf[256];
    char *key = stack_buf;
    size_t plen = strlen(prefix);
    size_t slen = suffix ? strlen(suffix) : 0;

    if (suffix && slen > 0) {
        if (plen > SIZE_MAX - slen - 2)
            return -1;
        size_t need = plen + 1 + slen + 1;
        if (need > sizeof stack_buf) {
            key = malloc(need);
            if (!key)
                return -1;
        }
        memcpy(key, prefix, plen);
        key[plen] = '_';
        memcpy(key + plen + 1, suffix, slen);
        key[plen + 1 + slen] = '\0';
    } else {
        key = (char *)prefix;
    }

    const struct pb_index_entry *e = find_feature_index(p, key);
    if (e)
        vec[e->index] = value;

    if (key != stack_buf && key != prefix)
        free(key);
    return 0;
}

static int preprocess_features(const struct preproc_cfg *p,
                               const struct bat_feature *feats, size_t n,
                               float *vec)
{
    struct work_feat *wf = NULL;
    size_t n_wf = 0;
    int ret = -1;

    memset(vec, 0, p->vec_size * sizeof *vec);

    if (n > 0) {
        if (!feats)
            return -1;
        wf = calloc(n, sizeof *wf);
        if (!wf)
            return -1;
        /* Deduplicate input features by name (last write wins, matching
         * protobuf RankerExample map<string, Feature>). */
        for (size_t i = 0; i < n; i++) {
            const struct bat_feature *src = &feats[i];
            if (!src->name || !isfinite(src->value))
                goto out;
            if (src->kind != BAT_FEAT_INT32 && src->kind != BAT_FEAT_FLOAT)
                goto out;

            size_t slot = n_wf;
            for (size_t j = 0; j < n_wf; j++) {
                if (strcmp(wf[j].name, src->name) == 0) {
                    slot = j;
                    break;
                }
            }
            if (slot == n_wf)
                n_wf++;

            wf[slot].name = src->name;
            if (src->kind == BAT_FEAT_INT32) {
                if (src->value < (double)INT32_MIN || src->value > (double)INT32_MAX)
                    goto out;
                wf[slot].kind = WF_INT32;
                wf[slot].i32 = (int32_t)src->value;
                wf[slot].f32 = 0.0f;
            } else {
                if (src->value < -(double)FLT_MAX || src->value > (double)FLT_MAX)
                    goto out;
                wf[slot].kind = WF_FLOAT;
                wf[slot].f32 = (float)src->value;
                wf[slot].i32 = 0;
            }
            wf[slot].str[0] = '\0';
        }
    }

    /* Step 1: AddMissingFeatures (example_preprocessing.cc lines 49-61).
     * For every name in config.missing_features not present in the example,
     * emit "_MissingFeature_<name>" = 1.0 if present in feature_indices. */
    for (size_t i = 0; i < p->n_missing; i++) {
        const char *mname = p->missing_features[i];
        int present = 0;
        for (size_t j = 0; j < n_wf; j++) {
            if (strcmp(wf[j].name, mname) == 0) {
                present = 1;
                break;
            }
        }
        if (!present) {
            if (set_vector_key(p, vec, "_MissingFeature", mname, 1.0f) < 0)
                goto out;
        }
    }

    /* Step 2: NormalizeFeatures (example_preprocessing.cc lines 107-128).
     * Runs before AddBucketizedFeatures in upstream ExamplePreprocessor::Process
     * (lines 42-46). If present as int32 or float: divide by divisor (failing if
     * divisor == 0, matching AcceptablePreprocessResult rejecting kNormalizerIsZero),
     * clamp to [-1.0f, 1.0f], and replace the feature as float. */
    for (size_t j = 0; j < n_wf; j++) {
        const struct pb_normalizer *nr = find_normalizer(p, wf[j].name);
        if (!nr)
            continue;
        if (nr->divisor == 0.0f)
            goto out;
        float val = (wf[j].kind == WF_INT32) ? (float)wf[j].i32 : wf[j].f32;
        val = val / nr->divisor;
        if (val < -1.0f)
            val = -1.0f;
        else if (val > 1.0f)
            val = 1.0f;
        wf[j].kind = WF_FLOAT;
        wf[j].f32 = val;
    }

    /* Step 3: AddBucketizedFeatures (example_preprocessing.cc lines 63-105).
     * For each present feature in config.bucketizers (int32 or float): find the
     * first index where value < boundaries[index] (or boundaries.size() if none)
     * and replace the feature with string(index). */
    for (size_t j = 0; j < n_wf; j++) {
        const struct pb_bucketizer *b = find_bucketizer(p, wf[j].name);
        if (!b)
            continue;
        float val = (wf[j].kind == WF_INT32) ? (float)wf[j].i32 : wf[j].f32;
        size_t idx = 0;
        for (; idx < b->count; idx++) {
            if (val < b->boundaries[idx])
                break;
        }
        wf[j].kind = WF_STRING;
        snprintf(wf[j].str, sizeof wf[j].str, "%zu", idx);
    }

    /* Step 4: ConvertToStringFeatures (example_preprocessing.cc lines 130-156).
     * Int32 becomes decimal string; string stays string; float is rejected
     * (upstream sets kNonConvertibleToStringFeatureType, which
     * AcceptablePreprocessResult rejects). */
    for (size_t j = 0; j < n_wf; j++) {
        if (!in_convert_to_string(p, wf[j].name))
            continue;
        if (wf[j].kind == WF_INT32) {
            wf[j].kind = WF_STRING;
            snprintf(wf[j].str, sizeof wf[j].str, "%d", wf[j].i32);
        } else if (wf[j].kind == WF_STRING) {
            /* Keep as-is. */
        } else {
            goto out;
        }
    }

    /* Step 5: Vectorization via ExampleFloatIterator semantics
     * (example_preprocessing.cc lines 158-231).
     * - int32/float -> key is feature_name, value is float(value)
     * - string      -> key is FeatureFullname(feature_name, string_value), value 1.0
     * Missing feature_indices entries are ignored (kNoFeatureIndexFound is
     * accepted by AcceptablePreprocessResult). */
    for (size_t j = 0; j < n_wf; j++) {
        if (wf[j].kind == WF_INT32) {
            if (set_vector_key(p, vec, wf[j].name, NULL, (float)wf[j].i32) < 0)
                goto out;
        } else if (wf[j].kind == WF_FLOAT) {
            if (set_vector_key(p, vec, wf[j].name, NULL, wf[j].f32) < 0)
                goto out;
        } else {
            if (set_vector_key(p, vec, wf[j].name, wf[j].str, 1.0f) < 0)
                goto out;
        }
    }

    ret = 0;

out:
    free(wf);
    return ret;
}

/* --------------------------------------------------------------------------
 * Graph execution
 * -------------------------------------------------------------------------- */

static int run_graph(const struct tfl_graph *g, const float *input_vec,
                     double out[BAT_ADAPTIVE_OUTPUTS])
{
    float **slots = calloc(g->n_tensors, sizeof *slots);
    if (!slots)
        return -1;
    int ret = -1;

    for (size_t i = 0; i < g->n_tensors; i++) {
        if (g->tensors[i].const_data) {
            slots[i] = g->tensors[i].const_data;
        } else {
            slots[i] = calloc(g->tensors[i].num_elems, sizeof(float));
            if (!slots[i])
                goto out;
        }
    }

    memcpy(slots[g->input_tensor], input_vec,
           g->tensors[g->input_tensor].num_elems * sizeof(float));

    for (size_t i = 0; i < g->n_ops; i++) {
        const struct tfl_op *op = &g->ops[i];
        if (op->kind == OP_FULLY_CONNECTED) {
            const float *x = slots[op->in_tensor];
            const float *w = slots[op->weights_tensor];
            const float *b = (op->bias_tensor >= 0) ? slots[op->bias_tensor] : NULL;
            float *y = slots[op->out_tensor];
            size_t in_dim = (size_t)op->in_dim;
            size_t out_dim = (size_t)op->out_dim;
            size_t batch = (size_t)op->batch_dim;

            for (size_t n = 0; n < batch; n++) {
                const float *xn = x + n * in_dim;
                float *yn = y + n * out_dim;
                for (size_t r = 0; r < out_dim; r++) {
                    const float *wr = w + r * in_dim;
                    float acc = b ? b[r] : 0.0f;
                    for (size_t c = 0; c < in_dim; c++)
                        acc += wr[c] * xn[c];
                    if (op->fused_act == TFL_ACT_RELU && acc < 0.0f)
                        acc = 0.0f;
                    yn[r] = acc;
                }
            }
        } else if (op->kind == OP_SOFTMAX) {
            const float *x = slots[op->in_tensor];
            float *y = slots[op->out_tensor];
            size_t dim = (size_t)op->in_dim;
            size_t batch = (size_t)op->batch_dim;
            double beta = (double)op->beta;

            for (size_t n = 0; n < batch; n++) {
                const float *xn = x + n * dim;
                float *yn = y + n * dim;
                float max_val = xn[0];
                for (size_t c = 1; c < dim; c++) {
                    if (xn[c] > max_val)
                        max_val = xn[c];
                }
                double sum = 0.0;
                for (size_t c = 0; c < dim; c++) {
                    double e = exp(((double)xn[c] - (double)max_val) * beta);
                    sum += e;
                }
                if (!(sum > 0.0) || !isfinite(sum))
                    goto out;
                for (size_t c = 0; c < dim; c++) {
                    double e = exp(((double)xn[c] - (double)max_val) * beta);
                    yn[c] = (float)(e / sum);
                }
            }
        } else {
            goto out;
        }
    }

    const float *final_out = slots[g->output_tensor];
    for (size_t i = 0; i < BAT_ADAPTIVE_OUTPUTS; i++) {
        if (!isfinite(final_out[i]))
            goto out;
        out[i] = (double)final_out[i];
    }

    ret = 0;

out:
    for (size_t i = 0; i < g->n_tensors; i++) {
        if (slots[i] && slots[i] != g->tensors[i].const_data)
            free(slots[i]);
    }
    free(slots);
    return ret;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int bat_model_load(struct bat_model **out, const void *tflite, size_t tflite_len,
                   const void *preproc, size_t preproc_len)
{
    if (!out)
        return -1;
    *out = NULL;
    if (!tflite || tflite_len == 0 || !preproc || preproc_len == 0)
        return -1;

    struct bat_model *m = calloc(1, sizeof *m);
    if (!m)
        return -1;

    if (parse_tflite(&m->graph, (const uint8_t *)tflite, tflite_len) < 0)
        goto fail;
    if (parse_preproc(&m->preproc, (const uint8_t *)preproc, preproc_len) < 0)
        goto fail;

    /* Preprocessor vector width must match the graph's input tensor size. */
    if (m->graph.tensors[m->graph.input_tensor].num_elems != m->preproc.vec_size)
        goto fail;

    *out = m;
    return 0;

fail:
    bat_model_free(m);
    return -1;
}

int bat_model_predict(const struct bat_model *m, const struct bat_feature *feats,
                      size_t n, double out[BAT_ADAPTIVE_OUTPUTS])
{
    if (!m || !out || (n > 0 && !feats))
        return -1;

    float *vec = calloc(m->preproc.vec_size, sizeof *vec);
    if (!vec)
        return -1;

    int ret = -1;
    if (preprocess_features(&m->preproc, feats, n, vec) == 0 &&
        run_graph(&m->graph, vec, out) == 0)
        ret = 0;

    free(vec);
    return ret;
}

void bat_model_free(struct bat_model *m)
{
    if (!m)
        return;
    tfl_graph_free(&m->graph);
    preproc_free(&m->preproc);
    free(m);
}
