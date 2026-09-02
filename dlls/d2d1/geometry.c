/*
 * Copyright 2015 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d2d1_private.h"
#include <float.h>

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

#define D2D_FIGURE_FLAG_CLOSED          0x00000001u
#define D2D_FIGURE_FLAG_HOLLOW          0x00000002u

#define D2D_CDT_EDGE_FLAG_FREED         0x80000000u
#define D2D_CDT_EDGE_FLAG_VISITED(r)    (1u << (r))

#define D2D_FP_EPS (1.0f / (1 << FLT_MANT_DIG))

static const D2D1_MATRIX_3X2_F identity =
{{{
    1.0f, 0.0f,
    0.0f, 1.0f,
    0.0f, 0.0f,
}}};

enum d2d_cdt_edge_next
{
    D2D_EDGE_NEXT_ORIGIN = 0,
    D2D_EDGE_NEXT_ROT = 1,
    D2D_EDGE_NEXT_SYM = 2,
    D2D_EDGE_NEXT_TOR = 3,
};

enum d2d_vertex_type
{
    D2D_VERTEX_TYPE_NONE,
    D2D_VERTEX_TYPE_LINE,
    D2D_VERTEX_TYPE_BEZIER,
    D2D_VERTEX_TYPE_SPLIT_BEZIER,
    D2D_VERTEX_TYPE_END,
};

struct d2d_segment_idx
{
    size_t figure_idx;
    size_t vertex_idx;
    size_t control_idx;
};

enum d2d_segment_type
{
    D2D_SEGMENT_TYPE_BEZIERS = 0,
    D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS,
    D2D_SEGMENT_TYPE_LINES,
    D2D_SEGMENT_TYPE_ARCS,
};

struct d2d_segment
{
    uint32_t type;
    uint32_t flags;
    uint32_t count;
};

struct d2d_segment_beziers
{
    uint32_t type;
    uint32_t flags;
    uint32_t count;
    D2D1_BEZIER_SEGMENT segments[1];
};

struct d2d_segment_quadratic_beziers
{
    uint32_t type;
    uint32_t flags;
    uint32_t count;
    D2D1_QUADRATIC_BEZIER_SEGMENT segments[1];
};

struct d2d_segment_lines
{
    uint32_t type;
    uint32_t flags;
    uint32_t count;
    D2D1_POINT_2F points[1];
};

struct d2d_segment_arcs
{
    uint32_t type;
    uint32_t flags;
    uint32_t count;
    D2D1_ARC_SEGMENT segments[1];
};

struct d2d_figure
{
    D2D1_POINT_2F *vertices;
    size_t vertices_size;
    enum d2d_vertex_type *vertex_types;
    size_t vertex_types_size;
    size_t vertex_count;

    D2D1_POINT_2F *bezier_controls;
    size_t bezier_controls_size;
    size_t bezier_control_count;

    D2D1_POINT_2F *original_bezier_controls;
    size_t original_bezier_controls_size;
    size_t original_bezier_control_count;

    D2D1_RECT_F bounds;
    unsigned int flags;

    struct
    {
        uint8_t *data;
        size_t size;
        size_t capacity;

        size_t current;
        size_t count;
    } segments;
};

static struct d2d_segment *d2d_figure_get_current_segment(struct d2d_figure *figure)
{
    if (!figure->segments.data)
        return NULL;

    return (struct d2d_segment *)(figure->segments.data + figure->segments.current);
}

static size_t d2d_figure_get_segment_data_size(enum d2d_segment_type type)
{
    if (type == D2D_SEGMENT_TYPE_BEZIERS) return sizeof(D2D1_BEZIER_SEGMENT);
    if (type == D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS) return sizeof(D2D1_QUADRATIC_BEZIER_SEGMENT);
    if (type == D2D_SEGMENT_TYPE_LINES) return sizeof(D2D1_POINT_2F);
    return sizeof(D2D1_ARC_SEGMENT);
}

static bool d2d_figure_new_segment(struct d2d_geometry *geometry, enum d2d_segment_type type,
        const void *data, unsigned int count)
{
    struct d2d_figure *figure = &geometry->u.path.figures[geometry->u.path.figure_count - 1];
    size_t element_size = d2d_figure_get_segment_data_size(type);
    unsigned int segment_flags = geometry->u.path.segment_flags;
    size_t segment_size, size;
    struct d2d_segment *segment;

    /* Guard the size math against overflow: on the 32-bit (i386 PE) build
     * size_t is 32-bit, so a pathological count could wrap to an undersized
     * allocation and OOB on replay. */
    if (count > (SIZE_MAX - sizeof(struct d2d_segment)) / element_size)
        return false;
    segment_size = element_size * count;
    size = segment_size + sizeof(struct d2d_segment);

    if (!d2d_array_reserve((void **)&figure->segments.data, &figure->segments.capacity,
            figure->segments.size + size, 1))
    {
        return false;
    }
    figure->segments.current = figure->segments.size;
    figure->segments.size += size;
    ++figure->segments.count;

    segment = d2d_figure_get_current_segment(figure);
    segment->type = type;
    segment->flags = segment_flags;
    segment->count = count;
    memcpy(segment + 1, data, segment_size);

    return true;
}

static bool d2d_figure_append_segment_data(struct d2d_geometry *geometry,
        enum d2d_segment_type type, const void *data, unsigned int count)
{
    struct d2d_figure *figure = &geometry->u.path.figures[geometry->u.path.figure_count - 1];
    size_t element_size = d2d_figure_get_segment_data_size(type);
    size_t size;
    struct d2d_segment *segment;

    /* Same overflow guard as d2d_figure_new_segment (32-bit PE size_t wrap). */
    if (count > SIZE_MAX / element_size)
        return false;
    size = element_size * count;

    if (!d2d_array_reserve((void **)&figure->segments.data, &figure->segments.capacity,
            figure->segments.size + size, 1))
    {
        return false;
    }
    segment = d2d_figure_get_current_segment(figure);

    memcpy(figure->segments.data + figure->segments.size, data, size);
    figure->segments.size += size;
    segment->count += count;

    return true;
}

static bool d2d_figure_add_segment_data(struct d2d_geometry *geometry,
        enum d2d_segment_type type, const void *data, unsigned int count)
{
    struct d2d_figure *figure = &geometry->u.path.figures[geometry->u.path.figure_count - 1];
    struct d2d_segment *current = d2d_figure_get_current_segment(figure);
    unsigned int segment_flags = geometry->u.path.segment_flags;

    /* Nothing to add: a zero count would reach a memcpy() with size 0 and a
     * possibly-NULL source (e.g. AddLines(NULL, 0)), which is C UB even for
     * n == 0. */
    if (!count)
        return true;

    if (!current || current->flags != segment_flags || current->type != type)
        return d2d_figure_new_segment(geometry, type, data, count);

    return d2d_figure_append_segment_data(geometry, type, data, count);
}

static bool d2d_figure_add_bezier_segments(struct d2d_geometry *geometry,
        const D2D1_BEZIER_SEGMENT *segments, UINT32 count)
{
    return d2d_figure_add_segment_data(geometry, D2D_SEGMENT_TYPE_BEZIERS, segments, count);
}

static bool d2d_figure_add_quadratic_bezier_segments(struct d2d_geometry *geometry,
        const D2D1_QUADRATIC_BEZIER_SEGMENT *segments, UINT32 count)
{
    return d2d_figure_add_segment_data(geometry, D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS, segments, count);
}

static bool d2d_figure_add_line_segments(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *points, UINT32 count)
{
    return d2d_figure_add_segment_data(geometry, D2D_SEGMENT_TYPE_LINES, points, count);
}

static bool d2d_figure_add_arc_segment(struct d2d_geometry *geometry, const D2D1_ARC_SEGMENT *arc)
{
    return d2d_figure_add_segment_data(geometry, D2D_SEGMENT_TYPE_ARCS, arc, 1);
}

struct d2d_cdt_edge_ref
{
    size_t idx;
    enum d2d_cdt_edge_next r;
};

struct d2d_cdt_edge
{
    struct d2d_cdt_edge_ref next[4];
    size_t vertex[2];
    unsigned int flags;
};

struct d2d_cdt
{
    struct d2d_cdt_edge *edges;
    size_t edges_size;
    size_t edge_count;
    size_t free_edge;

    const D2D1_POINT_2F *vertices;
};

struct d2d_geometry_intersection
{
    size_t figure_idx;
    size_t vertex_idx;
    size_t control_idx;
    float t;
    D2D1_POINT_2F p;
};

struct d2d_geometry_intersections
{
    struct d2d_geometry_intersection *intersections;
    size_t intersections_size;
    size_t intersection_count;
};

struct d2d_fp_two_vec2
{
    float x[2];
    float y[2];
};

struct d2d_fp_fin
{
    float *now, *other;
    size_t length;
};

static void d2d_curve_vertex_set(struct d2d_curve_vertex *b,
        const D2D1_POINT_2F *p, float u, float v, float sign)
{
    b->position = *p;
    b->texcoord.u = u;
    b->texcoord.v = v;
    b->texcoord.sign = sign;
}

static void d2d_face_set(struct d2d_face *f, UINT16 v0, UINT16 v1, UINT16 v2)
{
    f->v[0] = v0;
    f->v[1] = v1;
    f->v[2] = v2;
}

static void d2d_outline_vertex_set(struct d2d_outline_vertex *v, float x, float y,
        float prev_x, float prev_y, float next_x, float next_y)
{
    d2d_point_set(&v->position, x, y);
    d2d_point_set(&v->prev, prev_x, prev_y);
    d2d_point_set(&v->next, next_x, next_y);
}

static void d2d_curve_outline_vertex_set(struct d2d_curve_outline_vertex *a, const D2D1_POINT_2F *position,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2,
        float prev_x, float prev_y, float next_x, float next_y)
{
    a->position = *position;
    a->p0 = *p0;
    a->p1 = *p1;
    a->p2 = *p2;
    d2d_point_set(&a->prev, prev_x, prev_y);
    d2d_point_set(&a->next, next_x, next_y);
}

static void d2d_fp_two_sum(float *out, float a, float b)
{
    float a_virt, a_round, b_virt, b_round;

    out[1] = a + b;
    b_virt = out[1] - a;
    a_virt = out[1] - b_virt;
    b_round = b - b_virt;
    a_round = a - a_virt;
    out[0] = a_round + b_round;
}

static void d2d_fp_fast_two_sum(float *out, float a, float b)
{
    float b_virt;

    out[1] = a + b;
    b_virt = out[1] - a;
    out[0] = b - b_virt;
}

static void d2d_fp_two_two_sum(float *out, const float *a, const float *b)
{
    float sum[2];

    d2d_fp_two_sum(out, a[0], b[0]);
    d2d_fp_two_sum(sum, a[1], out[1]);
    d2d_fp_two_sum(&out[1], sum[0], b[1]);
    d2d_fp_two_sum(&out[2], sum[1], out[2]);
}

static void d2d_fp_two_diff_tail(float *out, float a, float b, float x)
{
    float a_virt, a_round, b_virt, b_round;

    b_virt = a - x;
    a_virt = x + b_virt;
    b_round = b_virt - b;
    a_round = a - a_virt;
    *out = a_round + b_round;
}

static void d2d_fp_two_two_diff(float *out, const float *a, const float *b)
{
    float sum[2], diff;

    diff = a[0] - b[0];
    d2d_fp_two_diff_tail(out, a[0], b[0], diff);
    d2d_fp_two_sum(sum, a[1], diff);
    diff = sum[0] - b[1];
    d2d_fp_two_diff_tail(&out[1], sum[0], b[1], diff);
    d2d_fp_two_sum(&out[2], sum[1], diff);
}

static void d2d_fp_split(float *out, float a)
{
    float a_big, c;

    c = a * ((1 << (FLT_MANT_DIG / 2)) + 1.0f);
    a_big = c - a;
    out[1] = c - a_big;
    out[0] = a - out[1];
}

static void d2d_fp_two_product_presplit(float *out, float a, float b, const float *b_split)
{
    float a_split[2], err1, err2, err3;

    out[1] = a * b;
    d2d_fp_split(a_split, a);
    err1 = out[1] - (a_split[1] * b_split[1]);
    err2 = err1 - (a_split[0] * b_split[1]);
    err3 = err2 - (a_split[1] * b_split[0]);
    out[0] = (a_split[0] * b_split[0]) - err3;
}

static void d2d_fp_two_product(float *out, float a, float b)
{
    float b_split[2];

    d2d_fp_split(b_split, b);
    d2d_fp_two_product_presplit(out, a, b, b_split);
}

static void d2d_fp_square(float *out, float a)
{
    float a_split[2], err1, err2;

    out[1] = a * a;
    d2d_fp_split(a_split, a);
    err1 = out[1] - (a_split[1] * a_split[1]);
    err2 = err1 - ((a_split[1] + a_split[1]) * a_split[0]);
    out[0] = (a_split[0] * a_split[0]) - err2;
}

static float d2d_fp_estimate(float *a, size_t len)
{
    float out = a[0];
    size_t idx = 1;

    while (idx < len)
        out += a[idx++];

    return out;
}

static void d2d_fp_fast_expansion_sum_zeroelim(float *out, size_t *out_len,
        const float *a, size_t a_len, const float *b, size_t b_len)
{
    float sum[2], q;
    size_t a_idx, b_idx, out_idx;

    a_idx = b_idx = 0;
    if ((b[b_idx] > a[a_idx]) == (b[b_idx] > -a[a_idx]))
        q = a[a_idx++];
    else
        q = b[b_idx++];
    out_idx = 0;
    if (a_idx < a_len && b_idx < b_len)
    {
        if ((b[b_idx] > a[a_idx]) == (b[b_idx] > -a[a_idx]))
            d2d_fp_fast_two_sum(sum, a[a_idx++], q);
        else
            d2d_fp_fast_two_sum(sum, b[b_idx++], q);
        if (sum[0] != 0.0f)
            out[out_idx++] = sum[0];
        q = sum[1];
        while (a_idx < a_len && b_idx < b_len)
        {
            if ((b[b_idx] > a[a_idx]) == (b[b_idx] > -a[a_idx]))
                d2d_fp_two_sum(sum, q, a[a_idx++]);
            else
                d2d_fp_two_sum(sum, q, b[b_idx++]);
            if (sum[0] != 0.0f)
                out[out_idx++] = sum[0];
            q = sum[1];
        }
    }
    while (a_idx < a_len)
    {
        d2d_fp_two_sum(sum, q, a[a_idx++]);
        if (sum[0] != 0.0f)
            out[out_idx++] = sum[0];
        q = sum[1];
    }
    while (b_idx < b_len)
    {
        d2d_fp_two_sum(sum, q, b[b_idx++]);
        if (sum[0] != 0.0f)
            out[out_idx++] = sum[0];
        q = sum[1];
    }
    if (q != 0.0f || !out_idx)
        out[out_idx++] = q;

    *out_len = out_idx;
}

static void d2d_fp_scale_expansion_zeroelim(float *out, size_t *out_len, const float *a, size_t a_len, float b)
{
    float product[2], sum[2], b_split[2], q[2], a_curr;
    size_t a_idx, out_idx;

    d2d_fp_split(b_split, b);
    d2d_fp_two_product_presplit(q, a[0], b, b_split);
    out_idx = 0;
    if (q[0] != 0.0f)
        out[out_idx++] = q[0];
    for (a_idx = 1; a_idx < a_len; ++a_idx)
    {
        a_curr = a[a_idx];
        d2d_fp_two_product_presplit(product, a_curr, b, b_split);
        d2d_fp_two_sum(sum, q[1], product[0]);
        if (sum[0] != 0.0f)
            out[out_idx++] = sum[0];
        d2d_fp_fast_two_sum(q, product[1], sum[1]);
        if (q[0] != 0.0f)
            out[out_idx++] = q[0];
    }
    if (q[1] != 0.0f || !out_idx)
        out[out_idx++] = q[1];

    *out_len = out_idx;
}

static void d2d_point_subtract(D2D1_POINT_2F *out,
        const D2D1_POINT_2F *a, const D2D1_POINT_2F *b)
{
    out->x = a->x - b->x;
    out->y = a->y - b->y;
}

static void d2d_point_scale(D2D1_POINT_2F *p, float scale)
{
    p->x *= scale;
    p->y *= scale;
}

static void d2d_point_lerp(D2D1_POINT_2F *out,
        const D2D1_POINT_2F *a, const D2D1_POINT_2F *b, float t)
{
    out->x = a->x * (1.0f - t) + b->x * t;
    out->y = a->y * (1.0f - t) + b->y * t;
}

static void d2d_point_calculate_bezier(D2D1_POINT_2F *out, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2, float t)
{
    float t_c = 1.0f - t;

    out->x = t_c * (t_c * p0->x + t * p1->x) + t * (t_c * p1->x + t * p2->x);
    out->y = t_c * (t_c * p0->y + t * p1->y) + t * (t_c * p1->y + t * p2->y);
}

static float d2d_point_length(const D2D1_POINT_2F *p)
{
    return sqrtf(d2d_point_dot(p, p));
}

static void d2d_point_normalise(D2D1_POINT_2F *p)
{
    float l;

    if ((l = d2d_point_length(p)) != 0.0f)
        d2d_point_scale(p, 1.0f / l);
}

static BOOL d2d_vertex_type_is_bezier(enum d2d_vertex_type t)
{
    return (t == D2D_VERTEX_TYPE_BEZIER || t == D2D_VERTEX_TYPE_SPLIT_BEZIER);
}

static BOOL d2d_vertex_type_is_split_bezier(enum d2d_vertex_type t)
{
    return t == D2D_VERTEX_TYPE_SPLIT_BEZIER;
}

/* This implementation is based on the paper "Adaptive Precision
 * Floating-Point Arithmetic and Fast Robust Geometric Predicates" and
 * associated (Public Domain) code by Jonathan Richard Shewchuk. */
static float d2d_point_ccw(const D2D1_POINT_2F *a, const D2D1_POINT_2F *b, const D2D1_POINT_2F *c)
{
    static const float err_bound_result = (3.0f + 8.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_a = (3.0f + 16.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_b = (2.0f + 12.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_c = (9.0f + 64.0f * D2D_FP_EPS) * D2D_FP_EPS * D2D_FP_EPS;
    float det_d[16], det_c2[12], det_c1[8], det_b[4], temp4[4], temp2a[2], temp2b[2], abxacy[2], abyacx[2];
    size_t det_d_len, det_c2_len, det_c1_len;
    float det, det_sum, err_bound;
    struct d2d_fp_two_vec2 ab, ac;

    ab.x[1] = b->x - a->x;
    ab.y[1] = b->y - a->y;
    ac.x[1] = c->x - a->x;
    ac.y[1] = c->y - a->y;

    abxacy[1] = ab.x[1] * ac.y[1];
    abyacx[1] = ab.y[1] * ac.x[1];
    det = abxacy[1] - abyacx[1];

    if (abxacy[1] > 0.0f)
    {
        if (abyacx[1] <= 0.0f)
            return det;
        det_sum = abxacy[1] + abyacx[1];
    }
    else if (abxacy[1] < 0.0f)
    {
        if (abyacx[1] >= 0.0f)
            return det;
        det_sum = -abxacy[1] - abyacx[1];
    }
    else
    {
        return det;
    }

    err_bound = err_bound_a * det_sum;
    if (det >= err_bound || -det >= err_bound)
        return det;

    d2d_fp_two_product(abxacy, ab.x[1], ac.y[1]);
    d2d_fp_two_product(abyacx, ab.y[1], ac.x[1]);
    d2d_fp_two_two_diff(det_b, abxacy, abyacx);

    det = d2d_fp_estimate(det_b, 4);
    err_bound = err_bound_b * det_sum;
    if (det >= err_bound || -det >= err_bound)
        return det;

    d2d_fp_two_diff_tail(&ab.x[0], b->x, a->x, ab.x[1]);
    d2d_fp_two_diff_tail(&ab.y[0], b->y, a->y, ab.y[1]);
    d2d_fp_two_diff_tail(&ac.x[0], c->x, a->x, ac.x[1]);
    d2d_fp_two_diff_tail(&ac.y[0], c->y, a->y, ac.y[1]);

    if (ab.x[0] == 0.0f && ab.y[0] == 0.0f && ac.x[0] == 0.0f && ac.y[0] == 0.0f)
        return det;

    err_bound = err_bound_c * det_sum + err_bound_result * fabsf(det);
    det += (ab.x[1] * ac.y[0] + ac.y[1] * ab.x[0]) - (ab.y[1] * ac.x[0] + ac.x[1] * ab.y[0]);
    if (det >= err_bound || -det >= err_bound)
        return det;

    d2d_fp_two_product(temp2a, ab.x[0], ac.y[1]);
    d2d_fp_two_product(temp2b, ab.y[0], ac.x[1]);
    d2d_fp_two_two_diff(temp4, temp2a, temp2b);
    d2d_fp_fast_expansion_sum_zeroelim(det_c1, &det_c1_len, det_b, 4, temp4, 4);

    d2d_fp_two_product(temp2a, ab.x[1], ac.y[0]);
    d2d_fp_two_product(temp2b, ab.y[1], ac.x[0]);
    d2d_fp_two_two_diff(temp4, temp2a, temp2b);
    d2d_fp_fast_expansion_sum_zeroelim(det_c2, &det_c2_len, det_c1, det_c1_len, temp4, 4);

    d2d_fp_two_product(temp2a, ab.x[0], ac.y[0]);
    d2d_fp_two_product(temp2b, ab.y[0], ac.x[0]);
    d2d_fp_two_two_diff(temp4, temp2a, temp2b);
    d2d_fp_fast_expansion_sum_zeroelim(det_d, &det_d_len, det_c2, det_c2_len, temp4, 4);

    return det_d[det_d_len - 1];
}

/* Determine whether the point q is within the given tolerance of the line
 * segment defined by p0 and p1, with the given stroke width and transform.
 * Note that we don't care about the tolerance with respect to end-points or
 * joins here; those are handled separately. */
static BOOL d2d_point_on_line_segment(const D2D1_POINT_2F *q, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, const D2D1_MATRIX_3X2_F *transform, float stroke_width, float tolerance)
{
    D2D1_POINT_2F v_n, v_p, v_q, v_r;
    float l;

    d2d_point_subtract(&v_p, p1, p0);
    if ((l = d2d_point_length(&v_p)) == 0.0f)
        return FALSE;

    /* After (shear) transformation, the line segment is a parallelogram
     * defined by p⃑' and n⃑':
     *
     *   p⃑ = P₁ - P₀
     *   n⃑ = wp̂⟂
     *   p⃑' = p⃑T
     *   n⃑' = n⃑T */
    l = stroke_width / l;
    d2d_point_set(&v_r, transform->_31, transform->_32);
    d2d_point_transform(&v_n, transform, -v_p.y * l, v_p.x * l);
    d2d_point_subtract(&v_n, &v_n, &v_r);
    d2d_point_transform(&v_p, transform, v_p.x, v_p.y);
    d2d_point_subtract(&v_p, &v_p, &v_r);

    /* Decompose the vector q⃑ = Q - P₀T into a linear combination of
     * p⃑' and n⃑':
     *
     *   lq⃑ = xp⃑' + yn⃑' */
    d2d_point_transform(&v_q, transform, p0->x, p0->y);
    d2d_point_subtract(&v_q, q, &v_q);
    l = v_p.x * v_n.y - v_p.y * v_n.x;
    v_r.x = v_q.x * v_n.y - v_q.y * v_n.x;
    v_r.y = v_q.x * v_p.y - v_q.y * v_p.x;

    if (l < 0.0f)
    {
        l *= -1.0f;
        v_r.x *= -1.0f;
    }

    /* Check where Q projects onto p⃑'. */
    if (v_r.x < 0.0f || v_r.x > l)
        return FALSE;

    /* Check where Q projects onto n⃑'. */
    if (fabs(v_r.y) < l)
        return TRUE;

    /* Q lies outside the segment. Check whether the distance to the edge is
     * within the tolerance.
     *
     *   P₀' = P₀T + n⃑'
     *   q⃑' = Q - P₀'
     *      = q⃑ - n⃑'
     *
     * The distance is then q⃑' · p̂'⟂. */

    if (v_r.y > 0.0f)
        d2d_point_scale(&v_n, -1.0f);
    d2d_point_subtract(&v_q, &v_q, &v_n);

    /* Check where Q projects onto p⃑' + n⃑'. */
    l = d2d_point_dot(&v_q, &v_p);
    if (l < 0.0f || l > d2d_point_dot(&v_p, &v_p))
        return FALSE;

    v_n.x = -v_p.y;
    v_n.y = v_p.x;
    d2d_point_normalise(&v_n);

    return fabsf(d2d_point_dot(&v_q, &v_n)) < tolerance;
}

/* Approximate the Bézier segment with a (wide) line segment. If the point
 * lies outside the approximation, we're done. If the width of the
 * approximation is less than the tolerance and the point lies inside, we're
 * also done. If neither of those is the case, we subdivide the Bézier segment
 * and try again. */
static BOOL d2d_point_on_bezier_segment(const D2D1_POINT_2F *q, const D2D1_POINT_2F *p0,
        const D2D1_BEZIER_SEGMENT *b, const D2D1_MATRIX_3X2_F *transform, float stroke_width, float tolerance)
{
    float d1, d2, d3, d4, d, l, m, w, w2;
    D2D1_POINT_2F t[7], start, end, v_p;
    D2D1_BEZIER_SEGMENT b0, b1;

    m = 1.0f;
    w = stroke_width * 0.5f;

    d2d_point_subtract(&v_p, &b->point3, p0);
    /* If the endpoints coincide, use the line through the control points as
     * the direction vector. That choice is somewhat arbitrary; other choices
     * with tighter error bounds exist. */
    if ((l = d2d_point_dot(&v_p, &v_p)) == 0.0f)
    {
        d2d_point_subtract(&v_p, &b->point2, &b->point1);
        /* If the control points also coincide, the curve is in fact a line. */
        if ((l = d2d_point_dot(&v_p, &v_p)) == 0.0f)
        {
            d2d_point_subtract(&v_p, &b->point1, p0);
            end.x = p0->x + 0.75f * v_p.x;
            end.y = p0->y + 0.75f * v_p.y;

            return d2d_point_on_line_segment(q, p0, &end, transform, w, tolerance);
        }
        m = 0.0f;
    }
    l = sqrtf(l);
    d2d_point_scale(&v_p, 1.0f / l);
    m *= l;

    /* Calculate the width w2 of the approximation. */

    end.x = p0->x + v_p.x;
    end.y = p0->y + v_p.y;
    /* Here, d1 and d2 are the maximum (signed) distance of the control points
     * from the line through the start and end points. */
    d1 = d2d_point_ccw(p0, &end, &b->point1);
    d2 = d2d_point_ccw(p0, &end, &b->point2);
    /* It can be shown that if the control points of a cubic Bézier curve lie
     * on the same side of the line through the endpoints, the distance of the
     * curve itself to that line will be within 3/4 of the distance of the
     * control points to that line; if the control points lie on opposite
     * sides, that distance will be within 4/9 of the distance of the
     * corresponding control point. We're taking that as a given here. */
    if (d1 * d2 > 0.0f)
    {
        d1 *= 0.75f;
        d2 *= 0.75f;
    }
    else
    {
        d1 = (d1 * 4.0f) / 9.0f;
        d2 = (d2 * 4.0f) / 9.0f;
    }
    w2 = max(fabsf(d1), fabsf(d2));

    /* Project the control points onto the line through the endpoints of the
     * curve. We will use these to calculate the endpoints of the
     * approximation. */
    d2d_point_subtract(&t[1], &b->point1, p0);
    d1 = d2d_point_dot(&v_p, &t[1]);
    d2d_point_subtract(&t[2], &b->point2, p0);
    d2 = d2d_point_dot(&v_p, &t[2]);

    /* Calculate the start point of the approximation. Like further above, the
     * actual curve is somewhat closer to the endpoints than the control
     * points are. */
    d = min(min(d1, d2), 0);
    if (d1 * d2 > 0.0f)
        d *= 0.75f;
    else
        d = (d * 4.0f) / 9.0f;
    /* Account for the stroke width and tolerance around the endpoints by
     * adjusting the endpoints here. This matters because there are no joins
     * in the original geometry for the places where we subdivide the original
     * curve. We do this here because it's easy; alternatively we could
     * explicitly test for this when subdividing the curve further below. */
    d -= min(w + tolerance, w2);
    start.x = p0->x + d * v_p.x;
    start.y = p0->y + d * v_p.y;

    /* Calculate the end point of the approximation. */
    d1 -= m;
    d2 -= m;
    d = max(max(d1, d2), 0);
    if (d1 * d2 > 0.0f)
        d = m + d * 0.75f;
    else
        d = m + (d * 4.0f) / 9.0f;
    d += min(w2, w + tolerance);
    end.x = p0->x + d * v_p.x;
    end.y = p0->y + d * v_p.y;

    /* Calculate the error bounds of the approximation. We do this in
     * transformed space because we need these to be relative to the given
     * tolerance. */

    d2d_point_transform(&t[0], transform, p0->x, p0->y);
    d2d_point_transform(&t[1], transform, b->point1.x, b->point1.y);
    d2d_point_transform(&t[2], transform, b->point2.x, b->point2.y);
    d2d_point_transform(&t[3], transform, b->point3.x, b->point3.y);
    d2d_point_transform(&t[4], transform, start.x, start.y);
    d2d_point_transform(&t[5], transform, end.x, end.y);

    d2d_point_subtract(&t[6], &t[5], &t[4]);
    l = d2d_point_length(&t[6]);
    /* Here, d1 and d2 are the maximum (signed) distance of the control points
     * from the line through the start and end points. */
    d1 = d2d_point_ccw(&t[4], &t[5], &t[1]) / l;
    d2 = d2d_point_ccw(&t[4], &t[5], &t[2]) / l;
    if (d1 * d2 > 0.0f)
    {
        d1 *= 0.75f;
        d2 *= 0.75f;
    }
    else
    {
        d1 = (d1 * 4.0f) / 9.0f;
        d2 = (d2 * 4.0f) / 9.0f;
    }
    l = max(max(d1, d2), 0) - min(min(d1, d2), 0);

    /* d3 and d4 are the (unsigned) distance of the endpoints of the
     * approximation from the original endpoints. */
    d2d_point_subtract(&t[6], &t[4], &t[0]);
    d3 = d2d_point_length(&t[6]);
    d2d_point_subtract(&t[6], &t[5], &t[3]);
    d4 = d2d_point_length(&t[6]);
    l = max(max(d3, d4), l);

    /* If the error of the approximation is less than the tolerance, and Q
     * lies on the approximation, the distance of Q to the stroked curve is
     * definitely within the tolerance. */
    if (l <= tolerance && d2d_point_on_line_segment(q, &start, &end, transform, w, tolerance - l))
        return TRUE;
    /* On the other hand, if the distance of Q to the stroked curve is more
     * than the sum of the tolerance and d, the distance of Q to the stroked
     * curve can't possibly be within the tolerance. */
    if (!d2d_point_on_line_segment(q, &start, &end, transform, w + w2, tolerance))
        return FALSE;

    /* Subdivide the curve. Note that simply splitting the segment in half
     * here works and is easy, but may not be optimal. We could potentially
     * reduce the number of iterations we need to do by splitting based on
     * curvature or segment length. */
    d2d_point_lerp(&t[0], &b->point1, &b->point2, 0.5f);

    b1.point3 = b->point3;
    d2d_point_lerp(&b1.point2, &b->point3, &b->point2, 0.5f);
    d2d_point_lerp(&b1.point1, &t[0], &b1.point2, 0.5f);

    d2d_point_lerp(&b0.point1, p0, &b->point1, 0.5f);
    d2d_point_lerp(&b0.point2, &t[0], &b0.point1, 0.5f);
    d2d_point_lerp(&b0.point3, &b0.point2, &b1.point1, 0.5f);

    return d2d_point_on_bezier_segment(q, p0, &b0, transform, stroke_width, tolerance)
            || d2d_point_on_bezier_segment(q, &b0.point3, &b1, transform, stroke_width, tolerance);
}

static void d2d_rect_union(D2D1_RECT_F *l, const D2D1_RECT_F *r)
{
    l->left   = min(l->left, r->left);
    l->top    = min(l->top, r->top);
    l->right  = max(l->right, r->right);
    l->bottom = max(l->bottom, r->bottom);
}

static BOOL d2d_rect_check_overlap(const D2D_RECT_F *p, const D2D_RECT_F *q)
{
    return p->left < q->right && p->top < q->bottom && p->right > q->left && p->bottom > q->top;
}

static void d2d_rect_get_bezier_bounds(D2D_RECT_F *bounds, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2)
{
    D2D1_POINT_2F p;
    float root;

    bounds->left = p0->x;
    bounds->top = p0->y;
    bounds->right = p0->x;
    bounds->bottom = p0->y;

    d2d_rect_expand(bounds, p2);

    /* f(t) = (1 - t)²P₀ + 2(1 - t)tP₁ + t²P₂
     * f'(t) = 2(1 - t)(P₁ - P₀) + 2t(P₂ - P₁)
     *       = 2(P₂ - 2P₁ + P₀)t + 2(P₁ - P₀)
     *
     * f'(t) = 0
     * t = (P₀ - P₁) / (P₂ - 2P₁ + P₀) */
    root = (p0->x - p1->x) / (p2->x - 2.0f * p1->x + p0->x);
    if (root > 0.0f && root < 1.0f)
    {
        d2d_point_calculate_bezier(&p, p0, p1, p2, root);
        d2d_rect_expand(bounds, &p);
    }

    root = (p0->y - p1->y) / (p2->y - 2.0f * p1->y + p0->y);
    if (root > 0.0f && root < 1.0f)
    {
        d2d_point_calculate_bezier(&p, p0, p1, p2, root);
        d2d_rect_expand(bounds, &p);
    }
}

static void d2d_rect_get_bezier_segment_bounds(D2D_RECT_F *bounds, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2, float start, float end)
{
    D2D1_POINT_2F q[3], r[2];

    d2d_point_lerp(&r[0], p0, p1, start);
    d2d_point_lerp(&r[1], p1, p2, start);
    d2d_point_lerp(&q[0], &r[0], &r[1], start);

    end = (end - start) / (1.0f - start);
    d2d_point_lerp(&q[1], &q[0], &r[1], end);
    d2d_point_lerp(&r[0], &r[1], p2, end);
    d2d_point_lerp(&q[2], &q[1], &r[0], end);

    d2d_rect_get_bezier_bounds(bounds, &q[0], &q[1], &q[2]);
}

static BOOL d2d_figure_insert_vertex(struct d2d_figure *figure, size_t idx, D2D1_POINT_2F vertex)
{
    if (!d2d_array_reserve((void **)&figure->vertices, &figure->vertices_size,
            figure->vertex_count + 1, sizeof(*figure->vertices)))
    {
        ERR("Failed to grow vertices array.\n");
        return FALSE;
    }

    if (!d2d_array_reserve((void **)&figure->vertex_types, &figure->vertex_types_size,
            figure->vertex_count + 1, sizeof(*figure->vertex_types)))
    {
        ERR("Failed to grow vertex types array.\n");
        return FALSE;
    }

    memmove(&figure->vertices[idx + 1], &figure->vertices[idx],
            (figure->vertex_count - idx) * sizeof(*figure->vertices));
    memmove(&figure->vertex_types[idx + 1], &figure->vertex_types[idx],
            (figure->vertex_count - idx) * sizeof(*figure->vertex_types));
    figure->vertices[idx] = vertex;
    figure->vertex_types[idx] = D2D_VERTEX_TYPE_NONE;
    d2d_rect_expand(&figure->bounds, &vertex);
    ++figure->vertex_count;
    return TRUE;
}

static bool d2d_figure_add_vertex(struct d2d_figure *figure, D2D1_POINT_2F vertex)
{
    size_t last = figure->vertex_count - 1;

    if (figure->vertex_count && figure->vertex_types[last] == D2D_VERTEX_TYPE_LINE
            && !memcmp(&figure->vertices[last], &vertex, sizeof(vertex)))
        return true;

    if (!d2d_array_reserve((void **)&figure->vertices, &figure->vertices_size,
            figure->vertex_count + 1, sizeof(*figure->vertices)))
    {
        ERR("Failed to grow vertices array.\n");
        return false;
    }

    if (!d2d_array_reserve((void **)&figure->vertex_types, &figure->vertex_types_size,
            figure->vertex_count + 1, sizeof(*figure->vertex_types)))
    {
        ERR("Failed to grow vertex types array.\n");
        return false;
    }

    figure->vertices[figure->vertex_count] = vertex;
    figure->vertex_types[figure->vertex_count] = D2D_VERTEX_TYPE_NONE;
    d2d_rect_expand(&figure->bounds, &vertex);
    ++figure->vertex_count;
    return true;
}

static BOOL d2d_figure_insert_bezier_controls(struct d2d_figure *figure,
        size_t idx, size_t count, const D2D1_POINT_2F *p)
{
    if (!d2d_array_reserve((void **)&figure->bezier_controls, &figure->bezier_controls_size,
            figure->bezier_control_count + count, sizeof(*figure->bezier_controls)))
    {
        ERR("Failed to grow bezier controls array.\n");
        return FALSE;
    }

    memmove(&figure->bezier_controls[idx + count], &figure->bezier_controls[idx],
            (figure->bezier_control_count - idx) * sizeof(*figure->bezier_controls));
    memcpy(&figure->bezier_controls[idx], p, count * sizeof(*figure->bezier_controls));
    figure->bezier_control_count += count;

    return TRUE;
}

static BOOL d2d_figure_add_bezier_controls(struct d2d_figure *figure, size_t count, const D2D1_POINT_2F *p)
{
    if (!d2d_array_reserve((void **)&figure->bezier_controls, &figure->bezier_controls_size,
            figure->bezier_control_count + count, sizeof(*figure->bezier_controls)))
    {
        ERR("Failed to grow bezier controls array.\n");
        return FALSE;
    }

    memcpy(&figure->bezier_controls[figure->bezier_control_count], p, count * sizeof(*figure->bezier_controls));
    figure->bezier_control_count += count;

    return TRUE;
}

static BOOL d2d_figure_add_original_bezier_controls(struct d2d_figure *figure, size_t count, const D2D1_POINT_2F *p)
{
    if (!d2d_array_reserve((void **)&figure->original_bezier_controls, &figure->original_bezier_controls_size,
            figure->original_bezier_control_count + count, sizeof(*figure->original_bezier_controls)))
    {
        ERR("Failed to grow cubic Bézier controls array.\n");
        return FALSE;
    }

    memcpy(&figure->original_bezier_controls[figure->original_bezier_control_count],
            p, count * sizeof(*figure->original_bezier_controls));
    figure->original_bezier_control_count += count;

    return TRUE;
}

/* Reducing a cubic Bézier to the quadratic that shares its end points places the
 * quadratic control at (3 * (p1 + p2) - (p0 + p3)) / 4. The deviation that leaves
 * is exactly |p3 - 3 * p2 + 3 * p1 - p0| / (12 * sqrt(3)) - the third difference of
 * the control points times a constant - and it is signed, so a quadratic cannot
 * follow a cubic through an inflection at all: an S shaped curve reduces to an arc.
 *
 * Subdividing the cubic first and reducing each piece separately fixes both, because
 * de Casteljau subdivision into n pieces scales the third difference by 1 / n^3.
 * That cube root is what keeps the piece count small even for very large curves. */
#define D2D_BEZIER_REDUCTION_DEVIATION 0.0481125f
/* The quadratics are rendered as curves and the world transform is applied after
 * this, so the error budget here has to be tighter than a flattening tolerance
 * would be; a tenth of it costs only cbrt(10) times as many pieces. */
#define D2D_BEZIER_REDUCTION_TOLERANCE D2D1_DEFAULT_FLATTENING_TOLERANCE
/* Bounds the pathological cases; a curve big enough to need this is already far
 * outside anything the rest of the pipeline handles gracefully. */
#define D2D_BEZIER_MAX_QUADRATICS 32

static void d2d_cubic_bezier_reduce(D2D1_POINT_2F *control, const D2D1_POINT_2F *p)
{
    control->x = (p[1].x + p[2].x) * 0.75f - (p[0].x + p[3].x) * 0.25f;
    control->y = (p[1].y + p[2].y) * 0.75f - (p[0].y + p[3].y) * 0.25f;
}

/* Split a cubic Bézier at t into the piece before t and the piece after it. Either
 * output may alias the input, which is what lets a caller peel pieces off the front
 * of a curve in place. */
static void d2d_cubic_bezier_split(D2D1_POINT_2F *before, D2D1_POINT_2F *after,
        const D2D1_POINT_2F *p, float t)
{
    D2D1_POINT_2F p0 = p[0], p3 = p[3], a, b, c, d, e, f;

    d2d_point_lerp(&a, &p[0], &p[1], t);
    d2d_point_lerp(&b, &p[1], &p[2], t);
    d2d_point_lerp(&c, &p[2], &p[3], t);
    d2d_point_lerp(&d, &a, &b, t);
    d2d_point_lerp(&e, &b, &c, t);
    d2d_point_lerp(&f, &d, &e, t);

    before[0] = p0; before[1] = a; before[2] = d; before[3] = f;
    after[0] = f; after[1] = e; after[2] = c; after[3] = p3;
}

static unsigned int d2d_cubic_bezier_quadratic_count(const D2D1_POINT_2F *p)
{
    float deviation, count;
    D2D1_POINT_2F e;

    e.x = p[3].x - 3.0f * p[2].x + 3.0f * p[1].x - p[0].x;
    e.y = p[3].y - 3.0f * p[2].y + 3.0f * p[1].y - p[0].y;
    deviation = sqrtf(e.x * e.x + e.y * e.y) * D2D_BEZIER_REDUCTION_DEVIATION;

    /* Negated so that a deviation that is not a number ends up with one piece. */
    if (!(deviation > D2D_BEZIER_REDUCTION_TOLERANCE))
        return 1;

    count = ceilf(powf(deviation / D2D_BEZIER_REDUCTION_TOLERANCE, 1.0f / 3.0f));
    if (count >= (float)D2D_BEZIER_MAX_QUADRATICS)
        return D2D_BEZIER_MAX_QUADRATICS;

    return count;
}

struct d2d_quadratic_bezier
{
    D2D1_POINT_2F p0, control, p2;
};

/* Express a cubic Bézier as a chain of quadratics, subdividing it as far as the
 * tolerance requires. Returns the number written; the caller provides room for
 * D2D_BEZIER_MAX_QUADRATICS of them. */
static unsigned int d2d_cubic_bezier_to_quadratics(const D2D1_POINT_2F *p,
        struct d2d_quadratic_bezier *quadratics)
{
    D2D1_POINT_2F cubic[4], piece[4];
    unsigned int count, i;

    memcpy(cubic, p, sizeof(cubic));
    count = d2d_cubic_bezier_quadratic_count(cubic);

    for (i = 0; i < count; ++i)
    {
        /* Peel the next piece off the front of what is left of the curve. The
         * last piece is the remainder, so its end point stays bit exact. */
        if (i + 1 < count)
            d2d_cubic_bezier_split(piece, cubic, cubic, 1.0f / (float)(count - i));
        else
            memcpy(piece, cubic, sizeof(piece));

        quadratics[i].p0 = piece[0];
        d2d_cubic_bezier_reduce(&quadratics[i].control, piece);
        quadratics[i].p2 = piece[3];
    }

    return count;
}

static bool d2d_figure_add_beziers(struct d2d_figure *figure, const D2D1_BEZIER_SEGMENT *beziers,
        UINT32 count)
{
    struct d2d_quadratic_bezier quadratics[D2D_BEZIER_MAX_QUADRATICS];
    unsigned int i, j, quadratic_count;
    D2D1_POINT_2F cubic[4];

    for (i = 0; i < count; ++i)
    {
        D2D1_RECT_F bezier_bounds;

        if (!d2d_figure_add_original_bezier_controls(figure, 1, &beziers[i].point1)
                || !d2d_figure_add_original_bezier_controls(figure, 1, &beziers[i].point2))
        {
            return false;
        }

        cubic[0] = figure->vertices[figure->vertex_count - 1];
        cubic[1] = beziers[i].point1;
        cubic[2] = beziers[i].point2;
        cubic[3] = beziers[i].point3;
        quadratic_count = d2d_cubic_bezier_to_quadratics(cubic, quadratics);

        for (j = 0; j < quadratic_count; ++j)
        {
            /* Only the first piece opens the segment the application added. The
             * others are marked as splits, which is the same thing the tessellator
             * does when it subdivides a quadratic - Simplify(), Widen(), the dash
             * and outline code all skip those and keep seeing one cubic per
             * segment, with its original control points. */
            figure->vertex_types[figure->vertex_count - 1] = j ? D2D_VERTEX_TYPE_SPLIT_BEZIER
                    : D2D_VERTEX_TYPE_BEZIER;

            d2d_rect_get_bezier_bounds(&bezier_bounds, &quadratics[j].p0,
                    &quadratics[j].control, &quadratics[j].p2);

            if (!d2d_figure_add_bezier_controls(figure, 1, &quadratics[j].control))
                return false;

            if (!d2d_figure_add_vertex(figure, quadratics[j].p2))
                return false;

            d2d_rect_union(&figure->bounds, &bezier_bounds);
        }
    }

    return true;
}

static bool d2d_figure_add_quadratic_beziers(struct d2d_figure *figure,
        const D2D1_QUADRATIC_BEZIER_SEGMENT *beziers, UINT32 bezier_count)
{
    unsigned int i;

    for (i = 0; i < bezier_count; ++i)
    {
        D2D1_RECT_F bezier_bounds;
        D2D1_POINT_2F p[2];

        /* Construct a cubic curve. */
        d2d_point_lerp(&p[0], &figure->vertices[figure->vertex_count - 1], &beziers[i].point1, 2.0f / 3.0f);
        d2d_point_lerp(&p[1], &beziers[i].point2, &beziers[i].point1, 2.0f / 3.0f);
        if (!d2d_figure_add_original_bezier_controls(figure, 2, p))
            return false;

        d2d_rect_get_bezier_bounds(&bezier_bounds, &figure->vertices[figure->vertex_count - 1],
                &beziers[i].point1, &beziers[i].point2);

        figure->vertex_types[figure->vertex_count - 1] = D2D_VERTEX_TYPE_BEZIER;
        if (!d2d_figure_add_bezier_controls(figure, 1, &beziers[i].point1))
            return false;

        if (!d2d_figure_add_vertex(figure, beziers[i].point2))
            return false;

        d2d_rect_union(&figure->bounds, &bezier_bounds);
    }

    return true;
}

static bool d2d_figure_add_lines(struct d2d_figure *figure, const D2D1_POINT_2F *points,
        UINT32 count)
{
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        figure->vertex_types[figure->vertex_count - 1] = D2D_VERTEX_TYPE_LINE;
        if (!d2d_figure_add_vertex(figure, points[i]))
            return false;
    }

    return true;
}

static bool d2d_arc_check_radius(float halfchord2, float fuzz2, float *radius)
{
    bool accept = !(*radius * *radius <= halfchord2 * fuzz2);
    if (accept)
    {
        if (*radius < 0.0f)
            *radius = -*radius;
    }
    return accept;
}

static int d2d_arc_get_piece_count(const D2D_POINT_2F *start, const D2D_POINT_2F *end,
        bool large_arc, bool sweep_up, float *cos_angle, float *sin_angle)
{
    float angle;
    int count;

    *cos_angle = d2d_point_dot(start, end);
    *sin_angle = start->x * end->y - start->y * end->x;

    if (*cos_angle >= 0.0f)
    {
        if (!large_arc) return 1;
        count = 4;
    }
    else
    {
        count = large_arc ? 3 : 2;
    }

    angle = atan2f(*sin_angle, *cos_angle);
    if (sweep_up)
    {
        if (angle < 0.0f)
            angle += 2.0f * M_PI;
    }
    else
    {
        if (angle > 0.0f)
            angle -= 2.0f * M_PI;
    }

    angle /= count;
    *cos_angle = cosf(angle);
    *sin_angle = sinf(angle);

    return count;
}

static float d2d_arc_get_bezier_distance(float rDot, bool sweep_up)
{
    float denom_squared, denominator;
    const float fuzz = 1.e-6;
    float numerator, rA, dist;

    rA = 0.5f * (1.0f + rDot);
    if (rA < 0.0f)
        return 0.0f;

    denom_squared = 1.0f - rA;
    if (denom_squared <= 0.0f)
        return 0.0f;

    denominator = sqrt(denom_squared);
    numerator = (4.0f / 3.0f) * (1.0f - sqrt(rA));

    dist = (numerator <= denominator * fuzz) ? 0.0f : numerator / denominator;
    if (!sweep_up)
        dist = -dist;

    return dist;
}

/* Approximation logic is taken in its entirety from WpfGfx graphics core of
   Windows Presentation Foundation framework, distributed under MIT license. */
static int d2d_arc_to_bezier(const D2D_POINT_2F *start_point, const D2D1_ARC_SEGMENT *arc,
        D2D_POINT_2F *points)
{
    float x, y, rHalfChord2, rCos, rSin, rCosArcAngle, rSinArcAngle, dist, rotation;
    D2D_POINT_2F ptStart, ptEnd, vecToBez1, vecToBez2;
    const float FUZZ = 1.e-6;
    const float PI_OVER_180 = 0.0174532925199432957692;
    bool large_arc, sweep_up, zero_center = false;
    D2D_POINT_2F center, radius;
    D2D1_MATRIX_3X2_F m;
    int cPieces = -1;

    float fuzz2 = FUZZ * FUZZ;
    int i, j;

    d2d_point_set(&radius, arc->size.width, arc->size.height);
    rotation = arc->rotationAngle;
    large_arc = arc->arcSize == D2D1_ARC_SIZE_LARGE;
    sweep_up = arc->sweepDirection == D2D1_SWEEP_DIRECTION_CLOCKWISE;

    x = 0.5f * (arc->point.x - start_point->x);
    y = 0.5f * (arc->point.y - start_point->y);

    rHalfChord2 = x * x + y * y;
    if (rHalfChord2 < fuzz2)
        return -1;

    if (!d2d_arc_check_radius(rHalfChord2, fuzz2, &radius.x) ||
        !d2d_arc_check_radius(rHalfChord2, fuzz2, &radius.y))
    {
        points[0] = arc->point;
        return 0;
    }

    if (fabs(rotation) < FUZZ)
    {
        rCos = 1.0f;
        rSin = 0.0f;
    }
    else
    {
        float tmp;

        rotation = -rotation * PI_OVER_180;

        rCos = cosf(rotation);
        rSin = sinf(rotation);

        tmp = x * rCos - y * rSin;
        y = x * rSin + y * rCos;
        x = tmp;
    }

    x /= radius.x;
    y /= radius.y;

    rHalfChord2 = x * x + y * y;
    if (rHalfChord2 > 1.0f)
    {
        float tmp = sqrtf(rHalfChord2);

        radius.x *= tmp;
        radius.y *= tmp;
        center.x = center.y = 0.0f;
        zero_center = true;

        x /= tmp;
        y /= tmp;
    }
    else
    {
        float tmp = sqrtf((1.0f - rHalfChord2) / rHalfChord2);
        if (large_arc != sweep_up)
        {
            center.x = -tmp * y;
            center.y = tmp * x;
        }
        else
        {
            center.x = tmp * y;
            center.y = -tmp * x;
        }
    }

    d2d_point_set(&ptStart, -x - center.x, -y - center.y);
    d2d_point_set(&ptEnd, x - center.x, y - center.y);

    m._11 = rCos * radius.x;
    m._12 = -rSin * radius.x;
    m._21 = rSin * radius.y;
    m._22 = rCos * radius.y;
    m._31 = 0.5f * (arc->point.x + start_point->x);
    m._32 = 0.5f * (arc->point.y + start_point->y);
    if (!zero_center)
    {
        m._31 += (m._11 * center.x + m._12 * center.y);
        m._32 += (m._21 * center.x + m._22 * center.y);
    }

    cPieces = d2d_arc_get_piece_count(&ptStart, &ptEnd, large_arc, sweep_up, &rCosArcAngle, &rSinArcAngle);

    dist = d2d_arc_get_bezier_distance(rCosArcAngle, sweep_up);
    d2d_point_set(&vecToBez1, -dist * ptStart.y, dist * ptStart.x);

    j = 0;
    for (i = 1; i < cPieces; ++i)
    {
        D2D_POINT_2F ptPieceEnd;

        d2d_point_set(&ptPieceEnd, ptStart.x * rCosArcAngle - ptStart.y * rSinArcAngle,
                              ptStart.x * rSinArcAngle + ptStart.y * rCosArcAngle);
        d2d_point_set(&vecToBez2, -dist * ptPieceEnd.y, dist * ptPieceEnd.x);

        d2d_point_transform(&points[j++], &m, ptStart.x + vecToBez1.x, ptStart.y + vecToBez1.y);
        d2d_point_transform(&points[j++], &m, ptPieceEnd.x - vecToBez2.x, ptPieceEnd.y - vecToBez2.y);
        d2d_point_transform(&points[j++], &m, ptPieceEnd.x, ptPieceEnd.y);

        ptStart = ptPieceEnd;
        vecToBez1 = vecToBez2;
    }

    d2d_point_set(&vecToBez2, -dist * ptEnd.y, dist * ptEnd.x);
    d2d_point_transform(&points[j++], &m, ptStart.x + vecToBez1.x, ptStart.y + vecToBez1.y);
    d2d_point_transform(&points[j++], &m, ptEnd.x - vecToBez2.x, ptEnd.y - vecToBez2.y);
    d2d_point_set(&points[j], arc->point.x, arc->point.y);

    return cPieces;
}

static bool d2d_figure_add_arc(struct d2d_figure *figure, const D2D1_ARC_SEGMENT *arc)
{
    size_t last = figure->vertex_count - 1;
    D2D_POINT_2F points[12];
    int count = 0;

    count = d2d_arc_to_bezier(&figure->vertices[last], arc, points);

    if (count > 0)
    {
        return d2d_figure_add_beziers(figure, (D2D1_BEZIER_SEGMENT *)points, count);
    }
    else if (count == 0)
    {
        return d2d_figure_add_lines(figure, points, 1);
    }

    return true;
}

static bool d2d_figure_produce_vertices(struct d2d_figure *figure)
{
    union
    {
        const struct d2d_segment *segment;
        const struct d2d_segment_beziers *beziers;
        const struct d2d_segment_quadratic_beziers *quad_beziers;
        const struct d2d_segment_lines *lines;
        const struct d2d_segment_arcs *arcs;
    } s = { (struct d2d_segment *)figure->segments.data };
    unsigned int i, j;
    size_t size = 0;

    for (i = 0; i < figure->segments.count; ++i)
    {
        switch (s.segment->type)
        {
            case D2D_SEGMENT_TYPE_BEZIERS:
                if (!d2d_figure_add_beziers(figure, s.beziers->segments, s.beziers->count))
                    return false;

                size = FIELD_OFFSET(struct d2d_segment_beziers, segments[s.beziers->count]);
                break;
            case D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS:
                if (!d2d_figure_add_quadratic_beziers(figure, s.quad_beziers->segments, s.quad_beziers->count))
                    return false;

                size = FIELD_OFFSET(struct d2d_segment_quadratic_beziers, segments[s.quad_beziers->count]);
                break;
            case D2D_SEGMENT_TYPE_LINES:
                if (!d2d_figure_add_lines(figure, s.lines->points, s.lines->count))
                    return false;

                size = FIELD_OFFSET(struct d2d_segment_lines, points[s.lines->count]);
                break;
            case D2D_SEGMENT_TYPE_ARCS:
                for (j = 0; j < s.arcs->count; ++j)
                {
                    if (!d2d_figure_add_arc(figure, &s.arcs->segments[j]))
                        return false;
                }

                size = FIELD_OFFSET(struct d2d_segment_arcs, segments[s.arcs->count]);
                break;
            default:
                ;
        }

        s.segment = (struct d2d_segment *)((uint8_t *)s.segment + size);
    }

    return true;
}

static bool d2d_figure_begin(struct d2d_figure *figure, D2D1_POINT_2F start_point,
        D2D1_FIGURE_BEGIN figure_begin)
{
    if (figure_begin == D2D1_FIGURE_BEGIN_HOLLOW)
        figure->flags |= D2D_FIGURE_FLAG_HOLLOW;

    return d2d_figure_add_vertex(figure, start_point);
}

static bool d2d_figure_end(struct d2d_figure *figure, D2D1_FIGURE_END figure_end)
{
    /* Propagate allocation failures from deferred segment replay instead of
     * dereferencing a partially-materialized vertex array below. */
    if (!d2d_figure_produce_vertices(figure))
        return false;

    if (memcmp(&figure->vertices[0], &figure->vertices[figure->vertex_count - 1], sizeof(*figure->vertices)))
        figure->vertex_types[figure->vertex_count - 1] = D2D_VERTEX_TYPE_LINE;
    else
        figure->vertex_types[figure->vertex_count - 1] = D2D_VERTEX_TYPE_END;
    if (figure_end == D2D1_FIGURE_END_CLOSED)
        figure->flags |= D2D_FIGURE_FLAG_CLOSED;

    return true;
}

static void d2d_figure_cleanup(struct d2d_figure *figure)
{
    free(figure->original_bezier_controls);
    free(figure->bezier_controls);
    free(figure->vertices);
    free(figure->vertex_types);
    free(figure->segments.data);
    memset(figure, 0, sizeof(*figure));
}

static void d2d_cdt_edge_rot(struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    dst->idx = src->idx;
    dst->r = (src->r + D2D_EDGE_NEXT_ROT) & 3;
}

static void d2d_cdt_edge_sym(struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    dst->idx = src->idx;
    dst->r = (src->r + D2D_EDGE_NEXT_SYM) & 3;
}

static void d2d_cdt_edge_tor(struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    dst->idx = src->idx;
    dst->r = (src->r + D2D_EDGE_NEXT_TOR) & 3;
}

static void d2d_cdt_edge_next_left(const struct d2d_cdt *cdt,
        struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    d2d_cdt_edge_rot(dst, &cdt->edges[src->idx].next[(src->r + D2D_EDGE_NEXT_TOR) & 3]);
}

static void d2d_cdt_edge_next_origin(const struct d2d_cdt *cdt,
        struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    *dst = cdt->edges[src->idx].next[src->r];
}

static void d2d_cdt_edge_prev_origin(const struct d2d_cdt *cdt,
        struct d2d_cdt_edge_ref *dst, const struct d2d_cdt_edge_ref *src)
{
    d2d_cdt_edge_rot(dst, &cdt->edges[src->idx].next[(src->r + D2D_EDGE_NEXT_ROT) & 3]);
}

static size_t d2d_cdt_edge_origin(const struct d2d_cdt *cdt, const struct d2d_cdt_edge_ref *e)
{
    return cdt->edges[e->idx].vertex[e->r >> 1];
}

static size_t d2d_cdt_edge_destination(const struct d2d_cdt *cdt, const struct d2d_cdt_edge_ref *e)
{
    return cdt->edges[e->idx].vertex[!(e->r >> 1)];
}

static void d2d_cdt_edge_set_origin(const struct d2d_cdt *cdt,
        const struct d2d_cdt_edge_ref *e, size_t vertex)
{
    cdt->edges[e->idx].vertex[e->r >> 1] = vertex;
}

static void d2d_cdt_edge_set_destination(const struct d2d_cdt *cdt,
        const struct d2d_cdt_edge_ref *e, size_t vertex)
{
    cdt->edges[e->idx].vertex[!(e->r >> 1)] = vertex;
}

static float d2d_cdt_ccw(const struct d2d_cdt *cdt, size_t a, size_t b, size_t c)
{
    return d2d_point_ccw(&cdt->vertices[a], &cdt->vertices[b], &cdt->vertices[c]);
}

static BOOL d2d_cdt_rightof(const struct d2d_cdt *cdt, size_t p, const struct d2d_cdt_edge_ref *e)
{
    return d2d_cdt_ccw(cdt, p, d2d_cdt_edge_destination(cdt, e), d2d_cdt_edge_origin(cdt, e)) > 0.0f;
}

static BOOL d2d_cdt_leftof(const struct d2d_cdt *cdt, size_t p, const struct d2d_cdt_edge_ref *e)
{
    return d2d_cdt_ccw(cdt, p, d2d_cdt_edge_origin(cdt, e), d2d_cdt_edge_destination(cdt, e)) > 0.0f;
}

/* |ax ay|
 * |bx by| */
static void d2d_fp_four_det2x2(float *out, float ax, float ay, float bx, float by)
{
    float axby[2], aybx[2];

    d2d_fp_two_product(axby, ax, by);
    d2d_fp_two_product(aybx, ay, bx);
    d2d_fp_two_two_diff(out, axby, aybx);
}

/* (a->x² + a->y²) * det2x2 */
static void d2d_fp_sub_det3x3(float *out, size_t *out_len, const struct d2d_fp_two_vec2 *a, const float *det2x2)
{
    size_t axd_len, ayd_len, axxd_len, ayyd_len;
    float axd[8], ayd[8], axxd[16], ayyd[16];

    d2d_fp_scale_expansion_zeroelim(axd, &axd_len, det2x2, 4, a->x[1]);
    d2d_fp_scale_expansion_zeroelim(axxd, &axxd_len, axd, axd_len, a->x[1]);
    d2d_fp_scale_expansion_zeroelim(ayd, &ayd_len, det2x2, 4, a->y[1]);
    d2d_fp_scale_expansion_zeroelim(ayyd, &ayyd_len, ayd, ayd_len, a->y[1]);
    d2d_fp_fast_expansion_sum_zeroelim(out, out_len, axxd, axxd_len, ayyd, ayyd_len);
}

/* det_abt = det_ab * c[0]
 * fin += c[0] * (az * b - bz * a + c[1] * det_ab * 2.0f) */
static void d2d_cdt_incircle_refine1(struct d2d_fp_fin *fin, float *det_abt, size_t *det_abt_len,
        const float *det_ab, float a, const float *az, float b, const float *bz, const float *c)
{
    size_t temp48_len, temp32_len, temp16a_len, temp16b_len, temp16c_len, temp8_len;
    float temp48[48], temp32[32], temp16a[16], temp16b[16], temp16c[16], temp8[8];
    float *swap;

    d2d_fp_scale_expansion_zeroelim(det_abt, det_abt_len, det_ab, 4, c[0]);
    d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, det_abt, *det_abt_len, 2.0f * c[1]);
    d2d_fp_scale_expansion_zeroelim(temp8, &temp8_len, az, 4, c[0]);
    d2d_fp_scale_expansion_zeroelim(temp16b, &temp16b_len, temp8, temp8_len, b);
    d2d_fp_scale_expansion_zeroelim(temp8, &temp8_len, bz, 4, c[0]);
    d2d_fp_scale_expansion_zeroelim(temp16c, &temp16c_len, temp8, temp8_len, -a);
    d2d_fp_fast_expansion_sum_zeroelim(temp32, &temp32_len, temp16a, temp16a_len, temp16b, temp16b_len);
    d2d_fp_fast_expansion_sum_zeroelim(temp48, &temp48_len, temp16c, temp16c_len, temp32, temp32_len);
    d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp48, temp48_len);
    swap = fin->now; fin->now = fin->other; fin->other = swap;
}

static void d2d_cdt_incircle_refine2(struct d2d_fp_fin *fin, const struct d2d_fp_two_vec2 *a,
        const struct d2d_fp_two_vec2 *b, const float *bz, const struct d2d_fp_two_vec2 *c, const float *cz,
        const float *axt_det_bc, size_t axt_det_bc_len, const float *ayt_det_bc, size_t ayt_det_bc_len)
{
    size_t temp64_len, temp48_len, temp32a_len, temp32b_len, temp16a_len, temp16b_len, temp8_len;
    float temp64[64], temp48[48], temp32a[32], temp32b[32], temp16a[16], temp16b[16], temp8[8];
    float bct[8], bctt[4], temp4a[4], temp4b[4], temp2a[2], temp2b[2];
    size_t bct_len, bctt_len;
    float *swap;

    /* bct = (b->x[0] * c->y[1] + b->x[1] * c->y[0]) - (c->x[0] * b->y[1] + c->x[1] * b->y[0]) */
    /* bctt = b->x[0] * c->y[0] + c->x[0] * b->y[0] */
    if (b->x[0] != 0.0f || b->y[0] != 0.0f || c->x[0] != 0.0f || c->y[0] != 0.0f)
    {
        d2d_fp_two_product(temp2a, b->x[0], c->y[1]);
        d2d_fp_two_product(temp2b, b->x[1], c->y[0]);
        d2d_fp_two_two_sum(temp4a, temp2a, temp2b);
        d2d_fp_two_product(temp2a, c->x[0], -b->y[1]);
        d2d_fp_two_product(temp2b, c->x[1], -b->y[0]);
        d2d_fp_two_two_sum(temp4b, temp2a, temp2b);
        d2d_fp_fast_expansion_sum_zeroelim(bct, &bct_len, temp4a, 4, temp4b, 4);

        d2d_fp_two_product(temp2a, b->x[0], c->y[0]);
        d2d_fp_two_product(temp2b, c->x[0], b->y[0]);
        d2d_fp_two_two_diff(bctt, temp2a, temp2b);
        bctt_len = 4;
    }
    else
    {
        bct[0] = 0.0f;
        bct_len = 1;
        bctt[0] = 0.0f;
        bctt_len = 1;
    }

    if (a->x[0] != 0.0f)
    {
        size_t axt_bct_len, axt_bctt_len;
        float axt_bct[16], axt_bctt[8];

        /* fin += a->x[0] * (axt_det_bc + bct * 2.0f * a->x[1]) */
        d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, axt_det_bc, axt_det_bc_len, a->x[0]);
        d2d_fp_scale_expansion_zeroelim(axt_bct, &axt_bct_len, bct, bct_len, a->x[0]);
        d2d_fp_scale_expansion_zeroelim(temp32a, &temp32a_len, axt_bct, axt_bct_len, 2.0f * a->x[1]);
        d2d_fp_fast_expansion_sum_zeroelim(temp48, &temp48_len, temp16a, temp16a_len, temp32a, temp32a_len);
        d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp48, temp48_len);
        swap = fin->now; fin->now = fin->other; fin->other = swap;

        if (b->y[0] != 0.0f)
        {
            /* fin += a->x[0] * cz * b->y[0] */
            d2d_fp_scale_expansion_zeroelim(temp8, &temp8_len, cz, 4, a->x[0]);
            d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, temp8, temp8_len, b->y[0]);
            d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp16a, temp16a_len);
            swap = fin->now; fin->now = fin->other; fin->other = swap;
        }

        if (c->y[0] != 0.0f)
        {
            /* fin -= a->x[0] * bz * c->y[0] */
            d2d_fp_scale_expansion_zeroelim(temp8, &temp8_len, bz, 4, -a->x[0]);
            d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, temp8, temp8_len, c->y[0]);
            d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp16a, temp16a_len);
            swap = fin->now; fin->now = fin->other; fin->other = swap;
        }

        /* fin += a->x[0] * (bct * a->x[0] + bctt * (2.0f * a->x[1] + a->x[0])) */
        d2d_fp_scale_expansion_zeroelim(temp32a, &temp32a_len, axt_bct, axt_bct_len, a->x[0]);
        d2d_fp_scale_expansion_zeroelim(axt_bctt, &axt_bctt_len, bctt, bctt_len, a->x[0]);
        d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, axt_bctt, axt_bctt_len, 2.0f * a->x[1]);
        d2d_fp_scale_expansion_zeroelim(temp16b, &temp16b_len, axt_bctt, axt_bctt_len, a->x[0]);
        d2d_fp_fast_expansion_sum_zeroelim(temp32b, &temp32b_len, temp16a, temp16a_len, temp16b, temp16b_len);
        d2d_fp_fast_expansion_sum_zeroelim(temp64, &temp64_len, temp32a, temp32a_len, temp32b, temp32b_len);
        d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp64, temp64_len);
        swap = fin->now; fin->now = fin->other; fin->other = swap;
    }

    if (a->y[0] != 0.0f)
    {
        size_t ayt_bct_len, ayt_bctt_len;
        float ayt_bct[16], ayt_bctt[8];

        /* fin += a->y[0] * (ayt_det_bc + bct * 2.0f * a->y[1]) */
        d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, ayt_det_bc, ayt_det_bc_len, a->y[0]);
        d2d_fp_scale_expansion_zeroelim(ayt_bct, &ayt_bct_len, bct, bct_len, a->y[0]);
        d2d_fp_scale_expansion_zeroelim(temp32a, &temp32a_len, ayt_bct, ayt_bct_len, 2.0f * a->y[1]);
        d2d_fp_fast_expansion_sum_zeroelim(temp48, &temp48_len, temp16a, temp16a_len, temp32a, temp32a_len);
        d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp48, temp48_len);
        swap = fin->now; fin->now = fin->other; fin->other = swap;

        /* fin += a->y[0] * (bct * a->y[0] + bctt * (2.0f * a->y[1] + a->y[0])) */
        d2d_fp_scale_expansion_zeroelim(temp32a, &temp32a_len, ayt_bct, ayt_bct_len, a->y[0]);
        d2d_fp_scale_expansion_zeroelim(ayt_bctt, &ayt_bctt_len, bctt, bctt_len, a->y[0]);
        d2d_fp_scale_expansion_zeroelim(temp16a, &temp16a_len, ayt_bctt, ayt_bctt_len, 2.0f * a->y[1]);
        d2d_fp_scale_expansion_zeroelim(temp16b, &temp16b_len, ayt_bctt, ayt_bctt_len, a->y[0]);
        d2d_fp_fast_expansion_sum_zeroelim(temp32b, &temp32b_len, temp16a, temp16a_len, temp16b, temp16b_len);
        d2d_fp_fast_expansion_sum_zeroelim(temp64, &temp64_len, temp32a, temp32a_len, temp32b, temp32b_len);
        d2d_fp_fast_expansion_sum_zeroelim(fin->other, &fin->length, fin->now, fin->length, temp64, temp64_len);
        swap = fin->now; fin->now = fin->other; fin->other = swap;
    }
}

/* Determine if point D is inside or outside the circle defined by points A,
 * B, C. As explained in the paper by Guibas and Stolfi, this is equivalent to
 * calculating the signed volume of the tetrahedron defined by projecting the
 * points onto the paraboloid of revolution x = x² + y²,
 * λ:(x, y) → (x, y, x² + y²). I.e., D is inside the cirlce if
 *
 * |λ(A) 1|
 * |λ(B) 1| > 0
 * |λ(C) 1|
 * |λ(D) 1|
 *
 * After translating D to the origin, that becomes:
 *
 * |λ(A-D)|
 * |λ(B-D)| > 0
 * |λ(C-D)|
 *
 * This implementation is based on the paper "Adaptive Precision
 * Floating-Point Arithmetic and Fast Robust Geometric Predicates" and
 * associated (Public Domain) code by Jonathan Richard Shewchuk. */
static BOOL d2d_cdt_incircle(const struct d2d_cdt *cdt, size_t a, size_t b, size_t c, size_t d)
{
    static const float err_bound_result = (3.0f + 8.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_a = (10.0f + 96.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_b = (4.0f + 48.0f * D2D_FP_EPS) * D2D_FP_EPS;
    static const float err_bound_c = (44.0f + 576.0f * D2D_FP_EPS) * D2D_FP_EPS * D2D_FP_EPS;

    size_t axt_det_bc_len, ayt_det_bc_len, bxt_det_ca_len, byt_det_ca_len, cxt_det_ab_len, cyt_det_ab_len;
    float axt_det_bc[8], ayt_det_bc[8], bxt_det_ca[8], byt_det_ca[8], cxt_det_ab[8], cyt_det_ab[8];
    float fin1[1152], fin2[1152], temp64[64], sub_det_a[32], sub_det_b[32], sub_det_c[32];
    float det_bc[4], det_ca[4], det_ab[4], daz[4], dbz[4], dcz[4], temp2a[2], temp2b[2];
    size_t temp64_len, sub_det_a_len, sub_det_b_len, sub_det_c_len;
    float dbxdcy, dbydcx, dcxday, dcydax, daxdby, daydbx;
    const D2D1_POINT_2F *p = cdt->vertices;
    struct d2d_fp_two_vec2 da, db, dc;
    float permanent, err_bound, det;
    struct d2d_fp_fin fin;

    da.x[1] = p[a].x - p[d].x;
    da.y[1] = p[a].y - p[d].y;
    db.x[1] = p[b].x - p[d].x;
    db.y[1] = p[b].y - p[d].y;
    dc.x[1] = p[c].x - p[d].x;
    dc.y[1] = p[c].y - p[d].y;

    daz[3] = da.x[1] * da.x[1] + da.y[1] * da.y[1];
    dbxdcy = db.x[1] * dc.y[1];
    dbydcx = db.y[1] * dc.x[1];

    dbz[3] = db.x[1] * db.x[1] + db.y[1] * db.y[1];
    dcxday = dc.x[1] * da.y[1];
    dcydax = dc.y[1] * da.x[1];

    dcz[3] = dc.x[1] * dc.x[1] + dc.y[1] * dc.y[1];
    daxdby = da.x[1] * db.y[1];
    daydbx = da.y[1] * db.x[1];

    det = daz[3] * (dbxdcy - dbydcx) + dbz[3] * (dcxday - dcydax) + dcz[3] * (daxdby - daydbx);
    permanent = daz[3] * (fabsf(dbxdcy) + fabsf(dbydcx))
            + dbz[3] * (fabsf(dcxday) + fabsf(dcydax))
            + dcz[3] * (fabsf(daxdby) + fabsf(daydbx));
    err_bound = err_bound_a * permanent;
    if (det > err_bound || -det > err_bound)
        return det > 0.0f;

    fin.now = fin1;
    fin.other = fin2;

    d2d_fp_four_det2x2(det_bc, db.x[1], db.y[1], dc.x[1], dc.y[1]);
    d2d_fp_sub_det3x3(sub_det_a, &sub_det_a_len, &da, det_bc);

    d2d_fp_four_det2x2(det_ca, dc.x[1], dc.y[1], da.x[1], da.y[1]);
    d2d_fp_sub_det3x3(sub_det_b, &sub_det_b_len, &db, det_ca);

    d2d_fp_four_det2x2(det_ab, da.x[1], da.y[1], db.x[1], db.y[1]);
    d2d_fp_sub_det3x3(sub_det_c, &sub_det_c_len, &dc, det_ab);

    d2d_fp_fast_expansion_sum_zeroelim(temp64, &temp64_len, sub_det_a, sub_det_a_len, sub_det_b, sub_det_b_len);
    d2d_fp_fast_expansion_sum_zeroelim(fin.now, &fin.length, temp64, temp64_len, sub_det_c, sub_det_c_len);
    det = d2d_fp_estimate(fin.now, fin.length);
    err_bound = err_bound_b * permanent;
    if (det >= err_bound || -det >= err_bound)
        return det > 0.0f;

    d2d_fp_two_diff_tail(&da.x[0], p[a].x, p[d].x, da.x[1]);
    d2d_fp_two_diff_tail(&da.y[0], p[a].y, p[d].y, da.y[1]);
    d2d_fp_two_diff_tail(&db.x[0], p[b].x, p[d].x, db.x[1]);
    d2d_fp_two_diff_tail(&db.y[0], p[b].y, p[d].y, db.y[1]);
    d2d_fp_two_diff_tail(&dc.x[0], p[c].x, p[d].x, dc.x[1]);
    d2d_fp_two_diff_tail(&dc.y[0], p[c].y, p[d].y, dc.y[1]);
    if (da.x[0] == 0.0f && db.x[0] == 0.0f && dc.x[0] == 0.0f
            && da.y[0] == 0.0f && db.y[0] == 0.0f && dc.y[0] == 0.0f)
        return det > 0.0f;

    err_bound = err_bound_c * permanent + err_bound_result * fabsf(det);
    det += (daz[3] * ((db.x[1] * dc.y[0] + dc.y[1] * db.x[0]) - (db.y[1] * dc.x[0] + dc.x[1] * db.y[0]))
            + 2.0f * (da.x[1] * da.x[0] + da.y[1] * da.y[0]) * (db.x[1] * dc.y[1] - db.y[1] * dc.x[1]))
            + (dbz[3] * ((dc.x[1] * da.y[0] + da.y[1] * dc.x[0]) - (dc.y[1] * da.x[0] + da.x[1] * dc.y[0]))
            + 2.0f * (db.x[1] * db.x[0] + db.y[1] * db.y[0]) * (dc.x[1] * da.y[1] - dc.y[1] * da.x[1]))
            + (dcz[3] * ((da.x[1] * db.y[0] + db.y[1] * da.x[0]) - (da.y[1] * db.x[0] + db.x[1] * da.y[0]))
            + 2.0f * (dc.x[1] * dc.x[0] + dc.y[1] * dc.y[0]) * (da.x[1] * db.y[1] - da.y[1] * db.x[1]));
    if (det >= err_bound || -det >= err_bound)
        return det > 0.0f;

    if (db.x[0] != 0.0f || db.y[0] != 0.0f || dc.x[0] != 0.0f || dc.y[0] != 0.0f)
    {
        d2d_fp_square(temp2a, da.x[1]);
        d2d_fp_square(temp2b, da.y[1]);
        d2d_fp_two_two_sum(daz, temp2a, temp2b);
    }
    if (dc.x[0] != 0.0f || dc.y[0] != 0.0f || da.x[0] != 0.0f || da.y[0] != 0.0f)
    {
        d2d_fp_square(temp2a, db.x[1]);
        d2d_fp_square(temp2b, db.y[1]);
        d2d_fp_two_two_sum(dbz, temp2a, temp2b);
    }
    if (da.x[0] != 0.0f || da.y[0] != 0.0f || db.x[0] != 0.0f || db.y[0] != 0.0f)
    {
        d2d_fp_square(temp2a, dc.x[1]);
        d2d_fp_square(temp2b, dc.y[1]);
        d2d_fp_two_two_sum(dcz, temp2a, temp2b);
    }

    if (da.x[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, axt_det_bc, &axt_det_bc_len, det_bc, dc.y[1], dcz, db.y[1], dbz, da.x);
    if (da.y[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, ayt_det_bc, &ayt_det_bc_len, det_bc, db.x[1], dbz, dc.x[1], dcz, da.y);
    if (db.x[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, bxt_det_ca, &bxt_det_ca_len, det_ca, da.y[1], daz, dc.y[1], dcz, db.x);
    if (db.y[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, byt_det_ca, &byt_det_ca_len, det_ca, dc.x[1], dcz, da.x[1], daz, db.y);
    if (dc.x[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, cxt_det_ab, &cxt_det_ab_len, det_ab, db.y[1], dbz, da.y[1], daz, dc.x);
    if (dc.y[0] != 0.0f)
        d2d_cdt_incircle_refine1(&fin, cyt_det_ab, &cyt_det_ab_len, det_ab, da.x[1], daz, db.x[1], dbz, dc.y);

    if (da.x[0] != 0.0f || da.y[0] != 0.0f)
        d2d_cdt_incircle_refine2(&fin, &da, &db, dbz, &dc, dcz,
                axt_det_bc, axt_det_bc_len, ayt_det_bc, ayt_det_bc_len);
    if (db.x[0] != 0.0f || db.y[0] != 0.0f)
        d2d_cdt_incircle_refine2(&fin, &db, &dc, dcz, &da, daz,
                bxt_det_ca, bxt_det_ca_len, byt_det_ca, byt_det_ca_len);
    if (dc.x[0] != 0.0f || dc.y[0] != 0.0f)
        d2d_cdt_incircle_refine2(&fin, &dc, &da, daz, &db, dbz,
                cxt_det_ab, cxt_det_ab_len, cyt_det_ab, cyt_det_ab_len);

    return fin.now[fin.length - 1] > 0.0f;
}

static void d2d_cdt_splice(const struct d2d_cdt *cdt, const struct d2d_cdt_edge_ref *a,
        const struct d2d_cdt_edge_ref *b)
{
    struct d2d_cdt_edge_ref ta, tb, alpha, beta;

    ta = cdt->edges[a->idx].next[a->r];
    tb = cdt->edges[b->idx].next[b->r];
    cdt->edges[a->idx].next[a->r] = tb;
    cdt->edges[b->idx].next[b->r] = ta;

    d2d_cdt_edge_rot(&alpha, &ta);
    d2d_cdt_edge_rot(&beta, &tb);

    ta = cdt->edges[alpha.idx].next[alpha.r];
    tb = cdt->edges[beta.idx].next[beta.r];
    cdt->edges[alpha.idx].next[alpha.r] = tb;
    cdt->edges[beta.idx].next[beta.r] = ta;
}

static BOOL d2d_cdt_create_edge(struct d2d_cdt *cdt, struct d2d_cdt_edge_ref *e)
{
    struct d2d_cdt_edge *edge;

    if (cdt->free_edge != ~0u)
    {
        e->idx = cdt->free_edge;
        cdt->free_edge = cdt->edges[e->idx].next[D2D_EDGE_NEXT_ORIGIN].idx;
    }
    else
    {
        if (!d2d_array_reserve((void **)&cdt->edges, &cdt->edges_size, cdt->edge_count + 1, sizeof(*cdt->edges)))
        {
            ERR("Failed to grow edges array.\n");
            return FALSE;
        }
        e->idx = cdt->edge_count++;
    }
    e->r = 0;

    edge = &cdt->edges[e->idx];
    edge->next[D2D_EDGE_NEXT_ORIGIN] = *e;
    d2d_cdt_edge_tor(&edge->next[D2D_EDGE_NEXT_ROT], e);
    d2d_cdt_edge_sym(&edge->next[D2D_EDGE_NEXT_SYM], e);
    d2d_cdt_edge_rot(&edge->next[D2D_EDGE_NEXT_TOR], e);
    edge->flags = 0;

    return TRUE;
}

static void d2d_cdt_destroy_edge(struct d2d_cdt *cdt, const struct d2d_cdt_edge_ref *e)
{
    struct d2d_cdt_edge_ref next, sym, prev;

    d2d_cdt_edge_next_origin(cdt, &next, e);
    if (next.idx != e->idx || next.r != e->r)
    {
        d2d_cdt_edge_prev_origin(cdt, &prev, e);
        d2d_cdt_splice(cdt, e, &prev);
    }

    d2d_cdt_edge_sym(&sym, e);

    d2d_cdt_edge_next_origin(cdt, &next, &sym);
    if (next.idx != sym.idx || next.r != sym.r)
    {
        d2d_cdt_edge_prev_origin(cdt, &prev, &sym);
        d2d_cdt_splice(cdt, &sym, &prev);
    }

    cdt->edges[e->idx].flags |= D2D_CDT_EDGE_FLAG_FREED;
    cdt->edges[e->idx].next[D2D_EDGE_NEXT_ORIGIN].idx = cdt->free_edge;
    cdt->free_edge = e->idx;
}

static BOOL d2d_cdt_connect(struct d2d_cdt *cdt, struct d2d_cdt_edge_ref *e,
        const struct d2d_cdt_edge_ref *a, const struct d2d_cdt_edge_ref *b)
{
    struct d2d_cdt_edge_ref tmp;

    if (!d2d_cdt_create_edge(cdt, e))
        return FALSE;
    d2d_cdt_edge_set_origin(cdt, e, d2d_cdt_edge_destination(cdt, a));
    d2d_cdt_edge_set_destination(cdt, e, d2d_cdt_edge_origin(cdt, b));
    d2d_cdt_edge_next_left(cdt, &tmp, a);
    d2d_cdt_splice(cdt, e, &tmp);
    d2d_cdt_edge_sym(&tmp, e);
    d2d_cdt_splice(cdt, &tmp, b);

    return TRUE;
}

static BOOL d2d_cdt_merge(struct d2d_cdt *cdt, struct d2d_cdt_edge_ref *left_outer,
        struct d2d_cdt_edge_ref *left_inner, struct d2d_cdt_edge_ref *right_inner,
        struct d2d_cdt_edge_ref *right_outer)
{
    struct d2d_cdt_edge_ref base_edge, tmp;

    /* Create the base edge between both parts. */
    for (;;)
    {
        if (d2d_cdt_leftof(cdt, d2d_cdt_edge_origin(cdt, right_inner), left_inner))
        {
            d2d_cdt_edge_next_left(cdt, left_inner, left_inner);
        }
        else if (d2d_cdt_rightof(cdt, d2d_cdt_edge_origin(cdt, left_inner), right_inner))
        {
            d2d_cdt_edge_sym(&tmp, right_inner);
            d2d_cdt_edge_next_origin(cdt, right_inner, &tmp);
        }
        else
        {
            break;
        }
    }

    d2d_cdt_edge_sym(&tmp, right_inner);
    if (!d2d_cdt_connect(cdt, &base_edge, &tmp, left_inner))
        return FALSE;
    if (d2d_cdt_edge_origin(cdt, left_inner) == d2d_cdt_edge_origin(cdt, left_outer))
        d2d_cdt_edge_sym(left_outer, &base_edge);
    if (d2d_cdt_edge_origin(cdt, right_inner) == d2d_cdt_edge_origin(cdt, right_outer))
        *right_outer = base_edge;

    for (;;)
    {
        struct d2d_cdt_edge_ref left_candidate, right_candidate, sym_base_edge;
        BOOL left_valid, right_valid;

        /* Find the left candidate. */
        d2d_cdt_edge_sym(&sym_base_edge, &base_edge);
        d2d_cdt_edge_next_origin(cdt, &left_candidate, &sym_base_edge);
        if ((left_valid = d2d_cdt_leftof(cdt, d2d_cdt_edge_destination(cdt, &left_candidate), &sym_base_edge)))
        {
            d2d_cdt_edge_next_origin(cdt, &tmp, &left_candidate);
            while (d2d_cdt_edge_destination(cdt, &tmp) != d2d_cdt_edge_destination(cdt, &sym_base_edge)
                    && d2d_cdt_incircle(cdt,
                    d2d_cdt_edge_origin(cdt, &sym_base_edge), d2d_cdt_edge_destination(cdt, &sym_base_edge),
                    d2d_cdt_edge_destination(cdt, &left_candidate), d2d_cdt_edge_destination(cdt, &tmp)))
            {
                d2d_cdt_destroy_edge(cdt, &left_candidate);
                left_candidate = tmp;
                d2d_cdt_edge_next_origin(cdt, &tmp, &left_candidate);
            }
        }
        d2d_cdt_edge_sym(&left_candidate, &left_candidate);

        /* Find the right candidate. */
        d2d_cdt_edge_prev_origin(cdt, &right_candidate, &base_edge);
        if ((right_valid = d2d_cdt_rightof(cdt, d2d_cdt_edge_destination(cdt, &right_candidate), &base_edge)))
        {
            d2d_cdt_edge_prev_origin(cdt, &tmp, &right_candidate);
            while (d2d_cdt_edge_destination(cdt, &tmp) != d2d_cdt_edge_destination(cdt, &base_edge)
                    && d2d_cdt_incircle(cdt,
                    d2d_cdt_edge_origin(cdt, &sym_base_edge), d2d_cdt_edge_destination(cdt, &sym_base_edge),
                    d2d_cdt_edge_destination(cdt, &right_candidate), d2d_cdt_edge_destination(cdt, &tmp)))
            {
                d2d_cdt_destroy_edge(cdt, &right_candidate);
                right_candidate = tmp;
                d2d_cdt_edge_prev_origin(cdt, &tmp, &right_candidate);
            }
        }

        if (!left_valid && !right_valid)
            break;

        /* Connect the appropriate candidate with the base edge. */
        if (!left_valid || (right_valid && d2d_cdt_incircle(cdt,
                d2d_cdt_edge_origin(cdt, &left_candidate), d2d_cdt_edge_destination(cdt, &left_candidate),
                d2d_cdt_edge_origin(cdt, &right_candidate), d2d_cdt_edge_destination(cdt, &right_candidate))))
        {
            if (!d2d_cdt_connect(cdt, &base_edge, &right_candidate, &sym_base_edge))
                return FALSE;
        }
        else
        {
            if (!d2d_cdt_connect(cdt, &base_edge, &sym_base_edge, &left_candidate))
                return FALSE;
        }
    }

    return TRUE;
}

/* Create a Delaunay triangulation from a set of vertices. This is an
 * implementation of the divide-and-conquer algorithm described by Guibas and
 * Stolfi. Should be called with at least two vertices. */
static BOOL d2d_cdt_triangulate(struct d2d_cdt *cdt, size_t start_vertex, size_t vertex_count,
        struct d2d_cdt_edge_ref *left_edge, struct d2d_cdt_edge_ref *right_edge)
{
    struct d2d_cdt_edge_ref left_inner, left_outer, right_inner, right_outer, tmp;
    size_t cut;

    /* Only two vertices, create a single edge. */
    if (vertex_count == 2)
    {
        struct d2d_cdt_edge_ref a;

        if (!d2d_cdt_create_edge(cdt, &a))
            return FALSE;
        d2d_cdt_edge_set_origin(cdt, &a, start_vertex);
        d2d_cdt_edge_set_destination(cdt, &a, start_vertex + 1);

        *left_edge = a;
        d2d_cdt_edge_sym(right_edge, &a);

        return TRUE;
    }

    /* Three vertices, create a triangle. */
    if (vertex_count == 3)
    {
        struct d2d_cdt_edge_ref a, b, c;
        float det;

        if (!d2d_cdt_create_edge(cdt, &a))
            return FALSE;
        if (!d2d_cdt_create_edge(cdt, &b))
            return FALSE;
        d2d_cdt_edge_sym(&tmp, &a);
        d2d_cdt_splice(cdt, &tmp, &b);

        d2d_cdt_edge_set_origin(cdt, &a, start_vertex);
        d2d_cdt_edge_set_destination(cdt, &a, start_vertex + 1);
        d2d_cdt_edge_set_origin(cdt, &b, start_vertex + 1);
        d2d_cdt_edge_set_destination(cdt, &b, start_vertex + 2);

        det = d2d_cdt_ccw(cdt, start_vertex, start_vertex + 1, start_vertex + 2);
        if (det != 0.0f && !d2d_cdt_connect(cdt, &c, &b, &a))
            return FALSE;

        if (det < 0.0f)
        {
            d2d_cdt_edge_sym(left_edge, &c);
            *right_edge = c;
        }
        else
        {
            *left_edge = a;
            d2d_cdt_edge_sym(right_edge, &b);
        }

        return TRUE;
    }

    /* More than three vertices, divide. */
    cut = vertex_count / 2;
    if (!d2d_cdt_triangulate(cdt, start_vertex, cut, &left_outer, &left_inner))
        return FALSE;
    if (!d2d_cdt_triangulate(cdt, start_vertex + cut, vertex_count - cut, &right_inner, &right_outer))
        return FALSE;
    /* Merge the left and right parts. */
    if (!d2d_cdt_merge(cdt, &left_outer, &left_inner, &right_inner, &right_outer))
        return FALSE;

    *left_edge = left_outer;
    *right_edge = right_outer;
    return TRUE;
}

/* Order NaNs after all numbers and equal to each other, so that the result is
 * a total order.  Comparing the difference against zero instead would return
 * "less than" for every pair involving a NaN, including a pair of equal ones,
 * which is not an ordering qsort() and bsearch() can work with. */
static int d2d_compare_float(float a, float b)
{
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    if (a == b)
        return 0;
    if (!isnan(a))
        return -1;
    if (!isnan(b))
        return 1;
    return 0;
}

static int __cdecl d2d_cdt_compare_vertices(const void *a, const void *b)
{
    const D2D1_POINT_2F *p0 = a;
    const D2D1_POINT_2F *p1 = b;
    int ret;

    if ((ret = d2d_compare_float(p0->x, p1->x)))
        return ret;

    return d2d_compare_float(p0->y, p1->y);
}

/* Determine whether a given point is inside the geometry, using the current
 * fill mode rule. */
static BOOL d2d_path_geometry_point_inside(const struct d2d_geometry *geometry,
        const D2D1_POINT_2F *probe, BOOL triangles_only)
{
    const D2D1_POINT_2F *p0, *p1;
    D2D1_POINT_2F v_p, v_probe;
    unsigned int score;
    size_t i, j, last;

    for (i = 0, score = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];

        if (probe->x < figure->bounds.left || probe->x > figure->bounds.right
                || probe->y < figure->bounds.top || probe->y > figure->bounds.bottom)
            continue;

        last = figure->vertex_count - 1;
        if (!triangles_only)
        {
            while (last && figure->vertex_types[last] == D2D_VERTEX_TYPE_NONE)
                --last;
        }
        p0 = &figure->vertices[last];
        for (j = 0; j <= last; ++j)
        {
            if (!triangles_only && figure->vertex_types[j] == D2D_VERTEX_TYPE_NONE)
                continue;

            p1 = &figure->vertices[j];
            d2d_point_subtract(&v_p, p1, p0);
            d2d_point_subtract(&v_probe, probe, p0);

            if ((probe->y < p0->y) != (probe->y < p1->y) && v_probe.x < v_p.x * (v_probe.y / v_p.y))
            {
                if (geometry->u.path.fill_mode == D2D1_FILL_MODE_ALTERNATE || (probe->y < p0->y))
                    ++score;
                else
                    --score;
            }

            p0 = p1;
        }
    }

    return geometry->u.path.fill_mode == D2D1_FILL_MODE_ALTERNATE ? score & 1 : score;
}

static BOOL d2d_path_geometry_add_fill_face(struct d2d_geometry *geometry, const struct d2d_cdt *cdt,
        const struct d2d_cdt_edge_ref *base_edge)
{
    struct d2d_cdt_edge_ref tmp;
    struct d2d_face *face;
    D2D1_POINT_2F probe;

    if (cdt->edges[base_edge->idx].flags & D2D_CDT_EDGE_FLAG_VISITED(base_edge->r))
        return TRUE;

    if (!d2d_array_reserve((void **)&geometry->fill.faces, &geometry->fill.faces_size,
            geometry->fill.face_count + 1, sizeof(*geometry->fill.faces)))
    {
        ERR("Failed to grow faces array.\n");
        return FALSE;
    }

    face = &geometry->fill.faces[geometry->fill.face_count];

    /* It may seem tempting to use the center of the face as probe origin, but
     * multiplying by powers of two works much better for preserving accuracy. */

    tmp = *base_edge;
    cdt->edges[tmp.idx].flags |= D2D_CDT_EDGE_FLAG_VISITED(tmp.r);
    face->v[0] = d2d_cdt_edge_origin(cdt, &tmp);
    probe.x = cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].x * 0.25f;
    probe.y = cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].y * 0.25f;

    d2d_cdt_edge_next_left(cdt, &tmp, &tmp);
    cdt->edges[tmp.idx].flags |= D2D_CDT_EDGE_FLAG_VISITED(tmp.r);
    face->v[1] = d2d_cdt_edge_origin(cdt, &tmp);
    probe.x += cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].x * 0.25f;
    probe.y += cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].y * 0.25f;

    d2d_cdt_edge_next_left(cdt, &tmp, &tmp);
    cdt->edges[tmp.idx].flags |= D2D_CDT_EDGE_FLAG_VISITED(tmp.r);
    face->v[2] = d2d_cdt_edge_origin(cdt, &tmp);
    probe.x += cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].x * 0.50f;
    probe.y += cdt->vertices[d2d_cdt_edge_origin(cdt, &tmp)].y * 0.50f;

    if (d2d_cdt_leftof(cdt, face->v[2], base_edge) && d2d_path_geometry_point_inside(geometry, &probe, TRUE))
        ++geometry->fill.face_count;

    return TRUE;
}

static BOOL d2d_cdt_generate_faces(const struct d2d_cdt *cdt, struct d2d_geometry *geometry)
{
    struct d2d_cdt_edge_ref base_edge;
    size_t i;

    for (i = 0; i < cdt->edge_count; ++i)
    {
        if (cdt->edges[i].flags & D2D_CDT_EDGE_FLAG_FREED)
            continue;

        base_edge.idx = i;
        base_edge.r = 0;
        if (!d2d_path_geometry_add_fill_face(geometry, cdt, &base_edge))
            goto fail;
        d2d_cdt_edge_sym(&base_edge, &base_edge);
        if (!d2d_path_geometry_add_fill_face(geometry, cdt, &base_edge))
            goto fail;
    }

    return TRUE;

fail:
    free(geometry->fill.faces);
    geometry->fill.faces = NULL;
    geometry->fill.faces_size = 0;
    geometry->fill.face_count = 0;
    return FALSE;
}

static BOOL d2d_cdt_fixup(struct d2d_cdt *cdt, const struct d2d_cdt_edge_ref *base_edge)
{
    struct d2d_cdt_edge_ref *stack = NULL;
    size_t stack_count = 0, stack_capacity = 0;
    BOOL result = TRUE;

    if (!d2d_array_reserve((void **)&stack, &stack_capacity, 1, sizeof(*stack)))
    {
        ERR("Failed to allocate fixup stack.\n");
        return FALSE;
    }
    stack[stack_count++] = *base_edge;

    while (stack_count > 0)
    {
        struct d2d_cdt_edge_ref current_base = stack[--stack_count];
        struct d2d_cdt_edge_ref candidate, next, new_base;
        unsigned int count = 0;

        d2d_cdt_edge_next_left(cdt, &next, &current_base);
        if (next.idx == current_base.idx)
        {
            ERR("Degenerate face.\n");
            result = FALSE;
            break;
        }

        candidate = next;
        while (d2d_cdt_edge_destination(cdt, &next) != d2d_cdt_edge_origin(cdt, &current_base))
        {
            if (d2d_cdt_incircle(cdt, d2d_cdt_edge_origin(cdt, &current_base),
                    d2d_cdt_edge_destination(cdt, &current_base),
                    d2d_cdt_edge_destination(cdt, &candidate), d2d_cdt_edge_destination(cdt, &next)))
                candidate = next;
            d2d_cdt_edge_next_left(cdt, &next, &next);
            ++count;
        }

        if (count > 1)
        {
            d2d_cdt_edge_next_left(cdt, &next, &candidate);
            if (d2d_cdt_edge_destination(cdt, &next) == d2d_cdt_edge_origin(cdt, &current_base))
                d2d_cdt_edge_next_left(cdt, &next, &current_base);
            else
                next = current_base;
            if (!d2d_cdt_connect(cdt, &new_base, &candidate, &next))
            {
                result = FALSE;
                break;
            }
            if (!d2d_array_reserve((void **)&stack, &stack_capacity,
                    stack_count + 2, sizeof(*stack)))
            {
                ERR("Failed to grow fixup stack.\n");
                result = FALSE;
                break;
            }
            /* Push sym side first (processed second), then normal side (processed first). */
            d2d_cdt_edge_sym(&new_base, &new_base);
            stack[stack_count++] = new_base;
            d2d_cdt_edge_sym(&new_base, &new_base);
            stack[stack_count++] = new_base;
        }
    }

    free(stack);
    return result;
}

static void d2d_cdt_cut_edges(struct d2d_cdt *cdt, struct d2d_cdt_edge_ref *end_edge,
        const struct d2d_cdt_edge_ref *base_edge, size_t start_vertex, size_t end_vertex)
{
    struct d2d_cdt_edge_ref *destroy_stack = NULL;
    size_t destroy_count = 0, destroy_capacity = 0;
    struct d2d_cdt_edge_ref current = *base_edge;

    for (;;)
    {
        struct d2d_cdt_edge_ref next;
        float ccw;

        d2d_cdt_edge_next_left(cdt, &next, &current);
        if (d2d_cdt_edge_destination(cdt, &next) == end_vertex)
        {
            *end_edge = next;
            break;
        }

        ccw = d2d_cdt_ccw(cdt, d2d_cdt_edge_destination(cdt, &next), end_vertex, start_vertex);
        if (ccw == 0.0f)
        {
            *end_edge = next;
            break;
        }

        if (ccw > 0.0f)
            d2d_cdt_edge_next_left(cdt, &next, &next);

        d2d_cdt_edge_sym(&next, &next);

        if (!d2d_array_reserve((void **)&destroy_stack, &destroy_capacity,
                destroy_count + 1, sizeof(*destroy_stack)))
        {
            ERR("Failed to grow destroy stack.\n");
            break;
        }
        destroy_stack[destroy_count++] = next;
        current = next;
    }

    while (destroy_count > 0)
        d2d_cdt_destroy_edge(cdt, &destroy_stack[--destroy_count]);

    free(destroy_stack);
}

static BOOL d2d_cdt_insert_segment(struct d2d_cdt *cdt, struct d2d_geometry *geometry,
        const struct d2d_cdt_edge_ref *origin, struct d2d_cdt_edge_ref *edge, size_t end_vertex)
{
    struct d2d_cdt_edge_ref current_origin = *origin;
    size_t last_origin_vtx = ~(size_t)0;
    size_t collinear_steps = 0;

    for (;;)
    {
        struct d2d_cdt_edge_ref base_edge, current, new_origin, next, target;
        size_t current_destination, current_origin_vtx;

        for (current = current_origin;; current = next)
        {
            d2d_cdt_edge_next_origin(cdt, &next, &current);

            current_destination = d2d_cdt_edge_destination(cdt, &current);
            if (current_destination == end_vertex)
            {
                d2d_cdt_edge_sym(edge, &current);
                return TRUE;
            }

            current_origin_vtx = d2d_cdt_edge_origin(cdt, &current);
            if (d2d_cdt_ccw(cdt, end_vertex, current_origin_vtx, current_destination) == 0.0f
                    && (cdt->vertices[current_destination].x > cdt->vertices[current_origin_vtx].x)
                    == (cdt->vertices[end_vertex].x > cdt->vertices[current_origin_vtx].x)
                    && (cdt->vertices[current_destination].y > cdt->vertices[current_origin_vtx].y)
                    == (cdt->vertices[end_vertex].y > cdt->vertices[current_origin_vtx].y))
            {
                /* Cycle detection: if we revisit the same origin vertex via
                 * collinear edges, we are stuck in a loop. */
                if (current_destination == last_origin_vtx)
                {
                    static int once;
                    if (!once++)
                        FIXME("Collinear cycle detected, aborting.\n");
                    return FALSE;
                }
                last_origin_vtx = current_origin_vtx;
                if (++collinear_steps > cdt->edge_count)
                {
                    static int once2;
                    if (!once2++)
                        FIXME("Too many collinear steps (%lu), aborting.\n",
                            (unsigned long)collinear_steps);
                    return FALSE;
                }
                d2d_cdt_edge_sym(&new_origin, &current);
                current_origin = new_origin;
                goto next_segment;
            }

            if (d2d_cdt_rightof(cdt, end_vertex, &next) && d2d_cdt_leftof(cdt, end_vertex, &current))
            {
                d2d_cdt_edge_next_left(cdt, &base_edge, &current);

                d2d_cdt_edge_sym(&base_edge, &base_edge);
                d2d_cdt_cut_edges(cdt, &target, &base_edge,
                        d2d_cdt_edge_origin(cdt, &current_origin), end_vertex);
                d2d_cdt_destroy_edge(cdt, &base_edge);

                if (!d2d_cdt_connect(cdt, &base_edge, &target, &current))
                    return FALSE;
                *edge = base_edge;
                if (!d2d_cdt_fixup(cdt, &base_edge))
                    return FALSE;
                d2d_cdt_edge_sym(&base_edge, &base_edge);
                if (!d2d_cdt_fixup(cdt, &base_edge))
                    return FALSE;

                if (d2d_cdt_edge_origin(cdt, edge) == end_vertex)
                    return TRUE;
                current_origin = *edge;
                goto next_segment;
            }

            if (next.idx == current_origin.idx)
            {
                static int once;
                if (!once++)
                    FIXME("Triangle not found.\n");
                return FALSE;
            }
        }
    next_segment:
        continue;
    }
}

static BOOL d2d_cdt_insert_segments(struct d2d_cdt *cdt, struct d2d_geometry *geometry)
{
    size_t start_vertex, end_vertex, i, j, k;
    struct d2d_cdt_edge_ref edge, new_edge;
    const struct d2d_figure *figure;
    const D2D1_POINT_2F *p;
    BOOL found;

    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        figure = &geometry->u.path.figures[i];

        if (figure->flags & D2D_FIGURE_FLAG_HOLLOW)
            continue;

        /* Degenerate figure. */
        if (figure->vertex_count < 2)
            continue;

        p = bsearch(&figure->vertices[figure->vertex_count - 1], cdt->vertices,
                geometry->fill.vertex_count, sizeof(*p), d2d_cdt_compare_vertices);
        start_vertex = p - cdt->vertices;

        for (k = 0, found = FALSE; k < cdt->edge_count; ++k)
        {
            if (cdt->edges[k].flags & D2D_CDT_EDGE_FLAG_FREED)
                continue;

            edge.idx = k;
            edge.r = 0;

            if (d2d_cdt_edge_origin(cdt, &edge) == start_vertex)
            {
                found = TRUE;
                break;
            }
            d2d_cdt_edge_sym(&edge, &edge);
            if (d2d_cdt_edge_origin(cdt, &edge) == start_vertex)
            {
                found = TRUE;
                break;
            }
        }

        if (!found)
        {
            ERR("Edge not found.\n");
            return FALSE;
        }

        for (j = 0; j < figure->vertex_count; start_vertex = end_vertex, ++j)
        {
            p = bsearch(&figure->vertices[j], cdt->vertices,
                    geometry->fill.vertex_count, sizeof(*p), d2d_cdt_compare_vertices);
            end_vertex = p - cdt->vertices;

            if (start_vertex == end_vertex)
                continue;

            if (!d2d_cdt_insert_segment(cdt, geometry, &edge, &new_edge, end_vertex))
            {
                /* Skip this constraint edge rather than aborting the entire
                 * triangulation. Find an edge starting at end_vertex so we
                 * can continue with the next segment. */
                BOOL refound = FALSE;
                for (k = 0; k < cdt->edge_count; ++k)
                {
                    if (cdt->edges[k].flags & D2D_CDT_EDGE_FLAG_FREED)
                        continue;
                    edge.idx = k;
                    edge.r = 0;
                    if (d2d_cdt_edge_origin(cdt, &edge) == end_vertex)
                    {
                        refound = TRUE;
                        break;
                    }
                    d2d_cdt_edge_sym(&edge, &edge);
                    if (d2d_cdt_edge_origin(cdt, &edge) == end_vertex)
                    {
                        refound = TRUE;
                        break;
                    }
                }
                if (!refound)
                    return FALSE;
                continue;
            }
            edge = new_edge;
        }
    }

    return TRUE;
}

static BOOL d2d_geometry_intersections_add(struct d2d_geometry_intersections *i,
        const struct d2d_segment_idx *segment_idx, float t, D2D1_POINT_2F p)
{
    struct d2d_geometry_intersection *intersection;

    if (!d2d_array_reserve((void **)&i->intersections, &i->intersections_size,
            i->intersection_count + 1, sizeof(*i->intersections)))
    {
        ERR("Failed to grow intersections array.\n");
        return FALSE;
    }

    intersection = &i->intersections[i->intersection_count++];
    intersection->figure_idx = segment_idx->figure_idx;
    intersection->vertex_idx = segment_idx->vertex_idx;
    intersection->control_idx = segment_idx->control_idx;
    intersection->t = t;
    intersection->p = p;

    return TRUE;
}

static int __cdecl d2d_geometry_intersections_compare(const void *a, const void *b)
{
    const struct d2d_geometry_intersection *i0 = a;
    const struct d2d_geometry_intersection *i1 = b;

    if (i0->figure_idx != i1->figure_idx)
        return i0->figure_idx - i1->figure_idx;
    if (i0->vertex_idx != i1->vertex_idx)
        return i0->vertex_idx - i1->vertex_idx;
    if (i0->t != i1->t)
        return i0->t > i1->t ? 1 : -1;
    return 0;
}

static BOOL d2d_geometry_intersect_line_line(struct d2d_geometry *geometry,
        struct d2d_geometry_intersections *intersections, const struct d2d_segment_idx *idx_p,
        const struct d2d_segment_idx *idx_q)
{
    D2D1_POINT_2F v_p, v_q, v_qp, intersection;
    const D2D1_POINT_2F *p[2], *q[2];
    const struct d2d_figure *figure;
    float s, t, det;
    size_t next;

    figure = &geometry->u.path.figures[idx_p->figure_idx];
    p[0] = &figure->vertices[idx_p->vertex_idx];
    next = idx_p->vertex_idx + 1;
    if (next == figure->vertex_count)
        next = 0;
    p[1] = &figure->vertices[next];

    figure = &geometry->u.path.figures[idx_q->figure_idx];
    q[0] = &figure->vertices[idx_q->vertex_idx];
    next = idx_q->vertex_idx + 1;
    if (next == figure->vertex_count)
        next = 0;
    q[1] = &figure->vertices[next];

    d2d_point_subtract(&v_p, p[1], p[0]);
    d2d_point_subtract(&v_q, q[1], q[0]);
    d2d_point_subtract(&v_qp, p[0], q[0]);

    det = v_p.x * v_q.y - v_p.y * v_q.x;
    if (det == 0.0f)
        return TRUE;

    s = (v_q.x * v_qp.y - v_q.y * v_qp.x) / det;
    t = (v_p.x * v_qp.y - v_p.y * v_qp.x) / det;

    if (s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f)
        return TRUE;

    intersection.x = p[0]->x + v_p.x * s;
    intersection.y = p[0]->y + v_p.y * s;

    if (s > 0.0f && s < 1.0f && !d2d_geometry_intersections_add(intersections, idx_p, s, intersection))
        return FALSE;

    if (t > 0.0f && t < 1.0f && !d2d_geometry_intersections_add(intersections, idx_q, t, intersection))
        return FALSE;

    return TRUE;
}

static BOOL d2d_geometry_add_bezier_line_intersections(struct d2d_geometry *geometry,
        struct d2d_geometry_intersections *intersections, const struct d2d_segment_idx *idx_p,
        const D2D1_POINT_2F **p, const struct d2d_segment_idx *idx_q, const D2D1_POINT_2F **q, float s)
{
    D2D1_POINT_2F intersection;
    float t;

    d2d_point_calculate_bezier(&intersection, p[0], p[1], p[2], s);
    if (fabsf(q[1]->x - q[0]->x) > fabsf(q[1]->y - q[0]->y))
        t = (intersection.x - q[0]->x) / (q[1]->x - q[0]->x);
    else
        t = (intersection.y - q[0]->y) / (q[1]->y - q[0]->y);
    if (t < 0.0f || t > 1.0f)
        return TRUE;

    d2d_point_lerp(&intersection, q[0], q[1], t);

    if (s > 0.0f && s < 1.0f && !d2d_geometry_intersections_add(intersections, idx_p, s, intersection))
        return FALSE;

    if (t > 0.0f && t < 1.0f && !d2d_geometry_intersections_add(intersections, idx_q, t, intersection))
        return FALSE;

    return TRUE;
}

static BOOL d2d_geometry_intersect_bezier_line(struct d2d_geometry *geometry,
        struct d2d_geometry_intersections *intersections,
        const struct d2d_segment_idx *idx_p, const struct d2d_segment_idx *idx_q)
{
    const D2D1_POINT_2F *p[3], *q[2];
    const struct d2d_figure *figure;
    float y[3], root, theta, d, e;
    size_t next;

    figure = &geometry->u.path.figures[idx_p->figure_idx];
    p[0] = &figure->vertices[idx_p->vertex_idx];
    p[1] = &figure->bezier_controls[idx_p->control_idx];
    next = idx_p->vertex_idx + 1;
    p[2] = &figure->vertices[next];

    figure = &geometry->u.path.figures[idx_q->figure_idx];
    q[0] = &figure->vertices[idx_q->vertex_idx];
    next = idx_q->vertex_idx + 1;
    if (next == figure->vertex_count)
        next = 0;
    q[1] = &figure->vertices[next];

    /* Align the line with x-axis. */
    theta = -atan2f(q[1]->y - q[0]->y, q[1]->x - q[0]->x);
    y[0] = (p[0]->x - q[0]->x) * sinf(theta) + (p[0]->y - q[0]->y) * cosf(theta);
    y[1] = (p[1]->x - q[0]->x) * sinf(theta) + (p[1]->y - q[0]->y) * cosf(theta);
    y[2] = (p[2]->x - q[0]->x) * sinf(theta) + (p[2]->y - q[0]->y) * cosf(theta);

    /* Intersect the transformed curve with the x-axis.
     *
     * f(t) = (1 - t)²P₀ + 2(1 - t)tP₁ + t²P₂
     *      = (P₀ - 2P₁ + P₂)t² + 2(P₁ - P₀)t + P₀
     *
     * a = P₀ - 2P₁ + P₂
     * b = 2(P₁ - P₀)
     * c = P₀
     *
     * f(t) = 0
     * t = (-b ± √(b² - 4ac)) / 2a
     *   = (-2(P₁ - P₀) ± √((2(P₁ - P₀))² - 4((P₀ - 2P₁ + P₂)P₀))) / 2(P₀ - 2P₁ + P₂)
     *   = (2P₀ - 2P₁ ± √(4P₀² + 4P₁² - 8P₀P₁ - 4P₀² + 8P₀P₁ - 4P₀P₂)) / (2P₀ - 4P₁ + 2P₂)
     *   = (P₀ - P₁ ± √(P₁² - P₀P₂)) / (P₀ - 2P₁ + P₂) */

    d = y[0] - 2 * y[1] + y[2];
    if (d == 0.0f)
    {
        /* P₀ - 2P₁ + P₂ = 0
         * f(t) = (P₀ - 2P₁ + P₂)t² + 2(P₁ - P₀)t + P₀ = 0
         * t = -P₀ / 2(P₁ - P₀) */
        root = -y[0] / (2.0f * (y[1] - y[0]));
        if (root < 0.0f || root > 1.0f)
            return TRUE;

        return d2d_geometry_add_bezier_line_intersections(geometry, intersections, idx_p, p, idx_q, q, root);
    }

    e = y[1] * y[1] - y[0] * y[2];
    if (e < 0.0f)
        return TRUE;

    root = (y[0] - y[1] + sqrtf(e)) / d;
    if (root >= 0.0f && root <= 1.0f && !d2d_geometry_add_bezier_line_intersections(geometry,
            intersections, idx_p, p, idx_q, q, root))
        return FALSE;

    root = (y[0] - y[1] - sqrtf(e)) / d;
    if (root >= 0.0f && root <= 1.0f && !d2d_geometry_add_bezier_line_intersections(geometry,
            intersections, idx_p, p, idx_q, q, root))
        return FALSE;

    return TRUE;
}

/* Two quadratic segments describe the same curve, possibly traversed in
 * opposite directions. The tolerance is a fraction of a DIP well below anything
 * rasterisation resolves, scaled with the coordinate magnitude so that float
 * rounding in the caller's own conversion (a cubic converted twice into the
 * same quadratics) does not defeat the test. */
static BOOL d2d_point_approx_equal(const D2D1_POINT_2F *a, const D2D1_POINT_2F *b)
{
    float eps = 1e-4f * fmaxf(1.0f, fmaxf(fabsf(a->x), fabsf(a->y)));

    return fabsf(a->x - b->x) <= eps && fabsf(a->y - b->y) <= eps;
}

static BOOL d2d_bezier_segments_coincide(const D2D1_POINT_2F *const p[3], const D2D1_POINT_2F *const q[3])
{
    if (!d2d_point_approx_equal(p[1], q[1]))
        return FALSE;
    if (d2d_point_approx_equal(p[0], q[0]) && d2d_point_approx_equal(p[2], q[2]))
        return TRUE;
    return d2d_point_approx_equal(p[0], q[2]) && d2d_point_approx_equal(p[2], q[0]);
}

static BOOL d2d_geometry_intersect_bezier_bezier(struct d2d_geometry *geometry,
        struct d2d_geometry_intersections *intersections,
        const struct d2d_segment_idx *idx_p, float start_p, float end_p,
        const struct d2d_segment_idx *idx_q, float start_q, float end_q)
{
    const D2D1_POINT_2F *p[3], *q[3];
    const struct d2d_figure *figure;
    D2D_RECT_F p_bounds, q_bounds;
    D2D1_POINT_2F intersection;
    float centre_p, centre_q;
    size_t next;

    figure = &geometry->u.path.figures[idx_p->figure_idx];
    p[0] = &figure->vertices[idx_p->vertex_idx];
    p[1] = &figure->bezier_controls[idx_p->control_idx];
    next = idx_p->vertex_idx + 1;
    p[2] = &figure->vertices[next];

    figure = &geometry->u.path.figures[idx_q->figure_idx];
    q[0] = &figure->vertices[idx_q->vertex_idx];
    q[1] = &figure->bezier_controls[idx_q->control_idx];
    next = idx_q->vertex_idx + 1;
    q[2] = &figure->vertices[next];

    d2d_rect_get_bezier_segment_bounds(&p_bounds, p[0], p[1], p[2], start_p, end_p);
    d2d_rect_get_bezier_segment_bounds(&q_bounds, q[0], q[1], q[2], start_q, end_q);

    if (!d2d_rect_check_overlap(&p_bounds, &q_bounds))
        return TRUE;

    /* Coincident segments never cross, so there is no intersection to report,
     * just like d2d_geometry_intersect_line_line() reports none for parallel
     * lines. Subdividing them instead finds an overlap in all four sub-pairs on
     * every level, visits 4^10 leaves before the termination below and adds
     * about two million duplicate intersections, all but a thousand of which
     * d2d_geometry_apply_intersections() throws away again: a 13 DIP icon whose
     * figures share a stroke cap took over a second to close. */
    if (start_p == 0.0f && end_p == 1.0f && start_q == 0.0f && end_q == 1.0f
            && d2d_bezier_segments_coincide(p, q))
        return TRUE;

    centre_p = (start_p + end_p) / 2.0f;
    centre_q = (start_q + end_q) / 2.0f;

    if (end_p - start_p < 1e-3f)
    {
        d2d_point_calculate_bezier(&intersection, p[0], p[1], p[2], centre_p);
        if (start_p > 0.0f && end_p < 1.0f && !d2d_geometry_intersections_add(intersections,
                idx_p, centre_p, intersection))
            return FALSE;
        if (start_q > 0.0f && end_q < 1.0f && !d2d_geometry_intersections_add(intersections,
                idx_q, centre_q, intersection))
            return FALSE;
        return TRUE;
    }

    if (!d2d_geometry_intersect_bezier_bezier(geometry, intersections,
            idx_p, start_p, centre_p, idx_q, start_q, centre_q))
        return FALSE;
    if (!d2d_geometry_intersect_bezier_bezier(geometry, intersections,
            idx_p, start_p, centre_p, idx_q, centre_q, end_q))
        return FALSE;
    if (!d2d_geometry_intersect_bezier_bezier(geometry, intersections,
            idx_p, centre_p, end_p, idx_q, start_q, centre_q))
        return FALSE;
    if (!d2d_geometry_intersect_bezier_bezier(geometry, intersections,
            idx_p, centre_p, end_p, idx_q, centre_q, end_q))
        return FALSE;

    return TRUE;
}

static BOOL d2d_geometry_apply_intersections(struct d2d_geometry *geometry,
        struct d2d_geometry_intersections *intersections)
{
    size_t vertex_offset, control_offset, next, i;
    struct d2d_geometry_intersection *inter;
    enum d2d_vertex_type vertex_type;
    const D2D1_POINT_2F *p[3];
    struct d2d_figure *figure;
    D2D1_POINT_2F q[2];
    float t, t_prev;

    for (i = 0; i < intersections->intersection_count; ++i)
    {
        inter = &intersections->intersections[i];
        if (!i || inter->figure_idx != intersections->intersections[i - 1].figure_idx)
            vertex_offset = control_offset = 0;

        figure = &geometry->u.path.figures[inter->figure_idx];
        vertex_type = figure->vertex_types[inter->vertex_idx + vertex_offset];
        if (!d2d_vertex_type_is_bezier(vertex_type))
        {
            if (!d2d_figure_insert_vertex(&geometry->u.path.figures[inter->figure_idx],
                    inter->vertex_idx + vertex_offset + 1, inter->p))
                return FALSE;
            ++vertex_offset;
            continue;
        }

        t = inter->t;
        if (i && inter->figure_idx == intersections->intersections[i - 1].figure_idx
                && inter->vertex_idx == intersections->intersections[i - 1].vertex_idx)
        {
            t_prev = intersections->intersections[i - 1].t;
            if (t - t_prev < 1e-3f)
            {
                inter->t = intersections->intersections[i - 1].t;
                continue;
            }
            t = (t - t_prev) / (1.0f - t_prev);
        }

        p[0] = &figure->vertices[inter->vertex_idx + vertex_offset];
        p[1] = &figure->bezier_controls[inter->control_idx + control_offset];
        next = inter->vertex_idx + vertex_offset + 1;
        p[2] = &figure->vertices[next];

        d2d_point_lerp(&q[0], p[0], p[1], t);
        d2d_point_lerp(&q[1], p[1], p[2], t);

        figure->bezier_controls[inter->control_idx + control_offset] = q[0];
        if (!(d2d_figure_insert_bezier_controls(figure, inter->control_idx + control_offset + 1, 1, &q[1])))
            return FALSE;
        ++control_offset;

        if (!(d2d_figure_insert_vertex(figure, inter->vertex_idx + vertex_offset + 1, inter->p)))
            return FALSE;
        figure->vertex_types[inter->vertex_idx + vertex_offset + 1] = D2D_VERTEX_TYPE_SPLIT_BEZIER;
        ++vertex_offset;
    }

    return TRUE;
}


/* ---- Grid-accelerated self-intersection infrastructure (Phase 1) ---- */

struct d2d_segment_desc
{
    struct d2d_segment_idx idx;
    enum d2d_vertex_type type;
    D2D_RECT_F bounds;
    size_t flat_idx;  /* global segment index across all figures */
};

struct d2d_grid_entry
{
    size_t segment_idx;  /* index into segment_descs[] */
    size_t next;         /* next entry in this cell, SIZE_MAX = end */
};

struct d2d_grid
{
    size_t *cells;                    /* cells[row * cols + col] = first grid_entry index */
    struct d2d_grid_entry *entries;
    size_t entry_count;
    size_t entry_capacity;
    size_t cols, rows;
    float cell_w, cell_h;
    float origin_x, origin_y;
};

static void d2d_segment_get_line_bounds(D2D_RECT_F *bounds,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1)
{
    bounds->left   = min(p0->x, p1->x);
    bounds->top    = min(p0->y, p1->y);
    bounds->right  = max(p0->x, p1->x);
    bounds->bottom = max(p0->y, p1->y);
}

static BOOL d2d_grid_init(struct d2d_grid *grid, const D2D_RECT_F *total_bounds,
        size_t segment_count)
{
    float width, height, aspect;
    size_t grid_size, i;

    width  = total_bounds->right - total_bounds->left;
    height = total_bounds->bottom - total_bounds->top;

    if (width < 1e-6f) width = 1.0f;
    if (height < 1e-6f) height = 1.0f;

    /* Target ~4 segments per cell on average.
     * cell_count ≈ segment_count / 4, distributed by aspect ratio. */
    if (segment_count < 16)
    {
        grid->cols = 1;
        grid->rows = 1;
    }
    else
    {
        float target_cells = (float)segment_count / 4.0f;
        aspect = width / height;
        grid->cols = (size_t)(sqrtf(target_cells * aspect) + 0.5f);
        grid->rows = (size_t)(sqrtf(target_cells / aspect) + 0.5f);
        if (grid->cols < 1) grid->cols = 1;
        if (grid->rows < 1) grid->rows = 1;
        if (grid->cols > 256) grid->cols = 256;
        if (grid->rows > 256) grid->rows = 256;
    }

    grid->cell_w = width / (float)grid->cols;
    grid->cell_h = height / (float)grid->rows;
    grid->origin_x = total_bounds->left;
    grid->origin_y = total_bounds->top;

    grid_size = grid->cols * grid->rows;
    grid->cells = calloc(grid_size, sizeof(*grid->cells));
    if (!grid->cells)
        return FALSE;
    for (i = 0; i < grid_size; ++i)
        grid->cells[i] = SIZE_MAX;

    /* Allocate entry pool: each segment typically spans 1-4 cells.
     * We use 8x as generous upper bound and grow if needed. */
    grid->entry_capacity = segment_count * 8;
    if (grid->entry_capacity < 64)
        grid->entry_capacity = 64;
    grid->entries = calloc(grid->entry_capacity, sizeof(*grid->entries));
    if (!grid->entries)
    {
        free(grid->cells);
        grid->cells = NULL;
        return FALSE;
    }
    grid->entry_count = 0;

    return TRUE;
}

static BOOL d2d_grid_insert(struct d2d_grid *grid, size_t segment_idx,
        const D2D_RECT_F *bounds)
{
    size_t cx0, cy0, cx1, cy1, cx, cy, cell_idx;
    double fx0, fy0, fx1, fy1;
    struct d2d_grid_entry *entry;

    /* Map AABB to grid cell range. Compute as signed doubles first: a slightly
     * negative coordinate (float drift at the min edge) must clamp to cell 0,
     * not wrap to a huge size_t (which the upper-bound clamp below would then
     * fold to cols-1/rows-1, mis-bucketing boundary-heavy inputs). */
    fx0 = (bounds->left   - grid->origin_x) / grid->cell_w;
    fy0 = (bounds->top    - grid->origin_y) / grid->cell_h;
    fx1 = (bounds->right  - grid->origin_x) / grid->cell_w;
    fy1 = (bounds->bottom - grid->origin_y) / grid->cell_h;

    cx0 = fx0 < 0.0 ? 0 : (size_t)fx0;
    cy0 = fy0 < 0.0 ? 0 : (size_t)fy0;
    cx1 = fx1 < 0.0 ? 0 : (size_t)fx1;
    cy1 = fy1 < 0.0 ? 0 : (size_t)fy1;

    /* Clamp the upper bound to grid extents (handles floating-point edge cases) */
    if (cx0 >= grid->cols) cx0 = grid->cols - 1;
    if (cy0 >= grid->rows) cy0 = grid->rows - 1;
    if (cx1 >= grid->cols) cx1 = grid->cols - 1;
    if (cy1 >= grid->rows) cy1 = grid->rows - 1;

    for (cy = cy0; cy <= cy1; ++cy)
    {
        for (cx = cx0; cx <= cx1; ++cx)
        {
            /* Grow entry pool if needed */
            if (grid->entry_count >= grid->entry_capacity)
            {
                size_t new_cap = grid->entry_capacity * 2;
                struct d2d_grid_entry *new_entries = realloc(grid->entries,
                        new_cap * sizeof(*new_entries));
                if (!new_entries)
                    return FALSE;
                grid->entries = new_entries;
                grid->entry_capacity = new_cap;
            }

            entry = &grid->entries[grid->entry_count];
            entry->segment_idx = segment_idx;
            cell_idx = cy * grid->cols + cx;
            entry->next = grid->cells[cell_idx];
            grid->cells[cell_idx] = grid->entry_count;
            ++grid->entry_count;
        }
    }

    return TRUE;
}

static void d2d_grid_destroy(struct d2d_grid *grid)
{
    free(grid->cells);
    free(grid->entries);
    memset(grid, 0, sizeof(*grid));
}

static BOOL d2d_segments_adjacent(const struct d2d_segment_desc *a,
        const struct d2d_segment_desc *b, const struct d2d_geometry *geometry)
{
    size_t diff, n;

    if (a->idx.figure_idx != b->idx.figure_idx)
        return FALSE;

    n = geometry->u.path.figures[a->idx.figure_idx].vertex_count;
    if (a->idx.vertex_idx > b->idx.vertex_idx)
        diff = a->idx.vertex_idx - b->idx.vertex_idx;
    else
        diff = b->idx.vertex_idx - a->idx.vertex_idx;

    return (diff == 1 || diff == n - 1);
}


/* Intersect the geometry's segments with themselves. For small segment
 * counts (< 16), use the straightforward O(n^2) approach. For larger
 * geometries, build a uniform spatial grid and only test segment pairs
 * that share at least one grid cell. */
static BOOL d2d_geometry_intersect_self(struct d2d_geometry *geometry)
{
    struct d2d_geometry_intersections intersections = {0};
    const struct d2d_figure *figure_p, *figure_q;
    struct d2d_segment_idx idx_p, idx_q;
    enum d2d_vertex_type type_p, type_q;
    BOOL ret = FALSE;
    size_t max_q;

    struct d2d_segment_desc *segment_descs = NULL;
    size_t segment_count = 0, segment_capacity = 0;
    struct d2d_grid grid = {0};
    D2D_RECT_F total_bounds;
    BOOL use_grid = FALSE;
    BOOL *tested = NULL;

    if (!geometry->u.path.figure_count)
        return TRUE;

    /* --- Step 1: Count segments and decide whether to use grid --- */
    {
        size_t fi, vi;
        for (fi = 0; fi < geometry->u.path.figure_count; ++fi)
        {
            const struct d2d_figure *fig = &geometry->u.path.figures[fi];
            for (vi = 0; vi < fig->vertex_count; ++vi)
            {
                if (fig->vertex_types[vi] != D2D_VERTEX_TYPE_END)
                    ++segment_capacity;
            }
        }
    }

    if (segment_capacity >= 16)
    {
        /* --- Step 2: Build segment descriptors with per-segment AABBs --- */
        segment_descs = calloc(segment_capacity, sizeof(*segment_descs));
        if (segment_descs)
        {
            size_t fi, vi, ci;

            total_bounds.left = FLT_MAX;
            total_bounds.top = FLT_MAX;
            total_bounds.right = -FLT_MAX;
            total_bounds.bottom = -FLT_MAX;

            for (fi = 0; fi < geometry->u.path.figure_count; ++fi)
            {
                const struct d2d_figure *fig = &geometry->u.path.figures[fi];
                ci = 0;
                for (vi = 0; vi < fig->vertex_count; ++vi)
                {
                    enum d2d_vertex_type vtype = fig->vertex_types[vi];
                    if (vtype == D2D_VERTEX_TYPE_END)
                        continue;

                    if (segment_count < segment_capacity)
                    {
                        struct d2d_segment_desc *desc = &segment_descs[segment_count];
                        size_t next_vi = vi + 1;

                        desc->idx.figure_idx = fi;
                        desc->idx.vertex_idx = vi;
                        desc->idx.control_idx = ci;
                        desc->type = vtype;
                        desc->flat_idx = segment_count;

                        if (d2d_vertex_type_is_bezier(vtype))
                        {
                            d2d_rect_get_bezier_bounds(&desc->bounds,
                                    &fig->vertices[vi],
                                    &fig->bezier_controls[ci],
                                    &fig->vertices[next_vi]);
                        }
                        else
                        {
                            if (next_vi >= fig->vertex_count)
                                next_vi = 0;
                            d2d_segment_get_line_bounds(&desc->bounds,
                                    &fig->vertices[vi],
                                    &fig->vertices[next_vi]);
                        }

                        d2d_rect_union(&total_bounds, &desc->bounds);
                        ++segment_count;
                    }

                    if (d2d_vertex_type_is_bezier(vtype))
                        ++ci;
                }
            }

            /* --- Step 3: Build spatial grid --- */
            if (segment_count >= 16 && d2d_grid_init(&grid, &total_bounds, segment_count))
            {
                size_t si;
                use_grid = TRUE;
                for (si = 0; si < segment_count; ++si)
                {
                    if (!d2d_grid_insert(&grid, si, &segment_descs[si].bounds))
                    {
                        use_grid = FALSE;
                        break;
                    }
                }
            }

            /* Allocate tested-array for duplicate avoidance */
            if (use_grid)
            {
                tested = calloc(segment_count, sizeof(*tested));
                if (!tested)
                    use_grid = FALSE;
            }
        }
    }

    if (use_grid)
    {
        /* --- Grid-accelerated intersection loop --- */
        size_t i, j;

        for (i = 0; i < segment_count; ++i)
        {
            const struct d2d_segment_desc *desc_i = &segment_descs[i];
            size_t cx0, cy0, cx1, cy1, cx, cy;

            /* Clear tested flags for this outer iteration.
             * Only clear entries that were actually set in previous iteration
             * would be faster, but memset on segment_count bytes is cheap. */
            memset(tested, 0, segment_count * sizeof(*tested));

            /* Compute grid cell range for segment i */
            cx0 = (size_t)((desc_i->bounds.left   - grid.origin_x) / grid.cell_w);
            cy0 = (size_t)((desc_i->bounds.top    - grid.origin_y) / grid.cell_h);
            cx1 = (size_t)((desc_i->bounds.right  - grid.origin_x) / grid.cell_w);
            cy1 = (size_t)((desc_i->bounds.bottom - grid.origin_y) / grid.cell_h);
            if (cx0 >= grid.cols) cx0 = grid.cols - 1;
            if (cy0 >= grid.rows) cy0 = grid.rows - 1;
            if (cx1 >= grid.cols) cx1 = grid.cols - 1;
            if (cy1 >= grid.rows) cy1 = grid.rows - 1;

            for (cy = cy0; cy <= cy1; ++cy)
            {
                for (cx = cx0; cx <= cx1; ++cx)
                {
                    size_t ei = grid.cells[cy * grid.cols + cx];

                    while (ei != SIZE_MAX)
                    {
                        const struct d2d_grid_entry *entry = &grid.entries[ei];
                        j = entry->segment_idx;
                        ei = entry->next;

                        /* Only test j < i to avoid duplicates (like original) */
                        if (j >= i)
                            continue;

                        /* Skip if already tested via another shared cell */
                        if (tested[j])
                            continue;
                        tested[j] = TRUE;

                        /* Skip adjacent segments (share a vertex — endpoint
                         * intersections are filtered by s/t strictly in (0,1)) */
                        if (d2d_segments_adjacent(desc_i, &segment_descs[j], geometry))
                            continue;

                        /* Fine-grained AABB overlap check */
                        if (!d2d_rect_check_overlap(&desc_i->bounds,
                                &segment_descs[j].bounds))
                            continue;

                        /* Perform actual intersection test */
                        {
                            const struct d2d_segment_desc *desc_j = &segment_descs[j];

                            if (d2d_vertex_type_is_bezier(desc_j->type))
                            {
                                if (d2d_vertex_type_is_bezier(desc_i->type))
                                {
                                    if (!d2d_geometry_intersect_bezier_bezier(geometry,
                                            &intersections, &desc_i->idx, 0.0f, 1.0f,
                                            &desc_j->idx, 0.0f, 1.0f))
                                        goto done;
                                }
                                else
                                {
                                    if (!d2d_geometry_intersect_bezier_line(geometry,
                                            &intersections, &desc_j->idx, &desc_i->idx))
                                        goto done;
                                }
                            }
                            else
                            {
                                if (d2d_vertex_type_is_bezier(desc_i->type))
                                {
                                    if (!d2d_geometry_intersect_bezier_line(geometry,
                                            &intersections, &desc_i->idx, &desc_j->idx))
                                        goto done;
                                }
                                else
                                {
                                    if (!d2d_geometry_intersect_line_line(geometry,
                                            &intersections, &desc_i->idx, &desc_j->idx))
                                        goto done;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        /* --- Original brute-force loop for small geometries --- */
        for (idx_p.figure_idx = 0; idx_p.figure_idx < geometry->u.path.figure_count;
                ++idx_p.figure_idx)
        {
            figure_p = &geometry->u.path.figures[idx_p.figure_idx];
            idx_p.control_idx = 0;
            for (idx_p.vertex_idx = 0; idx_p.vertex_idx < figure_p->vertex_count;
                    ++idx_p.vertex_idx)
            {
                if ((type_p = figure_p->vertex_types[idx_p.vertex_idx]) == D2D_VERTEX_TYPE_END)
                    continue;

                for (idx_q.figure_idx = 0; idx_q.figure_idx <= idx_p.figure_idx;
                        ++idx_q.figure_idx)
                {
                    figure_q = &geometry->u.path.figures[idx_q.figure_idx];
                    if (idx_q.figure_idx != idx_p.figure_idx)
                    {
                        if (!d2d_rect_check_overlap(&figure_p->bounds, &figure_q->bounds))
                            continue;
                        if ((max_q = figure_q->vertex_count)
                                && figure_q->vertex_types[max_q - 1] == D2D_VERTEX_TYPE_END)
                            --max_q;
                    }
                    else
                    {
                        max_q = idx_p.vertex_idx;
                    }

                    idx_q.control_idx = 0;
                    for (idx_q.vertex_idx = 0; idx_q.vertex_idx < max_q; ++idx_q.vertex_idx)
                    {
                        type_q = figure_q->vertex_types[idx_q.vertex_idx];
                        if (d2d_vertex_type_is_bezier(type_q))
                        {
                            if (d2d_vertex_type_is_bezier(type_p))
                            {
                                if (!d2d_geometry_intersect_bezier_bezier(geometry,
                                        &intersections, &idx_p, 0.0f, 1.0f,
                                        &idx_q, 0.0f, 1.0f))
                                    goto done;
                            }
                            else
                            {
                                if (!d2d_geometry_intersect_bezier_line(geometry,
                                        &intersections, &idx_q, &idx_p))
                                    goto done;
                            }
                            ++idx_q.control_idx;
                        }
                        else
                        {
                            if (d2d_vertex_type_is_bezier(type_p))
                            {
                                if (!d2d_geometry_intersect_bezier_line(geometry,
                                        &intersections, &idx_p, &idx_q))
                                    goto done;
                            }
                            else
                            {
                                if (!d2d_geometry_intersect_line_line(geometry,
                                        &intersections, &idx_p, &idx_q))
                                    goto done;
                            }
                        }
                    }
                }
                if (d2d_vertex_type_is_bezier(type_p))
                    ++idx_p.control_idx;
            }
        }
    }

    qsort(intersections.intersections, intersections.intersection_count,
            sizeof(*intersections.intersections), d2d_geometry_intersections_compare);
    ret = d2d_geometry_apply_intersections(geometry, &intersections);

done:
    free(tested);
    d2d_grid_destroy(&grid);
    free(segment_descs);
    free(intersections.intersections);
    return ret;
}

static HRESULT d2d_path_geometry_triangulate(struct d2d_geometry *geometry)
{
    struct d2d_cdt_edge_ref left_edge, right_edge;
    size_t vertex_count, i, j;
    struct d2d_cdt cdt = {0};
    D2D1_POINT_2F *vertices;
#ifdef __i386__
    unsigned int control_word_x87, mask = 0;
#endif

    for (i = 0, vertex_count = 0; i < geometry->u.path.figure_count; ++i)
    {
        if (geometry->u.path.figures[i].flags & D2D_FIGURE_FLAG_HOLLOW)
            continue;
        vertex_count += geometry->u.path.figures[i].vertex_count;
    }

    if (vertex_count < 3)
    {
        WARN("Geometry has %lu vertices.\n", (long)vertex_count);
        return S_OK;
    }

    if (!(vertices = calloc(vertex_count, sizeof(*vertices))))
        return E_OUTOFMEMORY;

    for (i = 0, j = 0; i < geometry->u.path.figure_count; ++i)
    {
        if (geometry->u.path.figures[i].flags & D2D_FIGURE_FLAG_HOLLOW)
            continue;
        memcpy(&vertices[j], geometry->u.path.figures[i].vertices,
                geometry->u.path.figures[i].vertex_count * sizeof(*vertices));
        j += geometry->u.path.figures[i].vertex_count;
    }

    /* Sort vertices, eliminate duplicates. */
    qsort(vertices, vertex_count, sizeof(*vertices), d2d_cdt_compare_vertices);
    for (i = 1; i < vertex_count; ++i)
    {
        if (!memcmp(&vertices[i - 1], &vertices[i], sizeof(*vertices)))
        {
            --vertex_count;
            memmove(&vertices[i], &vertices[i + 1], (vertex_count - i) * sizeof(*vertices));
            --i;
        }
    }

    if (vertex_count < 3)
    {
        WARN("Geometry has %lu vertices after eliminating duplicates.\n", (long)vertex_count);
        free(vertices);
        return S_OK;
    }

    geometry->fill.vertices = vertices;
    geometry->fill.vertex_count = vertex_count;

    cdt.free_edge = ~0u;
    cdt.vertices = vertices;

#ifdef __i386__
    control_word_x87 = _controlfp(0, 0);
    _controlfp(_PC_24, mask = _MCW_PC);
#endif
    if (!d2d_cdt_triangulate(&cdt, 0, vertex_count, &left_edge, &right_edge))
        goto fail;
    if (!d2d_cdt_insert_segments(&cdt, geometry))
        goto fail;
#ifdef __i386__
    _controlfp(control_word_x87, _MCW_PC);
    mask = 0;
#endif

    if (!d2d_cdt_generate_faces(&cdt, geometry))
        goto fail;

    free(cdt.edges);
    return S_OK;

fail:
    geometry->fill.vertices = NULL;
    geometry->fill.vertex_count = 0;
    free(vertices);
    free(cdt.edges);
#ifdef __i386__
    if (mask) _controlfp(control_word_x87, mask);
#endif
    return E_FAIL;
}

static BOOL d2d_path_geometry_add_figure(struct d2d_geometry *geometry)
{
    struct d2d_figure *figure;

    if (!d2d_array_reserve((void **)&geometry->u.path.figures, &geometry->u.path.figures_size,
            geometry->u.path.figure_count + 1, sizeof(*geometry->u.path.figures)))
    {
        ERR("Failed to grow figures array.\n");
        return FALSE;
    }

    figure = &geometry->u.path.figures[geometry->u.path.figure_count];
    memset(figure, 0, sizeof(*figure));
    figure->bounds.left = FLT_MAX;
    figure->bounds.top = FLT_MAX;
    figure->bounds.right = -FLT_MAX;
    figure->bounds.bottom = -FLT_MAX;

    ++geometry->u.path.figure_count;
    return TRUE;
}

static BOOL d2d_geometry_outline_add_join(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *prev, const D2D1_POINT_2F *p0, const D2D1_POINT_2F *next)
{
    static const D2D1_POINT_2F origin = {0.0f, 0.0f};
    struct d2d_outline_vertex *v;
    struct d2d_face *f;
    size_t base_idx;
    float ccw;

    if (!d2d_array_reserve((void **)&geometry->outline.vertices, &geometry->outline.vertices_size,
            geometry->outline.vertex_count + 4, sizeof(*geometry->outline.vertices)))
    {
        ERR("Failed to grow outline vertices array.\n");
        return FALSE;
    }
    base_idx = geometry->outline.vertex_count;
    v = &geometry->outline.vertices[base_idx];

    if (!d2d_array_reserve((void **)&geometry->outline.faces, &geometry->outline.faces_size,
            geometry->outline.face_count + 2, sizeof(*geometry->outline.faces)))
    {
        ERR("Failed to grow outline faces array.\n");
        return FALSE;
    }
    f = &geometry->outline.faces[geometry->outline.face_count];

    ccw = d2d_point_ccw(&origin, prev, next);
    if (ccw == 0.0f)
    {
        /* Collinear case: prev and next are parallel. Check if same direction
         * (straight through) or opposite direction (U-turn/hairpin).
         * In both cases, keep all vertices at p0 — the vertex shader computes
         * the actual stroke-width offset. The previous code offset v[2]/v[3]
         * by a hardcoded 25.0f in geometry space, creating visible spikes. */
        float dot = prev->x * next->x + prev->y * next->y;
        if (dot >= 0.0f)
        {
            /* Same direction: the two segments continue straight.
             * Create a degenerate join (zero area) since no visible join is needed. */
            d2d_outline_vertex_set(&v[0], p0->x, p0->y, -prev->x, -prev->y, -prev->x, -prev->y);
            d2d_outline_vertex_set(&v[1], p0->x, p0->y,  prev->x,  prev->y,  prev->x,  prev->y);
            d2d_outline_vertex_set(&v[2], p0->x, p0->y,  prev->x,  prev->y,  prev->x,  prev->y);
            d2d_outline_vertex_set(&v[3], p0->x, p0->y, -prev->x, -prev->y, -prev->x, -prev->y);
        }
        else
        {
            /* U-turn (hairpin): segments go in opposite directions.
             * Create a flat end cap by using perpendicular directions. */
            float perp_x = -prev->y, perp_y = prev->x;
            d2d_outline_vertex_set(&v[0], p0->x, p0->y,  perp_x,  perp_y, -prev->x, -prev->y);
            d2d_outline_vertex_set(&v[1], p0->x, p0->y, -perp_x, -perp_y, -perp_x, -perp_y);
            d2d_outline_vertex_set(&v[2], p0->x, p0->y, -perp_x, -perp_y,  prev->x,  prev->y);
            d2d_outline_vertex_set(&v[3], p0->x, p0->y,  prev->x,  prev->y,  prev->x,  prev->y);
        }
    }
    else if (ccw < 0.0f)
    {
        d2d_outline_vertex_set(&v[0], p0->x, p0->y,  next->x,  next->y, -prev->x, -prev->y);
        d2d_outline_vertex_set(&v[1], p0->x, p0->y, -next->x, -next->y, -next->x, -next->y);
        d2d_outline_vertex_set(&v[2], p0->x, p0->y, -next->x, -next->y,  prev->x,  prev->y);
        d2d_outline_vertex_set(&v[3], p0->x, p0->y,  prev->x,  prev->y,  prev->x,  prev->y);
    }
    else
    {
        d2d_outline_vertex_set(&v[0], p0->x, p0->y,  prev->x,  prev->y, -next->x, -next->y);
        d2d_outline_vertex_set(&v[1], p0->x, p0->y, -prev->x, -prev->y, -prev->x, -prev->y);
        d2d_outline_vertex_set(&v[2], p0->x, p0->y, -prev->x, -prev->y,  next->x,  next->y);
        d2d_outline_vertex_set(&v[3], p0->x, p0->y,  next->x,  next->y,  next->x,  next->y);
    }
    geometry->outline.vertex_count += 4;

    d2d_face_set(&f[0], base_idx + 1, base_idx + 0, base_idx + 2);
    d2d_face_set(&f[1], base_idx + 2, base_idx + 0, base_idx + 3);
    geometry->outline.face_count += 2;

    return TRUE;
}

static BOOL d2d_geometry_outline_add_line_segment(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *next)
{
    struct d2d_outline_vertex *v;
    D2D1_POINT_2F q_next;
    struct d2d_face *f;
    size_t base_idx;

    if (!d2d_array_reserve((void **)&geometry->outline.vertices, &geometry->outline.vertices_size,
            geometry->outline.vertex_count + 4, sizeof(*geometry->outline.vertices)))
    {
        ERR("Failed to grow outline vertices array.\n");
        return FALSE;
    }
    base_idx = geometry->outline.vertex_count;
    v = &geometry->outline.vertices[base_idx];

    if (!d2d_array_reserve((void **)&geometry->outline.faces, &geometry->outline.faces_size,
            geometry->outline.face_count + 2, sizeof(*geometry->outline.faces)))
    {
        ERR("Failed to grow outline faces array.\n");
        return FALSE;
    }
    f = &geometry->outline.faces[geometry->outline.face_count];

    d2d_point_subtract(&q_next, next, p0);
    d2d_point_normalise(&q_next);

    d2d_outline_vertex_set(&v[0], p0->x, p0->y,  q_next.x,  q_next.y,  q_next.x,  q_next.y);
    d2d_outline_vertex_set(&v[1], p0->x, p0->y, -2.0f * q_next.x, -2.0f * q_next.y, -2.0f * q_next.x, -2.0f * q_next.y);
    d2d_outline_vertex_set(&v[2], next->x, next->y,  q_next.x,  q_next.y,  q_next.x,  q_next.y);
    d2d_outline_vertex_set(&v[3], next->x, next->y, -2.0f * q_next.x, -2.0f * q_next.y, -2.0f * q_next.x, -2.0f * q_next.y);
    geometry->outline.vertex_count += 4;

    d2d_face_set(&f[0], base_idx + 0, base_idx + 1, base_idx + 2);
    d2d_face_set(&f[1], base_idx + 2, base_idx + 1, base_idx + 3);
    geometry->outline.face_count += 2;

    return TRUE;
}

static BOOL d2d_geometry_outline_add_bezier_segment(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2)
{
    struct d2d_curve_outline_vertex *b;
    D2D1_POINT_2F r0, r1, r2;
    D2D1_POINT_2F q0, q1, q2;
    struct d2d_face *f;
    size_t base_idx;

    if (!d2d_array_reserve((void **)&geometry->outline.beziers, &geometry->outline.beziers_size,
            geometry->outline.bezier_count + 7, sizeof(*geometry->outline.beziers)))
    {
        ERR("Failed to grow outline beziers array.\n");
        return FALSE;
    }
    base_idx = geometry->outline.bezier_count;
    b = &geometry->outline.beziers[base_idx];

    if (!d2d_array_reserve((void **)&geometry->outline.bezier_faces, &geometry->outline.bezier_faces_size,
            geometry->outline.bezier_face_count + 5, sizeof(*geometry->outline.bezier_faces)))
    {
        ERR("Failed to grow outline faces array.\n");
        return FALSE;
    }
    f = &geometry->outline.bezier_faces[geometry->outline.bezier_face_count];

    d2d_point_lerp(&q0, p0, p1, 0.5f);
    d2d_point_lerp(&q1, p1, p2, 0.5f);
    d2d_point_lerp(&q2, &q0, &q1, 0.5f);

    d2d_point_subtract(&r0, &q0, p0);
    d2d_point_subtract(&r1, &q1, &q0);
    d2d_point_subtract(&r2, p2, &q1);

    d2d_point_normalise(&r0);
    d2d_point_normalise(&r1);
    d2d_point_normalise(&r2);

    if (d2d_point_ccw(p0, p1, p2) > 0.0f)
    {
        d2d_point_scale(&r0, -1.0f);
        d2d_point_scale(&r1, -1.0f);
        d2d_point_scale(&r2, -1.0f);
    }

    d2d_curve_outline_vertex_set(&b[0],  p0, p0, p1, p2,  r0.x,  r0.y,  r0.x,  r0.y);
    d2d_curve_outline_vertex_set(&b[1],  p0, p0, p1, p2, -r0.x, -r0.y, -r0.x, -r0.y);
    d2d_curve_outline_vertex_set(&b[2], &q0, p0, p1, p2,  r0.x,  r0.y,  r1.x,  r1.y);
    d2d_curve_outline_vertex_set(&b[3], &q2, p0, p1, p2, -r1.x, -r1.y, -r1.x, -r1.y);
    d2d_curve_outline_vertex_set(&b[4], &q1, p0, p1, p2,  r1.x,  r1.y,  r2.x,  r2.y);
    d2d_curve_outline_vertex_set(&b[5],  p2, p0, p1, p2, -r2.x, -r2.y, -r2.x, -r2.y);
    d2d_curve_outline_vertex_set(&b[6],  p2, p0, p1, p2,  r2.x,  r2.y,  r2.x,  r2.y);
    geometry->outline.bezier_count += 7;

    d2d_face_set(&f[0], base_idx + 0, base_idx + 1, base_idx + 2);
    d2d_face_set(&f[1], base_idx + 2, base_idx + 1, base_idx + 3);
    d2d_face_set(&f[2], base_idx + 3, base_idx + 4, base_idx + 2);
    d2d_face_set(&f[3], base_idx + 5, base_idx + 4, base_idx + 3);
    d2d_face_set(&f[4], base_idx + 5, base_idx + 6, base_idx + 4);
    geometry->outline.bezier_face_count += 5;

    return TRUE;
}

static BOOL d2d_geometry_outline_add_arc_quadrant(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2)
{
    struct d2d_curve_outline_vertex *a;
    D2D1_POINT_2F r0, r1;
    struct d2d_face *f;
    size_t base_idx;

    if (!d2d_array_reserve((void **)&geometry->outline.arcs, &geometry->outline.arcs_size,
            geometry->outline.arc_count + 5, sizeof(*geometry->outline.arcs)))
    {
        ERR("Failed to grow outline arcs array.\n");
        return FALSE;
    }
    base_idx = geometry->outline.arc_count;
    a = &geometry->outline.arcs[base_idx];

    if (!d2d_array_reserve((void **)&geometry->outline.arc_faces, &geometry->outline.arc_faces_size,
            geometry->outline.arc_face_count + 3, sizeof(*geometry->outline.arc_faces)))
    {
        ERR("Failed to grow outline faces array.\n");
        return FALSE;
    }
    f = &geometry->outline.arc_faces[geometry->outline.arc_face_count];

    d2d_point_subtract(&r0, p1, p0);
    d2d_point_subtract(&r1, p2, p1);

    d2d_point_normalise(&r0);
    d2d_point_normalise(&r1);

    if (d2d_point_ccw(p0, p1, p2) > 0.0f)
    {
        d2d_point_scale(&r0, -1.0f);
        d2d_point_scale(&r1, -1.0f);
    }

    d2d_curve_outline_vertex_set(&a[0],  p0, p0, p1, p2,  r0.x,  r0.y,  r0.x,  r0.y);
    d2d_curve_outline_vertex_set(&a[1],  p0, p0, p1, p2, -r0.x, -r0.y, -r0.x, -r0.y);
    d2d_curve_outline_vertex_set(&a[2],  p1, p0, p1, p2,  r0.x,  r0.y,  r1.x,  r1.y);
    d2d_curve_outline_vertex_set(&a[3],  p2, p0, p1, p2, -r1.x, -r1.y, -r1.x, -r1.y);
    d2d_curve_outline_vertex_set(&a[4],  p2, p0, p1, p2,  r1.x,  r1.y,  r1.x,  r1.y);
    geometry->outline.arc_count += 5;

    d2d_face_set(&f[0], base_idx + 0, base_idx + 1, base_idx + 2);
    d2d_face_set(&f[1], base_idx + 2, base_idx + 1, base_idx + 3);
    d2d_face_set(&f[2], base_idx + 2, base_idx + 4, base_idx + 3);
    geometry->outline.arc_face_count += 3;

    return TRUE;
}

static BOOL d2d_geometry_add_figure_outline(struct d2d_geometry *geometry,
        struct d2d_figure *figure, D2D1_FIGURE_END figure_end)
{
    const D2D1_POINT_2F *prev, *p0, *p1, *next, *next_prev;
    size_t bezier_idx, i, vertex_count;
    enum d2d_vertex_type type;

    if (!(vertex_count = figure->vertex_count))
        return TRUE;

    p0 = &figure->vertices[0];
    if (figure_end == D2D1_FIGURE_END_CLOSED)
    {
        if (figure->vertex_types[vertex_count - 1] == D2D_VERTEX_TYPE_END && !--vertex_count)
            return TRUE;

        /* In case of a CLOSED path, a join between first and last vertex is
         * required. */
        if (d2d_vertex_type_is_bezier(figure->vertex_types[vertex_count - 1]))
            prev = &figure->bezier_controls[figure->bezier_control_count - 1];
        else
            prev = &figure->vertices[vertex_count - 1];
    }
    else
    {
        if (!--vertex_count)
            return TRUE;
        prev = p0;
    }

    for (i = 0, bezier_idx = 0; i < vertex_count; ++i)
    {
        if ((type = figure->vertex_types[i]) == D2D_VERTEX_TYPE_NONE)
        {
            prev = next_prev = &figure->vertices[i];
            continue;
        }

        /* next: tangent along next segment, at p0.
         * p1: next vertex. */
        if (d2d_vertex_type_is_bezier(type))
        {
            next_prev = next = &figure->bezier_controls[bezier_idx++];
            /* type BEZIER implies i + 1 < figure->vertex_count. */
            p1 = &figure->vertices[i + 1];

            if (!d2d_geometry_outline_add_bezier_segment(geometry, p0, next, p1))
            {
                ERR("Failed to add bezier segment.\n");
                return FALSE;
            }
        }
        else
        {
            if (i + 1 == figure->vertex_count)
                next = p1 = &figure->vertices[0];
            else
                next = p1 = &figure->vertices[i + 1];
            next_prev = p0;

            if (!d2d_geometry_outline_add_line_segment(geometry, p0, p1))
            {
                ERR("Failed to add line segment.\n");
                return FALSE;
            }
        }

        if (i || figure_end == D2D1_FIGURE_END_CLOSED)
        {
            D2D1_POINT_2F q_next, q_prev;

            d2d_point_subtract(&q_prev, prev, p0);
            d2d_point_subtract(&q_next, next, p0);

            d2d_point_normalise(&q_prev);
            d2d_point_normalise(&q_next);

            if (!d2d_geometry_outline_add_join(geometry, &q_prev, p0, &q_next))
            {
                ERR("Failed to add join.\n");
                return FALSE;
            }
        }

        p0 = p1;
        prev = next_prev;
    }

    return TRUE;
}

static BOOL d2d_geometry_fill_add_arc_triangle(struct d2d_geometry *geometry,
        const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1, const D2D1_POINT_2F *p2)
{
    struct d2d_curve_vertex *a;

    if (!d2d_array_reserve((void **)&geometry->fill.arc_vertices, &geometry->fill.arc_vertices_size,
            geometry->fill.arc_vertex_count + 3, sizeof(*geometry->fill.arc_vertices)))
        return FALSE;

    a = &geometry->fill.arc_vertices[geometry->fill.arc_vertex_count];
    d2d_curve_vertex_set(&a[0], p0, 0.0f, 1.0f, -1.0f);
    d2d_curve_vertex_set(&a[1], p1, 1.0f, 1.0f, -1.0f);
    d2d_curve_vertex_set(&a[2], p2, 1.0f, 0.0f, -1.0f);
    geometry->fill.arc_vertex_count += 3;

    return TRUE;
}

void d2d_geometry_cleanup(struct d2d_geometry *geometry)
{
    free(geometry->outline.arc_faces);
    free(geometry->outline.arcs);
    free(geometry->outline.bezier_faces);
    free(geometry->outline.beziers);
    free(geometry->outline.faces);
    free(geometry->outline.vertices);
    free(geometry->fill.arc_vertices);
    free(geometry->fill.bezier_vertices);
    free(geometry->fill.faces);
    free(geometry->fill.vertices);
    ID2D1Factory_Release(geometry->factory);
}

static void d2d_geometry_init(struct d2d_geometry *geometry, ID2D1Factory *factory,
        const D2D1_MATRIX_3X2_F *transform, const struct ID2D1GeometryVtbl *vtbl,
        const struct d2d_geometry_ops *ops)
{
    geometry->ID2D1Geometry_iface.lpVtbl = vtbl;
    geometry->refcount = 1;
    ID2D1Factory_AddRef(geometry->factory = factory);
    geometry->transform = *transform;
    geometry->ops = ops;
}

static inline struct d2d_geometry *impl_from_ID2D1GeometrySink(ID2D1GeometrySink *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, u.path.ID2D1GeometrySink_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_sink_QueryInterface(ID2D1GeometrySink *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1GeometrySink)
            || IsEqualGUID(iid, &IID_ID2D1SimplifiedGeometrySink)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1GeometrySink_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_geometry_sink_AddRef(ID2D1GeometrySink *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p.\n", iface);

    return ID2D1Geometry_AddRef(&geometry->ID2D1Geometry_iface);
}

static ULONG STDMETHODCALLTYPE d2d_geometry_sink_Release(ID2D1GeometrySink *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p.\n", iface);

    return ID2D1Geometry_Release(&geometry->ID2D1Geometry_iface);
}

static void STDMETHODCALLTYPE d2d_geometry_sink_SetFillMode(ID2D1GeometrySink *iface, D2D1_FILL_MODE mode)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, mode %#x.\n", iface, mode);

    if (geometry->u.path.state == D2D_GEOMETRY_STATE_CLOSED)
        return;
    geometry->u.path.fill_mode = mode;
}

static void d2d_geometry_set_error(struct d2d_geometry *geometry, HRESULT code)
{
    if (geometry->u.path.state == D2D_GEOMETRY_STATE_ERROR)
        return;

    geometry->u.path.state = D2D_GEOMETRY_STATE_ERROR;
    geometry->u.path.code = code;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_SetSegmentFlags(ID2D1GeometrySink *iface, D2D1_PATH_SEGMENT flags)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, flags %#x.\n", iface, flags);

    if (flags & ~(D2D1_PATH_SEGMENT_FORCE_UNSTROKED | D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN))
    {
        d2d_geometry_set_error(geometry, E_INVALIDARG);
        return;
    }

    if (flags != D2D1_PATH_SEGMENT_NONE)
        FIXME("Ignoring flags %#x.\n", flags);

    geometry->u.path.segment_flags = flags;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_BeginFigure(ID2D1GeometrySink *iface,
        D2D1_POINT_2F start_point, D2D1_FIGURE_BEGIN figure_begin)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);
    struct d2d_figure *figure;

    TRACE("iface %p, start_point %s, figure_begin %#x.\n",
            iface, debug_d2d_point_2f(&start_point), figure_begin);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_OPEN)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    if (!d2d_path_geometry_add_figure(geometry))
    {
        ERR("Failed to add figure.\n");
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    figure = &geometry->u.path.figures[geometry->u.path.figure_count - 1];
    if (!d2d_figure_begin(figure, start_point, figure_begin))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    geometry->u.path.state = D2D_GEOMETRY_STATE_FIGURE;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddLines(ID2D1GeometrySink *iface,
        const D2D1_POINT_2F *points, UINT32 count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, points %p, count %u.\n", iface, points, count);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_FIGURE)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    if (!d2d_figure_add_line_segments(geometry, points, count))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    geometry->u.path.segment_count += count;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddBeziers(ID2D1GeometrySink *iface,
        const D2D1_BEZIER_SEGMENT *beziers, UINT32 count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, beziers %p, count %u.\n", iface, beziers, count);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_FIGURE)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    if (!d2d_figure_add_bezier_segments(geometry, beziers, count))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    geometry->u.path.segment_count += count;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_EndFigure(ID2D1GeometrySink *iface, D2D1_FIGURE_END figure_end)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);
    struct d2d_figure *figure;

    TRACE("iface %p, figure_end %#x.\n", iface, figure_end);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_FIGURE)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    figure = &geometry->u.path.figures[geometry->u.path.figure_count - 1];
    if (!d2d_figure_end(figure, figure_end))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    if (figure_end == D2D1_FIGURE_END_CLOSED)
        ++geometry->u.path.segment_count;

    if (!d2d_geometry_add_figure_outline(geometry, figure, figure_end))
    {
        ERR("Failed to add figure outline.\n");
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    geometry->u.path.state = D2D_GEOMETRY_STATE_OPEN;
}

static void d2d_path_geometry_free_figures(struct d2d_geometry *geometry)
{
    size_t i;

    if (!geometry->u.path.figures)
        return;

    for (i = 0; i < geometry->u.path.figure_count; ++i)
        d2d_figure_cleanup(&geometry->u.path.figures[i]);

    free(geometry->u.path.figures);
    geometry->u.path.figures = NULL;
    geometry->u.path.figures_size = 0;
}

static BOOL d2d_geometry_get_bezier_segment_idx(struct d2d_geometry *geometry, struct d2d_segment_idx *idx, BOOL next)
{
    if (next)
    {
        ++idx->vertex_idx;
        ++idx->control_idx;
    }

    for (; idx->figure_idx < geometry->u.path.figure_count; ++idx->figure_idx, idx->vertex_idx = idx->control_idx = 0)
    {
        struct d2d_figure *figure = &geometry->u.path.figures[idx->figure_idx];

        if (!figure->bezier_control_count || figure->flags & D2D_FIGURE_FLAG_HOLLOW)
            continue;

        for (; idx->vertex_idx < figure->vertex_count; ++idx->vertex_idx)
        {
            if (d2d_vertex_type_is_bezier(figure->vertex_types[idx->vertex_idx]))
                return TRUE;
        }
    }

    return FALSE;
}

static BOOL d2d_geometry_get_first_bezier_segment_idx(struct d2d_geometry *geometry, struct d2d_segment_idx *idx)
{
    memset(idx, 0, sizeof(*idx));

    return d2d_geometry_get_bezier_segment_idx(geometry, idx, FALSE);
}

static BOOL d2d_geometry_get_next_bezier_segment_idx(struct d2d_geometry *geometry, struct d2d_segment_idx *idx)
{
    return d2d_geometry_get_bezier_segment_idx(geometry, idx, TRUE);
}

static BOOL d2d_geometry_check_bezier_overlap(struct d2d_geometry *geometry,
        const struct d2d_segment_idx *idx_p, const struct d2d_segment_idx *idx_q)
{
    const D2D1_POINT_2F *a[3], *b[3], *p[2], *q;
    const struct d2d_figure *figure;
    D2D1_POINT_2F v_q[3], v_p, v_qp;
    unsigned int i, j, score;
    float ccw_a, ccw_b;
    float det, t;

    figure = &geometry->u.path.figures[idx_p->figure_idx];
    a[0] = &figure->vertices[idx_p->vertex_idx];
    a[1] = &figure->bezier_controls[idx_p->control_idx];
    a[2] = &figure->vertices[idx_p->vertex_idx + 1];

    figure = &geometry->u.path.figures[idx_q->figure_idx];
    b[0] = &figure->vertices[idx_q->vertex_idx];
    b[1] = &figure->bezier_controls[idx_q->control_idx];
    b[2] = &figure->vertices[idx_q->vertex_idx + 1];

    /* Degenerate control triangles can't overlap in a way that splitting them
     * would resolve.  Note that a non-finite area needs to be rejected here as
     * well: every comparison against a NaN is false, so none of the tests below
     * would ever reject an overlap, and the caller would keep subdividing the
     * curve indefinitely. */
    ccw_a = d2d_point_ccw(a[0], a[1], a[2]);
    ccw_b = d2d_point_ccw(b[0], b[1], b[2]);
    if (!isfinite(ccw_a) || ccw_a == 0.0f || !isfinite(ccw_b) || ccw_b == 0.0f)
        return FALSE;

    /* Coincident segments have the same control triangle. Splitting one of
     * them would only produce two smaller triangles that still coincide with
     * the other's halves, and because the two curves would be split in a
     * different order they would end up with different vertices along the
     * same curve, leaving slivers between them. Keep them identical instead;
     * their curve triangles then cancel or add exactly. */
    if (d2d_bezier_segments_coincide(a, b))
        return FALSE;

    d2d_point_subtract(&v_q[0], b[1], b[0]);
    d2d_point_subtract(&v_q[1], b[2], b[0]);
    d2d_point_subtract(&v_q[2], b[1], b[2]);

    /* Check for intersections between the edges. Strictly speaking we'd only
     * need to check 8 of the 9 possible intersections, since if there's any
     * intersection there has to be a second intersection as well. */
    for (i = 0; i < 3; ++i)
    {
        d2d_point_subtract(&v_p, a[(i & 1) + 1], a[i & 2]);
        for (j = 0; j < 3; ++j)
        {
            det = v_p.x * v_q[j].y - v_p.y * v_q[j].x;
            if (det == 0.0f)
                continue;

            d2d_point_subtract(&v_qp, a[i & 2], b[j & 2]);
            t = (v_q[j].x * v_qp.y - v_q[j].y * v_qp.x) / det;
            if (t <= 0.0f || t >= 1.0f)
                continue;

            t = (v_p.x * v_qp.y - v_p.y * v_qp.x) / det;
            if (t <= 0.0f || t >= 1.0f)
                continue;

            return TRUE;
        }
    }

    /* Check if one triangle is contained within the other. */
    for (j = 0, score = 0, q = a[1], p[0] = b[2]; j < 3; ++j)
    {
        p[1] = b[j];
        d2d_point_subtract(&v_p, p[1], p[0]);
        d2d_point_subtract(&v_qp, q, p[0]);

        if ((q->y < p[0]->y) != (q->y < p[1]->y) && v_qp.x < v_p.x * (v_qp.y / v_p.y))
            ++score;

        p[0] = p[1];
    }

    if (score & 1)
        return TRUE;

    for (j = 0, score = 0, q = b[1], p[0] = a[2]; j < 3; ++j)
    {
        p[1] = a[j];
        d2d_point_subtract(&v_p, p[1], p[0]);
        d2d_point_subtract(&v_qp, q, p[0]);

        if ((q->y < p[0]->y) != (q->y < p[1]->y) && v_qp.x < v_p.x * (v_qp.y / v_p.y))
            ++score;

        p[0] = p[1];
    }

    return score & 1;
}

static float d2d_geometry_bezier_ccw(struct d2d_geometry *geometry, const struct d2d_segment_idx *idx)
{
    const struct d2d_figure *figure = &geometry->u.path.figures[idx->figure_idx];
    size_t next = idx->vertex_idx + 1;

    return d2d_point_ccw(&figure->vertices[idx->vertex_idx],
            &figure->bezier_controls[idx->control_idx], &figure->vertices[next]);
}

static BOOL d2d_geometry_split_bezier(struct d2d_geometry *geometry, const struct d2d_segment_idx *idx)
{
    const D2D1_POINT_2F *p[3];
    struct d2d_figure *figure;
    D2D1_POINT_2F q[3];
    size_t next;

    figure = &geometry->u.path.figures[idx->figure_idx];
    p[0] = &figure->vertices[idx->vertex_idx];
    p[1] = &figure->bezier_controls[idx->control_idx];
    next = idx->vertex_idx + 1;
    p[2] = &figure->vertices[next];

    d2d_point_lerp(&q[0], p[0], p[1], 0.5f);
    d2d_point_lerp(&q[1], p[1], p[2], 0.5f);
    d2d_point_lerp(&q[2], &q[0], &q[1], 0.5f);

    figure->bezier_controls[idx->control_idx] = q[0];
    if (!(d2d_figure_insert_bezier_controls(figure, idx->control_idx + 1, 1, &q[1])))
        return FALSE;
    if (!(d2d_figure_insert_vertex(figure, idx->vertex_idx + 1, q[2])))
        return FALSE;
    figure->vertex_types[idx->vertex_idx + 1] = D2D_VERTEX_TYPE_SPLIT_BEZIER;

    return TRUE;
}

struct d2d_bezier_cap
{
    D2D1_POINT_2F p[3];     /* Canonical: p[0] is the lesser end point. */
    size_t ordinal;         /* Position in the bezier segment iteration order. */
    size_t group;
    BOOL reversed;
};

static int d2d_bezier_cap_compare(const void *a, const void *b)
{
    const struct d2d_bezier_cap *ca = a, *cb = b;

    if (ca->p[0].x < cb->p[0].x)
        return -1;
    return ca->p[0].x > cb->p[0].x;
}

/* Coincident curve segments of different figures keep the same control
 * triangle (d2d_geometry_check_bezier_overlap() refuses to split them), so
 * their curve triangles would be rasterised on top of each other. The fill
 * rule says how many of them count: two arcs running the same way fill their
 * side once, an arc and its reverse cancel out, and under the alternate rule
 * every pair cancels. Emit a single curve triangle for a group with a
 * non-zero net winding and none otherwise. The chords stay in their figures
 * and are handled by the triangulation like any other edge, so the polygon
 * part of the fill is already correct. */
static BOOL d2d_geometry_shared_bezier_caps(struct d2d_geometry *geometry, BOOL **emit, size_t *emitted)
{
    struct d2d_bezier_cap *caps, *ca, *cb;
    struct d2d_segment_idx idx;
    const D2D1_POINT_2F *p[3];
    struct d2d_figure *figure;
    size_t count, i, j, next;
    BOOL *e;

    *emit = NULL;
    *emitted = 0;
    if (!d2d_geometry_get_first_bezier_segment_idx(geometry, &idx))
        return TRUE;
    count = 1;
    while (d2d_geometry_get_next_bezier_segment_idx(geometry, &idx))
        ++count;
    *emitted = count;
    if (count < 2)
        return TRUE;

    if (!(caps = calloc(count, sizeof(*caps))))
        return FALSE;
    d2d_geometry_get_first_bezier_segment_idx(geometry, &idx);
    for (i = 0; i < count; ++i, d2d_geometry_get_next_bezier_segment_idx(geometry, &idx))
    {
        figure = &geometry->u.path.figures[idx.figure_idx];
        p[0] = &figure->vertices[idx.vertex_idx];
        p[1] = &figure->bezier_controls[idx.control_idx];
        if ((next = idx.vertex_idx + 1) == figure->vertex_count)
            next = 0;
        p[2] = &figure->vertices[next];

        ca = &caps[i];
        ca->ordinal = i;
        ca->group = i;
        ca->reversed = p[2]->x < p[0]->x || (p[2]->x == p[0]->x && p[2]->y < p[0]->y);
        ca->p[0] = ca->reversed ? *p[2] : *p[0];
        ca->p[1] = *p[1];
        ca->p[2] = ca->reversed ? *p[0] : *p[2];
    }
    qsort(caps, count, sizeof(*caps), d2d_bezier_cap_compare);

    /* Sorted by the lesser end point's x, so coincident segments sit next to
     * each other, at most a tolerance apart. */
    for (i = 0; i < count; ++i)
    {
        const D2D1_POINT_2F *pa[3], *pb[3];

        ca = &caps[i];
        if (ca->group != ca->ordinal)
            continue;
        pa[0] = &ca->p[0]; pa[1] = &ca->p[1]; pa[2] = &ca->p[2];
        for (j = i + 1; j < count; ++j)
        {
            cb = &caps[j];
            if (cb->p[0].x - ca->p[0].x > 1e-4f * fmaxf(1.0f, fabsf(ca->p[0].x)))
                break;
            if (cb->group != cb->ordinal)
                continue;
            pb[0] = &cb->p[0]; pb[1] = &cb->p[1]; pb[2] = &cb->p[2];
            if (d2d_bezier_segments_coincide(pa, pb))
                cb->group = ca->ordinal;
        }
    }

    if (!(e = malloc(count * sizeof(*e))))
    {
        free(caps);
        return FALSE;
    }
    for (i = 0; i < count; ++i)
        e[i] = TRUE;

    for (i = 0; i < count; ++i)
    {
        int net = 0;
        size_t n = 0;

        ca = &caps[i];
        if (ca->group != ca->ordinal)
            continue;
        for (j = i; j < count; ++j)
        {
            cb = &caps[j];
            if (cb->group != ca->ordinal)
                continue;
            net += cb->reversed == ca->reversed ? 1 : -1;
            ++n;
            if (j != i)
                e[cb->ordinal] = FALSE;
        }
        if (n < 2)
            continue;
        if (geometry->u.path.fill_mode == D2D1_FILL_MODE_ALTERNATE ? !(n & 1) : !net)
            e[ca->ordinal] = FALSE;
        TRACE("%Iu coincident bezier segments, net winding %d.\n", n, net);
    }

    *emitted = 0;
    for (i = 0; i < count; ++i)
        if (e[i])
            ++*emitted;
    *emit = e;
    free(caps);
    return TRUE;
}

static HRESULT d2d_geometry_resolve_beziers(struct d2d_geometry *geometry)
{
    struct d2d_segment_idx idx_p, idx_q;
    struct d2d_curve_vertex *b;
    const D2D1_POINT_2F *p[3];
    struct d2d_figure *figure;
    size_t bezier_idx, i, ordinal, emitted;
    BOOL *emit;

    if (!d2d_geometry_get_first_bezier_segment_idx(geometry, &idx_p))
        return S_OK;

    /* Split overlapping bezier control triangles. */
    while (d2d_geometry_get_next_bezier_segment_idx(geometry, &idx_p))
    {
        d2d_geometry_get_first_bezier_segment_idx(geometry, &idx_q);
        while (idx_q.figure_idx < idx_p.figure_idx || idx_q.vertex_idx < idx_p.vertex_idx)
        {
            while (d2d_geometry_check_bezier_overlap(geometry, &idx_p, &idx_q))
            {
                if (fabsf(d2d_geometry_bezier_ccw(geometry, &idx_q)) > fabsf(d2d_geometry_bezier_ccw(geometry, &idx_p)))
                {
                    if (!d2d_geometry_split_bezier(geometry, &idx_q))
                        return E_OUTOFMEMORY;
                    if (idx_p.figure_idx == idx_q.figure_idx)
                    {
                        ++idx_p.vertex_idx;
                        ++idx_p.control_idx;
                    }
                }
                else
                {
                    if (!d2d_geometry_split_bezier(geometry, &idx_p))
                        return E_OUTOFMEMORY;
                }
            }
            d2d_geometry_get_next_bezier_segment_idx(geometry, &idx_q);
        }
    }

    if (!d2d_geometry_shared_bezier_caps(geometry, &emit, &emitted))
        return E_OUTOFMEMORY;
    geometry->fill.bezier_vertex_count = 3 * emitted;

    if (emitted && !(geometry->fill.bezier_vertices = calloc(geometry->fill.bezier_vertex_count,
            sizeof(*geometry->fill.bezier_vertices))))
    {
        ERR("Failed to allocate bezier vertices array.\n");
        geometry->fill.bezier_vertex_count = 0;
        free(emit);
        return E_OUTOFMEMORY;
    }

    bezier_idx = 0;
    ordinal = 0;
    if (d2d_geometry_get_first_bezier_segment_idx(geometry, &idx_p)) do
    {
        float sign = -1.0f;

        if (emit && !emit[ordinal++])
            continue;

        figure = &geometry->u.path.figures[idx_p.figure_idx];
        p[0] = &figure->vertices[idx_p.vertex_idx];
        p[1] = &figure->bezier_controls[idx_p.control_idx];

        i = idx_p.vertex_idx + 1;
        if (d2d_path_geometry_point_inside(geometry, p[1], FALSE))
        {
            sign = 1.0f;
            d2d_figure_insert_vertex(figure, i, *p[1]);
            /* Inserting a vertex potentially invalidates p[0]. */
            p[0] = &figure->vertices[idx_p.vertex_idx];
            ++i;
        }

        if (i == figure->vertex_count)
            i = 0;
        p[2] = &figure->vertices[i];

        b = &geometry->fill.bezier_vertices[bezier_idx++ * 3];
        d2d_curve_vertex_set(&b[0], p[0], 0.0f, 0.0f, sign);
        d2d_curve_vertex_set(&b[1], p[1], 0.5f, 0.0f, sign);
        d2d_curve_vertex_set(&b[2], p[2], 1.0f, 1.0f, sign);
    } while (d2d_geometry_get_next_bezier_segment_idx(geometry, &idx_p));

    free(emit);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_sink_Close(ID2D1GeometrySink *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);
    HRESULT hr = E_FAIL;
    size_t i;

    TRACE("iface %p.\n", iface);

    if (geometry->u.path.state == D2D_GEOMETRY_STATE_CLOSED)
        return D2DERR_WRONG_STATE;

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_OPEN)
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);

    if (geometry->u.path.state == D2D_GEOMETRY_STATE_ERROR)
        return geometry->u.path.code;

    geometry->u.path.state = D2D_GEOMETRY_STATE_CLOSED;

    /* Remove collinear LINE vertices from figures to reduce CDT complexity.
     * This is safe because the outline data is already computed in EndFigure,
     * and removing collinear points does not change the filled area. */
    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        struct d2d_figure *figure = &geometry->u.path.figures[i];
        size_t k;

        if (figure->vertex_count < 3)
            continue;

        for (k = 1; k + 1 < figure->vertex_count;)
        {
            float cross;

            if (figure->vertex_types[k] != D2D_VERTEX_TYPE_LINE)
            {
                ++k;
                continue;
            }

            /* Check if vertex k is collinear with k-1 and k+1 using cross product. */
            cross = (figure->vertices[k].x - figure->vertices[k - 1].x)
                    * (figure->vertices[k + 1].y - figure->vertices[k - 1].y)
                    - (figure->vertices[k].y - figure->vertices[k - 1].y)
                    * (figure->vertices[k + 1].x - figure->vertices[k - 1].x);

            if (cross >= -1e-6f && cross <= 1e-6f)
            {
                memmove(&figure->vertices[k], &figure->vertices[k + 1],
                        (figure->vertex_count - k - 1) * sizeof(*figure->vertices));
                memmove(&figure->vertex_types[k], &figure->vertex_types[k + 1],
                        (figure->vertex_count - k - 1) * sizeof(*figure->vertex_types));
                --figure->vertex_count;
            }
            else
            {
                ++k;
            }
        }
    }

    if (!d2d_geometry_intersect_self(geometry))
        goto done;
    if (FAILED(hr = d2d_geometry_resolve_beziers(geometry)))
        goto done;
    if (FAILED(hr = d2d_path_geometry_triangulate(geometry)))
        goto done;

done:
    if (FAILED(hr))
    {
        free(geometry->fill.bezier_vertices);
        geometry->fill.bezier_vertices = NULL;
        geometry->fill.bezier_vertex_count = 0;
        d2d_path_geometry_free_figures(geometry);
        d2d_geometry_set_error(geometry, hr);
    }
    return hr;
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddLine(ID2D1GeometrySink *iface, D2D1_POINT_2F point)
{
    TRACE("iface %p, point %s.\n", iface, debug_d2d_point_2f(&point));

    d2d_geometry_sink_AddLines(iface, &point, 1);
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddBezier(ID2D1GeometrySink *iface, const D2D1_BEZIER_SEGMENT *bezier)
{
    TRACE("iface %p, bezier %p.\n", iface, bezier);

    d2d_geometry_sink_AddBeziers(iface, bezier, 1);
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddQuadraticBezier(ID2D1GeometrySink *iface,
        const D2D1_QUADRATIC_BEZIER_SEGMENT *bezier)
{
    TRACE("iface %p, bezier %p.\n", iface, bezier);

    ID2D1GeometrySink_AddQuadraticBeziers(iface, bezier, 1);
}

static void STDMETHODCALLTYPE d2d_geometry_sink_AddQuadraticBeziers(ID2D1GeometrySink *iface,
        const D2D1_QUADRATIC_BEZIER_SEGMENT *beziers, UINT32 bezier_count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, beziers %p, bezier_count %u.\n", iface, beziers, bezier_count);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_FIGURE)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    if (!d2d_figure_add_quadratic_bezier_segments(geometry, beziers, bezier_count))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    geometry->u.path.segment_count += bezier_count;
}


static void STDMETHODCALLTYPE d2d_geometry_sink_AddArc(ID2D1GeometrySink *iface, const D2D1_ARC_SEGMENT *arc)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometrySink(iface);

    TRACE("iface %p, arc %p.\n", iface, arc);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_FIGURE)
    {
        d2d_geometry_set_error(geometry, D2DERR_WRONG_STATE);
        return;
    }

    if (!d2d_figure_add_arc_segment(geometry, arc))
    {
        d2d_geometry_set_error(geometry, E_OUTOFMEMORY);
        return;
    }

    ++geometry->u.path.segment_count;
}

static const struct ID2D1GeometrySinkVtbl d2d_geometry_sink_vtbl =
{
    d2d_geometry_sink_QueryInterface,
    d2d_geometry_sink_AddRef,
    d2d_geometry_sink_Release,
    d2d_geometry_sink_SetFillMode,
    d2d_geometry_sink_SetSegmentFlags,
    d2d_geometry_sink_BeginFigure,
    d2d_geometry_sink_AddLines,
    d2d_geometry_sink_AddBeziers,
    d2d_geometry_sink_EndFigure,
    d2d_geometry_sink_Close,
    d2d_geometry_sink_AddLine,
    d2d_geometry_sink_AddBezier,
    d2d_geometry_sink_AddQuadraticBezier,
    d2d_geometry_sink_AddQuadraticBeziers,
    d2d_geometry_sink_AddArc,
};

static inline struct d2d_geometry *impl_from_ID2D1PathGeometry1(ID2D1PathGeometry1 *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_QueryInterface(ID2D1PathGeometry1 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1PathGeometry1)
            || IsEqualGUID(iid, &IID_ID2D1PathGeometry)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1PathGeometry1_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_path_geometry_AddRef(ID2D1PathGeometry1 *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_path_geometry_Release(ID2D1PathGeometry1 *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        d2d_path_geometry_free_figures(geometry);
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_path_geometry_GetFactory(ID2D1PathGeometry1 *iface, ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_GetBounds(ID2D1PathGeometry1 *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    size_t i;

    TRACE("iface %p, transform %p, bounds %p.\n", iface, transform, bounds);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_CLOSED)
        return D2DERR_WRONG_STATE;

    bounds->left = FLT_MAX;
    bounds->top = FLT_MAX;
    bounds->right = -FLT_MAX;
    bounds->bottom = -FLT_MAX;

    if (!transform)
    {
        if (geometry->u.path.bounds.left > geometry->u.path.bounds.right
                && !isinf(geometry->u.path.bounds.left))
        {
            for (i = 0; i < geometry->u.path.figure_count; ++i)
            {
                if (geometry->u.path.figures[i].flags & D2D_FIGURE_FLAG_HOLLOW)
                    continue;
                d2d_rect_union(&geometry->u.path.bounds, &geometry->u.path.figures[i].bounds);
            }
            if (geometry->u.path.bounds.left > geometry->u.path.bounds.right)
            {
                geometry->u.path.bounds.left = INFINITY;
                geometry->u.path.bounds.right = FLT_MAX;
                geometry->u.path.bounds.top = INFINITY;
                geometry->u.path.bounds.bottom = FLT_MAX;
            }
        }

        *bounds = geometry->u.path.bounds;
        return S_OK;
    }

    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];
        struct d2d_quadratic_bezier quadratics[D2D_BEZIER_MAX_QUADRATICS];
        enum d2d_vertex_type type = D2D_VERTEX_TYPE_NONE;
        unsigned int k, quadratic_count;
        D2D1_RECT_F bezier_bounds;
        D2D1_POINT_2F p, cubic[4];
        size_t j, bezier_idx;

        if (figure->flags & D2D_FIGURE_FLAG_HOLLOW)
            continue;

        for (j = 0; j < figure->vertex_count; ++j)
        {
            if (figure->vertex_types[j] == D2D_VERTEX_TYPE_NONE)
                continue;

            p = figure->vertices[j];
            type = figure->vertex_types[j];
            d2d_point_transform(&p, transform, p.x, p.y);
            d2d_rect_expand(bounds, &p);
            break;
        }

        for (bezier_idx = 0, ++j; j < figure->vertex_count; ++j)
        {
            enum d2d_vertex_type next_type;

            if ((next_type = figure->vertex_types[j]) == D2D_VERTEX_TYPE_NONE
                    || d2d_vertex_type_is_split_bezier(next_type))
                continue;

            switch (type)
            {
                case D2D_VERTEX_TYPE_LINE:
                    p = figure->vertices[j];
                    d2d_point_transform(&p, transform, p.x, p.y);
                    d2d_rect_expand(bounds, &p);
                    break;

                case D2D_VERTEX_TYPE_BEZIER:
                    cubic[0] = p;
                    cubic[1] = figure->original_bezier_controls[bezier_idx++];
                    d2d_point_transform(&cubic[1], transform, cubic[1].x, cubic[1].y);
                    cubic[2] = figure->original_bezier_controls[bezier_idx++];
                    d2d_point_transform(&cubic[2], transform, cubic[2].x, cubic[2].y);
                    cubic[3] = figure->vertices[j];
                    d2d_point_transform(&cubic[3], transform, cubic[3].x, cubic[3].y);

                    /* Bound the same chain of quadratics the figure itself is
                     * built from. A single reduced quadratic cannot follow the
                     * cubic through an inflection, so the rectangle it reports
                     * can leave part of the curve outside. */
                    quadratic_count = d2d_cubic_bezier_to_quadratics(cubic, quadratics);
                    for (k = 0; k < quadratic_count; ++k)
                    {
                        d2d_rect_get_bezier_bounds(&bezier_bounds, &quadratics[k].p0,
                                &quadratics[k].control, &quadratics[k].p2);
                        d2d_rect_union(bounds, &bezier_bounds);
                    }
                    p = cubic[3];
                    break;

                default:
                    FIXME("Unhandled vertex type %#x.\n", type);
                    p = figure->vertices[j];
                    d2d_point_transform(&p, transform, p.x, p.y);
                    d2d_rect_expand(bounds, &p);
                    break;
            }

            type = next_type;
        }
    }

    if (bounds->left > bounds->right)
    {
        bounds->left = INFINITY;
        bounds->right = FLT_MAX;
        bounds->top = INFINITY;
        bounds->bottom = FLT_MAX;
    }

    return S_OK;
}

/* Conservative approximation of the widened bounding box.
 *
 * The real widened bounds enclose the stroke outline, which can exceed the fill
 * outline by up to half the stroke width — and, for miter joins at sharp
 * corners, by up to miter_limit * half_width. Callers use widened bounds for
 * invalidation regions and clipping, where a slightly large rectangle is
 * correct and safe; a too-small one would drop dirty pixels. The previous stub
 * returned no bounds at all (E_NOTIMPL), so this approximation is never a
 * regression. */
static void d2d_geometry_widen_bounds(const D2D1_RECT_F *fill_bounds, float stroke_width,
        ID2D1StrokeStyle *stroke_style, D2D1_RECT_F *bounds)
{
    float half_width = stroke_width * 0.5f;
    float expand = half_width;

    if (stroke_style)
    {
        D2D1_LINE_JOIN join = ID2D1StrokeStyle_GetLineJoin(stroke_style);
        if (join == D2D1_LINE_JOIN_MITER || join == D2D1_LINE_JOIN_MITER_OR_BEVEL)
        {
            float miter_limit = ID2D1StrokeStyle_GetMiterLimit(stroke_style);
            if (miter_limit > 1.0f && half_width * miter_limit > expand)
                expand = half_width * miter_limit;
        }
    }

    bounds->left = fill_bounds->left - expand;
    bounds->top = fill_bounds->top - expand;
    bounds->right = fill_bounds->right + expand;
    bounds->bottom = fill_bounds->bottom + expand;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_GetWidenedBounds(ID2D1PathGeometry1 *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_RECT_F *bounds)
{
    D2D1_RECT_F fill_bounds;
    HRESULT hr;

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    if (FAILED(hr = ID2D1PathGeometry1_GetBounds(iface, transform, &fill_bounds)))
        return hr;

    d2d_geometry_widen_bounds(&fill_bounds, stroke_width, stroke_style, bounds);

    return S_OK;
}

/* Dashed strokes only cover part of the path, so a point sitting in a gap is
 * not on the stroke. The dash pattern is measured in multiples of the stroke
 * width along the path, which means the work happens in geometry space, before
 * the transformation. */
#define D2D_MAX_DASHES 32

struct d2d_dash_pattern
{
    float dashes[D2D_MAX_DASHES];
    unsigned int count;
    float period;
    float offset;
    float cap_extend;
};

static BOOL d2d_dash_pattern_init(struct d2d_dash_pattern *pattern, ID2D1StrokeStyle *stroke_style,
        float stroke_width)
{
    D2D1_CAP_STYLE dash_cap;
    unsigned int i, count;

    if (!stroke_style || ID2D1StrokeStyle_GetDashStyle(stroke_style) == D2D1_DASH_STYLE_SOLID)
        return FALSE;

    if (!(count = ID2D1StrokeStyle_GetDashesCount(stroke_style)) || count > D2D_MAX_DASHES)
    {
        if (count)
            FIXME("Ignoring dash pattern with %u entries.\n", count);
        return FALSE;
    }

    ID2D1StrokeStyle_GetDashes(stroke_style, pattern->dashes, count);
    pattern->count = count;

    /* The pattern alternates dash, gap, dash, …; an odd entry count means the
     * sequence repeats shifted, which is handled by doubling it. */
    if (count & 1)
    {
        if (count * 2 > D2D_MAX_DASHES)
            return FALSE;
        memcpy(&pattern->dashes[count], pattern->dashes, count * sizeof(*pattern->dashes));
        pattern->count = count * 2;
    }

    pattern->period = 0.0f;
    for (i = 0; i < pattern->count; ++i)
    {
        /* Dash lengths are multiples of the stroke width. */
        pattern->dashes[i] *= stroke_width;
        pattern->period += pattern->dashes[i];
    }

    if (pattern->period <= 0.0f)
        return FALSE;

    pattern->offset = ID2D1StrokeStyle_GetDashOffset(stroke_style) * stroke_width;
    pattern->offset = fmodf(pattern->offset, pattern->period);
    if (pattern->offset < 0.0f)
        pattern->offset += pattern->period;

    /* A flat dash cap ends the dash exactly; square and round caps extend it by
     * half the stroke width. Round caps are approximated by that extension,
     * which is a superset of the half disc — the difference is confined to the
     * corners. */
    dash_cap = ID2D1StrokeStyle_GetDashCap(stroke_style);
    pattern->cap_extend = dash_cap == D2D1_CAP_STYLE_FLAT ? 0.0f : stroke_width * 0.5f;

    return TRUE;
}

/* Test the part of [start, end] that the dash pattern actually paints. The
 * segment covers the path from arc length "pos" onwards. */
static BOOL d2d_dash_pattern_test_segment(const struct d2d_dash_pattern *pattern,
        const D2D1_POINT_2F *q, const D2D1_POINT_2F *start, const D2D1_POINT_2F *end,
        const D2D1_MATRIX_3X2_F *transform, float stroke_width, float tolerance, float *pos)
{
    float length, covered, dash_start, dash_end;
    D2D1_POINT_2F dir, p0, p1;
    unsigned int i;

    d2d_point_subtract(&dir, end, start);
    if ((length = d2d_point_length(&dir)) == 0.0f)
        return FALSE;
    d2d_point_scale(&dir, 1.0f / length);

    /* Walk the pattern from where this segment starts. */
    covered = -fmodf(*pos + pattern->offset, pattern->period);
    i = 0;
    while (covered + pattern->dashes[i] <= 0.0f)
    {
        covered += pattern->dashes[i];
        i = (i + 1) % pattern->count;
    }

    for (; covered < length; i = (i + 1) % pattern->count)
    {
        float next = covered + pattern->dashes[i];

        /* Even entries are dashes, odd ones gaps. */
        if (!(i & 1))
        {
            dash_start = covered - pattern->cap_extend;
            dash_end = next + pattern->cap_extend;

            if (dash_start < 0.0f)
                dash_start = 0.0f;
            if (dash_end > length)
                dash_end = length;

            if (dash_end > dash_start)
            {
                p0.x = start->x + dir.x * dash_start;
                p0.y = start->y + dir.y * dash_start;
                p1.x = start->x + dir.x * dash_end;
                p1.y = start->y + dir.y * dash_end;

                if (d2d_point_on_line_segment(q, &p0, &p1, transform, stroke_width * 0.5f, tolerance))
                {
                    *pos += length;
                    return TRUE;
                }
            }
        }

        covered = next;
    }

    *pos += length;
    return FALSE;
}

/* Flatten a Bézier segment into line segments and run the dash test on each of
 * them, so that the arc length keeps accumulating across the curve. */
static BOOL d2d_dash_pattern_test_bezier(const struct d2d_dash_pattern *pattern,
        const D2D1_POINT_2F *q, const D2D1_POINT_2F *p0, const D2D1_BEZIER_SEGMENT *b,
        const D2D1_MATRIX_3X2_F *transform, float stroke_width, float tolerance, float *pos,
        unsigned int depth)
{
    D2D1_BEZIER_SEGMENT b0, b1;
    D2D1_POINT_2F m;
    float d;

    /* Same deviation estimate and subdivision as d2d_geometry_flatten_cubic().
     * The depth limit keeps pathological curves bounded. */
    d2d_point_lerp(&m, p0, &b->point2, 0.5f);
    d2d_point_subtract(&m, &b->point1, &m);
    d = fabsf(m.x) + fabsf(m.y);
    d2d_point_lerp(&m, &b->point1, &b->point3, 0.5f);
    d2d_point_subtract(&m, &b->point2, &m);
    d += fabsf(m.x) + fabsf(m.y);

    if (depth >= 16 || d < tolerance)
        return d2d_dash_pattern_test_segment(pattern, q, p0, &b->point3, transform,
                stroke_width, tolerance, pos);

    d2d_point_lerp(&m, &b->point1, &b->point2, 0.5f);

    b1.point3 = b->point3;
    d2d_point_lerp(&b1.point2, &b1.point3, &b->point2, 0.5f);
    d2d_point_lerp(&b1.point1, &b1.point2, &m, 0.5f);

    d2d_point_lerp(&b0.point1, p0, &b->point1, 0.5f);
    d2d_point_lerp(&b0.point2, &b0.point1, &m, 0.5f);
    d2d_point_lerp(&b0.point3, &b0.point2, &b1.point1, 0.5f);

    if (d2d_dash_pattern_test_bezier(pattern, q, p0, &b0, transform, stroke_width,
            tolerance, pos, depth + 1))
        return TRUE;
    return d2d_dash_pattern_test_bezier(pattern, q, &b0.point3, &b1, transform, stroke_width,
            tolerance, pos, depth + 1);
}

static BOOL d2d_path_geometry_dashed_stroke_contains_point(const struct d2d_geometry *geometry,
        const D2D1_POINT_2F *q, const struct d2d_dash_pattern *pattern, float stroke_width,
        const D2D1_MATRIX_3X2_F *transform, float tolerance)
{
    enum d2d_vertex_type type;
    unsigned int i, j, bezier_idx;
    D2D1_BEZIER_SEGMENT b;
    D2D1_POINT_2F p, p1;
    float pos;

    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];

        type = D2D_VERTEX_TYPE_NONE;
        for (j = 0; j < figure->vertex_count; ++j)
        {
            if (figure->vertex_types[j] == D2D_VERTEX_TYPE_NONE)
                continue;

            p = figure->vertices[j];
            type = figure->vertex_types[j];
            break;
        }

        /* Each figure restarts the dash pattern. */
        pos = 0.0f;

        for (bezier_idx = 0, ++j; j < figure->vertex_count; ++j)
        {
            enum d2d_vertex_type next_type;

            if ((next_type = figure->vertex_types[j]) == D2D_VERTEX_TYPE_NONE
                    || d2d_vertex_type_is_split_bezier(next_type))
                continue;

            switch (type)
            {
                case D2D_VERTEX_TYPE_LINE:
                    p1 = figure->vertices[j];
                    if (d2d_dash_pattern_test_segment(pattern, q, &p, &p1, transform,
                            stroke_width, tolerance, &pos))
                        return TRUE;
                    p = p1;
                    break;

                case D2D_VERTEX_TYPE_BEZIER:
                    b.point1 = figure->original_bezier_controls[bezier_idx++];
                    b.point2 = figure->original_bezier_controls[bezier_idx++];
                    b.point3 = figure->vertices[j];
                    if (d2d_dash_pattern_test_bezier(pattern, q, &p, &b, transform,
                            stroke_width, tolerance, &pos, 0))
                        return TRUE;
                    p = b.point3;
                    break;

                default:
                    FIXME("Unhandled vertex type %#x.\n", type);
                    p = figure->vertices[j];
                    break;
            }
            type = next_type;
        }

        if (type == D2D_VERTEX_TYPE_LINE && figure->flags & D2D_FIGURE_FLAG_CLOSED)
        {
            p1 = figure->vertices[0];
            if (d2d_dash_pattern_test_segment(pattern, q, &p, &p1, transform,
                    stroke_width, tolerance, &pos))
                return TRUE;
        }
    }

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_StrokeContainsPoint(ID2D1PathGeometry1 *iface,
        D2D1_POINT_2F point, float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    enum d2d_vertex_type type = D2D_VERTEX_TYPE_NONE;
    struct d2d_dash_pattern dash_pattern;
    unsigned int i, j, bezier_idx;
    D2D1_BEZIER_SEGMENT b;
    D2D1_POINT_2F p, p1;

    TRACE("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    if (!transform)
        transform = &identity;

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    /* A dashed stroke only covers part of the path. Solid strokes keep taking
     * the plain path below, so the common case is unaffected. Line joins and
     * the caps at the ends of a figure are still not taken into account. */
    if (d2d_dash_pattern_init(&dash_pattern, stroke_style, stroke_width))
    {
        *contains = d2d_path_geometry_dashed_stroke_contains_point(geometry, &point,
                &dash_pattern, stroke_width, transform, tolerance);
        TRACE("-> %#x.\n", *contains);
        return S_OK;
    }

    *contains = FALSE;
    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];

        for (j = 0; j < figure->vertex_count; ++j)
        {
            if (figure->vertex_types[j] == D2D_VERTEX_TYPE_NONE)
                continue;

            p = figure->vertices[j];
            type = figure->vertex_types[j];
            break;
        }

        for (bezier_idx = 0, ++j; j < figure->vertex_count; ++j)
        {
            enum d2d_vertex_type next_type;

            if ((next_type = figure->vertex_types[j]) == D2D_VERTEX_TYPE_NONE
                    || d2d_vertex_type_is_split_bezier(next_type))
                continue;

            switch (type)
            {
                case D2D_VERTEX_TYPE_LINE:
                    p1 = figure->vertices[j];
                    *contains = d2d_point_on_line_segment(&point, &p, &p1, transform, stroke_width * 0.5f, tolerance);
                    p = p1;
                    break;

                case D2D_VERTEX_TYPE_BEZIER:
                    b.point1 = figure->original_bezier_controls[bezier_idx++];
                    b.point2 = figure->original_bezier_controls[bezier_idx++];
                    b.point3 = figure->vertices[j];
                    *contains = d2d_point_on_bezier_segment(&point, &p, &b, transform, stroke_width, tolerance);
                    p = b.point3;
                    break;

                default:
                    FIXME("Unhandled vertex type %#x.\n", type);
                    p = figure->vertices[j];
                    break;
            }
            if (*contains)
                return S_OK;
            type = next_type;
        }

        if (type == D2D_VERTEX_TYPE_LINE)
        {
            p1 = figure->vertices[0];
            if (figure->flags & D2D_FIGURE_FLAG_CLOSED)
                *contains = d2d_point_on_line_segment(&point, &p, &p1, transform, stroke_width * 0.5f, tolerance);
        }

        if (*contains)
            return S_OK;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_FillContainsPoint(ID2D1PathGeometry1 *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    D2D1_MATRIX_3X2_F g_i;

    TRACE("iface %p, point %s, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    if (transform)
    {
        if (!d2d_matrix_invert(&g_i, transform))
            return D2DERR_UNSUPPORTED_OPERATION;
        d2d_point_transform(&point, &g_i, point.x, point.y);
    }

    *contains = !!d2d_path_geometry_point_inside(geometry, &point, FALSE);

    TRACE("-> %#x.\n", *contains);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_CompareWithGeometry(ID2D1PathGeometry1 *iface,
        ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    FIXME("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p stub!\n",
            iface, geometry, transform, tolerance, relation);

    return E_NOTIMPL;
}

static void d2d_geometry_flatten_cubic(ID2D1SimplifiedGeometrySink *sink, const D2D1_POINT_2F *p0,
        const D2D1_BEZIER_SEGMENT *b, float tolerance)
{
    D2D1_BEZIER_SEGMENT b0, b1;
    D2D1_POINT_2F q;
    float d;

    /* It's certainly possible to calculate the maximum deviation of the
     * approximation from the curve, but it's a little involved. Instead, note
     * that if the control points were evenly spaced and collinear, p1 would
     * be exactly between p0 and p2, and p2 would be exactly between p1 and
     * p3. The deviation is a decent enough approximation, and much easier to
     * calculate.
     *
     * p1' = (p0 + p2) / 2
     * p2' = (p1 + p3) / 2
     *   d = ‖p1 - p1'‖₁ + ‖p2 - p2'‖₁ */
    d2d_point_lerp(&q, p0, &b->point2, 0.5f);
    d2d_point_subtract(&q, &b->point1, &q);
    d = fabsf(q.x) + fabsf(q.y);
    d2d_point_lerp(&q, &b->point1, &b->point3, 0.5f);
    d2d_point_subtract(&q, &b->point2, &q);
    d += fabsf(q.x) + fabsf(q.y);
    if (d < tolerance)
    {
        ID2D1SimplifiedGeometrySink_AddLines(sink, &b->point3, 1);
        return;
    }

    d2d_point_lerp(&q, &b->point1, &b->point2, 0.5f);

    b1.point3 = b->point3;
    d2d_point_lerp(&b1.point2, &b1.point3, &b->point2, 0.5f);
    d2d_point_lerp(&b1.point1, &b1.point2, &q, 0.5f);

    d2d_point_lerp(&b0.point1, p0, &b->point1, 0.5f);
    d2d_point_lerp(&b0.point2, &b0.point1, &q, 0.5f);
    d2d_point_lerp(&b0.point3, &b0.point2, &b1.point1, 0.5f);

    d2d_geometry_flatten_cubic(sink, p0, &b0, tolerance);
    ID2D1SimplifiedGeometrySink_SetSegmentFlags(sink, D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN);
    d2d_geometry_flatten_cubic(sink, &b0.point3, &b1, tolerance);
    ID2D1SimplifiedGeometrySink_SetSegmentFlags(sink, D2D1_PATH_SEGMENT_NONE);
}

static void d2d_figure_simplify(const struct d2d_figure *figure,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    enum d2d_vertex_type type = D2D_VERTEX_TYPE_NONE;
    D2D1_FIGURE_BEGIN begin;
    D2D1_BEZIER_SEGMENT b;
    size_t i, bezier_idx;
    D2D1_FIGURE_END end;
    D2D1_POINT_2F p;

    for (i = 0; i < figure->vertex_count; ++i)
    {
        if (figure->vertex_types[i] == D2D_VERTEX_TYPE_NONE)
            continue;

        p = figure->vertices[i];
        if (transform)
            d2d_point_transform(&p, transform, p.x, p.y);
        begin = figure->flags & D2D_FIGURE_FLAG_HOLLOW ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED;
        ID2D1SimplifiedGeometrySink_BeginFigure(sink, p, begin);
        type = figure->vertex_types[i];
        break;
    }

    for (bezier_idx = 0, ++i; i < figure->vertex_count; ++i)
    {
        enum d2d_vertex_type next_type;

        if ((next_type = figure->vertex_types[i]) == D2D_VERTEX_TYPE_NONE
                || d2d_vertex_type_is_split_bezier(next_type))
            continue;

        switch (type)
        {
            case D2D_VERTEX_TYPE_LINE:
                p = figure->vertices[i];
                if (transform)
                    d2d_point_transform(&p, transform, p.x, p.y);
                ID2D1SimplifiedGeometrySink_AddLines(sink, &p, 1);
                break;

            case D2D_VERTEX_TYPE_BEZIER:
                b.point1 = figure->original_bezier_controls[bezier_idx++];
                b.point2 = figure->original_bezier_controls[bezier_idx++];
                b.point3 = figure->vertices[i];
                if (transform)
                {
                    d2d_point_transform(&b.point1, transform, b.point1.x, b.point1.y);
                    d2d_point_transform(&b.point2, transform, b.point2.x, b.point2.y);
                    d2d_point_transform(&b.point3, transform, b.point3.x, b.point3.y);
                }

                if (option == D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES)
                    d2d_geometry_flatten_cubic(sink, &p, &b, tolerance);
                else
                    ID2D1SimplifiedGeometrySink_AddBeziers(sink, &b, 1);
                p = b.point3;
                break;

            default:
                FIXME("Unhandled vertex type %#x.\n", type);
                p = figure->vertices[i];
                if (transform)
                    d2d_point_transform(&p, transform, p.x, p.y);
                ID2D1SimplifiedGeometrySink_AddLines(sink, &p, 1);
                break;
        }

        type = next_type;
    }

    end = figure->flags & D2D_FIGURE_FLAG_CLOSED ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN;
    ID2D1SimplifiedGeometrySink_EndFigure(sink, end);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Simplify(ID2D1PathGeometry1 *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);
    size_t i;

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    ID2D1SimplifiedGeometrySink_SetFillMode(sink, geometry->u.path.fill_mode);
    for (i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];

        d2d_figure_simplify(figure, option, transform, tolerance, sink);
    }

    return S_OK;
}

static HRESULT d2d_geometry_get_simplified(ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1PathGeometry **ret)
{
    ID2D1PathGeometry *path_geometry = NULL;
    ID2D1GeometrySink *geometry_sink = NULL;
    ID2D1Factory *factory;
    HRESULT hr;

    *ret = NULL;

    ID2D1Geometry_GetFactory(geometry, &factory);

    hr = ID2D1Factory_CreatePathGeometry(factory, &path_geometry);
    if (SUCCEEDED(hr))
        hr = ID2D1PathGeometry_Open(path_geometry, &geometry_sink);
    if (SUCCEEDED(hr))
    {
        hr = ID2D1Geometry_Simplify(geometry, D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES,
                transform, tolerance, (ID2D1SimplifiedGeometrySink *)geometry_sink);
    }
    if (SUCCEEDED(hr))
        hr = ID2D1GeometrySink_Close(geometry_sink);
    if (geometry_sink)
        ID2D1GeometrySink_Release(geometry_sink);

    if (SUCCEEDED(hr))
    {
        *ret = path_geometry;
        ID2D1PathGeometry_AddRef(*ret);
    }

    if (path_geometry)
        ID2D1PathGeometry_Release(path_geometry);
    ID2D1Factory_Release(factory);

    return hr;
}
/* Boolean geometry combination.
 *
 * Both operands are normalised into polygon sets first. ID2D1Geometry::Simplify()
 * with D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES flattens curves to line segments
 * using the supplied tolerance and applies a transform on the way, and it is
 * implemented for every geometry type, so the code below only ever deals with
 * polygons. Two strategies are then available:
 *
 *   - When every contour on both sides is an axis-aligned rectangle, a scanline
 *     pass over the horizontal bands spanned by both operands computes all four
 *     combine modes exactly. This covers rectangles, transformed rectangles as
 *     long as the transform keeps them axis-aligned, and the multi-rectangle
 *     clip lists GUI toolkits build for partial repaints.
 *
 *   - Otherwise, for D2D1_COMBINE_MODE_INTERSECT with a convex operand, the other
 *     operand is clipped against it using the Sutherland-Hodgman algorithm. An
 *     affine transform maps a rectangle to a parallelogram, which is convex, so
 *     this covers clip masks under a rotating or skewing transform as well.
 *
 * The general boolean case - non-convex operands, or curves that need to be
 * preserved in the output rather than flattened - is not implemented. */

struct d2d_combine_contour
{
    D2D1_POINT_2F *points;
    size_t count;
    size_t size;
};

struct d2d_combine_shape
{
    struct d2d_combine_contour *contours;
    size_t count;
    size_t size;

    D2D1_FILL_MODE fill_mode;
    BOOL has_curves;
    BOOL failed;
};

struct d2d_combine_sink
{
    ID2D1SimplifiedGeometrySink ID2D1SimplifiedGeometrySink_iface;
    struct d2d_combine_shape *shape;
};

static void d2d_combine_shape_cleanup(struct d2d_combine_shape *shape)
{
    for (size_t i = 0; i < shape->count; ++i)
        free(shape->contours[i].points);
    free(shape->contours);
    memset(shape, 0, sizeof(*shape));
}

static struct d2d_combine_contour *d2d_combine_shape_add_contour(struct d2d_combine_shape *shape)
{
    struct d2d_combine_contour *contour;

    if (!d2d_array_reserve((void **)&shape->contours, &shape->size, shape->count + 1, sizeof(*shape->contours)))
    {
        shape->failed = TRUE;
        return NULL;
    }

    contour = &shape->contours[shape->count++];
    memset(contour, 0, sizeof(*contour));
    return contour;
}

static BOOL d2d_combine_contour_add_point(struct d2d_combine_contour *contour, const D2D1_POINT_2F *point)
{
    if (!d2d_array_reserve((void **)&contour->points, &contour->size, contour->count + 1, sizeof(*contour->points)))
        return FALSE;

    contour->points[contour->count++] = *point;
    return TRUE;
}

static inline struct d2d_combine_sink *impl_from_ID2D1SimplifiedGeometrySink(ID2D1SimplifiedGeometrySink *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_combine_sink, ID2D1SimplifiedGeometrySink_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_combine_sink_QueryInterface(ID2D1SimplifiedGeometrySink *iface,
        REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_ID2D1SimplifiedGeometrySink)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1SimplifiedGeometrySink_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_combine_sink_AddRef(ID2D1SimplifiedGeometrySink *iface)
{
    return 2;
}

static ULONG STDMETHODCALLTYPE d2d_combine_sink_Release(ID2D1SimplifiedGeometrySink *iface)
{
    return 1;
}

static void STDMETHODCALLTYPE d2d_combine_sink_SetFillMode(ID2D1SimplifiedGeometrySink *iface,
        D2D1_FILL_MODE mode)
{
    impl_from_ID2D1SimplifiedGeometrySink(iface)->shape->fill_mode = mode;
}

static void STDMETHODCALLTYPE d2d_combine_sink_SetSegmentFlags(ID2D1SimplifiedGeometrySink *iface,
        D2D1_PATH_SEGMENT flags)
{
}

static void STDMETHODCALLTYPE d2d_combine_sink_BeginFigure(ID2D1SimplifiedGeometrySink *iface,
        D2D1_POINT_2F start_point, D2D1_FIGURE_BEGIN figure_begin)
{
    struct d2d_combine_shape *shape = impl_from_ID2D1SimplifiedGeometrySink(iface)->shape;
    struct d2d_combine_contour *contour;

    /* Hollow figures do not contribute to the filled area. Add an empty contour
     * to keep EndFigure() and AddLines() in step, and drop it later. */
    if (!(contour = d2d_combine_shape_add_contour(shape)))
        return;
    if (figure_begin == D2D1_FIGURE_BEGIN_HOLLOW)
        return;

    if (!d2d_combine_contour_add_point(contour, &start_point))
        shape->failed = TRUE;
}

static void STDMETHODCALLTYPE d2d_combine_sink_AddLines(ID2D1SimplifiedGeometrySink *iface,
        const D2D1_POINT_2F *points, UINT32 count)
{
    struct d2d_combine_shape *shape = impl_from_ID2D1SimplifiedGeometrySink(iface)->shape;
    struct d2d_combine_contour *contour;

    if (!shape->count)
        return;
    contour = &shape->contours[shape->count - 1];
    if (!contour->count)
        return;

    for (UINT32 i = 0; i < count; ++i)
    {
        if (!d2d_combine_contour_add_point(contour, &points[i]))
        {
            shape->failed = TRUE;
            return;
        }
    }
}

static void STDMETHODCALLTYPE d2d_combine_sink_AddBeziers(ID2D1SimplifiedGeometrySink *iface,
        const D2D1_BEZIER_SEGMENT *beziers, UINT32 count)
{
    /* Simplify() was asked for lines only, so this should not happen. */
    impl_from_ID2D1SimplifiedGeometrySink(iface)->shape->has_curves = TRUE;
}

static void STDMETHODCALLTYPE d2d_combine_sink_EndFigure(ID2D1SimplifiedGeometrySink *iface,
        D2D1_FIGURE_END figure_end)
{
    struct d2d_combine_shape *shape = impl_from_ID2D1SimplifiedGeometrySink(iface)->shape;
    struct d2d_combine_contour *contour;

    if (!shape->count)
        return;
    contour = &shape->contours[shape->count - 1];

    /* Drop the repeated start point, and contours that enclose no area. */
    if (contour->count > 1 && !memcmp(&contour->points[0], &contour->points[contour->count - 1],
            sizeof(*contour->points)))
        --contour->count;

    if (contour->count < 3)
    {
        free(contour->points);
        --shape->count;
    }
}

static HRESULT STDMETHODCALLTYPE d2d_combine_sink_Close(ID2D1SimplifiedGeometrySink *iface)
{
    return S_OK;
}

static const struct ID2D1SimplifiedGeometrySinkVtbl d2d_combine_sink_vtbl =
{
    d2d_combine_sink_QueryInterface,
    d2d_combine_sink_AddRef,
    d2d_combine_sink_Release,
    d2d_combine_sink_SetFillMode,
    d2d_combine_sink_SetSegmentFlags,
    d2d_combine_sink_BeginFigure,
    d2d_combine_sink_AddLines,
    d2d_combine_sink_AddBeziers,
    d2d_combine_sink_EndFigure,
    d2d_combine_sink_Close,
};

static HRESULT d2d_combine_shape_init(struct d2d_combine_shape *shape, ID2D1Geometry *geometry,
        const D2D1_MATRIX_3X2_F *transform, float tolerance)
{
    struct d2d_combine_sink sink =
    {
        .ID2D1SimplifiedGeometrySink_iface.lpVtbl = &d2d_combine_sink_vtbl,
        .shape = shape,
    };
    HRESULT hr;

    memset(shape, 0, sizeof(*shape));
    /* Simplify() only emits SetFillMode() for path and rectangle geometries. */
    shape->fill_mode = D2D1_FILL_MODE_ALTERNATE;

    if (FAILED(hr = ID2D1Geometry_Simplify(geometry, D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES,
            transform, tolerance, &sink.ID2D1SimplifiedGeometrySink_iface)))
        return hr;

    if (shape->failed)
        return E_OUTOFMEMORY;

    return S_OK;
}

/* A non-horizontal polygon edge, stored top-down together with the winding it
 * contributes when crossed left to right. Horizontal edges carry no winding
 * and are dropped; their endpoints still bound the sweep bands through the
 * edges that meet them. */
struct d2d_combine_edge
{
    D2D1_POINT_2F top, bottom;
    int winding;
    unsigned int side;
};

struct d2d_combine_edges
{
    struct d2d_combine_edge *edges;
    size_t count;
    size_t size;
};

static BOOL d2d_combine_edges_add_shape(struct d2d_combine_edges *edges,
        const struct d2d_combine_shape *shape, unsigned int side)
{
    for (size_t i = 0; i < shape->count; ++i)
    {
        const struct d2d_combine_contour *contour = &shape->contours[i];

        for (size_t j = 0, k = contour->count - 1; j < contour->count; k = j++)
        {
            const D2D1_POINT_2F *p0 = &contour->points[k], *p1 = &contour->points[j];
            struct d2d_combine_edge *edge;

            if (p0->y == p1->y)
                continue;

            if (!d2d_array_reserve((void **)&edges->edges, &edges->size,
                    edges->count + 1, sizeof(*edges->edges)))
                return FALSE;

            edge = &edges->edges[edges->count++];
            if (p0->y < p1->y)
            {
                edge->top = *p0;
                edge->bottom = *p1;
                edge->winding = 1;
            }
            else
            {
                edge->top = *p1;
                edge->bottom = *p0;
                edge->winding = -1;
            }
            edge->side = side;
        }
    }

    return TRUE;
}

/* Evaluating an edge at a band boundary has to be exact at the endpoints and
 * reproducible in between: the bands on either side of a boundary evaluate
 * the same edge at the same y, and have to arrive at the same point for the
 * outline tracing to chain their trapezoids back together. */
static double d2d_combine_edge_x_at(const struct d2d_combine_edge *edge, float y)
{
    double t;

    if (y == edge->top.y)
        return edge->top.x;
    if (y == edge->bottom.y)
        return edge->bottom.x;

    t = ((double)y - edge->top.y) / ((double)edge->bottom.y - edge->top.y);
    return edge->top.x + t * ((double)edge->bottom.x - edge->top.x);
}

/* An edge crossed by the scanline, with the winding it contributes. */
struct d2d_combine_event
{
    double x;
    const struct d2d_combine_edge *edge;
};

static int __cdecl d2d_combine_event_compare(const void *a, const void *b)
{
    const struct d2d_combine_event *p = a, *q = b;

    if (p->x < q->x)
        return -1;
    if (p->x > q->x)
        return 1;
    return 0;
}

static BOOL d2d_combine_inside(D2D1_FILL_MODE fill_mode, int winding, unsigned int crossings)
{
    if (fill_mode == D2D1_FILL_MODE_ALTERNATE)
        return crossings & 1;
    return winding != 0;
}

static BOOL d2d_combine_op(D2D1_COMBINE_MODE mode, BOOL a, BOOL b)
{
    switch (mode)
    {
        case D2D1_COMBINE_MODE_UNION:
            return a || b;
        case D2D1_COMBINE_MODE_INTERSECT:
            return a && b;
        case D2D1_COMBINE_MODE_XOR:
            return a != b;
        case D2D1_COMBINE_MODE_EXCLUDE:
            return a && !b;
        default:
            return FALSE;
    }
}

static int __cdecl d2d_combine_float_compare(const void *a, const void *b)
{
    const float *p = a, *q = b;

    if (*p < *q)
        return -1;
    if (*p > *q)
        return 1;
    return 0;
}

/* The output of the sweep: a trapezoid with horizontal top and bottom edges,
 * cut from one band. The source edges are kept so the outline tracing can
 * recognise segments of the same edge in adjacent bands as collinear. */
struct d2d_combine_trapezoid
{
    float top, bottom;
    float l0, r0;
    float l1, r1;
    size_t left, right;
};

/* Band boundaries: every edge endpoint y, plus the y of every edge-edge
 * crossing. Within the resulting bands no edge crosses another and every edge
 * that enters a band spans it completely, so sorting the edges once per band
 * is enough for a single pass to evaluate the combination exactly. */
static BOOL d2d_combine_edges_bands(const struct d2d_combine_edges *edges, float **out, size_t *out_count)
{
    size_t count = 0, size = 0;
    float *bands = NULL;

    *out = NULL;
    *out_count = 0;

    if (!d2d_array_reserve((void **)&bands, &size, 2 * edges->count, sizeof(*bands)))
        return FALSE;
    for (size_t i = 0; i < edges->count; ++i)
    {
        bands[count++] = edges->edges[i].top.y;
        bands[count++] = edges->edges[i].bottom.y;
    }

    for (size_t i = 0; i < edges->count; ++i)
    {
        const struct d2d_combine_edge *a = &edges->edges[i];
        double ax = (double)a->bottom.x - a->top.x, ay = (double)a->bottom.y - a->top.y;

        for (size_t j = i + 1; j < edges->count; ++j)
        {
            const struct d2d_combine_edge *b = &edges->edges[j];
            double bx = (double)b->bottom.x - b->top.x, by = (double)b->bottom.y - b->top.y;
            double denom = ax * by - ay * bx;
            double ex, ey, s, t;

            /* Parallel or collinear edges do not change the left-to-right
             * order, so they contribute no boundary. */
            if (denom == 0.0)
                continue;

            ex = (double)b->top.x - a->top.x;
            ey = (double)b->top.y - a->top.y;
            t = (ex * by - ey * bx) / denom;
            s = (ex * ay - ey * ax) / denom;
            if (t < 0.0 || t > 1.0 || s < 0.0 || s > 1.0)
                continue;

            if (!d2d_array_reserve((void **)&bands, &size, count + 1, sizeof(*bands)))
            {
                free(bands);
                return FALSE;
            }
            bands[count++] = a->top.y + t * ay;
        }
    }

    qsort(bands, count, sizeof(*bands), d2d_combine_float_compare);
    for (size_t i = 0, j = 0; i < count; ++i)
    {
        if (j && bands[j - 1] == bands[i])
            continue;
        bands[j++] = bands[i];
        *out_count = j;
    }

    *out = bands;
    return TRUE;
}

/* Combine the two operands by sweeping the horizontal bands their edges span.
 * Within a band the coverage of either operand only changes at an edge, and
 * the edges do not cross, so a single pass over the edges in their order at
 * the middle of the band yields the result exactly, as one trapezoid per
 * covered interval. */
static HRESULT d2d_combine_edges_op(const struct d2d_combine_edges *edges,
        D2D1_FILL_MODE fill_mode0, D2D1_FILL_MODE fill_mode1, D2D1_COMBINE_MODE mode,
        struct d2d_combine_trapezoid **out, size_t *out_count)
{
    struct d2d_combine_trapezoid *traps = NULL;
    struct d2d_combine_event *events = NULL;
    size_t traps_size = 0, count = 0;
    size_t band_count = 0;
    size_t events_size = 0;
    float *bands = NULL;
    HRESULT hr = S_OK;

    *out = NULL;
    *out_count = 0;

    if (!d2d_combine_edges_bands(edges, &bands, &band_count))
        return E_OUTOFMEMORY;

    if (!d2d_array_reserve((void **)&events, &events_size, edges->count, sizeof(*events)))
    {
        free(bands);
        return E_OUTOFMEMORY;
    }

    for (size_t band = 0; band + 1 < band_count; ++band)
    {
        float top = bands[band], bottom = bands[band + 1];
        const struct d2d_combine_edge *left = NULL;
        unsigned int crossings[2] = {0};
        BOOL inside[2] = {FALSE};
        int winding[2] = {0};
        size_t event_count = 0;

        if (!(bottom > top))
            continue;

        for (size_t i = 0; i < edges->count; ++i)
        {
            const struct d2d_combine_edge *edge = &edges->edges[i];

            if (edge->top.y > top || edge->bottom.y < bottom)
                continue;
            events[event_count].x = d2d_combine_edge_x_at(edge, top)
                    + d2d_combine_edge_x_at(edge, bottom);
            events[event_count++].edge = edge;
        }

        qsort(events, event_count, sizeof(*events), d2d_combine_event_compare);

        for (size_t i = 0; i < event_count; )
        {
            BOOL was_inside = d2d_combine_op(mode, inside[0], inside[1]);
            const struct d2d_combine_edge *last = NULL;
            double x = events[i].x;

            while (i < event_count && events[i].x == x)
            {
                const struct d2d_combine_edge *edge = events[i].edge;

                winding[edge->side] += edge->winding;
                ++crossings[edge->side];
                last = edge;
                ++i;
            }

            inside[0] = d2d_combine_inside(fill_mode0, winding[0], crossings[0]);
            inside[1] = d2d_combine_inside(fill_mode1, winding[1], crossings[1]);

            if (!was_inside && d2d_combine_op(mode, inside[0], inside[1]))
            {
                left = last;
            }
            else if (was_inside && !d2d_combine_op(mode, inside[0], inside[1]) && left)
            {
                struct d2d_combine_trapezoid t;

                t.top = top;
                t.bottom = bottom;
                t.l0 = d2d_combine_edge_x_at(left, top);
                t.l1 = d2d_combine_edge_x_at(left, bottom);
                t.r0 = d2d_combine_edge_x_at(last, top);
                t.r1 = d2d_combine_edge_x_at(last, bottom);
                t.left = left - edges->edges;
                t.right = last - edges->edges;

                if (t.l0 < t.r0 || t.l1 < t.r1)
                {
                    if (!d2d_array_reserve((void **)&traps, &traps_size, count + 1, sizeof(*traps)))
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                    traps[count++] = t;
                }
            }
        }
    }

    *out = traps;
    *out_count = count;
    traps = NULL;

done:
    free(traps);
    free(events);
    free(bands);
    return hr;
}

/* Outline tracing over the trapezoids of the sweep.
 *
 * Writing one closed figure per trapezoid leaves interior edges in the result:
 * where two trapezoids meet along a band boundary, the shared edge is present
 * twice, and both the outline and any consumer that walks the figures see a
 * boundary that is not one. Cancelling those edges out turns the trapezoid
 * decomposition back into the outline of the combination, which is what a
 * boolean operation is supposed to produce.
 *
 * Every trapezoid contributes four directed edges, wound clockwise in a y-down
 * coordinate system. Trapezoids within one band are separated by uncovered
 * intervals, so only the horizontal edges along the band boundaries are ever
 * shared, and a shared interval is contributed once in each direction: summing
 * the directions over each elementary interval and keeping only the intervals
 * with a non-zero sum leaves exactly the boundary. The surviving edges are
 * then chained into closed contours. */
struct d2d_combine_span
{
    float pos;
    float a, b;
    int dir;
};

struct d2d_combine_outline_edge
{
    D2D1_POINT_2F p0, p1;
    size_t source;
    BOOL used;
};

static int __cdecl d2d_combine_span_compare(const void *a, const void *b)
{
    const struct d2d_combine_span *p = a, *q = b;

    if (p->pos != q->pos)
        return p->pos < q->pos ? -1 : 1;
    if (p->a != q->a)
        return p->a < q->a ? -1 : 1;
    return 0;
}

static BOOL d2d_combine_outline_add_edge(struct d2d_combine_outline_edge **edges, size_t *count,
        size_t *size, const D2D1_POINT_2F *p0, const D2D1_POINT_2F *p1, size_t source)
{
    if (!d2d_array_reserve((void **)edges, size, *count + 1, sizeof(**edges)))
        return FALSE;

    (*edges)[*count].p0 = *p0;
    (*edges)[*count].p1 = *p1;
    (*edges)[*count].source = source;
    (*edges)[(*count)++].used = FALSE;
    return TRUE;
}

/* Reduce the group of horizontal spans at each band boundary to the intervals
 * that are on the boundary, and emit those as directed edges. */
static BOOL d2d_combine_outline_trace_spans(struct d2d_combine_span *spans, size_t count,
        struct d2d_combine_outline_edge **edges, size_t *edge_count, size_t *edge_size)
{
    float *coords = NULL;
    size_t coords_size = 0;
    BOOL ret = FALSE;

    qsort(spans, count, sizeof(*spans), d2d_combine_span_compare);

    for (size_t i = 0; i < count; )
    {
        size_t group_end = i, coord_count = 0;
        float pos = spans[i].pos;

        while (group_end < count && spans[group_end].pos == pos)
            ++group_end;

        if (!d2d_array_reserve((void **)&coords, &coords_size, 2 * (group_end - i), sizeof(*coords)))
            goto done;
        for (size_t j = i; j < group_end; ++j)
        {
            coords[coord_count++] = spans[j].a;
            coords[coord_count++] = spans[j].b;
        }
        qsort(coords, coord_count, sizeof(*coords), d2d_combine_float_compare);

        for (size_t k = 0; k + 1 < coord_count; ++k)
        {
            float lo = coords[k], hi = coords[k + 1], mid;
            D2D1_POINT_2F p0, p1;
            int winding = 0;

            if (!(hi > lo))
                continue;
            mid = lo + (hi - lo) * 0.5f;

            for (size_t j = i; j < group_end; ++j)
            {
                if (spans[j].a <= mid && mid <= spans[j].b)
                    winding += spans[j].dir;
            }
            if (!winding)
                continue;

            /* A positive winding is a top edge, running right; a negative one
             * is a bottom edge, running left. */
            d2d_point_set(&p0, winding > 0 ? lo : hi, pos);
            d2d_point_set(&p1, winding > 0 ? hi : lo, pos);

            if (!d2d_combine_outline_add_edge(edges, edge_count, edge_size, &p0, &p1, SIZE_MAX))
                goto done;
        }

        i = group_end;
    }

    ret = TRUE;

done:
    free(coords);
    return ret;
}

/* Pick the continuation that turns as far clockwise as possible. At a point
 * where two parts of the boundary touch this keeps the outer contour connected
 * instead of cutting across the touch. Clockwise in a y-down system means a
 * positive cross product, and turning back is ranked below everything else. */
static size_t d2d_combine_outline_next(const struct d2d_combine_outline_edge *edges, size_t count,
        const struct d2d_combine_outline_edge *incoming)
{
    double dx = (double)incoming->p1.x - incoming->p0.x, dy = (double)incoming->p1.y - incoming->p0.y;
    const D2D1_POINT_2F *point = &incoming->p1;
    double best_angle = 0.0;
    size_t best = count;

    for (size_t i = 0; i < count; ++i)
    {
        double ex, ey, cross, dot, angle;

        if (edges[i].used || edges[i].p0.x != point->x || edges[i].p0.y != point->y)
            continue;

        ex = (double)edges[i].p1.x - edges[i].p0.x;
        ey = (double)edges[i].p1.y - edges[i].p0.y;
        cross = dx * ey - dy * ex;
        dot = dx * ex + dy * ey;
        if (cross == 0.0 && dot < 0.0)
            angle = -M_PI;
        else
            angle = atan2(cross, dot);

        if (best == count || angle > best_angle)
        {
            best_angle = angle;
            best = i;
        }
    }

    return best;
}

/* Whether two consecutive boundary edges continue the same straight line, so
 * that the vertex between them is redundant. Segments cut from the same sweep
 * edge are collinear by construction as long as they run the same way, and
 * horizontal segments compare by direction; oblique segments from different
 * sweep edges keep their vertex. */
static BOOL d2d_combine_outline_edge_continues(const struct d2d_combine_outline_edge *prev,
        const struct d2d_combine_outline_edge *next)
{
    if (prev->source != SIZE_MAX && prev->source == next->source)
        return (prev->p1.y > prev->p0.y) == (next->p1.y > next->p0.y);
    if (prev->p0.y == prev->p1.y && next->p0.y == next->p1.y)
        return (prev->p1.x > prev->p0.x) == (next->p1.x > next->p0.x);
    if (prev->p0.x == prev->p1.x && next->p0.x == next->p1.x)
        return (prev->p1.y > prev->p0.y) == (next->p1.y > next->p0.y);
    return FALSE;
}

/* Build the boundary of the trapezoid decomposition as closed contours.
 * Returns FALSE if the boundary could not be traced, in which case the caller
 * falls back to writing the trapezoids individually. Nothing is written to the
 * sink here, so that fallback stays safe. */
static BOOL d2d_combine_trapezoids_to_outline(const struct d2d_combine_trapezoid *traps, size_t count,
        struct d2d_combine_shape *out)
{
    struct d2d_combine_outline_edge *edges = NULL;
    size_t edge_count = 0, edge_size = 0;
    struct d2d_combine_span *spans = NULL;
    size_t span_count = 0, spans_size = 0;
    BOOL ret = FALSE;

    memset(out, 0, sizeof(*out));
    out->fill_mode = D2D1_FILL_MODE_WINDING;

    if (!count)
        return FALSE;

    if (!d2d_array_reserve((void **)&spans, &spans_size, 2 * count, sizeof(*spans)))
        goto done;

    for (size_t i = 0; i < count; ++i)
    {
        if (traps[i].l0 < traps[i].r0)
        {
            spans[span_count].pos = traps[i].top;
            spans[span_count].a = traps[i].l0;
            spans[span_count].b = traps[i].r0;
            spans[span_count++].dir = 1;
        }
        if (traps[i].l1 < traps[i].r1)
        {
            spans[span_count].pos = traps[i].bottom;
            spans[span_count].a = traps[i].l1;
            spans[span_count].b = traps[i].r1;
            spans[span_count++].dir = -1;
        }
    }
    if (!d2d_combine_outline_trace_spans(spans, span_count, &edges, &edge_count, &edge_size))
        goto done;

    /* The oblique edges are never shared - within a band, trapezoids are
     * separated by uncovered intervals - so they go in as they are: the right
     * edge runs down, the left edge runs up. */
    for (size_t i = 0; i < count; ++i)
    {
        D2D1_POINT_2F p0, p1;

        d2d_point_set(&p0, traps[i].r0, traps[i].top);
        d2d_point_set(&p1, traps[i].r1, traps[i].bottom);
        if (!d2d_combine_outline_add_edge(&edges, &edge_count, &edge_size, &p0, &p1, traps[i].right))
            goto done;

        d2d_point_set(&p0, traps[i].l1, traps[i].bottom);
        d2d_point_set(&p1, traps[i].l0, traps[i].top);
        if (!d2d_combine_outline_add_edge(&edges, &edge_count, &edge_size, &p0, &p1, traps[i].left))
            goto done;
    }

    for (size_t i = 0; i < edge_count; ++i)
    {
        struct d2d_combine_contour *contour;
        size_t next, prev, first;
        D2D1_POINT_2F start;

        if (edges[i].used)
            continue;

        if (!(contour = d2d_combine_shape_add_contour(out)))
            goto done;

        start = edges[i].p0;
        first = next = i;
        prev = edge_count;
        do
        {
            const struct d2d_combine_outline_edge *e = &edges[next];

            /* Skip the vertex when the edge continues the previous one, so
             * that the segments collapse back into full edges. */
            if ((prev >= edge_count || !d2d_combine_outline_edge_continues(&edges[prev], e))
                    && !d2d_combine_contour_add_point(contour, &e->p0))
                goto done;

            edges[next].used = TRUE;
            prev = next;

            if (e->p1.x == start.x && e->p1.y == start.y)
                break;

            next = d2d_combine_outline_next(edges, edge_count, e);
        } while (next < edge_count);

        /* An unclosed contour means the edges did not cancel out as expected;
         * do not use a partial result. */
        if (next >= edge_count || contour->count < 3)
            goto done;

        /* If the traversal started in the middle of a straight edge, the first
         * vertex continues the closing edge and is redundant. */
        if (contour->count > 3 && d2d_combine_outline_edge_continues(&edges[prev], &edges[first]))
        {
            memmove(contour->points, contour->points + 1, (contour->count - 1) * sizeof(*contour->points));
            --contour->count;
        }
    }

    ret = TRUE;

done:
    free(edges);
    free(spans);
    if (!ret)
        d2d_combine_shape_cleanup(out);
    return ret;
}

static void d2d_combine_write_trapezoids(ID2D1SimplifiedGeometrySink *sink,
        const struct d2d_combine_trapezoid *traps, size_t count)
{
    ID2D1SimplifiedGeometrySink_SetFillMode(sink, D2D1_FILL_MODE_WINDING);

    for (size_t i = 0; i < count; ++i)
    {
        D2D1_POINT_2F p[4];
        UINT32 n = 0;

        d2d_point_set(&p[n++], traps[i].l0, traps[i].top);
        if (traps[i].r0 > traps[i].l0)
            d2d_point_set(&p[n++], traps[i].r0, traps[i].top);
        d2d_point_set(&p[n++], traps[i].r1, traps[i].bottom);
        if (traps[i].l1 < traps[i].r1)
            d2d_point_set(&p[n++], traps[i].l1, traps[i].bottom);

        ID2D1SimplifiedGeometrySink_BeginFigure(sink, p[0], D2D1_FIGURE_BEGIN_FILLED);
        ID2D1SimplifiedGeometrySink_AddLines(sink, &p[1], n - 1);
        ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    }
}

static void d2d_combine_write_shape(ID2D1SimplifiedGeometrySink *sink, const struct d2d_combine_shape *shape)
{
    ID2D1SimplifiedGeometrySink_SetFillMode(sink, shape->fill_mode);

    for (size_t i = 0; i < shape->count; ++i)
    {
        const struct d2d_combine_contour *contour = &shape->contours[i];

        ID2D1SimplifiedGeometrySink_BeginFigure(sink, contour->points[0], D2D1_FIGURE_BEGIN_FILLED);
        ID2D1SimplifiedGeometrySink_AddLines(sink, &contour->points[1], contour->count - 1);
        ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    }
}

static HRESULT d2d_geometry_combine(ID2D1Geometry *geometry, ID2D1Geometry *geometry2,
        D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_combine_trapezoid *traps = NULL;
    struct d2d_combine_shape shape1, shape2;
    struct d2d_combine_edges edges = {0};
    size_t trap_count = 0;
    HRESULT hr;

    if (!geometry2 || !sink)
        return E_INVALIDARG;

    if (combine_mode > D2D1_COMBINE_MODE_EXCLUDE)
        return E_INVALIDARG;

    if (FAILED(hr = d2d_combine_shape_init(&shape1, geometry, NULL, tolerance)))
        return hr;
    if (FAILED(hr = d2d_combine_shape_init(&shape2, geometry2, transform, tolerance)))
    {
        d2d_combine_shape_cleanup(&shape1);
        return hr;
    }

    if (shape1.has_curves || shape2.has_curves)
    {
        FIXME("Curves are not handled, ignoring.\n");
        hr = E_NOTIMPL;
        goto done;
    }

    if (!d2d_combine_edges_add_shape(&edges, &shape1, 0)
            || !d2d_combine_edges_add_shape(&edges, &shape2, 1))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    if (SUCCEEDED(hr = d2d_combine_edges_op(&edges, shape1.fill_mode, shape2.fill_mode,
            combine_mode, &traps, &trap_count)))
    {
        struct d2d_combine_shape outline;

        /* Prefer the outline over the individual trapezoids: the band
         * decomposition is an artefact of the algorithm, not a property of
         * the result, and interior edges have no business showing up in a
         * boolean operation's output. */
        if (d2d_combine_trapezoids_to_outline(traps, trap_count, &outline))
        {
            d2d_combine_write_shape(sink, &outline);
            d2d_combine_shape_cleanup(&outline);
        }
        else
        {
            d2d_combine_write_trapezoids(sink, traps, trap_count);
        }
        free(traps);
    }

done:
    free(edges.edges);
    d2d_combine_shape_cleanup(&shape1);
    d2d_combine_shape_cleanup(&shape2);
    return hr;
}

static HRESULT d2d_geometry_tessellate(ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1TessellationSink *sink)
{
    ID2D1PathGeometry *path_geometry;
    HRESULT hr;

    if (SUCCEEDED(hr = d2d_geometry_get_simplified(geometry, transform, tolerance, &path_geometry)))
    {
        struct d2d_geometry *path_impl = unsafe_impl_from_ID2D1Geometry((ID2D1Geometry *)path_geometry);
        D2D1_TRIANGLE t;

        for (size_t i = 0; i < path_impl->fill.face_count; ++i)
        {
            const struct d2d_face *face = &path_impl->fill.faces[i];

            t.point1 = path_impl->fill.vertices[face->v[0]];
            t.point2 = path_impl->fill.vertices[face->v[1]];
            t.point3 = path_impl->fill.vertices[face->v[2]];
            ID2D1TessellationSink_AddTriangles(sink, &t, 1);
        }

        ID2D1PathGeometry_Release(path_geometry);
    }

    return hr;
}

static float d2d_triangle_area(const D2D1_TRIANGLE *triangle)
{
    D2D1_POINT_2F point2, point3;

    /* Translate one vertex to origin */
    point2.x = triangle->point2.x - triangle->point1.x;
    point2.y = triangle->point2.y - triangle->point1.y;
    point3.x = triangle->point3.x - triangle->point1.x;
    point3.y = triangle->point3.y - triangle->point1.y;

    return 0.5f * fabsf(point2.x * point3.y - point3.x * point2.y);
}

static HRESULT d2d_geometry_compute_area(ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, float *ret)
{
    ID2D1PathGeometry *path_geometry;
    float area = 0.0f;
    HRESULT hr;

    if (SUCCEEDED(hr = d2d_geometry_get_simplified(geometry, transform, tolerance, &path_geometry)))
    {
        struct d2d_geometry *path_impl = unsafe_impl_from_ID2D1Geometry((ID2D1Geometry *)path_geometry);
        D2D1_TRIANGLE t;

        for (size_t i = 0; i < path_impl->fill.face_count; ++i)
        {
            const struct d2d_face *face = &path_impl->fill.faces[i];

            t.point1 = path_impl->fill.vertices[face->v[0]];
            t.point2 = path_impl->fill.vertices[face->v[1]];
            t.point3 = path_impl->fill.vertices[face->v[2]];
            area += d2d_triangle_area(&t);
        }

        *ret = area;

        ID2D1PathGeometry_Release(path_geometry);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Tessellate(ID2D1PathGeometry1 *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return d2d_geometry_tessellate(&geometry->ID2D1Geometry_iface, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_CombineWithGeometry(ID2D1PathGeometry1 *iface,
        ID2D1Geometry *geometry, D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *g = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry, combine_mode, transform, tolerance, sink);

    return d2d_geometry_combine(&g->ID2D1Geometry_iface, geometry, combine_mode, transform,
            tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Outline(ID2D1PathGeometry1 *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, transform %p, tolerance %.8e, sink %p stub!\n", iface, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_ComputeArea(ID2D1PathGeometry1 *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    return d2d_geometry_compute_area(&geometry->ID2D1Geometry_iface, transform, tolerance, area);
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_ComputeLength(ID2D1PathGeometry1 *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    FIXME("iface %p, transform %p, tolerance %.8e, length %p stub!\n", iface, transform, tolerance, length);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_ComputePointAtLength(ID2D1PathGeometry1 *iface, float length,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_POINT_2F *point, D2D1_POINT_2F *tangent)
{
    FIXME("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p stub!\n",
            iface, length, transform, tolerance, point, tangent);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Widen(ID2D1PathGeometry1 *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p stub!\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Open(ID2D1PathGeometry1 *iface, ID2D1GeometrySink **sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, sink %p.\n", iface, sink);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_INITIAL)
        return D2DERR_WRONG_STATE;

    *sink = &geometry->u.path.ID2D1GeometrySink_iface;
    ID2D1GeometrySink_AddRef(*sink);

    geometry->u.path.state = D2D_GEOMETRY_STATE_OPEN;

    return S_OK;
}

static inline void d2d_arc_transform(D2D1_ARC_SEGMENT *arc, const D2D1_MATRIX_3X2_F *transform)
{
    D2D1_MATRIX_3X2_F m = *transform;
    D2D_POINT_2F point;

    m._31 = 0.0f;
    m._32 = 0.0f;

    d2d_point_transform(&arc->point, transform, arc->point.x, arc->point.y);
    d2d_point_transform(&point, &m, arc->size.width, 0.0f);
    arc->size.width = d2d_point_length(&point);
    d2d_point_transform(&point, &m, 0.0f, arc->size.height);
    arc->size.height = d2d_point_length(&point);
}

static void d2d_path_geometry_transformed_stream(struct d2d_geometry *geometry,
        const D2D_MATRIX_3X2_F *transform, ID2D1GeometrySink *sink)
{
    unsigned int flags = 0;
    D2D1_POINT_2F point;

    for (size_t i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];
        union
        {
            const struct d2d_segment *segment;
            const struct d2d_segment_beziers *beziers;
            const struct d2d_segment_quadratic_beziers *quad_beziers;
            const struct d2d_segment_lines *lines;
            const struct d2d_segment_arcs *arcs;
        } s = { (struct d2d_segment *)figure->segments.data };
        size_t size = 0;

        d2d_point_transform(&point, transform, figure->vertices[0].x, figure->vertices[0].y);
        ID2D1GeometrySink_BeginFigure(sink, point, figure->flags & D2D_FIGURE_FLAG_HOLLOW ?
                D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED);

        for (size_t j = 0; j < figure->segments.count; ++j)
        {
            if (flags != s.segment->flags)
            {
                ID2D1GeometrySink_SetSegmentFlags(sink, s.segment->flags);
                flags = s.segment->flags;
            }

            switch (s.segment->type)
            {
                case D2D_SEGMENT_TYPE_BEZIERS:
                    for (unsigned int k = 0; k < s.beziers->count; ++k)
                    {
                        D2D1_BEZIER_SEGMENT bezier_segment;
                        d2d_point_transform(&bezier_segment.point1, transform, s.beziers->segments[k].point1.x, s.beziers->segments[k].point1.y);
                        d2d_point_transform(&bezier_segment.point2, transform, s.beziers->segments[k].point2.x, s.beziers->segments[k].point2.y);
                        d2d_point_transform(&bezier_segment.point3, transform, s.beziers->segments[k].point3.x, s.beziers->segments[k].point3.y);
                        ID2D1GeometrySink_AddBeziers(sink, &bezier_segment, 1);
                    }
                    size = FIELD_OFFSET(struct d2d_segment_beziers, segments[s.beziers->count]);
                    break;
                case D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS:
                    for (unsigned int k = 0; k < s.quad_beziers->count; ++k)
                    {
                        D2D1_QUADRATIC_BEZIER_SEGMENT bezier_segment;
                        d2d_point_transform(&bezier_segment.point1, transform, s.quad_beziers->segments[k].point1.x, s.quad_beziers->segments[k].point1.y);
                        d2d_point_transform(&bezier_segment.point2, transform, s.quad_beziers->segments[k].point2.x, s.quad_beziers->segments[k].point2.y);
                        ID2D1GeometrySink_AddQuadraticBeziers(sink, &bezier_segment, 1);
                    }
                    size = FIELD_OFFSET(struct d2d_segment_quadratic_beziers, segments[s.quad_beziers->count]);
                    break;
                case D2D_SEGMENT_TYPE_LINES:
                    for (unsigned int k = 0; k < s.lines->count; ++k)
                    {
                        d2d_point_transform(&point, transform, s.lines->points[k].x, s.lines->points[k].y);
                        ID2D1GeometrySink_AddLines(sink, &point, 1);
                    }
                    size = FIELD_OFFSET(struct d2d_segment_lines, points[s.lines->count]);
                    break;
                case D2D_SEGMENT_TYPE_ARCS:
                    for (unsigned int k = 0; k < s.arcs->count; ++k)
                    {
                        D2D1_ARC_SEGMENT arc = s.arcs->segments[k];

                        d2d_arc_transform(&arc, transform);
                        ID2D1GeometrySink_AddArc(sink, &arc);
                    }
                    size = FIELD_OFFSET(struct d2d_segment_arcs, segments[s.arcs->count]);
                    break;
                default:
                    ;
            }

            s.segment = (struct d2d_segment *)((uint8_t *)s.segment + size);
        }

        ID2D1GeometrySink_EndFigure(sink, figure->flags & D2D_FIGURE_FLAG_CLOSED ?
                D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
}

static void d2d_path_geometry_stream(struct d2d_geometry *geometry, const D2D_MATRIX_3X2_F *transform,
        ID2D1GeometrySink *sink)
{
    unsigned int flags = 0;

    if (transform)
        return d2d_path_geometry_transformed_stream(geometry, transform, sink);

    for (size_t i = 0; i < geometry->u.path.figure_count; ++i)
    {
        const struct d2d_figure *figure = &geometry->u.path.figures[i];
        union
        {
            const struct d2d_segment *segment;
            const struct d2d_segment_beziers *beziers;
            const struct d2d_segment_quadratic_beziers *quad_beziers;
            const struct d2d_segment_lines *lines;
            const struct d2d_segment_arcs *arcs;
        } s = { (struct d2d_segment *)figure->segments.data };
        size_t size = 0;

        ID2D1GeometrySink_BeginFigure(sink, figure->vertices[0], figure->flags & D2D_FIGURE_FLAG_HOLLOW ?
                D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED);

        for (size_t j = 0; j < figure->segments.count; ++j)
        {
            if (flags != s.segment->flags)
            {
                ID2D1GeometrySink_SetSegmentFlags(sink, s.segment->flags);
                flags = s.segment->flags;
            }

            switch (s.segment->type)
            {
                case D2D_SEGMENT_TYPE_BEZIERS:
                    ID2D1GeometrySink_AddBeziers(sink, s.beziers->segments, s.beziers->count);
                    size = FIELD_OFFSET(struct d2d_segment_beziers, segments[s.beziers->count]);
                    break;
                case D2D_SEGMENT_TYPE_QUADRATIC_BEZIERS:
                    ID2D1GeometrySink_AddQuadraticBeziers(sink, s.quad_beziers->segments, s.quad_beziers->count);
                    size = FIELD_OFFSET(struct d2d_segment_quadratic_beziers, segments[s.quad_beziers->count]);
                    break;
                case D2D_SEGMENT_TYPE_LINES:
                    ID2D1GeometrySink_AddLines(sink, s.lines->points, s.lines->count);
                    size = FIELD_OFFSET(struct d2d_segment_lines, points[s.lines->count]);
                    break;
                case D2D_SEGMENT_TYPE_ARCS:
                    for (unsigned int k = 0; k < s.arcs->count; ++k)
                        ID2D1GeometrySink_AddArc(sink, &s.arcs->segments[k]);
                    size = FIELD_OFFSET(struct d2d_segment_arcs, segments[s.arcs->count]);
                    break;
                default:
                    ;
            }

            s.segment = (struct d2d_segment *)((uint8_t *)s.segment + size);
        }

        ID2D1GeometrySink_EndFigure(sink, figure->flags & D2D_FIGURE_FLAG_CLOSED ?
                D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_Stream(ID2D1PathGeometry1 *iface, ID2D1GeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, sink %p.\n", iface, sink);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_CLOSED)
        return D2DERR_WRONG_STATE;

    d2d_path_geometry_stream(geometry, NULL, sink);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_GetSegmentCount(ID2D1PathGeometry1 *iface, UINT32 *count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, count %p.\n", iface, count);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_CLOSED)
        return D2DERR_WRONG_STATE;

    *count = geometry->u.path.segment_count;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry_GetFigureCount(ID2D1PathGeometry1 *iface, UINT32 *count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1PathGeometry1(iface);

    TRACE("iface %p, count %p.\n", iface, count);

    if (geometry->u.path.state != D2D_GEOMETRY_STATE_CLOSED)
        return D2DERR_WRONG_STATE;

    *count = geometry->u.path.figure_count;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_path_geometry1_ComputePointAndSegmentAtLength(ID2D1PathGeometry1 *iface,
        float length, UINT32 start_segment, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        D2D1_POINT_DESCRIPTION *point_desc)
{
    FIXME("iface %p, length %.8e, start_segment %u, transform %p, tolerance %.8e, point_desc %p.\n",
            iface, length, start_segment, transform, tolerance, point_desc);

    return E_NOTIMPL;
}

static const struct ID2D1PathGeometry1Vtbl d2d_path_geometry_vtbl =
{
    d2d_path_geometry_QueryInterface,
    d2d_path_geometry_AddRef,
    d2d_path_geometry_Release,
    d2d_path_geometry_GetFactory,
    d2d_path_geometry_GetBounds,
    d2d_path_geometry_GetWidenedBounds,
    d2d_path_geometry_StrokeContainsPoint,
    d2d_path_geometry_FillContainsPoint,
    d2d_path_geometry_CompareWithGeometry,
    d2d_path_geometry_Simplify,
    d2d_path_geometry_Tessellate,
    d2d_path_geometry_CombineWithGeometry,
    d2d_path_geometry_Outline,
    d2d_path_geometry_ComputeArea,
    d2d_path_geometry_ComputeLength,
    d2d_path_geometry_ComputePointAtLength,
    d2d_path_geometry_Widen,
    d2d_path_geometry_Open,
    d2d_path_geometry_Stream,
    d2d_path_geometry_GetSegmentCount,
    d2d_path_geometry_GetFigureCount,
    d2d_path_geometry1_ComputePointAndSegmentAtLength,
};

static const struct d2d_geometry_ops d2d_path_geometry_ops =
{
    .stream = d2d_path_geometry_stream,
};

void d2d_path_geometry_init(struct d2d_geometry *geometry, ID2D1Factory *factory)
{
    d2d_geometry_init(geometry, factory, &identity, (ID2D1GeometryVtbl *)&d2d_path_geometry_vtbl,
            &d2d_path_geometry_ops);
    geometry->u.path.ID2D1GeometrySink_iface.lpVtbl = &d2d_geometry_sink_vtbl;
    geometry->u.path.bounds.left = FLT_MAX;
    geometry->u.path.bounds.right = -FLT_MAX;
    geometry->u.path.bounds.top = FLT_MAX;
    geometry->u.path.bounds.bottom = -FLT_MAX;
}

static inline struct d2d_geometry *impl_from_ID2D1EllipseGeometry(ID2D1EllipseGeometry *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_QueryInterface(ID2D1EllipseGeometry *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1EllipseGeometry)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1EllipseGeometry_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_ellipse_geometry_AddRef(ID2D1EllipseGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_ellipse_geometry_Release(ID2D1EllipseGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_ellipse_geometry_GetFactory(ID2D1EllipseGeometry *iface, ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_GetBounds(ID2D1EllipseGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    FIXME("iface %p, transform %p, bounds %p stub!\n", iface, transform, bounds);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_GetWidenedBounds(ID2D1EllipseGeometry *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_RECT_F *bounds)
{
    D2D1_RECT_F fill_bounds;
    HRESULT hr;

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    if (FAILED(hr = ID2D1EllipseGeometry_GetBounds(iface, transform, &fill_bounds)))
        return hr;

    d2d_geometry_widen_bounds(&fill_bounds, stroke_width, stroke_style, bounds);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_StrokeContainsPoint(ID2D1EllipseGeometry *iface,
        D2D1_POINT_2F point, float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, BOOL *contains)
{
    FIXME("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p stub!\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_FillContainsPoint(ID2D1EllipseGeometry *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    FIXME("iface %p, point %s, transform %p, tolerance %.8e, contains %p stub!\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_CompareWithGeometry(ID2D1EllipseGeometry *iface,
        ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    FIXME("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p stub!\n",
            iface, geometry, transform, tolerance, relation);

    return E_NOTIMPL;
}

static void d2d_ellipse_to_segments(const D2D1_ELLIPSE *ellipse, D2D1_POINT_2F *start_point,
        D2D1_BEZIER_SEGMENT *segments)
{
    const float coeff = 4.0f * (M_SQRT2 - 1.0f) / 3.0f;
    D2D1_MATRIX_3X2_F m;
    unsigned int i;

    /* Use four Bézier segments to approximate a unit circle.
       Endpoints tangents are tangential to the circle. Endpoints and the midpoint
       are lying on the circle. */

    d2d_point_set(start_point, -1.0f, 0.0f);

    d2d_point_set(&segments[0].point1, -1.0f, -coeff);
    d2d_point_set(&segments[0].point2, -coeff, -1.0f);
    d2d_point_set(&segments[0].point3, 0.0f, -1.0f);

    d2d_point_set(&segments[1].point1, coeff, -1.0f);
    d2d_point_set(&segments[1].point2, 1.0f, -coeff);
    d2d_point_set(&segments[1].point3, 1.0f, 0.0f);

    d2d_point_set(&segments[2].point1, 1.0f, coeff);
    d2d_point_set(&segments[2].point2, coeff, 1.0f);
    d2d_point_set(&segments[2].point3, 0.0f, 1.0f);

    d2d_point_set(&segments[3].point1, -coeff, 1.0f);
    d2d_point_set(&segments[3].point2, -1.0f, coeff);
    d2d_point_set(&segments[3].point3, start_point->x, start_point->y);

    m._11 = ellipse->radiusX;
    m._12 = 0.0f;
    m._21 = 0.0f;
    m._22 = ellipse->radiusY;
    m._31 = ellipse->point.x;
    m._32 = ellipse->point.y;

    d2d_point_transform(start_point, &m, start_point->x, start_point->y);
    for (i = 0; i < 4; ++i)
    {
        d2d_point_transform(&segments[i].point1, &m, segments[i].point1.x, segments[i].point1.y);
        d2d_point_transform(&segments[i].point2, &m, segments[i].point2.x, segments[i].point2.y);
        d2d_point_transform(&segments[i].point3, &m, segments[i].point3.x, segments[i].point3.y);
    }
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_Simplify(ID2D1EllipseGeometry *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);
    struct d2d_figure figure = { 0 };
    D2D1_BEZIER_SEGMENT segments[4];
    D2D1_POINT_2F start_point;

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    d2d_ellipse_to_segments(&geometry->u.ellipse.ellipse, &start_point, segments);

    if (!d2d_figure_begin(&figure, start_point, D2D1_FIGURE_BEGIN_FILLED))
        return E_OUTOFMEMORY;
    if (!d2d_figure_add_beziers(&figure, segments, ARRAY_SIZE(segments)))
    {
        d2d_figure_cleanup(&figure);
        return E_OUTOFMEMORY;
    }
    d2d_figure_end(&figure, D2D1_FIGURE_END_CLOSED);

    d2d_figure_simplify(&figure, option, transform, tolerance, sink);
    d2d_figure_cleanup(&figure);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_Tessellate(ID2D1EllipseGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return d2d_geometry_tessellate(&geometry->ID2D1Geometry_iface, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_CombineWithGeometry(ID2D1EllipseGeometry *iface,
        ID2D1Geometry *geometry, D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *g = impl_from_ID2D1EllipseGeometry(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry, combine_mode, transform, tolerance, sink);

    return d2d_geometry_combine(&g->ID2D1Geometry_iface, geometry, combine_mode, transform,
            tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_Outline(ID2D1EllipseGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, transform %p, tolerance %.8e, sink %p stub!\n", iface, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_ComputeArea(ID2D1EllipseGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    return d2d_geometry_compute_area(&geometry->ID2D1Geometry_iface, transform, tolerance, area);
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_ComputeLength(ID2D1EllipseGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    FIXME("iface %p, transform %p, tolerance %.8e, length %p stub!\n", iface, transform, tolerance, length);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_ComputePointAtLength(ID2D1EllipseGeometry *iface,
        float length, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_POINT_2F *point,
        D2D1_POINT_2F *tangent)
{
    FIXME("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p stub!\n",
            iface, length, transform, tolerance, point, tangent);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_ellipse_geometry_Widen(ID2D1EllipseGeometry *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p stub!\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_ellipse_geometry_GetEllipse(ID2D1EllipseGeometry *iface, D2D1_ELLIPSE *ellipse)
{
    struct d2d_geometry *geometry = impl_from_ID2D1EllipseGeometry(iface);

    TRACE("iface %p, ellipse %p.\n", iface, ellipse);

    *ellipse = geometry->u.ellipse.ellipse;
}

static const struct ID2D1EllipseGeometryVtbl d2d_ellipse_geometry_vtbl =
{
    d2d_ellipse_geometry_QueryInterface,
    d2d_ellipse_geometry_AddRef,
    d2d_ellipse_geometry_Release,
    d2d_ellipse_geometry_GetFactory,
    d2d_ellipse_geometry_GetBounds,
    d2d_ellipse_geometry_GetWidenedBounds,
    d2d_ellipse_geometry_StrokeContainsPoint,
    d2d_ellipse_geometry_FillContainsPoint,
    d2d_ellipse_geometry_CompareWithGeometry,
    d2d_ellipse_geometry_Simplify,
    d2d_ellipse_geometry_Tessellate,
    d2d_ellipse_geometry_CombineWithGeometry,
    d2d_ellipse_geometry_Outline,
    d2d_ellipse_geometry_ComputeArea,
    d2d_ellipse_geometry_ComputeLength,
    d2d_ellipse_geometry_ComputePointAtLength,
    d2d_ellipse_geometry_Widen,
    d2d_ellipse_geometry_GetEllipse,
};

static void d2d_ellipse_geometry_stream(struct d2d_geometry *geometry, const D2D_MATRIX_3X2_F *transform,
        ID2D1GeometrySink *sink)
{
    const D2D1_ELLIPSE *e = &geometry->u.ellipse.ellipse;
    D2D1_POINT_2F start_point;
    D2D1_ARC_SEGMENT arcs[4];

    arcs[0].size.width = e->radiusX;
    arcs[0].size.height = e->radiusY;
    arcs[0].rotationAngle = 0.0f;
    arcs[0].sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arcs[0].arcSize = D2D1_ARC_SIZE_SMALL;
    arcs[1] = arcs[2] = arcs[3] = arcs[0];

    d2d_point_set(&start_point, e->point.x - e->radiusX, e->point.y);
    d2d_point_set(&arcs[0].point, e->point.x, e->point.y - e->radiusY);
    d2d_point_set(&arcs[1].point, e->point.x + e->radiusX, e->point.y);
    d2d_point_set(&arcs[2].point, e->point.x, e->point.y + e->radiusY);
    d2d_point_set(&arcs[3].point, start_point.x, start_point.y);

    if (transform)
    {
        d2d_point_transform(&start_point, transform, start_point.x, start_point.y);
        for (int i = 0; i < ARRAYSIZE(arcs); ++i)
            d2d_arc_transform(&arcs[i], transform);
    }

    ID2D1GeometrySink_BeginFigure(sink, start_point, D2D1_FIGURE_BEGIN_FILLED);

    for (int i = 0; i < ARRAYSIZE(arcs); ++i)
        ID2D1GeometrySink_AddArc(sink, &arcs[i]);

    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
}

static const struct d2d_geometry_ops d2d_ellipse_geometry_ops =
{
    .stream = d2d_ellipse_geometry_stream,
};

HRESULT d2d_ellipse_geometry_init(struct d2d_geometry *geometry, ID2D1Factory *factory, const D2D1_ELLIPSE *ellipse)
{
    D2D1_POINT_2F *v, v1, v2, v3, v4;
    struct d2d_face *f;
    float l, r, t, b;

    d2d_geometry_init(geometry, factory, &identity, (ID2D1GeometryVtbl *)&d2d_ellipse_geometry_vtbl,
            &d2d_ellipse_geometry_ops);
    geometry->u.ellipse.ellipse = *ellipse;

    if (!(geometry->fill.vertices = malloc(4 * sizeof(*geometry->fill.vertices))))
        goto fail;
    if (!d2d_array_reserve((void **)&geometry->fill.faces,
            &geometry->fill.faces_size, 2, sizeof(*geometry->fill.faces)))
        goto fail;

    l = ellipse->point.x - ellipse->radiusX;
    r = ellipse->point.x + ellipse->radiusX;
    t = ellipse->point.y - ellipse->radiusY;
    b = ellipse->point.y + ellipse->radiusY;

    d2d_point_set(&v1, r, t);
    d2d_point_set(&v2, r, b);
    d2d_point_set(&v3, l, b);
    d2d_point_set(&v4, l, t);

    v = geometry->fill.vertices;
    d2d_point_set(&v[0], ellipse->point.x, t);
    d2d_point_set(&v[1], r, ellipse->point.y);
    d2d_point_set(&v[2], ellipse->point.x, b);
    d2d_point_set(&v[3], l, ellipse->point.y);
    geometry->fill.vertex_count = 4;

    f = geometry->fill.faces;
    d2d_face_set(&f[0], 0, 3, 2);
    d2d_face_set(&f[1], 0, 2, 1);
    geometry->fill.face_count = 2;

    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[0], &v1, &v[1]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[1], &v2, &v[2]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[2], &v3, &v[3]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[3], &v4, &v[0]))
        goto fail;

    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[0], &v1, &v[1]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[1], &v2, &v[2]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[2], &v3, &v[3]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[3], &v4, &v[0]))
        goto fail;

    return S_OK;

fail:
    d2d_geometry_cleanup(geometry);
    return E_OUTOFMEMORY;
}

static inline struct d2d_geometry *impl_from_ID2D1RectangleGeometry(ID2D1RectangleGeometry *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_QueryInterface(ID2D1RectangleGeometry *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1RectangleGeometry)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1RectangleGeometry_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_rectangle_geometry_AddRef(ID2D1RectangleGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_rectangle_geometry_Release(ID2D1RectangleGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_rectangle_geometry_GetFactory(ID2D1RectangleGeometry *iface, ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_GetBounds(ID2D1RectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    D2D1_RECT_F *rect;
    D2D1_POINT_2F p;

    TRACE("iface %p, transform %p, bounds %p.\n", iface, transform, bounds);

    rect = &geometry->u.rectangle.rect;
    if (!transform)
    {
        *bounds = *rect;
        return S_OK;
    }

    bounds->left = FLT_MAX;
    bounds->top = FLT_MAX;
    bounds->right = -FLT_MAX;
    bounds->bottom = -FLT_MAX;

    d2d_point_transform(&p, transform, rect->left, rect->top);
    d2d_rect_expand(bounds, &p);
    d2d_point_transform(&p, transform, rect->left, rect->bottom);
    d2d_rect_expand(bounds, &p);
    d2d_point_transform(&p, transform, rect->right, rect->bottom);
    d2d_rect_expand(bounds, &p);
    d2d_point_transform(&p, transform, rect->right, rect->top);
    d2d_rect_expand(bounds, &p);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_GetWidenedBounds(ID2D1RectangleGeometry *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_RECT_F *bounds)
{
    D2D1_RECT_F fill_bounds;
    HRESULT hr;

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    if (FAILED(hr = ID2D1RectangleGeometry_GetBounds(iface, transform, &fill_bounds)))
        return hr;

    d2d_geometry_widen_bounds(&fill_bounds, stroke_width, stroke_style, bounds);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_StrokeContainsPoint(ID2D1RectangleGeometry *iface,
        D2D1_POINT_2F point, float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, BOOL *contains)
{
    const struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    const D2D1_RECT_F *rect = &geometry->u.rectangle.rect;
    unsigned int i;
    struct
    {
        D2D1_POINT_2F s, e;
    }
    segments[4];

    TRACE("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    if (stroke_style)
        FIXME("Ignoring stroke style %p.\n", stroke_style);

    tolerance = fabsf(tolerance);

    if (!transform)
    {
        D2D1_POINT_2F d, s;

        s.x = rect->right - rect->left;
        s.y = rect->bottom - rect->top;
        d.x = fabsf((rect->right + rect->left) * 0.5f - point.x);
        d.y = fabsf((rect->bottom + rect->top) * 0.5f - point.y);

        /* Inside test. */
        if (d.x <= (s.x - stroke_width) * 0.5f - tolerance && d.y <= (s.y - stroke_width) * 0.5f - tolerance)
        {
            *contains = FALSE;
            return S_OK;
        }

        if (tolerance == 0.0f)
        {
            *contains = d.x < (s.x + stroke_width) * 0.5f && d.y < (s.y + stroke_width) * 0.5f;
        }
        else
        {
            d.x = max(d.x - (s.x + stroke_width) * 0.5f, 0.0f);
            d.y = max(d.y - (s.y + stroke_width) * 0.5f, 0.0f);

            *contains = d2d_point_dot(&d, &d) < tolerance * tolerance;
        }

        return S_OK;
    }

    stroke_width *= 0.5f;

    d2d_point_set(&segments[0].s, rect->left - stroke_width, rect->bottom);
    d2d_point_set(&segments[0].e, rect->right + stroke_width, rect->bottom);
    d2d_point_set(&segments[1].s, rect->right, rect->bottom + stroke_width);
    d2d_point_set(&segments[1].e, rect->right, rect->top - stroke_width);
    d2d_point_set(&segments[2].s, rect->right + stroke_width, rect->top);
    d2d_point_set(&segments[2].e, rect->left - stroke_width, rect->top);
    d2d_point_set(&segments[3].s, rect->left, rect->top - stroke_width);
    d2d_point_set(&segments[3].e, rect->left, rect->bottom + stroke_width);

    *contains = FALSE;
    for (i = 0; i < ARRAY_SIZE(segments); ++i)
    {
        if (d2d_point_on_line_segment(&point, &segments[i].s, &segments[i].e, transform, stroke_width, tolerance))
        {
            *contains = TRUE;
            break;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_FillContainsPoint(ID2D1RectangleGeometry *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    D2D1_RECT_F *rect = &geometry->u.rectangle.rect;
    float dx, dy;

    TRACE("iface %p, point %s, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    if (transform)
    {
        D2D1_MATRIX_3X2_F g_i;

        if (!d2d_matrix_invert(&g_i, transform))
            return D2DERR_UNSUPPORTED_OPERATION;
        d2d_point_transform(&point, &g_i, point.x, point.y);
    }

    if (tolerance == 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    dx = max(fabsf((rect->right  + rect->left) / 2.0f - point.x) - (rect->right  - rect->left) / 2.0f, 0.0f);
    dy = max(fabsf((rect->bottom + rect->top)  / 2.0f - point.y) - (rect->bottom - rect->top)  / 2.0f, 0.0f);

    *contains = tolerance * tolerance > (dx * dx + dy * dy);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_CompareWithGeometry(ID2D1RectangleGeometry *iface,
        ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    FIXME("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p stub!\n",
            iface, geometry, transform, tolerance, relation);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_Simplify(ID2D1RectangleGeometry *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    D2D1_RECT_F *rect = &geometry->u.rectangle.rect;
    D2D1_POINT_2F p[4];
    unsigned int i;

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    d2d_point_set(&p[0], rect->left, rect->top);
    d2d_point_set(&p[1], rect->right, rect->top);
    d2d_point_set(&p[2], rect->right, rect->bottom);
    d2d_point_set(&p[3], rect->left, rect->bottom);

    if (transform)
    {
        for (i = 0; i < ARRAY_SIZE(p); ++i)
        {
            d2d_point_transform(&p[i], transform, p[i].x, p[i].y);
        }
    }

    ID2D1SimplifiedGeometrySink_SetFillMode(sink, D2D1_FILL_MODE_ALTERNATE);
    ID2D1SimplifiedGeometrySink_BeginFigure(sink, p[0], D2D1_FIGURE_BEGIN_FILLED);
    ID2D1SimplifiedGeometrySink_AddLines(sink, &p[1], 3);
    ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_Tessellate(ID2D1RectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return d2d_geometry_tessellate(&geometry->ID2D1Geometry_iface, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_CombineWithGeometry(ID2D1RectangleGeometry *iface,
        ID2D1Geometry *geometry, D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *g = impl_from_ID2D1RectangleGeometry(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry, combine_mode, transform, tolerance, sink);

    return d2d_geometry_combine(&g->ID2D1Geometry_iface, geometry, combine_mode, transform,
            tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_Outline(ID2D1RectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, transform %p, tolerance %.8e, sink %p stub!\n", iface, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_ComputeArea(ID2D1RectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);
    const D2D_RECT_F *rect = &geometry->u.rectangle.rect;
    D2D1_TRIANGLE triangle;
    D2D1_MATRIX_3X2_F m;

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    if (transform)
    {
        m = *transform;
        m._31 = m._32 = 0.0f;

        d2d_point_transform(&triangle.point1, &m, rect->left, rect->bottom);
        d2d_point_transform(&triangle.point2, &m, rect->left, rect->top);
        d2d_point_transform(&triangle.point3, &m, rect->right, rect->top);

        *area = 2 * d2d_triangle_area(&triangle);
    }
    else
    {
        *area = fabsf((rect->right - rect->left) * (rect->bottom - rect->top));
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_ComputeLength(ID2D1RectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    FIXME("iface %p, transform %p, tolerance %.8e, length %p stub!\n", iface, transform, tolerance, length);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_ComputePointAtLength(ID2D1RectangleGeometry *iface,
        float length, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_POINT_2F *point,
        D2D1_POINT_2F *tangent)
{
    FIXME("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p stub!\n",
            iface, length, transform, tolerance, point, tangent);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rectangle_geometry_Widen(ID2D1RectangleGeometry *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p stub!\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_rectangle_geometry_GetRect(ID2D1RectangleGeometry *iface, D2D1_RECT_F *rect)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RectangleGeometry(iface);

    TRACE("iface %p, rect %p.\n", iface, rect);

    *rect = geometry->u.rectangle.rect;
}

static const struct ID2D1RectangleGeometryVtbl d2d_rectangle_geometry_vtbl =
{
    d2d_rectangle_geometry_QueryInterface,
    d2d_rectangle_geometry_AddRef,
    d2d_rectangle_geometry_Release,
    d2d_rectangle_geometry_GetFactory,
    d2d_rectangle_geometry_GetBounds,
    d2d_rectangle_geometry_GetWidenedBounds,
    d2d_rectangle_geometry_StrokeContainsPoint,
    d2d_rectangle_geometry_FillContainsPoint,
    d2d_rectangle_geometry_CompareWithGeometry,
    d2d_rectangle_geometry_Simplify,
    d2d_rectangle_geometry_Tessellate,
    d2d_rectangle_geometry_CombineWithGeometry,
    d2d_rectangle_geometry_Outline,
    d2d_rectangle_geometry_ComputeArea,
    d2d_rectangle_geometry_ComputeLength,
    d2d_rectangle_geometry_ComputePointAtLength,
    d2d_rectangle_geometry_Widen,
    d2d_rectangle_geometry_GetRect,
};

static void d2d_rectangle_stream(const D2D1_RECT_F *rect, const D2D1_MATRIX_3X2_F *transform,
        ID2D1GeometrySink *sink)
{
    D2D1_POINT_2F p[4];
    unsigned int i;

    d2d_point_set(&p[0], rect->left, rect->top);
    d2d_point_set(&p[1], rect->right, rect->top);
    d2d_point_set(&p[2], rect->right, rect->bottom);
    d2d_point_set(&p[3], rect->left, rect->bottom);

    if (transform)
    {
        for (i = 0; i < ARRAY_SIZE(p); ++i)
        {
            d2d_point_transform(&p[i], transform, p[i].x, p[i].y);
        }
    }

    ID2D1GeometrySink_BeginFigure(sink, p[0], D2D1_FIGURE_BEGIN_FILLED);
    ID2D1GeometrySink_AddLines(sink, &p[1], 3);
    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
}

static void d2d_rectangle_geometry_stream(struct d2d_geometry *geometry,
        const D2D_MATRIX_3X2_F *transform, ID2D1GeometrySink *sink)
{
    d2d_rectangle_stream(&geometry->u.rectangle.rect, transform, sink);
}

static const struct d2d_geometry_ops d2d_rectangle_geometry_ops =
{
    .stream = d2d_rectangle_geometry_stream,
};

HRESULT d2d_rectangle_geometry_init(struct d2d_geometry *geometry, ID2D1Factory *factory, const D2D1_RECT_F *rect)
{
    struct d2d_face *f;
    D2D1_POINT_2F *v;
    float l, r, t, b;

    static const D2D1_POINT_2F prev[] =
    {
        { 1.0f,  0.0f},
        { 0.0f, -1.0f},
        {-1.0f,  0.0f},
        { 0.0f,  1.0f},
    };
    static const D2D1_POINT_2F next[] =
    {
        { 0.0f,  1.0f},
        { 1.0f,  0.0f},
        { 0.0f, -1.0f},
        {-1.0f,  0.0f},
    };

    d2d_geometry_init(geometry, factory, &identity, (ID2D1GeometryVtbl *)&d2d_rectangle_geometry_vtbl,
            &d2d_rectangle_geometry_ops);
    geometry->u.rectangle.rect = *rect;

    if (!(geometry->fill.vertices = malloc(4 * sizeof(*geometry->fill.vertices))))
        goto fail;
    if (!d2d_array_reserve((void **)&geometry->fill.faces,
            &geometry->fill.faces_size, 2, sizeof(*geometry->fill.faces)))
        goto fail;

    l = min(rect->left, rect->right);
    r = max(rect->left, rect->right);
    t = min(rect->top, rect->bottom);
    b = max(rect->top, rect->bottom);

    v = geometry->fill.vertices;
    d2d_point_set(&v[0], l, t);
    d2d_point_set(&v[1], l, b);
    d2d_point_set(&v[2], r, b);
    d2d_point_set(&v[3], r, t);
    geometry->fill.vertex_count = 4;

    f = geometry->fill.faces;
    d2d_face_set(&f[0], 1, 2, 0);
    d2d_face_set(&f[1], 0, 2, 3);
    geometry->fill.face_count = 2;

    if (!d2d_geometry_outline_add_line_segment(geometry, &v[0], &v[1]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[1], &v[2]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[2], &v[3]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[3], &v[0]))
        goto fail;

    if (!d2d_geometry_outline_add_join(geometry, &prev[0], &v[0], &next[0]))
        goto fail;
    if (!d2d_geometry_outline_add_join(geometry, &prev[1], &v[1], &next[1]))
        goto fail;
    if (!d2d_geometry_outline_add_join(geometry, &prev[2], &v[2], &next[2]))
        goto fail;
    if (!d2d_geometry_outline_add_join(geometry, &prev[3], &v[3], &next[3]))
        goto fail;

    return S_OK;

fail:
    d2d_geometry_cleanup(geometry);
    return E_OUTOFMEMORY;
}

/* Session 6 (C1): Re-initialize a rectangle geometry for a new rect, reusing
 * the existing fill/outline arrays instead of freeing + reallocating. The
 * arrays are already sized for 4 vertices + 8 outline verts + 8 joins after
 * the first FillRectangle call, so d2d_array_reserve becomes a no-op on every
 * subsequent call. Caller must ensure the geometry was previously initialised
 * as a rectangle geometry (so vtable, factory, transform, and arrays match). */
void d2d_rectangle_geometry_reinit(struct d2d_geometry *geometry, const D2D1_RECT_F *rect)
{
    static const D2D1_POINT_2F prev[] =
    {
        { 1.0f,  0.0f},
        { 0.0f, -1.0f},
        {-1.0f,  0.0f},
        { 0.0f,  1.0f},
    };
    static const D2D1_POINT_2F next[] =
    {
        { 0.0f,  1.0f},
        { 1.0f,  0.0f},
        { 0.0f, -1.0f},
        {-1.0f,  0.0f},
    };
    D2D1_POINT_2F *v;
    struct d2d_face *f;
    float l, r, t, b;

    /* Reset counts — arrays stay allocated. */
    geometry->fill.vertex_count = 0;
    geometry->fill.face_count = 0;
    geometry->fill.bezier_vertex_count = 0;
    geometry->fill.arc_vertex_count = 0;
    geometry->outline.vertex_count = 0;
    geometry->outline.face_count = 0;
    geometry->outline.bezier_count = 0;
    geometry->outline.bezier_face_count = 0;
    geometry->outline.arc_count = 0;
    geometry->outline.arc_face_count = 0;

    geometry->u.rectangle.rect = *rect;

    l = min(rect->left, rect->right);
    r = max(rect->left, rect->right);
    t = min(rect->top, rect->bottom);
    b = max(rect->top, rect->bottom);

    /* fill.vertices already allocated (4 slots) on first init — reuse. */
    v = geometry->fill.vertices;
    d2d_point_set(&v[0], l, t);
    d2d_point_set(&v[1], l, b);
    d2d_point_set(&v[2], r, b);
    d2d_point_set(&v[3], r, t);
    geometry->fill.vertex_count = 4;

    f = geometry->fill.faces;
    d2d_face_set(&f[0], 1, 2, 0);
    d2d_face_set(&f[1], 0, 2, 3);
    geometry->fill.face_count = 2;

    /* Outline tessellation — d2d_array_reserve is a no-op because arrays are
     * already sized from the first init (or a previous reinit with larger
     * rect). These calls just write into the existing buffers. */
    d2d_geometry_outline_add_line_segment(geometry, &v[0], &v[1]);
    d2d_geometry_outline_add_line_segment(geometry, &v[1], &v[2]);
    d2d_geometry_outline_add_line_segment(geometry, &v[2], &v[3]);
    d2d_geometry_outline_add_line_segment(geometry, &v[3], &v[0]);

    d2d_geometry_outline_add_join(geometry, &prev[0], &v[0], &next[0]);
    d2d_geometry_outline_add_join(geometry, &prev[1], &v[1], &next[1]);
    d2d_geometry_outline_add_join(geometry, &prev[2], &v[2], &next[2]);
    d2d_geometry_outline_add_join(geometry, &prev[3], &v[3], &next[3]);
}

static inline struct d2d_geometry *impl_from_ID2D1RoundedRectangleGeometry(ID2D1RoundedRectangleGeometry *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_QueryInterface(ID2D1RoundedRectangleGeometry *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1RoundedRectangleGeometry)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1RoundedRectangleGeometry_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_AddRef(ID2D1RoundedRectangleGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_Release(ID2D1RoundedRectangleGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_GetFactory(ID2D1RoundedRectangleGeometry *iface,
        ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_GetBounds(ID2D1RoundedRectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    FIXME("iface %p, transform %p, bounds %p stub!\n", iface, transform, bounds);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_GetWidenedBounds(ID2D1RoundedRectangleGeometry *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_RECT_F *bounds)
{
    D2D1_RECT_F fill_bounds;
    HRESULT hr;

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    if (FAILED(hr = ID2D1RoundedRectangleGeometry_GetBounds(iface, transform, &fill_bounds)))
        return hr;

    d2d_geometry_widen_bounds(&fill_bounds, stroke_width, stroke_style, bounds);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_StrokeContainsPoint(
        ID2D1RoundedRectangleGeometry *iface, D2D1_POINT_2F point, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    FIXME("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p stub!\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_FillContainsPoint(ID2D1RoundedRectangleGeometry *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    FIXME("iface %p, point %s, transform %p, tolerance %.8e, contains %p stub!\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_CompareWithGeometry(
        ID2D1RoundedRectangleGeometry *iface, ID2D1Geometry *geometry,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    FIXME("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p stub!\n",
            iface, geometry, transform, tolerance, relation);

    return E_NOTIMPL;
}

static inline void d2d_point_translate(D2D1_POINT_2F *point, float x, float y)
{
    point->x += x;
    point->y += y;
}

static inline void d2d_bezier_segment_translate(D2D1_BEZIER_SEGMENT *segment, float x, float y)
{
    d2d_point_translate(&segment->point1, x, y);
    d2d_point_translate(&segment->point2, x, y);
    d2d_point_translate(&segment->point3, x, y);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_Simplify(ID2D1RoundedRectangleGeometry *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);
    const D2D1_ROUNDED_RECT *r = &geometry->u.rounded_rectangle.rounded_rect;
    struct d2d_figure figure = { 0 };
    D2D1_BEZIER_SEGMENT segments[4];
    D2D1_POINT_2F start_point, p;
    D2D1_ELLIPSE ellipse;
    bool ret;

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    d2d_point_set(&ellipse.point, 0.0f, 0.0f);
    ellipse.radiusX = r->radiusX;
    ellipse.radiusY = r->radiusY;

    d2d_ellipse_to_segments(&ellipse, &start_point, segments);

    d2d_point_set(&p, r->rect.left + r->radiusX, r->rect.top + r->radiusY);
    d2d_point_translate(&start_point, p.x, p.y);
    d2d_bezier_segment_translate(&segments[0], p.x, p.y);
    d2d_point_set(&p, r->rect.right - r->radiusX, r->rect.top + r->radiusY);
    d2d_bezier_segment_translate(&segments[1], p.x, p.y);
    d2d_point_set(&p, r->rect.right - r->radiusX, r->rect.bottom - r->radiusY);
    d2d_bezier_segment_translate(&segments[2], p.x, p.y);
    d2d_point_set(&p, r->rect.left + r->radiusX, r->rect.bottom - r->radiusY);
    d2d_bezier_segment_translate(&segments[3], p.x, p.y);

    ret = d2d_figure_begin(&figure, start_point, D2D1_FIGURE_BEGIN_FILLED);
    ret = ret && d2d_figure_add_beziers(&figure, &segments[0], 1);
    d2d_point_set(&p, r->rect.right - r->radiusX, r->rect.top);
    ret = ret && d2d_figure_add_lines(&figure, &p, 1);
    ret = ret && d2d_figure_add_beziers(&figure, &segments[1], 1);
    d2d_point_set(&p, r->rect.right, r->rect.bottom - r->radiusY);
    ret = ret && d2d_figure_add_lines(&figure, &p, 1);
    ret = ret && d2d_figure_add_beziers(&figure, &segments[2], 1);
    d2d_point_set(&p, r->rect.left + r->radiusX, r->rect.bottom);
    ret = ret && d2d_figure_add_lines(&figure, &p, 1);
    ret = ret && d2d_figure_add_beziers(&figure, &segments[3], 1);
    if (!ret)
    {
        d2d_figure_cleanup(&figure);
        return E_OUTOFMEMORY;
    }

    d2d_figure_end(&figure, D2D1_FIGURE_END_CLOSED);

    d2d_figure_simplify(&figure, option, transform, tolerance, sink);
    d2d_figure_cleanup(&figure);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_Tessellate(ID2D1RoundedRectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return d2d_geometry_tessellate(&geometry->ID2D1Geometry_iface, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_CombineWithGeometry(
        ID2D1RoundedRectangleGeometry *iface, ID2D1Geometry *geometry, D2D1_COMBINE_MODE combine_mode,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *g = impl_from_ID2D1RoundedRectangleGeometry(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry, combine_mode, transform, tolerance, sink);

    return d2d_geometry_combine(&g->ID2D1Geometry_iface, geometry, combine_mode, transform,
            tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_Outline(ID2D1RoundedRectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, transform %p, tolerance %.8e, sink %p stub!\n", iface, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_ComputeArea(ID2D1RoundedRectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    return d2d_geometry_compute_area(&geometry->ID2D1Geometry_iface, transform, tolerance, area);
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_ComputeLength(ID2D1RoundedRectangleGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    FIXME("iface %p, transform %p, tolerance %.8e, length %p stub!\n", iface, transform, tolerance, length);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_ComputePointAtLength(
        ID2D1RoundedRectangleGeometry *iface, float length, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_POINT_2F *point, D2D1_POINT_2F *tangent)
{
    FIXME("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p stub!\n",
            iface, length, transform, tolerance, point, tangent);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_Widen(ID2D1RoundedRectangleGeometry *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p stub!\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_rounded_rectangle_geometry_GetRoundedRect(ID2D1RoundedRectangleGeometry *iface,
        D2D1_ROUNDED_RECT *rounded_rect)
{
    struct d2d_geometry *geometry = impl_from_ID2D1RoundedRectangleGeometry(iface);

    TRACE("iface %p, rounded_rect %p.\n", iface, rounded_rect);

    *rounded_rect = geometry->u.rounded_rectangle.rounded_rect;
}

static const struct ID2D1RoundedRectangleGeometryVtbl d2d_rounded_rectangle_geometry_vtbl =
{
    d2d_rounded_rectangle_geometry_QueryInterface,
    d2d_rounded_rectangle_geometry_AddRef,
    d2d_rounded_rectangle_geometry_Release,
    d2d_rounded_rectangle_geometry_GetFactory,
    d2d_rounded_rectangle_geometry_GetBounds,
    d2d_rounded_rectangle_geometry_GetWidenedBounds,
    d2d_rounded_rectangle_geometry_StrokeContainsPoint,
    d2d_rounded_rectangle_geometry_FillContainsPoint,
    d2d_rounded_rectangle_geometry_CompareWithGeometry,
    d2d_rounded_rectangle_geometry_Simplify,
    d2d_rounded_rectangle_geometry_Tessellate,
    d2d_rounded_rectangle_geometry_CombineWithGeometry,
    d2d_rounded_rectangle_geometry_Outline,
    d2d_rounded_rectangle_geometry_ComputeArea,
    d2d_rounded_rectangle_geometry_ComputeLength,
    d2d_rounded_rectangle_geometry_ComputePointAtLength,
    d2d_rounded_rectangle_geometry_Widen,
    d2d_rounded_rectangle_geometry_GetRoundedRect,
};

static void d2d_rounded_rectangle_geometry_stream(struct d2d_geometry *geometry,
        const D2D_MATRIX_3X2_F *transform, ID2D1GeometrySink *sink)
{
    const D2D1_ROUNDED_RECT *r = &geometry->u.rounded_rectangle.rounded_rect;
    D2D1_ARC_SEGMENT arcs[4];
    D2D1_POINT_2F points[4];

    if (r->radiusX == 0.0f || r->radiusY == 0.0f)
        return d2d_rectangle_stream(&r->rect, transform, sink);

    arcs[0].size.width = r->radiusX;
    arcs[0].size.height = r->radiusY;
    arcs[0].rotationAngle = 90.0f;
    arcs[0].sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arcs[0].arcSize = D2D1_ARC_SIZE_SMALL;
    arcs[1] = arcs[2] = arcs[3] = arcs[0];

    d2d_point_set(&points[0], r->rect.left, r->rect.top - r->radiusY);
    d2d_point_set(&points[1], r->rect.right - r->radiusX, r->rect.top);
    d2d_point_set(&points[2], r->rect.right, r->rect.bottom - r->radiusY);
    d2d_point_set(&points[3], r->rect.left - r->radiusX, r->rect.bottom);

    d2d_point_set(&arcs[0].point, r->rect.left - r->radiusX, r->rect.top);
    d2d_point_set(&arcs[1].point, r->rect.right, r->rect.top - r->radiusY);
    d2d_point_set(&arcs[2].point, r->rect.right - r->radiusX, r->rect.bottom);
    d2d_point_set(&arcs[3].point, r->rect.left, r->rect.bottom - r->radiusY);

    if (transform)
    {
        for (int i = 0; i < ARRAYSIZE(points); ++i)
            d2d_point_transform(&points[i], transform, points[i].x, points[i].y);
        for (int i = 0; i < ARRAYSIZE(arcs); ++i)
            d2d_arc_transform(&arcs[i], transform);
    }

    ID2D1GeometrySink_BeginFigure(sink, points[0], D2D1_FIGURE_BEGIN_FILLED);

    ID2D1GeometrySink_AddArc(sink, &arcs[0]);
    ID2D1GeometrySink_AddLine(sink, points[1]);
    ID2D1GeometrySink_AddArc(sink, &arcs[1]);
    ID2D1GeometrySink_AddLine(sink, points[2]);
    ID2D1GeometrySink_AddArc(sink, &arcs[2]);
    ID2D1GeometrySink_AddLine(sink, points[3]);
    ID2D1GeometrySink_AddArc(sink, &arcs[3]);

    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
}

static const struct d2d_geometry_ops d2d_rounded_rectangle_geometry_ops =
{
    .stream = d2d_rounded_rectangle_geometry_stream,
};

HRESULT d2d_rounded_rectangle_geometry_init(struct d2d_geometry *geometry,
        ID2D1Factory *factory, const D2D1_ROUNDED_RECT *rounded_rect)
{
    D2D1_POINT_2F *v, v1, v2, v3, v4;
    struct d2d_face *f;
    float l, r, t, b;
    float rx, ry;

    d2d_geometry_init(geometry, factory, &identity, (ID2D1GeometryVtbl *)&d2d_rounded_rectangle_geometry_vtbl,
            &d2d_rounded_rectangle_geometry_ops);
    geometry->u.rounded_rectangle.rounded_rect = *rounded_rect;

    if (!(geometry->fill.vertices = malloc(8 * sizeof(*geometry->fill.vertices))))
        goto fail;
    if (!d2d_array_reserve((void **)&geometry->fill.faces,
            &geometry->fill.faces_size, 6, sizeof(*geometry->fill.faces)))
        goto fail;

    l = min(rounded_rect->rect.left, rounded_rect->rect.right);
    r = max(rounded_rect->rect.left, rounded_rect->rect.right);
    t = min(rounded_rect->rect.top, rounded_rect->rect.bottom);
    b = max(rounded_rect->rect.top, rounded_rect->rect.bottom);

    rx = min(rounded_rect->radiusX, 0.5f * (r - l));
    ry = min(rounded_rect->radiusY, 0.5f * (b - t));

    d2d_point_set(&v1, r, t);
    d2d_point_set(&v2, r, b);
    d2d_point_set(&v3, l, b);
    d2d_point_set(&v4, l, t);

    v = geometry->fill.vertices;
    d2d_point_set(&v[0], l + rx, t);
    d2d_point_set(&v[1], r - rx, t);
    d2d_point_set(&v[2], r, t + ry);
    d2d_point_set(&v[3], r, b - ry);
    d2d_point_set(&v[4], r - rx, b);
    d2d_point_set(&v[5], l + rx, b);
    d2d_point_set(&v[6], l, b - ry);
    d2d_point_set(&v[7], l, t + ry);
    geometry->fill.vertex_count = 8;

    f = geometry->fill.faces;
    d2d_face_set(&f[0], 0, 7, 6);
    d2d_face_set(&f[1], 0, 6, 5);
    d2d_face_set(&f[2], 0, 5, 4);
    d2d_face_set(&f[3], 0, 4, 1);
    d2d_face_set(&f[4], 1, 4, 3);
    d2d_face_set(&f[5], 1, 3, 2);
    geometry->fill.face_count = 6;

    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[1], &v1, &v[2]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[3], &v2, &v[4]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[5], &v3, &v[6]))
        goto fail;
    if (!d2d_geometry_fill_add_arc_triangle(geometry, &v[7], &v4, &v[0]))
        goto fail;

    if (!d2d_geometry_outline_add_line_segment(geometry, &v[0], &v[1]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[1], &v1, &v[2]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[2], &v[3]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[3], &v2, &v[4]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[4], &v[5]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[5], &v3, &v[6]))
        goto fail;
    if (!d2d_geometry_outline_add_line_segment(geometry, &v[6], &v[7]))
        goto fail;
    if (!d2d_geometry_outline_add_arc_quadrant(geometry, &v[7], &v4, &v[0]))
        goto fail;

    return S_OK;

fail:
    d2d_geometry_cleanup(geometry);
    return E_OUTOFMEMORY;
}

static inline struct d2d_geometry *impl_from_ID2D1TransformedGeometry(ID2D1TransformedGeometry *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_QueryInterface(ID2D1TransformedGeometry *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1TransformedGeometry)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1TransformedGeometry_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_transformed_geometry_AddRef(ID2D1TransformedGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_transformed_geometry_Release(ID2D1TransformedGeometry *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        geometry->outline.arc_faces = NULL;
        geometry->outline.arcs = NULL;
        geometry->outline.bezier_faces = NULL;
        geometry->outline.beziers = NULL;
        geometry->outline.faces = NULL;
        geometry->outline.vertices = NULL;
        geometry->fill.arc_vertices = NULL;
        geometry->fill.bezier_vertices = NULL;
        geometry->fill.faces = NULL;
        geometry->fill.vertices = NULL;
        ID2D1Geometry_Release(geometry->u.transformed.src_geometry);
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_transformed_geometry_GetFactory(ID2D1TransformedGeometry *iface,
        ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_GetBounds(ID2D1TransformedGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, transform %p, bounds %p.\n", iface, transform, bounds);

    g = geometry->u.transformed.transform;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    return ID2D1Geometry_GetBounds(geometry->u.transformed.src_geometry, &g, bounds);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_GetWidenedBounds(ID2D1TransformedGeometry *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_RECT_F *bounds)
{
    D2D1_RECT_F fill_bounds;
    HRESULT hr;

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    if (FAILED(hr = ID2D1TransformedGeometry_GetBounds(iface, transform, &fill_bounds)))
        return hr;

    d2d_geometry_widen_bounds(&fill_bounds, stroke_width, stroke_style, bounds);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_StrokeContainsPoint(ID2D1TransformedGeometry *iface,
        D2D1_POINT_2F point, float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    g = geometry->u.transformed.transform;
    stroke_width /= g.m11;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    if (tolerance <= 0.0f)
        tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    return ID2D1Geometry_StrokeContainsPoint(geometry->u.transformed.src_geometry, point, stroke_width, stroke_style,
            &g, tolerance, contains);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_FillContainsPoint(ID2D1TransformedGeometry *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, point %s, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    g = geometry->u.transformed.transform;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    return ID2D1Geometry_FillContainsPoint(geometry->u.transformed.src_geometry, point, &g, tolerance, contains);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_CompareWithGeometry(ID2D1TransformedGeometry *iface,
        ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    FIXME("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p stub!\n",
            iface, geometry, transform, tolerance, relation);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_Simplify(ID2D1TransformedGeometry *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    g = geometry->u.transformed.transform;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    return ID2D1Geometry_Simplify(geometry->u.transformed.src_geometry, option, &g, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_Tessellate(ID2D1TransformedGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    g = geometry->u.transformed.transform;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    return ID2D1Geometry_Tessellate(geometry->u.transformed.src_geometry, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_CombineWithGeometry(ID2D1TransformedGeometry *iface,
        ID2D1Geometry *geometry, D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *g = impl_from_ID2D1TransformedGeometry(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry, combine_mode, transform, tolerance, sink);

    return d2d_geometry_combine(&g->ID2D1Geometry_iface, geometry, combine_mode, transform,
            tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_Outline(ID2D1TransformedGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, transform %p, tolerance %.8e, sink %p stub!\n", iface, transform, tolerance, sink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_ComputeArea(ID2D1TransformedGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);
    D2D1_MATRIX_3X2_F g;

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    g = geometry->u.transformed.transform;
    if (transform)
        d2d_matrix_multiply(&g, transform);

    return ID2D1Geometry_ComputeArea(geometry->u.transformed.src_geometry, &g, tolerance, area);
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_ComputeLength(ID2D1TransformedGeometry *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    FIXME("iface %p, transform %p, tolerance %.8e, length %p stub!\n", iface, transform, tolerance, length);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_ComputePointAtLength(ID2D1TransformedGeometry *iface,
        float length, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_POINT_2F *point,
        D2D1_POINT_2F *tangent)
{
    FIXME("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p stub!\n",
            iface, length, transform, tolerance, point, tangent);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_transformed_geometry_Widen(ID2D1TransformedGeometry *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    FIXME("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p stub!\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_transformed_geometry_GetSourceGeometry(ID2D1TransformedGeometry *iface,
        ID2D1Geometry **src_geometry)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);

    TRACE("iface %p, src_geometry %p.\n", iface, src_geometry);

    ID2D1Geometry_AddRef(*src_geometry = geometry->u.transformed.src_geometry);
}

static void STDMETHODCALLTYPE d2d_transformed_geometry_GetTransform(ID2D1TransformedGeometry *iface,
        D2D1_MATRIX_3X2_F *transform)
{
    struct d2d_geometry *geometry = impl_from_ID2D1TransformedGeometry(iface);

    TRACE("iface %p, transform %p.\n", iface, transform);

    *transform = geometry->u.transformed.transform;
}

static const struct ID2D1TransformedGeometryVtbl d2d_transformed_geometry_vtbl =
{
    d2d_transformed_geometry_QueryInterface,
    d2d_transformed_geometry_AddRef,
    d2d_transformed_geometry_Release,
    d2d_transformed_geometry_GetFactory,
    d2d_transformed_geometry_GetBounds,
    d2d_transformed_geometry_GetWidenedBounds,
    d2d_transformed_geometry_StrokeContainsPoint,
    d2d_transformed_geometry_FillContainsPoint,
    d2d_transformed_geometry_CompareWithGeometry,
    d2d_transformed_geometry_Simplify,
    d2d_transformed_geometry_Tessellate,
    d2d_transformed_geometry_CombineWithGeometry,
    d2d_transformed_geometry_Outline,
    d2d_transformed_geometry_ComputeArea,
    d2d_transformed_geometry_ComputeLength,
    d2d_transformed_geometry_ComputePointAtLength,
    d2d_transformed_geometry_Widen,
    d2d_transformed_geometry_GetSourceGeometry,
    d2d_transformed_geometry_GetTransform,
};

static void d2d_geometry_stream(ID2D1Geometry *iface, const D2D_MATRIX_3X2_F *transform,
        ID2D1GeometrySink *sink)
{
    struct d2d_geometry *geometry = unsafe_impl_from_ID2D1Geometry(iface);
    geometry->ops->stream(geometry, transform, sink);
}

static void d2d_transformed_geometry_stream(struct d2d_geometry *geometry,
        const D2D_MATRIX_3X2_F *transform, ID2D1GeometrySink *sink)
{
    D2D_MATRIX_3X2_F m = geometry->transform;

    if (transform)
        d2d_matrix_multiply(&m, transform);
    d2d_geometry_stream(geometry->u.transformed.src_geometry, &m, sink);
}

static const struct d2d_geometry_ops d2d_transformed_geometry_ops =
{
    .stream = d2d_transformed_geometry_stream,
};

void d2d_transformed_geometry_init(struct d2d_geometry *geometry, ID2D1Factory *factory,
        ID2D1Geometry *src_geometry, const D2D_MATRIX_3X2_F *transform)
{
    struct d2d_geometry *src_impl;
    D2D_MATRIX_3X2_F g;

    src_impl = unsafe_impl_from_ID2D1Geometry(src_geometry);

    g = src_impl->transform;
    d2d_matrix_multiply(&g, transform);
    d2d_geometry_init(geometry, factory, &g, (ID2D1GeometryVtbl *)&d2d_transformed_geometry_vtbl,
            &d2d_transformed_geometry_ops);
    ID2D1Geometry_AddRef(geometry->u.transformed.src_geometry = src_geometry);
    geometry->u.transformed.transform = *transform;
    geometry->fill = src_impl->fill;
    geometry->outline = src_impl->outline;
}

static inline struct d2d_geometry *impl_from_ID2D1GeometryGroup(ID2D1GeometryGroup *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_QueryInterface(ID2D1GeometryGroup *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1GeometryGroup)
            || IsEqualGUID(iid, &IID_ID2D1Geometry)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1GeometryGroup_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_geometry_group_AddRef(ID2D1GeometryGroup *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);
    ULONG refcount = InterlockedIncrement(&geometry->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_geometry_group_Release(ID2D1GeometryGroup *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);
    ULONG refcount = InterlockedDecrement(&geometry->refcount);
    unsigned int i;

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        for (i = 0; i < geometry->u.group.geometry_count; ++i)
            ID2D1Geometry_Release(geometry->u.group.src_geometries[i]);
        ID2D1PathGeometry_Release(geometry->u.group.path);
        free(geometry->u.group.src_geometries);
        memset(&geometry->outline, 0, sizeof(geometry->outline));
        memset(&geometry->fill, 0, sizeof(geometry->fill));
        d2d_geometry_cleanup(geometry);
        free(geometry);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_geometry_group_GetFactory(ID2D1GeometryGroup *iface,
        ID2D1Factory **factory)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = geometry->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_GetBounds(ID2D1GeometryGroup *iface,
        const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, transform %p, bounds %p.\n", iface, transform, bounds);

    return ID2D1PathGeometry_GetBounds(geometry->u.group.path, transform, bounds);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_GetWidenedBounds(ID2D1GeometryGroup *iface,
        float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, D2D1_RECT_F *bounds)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, bounds %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, bounds);

    return ID2D1PathGeometry_GetWidenedBounds(geometry->u.group.path, stroke_width, stroke_style, transform,
            tolerance, bounds);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_StrokeContainsPoint(ID2D1GeometryGroup *iface,
        D2D1_POINT_2F point, float stroke_width, ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, point %s, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), stroke_width, stroke_style, transform, tolerance, contains);

    return ID2D1PathGeometry_StrokeContainsPoint(geometry->u.group.path, point, stroke_width,
            stroke_style, transform, tolerance, contains);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_FillContainsPoint(ID2D1GeometryGroup *iface,
        D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, float tolerance, BOOL *contains)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, point %s, transform %p, tolerance %.8e, contains %p.\n",
            iface, debug_d2d_point_2f(&point), transform, tolerance, contains);

    return ID2D1PathGeometry_FillContainsPoint(geometry->u.group.path, point, transform, tolerance, contains);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_CompareWithGeometry(ID2D1GeometryGroup *iface,
        ID2D1Geometry *geometry2, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_GEOMETRY_RELATION *relation)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, geometry %p, transform %p, tolerance %.8e, relation %p.\n",
            iface, geometry2, transform, tolerance, relation);

    return ID2D1PathGeometry_CompareWithGeometry(geometry->u.group.path, geometry2, transform, tolerance, relation);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_Simplify(ID2D1GeometryGroup *iface,
        D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, option %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, option, transform, tolerance, sink);

    return ID2D1PathGeometry_Simplify(geometry->u.group.path, option, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_Tessellate(ID2D1GeometryGroup *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1TessellationSink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return ID2D1PathGeometry_Tessellate(geometry->u.group.path, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_CombineWithGeometry(ID2D1GeometryGroup *iface,
        ID2D1Geometry *geometry2, D2D1_COMBINE_MODE combine_mode, const D2D1_MATRIX_3X2_F *transform,
        float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, geometry %p, combine_mode %#x, transform %p, tolerance %.8e, sink %p.\n",
            iface, geometry2, combine_mode, transform, tolerance, sink);

    return ID2D1PathGeometry_CombineWithGeometry(geometry->u.group.path, geometry2, combine_mode,
            transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_Outline(ID2D1GeometryGroup *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, sink %p.\n", iface, transform, tolerance, sink);

    return ID2D1PathGeometry_Outline(geometry->u.group.path, transform, tolerance, sink);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_ComputeArea(ID2D1GeometryGroup *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *area)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, area %p.\n", iface, transform, tolerance, area);

    return ID2D1PathGeometry_ComputeArea(geometry->u.group.path, transform, tolerance, area);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_ComputeLength(ID2D1GeometryGroup *iface,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, float *length)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, transform %p, tolerance %.8e, length %p.\n", iface, transform, tolerance, length);

    return ID2D1PathGeometry_ComputeLength(geometry->u.group.path, transform, tolerance, length);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_ComputePointAtLength(ID2D1GeometryGroup *iface,
        float length, const D2D1_MATRIX_3X2_F *transform, float tolerance, D2D1_POINT_2F *point,
        D2D1_POINT_2F *tangent)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, length %.8e, transform %p, tolerance %.8e, point %p, tangent %p.\n",
            iface, length, transform, tolerance, point, tangent);

    return ID2D1PathGeometry_ComputePointAtLength(geometry->u.group.path, length, transform, tolerance,
            point, tangent);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_group_Widen(ID2D1GeometryGroup *iface, float stroke_width,
        ID2D1StrokeStyle *stroke_style, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        ID2D1SimplifiedGeometrySink *sink)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p, stroke_width %.8e, stroke_style %p, transform %p, tolerance %.8e, sink %p.\n",
            iface, stroke_width, stroke_style, transform, tolerance, sink);

    return ID2D1PathGeometry_Widen(geometry->u.group.path, stroke_width, stroke_style, transform,
            tolerance, sink);
}

static D2D1_FILL_MODE STDMETHODCALLTYPE d2d_geometry_group_GetFillMode(ID2D1GeometryGroup *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p.\n", iface);

    return geometry->u.group.fill_mode;
}

static UINT32 STDMETHODCALLTYPE d2d_geometry_group_GetSourceGeometryCount(ID2D1GeometryGroup *iface)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);

    TRACE("iface %p.\n", iface);

    return geometry->u.group.geometry_count;
}

static void STDMETHODCALLTYPE d2d_geometry_group_GetSourceGeometries(ID2D1GeometryGroup *iface,
        ID2D1Geometry **geometries, UINT32 geometry_count)
{
    struct d2d_geometry *geometry = impl_from_ID2D1GeometryGroup(iface);
    unsigned int i;

    TRACE("iface %p, geometries %p, geometry_count %u.\n", iface, geometries, geometry_count);

    geometry_count = min(geometry_count, geometry->u.group.geometry_count);
    for (i = 0; i < geometry_count; ++i)
        ID2D1Geometry_AddRef(geometries[i] = geometry->u.group.src_geometries[i]);
}

static const struct ID2D1GeometryGroupVtbl d2d_geometry_group_vtbl =
{
    d2d_geometry_group_QueryInterface,
    d2d_geometry_group_AddRef,
    d2d_geometry_group_Release,
    d2d_geometry_group_GetFactory,
    d2d_geometry_group_GetBounds,
    d2d_geometry_group_GetWidenedBounds,
    d2d_geometry_group_StrokeContainsPoint,
    d2d_geometry_group_FillContainsPoint,
    d2d_geometry_group_CompareWithGeometry,
    d2d_geometry_group_Simplify,
    d2d_geometry_group_Tessellate,
    d2d_geometry_group_CombineWithGeometry,
    d2d_geometry_group_Outline,
    d2d_geometry_group_ComputeArea,
    d2d_geometry_group_ComputeLength,
    d2d_geometry_group_ComputePointAtLength,
    d2d_geometry_group_Widen,
    d2d_geometry_group_GetFillMode,
    d2d_geometry_group_GetSourceGeometryCount,
    d2d_geometry_group_GetSourceGeometries,
};

static void d2d_geometry_group_stream(struct d2d_geometry *geometry,
        const D2D_MATRIX_3X2_F *transform, ID2D1GeometrySink *sink)
{
    d2d_geometry_stream((ID2D1Geometry *)geometry->u.group.path, NULL, sink);
}

static const struct d2d_geometry_ops d2d_geometry_group_ops =
{
    .stream = d2d_geometry_group_stream,
};

HRESULT d2d_geometry_group_init(struct d2d_geometry *geometry, ID2D1Factory *factory,
        D2D1_FILL_MODE fill_mode, ID2D1Geometry **geometries, unsigned int geometry_count)
{
    ID2D1GeometrySink *sink;
    unsigned int i;
    HRESULT hr;

    d2d_geometry_init(geometry, factory, &identity, (ID2D1GeometryVtbl *)&d2d_geometry_group_vtbl,
            &d2d_geometry_group_ops);

    if (!(geometry->u.group.src_geometries = calloc(geometry_count, sizeof(*geometries))))
    {
        d2d_geometry_cleanup(geometry);
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < geometry_count; ++i)
    {
        ID2D1Geometry_AddRef(geometry->u.group.src_geometries[i] = geometries[i]);
    }
    geometry->u.group.geometry_count = geometry_count;
    geometry->u.group.fill_mode = fill_mode;

    if (FAILED(hr = ID2D1Factory_CreatePathGeometry(factory, &geometry->u.group.path)))
    {
        d2d_geometry_cleanup(geometry);
        return hr;
    }

    if (SUCCEEDED(hr = ID2D1PathGeometry_Open(geometry->u.group.path, &sink)))
    {
        ID2D1GeometrySink_SetFillMode(sink, fill_mode);
        for (i = 0; i < geometry_count; ++i)
        {
            ID2D1GeometrySink_SetSegmentFlags(sink, 0);
            d2d_geometry_stream(geometries[i], NULL, sink);
        }
        hr = ID2D1GeometrySink_Close(sink);
        ID2D1GeometrySink_Release(sink);
    }

    if (SUCCEEDED(hr))
    {
        struct d2d_geometry *path_impl = unsafe_impl_from_ID2D1Geometry((ID2D1Geometry *)geometry->u.group.path);

        geometry->fill = path_impl->fill;
        geometry->outline = path_impl->outline;
    }

    if (FAILED(hr))
    {
        d2d_geometry_cleanup(geometry);
        ID2D1PathGeometry_Release(geometry->u.group.path);
    }

    return hr;
}

struct d2d_geometry *unsafe_impl_from_ID2D1Geometry(ID2D1Geometry *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_ellipse_geometry_vtbl
            || iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_path_geometry_vtbl
            || iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_rectangle_geometry_vtbl
            || iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_rounded_rectangle_geometry_vtbl
            || iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_transformed_geometry_vtbl
            || iface->lpVtbl == (const ID2D1GeometryVtbl *)&d2d_geometry_group_vtbl);
    return CONTAINING_RECORD(iface, struct d2d_geometry, ID2D1Geometry_iface);
}

static inline struct d2d_geometry_realization *impl_from_ID2D1GeometryRealization(
        ID2D1GeometryRealization *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_geometry_realization, ID2D1GeometryRealization_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_geometry_realization_QueryInterface(
        ID2D1GeometryRealization *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1GeometryRealization)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1GeometryRealization_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_geometry_realization_AddRef(ID2D1GeometryRealization *iface)
{
    struct d2d_geometry_realization *realization = impl_from_ID2D1GeometryRealization(iface);
    ULONG refcount = InterlockedIncrement(&realization->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_geometry_realization_Release(ID2D1GeometryRealization *iface)
{
    struct d2d_geometry_realization *realization = impl_from_ID2D1GeometryRealization(iface);
    ULONG refcount = InterlockedDecrement(&realization->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (realization->stroke_style)
            ID2D1StrokeStyle_Release(realization->stroke_style);
        ID2D1Geometry_Release(realization->geometry);
        ID2D1Factory_Release(realization->factory);
        free(realization);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_geometry_realization_GetFactory(ID2D1GeometryRealization *iface,
        ID2D1Factory **factory)
{
    struct d2d_geometry_realization *realization = impl_from_ID2D1GeometryRealization(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = realization->factory);
}

static const ID2D1GeometryRealizationVtbl d2d_geometry_realization_vtbl =
{
    d2d_geometry_realization_QueryInterface,
    d2d_geometry_realization_AddRef,
    d2d_geometry_realization_Release,
    d2d_geometry_realization_GetFactory,
};

HRESULT d2d_geometry_realization_init(struct d2d_geometry_realization *realization,
        ID2D1Factory *factory, ID2D1Geometry *geometry)
{
    realization->ID2D1GeometryRealization_iface.lpVtbl = &d2d_geometry_realization_vtbl;
    realization->refcount = 1;
    ID2D1Factory_AddRef(realization->factory = factory);
    ID2D1Geometry_AddRef(realization->geometry = geometry);

    return S_OK;
}

struct d2d_geometry_realization *unsafe_impl_from_ID2D1GeometryRealization(ID2D1GeometryRealization *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d2d_geometry_realization_vtbl);
    return CONTAINING_RECORD(iface, struct d2d_geometry_realization, ID2D1GeometryRealization_iface);
}
