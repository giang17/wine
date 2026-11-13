/*
 * Copyright 2026 Giang Nguyen
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
#include "wincodec.h"

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

/* Colour contexts describe the colour space a bitmap or an effect input lives
 * in. Applications hand them to the ColorManagement effect, query the colour
 * space back, or read out the ICC profile. The colour space is what the drawing
 * code actually acts on; the ICC profile is generated on demand from the
 * primaries and the transfer function so that GetProfile() returns something
 * meaningful for the colour spaces d2d knows about. */

#define D2D_ICC_HEADER_SIZE 128
#define D2D_ICC_TAG_COUNT 9
#define D2D_ICC_TRC_ENTRIES 1024

/* CIE xy chromaticities of the Rec. 709 / sRGB primaries. */
static const D2D1_POINT_2F d2d_primaries_p709[3] =
{
    {0.640f, 0.330f},
    {0.300f, 0.600f},
    {0.150f, 0.060f},
};

/* CIE xy chromaticities of the Rec. 2020 primaries. */
static const D2D1_POINT_2F d2d_primaries_p2020[3] =
{
    {0.708f, 0.292f},
    {0.170f, 0.797f},
    {0.131f, 0.046f},
};

/* D65 white point, normalised to Y == 1. */
#define D2D_WHITE_D65_X 0.95047f
#define D2D_WHITE_D65_Z 1.08883f

/* D50 white point, the ICC profile connection space illuminant. */
static const double d2d_white_d50[3] = {0.96422, 1.0, 0.82491};

struct d2d_icc_profile_desc
{
    const char *description;
    D2D1_POINT_2F primaries[3];
    float white_x;
    float white_z;
    D2D1_GAMMA1 gamma;
};

static void d2d_icc_write_u16(BYTE *dst, unsigned int value)
{
    dst[0] = value >> 8;
    dst[1] = value;
}

static void d2d_icc_write_u32(BYTE *dst, unsigned int value)
{
    dst[0] = value >> 24;
    dst[1] = value >> 16;
    dst[2] = value >> 8;
    dst[3] = value;
}

static void d2d_icc_write_tag(BYTE *dst, const char *tag)
{
    memcpy(dst, tag, 4);
}

/* ICC s15Fixed16Number. */
static void d2d_icc_write_fixed(BYTE *dst, double value)
{
    d2d_icc_write_u32(dst, (unsigned int)(int)floor(value * 65536.0 + 0.5));
}

static void d2d_icc_write_xyz(BYTE *dst, const double xyz[3])
{
    d2d_icc_write_tag(dst, "XYZ ");
    d2d_icc_write_u32(dst + 4, 0);
    d2d_icc_write_fixed(dst + 8, xyz[0]);
    d2d_icc_write_fixed(dst + 12, xyz[1]);
    d2d_icc_write_fixed(dst + 16, xyz[2]);
}

static BOOL d2d_matrix3_invert(const double m[9], double out[9])
{
    double det;

    out[0] = m[4] * m[8] - m[5] * m[7];
    out[1] = m[2] * m[7] - m[1] * m[8];
    out[2] = m[1] * m[5] - m[2] * m[4];
    out[3] = m[5] * m[6] - m[3] * m[8];
    out[4] = m[0] * m[8] - m[2] * m[6];
    out[5] = m[2] * m[3] - m[0] * m[5];
    out[6] = m[3] * m[7] - m[4] * m[6];
    out[7] = m[1] * m[6] - m[0] * m[7];
    out[8] = m[0] * m[4] - m[1] * m[3];

    det = m[0] * out[0] + m[1] * out[3] + m[2] * out[6];
    if (fabs(det) < 1e-12)
        return FALSE;

    for (unsigned int i = 0; i < 9; ++i)
        out[i] /= det;

    return TRUE;
}

static void d2d_matrix3_mul(const double a[9], const double b[9], double out[9])
{
    for (unsigned int i = 0; i < 3; ++i)
    {
        for (unsigned int j = 0; j < 3; ++j)
        {
            out[i * 3 + j] = a[i * 3] * b[j]
                    + a[i * 3 + 1] * b[3 + j]
                    + a[i * 3 + 2] * b[6 + j];
        }
    }
}

static void d2d_matrix3_transform(const double m[9], const double v[3], double out[3])
{
    for (unsigned int i = 0; i < 3; ++i)
        out[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
}

/* Build the RGB -> XYZ matrix for a set of xy primaries and a white point,
 * chromatically adapted to the D50 profile connection space with the Bradford
 * transform. The columns of the result are the rXYZ/gXYZ/bXYZ tag values. */
static BOOL d2d_icc_build_matrix(const struct d2d_icc_profile_desc *desc, double matrix[9])
{
    static const double bradford[9] =
    {
         0.8951,  0.2664, -0.1614,
        -0.7502,  1.7135,  0.0367,
         0.0389, -0.0685,  1.0296,
    };
    double primaries[9], inverse[9], scale[3], scaled[9], adapt[9], tmp[9], cone_src[3], cone_dst[3];
    double white[3];

    white[0] = desc->white_x;
    white[1] = 1.0;
    white[2] = desc->white_z;

    /* Columns are the primaries in XYZ, normalised to Y == 1. */
    for (unsigned int i = 0; i < 3; ++i)
    {
        const D2D1_POINT_2F *xy = &desc->primaries[i];

        if (fabsf(xy->y) < 1e-6f)
        {
            WARN("Degenerate primary %u (%.3f, %.3f).\n", i, xy->x, xy->y);
            return FALSE;
        }

        primaries[i] = xy->x / xy->y;
        primaries[3 + i] = 1.0;
        primaries[6 + i] = (1.0 - xy->x - xy->y) / xy->y;
    }

    if (!d2d_matrix3_invert(primaries, inverse))
    {
        WARN("Primaries are not linearly independent.\n");
        return FALSE;
    }

    d2d_matrix3_transform(inverse, white, scale);

    for (unsigned int i = 0; i < 3; ++i)
    {
        for (unsigned int j = 0; j < 3; ++j)
            scaled[i * 3 + j] = primaries[i * 3 + j] * scale[j];
    }

    /* Bradford adaptation from the source white point to D50. */
    d2d_matrix3_transform(bradford, white, cone_src);
    d2d_matrix3_transform(bradford, d2d_white_d50, cone_dst);

    for (unsigned int i = 0; i < 3; ++i)
    {
        if (fabs(cone_src[i]) < 1e-12)
        {
            WARN("Degenerate white point (%.4f, %.4f).\n", desc->white_x, desc->white_z);
            return FALSE;
        }
    }

    memset(tmp, 0, sizeof(tmp));
    tmp[0] = cone_dst[0] / cone_src[0];
    tmp[4] = cone_dst[1] / cone_src[1];
    tmp[8] = cone_dst[2] / cone_src[2];

    if (!d2d_matrix3_invert(bradford, inverse))
        return FALSE;

    /* The adaptation matrix is Ma^-1 * diag(cone_dst / cone_src) * Ma. */
    d2d_matrix3_mul(inverse, tmp, adapt);
    d2d_matrix3_mul(adapt, bradford, tmp);
    d2d_matrix3_mul(tmp, scaled, matrix);

    return TRUE;
}

/* The ICC tone reproduction curve maps encoded device values to linear light,
 * i.e. it is the decoding direction. A curve with a zero element count is the
 * identity, which is exactly what a linear (scRGB) colour space needs. */
static unsigned int d2d_icc_trc_size(D2D1_GAMMA1 gamma)
{
    return gamma == D2D1_GAMMA1_G10 ? 12 : 12 + D2D_ICC_TRC_ENTRIES * 2;
}

static void d2d_icc_write_trc(BYTE *dst, D2D1_GAMMA1 gamma)
{
    d2d_icc_write_tag(dst, "curv");
    d2d_icc_write_u32(dst + 4, 0);

    if (gamma == D2D1_GAMMA1_G10)
    {
        d2d_icc_write_u32(dst + 8, 0);
        return;
    }

    d2d_icc_write_u32(dst + 8, D2D_ICC_TRC_ENTRIES);
    for (unsigned int i = 0; i < D2D_ICC_TRC_ENTRIES; ++i)
    {
        double value = i / (double)(D2D_ICC_TRC_ENTRIES - 1);

        /* sRGB electro-optical transfer function. */
        value = value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
        d2d_icc_write_u16(dst + 12 + i * 2, (unsigned int)floor(value * 65535.0 + 0.5));
    }
}

static unsigned int d2d_icc_align(unsigned int size)
{
    return (size + 3) & ~3u;
}

/* A minimal ICC v2.4 RGB matrix/shaper display profile. The three tone curves
 * are identical and share one tag data block, which the specification allows. */
static HRESULT d2d_icc_build_profile(const struct d2d_icc_profile_desc *desc, BYTE **out, UINT32 *out_size)
{
    unsigned int desc_size, cprt_size, trc_size, offset, size, i;
    static const char copyright[] = "No copyright, use freely.";
    double matrix[9], white[3], column[3];
    BYTE *profile, *table;

    if (desc->gamma != D2D1_GAMMA1_G22 && desc->gamma != D2D1_GAMMA1_G10)
    {
        WARN("No ICC profile generated for gamma %#x.\n", desc->gamma);
        return E_FAIL;
    }

    if (!d2d_icc_build_matrix(desc, matrix))
        return E_INVALIDARG;

    /* textDescriptionType: 8 byte tag header, ASCII length and string, plus the
     * unused Unicode and ScriptCode trailers the v2 layout mandates. */
    desc_size = d2d_icc_align(8 + 4 + strlen(desc->description) + 1 + 4 + 4 + 2 + 1 + 67);
    cprt_size = d2d_icc_align(8 + sizeof(copyright));
    trc_size = d2d_icc_align(d2d_icc_trc_size(desc->gamma));

    size = D2D_ICC_HEADER_SIZE + 4 + D2D_ICC_TAG_COUNT * 12;
    size += desc_size + cprt_size + 4 * 20 + trc_size;

    if (!(profile = calloc(1, size)))
        return E_OUTOFMEMORY;

    /* Header. */
    d2d_icc_write_u32(profile, size);
    d2d_icc_write_u32(profile + 8, 0x02400000);
    d2d_icc_write_tag(profile + 12, "mntr");
    d2d_icc_write_tag(profile + 16, "RGB ");
    d2d_icc_write_tag(profile + 20, "XYZ ");
    d2d_icc_write_tag(profile + 36, "acsp");
    d2d_icc_write_tag(profile + 40, "MSFT");
    d2d_icc_write_fixed(profile + 68, d2d_white_d50[0]);
    d2d_icc_write_fixed(profile + 72, d2d_white_d50[1]);
    d2d_icc_write_fixed(profile + 76, d2d_white_d50[2]);

    /* Tag table. */
    table = profile + D2D_ICC_HEADER_SIZE;
    d2d_icc_write_u32(table, D2D_ICC_TAG_COUNT);
    table += 4;
    offset = D2D_ICC_HEADER_SIZE + 4 + D2D_ICC_TAG_COUNT * 12;

    d2d_icc_write_tag(table, "desc");
    d2d_icc_write_u32(table + 4, offset);
    d2d_icc_write_u32(table + 8, desc_size);
    memcpy(profile + offset + 0, "desc", 4);
    d2d_icc_write_u32(profile + offset + 8, strlen(desc->description) + 1);
    memcpy(profile + offset + 12, desc->description, strlen(desc->description));
    table += 12;
    offset += desc_size;

    d2d_icc_write_tag(table, "cprt");
    d2d_icc_write_u32(table + 4, offset);
    d2d_icc_write_u32(table + 8, cprt_size);
    memcpy(profile + offset, "text", 4);
    memcpy(profile + offset + 8, copyright, sizeof(copyright));
    table += 12;
    offset += cprt_size;

    white[0] = d2d_white_d50[0];
    white[1] = d2d_white_d50[1];
    white[2] = d2d_white_d50[2];
    d2d_icc_write_tag(table, "wtpt");
    d2d_icc_write_u32(table + 4, offset);
    d2d_icc_write_u32(table + 8, 20);
    d2d_icc_write_xyz(profile + offset, white);
    table += 12;
    offset += 20;

    for (i = 0; i < 3; ++i)
    {
        static const char *const tags[] = {"rXYZ", "gXYZ", "bXYZ"};

        column[0] = matrix[i];
        column[1] = matrix[3 + i];
        column[2] = matrix[6 + i];

        d2d_icc_write_tag(table, tags[i]);
        d2d_icc_write_u32(table + 4, offset);
        d2d_icc_write_u32(table + 8, 20);
        d2d_icc_write_xyz(profile + offset, column);
        table += 12;
        offset += 20;
    }

    d2d_icc_write_trc(profile + offset, desc->gamma);
    for (i = 0; i < 3; ++i)
    {
        static const char *const tags[] = {"rTRC", "gTRC", "bTRC"};

        d2d_icc_write_tag(table, tags[i]);
        d2d_icc_write_u32(table + 4, offset);
        d2d_icc_write_u32(table + 8, d2d_icc_trc_size(desc->gamma));
        table += 12;
    }
    offset += trc_size;

    if (offset != size)
        ERR("Profile size mismatch, expected %u, got %u.\n", size, offset);

    *out = profile;
    *out_size = size;

    return S_OK;
}

static void d2d_color_context_profile_desc(struct d2d_icc_profile_desc *desc, const char *description,
        const D2D1_POINT_2F *primaries, D2D1_GAMMA1 gamma)
{
    desc->description = description;
    memcpy(desc->primaries, primaries, sizeof(desc->primaries));
    desc->white_x = D2D_WHITE_D65_X;
    desc->white_z = D2D_WHITE_D65_Z;
    desc->gamma = gamma;
}

/* Generate the ICC profile for the colour spaces that have a well known
 * definition. Contexts that carry no describable profile keep an empty one;
 * GetProfileSize() then reports zero, which is honest about what is known. */
static void d2d_color_context_generate_profile(struct d2d_color_context *context)
{
    struct d2d_icc_profile_desc desc;

    switch (context->space)
    {
        case D2D1_COLOR_SPACE_SRGB:
            d2d_color_context_profile_desc(&desc, "sRGB", d2d_primaries_p709, D2D1_GAMMA1_G22);
            break;

        case D2D1_COLOR_SPACE_SCRGB:
            d2d_color_context_profile_desc(&desc, "scRGB", d2d_primaries_p709, D2D1_GAMMA1_G10);
            break;

        default:
            if (context->type == D2D1_COLOR_CONTEXT_TYPE_SIMPLE)
            {
                desc.description = "Simple color profile";
                desc.primaries[0] = context->simple_profile.redPrimary;
                desc.primaries[1] = context->simple_profile.greenPrimary;
                desc.primaries[2] = context->simple_profile.bluePrimary;
                desc.white_x = context->simple_profile.whitePointXZ.x;
                desc.white_z = context->simple_profile.whitePointXZ.y;
                desc.gamma = context->simple_profile.gamma;
                break;
            }

            if (context->type == D2D1_COLOR_CONTEXT_TYPE_DXGI)
            {
                d2d_color_context_profile_desc(&desc, "Rec. 2020", d2d_primaries_p2020, D2D1_GAMMA1_G2084);
                break;
            }

            return;
    }

    if (FAILED(d2d_icc_build_profile(&desc, &context->profile, &context->profile_size)))
    {
        context->profile = NULL;
        context->profile_size = 0;
    }
}

static inline struct d2d_color_context *impl_from_ID2D1ColorContext1(ID2D1ColorContext1 *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_color_context, ID2D1ColorContext1_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_color_context_QueryInterface(ID2D1ColorContext1 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1ColorContext1)
            || IsEqualGUID(iid, &IID_ID2D1ColorContext)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1ColorContext1_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_color_context_AddRef(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_color_context_Release(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        ID2D1Factory_Release(context->factory);
        free(context->profile);
        free(context);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d2d_color_context_GetFactory(ID2D1ColorContext1 *iface, ID2D1Factory **factory)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    ID2D1Factory_AddRef(*factory = context->factory);
}

static D2D1_COLOR_SPACE STDMETHODCALLTYPE d2d_color_context_GetColorSpace(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p.\n", iface);

    return context->space;
}

static UINT32 STDMETHODCALLTYPE d2d_color_context_GetProfileSize(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p.\n", iface);

    return context->profile_size;
}

static HRESULT STDMETHODCALLTYPE d2d_color_context_GetProfile(ID2D1ColorContext1 *iface, BYTE *profile, UINT32 size)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p, profile %p, size %u.\n", iface, profile, size);

    if (!profile || size < context->profile_size)
        return E_INVALIDARG;

    if (context->profile_size)
        memcpy(profile, context->profile, context->profile_size);

    return S_OK;
}

static D2D1_COLOR_CONTEXT_TYPE STDMETHODCALLTYPE d2d_color_context_GetColorContextType(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p.\n", iface);

    return context->type;
}

static DXGI_COLOR_SPACE_TYPE STDMETHODCALLTYPE d2d_color_context_GetDXGIColorSpace(ID2D1ColorContext1 *iface)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p.\n", iface);

    if (context->type != D2D1_COLOR_CONTEXT_TYPE_DXGI)
        return DXGI_COLOR_SPACE_CUSTOM;

    return context->dxgi_space;
}

static HRESULT STDMETHODCALLTYPE d2d_color_context_GetSimpleColorProfile(ID2D1ColorContext1 *iface,
        D2D1_SIMPLE_COLOR_PROFILE *simple_profile)
{
    struct d2d_color_context *context = impl_from_ID2D1ColorContext1(iface);

    TRACE("iface %p, simple_profile %p.\n", iface, simple_profile);

    if (!simple_profile)
        return E_INVALIDARG;

    if (context->type != D2D1_COLOR_CONTEXT_TYPE_SIMPLE)
    {
        memset(simple_profile, 0, sizeof(*simple_profile));
        return E_INVALIDARG;
    }

    *simple_profile = context->simple_profile;

    return S_OK;
}

static const struct ID2D1ColorContext1Vtbl d2d_color_context_vtbl =
{
    d2d_color_context_QueryInterface,
    d2d_color_context_AddRef,
    d2d_color_context_Release,
    d2d_color_context_GetFactory,
    d2d_color_context_GetColorSpace,
    d2d_color_context_GetProfileSize,
    d2d_color_context_GetProfile,
    d2d_color_context_GetColorContextType,
    d2d_color_context_GetDXGIColorSpace,
    d2d_color_context_GetSimpleColorProfile,
};

static struct d2d_color_context *d2d_color_context_alloc(ID2D1Factory *factory,
        D2D1_COLOR_CONTEXT_TYPE type, D2D1_COLOR_SPACE space)
{
    struct d2d_color_context *context;

    if (!(context = calloc(1, sizeof(*context))))
        return NULL;

    context->ID2D1ColorContext1_iface.lpVtbl = &d2d_color_context_vtbl;
    context->refcount = 1;
    context->type = type;
    context->space = space;
    context->dxgi_space = DXGI_COLOR_SPACE_CUSTOM;
    ID2D1Factory_AddRef(context->factory = factory);

    return context;
}

HRESULT d2d_color_context_create(ID2D1Factory *factory, D2D1_COLOR_SPACE space,
        const BYTE *profile, UINT32 profile_size, struct d2d_color_context **out)
{
    struct d2d_color_context *context;

    /* A custom colour space is defined by the profile the caller passes in; for
     * the well known spaces the profile argument is ignored and the profile is
     * generated from the space instead. */
    if (space == D2D1_COLOR_SPACE_CUSTOM && (!profile || !profile_size))
        return E_INVALIDARG;

    if (space != D2D1_COLOR_SPACE_CUSTOM && space != D2D1_COLOR_SPACE_SRGB && space != D2D1_COLOR_SPACE_SCRGB)
        return E_INVALIDARG;

    if (!(context = d2d_color_context_alloc(factory, D2D1_COLOR_CONTEXT_TYPE_ICC, space)))
        return E_OUTOFMEMORY;

    if (space == D2D1_COLOR_SPACE_CUSTOM)
    {
        if (!(context->profile = malloc(profile_size)))
        {
            ID2D1ColorContext1_Release(&context->ID2D1ColorContext1_iface);
            return E_OUTOFMEMORY;
        }
        memcpy(context->profile, profile, profile_size);
        context->profile_size = profile_size;
    }
    else
    {
        d2d_color_context_generate_profile(context);
    }

    *out = context;

    return S_OK;
}

HRESULT d2d_color_context_create_from_filename(ID2D1Factory *factory, const WCHAR *filename,
        struct d2d_color_context **out)
{
    struct d2d_color_context *context;
    DWORD size, read_size;
    HANDLE file;
    HRESULT hr;

    if (!filename)
        return E_INVALIDARG;

    if ((file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE)
    {
        WARN("Failed to open %s, error %lu.\n", debugstr_w(filename), GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if ((size = GetFileSize(file, NULL)) == INVALID_FILE_SIZE || !size)
    {
        WARN("Failed to get the size of %s.\n", debugstr_w(filename));
        CloseHandle(file);
        return E_FAIL;
    }

    if (!(context = d2d_color_context_alloc(factory, D2D1_COLOR_CONTEXT_TYPE_ICC, D2D1_COLOR_SPACE_CUSTOM)))
    {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }

    hr = S_OK;
    if (!(context->profile = malloc(size)))
        hr = E_OUTOFMEMORY;
    else if (!ReadFile(file, context->profile, size, &read_size, NULL) || read_size != size)
        hr = E_FAIL;
    else
        context->profile_size = size;

    CloseHandle(file);

    if (FAILED(hr))
    {
        ID2D1ColorContext1_Release(&context->ID2D1ColorContext1_iface);
        return hr;
    }

    *out = context;

    return S_OK;
}

HRESULT d2d_color_context_create_from_wic(ID2D1Factory *factory, IWICColorContext *wic_context,
        struct d2d_color_context **out)
{
    struct d2d_color_context *context;
    WICColorContextType type;
    UINT32 size = 0, exif;
    HRESULT hr;

    if (!wic_context)
        return E_INVALIDARG;

    if (FAILED(hr = IWICColorContext_GetType(wic_context, &type)))
        return hr;

    switch (type)
    {
        case WICColorContextProfile:
            if (FAILED(hr = IWICColorContext_GetProfileBytes(wic_context, 0, NULL, &size)))
                return hr;
            if (!size)
                return E_INVALIDARG;

            if (!(context = d2d_color_context_alloc(factory, D2D1_COLOR_CONTEXT_TYPE_ICC, D2D1_COLOR_SPACE_CUSTOM)))
                return E_OUTOFMEMORY;

            if (!(context->profile = malloc(size)))
                hr = E_OUTOFMEMORY;
            else if (FAILED(hr = IWICColorContext_GetProfileBytes(wic_context, size, context->profile, &size)))
                WARN("Failed to read the WIC profile bytes, hr %#lx.\n", hr);
            else
                context->profile_size = size;

            if (FAILED(hr))
            {
                ID2D1ColorContext1_Release(&context->ID2D1ColorContext1_iface);
                return hr;
            }

            *out = context;

            return S_OK;

        case WICColorContextExifColorSpace:
            if (FAILED(hr = IWICColorContext_GetExifColorSpace(wic_context, &exif)))
                return hr;

            /* 1 is sRGB, 2 is Adobe RGB and 0xffff means uncalibrated. Only
             * sRGB has a definition d2d can act on. */
            if (exif != 1)
                FIXME("Unhandled EXIF colour space %u, treating it as sRGB.\n", exif);

            return d2d_color_context_create(factory, D2D1_COLOR_SPACE_SRGB, NULL, 0, out);

        default:
            FIXME("Unhandled WIC colour context type %#x.\n", type);
            return E_NOTIMPL;
    }
}

HRESULT d2d_color_context_create_from_dxgi_space(ID2D1Factory *factory, DXGI_COLOR_SPACE_TYPE dxgi_space,
        struct d2d_color_context **out)
{
    struct d2d_color_context *context;
    D2D1_COLOR_SPACE space;

    switch (dxgi_space)
    {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
            space = D2D1_COLOR_SPACE_SRGB;
            break;

        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            space = D2D1_COLOR_SPACE_SCRGB;
            break;

        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
            space = D2D1_COLOR_SPACE_CUSTOM;
            break;

        default:
            WARN("Unsupported DXGI colour space %u.\n", dxgi_space);
            return E_INVALIDARG;
    }

    if (!(context = d2d_color_context_alloc(factory, D2D1_COLOR_CONTEXT_TYPE_DXGI, space)))
        return E_OUTOFMEMORY;

    context->dxgi_space = dxgi_space;
    d2d_color_context_generate_profile(context);

    *out = context;

    return S_OK;
}

HRESULT d2d_color_context_create_from_simple_profile(ID2D1Factory *factory,
        const D2D1_SIMPLE_COLOR_PROFILE *simple_profile, struct d2d_color_context **out)
{
    struct d2d_color_context *context;

    if (!simple_profile)
        return E_INVALIDARG;

    if (!(context = d2d_color_context_alloc(factory, D2D1_COLOR_CONTEXT_TYPE_SIMPLE, D2D1_COLOR_SPACE_CUSTOM)))
        return E_OUTOFMEMORY;

    context->simple_profile = *simple_profile;
    d2d_color_context_generate_profile(context);

    *out = context;

    return S_OK;
}
