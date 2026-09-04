/*
 * Copyright 2014 Henri Verbeet for CodeWeavers
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
#include <d3dcompiler.h>

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

#define INITIAL_CLIP_STACK_SIZE 4

static const D2D1_MATRIX_3X2_F identity =
{{{
    1.0f, 0.0f,
    0.0f, 1.0f,
    0.0f, 0.0f,
}}};

static void d2d_device_context_flush_lines(struct d2d_device_context *context);
static BOOL d2d_device_context_batch_line(struct d2d_device_context *context, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, struct d2d_brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style);

struct d2d_draw_text_layout_ctx
{
    ID2D1Brush *brush;
    D2D1_DRAW_TEXT_OPTIONS options;
};

static inline struct d2d_device *impl_from_ID2D1Device(ID2D1Device6 *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_device, ID2D1Device6_iface);
}

static ID2D1Brush *d2d_draw_get_text_brush(struct d2d_draw_text_layout_ctx *context, IUnknown *effect)
{
    ID2D1Brush *brush = NULL;

    if (effect && SUCCEEDED(IUnknown_QueryInterface(effect, &IID_ID2D1Brush, (void**)&brush)))
        return brush;

    ID2D1Brush_AddRef(context->brush);
    return context->brush;
}

static void d2d_rect_intersect(D2D1_RECT_F *dst, const D2D1_RECT_F *src)
{
    if (src->left > dst->left)
        dst->left = src->left;
    if (src->top > dst->top)
        dst->top = src->top;
    if (src->right < dst->right)
        dst->right = src->right;
    if (src->bottom < dst->bottom)
        dst->bottom = src->bottom;
}

static void d2d_rect_set(D2D1_RECT_F *dst, float left, float top, float right, float bottom)
{
    dst->left = left;
    dst->top = top;
    dst->right = right;
    dst->bottom = bottom;
}

static void d2d_size_set(D2D1_SIZE_U *dst, float width, float height)
{
    dst->width = width;
    dst->height = height;
}

static BOOL d2d_clip_stack_init(struct d2d_clip_stack *stack)
{
    if (!(stack->stack = malloc(INITIAL_CLIP_STACK_SIZE * sizeof(*stack->stack))))
        return FALSE;

    stack->size = INITIAL_CLIP_STACK_SIZE;
    stack->count = 0;

    return TRUE;
}

static void d2d_clip_stack_cleanup(struct d2d_clip_stack *stack)
{
    free(stack->stack);
}

static BOOL d2d_clip_stack_push(struct d2d_clip_stack *stack, const D2D1_RECT_F *rect)
{
    D2D1_RECT_F r;

    if (!d2d_array_reserve((void **)&stack->stack, &stack->size, stack->count + 1, sizeof(*stack->stack)))
        return FALSE;

    r = *rect;
    if (stack->count)
        d2d_rect_intersect(&r, &stack->stack[stack->count - 1]);
    stack->stack[stack->count++] = r;

    return TRUE;
}

static void d2d_clip_stack_pop(struct d2d_clip_stack *stack)
{
    if (!stack->count)
        return;
    --stack->count;
}

static BOOL d2d_layer_stack_init(struct d2d_layer_stack *stack)
{
    if (!(stack->stack = malloc(4 * sizeof(*stack->stack))))
        return FALSE;
    stack->size = 4;
    stack->count = 0;
    return TRUE;
}

static void d2d_layer_stack_cleanup(struct d2d_layer_stack *stack)
{
    size_t i;

    for (i = 0; i < stack->count; ++i)
    {
        if (stack->stack[i].layer_bitmap)
            ID2D1Bitmap1_Release(&stack->stack[i].layer_bitmap->ID2D1Bitmap1_iface);
        if (stack->stack[i].prev_target)
            ID2D1Bitmap1_Release(&stack->stack[i].prev_target->ID2D1Bitmap1_iface);
        if (stack->stack[i].prev_bs)
            ID3D11BlendState_Release(stack->stack[i].prev_bs);
        /* PushLayer AddRefs these; PopLayer releases them. On teardown with a
         * non-empty layer stack (error path / unbalanced PushLayer) they would
         * otherwise leak. */
        if (stack->stack[i].mask_geometry)
            ID2D1Geometry_Release(stack->stack[i].mask_geometry);
        if (stack->stack[i].opacity_brush)
            ID2D1Brush_Release(stack->stack[i].opacity_brush);
        if (stack->stack[i].stencil_geometry)
            ID2D1Geometry_Release(stack->stack[i].stencil_geometry);
    }
    free(stack->stack);
}

static BOOL d2d_layer_stack_push(struct d2d_layer_stack *stack,
        const struct d2d_layer_info *info)
{
    if (!d2d_array_reserve((void **)&stack->stack, &stack->size,
            stack->count + 1, sizeof(*stack->stack)))
        return FALSE;
    stack->stack[stack->count] = *info;
    stack->count++;
    return TRUE;
}

static BOOL d2d_layer_stack_pop(struct d2d_layer_stack *stack,
        struct d2d_layer_info *info)
{
    if (!stack->count)
        return FALSE;
    *info = stack->stack[--stack->count];
    return TRUE;
}

/* Ensure stencil buffer exists and matches current render target size. */
static HRESULT d2d_device_context_ensure_stencil(struct d2d_device_context *context)
{
    D3D11_DEPTH_STENCIL_DESC ds_desc;
    D3D11_TEXTURE2D_DESC tex_desc;
    ID3D11Device1 *device = context->d3d_device;
    HRESULT hr;

    if (context->stencil_texture
            && context->stencil_size.width == context->pixel_size.width
            && context->stencil_size.height == context->pixel_size.height)
        return S_OK;

    /* Release old resources. */
    if (context->stencil_dsv)
    {
        ID3D11DepthStencilView_Release(context->stencil_dsv);
        context->stencil_dsv = NULL;
    }
    if (context->stencil_texture)
    {
        ID3D11Texture2D_Release(context->stencil_texture);
        context->stencil_texture = NULL;
    }

    /* Create stencil texture. */
    memset(&tex_desc, 0, sizeof(tex_desc));
    tex_desc.Width = context->pixel_size.width;
    tex_desc.Height = context->pixel_size.height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    if (FAILED(hr = ID3D11Device1_CreateTexture2D(device, &tex_desc, NULL, &context->stencil_texture)))
    {
        WARN("Failed to create stencil texture, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = ID3D11Device1_CreateDepthStencilView(device,
            (ID3D11Resource *)context->stencil_texture, NULL, &context->stencil_dsv)))
    {
        WARN("Failed to create depth-stencil view, hr %#lx.\n", hr);
        ID3D11Texture2D_Release(context->stencil_texture);
        context->stencil_texture = NULL;
        return hr;
    }

    /* Create depth-stencil states (only once — they're device-global). */
    if (!context->stencil_write_state)
    {
        memset(&ds_desc, 0, sizeof(ds_desc));
        ds_desc.DepthEnable = FALSE;
        ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ds_desc.StencilEnable = TRUE;
        ds_desc.StencilReadMask = 0xff;
        ds_desc.StencilWriteMask = 0xff;
        ds_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        ds_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
        ds_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        ds_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        ds_desc.BackFace = ds_desc.FrontFace;

        if (FAILED(hr = ID3D11Device1_CreateDepthStencilState(device, &ds_desc, &context->stencil_write_state)))
        {
            WARN("Failed to create stencil write state, hr %#lx.\n", hr);
            return hr;
        }

        /* DECR_SAT state for PopLayer: decrement stencil where mask geometry covers. */
        ds_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_DECR_SAT;
        ds_desc.BackFace = ds_desc.FrontFace;

        if (FAILED(hr = ID3D11Device1_CreateDepthStencilState(device, &ds_desc, &context->stencil_decr_state)))
        {
            WARN("Failed to create stencil decrement state, hr %#lx.\n", hr);
            return hr;
        }

        /* Test state: pass only where stencil == ref (current depth). */
        ds_desc.StencilWriteMask = 0x00;
        ds_desc.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        ds_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        ds_desc.BackFace = ds_desc.FrontFace;

        if (FAILED(hr = ID3D11Device1_CreateDepthStencilState(device, &ds_desc, &context->stencil_test_state)))
        {
            WARN("Failed to create stencil test state, hr %#lx.\n", hr);
            return hr;
        }
    }

    context->stencil_size = context->pixel_size;
    return S_OK;
}


/* Binds everything a draw with the given shape resources needs and returns the
 * immediate context with the d2d state swapped in; d2d_device_context_draw_finish()
 * swaps it back. Split out so that a line batch can issue several draws
 * against one setup. */
static ID3D11DeviceContext1 *d2d_device_context_draw_setup(struct d2d_device_context *render_target,
        enum d2d_shape_type shape_type, ID3D11Buffer *ib, ID3D11Buffer *vb, unsigned int vb_stride,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush, ID3DDeviceContextState **prev_state)
{
    struct d2d_shape_resources *shape_resources = &render_target->shape_resources[shape_type];
    ID3D11Device1 *device = render_target->d3d_device;
    ID3D11DeviceContext1 *context;
    ID3D11Buffer *vs_cb = render_target->vs_cb, *ps_cb = render_target->ps_cb;
    D3D11_RECT scissor_rect;
    unsigned int offset;
    D3D11_VIEWPORT vp;

    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = render_target->pixel_size.width;
    vp.Height = render_target->pixel_size.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (render_target->cs)
        EnterCriticalSection(render_target->cs);

    ID3D11Device1_GetImmediateContext1(device, &context);
    ID3D11DeviceContext1_SwapDeviceContextState(context, render_target->d3d_state, prev_state);

    ID3D11DeviceContext1_IASetInputLayout(context, shape_resources->il);
    ID3D11DeviceContext1_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_IASetIndexBuffer(context, ib, DXGI_FORMAT_R16_UINT, 0);
    offset = 0;
    ID3D11DeviceContext1_IASetVertexBuffers(context, 0, 1, &vb, &vb_stride, &offset);
    ID3D11DeviceContext1_VSSetConstantBuffers(context, 0, 1, &vs_cb);
    ID3D11DeviceContext1_VSSetShader(context, shape_resources->vs, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers(context, 0, 1, &ps_cb);
    ID3D11DeviceContext1_PSSetShader(context, render_target->ps, NULL, 0);
    ID3D11DeviceContext1_RSSetViewports(context, 1, &vp);
    if (render_target->clip_stack.count)
    {
        const D2D1_RECT_F *clip_rect;
        float l, t, r, b;

        clip_rect = &render_target->clip_stack.stack[render_target->clip_stack.count - 1];
        l = ceilf(clip_rect->left - 0.5f);
        t = ceilf(clip_rect->top - 0.5f);
        r = ceilf(clip_rect->right - 0.5f);
        b = ceilf(clip_rect->bottom - 0.5f);
        /* Clamp to LONG range before cast to prevent integer overflow.
         * Apps like JUCE push full-range clip rects {-FLT_MAX, -FLT_MAX,
         * FLT_MAX, FLT_MAX} which overflow LONG and produce inverted
         * scissor rects, causing GL_INVALID_VALUE in glScissor. */
        scissor_rect.left   = l < -2.0e9f ? (LONG)-2000000000 : l > 2.0e9f ? (LONG)2000000000 : (LONG)l;
        scissor_rect.top    = t < -2.0e9f ? (LONG)-2000000000 : t > 2.0e9f ? (LONG)2000000000 : (LONG)t;
        scissor_rect.right  = r < -2.0e9f ? (LONG)-2000000000 : r > 2.0e9f ? (LONG)2000000000 : (LONG)r;
        scissor_rect.bottom = b < -2.0e9f ? (LONG)-2000000000 : b > 2.0e9f ? (LONG)2000000000 : (LONG)b;
    }
    else
    {
        scissor_rect.left = 0.0f;
        scissor_rect.top = 0.0f;
        scissor_rect.right = render_target->pixel_size.width;
        scissor_rect.bottom = render_target->pixel_size.height;
    }

    /* Clamp scissor rect to render target pixel size to prevent out-of-bounds draws.
     * Without this, PushAxisAlignedClip can produce scissors larger than the target
     * (e.g. due to DPI scaling), causing repeated SOURCE_OVER compositing of tooltip
     * tiles to accumulate alpha at semi-transparent edges. */
    if (scissor_rect.left < 0)
        scissor_rect.left = 0;
    if (scissor_rect.top < 0)
        scissor_rect.top = 0;
    if (scissor_rect.right > (LONG)render_target->pixel_size.width)
        scissor_rect.right = render_target->pixel_size.width;
    if (scissor_rect.bottom > (LONG)render_target->pixel_size.height)
        scissor_rect.bottom = render_target->pixel_size.height;
    ID3D11DeviceContext1_RSSetScissorRects(context, 1, &scissor_rect);
    ID3D11DeviceContext1_RSSetState(context, render_target->rs);
    if (render_target->stencil_writing)
    {
        /* Stencil INCR pass: bind ONLY the DSV, no render target.
         * This increments the stencil buffer without affecting the backbuffer color. */
        ID3D11DeviceContext1_OMSetRenderTargets(context, 0, NULL, render_target->stencil_dsv);
        /* Ref-value 0: irrelevant for ALWAYS/INCR_SAT (only used with REPLACE). */
        ID3D11DeviceContext1_OMSetDepthStencilState(context, render_target->stencil_write_state, 0);
    }
    else if (render_target->stencil_decrementing)
    {
        /* Stencil DECR pass: bind ONLY the DSV, no render target.
         * This decrements the stencil buffer (restoring previous layer's values). */
        ID3D11DeviceContext1_OMSetRenderTargets(context, 0, NULL, render_target->stencil_dsv);
        /* Ref-value 0: irrelevant for ALWAYS/DECR_SAT (only used with REPLACE). */
        ID3D11DeviceContext1_OMSetDepthStencilState(context, render_target->stencil_decr_state, 0);
    }
    else if (render_target->stencil_depth > 0)
    {
        /* Stencil test pass: bind both RTV and DSV. Only pixels where stencil==depth pass. */
        ID3D11DeviceContext1_OMSetRenderTargets(context, 1, &render_target->target.bitmap->rtv,
                render_target->stencil_dsv);
        ID3D11DeviceContext1_OMSetDepthStencilState(context, render_target->stencil_test_state,
                render_target->stencil_depth);
    }
    else
    {
        ID3D11DeviceContext1_OMSetRenderTargets(context, 1, &render_target->target.bitmap->rtv, NULL);
    }
    if (brush)
    {
        /* D2D1_PRIMITIVE_BLEND_COPY disables alpha blending — the pixel shader
         * output replaces the destination directly. Used by JUCE 8 for
         * multiplyAllAlphasInArea() and component alpha compositing. */
        if (render_target->drawing_state.primitiveBlend == D2D1_PRIMITIVE_BLEND_COPY)
            ID3D11DeviceContext1_OMSetBlendState(context, NULL, NULL, D3D11_DEFAULT_SAMPLE_MASK);
        else
            ID3D11DeviceContext1_OMSetBlendState(context, render_target->bs, NULL, D3D11_DEFAULT_SAMPLE_MASK);
        d2d_brush_bind_resources(brush, render_target, 0);
    }
    else
    {
        ID3D11DeviceContext1_OMSetBlendState(context, NULL, NULL, D3D11_DEFAULT_SAMPLE_MASK);
    }
    if (opacity_brush)
        d2d_brush_bind_resources(opacity_brush, render_target, 1);
    if (render_target->linear_text)
    {
        ID3D11DeviceContext1_PSSetShaderResources(context, 4, 1, &render_target->text_dst_srv);
        ID3D11DeviceContext1_PSSetSamplers(context, 2, 1, &render_target->text_dst_sampler);
    }

    return context;
}

static void d2d_device_context_draw_finish(struct d2d_device_context *render_target,
        ID3D11DeviceContext1 *context, ID3DDeviceContextState *prev_state)
{
    ID3D11DeviceContext1_SwapDeviceContextState(context, prev_state, NULL);
    ID3D11DeviceContext1_Release(context);
    ID3DDeviceContextState_Release(prev_state);

    if (render_target->cs)
        LeaveCriticalSection(render_target->cs);
}

static void d2d_device_context_draw(struct d2d_device_context *render_target, enum d2d_shape_type shape_type,
        ID3D11Buffer *ib, unsigned int index_count, ID3D11Buffer *vb, unsigned int vb_stride,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *context;

    /* Lines recorded before this primitive have to land underneath it. */
    d2d_device_context_flush_lines(render_target);

    context = d2d_device_context_draw_setup(render_target, shape_type, ib, vb, vb_stride,
            brush, opacity_brush, &prev_state);

    if (ib)
        ID3D11DeviceContext1_DrawIndexed(context, index_count, 0, 0);
    else
        ID3D11DeviceContext1_Draw(context, index_count, 0);

    d2d_device_context_draw_finish(render_target, context, prev_state);
}

static void d2d_device_context_set_error(struct d2d_device_context *context, HRESULT code)
{
    WARN("code %#lx.\n", code);
    context->error.code = code;
    context->error.tag1 = context->drawing_state.tag1;
    context->error.tag2 = context->drawing_state.tag2;
}

static inline struct d2d_device_context *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_device_context, IUnknown_iface);
}

static inline struct d2d_device_context *impl_from_ID2D1DeviceContext(ID2D1DeviceContext6 *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_device_context, ID2D1DeviceContext6_iface);
}

static void d2d_glyph_mask_destroy(struct d2d_glyph_mask *mask)
{
    if (mask->brush)
        ID2D1Brush_Release(&mask->brush->ID2D1Brush_iface);
    free(mask->coverage);
    free(mask->data);
    free(mask);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_inner_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct d2d_device_context *context = impl_from_IUnknown(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1DeviceContext6)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext5)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext4)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext3)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext2)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext1)
            || IsEqualGUID(iid, &IID_ID2D1DeviceContext)
            || IsEqualGUID(iid, &IID_ID2D1RenderTarget)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1DeviceContext6_AddRef(&context->ID2D1DeviceContext6_iface);
        *out = &context->ID2D1DeviceContext6_iface;
        return S_OK;
    }
    else if (IsEqualGUID(iid, &IID_ID2D1GdiInteropRenderTarget))
    {
        ID2D1GdiInteropRenderTarget_AddRef(&context->ID2D1GdiInteropRenderTarget_iface);
        *out = &context->ID2D1GdiInteropRenderTarget_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_device_context_inner_AddRef(IUnknown *iface)
{
    struct d2d_device_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_device_context_inner_Release(IUnknown *iface)
{
    struct d2d_device_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        unsigned int i, j, k;

        d2d_clip_stack_cleanup(&context->clip_stack);
        d2d_layer_stack_cleanup(&context->layer_stack);
        IDWriteRenderingParams_Release(context->default_text_rendering_params);
        if (context->text_rendering_params)
            IDWriteRenderingParams_Release(context->text_rendering_params);
        if (context->bs)
            ID3D11BlendState_Release(context->bs);
        for (i = 0; i < ARRAY_SIZE(context->subpixel_bs); ++i)
            if (context->subpixel_bs[i])
                ID3D11BlendState_Release(context->subpixel_bs[i]);
        for (i = 0; i < ARRAY_SIZE(context->subpixel_copy_bs); ++i)
            if (context->subpixel_copy_bs[i])
                ID3D11BlendState_Release(context->subpixel_copy_bs[i]);
        if (context->text_dst_srv)
            ID3D11ShaderResourceView_Release(context->text_dst_srv);
        if (context->text_dst)
            ID3D11Texture2D_Release(context->text_dst);
        if (context->text_dst_sampler)
            ID3D11SamplerState_Release(context->text_dst_sampler);
        if (context->stencil_dsv)
            ID3D11DepthStencilView_Release(context->stencil_dsv);
        if (context->stencil_texture)
            ID3D11Texture2D_Release(context->stencil_texture);
        if (context->stencil_write_state)
            ID3D11DepthStencilState_Release(context->stencil_write_state);
        if (context->stencil_test_state)
            ID3D11DepthStencilState_Release(context->stencil_test_state);
        if (context->stencil_decr_state)
            ID3D11DepthStencilState_Release(context->stencil_decr_state);
        ID3D11RasterizerState_Release(context->rs);
        ID3D11Buffer_Release(context->vb);
        ID3D11Buffer_Release(context->ib);
        ID3D11Buffer_Release(context->ps_cb);
        ID3D11PixelShader_Release(context->ps);
        ID3D11Buffer_Release(context->vs_cb);
        for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
        {
            if (context->scratch_vb[i].buffer)
                ID3D11Buffer_Release(context->scratch_vb[i].buffer);
            if (context->scratch_ib[i].buffer)
                ID3D11Buffer_Release(context->scratch_ib[i].buffer);
        }
        free(context->line_batch.vertices);
        free(context->line_batch.faces);
        free(context->line_batch.runs);
        /* Session 6 (C1): free cached rectangle geometry. The geometry object
         * was never registered with the COM factory (we bypassed the refcount
         * path), so direct cleanup + free is correct. */
        if (context->rect_geometry_cache)
        {
            d2d_geometry_cleanup(context->rect_geometry_cache);
            free(context->rect_geometry_cache);
        }
        if (context->glyph_mask_cache)
            d2d_glyph_mask_destroy(context->glyph_mask_cache);
        for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
        {
            ID3D11VertexShader_Release(context->shape_resources[i].vs);
            ID3D11InputLayout_Release(context->shape_resources[i].il);
        }
        for (i = 0; i < D2D_SAMPLER_INTERPOLATION_MODE_COUNT; ++i)
        {
            for (j = 0; j < D2D_SAMPLER_EXTEND_MODE_COUNT; ++j)
            {
                for (k = 0; k < D2D_SAMPLER_EXTEND_MODE_COUNT; ++k)
                {
                    if (context->sampler_states[i][j][k])
                        ID3D11SamplerState_Release(context->sampler_states[i][j][k]);
                }
            }
        }
        if (context->d3d_state)
            ID3DDeviceContextState_Release(context->d3d_state);
        if (context->target.object)
            IUnknown_Release(context->target.object);
        ID3D11Device1_Release(context->d3d_device);
        ID2D1Factory_Release(context->factory);
        ID2D1Device6_Release(&context->device->ID2D1Device6_iface);
        d2d_device_indexed_objects_clear(&context->vertex_buffers);
        free(context);
    }

    return refcount;
}

static const struct IUnknownVtbl d2d_device_context_inner_unknown_vtbl =
{
    d2d_device_context_inner_QueryInterface,
    d2d_device_context_inner_AddRef,
    d2d_device_context_inner_Release,
};

static HRESULT STDMETHODCALLTYPE d2d_device_context_QueryInterface(ID2D1DeviceContext6 *iface,
        REFIID iid, void **out)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    return IUnknown_QueryInterface(context->outer_unknown, iid, out);
}

static ULONG STDMETHODCALLTYPE d2d_device_context_AddRef(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return IUnknown_AddRef(context->outer_unknown);
}

static ULONG STDMETHODCALLTYPE d2d_device_context_Release(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return IUnknown_Release(context->outer_unknown);
}

static void STDMETHODCALLTYPE d2d_device_context_GetFactory(ID2D1DeviceContext6 *iface, ID2D1Factory **factory)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    *factory = render_target->factory;
    ID2D1Factory_AddRef(*factory);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateBitmap(ID2D1DeviceContext6 *iface,
        D2D1_SIZE_U size, const void *src_data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, size {%u, %u}, src_data %p, pitch %u, desc %p, bitmap %p.\n",
            iface, size.width, size.height, src_data, pitch, desc, bitmap);

    if (desc)
    {
        memcpy(&bitmap_desc, desc, sizeof(*desc));
        bitmap_desc.bitmapOptions = 0;
        bitmap_desc.colorContext = NULL;
    }

    if (SUCCEEDED(hr = d2d_bitmap_create(context, size, src_data, pitch, desc ? &bitmap_desc : NULL, &object)))
        *bitmap = (ID2D1Bitmap *)&object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateBitmapFromWicBitmap(ID2D1DeviceContext6 *iface,
        IWICBitmapSource *bitmap_source, const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, bitmap_source %p, desc %p, bitmap %p.\n",
            iface, bitmap_source, desc, bitmap);

    if (desc)
    {
        memcpy(&bitmap_desc, desc, sizeof(*desc));
        bitmap_desc.bitmapOptions = 0;
        bitmap_desc.colorContext = NULL;
    }

    if (SUCCEEDED(hr = d2d_bitmap_create_from_wic_bitmap(context, bitmap_source, desc ? &bitmap_desc : NULL, &object)))
        *bitmap = (ID2D1Bitmap *)&object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateSharedBitmap(ID2D1DeviceContext6 *iface,
        REFIID iid, void *data, const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, iid %s, data %p, desc %p, bitmap %p.\n",
            iface, debugstr_guid(iid), data, desc, bitmap);

    if (desc)
    {
        memcpy(&bitmap_desc, desc, sizeof(*desc));
        if (IsEqualIID(iid, &IID_IDXGISurface) || IsEqualIID(iid, &IID_IDXGISurface1))
            bitmap_desc.bitmapOptions = d2d_get_bitmap_options_for_surface(data);
        else
            bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        bitmap_desc.colorContext = NULL;
    }

    if (SUCCEEDED(hr = d2d_bitmap_create_shared(context, iid, data, desc ? &bitmap_desc : NULL, &object)))
        *bitmap = (ID2D1Bitmap *)&object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateBitmapBrush(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *bitmap, const D2D1_BITMAP_BRUSH_PROPERTIES *bitmap_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1BitmapBrush **brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, bitmap %p, bitmap_brush_desc %p, brush_desc %p, brush %p.\n",
            iface, bitmap, bitmap_brush_desc, brush_desc, brush);

    if (SUCCEEDED(hr = d2d_bitmap_brush_create(context->factory, bitmap, (const D2D1_BITMAP_BRUSH_PROPERTIES1 *)bitmap_brush_desc,
            brush_desc, &object)))
        *brush = (ID2D1BitmapBrush *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateSolidColorBrush(ID2D1DeviceContext6 *iface,
        const D2D1_COLOR_F *color, const D2D1_BRUSH_PROPERTIES *desc, ID2D1SolidColorBrush **brush)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, color %p, desc %p, brush %p.\n", iface, color, desc, brush);

    if (SUCCEEDED(hr = d2d_solid_color_brush_create(render_target->factory, color, desc, &object)))
        *brush = (ID2D1SolidColorBrush *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateGradientStopCollection(ID2D1DeviceContext6 *iface,
        const D2D1_GRADIENT_STOP *stops, UINT32 stop_count, D2D1_GAMMA gamma, D2D1_EXTEND_MODE extend_mode,
        ID2D1GradientStopCollection **gradient)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_gradient *object;
    HRESULT hr;

    TRACE("iface %p, stops %p, stop_count %u, gamma %#x, extend_mode %#x, gradient %p.\n",
            iface, stops, stop_count, gamma, extend_mode, gradient);

    /* The ID2D1RenderTarget flavour interpolates in the colour space implied by
     * the gamma setting, at 8bpc precision, with premultiplied alpha. */
    if (SUCCEEDED(hr = d2d_gradient_create(render_target->factory, render_target->d3d_device,
            stops, stop_count, gamma, extend_mode,
            gamma == D2D1_GAMMA_1_0 ? D2D1_COLOR_SPACE_SCRGB : D2D1_COLOR_SPACE_SRGB,
            gamma == D2D1_GAMMA_1_0 ? D2D1_COLOR_SPACE_SCRGB : D2D1_COLOR_SPACE_SRGB,
            D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_COLOR_INTERPOLATION_MODE_PREMULTIPLIED, &object)))
        *gradient = d2d_gradient_iface(object);

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateLinearGradientBrush(ID2D1DeviceContext6 *iface,
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES *gradient_brush_desc, const D2D1_BRUSH_PROPERTIES *brush_desc,
        ID2D1GradientStopCollection *gradient, ID2D1LinearGradientBrush **brush)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, gradient_brush_desc %p, brush_desc %p, gradient %p, brush %p.\n",
            iface, gradient_brush_desc, brush_desc, gradient, brush);

    if (SUCCEEDED(hr = d2d_linear_gradient_brush_create(render_target->factory, gradient_brush_desc, brush_desc,
        gradient, &object)))
        *brush = (ID2D1LinearGradientBrush *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateRadialGradientBrush(ID2D1DeviceContext6 *iface,
        const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES *gradient_brush_desc, const D2D1_BRUSH_PROPERTIES *brush_desc,
        ID2D1GradientStopCollection *gradient, ID2D1RadialGradientBrush **brush)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, gradient_brush_desc %p, brush_desc %p, gradient %p, brush %p.\n",
            iface, gradient_brush_desc, brush_desc, gradient, brush);

    if (SUCCEEDED(hr = d2d_radial_gradient_brush_create(render_target->factory,
            gradient_brush_desc, brush_desc, gradient, &object)))
        *brush = (ID2D1RadialGradientBrush *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateCompatibleRenderTarget(ID2D1DeviceContext6 *iface,
        const D2D1_SIZE_F *size, const D2D1_SIZE_U *pixel_size, const D2D1_PIXEL_FORMAT *format,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS options, ID2D1BitmapRenderTarget **rt)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_bitmap_render_target *object;
    HRESULT hr;

    TRACE("iface %p, size %p, pixel_size %p, format %p, options %#x, render_target %p.\n",
            iface, size, pixel_size, format, options, rt);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d2d_bitmap_render_target_init(object, render_target, size, pixel_size,
            format, options)))
    {
        WARN("Failed to initialise render target, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    TRACE("Created render target %p.\n", object);
    *rt = &object->ID2D1BitmapRenderTarget_iface;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateLayer(ID2D1DeviceContext6 *iface,
        const D2D1_SIZE_F *size, ID2D1Layer **layer)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_layer *object;
    HRESULT hr;

    TRACE("iface %p, size %p, layer %p.\n", iface, size, layer);

    if (SUCCEEDED(hr = d2d_layer_create(render_target->factory, size, &object)))
        *layer = &object->ID2D1Layer_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateMesh(ID2D1DeviceContext6 *iface, ID2D1Mesh **mesh)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_mesh *object;
    HRESULT hr;

    TRACE("iface %p, mesh %p.\n", iface, mesh);

    if (SUCCEEDED(hr = d2d_mesh_create(render_target->factory, &object)))
        *mesh = &object->ID2D1Mesh_iface;

    return hr;
}

static void STDMETHODCALLTYPE d2d_device_context_DrawLine(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F p0, D2D1_POINT_2F p1, ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    ID2D1PathGeometry *geometry;
    ID2D1GeometrySink *sink;
    HRESULT hr;

    TRACE("iface %p, p0 %s, p1 %s, brush %p, stroke_width %.8e, stroke_style %p.\n",
            iface, debug_d2d_point_2f(&p0), debug_d2d_point_2f(&p1), brush, stroke_width, stroke_style);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_line(context->target.command_list, context, p0, p1, brush, stroke_width, stroke_style);
        return;
    }

    if (d2d_device_context_batch_line(context, &p0, &p1, unsafe_impl_from_ID2D1Brush(brush),
            stroke_width, stroke_style))
        return;

    if (FAILED(hr = ID2D1Factory_CreatePathGeometry(context->factory, &geometry)))
    {
        WARN("Failed to create path geometry, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = ID2D1PathGeometry_Open(geometry, &sink)))
    {
        WARN("Failed to open geometry sink, hr %#lx.\n", hr);
        ID2D1PathGeometry_Release(geometry);
        return;
    }

    ID2D1GeometrySink_BeginFigure(sink, p0, D2D1_FIGURE_BEGIN_HOLLOW);
    ID2D1GeometrySink_AddLine(sink, p1);
    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
    if (FAILED(hr = ID2D1GeometrySink_Close(sink)))
        WARN("Failed to close geometry sink, hr %#lx.\n", hr);
    ID2D1GeometrySink_Release(sink);

    ID2D1DeviceContext6_DrawGeometry(iface, (ID2D1Geometry *)geometry, brush, stroke_width, stroke_style);
    ID2D1PathGeometry_Release(geometry);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawRectangle(ID2D1DeviceContext6 *iface,
        const D2D1_RECT_F *rect, ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    ID2D1RectangleGeometry *geometry;
    HRESULT hr;

    TRACE("iface %p, rect %s, brush %p, stroke_width %.8e, stroke_style %p.\n",
            iface, debug_d2d_rect_f(rect), brush, stroke_width, stroke_style);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_rectangle(context->target.command_list, context, rect, brush, stroke_width, stroke_style);
        return;
    }

    if (FAILED(hr = ID2D1Factory_CreateRectangleGeometry(context->factory, rect, &geometry)))
    {
        ERR("Failed to create geometry, hr %#lx.\n", hr);
        return;
    }

    ID2D1DeviceContext6_DrawGeometry(iface, (ID2D1Geometry *)geometry, brush, stroke_width, stroke_style);
    ID2D1RectangleGeometry_Release(geometry);
}

/* Forward declaration for Session 6 (C1) rect-geometry cache in FillRectangle. */
static void d2d_device_context_fill_geometry(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, struct d2d_brush *opacity_brush);

/* Rectangle geometry shared by FillRectangle() and the glyph run paths.
 * Session 6 (C1): Serum2's GUI hits ~22k FillRectangle/s, each of which was
 * calloc'ing a ~1600-byte struct d2d_geometry plus ancillary arrays and
 * freeing them immediately; a single retained object is re-initialised in
 * place instead. It bypasses the COM factory path and never leaves the
 * context, so it is claimed and returned around a single fill; a second
 * claimant while it is out builds its own, and the one displaced when both
 * come back is destroyed, which keeps the cache bounded at one. */
static struct d2d_geometry *d2d_device_context_get_rect_geometry(struct d2d_device_context *context,
        const D2D1_RECT_F *rect)
{
    struct d2d_geometry *geometry;

    if ((geometry = InterlockedExchangePointer((void **)&context->rect_geometry_cache, NULL)))
    {
        d2d_rectangle_geometry_reinit(geometry, rect);
        return geometry;
    }

    if (!(geometry = calloc(1, sizeof(*geometry))))
    {
        ERR("Failed to allocate rectangle geometry.\n");
        return NULL;
    }
    if (FAILED(d2d_rectangle_geometry_init(geometry, context->factory, rect)))
    {
        free(geometry);
        return NULL;
    }

    return geometry;
}

static void d2d_device_context_put_rect_geometry(struct d2d_device_context *context,
        struct d2d_geometry *geometry)
{
    struct d2d_geometry *displaced;

    if ((displaced = InterlockedExchangePointer((void **)&context->rect_geometry_cache, geometry)))
    {
        d2d_geometry_cleanup(displaced);
        free(displaced);
    }
}

static void STDMETHODCALLTYPE d2d_device_context_FillRectangle(ID2D1DeviceContext6 *iface,
        const D2D1_RECT_F *rect, ID2D1Brush *brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_geometry *geometry;

    TRACE("iface %p, rect %s, brush %p.\n", iface, debug_d2d_rect_f(rect), brush);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_fill_rectangle(context->target.command_list, context, rect, brush);
        return;
    }

    if (!(geometry = d2d_device_context_get_rect_geometry(context, rect)))
        return;

    d2d_device_context_fill_geometry(context, geometry, unsafe_impl_from_ID2D1Brush(brush), NULL);

    d2d_device_context_put_rect_geometry(context, geometry);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawRoundedRectangle(ID2D1DeviceContext6 *iface,
        const D2D1_ROUNDED_RECT *rect, ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    ID2D1RoundedRectangleGeometry *geometry;
    HRESULT hr;

    TRACE("iface %p, rect %p, brush %p, stroke_width %.8e, stroke_style %p.\n",
            iface, rect, brush, stroke_width, stroke_style);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (FAILED(hr = ID2D1Factory_CreateRoundedRectangleGeometry(render_target->factory, rect, &geometry)))
    {
        ERR("Failed to create geometry, hr %#lx.\n", hr);
        return;
    }

    ID2D1DeviceContext6_DrawGeometry(iface, (ID2D1Geometry *)geometry, brush, stroke_width, stroke_style);
    ID2D1RoundedRectangleGeometry_Release(geometry);
}

static void STDMETHODCALLTYPE d2d_device_context_FillRoundedRectangle(ID2D1DeviceContext6 *iface,
        const D2D1_ROUNDED_RECT *rect, ID2D1Brush *brush)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    ID2D1RoundedRectangleGeometry *geometry;
    HRESULT hr;

    TRACE("iface %p, rect %p, brush %p.\n", iface, rect, brush);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (FAILED(hr = ID2D1Factory_CreateRoundedRectangleGeometry(render_target->factory, rect, &geometry)))
    {
        ERR("Failed to create geometry, hr %#lx.\n", hr);
        return;
    }

    ID2D1DeviceContext6_FillGeometry(iface, (ID2D1Geometry *)geometry, brush, NULL);
    ID2D1RoundedRectangleGeometry_Release(geometry);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawEllipse(ID2D1DeviceContext6 *iface,
        const D2D1_ELLIPSE *ellipse, ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    ID2D1EllipseGeometry *geometry;
    HRESULT hr;

    TRACE("iface %p, ellipse %p, brush %p, stroke_width %.8e, stroke_style %p.\n",
            iface, ellipse, brush, stroke_width, stroke_style);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (FAILED(hr = ID2D1Factory_CreateEllipseGeometry(render_target->factory, ellipse, &geometry)))
    {
        ERR("Failed to create geometry, hr %#lx.\n", hr);
        return;
    }

    ID2D1DeviceContext6_DrawGeometry(iface, (ID2D1Geometry *)geometry, brush, stroke_width, stroke_style);
    ID2D1EllipseGeometry_Release(geometry);
}

static void STDMETHODCALLTYPE d2d_device_context_FillEllipse(ID2D1DeviceContext6 *iface,
        const D2D1_ELLIPSE *ellipse, ID2D1Brush *brush)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    ID2D1EllipseGeometry *geometry;
    HRESULT hr;

    TRACE("iface %p, ellipse %p, brush %p.\n", iface, ellipse, brush);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (FAILED(hr = ID2D1Factory_CreateEllipseGeometry(render_target->factory, ellipse, &geometry)))
    {
        ERR("Failed to create geometry, hr %#lx.\n", hr);
        return;
    }

    ID2D1DeviceContext6_FillGeometry(iface, (ID2D1Geometry *)geometry, brush, NULL);
    ID2D1EllipseGeometry_Release(geometry);
}

static HRESULT d2d_device_context_update_ps_cb(struct d2d_device_context *context,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush, BOOL outline, BOOL is_arc)
{
    D3D11_MAPPED_SUBRESOURCE map_desc;
    ID3D11DeviceContext *d3d_context;
    struct d2d_ps_cb new_cb;
    HRESULT hr;

    /* Padding is compared along with the rest by the cache below, so it has to
     * hold a defined value. */
    memset(&new_cb, 0, sizeof(new_cb));
    new_cb.outline = outline;
    new_cb.is_arc = is_arc;
    new_cb.aa_mode = (context->drawing_state.antialiasMode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    new_cb.srgb_encode = context->srgb_encode;
    new_cb.linear_text = context->linear_text;
    new_cb.dst_scale_x = context->text_dst_scale_x;
    new_cb.dst_offset_x = context->text_dst_offset_x;
    new_cb.dst_scale_y = context->text_dst_scale_y;
    new_cb.dst_offset_y = context->text_dst_offset_y;
    if (!d2d_brush_fill_cb(brush, &new_cb.colour_brush))
        WARN("Failed to initialize colour brush buffer.\n");
    if (!d2d_brush_fill_cb(opacity_brush, &new_cb.opacity_brush))
        WARN("Failed to initialize opacity brush buffer.\n");

    if (context->ps_cb_cache_valid && !memcmp(&new_cb, &context->ps_cb_cache, sizeof(new_cb)))
        return S_OK;

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);

    if (FAILED(hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)context->ps_cb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map_desc)))
    {
        WARN("Failed to map constant buffer, hr %#lx.\n", hr);
        ID3D11DeviceContext_Release(d3d_context);
        return hr;
    }

    memcpy(map_desc.pData, &new_cb, sizeof(new_cb));
    ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)context->ps_cb, 0);
    ID3D11DeviceContext_Release(d3d_context);

    context->ps_cb_cache = new_cb;
    context->ps_cb_cache_valid = TRUE;

    return S_OK;
}

static HRESULT d2d_device_context_update_vs_cb(struct d2d_device_context *context,
        const D2D_MATRIX_3X2_F *geometry_transform, float stroke_width, float miter_limit)
{
    D3D11_MAPPED_SUBRESOURCE map_desc;
    ID3D11DeviceContext *d3d_context;
    const D2D1_MATRIX_3X2_F *w;
    struct d2d_vs_cb new_cb;
    float tmp_x, tmp_y;
    HRESULT hr;

    new_cb.transform_geometry._11 = geometry_transform->_11;
    new_cb.transform_geometry._21 = geometry_transform->_21;
    new_cb.transform_geometry._31 = geometry_transform->_31;
    new_cb.transform_geometry.miter_limit = miter_limit;
    new_cb.transform_geometry._12 = geometry_transform->_12;
    new_cb.transform_geometry._22 = geometry_transform->_22;
    new_cb.transform_geometry._32 = geometry_transform->_32;
    new_cb.transform_geometry.stroke_width = stroke_width;

    w = &context->drawing_state.transform;

    tmp_x = context->desc.dpiX / 96.0f;
    new_cb.transform_rtx.x = w->_11 * tmp_x;
    new_cb.transform_rtx.y = w->_21 * tmp_x;
    new_cb.transform_rtx.z = w->_31 * tmp_x;
    new_cb.transform_rtx.w = 2.0f / context->pixel_size.width;

    tmp_y = context->desc.dpiY / 96.0f;
    new_cb.transform_rty.x = w->_12 * tmp_y;
    new_cb.transform_rty.y = w->_22 * tmp_y;
    new_cb.transform_rty.z = w->_32 * tmp_y;
    new_cb.transform_rty.w = -2.0f / context->pixel_size.height;

    if (context->vs_cb_cache_valid && !memcmp(&new_cb, &context->vs_cb_cache, sizeof(new_cb)))
        return S_OK;

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);

    if (FAILED(hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)context->vs_cb,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map_desc)))
    {
        WARN("Failed to map constant buffer, hr %#lx.\n", hr);
        ID3D11DeviceContext_Release(d3d_context);
        return hr;
    }

    memcpy(map_desc.pData, &new_cb, sizeof(new_cb));
    ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)context->vs_cb, 0);
    ID3D11DeviceContext_Release(d3d_context);

    context->vs_cb_cache = new_cb;
    context->vs_cb_cache_valid = TRUE;

    return S_OK;
}

static HRESULT d2d_device_context_get_scratch_buffer(struct d2d_device_context *ctx,
        UINT bind_flags, unsigned int min_size, struct d2d_scratch_buffer *scratch,
        const void *data, ID3D11Buffer **out)
{
    D3D11_BUFFER_DESC desc;
    HRESULT hr;

    if (scratch->buffer && scratch->size >= min_size)
        goto update;

    if (scratch->buffer)
        ID3D11Buffer_Release(scratch->buffer);
    scratch->buffer = NULL;

    desc.ByteWidth = min_size + min_size / 2;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    if (FAILED(hr = ID3D11Device1_CreateBuffer(ctx->d3d_device, &desc, NULL, &scratch->buffer)))
        return hr;
    scratch->size = desc.ByteWidth;

update:
    {
        ID3D11DeviceContext *context;
        D3D11_BOX box;

        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = min_size;
        box.bottom = 1;
        box.back = 1;

        ID3D11Device1_GetImmediateContext(ctx->d3d_device, &context);
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)scratch->buffer,
                0, &box, data, min_size, 0);
        ID3D11DeviceContext_Release(context);
    }

    *out = scratch->buffer;
    return S_OK;
}


/* Render mask geometry triangles into the stencil buffer.
 * Temporarily sets identity transform since geometry is in device coords. */
static void d2d_device_context_render_mask_to_stencil(struct d2d_device_context *context,
        ID2D1Geometry *geometry)
{
    const struct d2d_geometry *mask_geo;
    D2D1_MATRIX_3X2_F saved_transform;
    D3D11_SUBRESOURCE_DATA buf_data;
    D3D11_BUFFER_DESC buf_desc;
    ID3D11Buffer *geo_ib, *geo_vb;
    HRESULT hr;

    mask_geo = unsafe_impl_from_ID2D1Geometry(geometry);
    if (!mask_geo->fill.face_count)
        return;

    saved_transform = context->drawing_state.transform;
    context->drawing_state.transform._11 = 1.0f;
    context->drawing_state.transform._12 = 0.0f;
    context->drawing_state.transform._21 = 0.0f;
    context->drawing_state.transform._22 = 1.0f;
    context->drawing_state.transform._31 = 0.0f;
    context->drawing_state.transform._32 = 0.0f;

    /* Bail on CB map failure rather than drawing the stencil mask with stale
     * constants, which would yield an undefined clip mask. */
    if (FAILED(hr = d2d_device_context_update_vs_cb(context, &mask_geo->transform, 0.0f, 0.0f))
            || FAILED(hr = d2d_device_context_update_ps_cb(context, NULL, NULL, FALSE, FALSE)))
    {
        WARN("Failed to update constant buffers, hr %#lx.\n", hr);
        context->drawing_state.transform = saved_transform;
        return;
    }

    buf_desc.Usage = D3D11_USAGE_DEFAULT;
    buf_desc.CPUAccessFlags = 0;
    buf_desc.MiscFlags = 0;
    buf_data.SysMemPitch = 0;
    buf_data.SysMemSlicePitch = 0;

    buf_desc.ByteWidth = mask_geo->fill.face_count * sizeof(*mask_geo->fill.faces);
    buf_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    buf_data.pSysMem = mask_geo->fill.faces;
    if (SUCCEEDED(ID3D11Device1_CreateBuffer(context->d3d_device, &buf_desc, &buf_data, &geo_ib)))
    {
        buf_desc.ByteWidth = mask_geo->fill.vertex_count * sizeof(*mask_geo->fill.vertices);
        buf_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buf_data.pSysMem = mask_geo->fill.vertices;
        if (SUCCEEDED(ID3D11Device1_CreateBuffer(context->d3d_device, &buf_desc, &buf_data, &geo_vb)))
        {
            d2d_device_context_draw(context, D2D_SHAPE_TYPE_TRIANGLE, geo_ib,
                    3 * mask_geo->fill.face_count, geo_vb,
                    sizeof(*mask_geo->fill.vertices), NULL, NULL);
            ID3D11Buffer_Release(geo_vb);
        }
        ID3D11Buffer_Release(geo_ib);
    }

    context->drawing_state.transform = saved_transform;
}

static void d2d_device_context_line_batch_reset(struct d2d_line_batch *batch)
{
    batch->vertex_count = 0;
    batch->face_count = 0;
    batch->run_count = 0;
}

/* Draws the pending line batch: one index/vertex upload, one state setup, and
 * a draw per colour run. The outline vertex shader takes the world transform
 * from the drawing state, so that is swapped for the batch's own while it
 * draws; everything else the shaders read (antialias mode, primitive blend,
 * clip and layer state, target) is guaranteed unchanged since the batch was
 * opened, because the setters flush it. */
static void d2d_device_context_flush_lines(struct d2d_device_context *context)
{
    struct d2d_line_batch *batch = &context->line_batch;
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *d3d_context;
    D2D1_MATRIX_3X2_F saved_transform;
    struct d2d_brush brush;
    ID3D11Buffer *ib, *vb;
    HRESULT hr;
    size_t i;

    if (!batch->run_count || batch->flushing)
        return;
    batch->flushing = TRUE;

    saved_transform = context->drawing_state.transform;
    context->drawing_state.transform = batch->transform;

    memset(&brush, 0, sizeof(brush));
    brush.type = D2D_BRUSH_TYPE_SOLID;
    brush.opacity = batch->runs[0].opacity;
    brush.u.solid.color = batch->runs[0].colour;

    if (FAILED(hr = d2d_device_context_update_vs_cb(context, &identity, batch->stroke_width, batch->miter_limit)))
    {
        WARN("Failed to update vs constant buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (FAILED(hr = d2d_device_context_update_ps_cb(context, &brush, NULL, TRUE, FALSE)))
    {
        WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (FAILED(hr = d2d_device_context_get_scratch_buffer(context, D3D11_BIND_INDEX_BUFFER,
            batch->face_count * sizeof(*batch->faces),
            &context->scratch_ib[D2D_SHAPE_TYPE_OUTLINE], batch->faces, &ib)))
    {
        WARN("Failed to create index buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (FAILED(hr = d2d_device_context_get_scratch_buffer(context, D3D11_BIND_VERTEX_BUFFER,
            batch->vertex_count * sizeof(*batch->vertices),
            &context->scratch_vb[D2D_SHAPE_TYPE_OUTLINE], batch->vertices, &vb)))
    {
        ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
        goto done;
    }

    d3d_context = d2d_device_context_draw_setup(context, D2D_SHAPE_TYPE_OUTLINE, ib, vb,
            sizeof(*batch->vertices), &brush, NULL, &prev_state);
    for (i = 0; i < batch->run_count; ++i)
    {
        const struct d2d_line_run *run = &batch->runs[i];

        if (i)
        {
            brush.opacity = run->opacity;
            brush.u.solid.color = run->colour;
            if (FAILED(hr = d2d_device_context_update_ps_cb(context, &brush, NULL, TRUE, FALSE)))
            {
                WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
                break;
            }
        }
        ID3D11DeviceContext1_DrawIndexed(d3d_context, 3 * run->face_count, 3 * run->face_start, 0);
    }
    d2d_device_context_draw_finish(context, d3d_context, prev_state);

done:
    context->drawing_state.transform = saved_transform;
    d2d_device_context_line_batch_reset(batch);
    batch->flushing = FALSE;
}

/* Appends a line to the batch. Returns FALSE when the line cannot be batched;
 * the caller then draws it the ordinary way, which flushes the batch first. The
 * geometry is what d2d_geometry_outline_add_line_segment() produces for an open
 * two-point figure: a quad the outline vertex shader widens to the stroke. */
static BOOL d2d_device_context_batch_line(struct d2d_device_context *context, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, struct d2d_brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_stroke_style *stroke_style_impl = unsafe_impl_from_ID2D1StrokeStyle(stroke_style);
    struct d2d_line_batch *batch = &context->line_batch;
    struct d2d_outline_vertex *v;
    float qx, qy, len, miter_limit;
    struct d2d_line_run *run;
    struct d2d_face *f;
    size_t base;

    if (!brush || brush->type != D2D_BRUSH_TYPE_SOLID
            || context->target.type != D2D_TARGET_BITMAP || batch->flushing)
        return FALSE;

    /* The same two things DrawGeometry() takes from a stroke style; caps,
     * joins and dashes are ignored there as well. */
    miter_limit = 10.0f;
    if (stroke_style_impl)
    {
        if (stroke_style_impl->desc.transformType == D2D1_STROKE_TRANSFORM_TYPE_FIXED)
            stroke_width /= context->drawing_state.transform.m11;
        miter_limit = stroke_style_impl->desc.miterLimit;
    }

    /* One vertex buffer with 16-bit indices per batch. */
    if (batch->run_count && (batch->vertex_count + 4 > 0xffff || batch->stroke_width != stroke_width
            || batch->miter_limit != miter_limit
            || memcmp(&batch->transform, &context->drawing_state.transform, sizeof(batch->transform))))
        d2d_device_context_flush_lines(context);

    if (!d2d_array_reserve((void **)&batch->vertices, &batch->vertices_size,
            batch->vertex_count + 4, sizeof(*batch->vertices))
            || !d2d_array_reserve((void **)&batch->faces, &batch->faces_size,
            batch->face_count + 2, sizeof(*batch->faces))
            || !d2d_array_reserve((void **)&batch->runs, &batch->runs_size,
            batch->run_count + 1, sizeof(*batch->runs)))
        return FALSE;

    if (!batch->run_count)
    {
        batch->transform = context->drawing_state.transform;
        batch->stroke_width = stroke_width;
        batch->miter_limit = miter_limit;
    }

    run = batch->run_count ? &batch->runs[batch->run_count - 1] : NULL;
    if (!run || run->opacity != brush->opacity
            || memcmp(&run->colour, &brush->u.solid.color, sizeof(run->colour)))
    {
        run = &batch->runs[batch->run_count++];
        run->colour = brush->u.solid.color;
        run->opacity = brush->opacity;
        run->face_start = batch->face_count;
        run->face_count = 0;
    }

    qx = p1->x - p0->x;
    qy = p1->y - p0->y;
    if ((len = sqrtf(qx * qx + qy * qy)) != 0.0f)
    {
        qx /= len;
        qy /= len;
    }

    base = batch->vertex_count;
    v = &batch->vertices[base];
    d2d_point_set(&v[0].position, p0->x, p0->y);
    d2d_point_set(&v[0].prev, qx, qy);
    d2d_point_set(&v[0].next, qx, qy);
    d2d_point_set(&v[1].position, p0->x, p0->y);
    d2d_point_set(&v[1].prev, -2.0f * qx, -2.0f * qy);
    d2d_point_set(&v[1].next, -2.0f * qx, -2.0f * qy);
    d2d_point_set(&v[2].position, p1->x, p1->y);
    d2d_point_set(&v[2].prev, qx, qy);
    d2d_point_set(&v[2].next, qx, qy);
    d2d_point_set(&v[3].position, p1->x, p1->y);
    d2d_point_set(&v[3].prev, -2.0f * qx, -2.0f * qy);
    d2d_point_set(&v[3].next, -2.0f * qx, -2.0f * qy);
    batch->vertex_count += 4;

    f = &batch->faces[batch->face_count];
    f[0].v[0] = base + 0;
    f[0].v[1] = base + 1;
    f[0].v[2] = base + 2;
    f[1].v[0] = base + 2;
    f[1].v[1] = base + 1;
    f[1].v[2] = base + 3;
    batch->face_count += 2;
    run->face_count += 2;

    return TRUE;
}

static void d2d_device_context_draw_geometry(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, float stroke_width, float miter_limit)
{
    ID3D11Buffer *ib, *vb;
    HRESULT hr;

    if (FAILED(hr = d2d_device_context_update_vs_cb(render_target, &geometry->transform, stroke_width, miter_limit)))
    {
        WARN("Failed to update vs constant buffer, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = d2d_device_context_update_ps_cb(render_target, brush, NULL, TRUE, FALSE)))
    {
        WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
        return;
    }

    if (geometry->outline.face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                geometry->outline.face_count * sizeof(*geometry->outline.faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_OUTLINE],
                geometry->outline.faces, &ib)))
        {
            WARN("Failed to create index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                geometry->outline.vertex_count * sizeof(*geometry->outline.vertices),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_OUTLINE],
                geometry->outline.vertices, &vb)))
        {
            ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
            return;
        }

        d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_OUTLINE, ib, 3 * geometry->outline.face_count, vb,
                sizeof(*geometry->outline.vertices), brush, NULL);
    }

    if (geometry->outline.bezier_face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                geometry->outline.bezier_face_count * sizeof(*geometry->outline.bezier_faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_BEZIER_OUTLINE],
                geometry->outline.bezier_faces, &ib)))
        {
            WARN("Failed to create curves index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                geometry->outline.bezier_count * sizeof(*geometry->outline.beziers),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_BEZIER_OUTLINE],
                geometry->outline.beziers, &vb)))
        {
            ERR("Failed to create curves vertex buffer, hr %#lx.\n", hr);
            return;
        }

        d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_BEZIER_OUTLINE, ib,
                3 * geometry->outline.bezier_face_count, vb,
                sizeof(*geometry->outline.beziers), brush, NULL);
    }

    if (geometry->outline.arc_face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                geometry->outline.arc_face_count * sizeof(*geometry->outline.arc_faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_ARC_OUTLINE],
                geometry->outline.arc_faces, &ib)))
        {
            WARN("Failed to create arcs index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                geometry->outline.arc_count * sizeof(*geometry->outline.arcs),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_ARC_OUTLINE],
                geometry->outline.arcs, &vb)))
        {
            ERR("Failed to create arcs vertex buffer, hr %#lx.\n", hr);
            return;
        }

        if (SUCCEEDED(d2d_device_context_update_ps_cb(render_target, brush, NULL, TRUE, TRUE)))
            d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_ARC_OUTLINE, ib,
                    3 * geometry->outline.arc_face_count, vb,
                    sizeof(*geometry->outline.arcs), brush, NULL);
    }
}

static void STDMETHODCALLTYPE d2d_device_context_DrawGeometry(ID2D1DeviceContext6 *iface,
        ID2D1Geometry *geometry, ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    const struct d2d_geometry *geometry_impl = unsafe_impl_from_ID2D1Geometry(geometry);
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *brush_impl = unsafe_impl_from_ID2D1Brush(brush);
    struct d2d_stroke_style *stroke_style_impl = unsafe_impl_from_ID2D1StrokeStyle(stroke_style);

    TRACE("iface %p, geometry %p, brush %p, stroke_width %.8e, stroke_style %p.\n",
            iface, geometry, brush, stroke_width, stroke_style);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_geometry(context->target.command_list, context, geometry, brush,
                stroke_width, stroke_style);
        return;
    }

    if (stroke_style)
    {
        static int once;
        if (!once++)
            FIXME("Ignoring stroke style %p.\n", stroke_style);
    }

    if (stroke_style_impl)
    {
        if (stroke_style_impl->desc.transformType == D2D1_STROKE_TRANSFORM_TYPE_FIXED)
            stroke_width /= context->drawing_state.transform.m11;
    }

    {
        float miter_limit = 10.0f;
        if (stroke_style_impl)
            miter_limit = stroke_style_impl->desc.miterLimit;
        d2d_device_context_draw_geometry(context, geometry_impl, brush_impl, stroke_width, miter_limit);
    }
}

/* Draw filled triangles without anti-aliasing (plain index+vertex buffer path). */
static void d2d_device_context_fill_triangles(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    ID3D11Buffer *ib, *vb;
    HRESULT hr;

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_INDEX_BUFFER,
            geometry->fill.face_count * sizeof(*geometry->fill.faces),
            &render_target->scratch_ib[D2D_SHAPE_TYPE_TRIANGLE],
            geometry->fill.faces, &ib)))
    {
        WARN("Failed to create index buffer, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_VERTEX_BUFFER,
            geometry->fill.vertex_count * sizeof(*geometry->fill.vertices),
            &render_target->scratch_vb[D2D_SHAPE_TYPE_TRIANGLE],
            geometry->fill.vertices, &vb)))
    {
        ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
        return;
    }

    d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_TRIANGLE, ib, 3 * geometry->fill.face_count, vb,
            sizeof(*geometry->fill.vertices), brush, opacity_brush);
}

/* Hash a vertex position by its bit pattern. The fill vertices and the corners
 * of the curve triangles are copies of the same figure points, so equal points
 * are bit-identical and an exact match is the right test here. */
static UINT32 d2d_fill_aa_position_hash(const D2D1_POINT_2F *p)
{
    UINT32 hx, hy;

    memcpy(&hx, &p->x, sizeof(hx));
    memcpy(&hy, &p->y, sizeof(hy));
    return hx * 2654435761u + hy * 2246822519u;
}

/* Index of a fill vertex at the given position, or ~0 if the point is not one
 * of them — which is the normal case for a bezier control point outside the
 * filled area, since only control points inside it become figure vertices. */
static size_t d2d_fill_aa_find_vertex(const struct d2d_geometry *geometry,
        const UINT32 *vertex_map, size_t map_size, const D2D1_POINT_2F *p)
{
    UINT32 slot = d2d_fill_aa_position_hash(p) % map_size;
    size_t probe = 0;

    while (probe < map_size && vertex_map[slot])
    {
        if (!memcmp(&geometry->fill.vertices[vertex_map[slot] - 1], p, sizeof(*p)))
            return vertex_map[slot] - 1;
        slot = (slot + 1) % map_size;
        ++probe;
    }
    return ~(size_t)0;
}

/* Mark the mesh edges that run along a curve segment.
 *
 * The tessellation puts the corners of every curve triangle on figure vertices,
 * so a mesh edge belongs to a curve exactly when both its endpoints are corners
 * of the same curve triangle: the chord for a control point outside the filled
 * area, and the two control edges for one inside it. Such an edge is the seam
 * between the flat mesh and the curve triangle that is drawn on top of it right
 * afterwards, and that triangle antialiases itself — widening the seam with a
 * skirt would cover the same pixels a second time.
 *
 * Only edges that are actually in the mesh are marked; a chord that ended up as
 * an interior diagonal is simply not found.
 *
 * Returns FALSE if the marking could not be completed. Half-marked data is
 * worse than none — an unmarked seam would get a skirt and be drawn twice — so
 * the caller falls back to the plain fill in that case. */
static BOOL d2d_fill_aa_mark_curve_edges(const struct d2d_geometry *geometry,
        const UINT32 *edge_keys, BYTE *edge_curve, size_t map_size)
{
    const struct d2d_curve_vertex *curves[2];
    size_t counts[2], vertex_map_size, i, c;
    UINT32 *vertex_map;

    curves[0] = geometry->fill.bezier_vertices;
    counts[0] = geometry->fill.bezier_vertex_count;
    curves[1] = geometry->fill.arc_vertices;
    counts[1] = geometry->fill.arc_vertex_count;

    vertex_map_size = geometry->fill.vertex_count * 2 + 16;
    if (!(vertex_map = calloc(vertex_map_size, sizeof(*vertex_map))))
        return FALSE;

    for (i = 0; i < geometry->fill.vertex_count; ++i)
    {
        const D2D1_POINT_2F *p = &geometry->fill.vertices[i];
        UINT32 slot = d2d_fill_aa_position_hash(p) % vertex_map_size;
        size_t probe = 0;

        while (probe < vertex_map_size && vertex_map[slot]
                && memcmp(&geometry->fill.vertices[vertex_map[slot] - 1], p, sizeof(*p)))
        {
            slot = (slot + 1) % vertex_map_size;
            ++probe;
        }
        if (probe == vertex_map_size)
        {
            free(vertex_map);
            return FALSE;
        }
        if (!vertex_map[slot])
            vertex_map[slot] = i + 1;
    }

    for (c = 0; c < 2; ++c)
    {
        for (i = 0; i + 2 < counts[c]; i += 3)
        {
            size_t idx[3], missing = 0;
            int a, b;

            for (a = 0; a < 3; ++a)
            {
                idx[a] = d2d_fill_aa_find_vertex(geometry, vertex_map, vertex_map_size,
                        &curves[c][i + a].position);
                if (idx[a] == ~(size_t)0)
                    ++missing;
            }

            /* Only the control point may be missing, and only when it lies
             * outside the filled area: the two curve end points are figure
             * points by construction, hence fill vertices. Two missing points
             * mean the positions are no longer bit-identical copies of each
             * other, so the seams cannot be identified at all. Fall back rather
             * than mark half of them -- an unmarked seam gets a skirt and is
             * drawn twice, which is the artefact this whole path avoids. */
            if (missing > 1)
            {
                ERR("%Iu of 3 curve triangle corners are not fill vertices, "
                        "the curve seams cannot be identified.\n", missing);
                free(vertex_map);
                return FALSE;
            }

            for (a = 0; a < 3; ++a)
            {
                for (b = a + 1; b < 3; ++b)
                {
                    UINT32 lo, hi, key, slot;
                    size_t probe = 0;

                    if (idx[a] > 0xffff || idx[b] > 0xffff || idx[a] == idx[b])
                        continue;
                    lo = idx[a] < idx[b] ? idx[a] : idx[b];
                    hi = idx[a] < idx[b] ? idx[b] : idx[a];
                    key = lo * 65536u + hi + 1u;
                    slot = key % map_size;
                    while (probe < map_size && edge_keys[slot] && edge_keys[slot] != key)
                    {
                        slot = (slot + 1) % map_size;
                        ++probe;
                    }
                    if (probe == map_size)
                    {
                        free(vertex_map);
                        return FALSE;
                    }
                    if (edge_keys[slot] == key)
                        edge_curve[slot] = 1;
                }
            }
        }
    }

    free(vertex_map);
    return TRUE;
}

/* Build and draw fill-AA geometry. Returns TRUE on success, FALSE if caller should fall back. */
static BOOL d2d_device_context_fill_triangles_aa(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    /* Fill-edge AA: per-face vertices with boundary edge weights + skirt expansion.
     * Inner triangles: bary coords identify boundary edges.
     * Skirt quads: extend boundary edges ~0.75px outward for full AA transition. */
    struct d2d_fill_aa_vertex { D2D1_POINT_2F pos; float u, v, s; float ex, ey; };
    struct d2d_fill_aa_vertex *expanded;
    size_t fc = geometry->fill.face_count;
    size_t boundary_count = 0;
    size_t expanded_count, map_size, i, vi;
    UINT32 *edge_keys;
    BYTE *edge_cnt, *edge_processed, *edge_curve;
    ID3D11Buffer *vb;
    HRESULT hr;

    /* Use 6x multiplier to keep load factor ≤ 50%, preventing open-addressing loops
     * from running unbounded even in worst-case all-boundary-edge meshes. */
    map_size = fc * 6 + 16;
    edge_keys      = calloc(map_size, sizeof(*edge_keys));
    edge_cnt       = calloc(map_size, sizeof(*edge_cnt));
    edge_processed = calloc(map_size, sizeof(*edge_processed));
    edge_curve     = calloc(map_size, sizeof(*edge_curve));
    if (!edge_keys || !edge_cnt || !edge_processed || !edge_curve)
    {
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve);
        return FALSE;
    }

    /* Pass 1: count edge occurrences via open-addressing hash. */
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &geometry->fill.faces[i];
        int e;
        for (e = 0; e < 3; e++)
        {
            UINT16 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
            UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
            UINT32 key = lo * 65536u + hi + 1u;
            UINT32 slot = key % map_size;
            size_t probe = 0;
            while (probe < map_size && edge_keys[slot] && edge_keys[slot] != key)
            {
                slot = (slot + 1) % map_size;
                probe++;
            }
            if (probe == map_size)
            {
                free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve);
                return FALSE;
            }
            edge_keys[slot] = key;
            edge_cnt[slot]++;
        }
    }

    /* A geometry that mixes straight edges with curve segments keeps the
     * antialiasing on its straight edges; the seams towards the curves are left
     * to the curve triangles, which antialias themselves. */
    if ((geometry->fill.bezier_vertex_count || geometry->fill.arc_vertex_count)
            && !d2d_fill_aa_mark_curve_edges(geometry, edge_keys, edge_curve, map_size))
    {
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve);
        return FALSE;
    }

    /* Count the boundary edges that get a skirt, i.e. everything but the seams. */
    for (i = 0; i < map_size; i++)
        if (edge_keys[i] && edge_cnt[i] == 1 && !edge_curve[i]) boundary_count++;

    /* Nothing left to antialias — a shape bounded by curves only, such as an
     * ellipse. The plain path draws exactly the same pixels more cheaply. */
    if (!boundary_count)
    {
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve);
        return FALSE;
    }

    /* Allocate: inner triangles (3 per face) + skirt quads (6 per boundary edge). */
    expanded_count = fc * 3 + boundary_count * 6;
    expanded = malloc(expanded_count * sizeof(*expanded));
    if (!expanded)
    {
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve);
        return FALSE;
    }

    /* Pass 2: build inner triangle vertices with edge weights. */
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &geometry->fill.faces[i];
        int j;
        for (j = 0; j < 3; j++)
        {
            int e;
            vi = i * 3 + j;
            expanded[vi].pos = geometry->fill.vertices[f->v[j]];
            expanded[vi].ex = 0.0f;
            expanded[vi].ey = 0.0f;
            for (e = 0; e < 3; e++)
            {
                UINT16 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
                UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
                UINT32 key = lo * 65536u + hi + 1u;
                UINT32 slot = key % map_size;
                float w;
                while (edge_keys[slot] != key)
                    slot = (slot + 1) % map_size;
                /* Interior edge weight: large value (>> 1.0) ensures it never limits
                 * min_edge_px, so only true boundary edges drive the AA fade.
                 * A seam towards a curve segment counts as interior here: fading
                 * the fill out along it would draw a line into the shape where
                 * the curve triangle continues it. */
                w = (edge_cnt[slot] == 1 && !edge_curve[slot]) ? (j == e ? 1.0f : 0.0f) : 10.0f;
                if (e == 0) expanded[vi].u = w;
                else if (e == 1) expanded[vi].v = w;
                else expanded[vi].s = w;
            }
        }
    }

    /* Pass 3: build skirt quads along boundary edges. */
    vi = fc * 3;
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &geometry->fill.faces[i];
        int e;
        for (e = 0; e < 3; e++)
        {
            UINT16 ai = f->v[(e + 1) % 3], bi = f->v[(e + 2) % 3];
            UINT32 lo = ai < bi ? ai : bi, hi = ai < bi ? bi : ai;
            UINT32 key = lo * 65536u + hi + 1u;
            UINT32 slot = key % map_size;
            D2D1_POINT_2F pa, pb, pc, edge_dir, outward;
            float dot_test, edge_len;

            while (edge_keys[slot] != key)
                slot = (slot + 1) % map_size;
            /* Skip interior edges, curve seams and already-processed boundary edges. */
            if (edge_cnt[slot] != 1 || edge_curve[slot] || edge_processed[slot]) continue;

            /* Compute outward normal (away from opposite vertex). */
            pa = geometry->fill.vertices[ai];
            pb = geometry->fill.vertices[bi];
            pc = geometry->fill.vertices[f->v[e]]; /* opposite vertex */
            edge_dir.x = pb.x - pa.x;
            edge_dir.y = pb.y - pa.y;
            edge_len = sqrtf(edge_dir.x * edge_dir.x + edge_dir.y * edge_dir.y);
            if (edge_len < 0.0001f)
            {
                /* Degenerate edge: mark processed and skip without writing vertices. */
                edge_processed[slot] = 1;
                continue;
            }
            /* Perpendicular: (-dy, dx) */
            outward.x = -edge_dir.y / edge_len;
            outward.y =  edge_dir.x / edge_len;
            /* Ensure it points away from opposite vertex c. */
            dot_test = outward.x * (pa.x - pc.x) + outward.y * (pa.y - pc.y);
            if (dot_test < 0.0f) { outward.x = -outward.x; outward.y = -outward.y; }

            /* Safety: ensure we never write past the allocated buffer. */
            if (vi + 6 > expanded_count) continue;

            /* Skirt quad: 2 triangles (A, B, A_out) + (B, B_out, A_out).
             * Inner verts (A, B): bary=0 for boundary component, ex/ey=0.
             * Outer verts (A_out, B_out): bary=-1 for boundary component, ex/ey=outward. */
            /* Triangle 1: A, B, A_out */
            expanded[vi].pos = pa;
            expanded[vi].u = 0.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = 0.0f; expanded[vi].ey = 0.0f;
            vi++;
            expanded[vi].pos = pb;
            expanded[vi].u = 0.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = 0.0f; expanded[vi].ey = 0.0f;
            vi++;
            expanded[vi].pos = pa;
            expanded[vi].u = -1.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = outward.x; expanded[vi].ey = outward.y;
            vi++;
            /* Triangle 2: B, B_out, A_out */
            expanded[vi].pos = pb;
            expanded[vi].u = 0.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = 0.0f; expanded[vi].ey = 0.0f;
            vi++;
            expanded[vi].pos = pb;
            expanded[vi].u = -1.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = outward.x; expanded[vi].ey = outward.y;
            vi++;
            expanded[vi].pos = pa;
            expanded[vi].u = -1.0f; expanded[vi].v = 10.0f; expanded[vi].s = 10.0f;
            expanded[vi].ex = outward.x; expanded[vi].ey = outward.y;
            vi++;

            /* Mark this edge as processed to avoid a duplicate skirt from the adjacent face. */
            edge_processed[slot] = 1;
        }
    }
    expanded_count = vi; /* actual count (may be less if degenerate edges skipped) */
    free(edge_keys);
    free(edge_cnt);
    free(edge_processed);
    free(edge_curve);

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_VERTEX_BUFFER,
            expanded_count * sizeof(*expanded),
            &render_target->scratch_vb[D2D_SHAPE_TYPE_FILL_AA],
            expanded, &vb)))
    {
        free(expanded);
        return FALSE;
    }
    free(expanded);

    d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_FILL_AA, NULL, expanded_count, vb,
            sizeof(*expanded), brush, opacity_brush);
    return TRUE;
}

static void d2d_device_context_fill_geometry(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    ID3D11Buffer *vb;
    HRESULT hr;

    if (FAILED(hr = d2d_device_context_update_vs_cb(render_target, &geometry->transform, 0.0f, 0.0f)))
    {
        WARN("Failed to update vs constant buffer, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = d2d_device_context_update_ps_cb(render_target, brush, opacity_brush, FALSE, FALSE)))
    {
        WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
        return;
    }

    /* A curve segment anywhere in the figure used to send the whole area fill
     * down the aliased path, straight edges included — a rounded rectangle or a
     * play triangle with rounded corners lost the antialiasing on every one of
     * its flat sides. The mixed case is handled inside fill_triangles_aa now: it
     * antialiases the straight boundary edges and leaves the seams towards the
     * curve segments alone, and it declines a shape bounded by curves only. */
    if (geometry->fill.face_count
            && render_target->drawing_state.antialiasMode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE)
    {
        if (!d2d_device_context_fill_triangles_aa(render_target, geometry, brush, opacity_brush))
            d2d_device_context_fill_triangles(render_target, geometry, brush, opacity_brush);
    }
    else if (geometry->fill.face_count)
    {
        d2d_device_context_fill_triangles(render_target, geometry, brush, opacity_brush);
    }

    if (geometry->fill.bezier_vertex_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                geometry->fill.bezier_vertex_count * sizeof(*geometry->fill.bezier_vertices),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_CURVE],
                geometry->fill.bezier_vertices, &vb)))
        {
            ERR("Failed to create curves vertex buffer, hr %#lx.\n", hr);
            return;
        }

        d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_CURVE, NULL, geometry->fill.bezier_vertex_count, vb,
                sizeof(*geometry->fill.bezier_vertices), brush, opacity_brush);
    }

    if (geometry->fill.arc_vertex_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                geometry->fill.arc_vertex_count * sizeof(*geometry->fill.arc_vertices),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_CURVE],
                geometry->fill.arc_vertices, &vb)))
        {
            ERR("Failed to create arc vertex buffer, hr %#lx.\n", hr);
            return;
        }

        if (SUCCEEDED(d2d_device_context_update_ps_cb(render_target, brush, opacity_brush, FALSE, TRUE)))
            d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_CURVE, NULL, geometry->fill.arc_vertex_count, vb,
                    sizeof(*geometry->fill.arc_vertices), brush, opacity_brush);
    }
}

static void STDMETHODCALLTYPE d2d_device_context_FillGeometry(ID2D1DeviceContext6 *iface,
        ID2D1Geometry *geometry, ID2D1Brush *brush, ID2D1Brush *opacity_brush)
{
    const struct d2d_geometry *geometry_impl = unsafe_impl_from_ID2D1Geometry(geometry);
    struct d2d_brush *opacity_brush_impl = unsafe_impl_from_ID2D1Brush(opacity_brush);
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *brush_impl = unsafe_impl_from_ID2D1Brush(brush);

    TRACE("iface %p, geometry %p, brush %p, opacity_brush %p.\n", iface, geometry, brush, opacity_brush);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (opacity_brush && brush_impl->type != D2D_BRUSH_TYPE_BITMAP)
    {
        d2d_device_context_set_error(context, D2DERR_INCOMPATIBLE_BRUSH_TYPES);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_fill_geometry(context->target.command_list, context, geometry, brush, opacity_brush);
    else
        d2d_device_context_fill_geometry(context, geometry_impl, brush_impl, opacity_brush_impl);
}

static void STDMETHODCALLTYPE d2d_device_context_FillMesh(ID2D1DeviceContext6 *iface,
        ID2D1Mesh *mesh, ID2D1Brush *brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    FIXME("iface %p, mesh %p, brush %p stub!\n", iface, mesh, brush);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_fill_mesh(context->target.command_list, context, mesh, brush);
}

static void STDMETHODCALLTYPE d2d_device_context_FillOpacityMask(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *mask, ID2D1Brush *brush, D2D1_OPACITY_MASK_CONTENT content,
        const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    FIXME("iface %p, mask %p, brush %p, content %#x, dst_rect %s, src_rect %s stub!\n",
            iface, mask, brush, content, debug_d2d_rect_f(dst_rect), debug_d2d_rect_f(src_rect));

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->drawing_state.antialiasMode != D2D1_ANTIALIAS_MODE_ALIASED)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if ((unsigned int)content > D2D1_OPACITY_MASK_CONTENT_TEXT_GDI_COMPATIBLE)
    {
        d2d_device_context_set_error(context, E_INVALIDARG);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_fill_opacity_mask(context->target.command_list, context, mask, brush, dst_rect, src_rect);
}

static void d2d_device_context_draw_bitmap(struct d2d_device_context *context, ID2D1Bitmap *bitmap,
        const D2D1_RECT_F *dst_rect, float opacity, D2D1_INTERPOLATION_MODE interpolation_mode,
        const D2D1_RECT_F *src_rect, const D2D1_POINT_2F *offset,
        const D2D1_MATRIX_4X4_F *perspective_transform)
{
    D2D1_BITMAP_BRUSH_PROPERTIES1 bitmap_brush_desc;
    D2D1_BRUSH_PROPERTIES brush_desc;
    struct d2d_brush *brush;
    D2D1_SIZE_F size;
    D2D1_RECT_F s, d;
    HRESULT hr;

    if (perspective_transform)
        FIXME("Perspective transform is ignored.\n");

    size = ID2D1Bitmap_GetSize(bitmap);
    d2d_rect_set(&s, 0.0f, 0.0f, size.width, size.height);
    if (src_rect && src_rect->left <= src_rect->right
            && src_rect->top <= src_rect->bottom)
    {
        d2d_rect_intersect(&s, src_rect);
    }

    if (s.left == s.right || s.top == s.bottom)
        return;

    if (dst_rect)
    {
        d = *dst_rect;
    }
    else
    {
        d.left = 0.0f;
        d.top = 0.0f;
        d.right = s.right - s.left;
        d.bottom = s.bottom - s.top;
    }

    if (offset)
    {
        d.left += offset->x;
        d.top += offset->y;
        d.right += offset->x;
        d.bottom += offset->y;
    }

    bitmap_brush_desc.extendModeX = D2D1_EXTEND_MODE_CLAMP;
    bitmap_brush_desc.extendModeY = D2D1_EXTEND_MODE_CLAMP;
    bitmap_brush_desc.interpolationMode = interpolation_mode;

    brush_desc.opacity = opacity;
    brush_desc.transform._11 = fabsf((d.right - d.left) / (s.right - s.left));
    brush_desc.transform._21 = 0.0f;
    brush_desc.transform._31 = min(d.left, d.right) - min(s.left, s.right) * brush_desc.transform._11;
    brush_desc.transform._12 = 0.0f;
    brush_desc.transform._22 = fabsf((d.bottom - d.top) / (s.bottom - s.top));
    brush_desc.transform._32 = min(d.top, d.bottom) - min(s.top, s.bottom) * brush_desc.transform._22;

    if (FAILED(hr = d2d_bitmap_brush_create(context->factory, bitmap, &bitmap_brush_desc, &brush_desc, &brush)))
    {
        ERR("Failed to create bitmap brush, hr %#lx.\n", hr);
        return;
    }

    d2d_device_context_FillRectangle(&context->ID2D1DeviceContext6_iface, &d, &brush->ID2D1Brush_iface);
    ID2D1Brush_Release(&brush->ID2D1Brush_iface);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawBitmap(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *bitmap, const D2D1_RECT_F *dst_rect, float opacity,
        D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode, const D2D1_RECT_F *src_rect)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, bitmap %p, dst_rect %s, opacity %.8e, interpolation_mode %#x, src_rect %s.\n",
            iface, bitmap, debug_d2d_rect_f(dst_rect), opacity, interpolation_mode, debug_d2d_rect_f(src_rect));

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (interpolation_mode != D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            && interpolation_mode != D2D1_BITMAP_INTERPOLATION_MODE_LINEAR)
    {
        d2d_device_context_set_error(context, E_INVALIDARG);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_bitmap(context->target.command_list, bitmap, dst_rect, opacity,
                d2d1_1_interp_mode_from_d2d1(interpolation_mode), src_rect, NULL);
    }
    else
    {
        d2d_device_context_draw_bitmap(context, bitmap, dst_rect, opacity,
                d2d1_1_interp_mode_from_d2d1(interpolation_mode), src_rect, NULL, NULL);
    }
}

static void STDMETHODCALLTYPE d2d_device_context_DrawText(ID2D1DeviceContext6 *iface,
        const WCHAR *string, UINT32 string_len, IDWriteTextFormat *text_format, const D2D1_RECT_F *layout_rect,
        ID2D1Brush *brush, D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring_mode)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    IDWriteTextLayout *text_layout;
    IDWriteFactory *dwrite_factory;
    D2D1_POINT_2F origin;
    float width, height;
    HRESULT hr;

    TRACE("iface %p, string %s, string_len %u, text_format %p, layout_rect %s, "
            "brush %p, options %#x, measuring_mode %#x.\n",
            iface, debugstr_wn(string, string_len), string_len, text_format, debug_d2d_rect_f(layout_rect),
            brush, options, measuring_mode);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            &IID_IDWriteFactory, (IUnknown **)&dwrite_factory)))
    {
        ERR("Failed to create dwrite factory, hr %#lx.\n", hr);
        return;
    }

    width = max(0.0f, layout_rect->right - layout_rect->left);
    height = max(0.0f, layout_rect->bottom - layout_rect->top);
    if (measuring_mode == DWRITE_MEASURING_MODE_NATURAL)
        hr = IDWriteFactory_CreateTextLayout(dwrite_factory, string, string_len, text_format,
                width, height, &text_layout);
    else
        hr = IDWriteFactory_CreateGdiCompatibleTextLayout(dwrite_factory, string, string_len, text_format,
                width, height, render_target->desc.dpiX / 96.0f, (DWRITE_MATRIX *)&render_target->drawing_state.transform,
                measuring_mode == DWRITE_MEASURING_MODE_GDI_NATURAL, &text_layout);
    IDWriteFactory_Release(dwrite_factory);
    if (FAILED(hr))
    {
        ERR("Failed to create text layout, hr %#lx.\n", hr);
        return;
    }

    d2d_point_set(&origin, min(layout_rect->left, layout_rect->right), min(layout_rect->top, layout_rect->bottom));
    ID2D1DeviceContext1_DrawTextLayout((ID2D1DeviceContext1 *)iface, origin, text_layout, brush, options);
    IDWriteTextLayout_Release(text_layout);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawTextLayout(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F origin, IDWriteTextLayout *layout, ID2D1Brush *brush, D2D1_DRAW_TEXT_OPTIONS options)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_draw_text_layout_ctx ctx;
    BOOL clipped = FALSE;
    HRESULT hr;

    TRACE("iface %p, origin %s, layout %p, brush %p, options %#x.\n",
            iface, debug_d2d_point_2f(&origin), layout, brush, options);

    d2d_device_context_flush_lines(render_target);

    ctx.brush = brush;
    ctx.options = options;

    /* D2D1_DRAW_TEXT_OPTIONS_CLIP confines the run to the layout box. The box
     * is the layout's maximum extent placed at the drawing origin, which is
     * also what DrawText() derives its layout from, so clipping here covers
     * both entry points. */
    if (options & D2D1_DRAW_TEXT_OPTIONS_CLIP)
    {
        D2D1_RECT_F clip_rect;

        clip_rect.left = origin.x;
        clip_rect.top = origin.y;
        clip_rect.right = origin.x + IDWriteTextLayout_GetMaxWidth(layout);
        clip_rect.bottom = origin.y + IDWriteTextLayout_GetMaxHeight(layout);
        ID2D1DeviceContext1_PushAxisAlignedClip((ID2D1DeviceContext1 *)iface,
                &clip_rect, D2D1_ANTIALIAS_MODE_ALIASED);
        clipped = TRUE;
    }

    if (FAILED(hr = IDWriteTextLayout_Draw(layout,
            &ctx, &render_target->IDWriteTextRenderer_iface, origin.x, origin.y)))
        FIXME("Failed to draw text layout, hr %#lx.\n", hr);

    if (clipped)
        ID2D1DeviceContext1_PopAxisAlignedClip((ID2D1DeviceContext1 *)iface);
}

static D2D1_ANTIALIAS_MODE d2d_device_context_set_aa_mode_from_text_aa_mode(struct d2d_device_context *rt)
{
    D2D1_ANTIALIAS_MODE prev_antialias_mode = rt->drawing_state.antialiasMode;
    /* Force ALIASED for text so shader-based geometry AA does not affect glyph bitmaps.
     * Text has its own alpha texture from DWrite - geometry AA would degrade quality. */
    rt->drawing_state.antialiasMode = D2D1_ANTIALIAS_MODE_ALIASED;
    return prev_antialias_mode;
}

static HRESULT d2d_device_context_get_glyph_run_geometry(struct d2d_device_context *context,
        const DWRITE_GLYPH_RUN *glyph_run, ID2D1PathGeometry **result)
{
    ID2D1PathGeometry *geometry;
    ID2D1GeometrySink *sink;
    HRESULT hr;

    *result = NULL;

    if (FAILED(hr = ID2D1Factory_CreatePathGeometry(context->factory, &geometry)))
        return hr;

    if (FAILED(hr = ID2D1PathGeometry_Open(geometry, &sink)))
    {
        ID2D1PathGeometry_Release(geometry);
        return hr;
    }

    if (FAILED(hr = IDWriteFontFace_GetGlyphRunOutline(glyph_run->fontFace, glyph_run->fontEmSize,
            glyph_run->glyphIndices, glyph_run->glyphAdvances, glyph_run->glyphOffsets, glyph_run->glyphCount,
            glyph_run->isSideways, glyph_run->bidiLevel & 1, (IDWriteGeometrySink *)sink)))
    {
        ERR("Failed to get glyph run outline, hr %#lx.\n", hr);
        ID2D1GeometrySink_Release(sink);
        ID2D1PathGeometry_Release(geometry);
        return hr;
    }

    if (FAILED(hr = ID2D1GeometrySink_Close(sink)))
        ERR("Failed to close geometry sink, hr %#lx.\n", hr);
    ID2D1GeometrySink_Release(sink);

    if (hr == S_OK)
        *result = geometry;
    else
        ID2D1PathGeometry_Release(geometry);

    return hr;
}

static HRESULT d2d_device_context_get_glyph_run_analysis(struct d2d_device_context *context,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        DWRITE_RENDERING_MODE rendering_mode, DWRITE_MEASURING_MODE measuring_mode,
        DWRITE_TEXT_ANTIALIAS_MODE antialias_mode, DWRITE_TEXTURE_TYPE *texture_type,
        IDWriteGlyphRunAnalysis **result)
{
    IDWriteGlyphRunAnalysis *analysis;
    IDWriteFactory2 *dwrite_factory;
    D2D1_MATRIX_3X2_F *transform;
    float scale_x, scale_y;
    DWRITE_MATRIX m;
    HRESULT hr;

    *result = NULL;

    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2,
            (IUnknown **)&dwrite_factory)))
    {
        ERR("Failed to create dwrite factory, hr %#lx.\n", hr);
        return hr;
    }

    transform = &context->drawing_state.transform;

    scale_x = context->desc.dpiX / 96.0f;
    m.m11 = transform->_11 * scale_x;
    m.m21 = transform->_21 * scale_x;
    m.dx  = transform->_31 * scale_x;

    scale_y = context->desc.dpiY / 96.0f;
    m.m12 = transform->_12 * scale_y;
    m.m22 = transform->_22 * scale_y;
    m.dy  = transform->_32 * scale_y;

    hr = IDWriteFactory2_CreateGlyphRunAnalysis(dwrite_factory, glyph_run, &m, rendering_mode,
            measuring_mode, DWRITE_GRID_FIT_MODE_DEFAULT, antialias_mode, baseline_origin.x,
            baseline_origin.y, &analysis);
    IDWriteFactory2_Release(dwrite_factory);
    if (FAILED(hr))
    {
        ERR("Failed to create glyph run analysis, hr %#lx.\n", hr);
        return hr;
    }

    if (rendering_mode == DWRITE_RENDERING_MODE_ALIASED || antialias_mode == DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE)
        *texture_type = DWRITE_TEXTURE_ALIASED_1x1;
    else
        *texture_type = DWRITE_TEXTURE_CLEARTYPE_3x1;

    *result = analysis;

    return S_OK;
}

/* The standard blend, restricted to one subpixel channel. ClearType is only
 * used for opaque targets, so alpha must remain untouched. */
static ID3D11BlendState *d2d_device_context_get_subpixel_blend_state(struct d2d_device_context *context,
        unsigned int channel)
{
    static const UINT8 write_masks[3] =
    {
        D3D11_COLOR_WRITE_ENABLE_RED,
        D3D11_COLOR_WRITE_ENABLE_GREEN,
        D3D11_COLOR_WRITE_ENABLE_BLUE,
    };
    D3D11_BLEND_DESC blend_desc;
    HRESULT hr;

    if (context->subpixel_bs[channel])
        return context->subpixel_bs[channel];

    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.IndependentBlendEnable = FALSE;
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = write_masks[channel];

    if (FAILED(hr = ID3D11Device1_CreateBlendState(context->d3d_device, &blend_desc,
            &context->subpixel_bs[channel])))
    {
        WARN("Failed to create subpixel blend state, hr %#lx.\n", hr);
        context->subpixel_bs[channel] = NULL;
    }

    return context->subpixel_bs[channel];
}

/* The same three channel masks with blending switched off. The linear text
 * path finishes the blend in the pixel shader, so the output merger must write
 * the result through untouched. */
static ID3D11BlendState *d2d_device_context_get_subpixel_copy_blend_state(struct d2d_device_context *context,
        unsigned int channel)
{
    static const UINT8 write_masks[3] =
    {
        D3D11_COLOR_WRITE_ENABLE_RED,
        D3D11_COLOR_WRITE_ENABLE_GREEN,
        D3D11_COLOR_WRITE_ENABLE_BLUE,
    };
    D3D11_BLEND_DESC blend_desc;
    HRESULT hr;

    if (context->subpixel_copy_bs[channel])
        return context->subpixel_copy_bs[channel];

    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = write_masks[channel];

    if (FAILED(hr = ID3D11Device1_CreateBlendState(context->d3d_device, &blend_desc,
            &context->subpixel_copy_bs[channel])))
    {
        WARN("Failed to create subpixel copy blend state, hr %#lx.\n", hr);
        context->subpixel_copy_bs[channel] = NULL;
    }

    return context->subpixel_copy_bs[channel];
}

/* Only plain 8 bit unsigned normalised targets are candidates for the linear
 * text path. An _SRGB target already gets decoded and encoded by the hardware,
 * and a float target is linear to begin with; in both cases reading the copy
 * back through an ordinary view would apply the transfer function a second
 * time. */
static BOOL d2d_device_context_text_dst_format_supported(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return TRUE;
        default:
            return FALSE;
    }
}

/* Take a copy of the destination under the glyph run and set up the mapping
 * from a world position to a texel in it. Returns FALSE when anything is not
 * available, which leaves the caller on the ordinary blend — this path may
 * never be worse than the one it replaces. */
static BOOL d2d_device_context_capture_text_dst(struct d2d_device_context *context,
        const RECT *bounds, unsigned int width, unsigned int height)
{
    unsigned int src_left, src_top, src_right, src_bottom;
    ID3D11DeviceContext *d3d_context;
    D3D11_TEXTURE2D_DESC texture_desc;
    ID3D11Texture2D *target_texture;
    D3D11_BOX box;
    float scale_x, scale_y;
    DXGI_FORMAT format;
    HRESULT hr;

    if (context->target.type != D2D_TARGET_BITMAP || !context->target.bitmap)
        return FALSE;

    format = context->target.bitmap->format.format;
    if (!d2d_device_context_text_dst_format_supported(format))
        return FALSE;

    if (FAILED(ID3D11Resource_QueryInterface(context->target.bitmap->resource,
            &IID_ID3D11Texture2D, (void **)&target_texture)))
        return FALSE;

    ID3D11Texture2D_GetDesc(target_texture, &texture_desc);
    /* A multisampled target cannot be the source of a region copy. */
    if (texture_desc.SampleDesc.Count > 1)
    {
        ID3D11Texture2D_Release(target_texture);
        return FALSE;
    }

    if (context->text_dst && (context->text_dst_width < width
            || context->text_dst_height < height || context->text_dst_format != format))
    {
        ID3D11ShaderResourceView_Release(context->text_dst_srv);
        ID3D11Texture2D_Release(context->text_dst);
        context->text_dst_srv = NULL;
        context->text_dst = NULL;
    }

    if (!context->text_dst)
    {
        D3D11_TEXTURE2D_DESC desc;

        memset(&desc, 0, sizeof(desc));
        desc.Width = max(width, context->text_dst_width);
        desc.Height = max(height, context->text_dst_height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(hr = ID3D11Device1_CreateTexture2D(context->d3d_device, &desc, NULL,
                &context->text_dst)))
        {
            WARN("Failed to create text destination copy, hr %#lx.\n", hr);
            ID3D11Texture2D_Release(target_texture);
            return FALSE;
        }

        if (FAILED(hr = ID3D11Device1_CreateShaderResourceView(context->d3d_device,
                (ID3D11Resource *)context->text_dst, NULL, &context->text_dst_srv)))
        {
            WARN("Failed to create text destination view, hr %#lx.\n", hr);
            ID3D11Texture2D_Release(context->text_dst);
            context->text_dst = NULL;
            ID3D11Texture2D_Release(target_texture);
            return FALSE;
        }

        context->text_dst_width = desc.Width;
        context->text_dst_height = desc.Height;
        context->text_dst_format = format;
    }

    if (!context->text_dst_sampler)
    {
        D3D11_SAMPLER_DESC sampler_desc;

        /* Point sampling: the copy is pixel aligned with the target, and any
         * filtering would smear a neighbouring pixel's colour into the blend. */
        memset(&sampler_desc, 0, sizeof(sampler_desc));
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

        if (FAILED(hr = ID3D11Device1_CreateSamplerState(context->d3d_device, &sampler_desc,
                &context->text_dst_sampler)))
        {
            WARN("Failed to create text destination sampler, hr %#lx.\n", hr);
            ID3D11Texture2D_Release(target_texture);
            return FALSE;
        }
    }

    /* Clip the copied region to the target. What falls outside is never
     * sampled: the scissor rect is clamped to the target as well. */
    src_left = max(bounds->left, 0);
    src_top = max(bounds->top, 0);
    src_right = min(bounds->right, (LONG)texture_desc.Width);
    src_bottom = min(bounds->bottom, (LONG)texture_desc.Height);

    if (src_left >= src_right || src_top >= src_bottom)
    {
        ID3D11Texture2D_Release(target_texture);
        return FALSE;
    }

    box.left = src_left;
    box.top = src_top;
    box.front = 0;
    box.right = src_right;
    box.bottom = src_bottom;
    box.back = 1;

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
    ID3D11DeviceContext_CopySubresourceRegion(d3d_context, (ID3D11Resource *)context->text_dst, 0,
            src_left - bounds->left, src_top - bounds->top, 0,
            (ID3D11Resource *)target_texture, 0, &box);
    ID3D11DeviceContext_Release(d3d_context);
    ID3D11Texture2D_Release(target_texture);

    /* uv = (world position * scale - bounds origin) / copy size. */
    scale_x = context->desc.dpiX / 96.0f;
    scale_y = context->desc.dpiY / 96.0f;
    context->text_dst_scale_x = scale_x / context->text_dst_width;
    context->text_dst_offset_x = -(float)bounds->left / context->text_dst_width;
    context->text_dst_scale_y = scale_y / context->text_dst_height;
    context->text_dst_offset_y = -(float)bounds->top / context->text_dst_height;

    return TRUE;
}

/* Draw the run once per subpixel channel, each pass masked by that channel's
 * coverage and writing only that channel. Build all resources before drawing
 * so an allocation failure cannot leave a partially coloured run behind. */
/* Enhanced contrast, as DirectWrite reports it through GetAlphaBlendParams().
 * Raises partial coverage while leaving both endpoints alone, so a fully covered
 * or fully empty sample never moves and the mapping stays monotonic in coverage.
 * At contrast 0 this is the identity, which is what Wine's dwrite reports today,
 * so the default path is bit-identical to having no correction at all.
 *
 * This is an approximation: the exact curve Windows applies is not documented.
 * It deliberately does not touch gamma — blending linearly is a separate and
 * much larger change, see the linear-blend issue. */
static float d2d_apply_enhanced_contrast(float coverage, float contrast)
{
    float a;

    if (!(contrast > 0.0f))
        return coverage;

    a = coverage / 255.0f;
    a += contrast * a * (1.0f - a);

    return a * 255.0f;
}

/* One coverage mask for all the glyph runs a device context draws.
 *
 * A run used to get a texture of its own: a D3D texture with an upload, a
 * shader resource view, the bitmap and bitmap brush wrapping them and a
 * rectangle geometry, all created and destroyed again for every line of text
 * on every repaint. Text-heavy windows spent a large part of each repaint on
 * that churn. The runs of a context share one mask instead: an A8 texture
 * grown in steps and kept for the lifetime of the context, together with the
 * objects wrapping it and the CPU buffers the coverage passes through. A run
 * uploads its coverage into the top-left corner of the texture and draws
 * before the next run touches it, so nothing is ever shared between two runs
 * in flight. The mask is claimed and returned around a single run the way the
 * rectangle geometry is; a second claimant while it is out builds its own. */

#define D2D_GLYPH_MASK_MIN_WIDTH   256
#define D2D_GLYPH_MASK_MIN_HEIGHT   64
#define D2D_GLYPH_MASK_MAX_WIDTH  4096
#define D2D_GLYPH_MASK_MAX_HEIGHT 1024

static struct d2d_glyph_mask *d2d_device_context_get_glyph_mask(struct d2d_device_context *context)
{
    struct d2d_glyph_mask *mask;

    if ((mask = InterlockedExchangePointer((void **)&context->glyph_mask_cache, NULL)))
        return mask;

    return calloc(1, sizeof(*mask));
}

static void d2d_device_context_put_glyph_mask(struct d2d_device_context *context,
        struct d2d_glyph_mask *mask)
{
    struct d2d_glyph_mask *displaced;

    if ((displaced = InterlockedExchangePointer((void **)&context->glyph_mask_cache, mask)))
        d2d_glyph_mask_destroy(displaced);
}

static BYTE *d2d_glyph_mask_reserve(BYTE **buffer, size_t *size, size_t needed)
{
    if (needed <= *size)
        return *buffer;

    /* Nothing in the old buffer is worth carrying over. */
    free(*buffer);
    *size = 0;
    if (!(*buffer = malloc(needed)))
        return NULL;
    *size = needed;

    return *buffer;
}

/* The plane a run is uploaded from: width x height bytes, rows pitch bytes
 * apart, with a spare column and row for d2d_glyph_mask_upload(). */
static BYTE *d2d_glyph_mask_get_plane(struct d2d_glyph_mask *mask, unsigned int width,
        unsigned int height, unsigned int *pitch)
{
    *pitch = width + 1;
    if ((size_t)height + 1 > ~(size_t)0 / *pitch)
        return NULL;

    return d2d_glyph_mask_reserve(&mask->data, &mask->data_size, (size_t)*pitch * (height + 1));
}

/* Hand the plane to the shader as an opacity brush placed at run_rect. The
 * brush is the shared one unless the run does not fit the shared texture, in
 * which case it is a one-off object the caller has to release. */
static HRESULT d2d_glyph_mask_upload(struct d2d_device_context *context, struct d2d_glyph_mask *mask,
        unsigned int width, unsigned int height, const D2D1_RECT_F *run_rect,
        struct d2d_brush **brush, BOOL *temporary)
{
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    D2D1_BRUSH_PROPERTIES brush_desc;
    ID3D11DeviceContext *d3d_context;
    unsigned int pitch = width + 1;
    struct d2d_bitmap *bitmap;
    D2D1_SIZE_U size;
    unsigned int y;
    D3D11_BOX box;
    HRESULT hr;

    bitmap_desc.pixelFormat.format = DXGI_FORMAT_A8_UNORM;
    bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmap_desc.dpiX = context->desc.dpiX;
    bitmap_desc.dpiY = context->desc.dpiY;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    bitmap_desc.colorContext = NULL;

    brush_desc.opacity = 1.0f;
    brush_desc.transform = identity;
    brush_desc.transform._31 = run_rect->left;
    brush_desc.transform._32 = run_rect->top;

    *temporary = FALSE;

    /* A run the shared texture cannot hold gets a texture of its own, the
     * way every run used to. */
    if (width >= D2D_GLYPH_MASK_MAX_WIDTH || height >= D2D_GLYPH_MASK_MAX_HEIGHT)
    {
        d2d_size_set(&size, width, height);
        if (FAILED(hr = d2d_bitmap_create(context, size, mask->data, pitch, &bitmap_desc, &bitmap)))
            return hr;
        hr = d2d_bitmap_brush_create(context->factory, (ID2D1Bitmap *)&bitmap->ID2D1Bitmap1_iface,
                NULL, &brush_desc, brush);
        ID2D1Bitmap1_Release(&bitmap->ID2D1Bitmap1_iface);
        *temporary = TRUE;
        return hr;
    }

    /* The brush clamps at the edge of its texture, and the antialiased edge
     * of the run rectangle reaches half a pixel past the run. A texture the
     * run has to itself clamps at the run; the shared one has to reproduce
     * that, so the last column and row are replicated once more beyond the
     * run and those samples stay what they were. */
    for (y = 0; y < height; ++y)
        mask->data[y * pitch + width] = mask->data[y * pitch + width - 1];
    memcpy(&mask->data[height * pitch], &mask->data[(height - 1) * pitch], pitch);

    if (mask->bitmap && (mask->bitmap->pixel_size.width < pitch
            || mask->bitmap->pixel_size.height < height + 1))
    {
        size.width = min(max(2 * mask->bitmap->pixel_size.width, pitch), D2D_GLYPH_MASK_MAX_WIDTH);
        size.height = min(max(2 * mask->bitmap->pixel_size.height, height + 1), D2D_GLYPH_MASK_MAX_HEIGHT);
        ID2D1Brush_Release(&mask->brush->ID2D1Brush_iface);
        mask->brush = NULL;
        mask->bitmap = NULL;
    }
    else if (!mask->bitmap)
    {
        size.width = max(pitch, D2D_GLYPH_MASK_MIN_WIDTH);
        size.height = max(height + 1, D2D_GLYPH_MASK_MIN_HEIGHT);
    }

    if (!mask->bitmap)
    {
        if (FAILED(hr = d2d_bitmap_create(context, size, NULL, 0, &bitmap_desc, &bitmap)))
            return hr;
        hr = d2d_bitmap_brush_create(context->factory, (ID2D1Bitmap *)&bitmap->ID2D1Bitmap1_iface,
                NULL, &brush_desc, &mask->brush);
        ID2D1Bitmap1_Release(&bitmap->ID2D1Bitmap1_iface);
        if (FAILED(hr))
            return hr;
        mask->bitmap = bitmap;
    }

    /* The brush maps its bitmap through the bitmap's size in DIPs, so the
     * bitmap has to follow the context's DPI; the transform places it at the
     * run. */
    mask->bitmap->dpi_x = context->desc.dpiX;
    mask->bitmap->dpi_y = context->desc.dpiY;
    mask->brush->transform = brush_desc.transform;

    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = pitch;
    box.bottom = height + 1;
    box.back = 1;
    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
    ID3D11DeviceContext_UpdateSubresource(d3d_context, mask->bitmap->resource, 0, &box, mask->data, pitch, 0);
    ID3D11DeviceContext_Release(d3d_context);

    *brush = mask->brush;

    return S_OK;
}

/* Fill run_rect with the brush, masked by the run's coverage. The caller has
 * set the identity transform: the glyph run analysis already applied it. */
static void d2d_device_context_fill_glyph_rect(struct d2d_device_context *context,
        const D2D1_RECT_F *run_rect, struct d2d_brush *brush, struct d2d_brush *mask_brush)
{
    struct d2d_geometry *geometry;

    if (!(geometry = d2d_device_context_get_rect_geometry(context, run_rect)))
        return;
    d2d_device_context_fill_geometry(context, geometry, brush, mask_brush);
    d2d_device_context_put_rect_geometry(context, geometry);
}

static HRESULT d2d_device_context_draw_glyph_run_subpixel(struct d2d_device_context *context,
        struct d2d_glyph_mask *mask, ID2D1Brush *brush, unsigned int width, unsigned int height,
        const RECT *bounds, DWRITE_PIXEL_GEOMETRY pixel_geometry, float cleartype_level,
        float enhanced_contrast)
{
    D2D1_ANTIALIAS_MODE antialias_mode = context->drawing_state.antialiasMode;
    struct d2d_brush *brush_impl = unsafe_impl_from_ID2D1Brush(brush);
    ID3D11BlendState *prev_bs = context->bs;
    ID3D11BlendState *blend_states[3];
    const BYTE *coverage = mask->coverage;
    D2D1_MATRIX_3X2_F *transform, m;
    struct d2d_brush *mask_brush;
    unsigned int c, x, y, pitch;
    float scale_x, scale_y;
    D2D1_RECT_F run_rect;
    BOOL linear, temporary;
    HRESULT hr = S_OK;
    BYTE *plane;

    if (!width || !height)
        return S_OK;

    if (!(plane = d2d_glyph_mask_get_plane(mask, width, height, &pitch)))
        return E_OUTOFMEMORY;

    scale_x = context->desc.dpiX / 96.0f;
    scale_y = context->desc.dpiY / 96.0f;
    d2d_rect_set(&run_rect, bounds->left / scale_x, bounds->top / scale_y,
            bounds->right / scale_x, bounds->bottom / scale_y);

    /* Linear blending needs the destination, which the output merger cannot
     * supply through a transfer function. Take a copy of it and let the shader
     * finish the blend; if that is not available, stay on the ordinary path. */
    linear = d2d_settings.text_linear_blend
            && d2d_device_context_capture_text_dst(context, bounds, width, height);

    for (c = 0; c < 3; ++c)
    {
        blend_states[c] = linear ? d2d_device_context_get_subpixel_copy_blend_state(context, c)
                : d2d_device_context_get_subpixel_blend_state(context, c);
        if (!blend_states[c])
            return E_OUTOFMEMORY;
    }

    transform = &context->drawing_state.transform;
    m = *transform;
    *transform = identity;
    context->linear_text = linear;

    /* The run rectangle is pixel aligned, so antialiasing its edges gains
     * nothing — but it costs correctness here: the antialiased fill path
     * covers the border pixels with partial coverage and scales the shader
     * result by it. That is harmless while the output merger blends, and
     * wrong once the shader produces the finished pixel, which is exactly
     * what the linear path does. Draw the rectangle aliased instead. */
    if (linear)
        context->drawing_state.antialiasMode = D2D1_ANTIALIAS_MODE_ALIASED;

    for (c = 0; c < 3; ++c)
    {
        unsigned int sample = pixel_geometry == DWRITE_PIXEL_GEOMETRY_BGR ? 2 - c : c;

        for (y = 0; y < height; ++y)
            for (x = 0; x < width; ++x)
            {
                const BYTE *pixel = &coverage[((size_t)y * width + x) * 3];
                int gray = (pixel[0] + pixel[1] + pixel[2] + 1) / 3;
                float value = gray + cleartype_level * (pixel[sample] - gray);

                value = d2d_apply_enhanced_contrast(value, enhanced_contrast);

                plane[y * pitch + x] = min(max((int)(value + 0.5f), 0), 255);
            }

        if (FAILED(hr = d2d_glyph_mask_upload(context, mask, width, height, &run_rect, &mask_brush, &temporary)))
        {
            ERR("Failed to upload glyph mask, hr %#lx.\n", hr);
            break;
        }

        context->bs = blend_states[c];
        d2d_device_context_fill_glyph_rect(context, &run_rect, brush_impl, mask_brush);
        if (temporary)
            ID2D1Brush_Release(&mask_brush->ID2D1Brush_iface);
    }

    context->drawing_state.antialiasMode = antialias_mode;
    context->linear_text = FALSE;
    context->bs = prev_bs;
    *transform = m;

    return hr;
}

static void d2d_device_context_draw_glyph_run_bitmap(struct d2d_device_context *context,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run, ID2D1Brush *brush,
        DWRITE_RENDERING_MODE rendering_mode, DWRITE_MEASURING_MODE measuring_mode,
        DWRITE_TEXT_ANTIALIAS_MODE antialias_mode, IDWriteRenderingParams *rendering_params)
{
    unsigned int width, height, pitch, y;
    struct d2d_glyph_mask *mask = NULL;
    IDWriteGlyphRunAnalysis *analysis;
    DWRITE_TEXTURE_TYPE texture_type;
    D2D1_MATRIX_3X2_F *transform, m;
    struct d2d_brush *mask_brush;
    float scale_x, scale_y;
    size_t coverage_size;
    D2D1_RECT_F run_rect;
    BOOL temporary;
    BYTE *plane;
    RECT bounds;
    HRESULT hr;

    hr = d2d_device_context_get_glyph_run_analysis(context, baseline_origin, glyph_run,
            rendering_mode, measuring_mode, antialias_mode, &texture_type, &analysis);
    if (FAILED(hr))
    {
        ERR("Failed to create glyph run analysis, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, texture_type, &bounds)))
    {
        ERR("Failed to get alpha texture bounds, hr %#lx.\n", hr);
        goto done;
    }

    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    if (!width || !height)
    {
        /* Empty run, nothing to do. */
        goto done;
    }

    if (!(mask = d2d_device_context_get_glyph_mask(context)))
    {
        ERR("Failed to allocate glyph mask.\n");
        goto done;
    }

    coverage_size = texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
    if (height > ~(size_t)0 / width / coverage_size)
        goto done;
    coverage_size *= (size_t)width * height;
    if (!d2d_glyph_mask_reserve(&mask->coverage, &mask->coverage_size, coverage_size))
    {
        ERR("Failed to allocate opacity values.\n");
        goto done;
    }

    /* dwrite clears the buffer before it writes the run. */
    if (FAILED(hr = IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis,
            texture_type, &bounds, mask->coverage, coverage_size)))
    {
        ERR("Failed to create alpha texture, hr %#lx.\n", hr);
        goto done;
    }

    /* A ClearType texture carries one coverage sample per subpixel, so the
     * three channels have to reach the target separately. Sampling them as a
     * single alpha mask three times as wide averages them back into one
     * value, which is the greyscale result the mask was meant to improve on.
     * Draw the run once per channel instead, each pass writing only its own
     * colour channel and masked by that channel's coverage. The blend is
     * unchanged, so each channel gets the same premultiplied-over maths it
     * would have had on its own. */
    if (texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1)
    {
        DWRITE_PIXEL_GEOMETRY pixel_geometry = IDWriteRenderingParams_GetPixelGeometry(rendering_params);
        float gamma, contrast, cleartype_level;

        if (FAILED(hr = IDWriteGlyphRunAnalysis_GetAlphaBlendParams(analysis, rendering_params,
                &gamma, &contrast, &cleartype_level)))
        {
            WARN("Failed to get ClearType blend parameters, hr %#lx.\n", hr);
            goto done;
        }
        if (!(cleartype_level >= 0.0f)) cleartype_level = 0.0f;
        else if (cleartype_level > 1.0f) cleartype_level = 1.0f;

        /* Wine's dwrite hardcodes enhanced contrast to zero, so honouring the
         * reported value changes nothing on its own. The registry key exists to
         * try other values without rebuilding; unset keeps DirectWrite's. */
        if (d2d_settings.text_enhanced_contrast_set)
            contrast = d2d_settings.text_enhanced_contrast / 100.0f;
        if (!(contrast >= 0.0f)) contrast = 0.0f;
        else if (contrast > 1.0f) contrast = 1.0f;

        TRACE("ClearType blend parameters: gamma %.3f, contrast %.3f, level %.3f, geometry %u.\n",
                gamma, contrast, cleartype_level, pixel_geometry);
        hr = d2d_device_context_draw_glyph_run_subpixel(context, mask, brush,
                width, height, &bounds, pixel_geometry, cleartype_level, contrast);
        if (FAILED(hr))
            d2d_device_context_set_error(context, hr);
        goto done;
    }

    if (!(plane = d2d_glyph_mask_get_plane(mask, width, height, &pitch)))
    {
        ERR("Failed to allocate glyph mask plane.\n");
        goto done;
    }
    for (y = 0; y < height; ++y)
        memcpy(&plane[y * pitch], &mask->coverage[(size_t)y * width], width);

    scale_x = context->desc.dpiX / 96.0f;
    scale_y = context->desc.dpiY / 96.0f;
    d2d_rect_set(&run_rect, bounds.left / scale_x, bounds.top / scale_y,
            bounds.right / scale_x, bounds.bottom / scale_y);

    if (FAILED(hr = d2d_glyph_mask_upload(context, mask, width, height, &run_rect, &mask_brush, &temporary)))
    {
        ERR("Failed to upload glyph mask, hr %#lx.\n", hr);
        goto done;
    }

    transform = &context->drawing_state.transform;
    m = *transform;
    *transform = identity;
    d2d_device_context_fill_glyph_rect(context, &run_rect, unsafe_impl_from_ID2D1Brush(brush), mask_brush);
    *transform = m;
    if (temporary)
        ID2D1Brush_Release(&mask_brush->ID2D1Brush_iface);

done:
    if (mask)
        d2d_device_context_put_glyph_mask(context, mask);
    IDWriteGlyphRunAnalysis_Release(analysis);
}

static BOOL d2d_device_context_can_draw_cleartype(const struct d2d_device_context *context,
        IDWriteRenderingParams *rendering_params)
{
    return context->target.type == D2D_TARGET_BITMAP
            && context->target.bitmap->format.alphaMode == D2D1_ALPHA_MODE_IGNORE
            && context->drawing_state.primitiveBlend == D2D1_PRIMITIVE_BLEND_SOURCE_OVER
            && IDWriteRenderingParams_GetPixelGeometry(rendering_params) != DWRITE_PIXEL_GEOMETRY_FLAT;
}

static HRESULT d2d_device_context_get_text_rendering_mode(struct d2d_device_context *context,
        const DWRITE_GLYPH_RUN *glyph_run, DWRITE_MEASURING_MODE measuring_mode,
        DWRITE_TEXT_ANTIALIAS_MODE *antialias_mode, DWRITE_RENDERING_MODE *rendering_mode,
        IDWriteRenderingParams **selected_rendering_params)
{
    IDWriteRenderingParams *rendering_params;
    HRESULT hr = S_OK;

    rendering_params = context->text_rendering_params ? context->text_rendering_params
            : context->default_text_rendering_params;
    if (selected_rendering_params) *selected_rendering_params = rendering_params;

    *antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    *rendering_mode = IDWriteRenderingParams_GetRenderingMode(rendering_params);

    switch (context->drawing_state.textAntialiasMode)
    {
        case D2D1_TEXT_ANTIALIAS_MODE_ALIASED:
            if (*rendering_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL
                    || *rendering_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC
                    || *rendering_mode == DWRITE_RENDERING_MODE_CLEARTYPE_GDI_NATURAL
                    || *rendering_mode == DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC)
            {
                return E_INVALIDARG;
            }
            break;

        case D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE:
            if (*rendering_mode == DWRITE_RENDERING_MODE_ALIASED
                    || *rendering_mode == DWRITE_RENDERING_MODE_OUTLINE)
            {
                return E_INVALIDARG;
            }
            break;

        case D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE:
            if (*rendering_mode == DWRITE_RENDERING_MODE_ALIASED)
                return E_INVALIDARG;
            break;

        default:
            break;
    }

    *rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
    switch (context->drawing_state.textAntialiasMode)
    {
        case D2D1_TEXT_ANTIALIAS_MODE_DEFAULT:
            if (IDWriteRenderingParams_GetClearTypeLevel(rendering_params) > 0.0f
                    && d2d_device_context_can_draw_cleartype(context, rendering_params))
                *antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
            break;

        case D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE:
            if (d2d_device_context_can_draw_cleartype(context, rendering_params))
                *antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
            break;

        case D2D1_TEXT_ANTIALIAS_MODE_ALIASED:
            *rendering_mode = DWRITE_RENDERING_MODE_ALIASED;
            break;

        default:
            break;
    }

    if (*rendering_mode == DWRITE_RENDERING_MODE_DEFAULT)
    {
        if (FAILED(hr = IDWriteFontFace_GetRecommendedRenderingMode(glyph_run->fontFace, glyph_run->fontEmSize,
                max(context->desc.dpiX, context->desc.dpiY) / 96.0f,
                measuring_mode, rendering_params, rendering_mode)))
        {
            ERR("Failed to get recommended rendering mode, hr %#lx.\n", hr);
            *rendering_mode = DWRITE_RENDERING_MODE_OUTLINE;
        }
    }

    return hr;
}

static void d2d_device_context_draw_glyph_run(struct d2d_device_context *context,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION *glyph_run_desc, ID2D1Brush *brush, DWRITE_MEASURING_MODE measuring_mode)
{
    DWRITE_TEXT_ANTIALIAS_MODE antialias_mode;
    DWRITE_RENDERING_MODE rendering_mode;
    IDWriteRenderingParams *rendering_params;
    HRESULT hr;

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_glyph_run(context->target.command_list, context, baseline_origin, glyph_run,
                glyph_run_desc, brush, measuring_mode);
        return;
    }

    if (FAILED(hr = d2d_device_context_get_text_rendering_mode(context, glyph_run, measuring_mode,
            &antialias_mode, &rendering_mode, &rendering_params)))
    {
        d2d_device_context_set_error(context, hr);
        return;
    }

    /* Force NATURAL rendering mode for better font quality with FreeType.
     * ALIASED and OUTLINE modes produce poor results with Wine's FreeType
     * backend. This forcing is intentional; because it leaves OUTLINE
     * unreachable here the outline draw path (and its helper) was removed as
     * dead code — all glyph runs go through the bitmap path below. */
    if (rendering_mode == DWRITE_RENDERING_MODE_ALIASED ||
        rendering_mode == DWRITE_RENDERING_MODE_OUTLINE)
    {
        rendering_mode = DWRITE_RENDERING_MODE_NATURAL;
    }

    d2d_device_context_draw_glyph_run_bitmap(context, baseline_origin, glyph_run, brush,
            rendering_mode, measuring_mode, antialias_mode, rendering_params);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawGlyphRun(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run, ID2D1Brush *brush,
        DWRITE_MEASURING_MODE measuring_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, baseline_origin %s, glyph_run %p, brush %p, measuring_mode %#x.\n",
            iface, debug_d2d_point_2f(&baseline_origin), glyph_run, brush, measuring_mode);

    d2d_device_context_flush_lines(context);

    d2d_device_context_draw_glyph_run(context, baseline_origin, glyph_run, NULL, brush, measuring_mode);
}

static void STDMETHODCALLTYPE d2d_device_context_SetTransform(ID2D1DeviceContext6 *iface,
        const D2D1_MATRIX_3X2_F *transform)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, transform %p.\n", iface, transform);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_transform(context->target.command_list, transform);

    context->drawing_state.transform = *transform;
}

static void STDMETHODCALLTYPE d2d_device_context_GetTransform(ID2D1DeviceContext6 *iface,
        D2D1_MATRIX_3X2_F *transform)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, transform %p.\n", iface, transform);

    *transform = render_target->drawing_state.transform;
}

static void STDMETHODCALLTYPE d2d_device_context_SetAntialiasMode(ID2D1DeviceContext6 *iface,
        D2D1_ANTIALIAS_MODE antialias_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, antialias_mode %#x.\n", iface, antialias_mode);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_antialias_mode(context->target.command_list, antialias_mode);

    context->drawing_state.antialiasMode = antialias_mode;
}

static D2D1_ANTIALIAS_MODE STDMETHODCALLTYPE d2d_device_context_GetAntialiasMode(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return render_target->drawing_state.antialiasMode;
}

static void STDMETHODCALLTYPE d2d_device_context_SetTextAntialiasMode(ID2D1DeviceContext6 *iface,
        D2D1_TEXT_ANTIALIAS_MODE antialias_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, antialias_mode %#x.\n", iface, antialias_mode);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_text_antialias_mode(context->target.command_list, antialias_mode);

    context->drawing_state.textAntialiasMode = antialias_mode;
}

static D2D1_TEXT_ANTIALIAS_MODE STDMETHODCALLTYPE d2d_device_context_GetTextAntialiasMode(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return render_target->drawing_state.textAntialiasMode;
}

static void STDMETHODCALLTYPE d2d_device_context_SetTextRenderingParams(ID2D1DeviceContext6 *iface,
        IDWriteRenderingParams *text_rendering_params)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, text_rendering_params %p.\n", iface, text_rendering_params);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_text_rendering_params(context->target.command_list, text_rendering_params);

    if (text_rendering_params)
        IDWriteRenderingParams_AddRef(text_rendering_params);
    if (context->text_rendering_params)
        IDWriteRenderingParams_Release(context->text_rendering_params);
    context->text_rendering_params = text_rendering_params;
}

static void STDMETHODCALLTYPE d2d_device_context_GetTextRenderingParams(ID2D1DeviceContext6 *iface,
        IDWriteRenderingParams **text_rendering_params)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, text_rendering_params %p.\n", iface, text_rendering_params);

    if ((*text_rendering_params = render_target->text_rendering_params))
        IDWriteRenderingParams_AddRef(*text_rendering_params);
}

static void STDMETHODCALLTYPE d2d_device_context_SetTags(ID2D1DeviceContext6 *iface, D2D1_TAG tag1, D2D1_TAG tag2)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, tag1 %s, tag2 %s.\n", iface, wine_dbgstr_longlong(tag1), wine_dbgstr_longlong(tag2));

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_tags(context->target.command_list, tag1, tag2);

    context->drawing_state.tag1 = tag1;
    context->drawing_state.tag2 = tag2;
}

static void STDMETHODCALLTYPE d2d_device_context_GetTags(ID2D1DeviceContext6 *iface, D2D1_TAG *tag1, D2D1_TAG *tag2)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, tag1 %p, tag2 %p.\n", iface, tag1, tag2);

    *tag1 = render_target->drawing_state.tag1;
    *tag2 = render_target->drawing_state.tag2;
}

static void STDMETHODCALLTYPE d2d_device_context_PushLayer(ID2D1DeviceContext6 *iface,
        const D2D1_LAYER_PARAMETERS *layer_parameters, ID2D1Layer *layer)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, layer_parameters %p, layer %p.\n", iface, layer_parameters, layer);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        D2D1_LAYER_PARAMETERS1 parameters;

        memcpy(&parameters, layer_parameters, sizeof(*layer_parameters));
        parameters.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
        d2d_command_list_push_layer(context->target.command_list, context, &parameters);
    }

    if (context->target.type == D2D_TARGET_BITMAP)
    {
        D2D1_BITMAP_PROPERTIES1 bitmap_desc;
        D2D1_RECT_F transformed_rect;
        struct d2d_layer_info info;
        struct d2d_bitmap *layer_bitmap;
        float x_scale, y_scale;
        D2D1_POINT_2F point;
        HRESULT hr;

        if (layer_parameters->geometricMask)
        {
            static int once;
            if (!once++)
                TRACE("Geometric mask in old PushLayer (mask=%p, maskAA=%u).\n",
                        layer_parameters->geometricMask, layer_parameters->maskAntialiasMode);
        }

        /* Transform contentBounds to device coordinates for scissor clip. */
        x_scale = context->desc.dpiX / 96.0f;
        y_scale = context->desc.dpiY / 96.0f;
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.left * x_scale,
                layer_parameters->contentBounds.top * y_scale);
        d2d_rect_set(&transformed_rect, point.x, point.y, point.x, point.y);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.left * x_scale,
                layer_parameters->contentBounds.bottom * y_scale);
        d2d_rect_expand(&transformed_rect, &point);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.right * x_scale,
                layer_parameters->contentBounds.top * y_scale);
        d2d_rect_expand(&transformed_rect, &point);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.right * x_scale,
                layer_parameters->contentBounds.bottom * y_scale);
        d2d_rect_expand(&transformed_rect, &point);

        if (!d2d_clip_stack_push(&context->clip_stack, &transformed_rect))
            WARN("Failed to push clip rect.\n");

        /* Clip-only bypass: if this context has never used element-layer rendering
         * (PushLayer with mask=NULL), and this is a clip-only layer (mask!=NULL,
         * opacity=1.0, no opacityBrush), render directly on the backbuffer using
         * the mask geometry BBox as an additional scissor clip. This avoids the
         * temporary layer_bitmap, so Clear() and drawPopupMenuBackground both go
         * directly to the backbuffer — fixing the hover persistence + black bg. */
        if (layer_parameters->geometricMask && !context->has_element_layers
                && layer_parameters->opacity >= 1.0f && !layer_parameters->opacityBrush
                && layer_parameters->maskAntialiasMode != D2D1_ANTIALIAS_MODE_PER_PRIMITIVE)
        {
            D2D1_RECT_F mask_clip;

            ID2D1Geometry_GetBounds(layer_parameters->geometricMask, NULL, &mask_clip);

            /* Push the mask BBox as an additional scissor clip (coarse optimization). */
            if (!d2d_clip_stack_push(&context->clip_stack, &mask_clip))
                WARN("Failed to push mask clip rect.\n");

            /* Stencil-based clipping: render mask geometry to stencil buffer,
             * then enable stencil test so all subsequent rendering (Clear, draw)
             * only affects pixels within the mask geometry.
             * Supports nesting: each layer increments stencil, test == depth. */
            if (SUCCEEDED(d2d_device_context_ensure_stencil(context)))
            {
                ID3D11DeviceContext *d3d_context;

                ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);

                /* Clear stencil to 0 only on first stencil layer. */
                if (context->stencil_depth == 0)
                    ID3D11DeviceContext_ClearDepthStencilView(d3d_context,
                            context->stencil_dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);

                /* Increment stencil where mask geometry covers. */
                context->stencil_depth++;
                context->stencil_writing = TRUE;

                d2d_device_context_render_mask_to_stencil(context, layer_parameters->geometricMask);

                context->stencil_writing = FALSE;
                ID3D11DeviceContext_Release(d3d_context);
            }

            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.bypass_layer = TRUE;
            info.clip_push_count = 2; /* transformed_rect + mask BBox clip */
            /* Save mask geometry for stencil DECR in PopLayer. */
            info.stencil_geometry = layer_parameters->geometricMask;
            if (info.stencil_geometry)
                ID2D1Geometry_AddRef(info.stencil_geometry);

            if (!d2d_layer_stack_push(&context->layer_stack, &info))
                WARN("Failed to push layer.\n");

            return;
        }

        /* Create a temporary render target for layer content. */
        memset(&bitmap_desc, 0, sizeof(bitmap_desc));
        bitmap_desc.pixelFormat = context->desc.pixelFormat;
        bitmap_desc.dpiX = context->desc.dpiX;
        bitmap_desc.dpiY = context->desc.dpiY;
        bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

        layer_bitmap = NULL;
        hr = d2d_bitmap_create(context, context->pixel_size, NULL, 0, &bitmap_desc, &layer_bitmap);
        if (SUCCEEDED(hr) && layer_bitmap)
        {
            ID3D11DeviceContext *d3d_context;
            static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            /* Save current state. */
            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.mask_aa_mode = layer_parameters->maskAntialiasMode;
            info.prev_target = context->target.bitmap;
            ID2D1Bitmap1_AddRef(&info.prev_target->ID2D1Bitmap1_iface);
            info.prev_pixel_size = context->pixel_size;
            info.prev_bs = context->bs;
            if (context->bs)
                ID3D11BlendState_AddRef(context->bs);
            info.layer_bitmap = layer_bitmap;
            info.clip_push_count = 1; /* transformed_rect clip */

            /* Save geometric mask for compositing in PopLayer. */
            if (layer_parameters->geometricMask)
            {
                D2D1_RECT_F mb;
                const struct d2d_geometry *mask_geo;

                ID2D1Geometry_GetBounds(layer_parameters->geometricMask, NULL, &mb);
                mask_geo = unsafe_impl_from_ID2D1Geometry(layer_parameters->geometricMask);

                if (mb.left <= 1.0f && mb.top <= 1.0f
                        && mb.right >= context->pixel_size.width - 1
                        && mb.bottom >= context->pixel_size.height - 1
                        && mask_geo->fill.face_count <= 2)
                {
                    /* Full-frame simple rectangle — skip geometry. Complex
                     * shapes (face_count > 2) must always be applied. */
                }
                else
                {
                    info.mask_geometry = layer_parameters->geometricMask;
                    ID2D1Geometry_AddRef(info.mask_geometry);
                    info.mask_transform = layer_parameters->maskTransform;
                }
            }

            /* Save opacity brush for per-pixel masking in PopLayer. */
            if (layer_parameters->opacityBrush)
            {
                info.opacity_brush = layer_parameters->opacityBrush;
                ID2D1Brush_AddRef(info.opacity_brush);
            }

            /* Clear temporary surface to transparent. */
            ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
            ID3D11DeviceContext_ClearRenderTargetView(d3d_context, layer_bitmap->rtv, transparent);
            ID3D11DeviceContext_Release(d3d_context);

            if (layer_parameters->layerOptions & D2D1_LAYER_OPTIONS1_IGNORE_ALPHA)
                info.ignore_alpha = TRUE;

            /* Switch render target to the layer bitmap. */
            context->target.bitmap = layer_bitmap;
        }
        else
        {
            WARN("Failed to create layer bitmap, hr %#lx. Rendering without layer.\n", hr);
            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.clip_push_count = 1; /* transformed_rect clip pushed before bitmap creation */
        }

        if (!d2d_layer_stack_push(&context->layer_stack, &info))
            WARN("Failed to push layer.\n");
    }
}

/* Pick the alpha mode the layer bitmap is composited back with.  The bitmap
 * inherits its pixel format from the target, so on an opaque target it carries
 * D2D1_ALPHA_MODE_IGNORE and the bitmap brush forces alpha to 1.0.  Since
 * PushLayer() cleared the bitmap to transparent, everything the layer did not
 * paint would then land as opaque black over the composited area.  Composite
 * with the alpha the layer actually holds, unless the caller asked through
 * D2D1_LAYER_OPTIONS1_IGNORE_ALPHA for it to be ignored.  Returns the previous
 * alpha mode, which the caller restores once the composite is done. */
static D2D1_ALPHA_MODE d2d_layer_composite_alpha_mode(struct d2d_layer_info *info)
{
    D2D1_ALPHA_MODE saved = info->layer_bitmap->format.alphaMode;

    info->layer_bitmap->format.alphaMode = info->ignore_alpha
            ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED;
    return saved;
}

static void STDMETHODCALLTYPE d2d_device_context_PopLayer(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_layer_info info;

    TRACE("iface %p.\n", iface);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_pop_layer(context->target.command_list);

    if (context->target.type == D2D_TARGET_BITMAP)
    {
        if (!d2d_layer_stack_pop(&context->layer_stack, &info))
        {
            WARN("Layer stack underflow.\n");
            return;
        }

        /* Pop exactly the clips this layer pushed (0 for element-layer bypass,
         * 1 for a normal layer, 2 for a stencil/clip-only bypass). Using the
         * per-layer count avoids the old over-pop where the shared bypass_layer
         * flag popped 2 clips even for the element-layer bypass that pushed none. */
        {
            unsigned int i;
            for (i = 0; i < info.clip_push_count; ++i)
                d2d_clip_stack_pop(&context->clip_stack);
        }

        /* Bypass layer: decrement stencil, restore previous layer. */
        if (info.bypass_layer)
        {
            /* Decrement stencil where this layer's mask covered, restoring
             * the previous layer's stencil values for correct nesting. */
            if (context->stencil_depth > 0 && info.stencil_geometry)
            {
                ID3D11DeviceContext *d3d_context;

                ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);

                context->stencil_decrementing = TRUE;

                d2d_device_context_render_mask_to_stencil(context, info.stencil_geometry);

                context->stencil_decrementing = FALSE;
                context->stencil_depth--;

                /* If no more stencil layers, disable stencil test entirely. */
                if (context->stencil_depth == 0)
                    ID3D11DeviceContext_OMSetDepthStencilState(d3d_context, NULL, 0);

                ID3D11DeviceContext_Release(d3d_context);
                ID2D1Geometry_Release(info.stencil_geometry);
            }
            else if (context->stencil_depth > 0)
            {
                /* No geometry saved (shouldn't happen), just decrement depth. */
                context->stencil_depth--;
                if (context->stencil_depth == 0)
                {
                    ID3D11DeviceContext *d3d_context;
                    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
                    ID3D11DeviceContext_OMSetDepthStencilState(d3d_context, NULL, 0);
                    ID3D11DeviceContext_Release(d3d_context);
                }
            }
            return;
        }

        if (info.layer_bitmap && info.prev_target)
        {
            D2D1_RECT_F dst_rect;
            D2D1_SIZE_F size;

            /* Restore original render target. */
            context->target.bitmap = info.prev_target;
            context->pixel_size = info.prev_pixel_size;
            if (context->bs)
                ID3D11BlendState_Release(context->bs);
            context->bs = info.prev_bs;
            info.prev_bs = NULL;

            if (info.mask_geometry)
            {
                /* Composite layer through geometric mask using FillGeometry.
                 * Creates a bitmap brush from the layer bitmap and fills
                 * only the mask shape, clipping the layer content to the
                 * mask geometry boundary. */
                D2D1_BITMAP_BRUSH_PROPERTIES1 bitmap_brush_desc;
                D2D1_BRUSH_PROPERTIES brush_desc;
                struct d2d_brush *mask_brush;
                D2D1_MATRIX_3X2_F saved_transform;
                HRESULT hr;

                memset(&bitmap_brush_desc, 0, sizeof(bitmap_brush_desc));
                bitmap_brush_desc.extendModeX = D2D1_EXTEND_MODE_CLAMP;
                bitmap_brush_desc.extendModeY = D2D1_EXTEND_MODE_CLAMP;
                bitmap_brush_desc.interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

                /* Brush transform = inverse(maskTransform) maps from
                 * mask-local space back to logical/bitmap space so the
                 * bitmap brush samples correctly. For identity maskTransform
                 * (common case), this is just identity. */
                brush_desc.opacity = info.opacity;
                if (!d2d_matrix_invert(&brush_desc.transform, &info.mask_transform))
                {
                    brush_desc.transform._11 = 1.0f;
                    brush_desc.transform._21 = 0.0f;
                    brush_desc.transform._31 = 0.0f;
                    brush_desc.transform._12 = 0.0f;
                    brush_desc.transform._22 = 1.0f;
                    brush_desc.transform._32 = 0.0f;
                }

                hr = d2d_bitmap_brush_create(context->factory,
                        (ID2D1Bitmap *)&info.layer_bitmap->ID2D1Bitmap1_iface,
                        &bitmap_brush_desc, &brush_desc, &mask_brush);
                if (SUCCEEDED(hr))
                {
                    D2D1_ALPHA_MODE saved_alpha = d2d_layer_composite_alpha_mode(&info);

                    /* Apply maskTransform to world transform so the mask
                     * geometry is positioned correctly in device space. */
                    saved_transform = context->drawing_state.transform;
                    d2d_matrix_multiply(&context->drawing_state.transform,
                            &info.mask_transform);

                    /* Use the stored maskAntialiasMode for compositing so
                     * the mask geometry edges are properly antialiased. */
                    {
                        D2D1_ANTIALIAS_MODE prev_aa = context->drawing_state.antialiasMode;
                        context->drawing_state.antialiasMode = info.mask_aa_mode;

                        d2d_device_context_FillGeometry(
                                &context->ID2D1DeviceContext6_iface,
                                info.mask_geometry,
                                &mask_brush->ID2D1Brush_iface, NULL);

                        context->drawing_state.antialiasMode = prev_aa;
                    }

                    context->drawing_state.transform = saved_transform;
                    info.layer_bitmap->format.alphaMode = saved_alpha;
                    ID2D1Brush_Release(&mask_brush->ID2D1Brush_iface);
                }
                else
                {
                    WARN("Failed to create mask brush, hr %#lx. "
                            "Falling back to unmasked composite.\n", hr);
                    size = ID2D1Bitmap1_GetSize(&info.layer_bitmap->ID2D1Bitmap1_iface);
                    d2d_rect_set(&dst_rect, 0.0f, 0.0f, size.width, size.height);
                    d2d_device_context_draw_bitmap(context,
                            (ID2D1Bitmap *)&info.layer_bitmap->ID2D1Bitmap1_iface,
                            &dst_rect, info.opacity,
                            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                            NULL, NULL, NULL);
                }
                ID2D1Geometry_Release(info.mask_geometry);
            }
            else if (info.opacity_brush)
            {
                /* Opacity brush — composite layer through per-pixel opacity mask.
                 * Creates a bitmap brush from the layer bitmap and fills
                 * a full-frame rectangle, using the opacity brush for
                 * per-pixel alpha masking in the shader. */
                D2D1_BITMAP_BRUSH_PROPERTIES1 bitmap_brush_desc;
                D2D1_BRUSH_PROPERTIES brush_desc;
                struct d2d_brush *layer_brush;
                ID2D1RectangleGeometry *rect_geo = NULL;
                D2D1_MATRIX_3X2_F saved_transform;
                HRESULT hr;

                memset(&bitmap_brush_desc, 0, sizeof(bitmap_brush_desc));
                bitmap_brush_desc.extendModeX = D2D1_EXTEND_MODE_CLAMP;
                bitmap_brush_desc.extendModeY = D2D1_EXTEND_MODE_CLAMP;
                bitmap_brush_desc.interpolationMode = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

                brush_desc.opacity = info.opacity;
                brush_desc.transform._11 = 1.0f;
                brush_desc.transform._21 = 0.0f;
                brush_desc.transform._31 = 0.0f;
                brush_desc.transform._12 = 0.0f;
                brush_desc.transform._22 = 1.0f;
                brush_desc.transform._32 = 0.0f;

                hr = d2d_bitmap_brush_create(context->factory,
                        (ID2D1Bitmap *)&info.layer_bitmap->ID2D1Bitmap1_iface,
                        &bitmap_brush_desc, &brush_desc, &layer_brush);
                if (SUCCEEDED(hr))
                {
                    D2D1_ALPHA_MODE saved_alpha = d2d_layer_composite_alpha_mode(&info);

                    size = ID2D1Bitmap1_GetSize(&info.layer_bitmap->ID2D1Bitmap1_iface);
                    d2d_rect_set(&dst_rect, 0.0f, 0.0f, size.width, size.height);

                    if (SUCCEEDED(ID2D1Factory1_CreateRectangleGeometry(
                            (ID2D1Factory1 *)context->factory, &dst_rect, &rect_geo)))
                    {
                        saved_transform = context->drawing_state.transform;
                        context->drawing_state.transform._11 = 1.0f;
                        context->drawing_state.transform._21 = 0.0f;
                        context->drawing_state.transform._31 = 0.0f;
                        context->drawing_state.transform._12 = 0.0f;
                        context->drawing_state.transform._22 = 1.0f;
                        context->drawing_state.transform._32 = 0.0f;

                        d2d_device_context_fill_geometry(context,
                                unsafe_impl_from_ID2D1Geometry((ID2D1Geometry *)rect_geo),
                                layer_brush,
                                unsafe_impl_from_ID2D1Brush(info.opacity_brush));

                        context->drawing_state.transform = saved_transform;
                        ID2D1RectangleGeometry_Release(rect_geo);
                    }
                    info.layer_bitmap->format.alphaMode = saved_alpha;
                    ID2D1Brush_Release(&layer_brush->ID2D1Brush_iface);
                }
                else
                {
                    WARN("Failed to create layer brush for opacity mask, hr %#lx.\n", hr);
                    size = ID2D1Bitmap1_GetSize(&info.layer_bitmap->ID2D1Bitmap1_iface);
                    d2d_rect_set(&dst_rect, 0.0f, 0.0f, size.width, size.height);
                    d2d_device_context_draw_bitmap(context,
                            (ID2D1Bitmap *)&info.layer_bitmap->ID2D1Bitmap1_iface,
                            &dst_rect, info.opacity,
                            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                            NULL, NULL, NULL);
                }
            }
            else
            {
                /* No geometric mask, no opacity brush — composite full layer bitmap. */
                D2D1_ALPHA_MODE saved_alpha = d2d_layer_composite_alpha_mode(&info);

                size = ID2D1Bitmap1_GetSize(&info.layer_bitmap->ID2D1Bitmap1_iface);
                d2d_rect_set(&dst_rect, 0.0f, 0.0f, size.width, size.height);
                d2d_device_context_draw_bitmap(context,
                        (ID2D1Bitmap *)&info.layer_bitmap->ID2D1Bitmap1_iface,
                        &dst_rect, info.opacity,
                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        NULL, NULL, NULL);

                info.layer_bitmap->format.alphaMode = saved_alpha;
            }

            /* Release the layer bitmap (prev_target ref released below). */
            ID2D1Bitmap1_Release(&info.layer_bitmap->ID2D1Bitmap1_iface);
            /* Release the saved prev_target ref (we restored the pointer above,
             * the context still holds its own reference). */
            ID2D1Bitmap1_Release(&info.prev_target->ID2D1Bitmap1_iface);
            if (info.opacity_brush)
                ID2D1Brush_Release(info.opacity_brush);
        }
        else
        {
            /* Layer bitmap creation failed in PushLayer — just clean up. */
            if (info.prev_bs)
                ID3D11BlendState_Release(info.prev_bs);
            if (info.mask_geometry)
                ID2D1Geometry_Release(info.mask_geometry);
            if (info.opacity_brush)
                ID2D1Brush_Release(info.opacity_brush);
        }
    }
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_Flush(ID2D1DeviceContext6 *iface, D2D1_TAG *tag1, D2D1_TAG *tag2)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    FIXME("iface %p, tag1 %p, tag2 %p stub!\n", iface, tag1, tag2);

    d2d_device_context_flush_lines(context);

    if (context->ops && context->ops->device_context_present)
        context->ops->device_context_present(context->outer_unknown);

    return S_OK;
}

static void STDMETHODCALLTYPE d2d_device_context_SaveDrawingState(ID2D1DeviceContext6 *iface,
        ID2D1DrawingStateBlock *state_block)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_state_block *state_block_impl;

    TRACE("iface %p, state_block %p.\n", iface, state_block);

    if (!(state_block_impl = unsafe_impl_from_ID2D1DrawingStateBlock(state_block))) return;
    state_block_impl->drawing_state = render_target->drawing_state;
    if (render_target->text_rendering_params)
        IDWriteRenderingParams_AddRef(render_target->text_rendering_params);
    if (state_block_impl->text_rendering_params)
        IDWriteRenderingParams_Release(state_block_impl->text_rendering_params);
    state_block_impl->text_rendering_params = render_target->text_rendering_params;
}

static void STDMETHODCALLTYPE d2d_device_context_RestoreDrawingState(ID2D1DeviceContext6 *iface,
        ID2D1DrawingStateBlock *state_block)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_state_block *state_block_impl;

    TRACE("iface %p, state_block %p.\n", iface, state_block);

    d2d_device_context_flush_lines(context);

    if (!(state_block_impl = unsafe_impl_from_ID2D1DrawingStateBlock(state_block))) return;
    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        struct d2d_command_list *command_list = context->target.command_list;

        if (context->drawing_state.antialiasMode != state_block_impl->drawing_state.antialiasMode)
            d2d_command_list_set_antialias_mode(command_list, state_block_impl->drawing_state.antialiasMode);
        d2d_command_list_set_text_antialias_mode(command_list, state_block_impl->drawing_state.textAntialiasMode);
        d2d_command_list_set_tags(command_list, state_block_impl->drawing_state.tag1, state_block_impl->drawing_state.tag2);
        d2d_command_list_set_transform(command_list, &state_block_impl->drawing_state.transform);
        d2d_command_list_set_primitive_blend(command_list, state_block_impl->drawing_state.primitiveBlend);
        d2d_command_list_set_unit_mode(command_list, state_block_impl->drawing_state.unitMode);
        d2d_command_list_set_text_rendering_params(command_list, state_block_impl->text_rendering_params);
    }

    context->drawing_state = state_block_impl->drawing_state;
    if (state_block_impl->text_rendering_params)
        IDWriteRenderingParams_AddRef(state_block_impl->text_rendering_params);
    if (context->text_rendering_params)
        IDWriteRenderingParams_Release(context->text_rendering_params);
    context->text_rendering_params = state_block_impl->text_rendering_params;
}

static void STDMETHODCALLTYPE d2d_device_context_PushAxisAlignedClip(ID2D1DeviceContext6 *iface,
        const D2D1_RECT_F *clip_rect, D2D1_ANTIALIAS_MODE antialias_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D1_RECT_F transformed_rect;
    float x_scale, y_scale;
    D2D1_POINT_2F point;

    TRACE("iface %p, clip_rect %s, antialias_mode %#x.\n", iface, debug_d2d_rect_f(clip_rect), antialias_mode);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_push_clip(context->target.command_list, clip_rect, antialias_mode);

    if (antialias_mode != D2D1_ANTIALIAS_MODE_ALIASED)
    {
        static int once;
        if (!once++)
            FIXME("Ignoring antialias_mode %#x.\n", antialias_mode);
    }

    x_scale = context->desc.dpiX / 96.0f;
    y_scale = context->desc.dpiY / 96.0f;
    d2d_point_transform(&point, &context->drawing_state.transform,
            clip_rect->left * x_scale, clip_rect->top * y_scale);
    d2d_rect_set(&transformed_rect, point.x, point.y, point.x, point.y);
    d2d_point_transform(&point, &context->drawing_state.transform,
            clip_rect->left * x_scale, clip_rect->bottom * y_scale);
    d2d_rect_expand(&transformed_rect, &point);
    d2d_point_transform(&point, &context->drawing_state.transform,
            clip_rect->right * x_scale, clip_rect->top * y_scale);
    d2d_rect_expand(&transformed_rect, &point);
    d2d_point_transform(&point, &context->drawing_state.transform,
            clip_rect->right * x_scale, clip_rect->bottom * y_scale);
    d2d_rect_expand(&transformed_rect, &point);

    if (!d2d_clip_stack_push(&context->clip_stack, &transformed_rect))
        WARN("Failed to push clip rect.\n");
}

static void STDMETHODCALLTYPE d2d_device_context_PopAxisAlignedClip(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_pop_clip(context->target.command_list);

    d2d_clip_stack_pop(&context->clip_stack);
}

static void STDMETHODCALLTYPE d2d_device_context_Clear(ID2D1DeviceContext6 *iface, const D2D1_COLOR_F *colour)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D3D11_MAPPED_SUBRESOURCE map_desc;
    ID3D11DeviceContext *d3d_context;
    D2D1_COLOR_F *c;
    HRESULT hr;

    TRACE("iface %p, colour %p.\n", iface, colour);

    d2d_device_context_flush_lines(context);

    /* Track Clear() calls inside layers for clear-propagation in PopLayer. */
    if (context->layer_stack.count > 0)
        context->layer_stack.stack[context->layer_stack.count - 1].clear_called = TRUE;

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_clear(context->target.command_list, colour);
        return;
    }

    /* Update vs_cb with identity transform for Clear. */
    {
        struct d2d_vs_cb new_vs;

        new_vs.transform_geometry._11 = 1.0f;
        new_vs.transform_geometry._21 = 0.0f;
        new_vs.transform_geometry._31 = 0.0f;
        new_vs.transform_geometry.miter_limit = 0.0f;
        new_vs.transform_geometry._12 = 0.0f;
        new_vs.transform_geometry._22 = 1.0f;
        new_vs.transform_geometry._32 = 0.0f;
        new_vs.transform_geometry.stroke_width = 0.0f;
        new_vs.transform_rtx.x = 1.0f;
        new_vs.transform_rtx.y = 0.0f;
        new_vs.transform_rtx.z = 1.0f;
        new_vs.transform_rtx.w = 1.0f;
        new_vs.transform_rty.x = 0.0f;
        new_vs.transform_rty.y = 1.0f;
        new_vs.transform_rty.z = 1.0f;
        new_vs.transform_rty.w = -1.0f;

        if (!context->vs_cb_cache_valid || memcmp(&new_vs, &context->vs_cb_cache, sizeof(new_vs)))
        {
            ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
            if (FAILED(hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)context->vs_cb,
                    0, D3D11_MAP_WRITE_DISCARD, 0, &map_desc)))
            {
                WARN("Failed to map vs constant buffer, hr %#lx.\n", hr);
                ID3D11DeviceContext_Release(d3d_context);
                return;
            }
            memcpy(map_desc.pData, &new_vs, sizeof(new_vs));
            ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)context->vs_cb, 0);
            ID3D11DeviceContext_Release(d3d_context);
            context->vs_cb_cache = new_vs;
            context->vs_cb_cache_valid = TRUE;
        }
    }

    /* Update ps_cb with solid brush for Clear. */
    {
        struct d2d_ps_cb new_ps;

        memset(&new_ps, 0, sizeof(new_ps));
        new_ps.colour_brush.type = D2D_BRUSH_TYPE_SOLID;
        new_ps.colour_brush.opacity = 1.0f;
        new_ps.opacity_brush.type = D2D_BRUSH_TYPE_COUNT;
        c = &new_ps.colour_brush.u.solid.colour;
        if (colour)
            *c = *colour;
        if (context->desc.pixelFormat.alphaMode == D2D1_ALPHA_MODE_IGNORE)
            c->a = 1.0f;
        c->r *= c->a;
        c->g *= c->a;
        c->b *= c->a;

        if (!context->ps_cb_cache_valid || memcmp(&new_ps, &context->ps_cb_cache, sizeof(new_ps)))
        {
            ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
            if (FAILED(hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)context->ps_cb,
                    0, D3D11_MAP_WRITE_DISCARD, 0, &map_desc)))
            {
                WARN("Failed to map ps constant buffer, hr %#lx.\n", hr);
                ID3D11DeviceContext_Release(d3d_context);
                return;
            }
            memcpy(map_desc.pData, &new_ps, sizeof(new_ps));
            ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)context->ps_cb, 0);
            ID3D11DeviceContext_Release(d3d_context);
            context->ps_cb_cache = new_ps;
            context->ps_cb_cache_valid = TRUE;
        }
    }

    d2d_device_context_draw(context, D2D_SHAPE_TYPE_TRIANGLE, context->ib, 6,
            context->vb, context->vb_stride, NULL, NULL);
}

static void STDMETHODCALLTYPE d2d_device_context_BeginDraw(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_begin_draw(context->target.command_list, context);

    memset(&context->error, 0, sizeof(context->error));
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_EndDraw(ID2D1DeviceContext6 *iface,
        D2D1_TAG *tag1, D2D1_TAG *tag2)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    HRESULT hr;

    TRACE("iface %p, tag1 %p, tag2 %p.\n", iface, tag1, tag2);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        FIXME("Unimplemented for command list target.\n");
        return E_NOTIMPL;
    }

    if (tag1)
        *tag1 = context->error.tag1;
    if (tag2)
        *tag2 = context->error.tag2;

    if (context->ops && context->ops->device_context_present)
    {
        if (FAILED(hr = context->ops->device_context_present(context->outer_unknown)))
            context->error.code = hr;
    }

    return context->error.code;
}

static D2D1_PIXEL_FORMAT * STDMETHODCALLTYPE d2d_device_context_GetPixelFormat(ID2D1DeviceContext6 *iface,
        D2D1_PIXEL_FORMAT *format)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, format %p.\n", iface, format);

    *format = render_target->desc.pixelFormat;
    return format;
}

static void STDMETHODCALLTYPE d2d_device_context_SetDpi(ID2D1DeviceContext6 *iface, float dpi_x, float dpi_y)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, dpi_x %.8e, dpi_y %.8e.\n", iface, dpi_x, dpi_y);

    d2d_device_context_flush_lines(render_target);

    if (dpi_x == 0.0f && dpi_y == 0.0f)
    {
        dpi_x = 96.0f;
        dpi_y = 96.0f;
    }
    else if (dpi_x <= 0.0f || dpi_y <= 0.0f)
        return;

    render_target->desc.dpiX = dpi_x;
    render_target->desc.dpiY = dpi_y;
}

static void STDMETHODCALLTYPE d2d_device_context_GetDpi(ID2D1DeviceContext6 *iface, float *dpi_x, float *dpi_y)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, dpi_x %p, dpi_y %p.\n", iface, dpi_x, dpi_y);

    *dpi_x = render_target->desc.dpiX;
    *dpi_y = render_target->desc.dpiY;
}

static D2D1_SIZE_F * STDMETHODCALLTYPE d2d_device_context_GetSize(ID2D1DeviceContext6 *iface, D2D1_SIZE_F *size)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, size %p.\n", iface, size);

    size->width = render_target->pixel_size.width / (render_target->desc.dpiX / 96.0f);
    size->height = render_target->pixel_size.height / (render_target->desc.dpiY / 96.0f);
    return size;
}

static D2D1_SIZE_U * STDMETHODCALLTYPE d2d_device_context_GetPixelSize(ID2D1DeviceContext6 *iface,
        D2D1_SIZE_U *pixel_size)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, pixel_size %p.\n", iface, pixel_size);

    *pixel_size = render_target->pixel_size;
    return pixel_size;
}

static UINT32 STDMETHODCALLTYPE d2d_device_context_GetMaximumBitmapSize(ID2D1DeviceContext6 *iface)
{
    TRACE("iface %p.\n", iface);

    return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

static BOOL STDMETHODCALLTYPE d2d_device_context_IsSupported(ID2D1DeviceContext6 *iface,
        const D2D1_RENDER_TARGET_PROPERTIES *desc)
{
    FIXME("iface %p, desc %p stub!\n", iface, desc);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_CreateBitmap(ID2D1DeviceContext6 *iface,
        D2D1_SIZE_U size, const void *src_data, UINT32 pitch,
        const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, size {%u, %u}, src_data %p, pitch %u, desc %p, bitmap %p.\n",
            iface, size.width, size.height, src_data, pitch, desc, bitmap);

    if (SUCCEEDED(hr = d2d_bitmap_create(context, size, src_data, pitch, desc, &object)))
        *bitmap = &object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_CreateBitmapFromWicBitmap(
        ID2D1DeviceContext6 *iface, IWICBitmapSource *bitmap_source,
        const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, bitmap_source %p, desc %p, bitmap %p.\n", iface, bitmap_source, desc, bitmap);

    if (SUCCEEDED(hr = d2d_bitmap_create_from_wic_bitmap(context, bitmap_source, desc, &object)))
        *bitmap = &object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateColorContext(ID2D1DeviceContext6 *iface,
        D2D1_COLOR_SPACE space, const BYTE *profile, UINT32 profile_size, ID2D1ColorContext **color_context)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_color_context *object;
    HRESULT hr;

    TRACE("iface %p, space %#x, profile %p, profile_size %u, color_context %p.\n",
            iface, space, profile, profile_size, color_context);

    if (SUCCEEDED(hr = d2d_color_context_create(context->factory, space, profile, profile_size, &object)))
        *color_context = (ID2D1ColorContext *)&object->ID2D1ColorContext1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateColorContextFromFilename(ID2D1DeviceContext6 *iface,
        const WCHAR *filename, ID2D1ColorContext **color_context)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_color_context *object;
    HRESULT hr;

    TRACE("iface %p, filename %s, color_context %p.\n", iface, debugstr_w(filename), color_context);

    if (SUCCEEDED(hr = d2d_color_context_create_from_filename(context->factory, filename, &object)))
        *color_context = (ID2D1ColorContext *)&object->ID2D1ColorContext1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateColorContextFromWicColorContext(ID2D1DeviceContext6 *iface,
        IWICColorContext *wic_color_context, ID2D1ColorContext **color_context)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_color_context *object;
    HRESULT hr;

    TRACE("iface %p, wic_color_context %p, color_context %p.\n", iface, wic_color_context, color_context);

    if (SUCCEEDED(hr = d2d_color_context_create_from_wic(context->factory, wic_color_context, &object)))
        *color_context = (ID2D1ColorContext *)&object->ID2D1ColorContext1_iface;

    return hr;
}

static BOOL d2d_bitmap_check_options_with_surface(unsigned int options, unsigned int surface_options)
{
    switch (options)
    {
        case D2D1_BITMAP_OPTIONS_NONE:
        case D2D1_BITMAP_OPTIONS_TARGET:
        case D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW:
        case D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE:
        case D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE:
        case D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ:
        case D2D1_BITMAP_OPTIONS_CANNOT_DRAW:
            break;
        default:
            WARN("Invalid bitmap options %#x.\n", options);
            return FALSE;
    }

    if (options && (options & D2D1_BITMAP_OPTIONS_TARGET) != (surface_options & D2D1_BITMAP_OPTIONS_TARGET))
        return FALSE;
    if (!(options & D2D1_BITMAP_OPTIONS_CANNOT_DRAW) && (surface_options & D2D1_BITMAP_OPTIONS_CANNOT_DRAW))
        return FALSE;
    if (options & D2D1_BITMAP_OPTIONS_TARGET)
    {
        if (options & D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE && !(surface_options & D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE))
            return FALSE;
        return TRUE;
    }

    if (options & D2D1_BITMAP_OPTIONS_CANNOT_DRAW)
    {
        if (!(surface_options & D2D1_BITMAP_OPTIONS_CANNOT_DRAW))
            return FALSE;

        if (options & D2D1_BITMAP_OPTIONS_CPU_READ && !(surface_options & D2D1_BITMAP_OPTIONS_CPU_READ))
            return FALSE;
    }

    return TRUE;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateBitmapFromDxgiSurface(ID2D1DeviceContext6 *iface,
        IDXGISurface *surface, const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    unsigned int surface_options;
    struct d2d_bitmap *object;
    HRESULT hr;

    TRACE("iface %p, surface %p, desc %p, bitmap %p.\n", iface, surface, desc, bitmap);

    surface_options = d2d_get_bitmap_options_for_surface(surface);

    if (desc)
    {
        if (!d2d_bitmap_check_options_with_surface(desc->bitmapOptions, surface_options))
        {
            WARN("Incompatible bitmap options %#x, surface options %#x.\n",
                    desc->bitmapOptions, surface_options);
            return E_INVALIDARG;
        }
    }
    else
    {
        DXGI_SURFACE_DESC surface_desc;

        if (FAILED(hr = IDXGISurface_GetDesc(surface, &surface_desc)))
        {
            WARN("Failed to get surface desc, hr %#lx.\n", hr);
            return hr;
        }

        memset(&bitmap_desc, 0, sizeof(bitmap_desc));
        bitmap_desc.pixelFormat.format = surface_desc.Format;
        bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bitmap_desc.bitmapOptions = surface_options;
        desc = &bitmap_desc;
    }

    if (SUCCEEDED(hr = d2d_bitmap_create_shared(context, &IID_IDXGISurface, surface, desc, &object)))
        *bitmap = &object->ID2D1Bitmap1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateEffect(ID2D1DeviceContext6 *iface,
        REFCLSID effect_id, ID2D1Effect **effect)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, effect_id %s, effect %p.\n", iface, debugstr_guid(effect_id), effect);

    return d2d_effect_create(context, effect_id, effect);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_CreateGradientStopCollection(
        ID2D1DeviceContext6 *iface, const D2D1_GRADIENT_STOP *stops, UINT32 stop_count,
        D2D1_COLOR_SPACE preinterpolation_space, D2D1_COLOR_SPACE postinterpolation_space,
        D2D1_BUFFER_PRECISION buffer_precision, D2D1_EXTEND_MODE extend_mode,
        D2D1_COLOR_INTERPOLATION_MODE color_interpolation_mode, ID2D1GradientStopCollection1 **gradient)
{
    struct d2d_device_context *render_target = impl_from_ID2D1DeviceContext(iface);
    struct d2d_gradient *object;
    HRESULT hr;

    TRACE("iface %p, stops %p, stop_count %u, preinterpolation_space %#x, postinterpolation_space %#x, "
            "buffer_precision %#x, extend_mode %#x, color_interpolation_mode %#x, gradient %p.\n",
            iface, stops, stop_count, preinterpolation_space, postinterpolation_space,
            buffer_precision, extend_mode, color_interpolation_mode, gradient);

    if (SUCCEEDED(hr = d2d_gradient_create(render_target->factory, render_target->d3d_device,
            stops, stop_count, preinterpolation_space == D2D1_COLOR_SPACE_SCRGB ? D2D1_GAMMA_1_0 : D2D1_GAMMA_2_2,
            extend_mode, preinterpolation_space, postinterpolation_space, buffer_precision,
            color_interpolation_mode, &object)))
        *gradient = &object->ID2D1GradientStopCollection1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateImageBrush(ID2D1DeviceContext6 *iface,
        ID2D1Image *image, const D2D1_IMAGE_BRUSH_PROPERTIES *image_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1ImageBrush **brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, image %p, image_brush_desc %p, brush_desc %p, brush %p.\n", iface, image, image_brush_desc,
            brush_desc, brush);

    if (SUCCEEDED(hr = d2d_image_brush_create(context->factory, image, image_brush_desc,
            brush_desc, &object)))
        *brush = (ID2D1ImageBrush *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_CreateBitmapBrush(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *bitmap, const D2D1_BITMAP_BRUSH_PROPERTIES1 *bitmap_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1BitmapBrush1 **brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_brush *object;
    HRESULT hr;

    TRACE("iface %p, bitmap %p, bitmap_brush_desc %p, brush_desc %p, brush %p.\n", iface, bitmap, bitmap_brush_desc,
            brush_desc, brush);

    if (SUCCEEDED(hr = d2d_bitmap_brush_create(context->factory, bitmap, bitmap_brush_desc, brush_desc, &object)))
        *brush = (ID2D1BitmapBrush1 *)&object->ID2D1Brush_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateCommandList(ID2D1DeviceContext6 *iface,
        ID2D1CommandList **command_list)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_command_list *object;
    HRESULT hr;

    TRACE("iface %p, command_list %p.\n", iface, command_list);

    if (SUCCEEDED(hr = d2d_command_list_create(context->factory, &object)))
        *command_list = &object->ID2D1CommandList_iface;

    return hr;
}

static BOOL STDMETHODCALLTYPE d2d_device_context_IsDxgiFormatSupported(ID2D1DeviceContext6 *iface, DXGI_FORMAT format)
{
    FIXME("iface %p, format %#x stub!\n", iface, format);

    return FALSE;
}

static BOOL STDMETHODCALLTYPE d2d_device_context_IsBufferPrecisionSupported(ID2D1DeviceContext6 *iface,
        D2D1_BUFFER_PRECISION buffer_precision)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    DXGI_FORMAT format;
    UINT support = 0;
    HRESULT hr;

    TRACE("iface %p, buffer_precision %u.\n", iface, buffer_precision);

    switch (buffer_precision)
    {
        case D2D1_BUFFER_PRECISION_8BPC_UNORM: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case D2D1_BUFFER_PRECISION_8BPC_UNORM_SRGB: format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        case D2D1_BUFFER_PRECISION_16BPC_UNORM: format = DXGI_FORMAT_R16G16B16A16_UNORM; break;
        case D2D1_BUFFER_PRECISION_16BPC_FLOAT: format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case D2D1_BUFFER_PRECISION_32BPC_FLOAT: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        default:
            WARN("Unexpected precision %u.\n", buffer_precision);
            return FALSE;
    }

    if (FAILED(hr = ID3D11Device1_CheckFormatSupport(context->d3d_device, format, &support)))
    {
        WARN("Format support check failed, hr %#lx.\n", hr);
    }

    return !!(support & D3D11_FORMAT_SUPPORT_BUFFER);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetImageLocalBounds(ID2D1DeviceContext6 *iface,
        ID2D1Image *image, D2D1_RECT_F *local_bounds)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    D2D_SIZE_U pixel_size;
    ID2D1Bitmap *bitmap;
    D2D_SIZE_F size;

    TRACE("iface %p, image %p, local_bounds %p.\n", iface, image, local_bounds);

    if (SUCCEEDED(ID2D1Image_QueryInterface(image, &IID_ID2D1Bitmap, (void **)&bitmap)))
    {
        local_bounds->left = 0.0f;
        local_bounds->top  = 0.0f;
        switch (context->drawing_state.unitMode)
        {
            case D2D1_UNIT_MODE_DIPS:
                size = ID2D1Bitmap_GetSize(bitmap);
                local_bounds->right  = size.width;
                local_bounds->bottom = size.height;
                break;

            case D2D1_UNIT_MODE_PIXELS:
                pixel_size = ID2D1Bitmap_GetPixelSize(bitmap);
                local_bounds->right  = pixel_size.width;
                local_bounds->bottom = pixel_size.height;
                break;

            default:
                WARN("Unknown unit mode %#x.\n", context->drawing_state.unitMode);
                break;
        }
        ID2D1Bitmap_Release(bitmap);

        return S_OK;
    }
    else
    {
        FIXME("Unable to get local bounds of image %p.\n", image);

        return E_NOTIMPL;
    }
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetImageWorldBounds(ID2D1DeviceContext6 *iface,
        ID2D1Image *image, D2D1_RECT_F *world_bounds)
{
    FIXME("iface %p, image %p, world_bounds %p stub!\n", iface, image, world_bounds);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetGlyphRunWorldBounds(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        DWRITE_MEASURING_MODE measuring_mode, D2D1_RECT_F *bounds)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    DWRITE_TEXT_ANTIALIAS_MODE antialias_mode;
    DWRITE_RENDERING_MODE rendering_mode;
    IDWriteGlyphRunAnalysis *analysis;
    DWRITE_TEXTURE_TYPE texture_type;
    ID2D1PathGeometry *geometry;
    D2D1_MATRIX_3X2_F transform;
    float scale_x, scale_y;
    HRESULT hr;
    RECT rect;

    TRACE("iface %p, baseline_origin %s, glyph_run %p, measuring_mode %#x, bounds %p.\n",
            iface, debug_d2d_point_2f(&baseline_origin), glyph_run, measuring_mode, bounds);

    if (FAILED(hr = d2d_device_context_get_text_rendering_mode(context, glyph_run, measuring_mode,
            &antialias_mode, &rendering_mode, NULL)))
    {
        return hr;
    }

    if (rendering_mode == DWRITE_RENDERING_MODE_OUTLINE)
    {
        if (FAILED(hr = d2d_device_context_get_glyph_run_geometry(context, glyph_run, &geometry)))
            return hr;

        transform = context->drawing_state.transform;
        transform._31 += baseline_origin.x * transform._11 + baseline_origin.y * transform._21;
        transform._32 += baseline_origin.x * transform._12 + baseline_origin.y * transform._22;
        hr = ID2D1PathGeometry_GetBounds(geometry, &transform, bounds);
        ID2D1PathGeometry_Release(geometry);
    }
    else
    {
        if (FAILED(hr = d2d_device_context_get_glyph_run_analysis(context, baseline_origin,
                glyph_run, rendering_mode, measuring_mode, antialias_mode, &texture_type, &analysis)))
        {
            return hr;
        }

        if (SUCCEEDED(hr = IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, texture_type, &rect)))
        {
            scale_x = context->desc.dpiX / 96.0f;
            scale_y = context->desc.dpiY / 96.0f;
            d2d_rect_set(bounds, rect.left / scale_x, rect.top / scale_y,
                    rect.right / scale_x, rect.bottom / scale_y);
        }

        IDWriteGlyphRunAnalysis_Release(analysis);
    }

    return hr;
}

static void STDMETHODCALLTYPE d2d_device_context_GetDevice(ID2D1DeviceContext6 *iface, ID2D1Device **device)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID2D1Device *)&context->device->ID2D1Device6_iface;
    ID2D1Device_AddRef(*device);
}

/* Drop the target's views from the context's private D3D state. The state
 * keeps them bound after a draw, and a bound view keeps the wined3d resource
 * alive past the last D2D or D3D11 reference to it. A swapchain back buffer
 * released this way is still held when the application calls
 * IDXGISwapChain::ResizeBuffers() after SetTarget(NULL), and wined3d then
 * orphans it until the next draw rebinds the state. */
static void d2d_device_context_unbind_target(struct d2d_device_context *context)
{
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *d3d_context;

    if (!context->d3d_state)
        return;

    if (context->cs)
        EnterCriticalSection(context->cs);

    ID3D11Device1_GetImmediateContext1(context->d3d_device, &d3d_context);
    ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, context->d3d_state, &prev_state);
    ID3D11DeviceContext1_OMSetRenderTargets(d3d_context, 0, NULL, NULL);
    ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, prev_state, NULL);
    ID3DDeviceContextState_Release(prev_state);
    ID3D11DeviceContext1_Release(d3d_context);

    if (context->cs)
        LeaveCriticalSection(context->cs);
}

static void d2d_device_context_reset_target(struct d2d_device_context *context)
{
    if (!context->target.object)
        return;

    d2d_device_context_unbind_target(context);

    IUnknown_Release(context->target.object);
    memset(&context->target, 0, sizeof(context->target));

    /* Note that DPI settings are kept. */
    memset(&context->desc.pixelFormat, 0, sizeof(context->desc.pixelFormat));
    memset(&context->pixel_size, 0, sizeof(context->pixel_size));

    if (context->bs)
        ID3D11BlendState_Release(context->bs);
    context->bs = NULL;
}

static void STDMETHODCALLTYPE d2d_device_context_SetTarget(ID2D1DeviceContext6 *iface, ID2D1Image *target)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_command_list *command_list_impl;
    struct d2d_bitmap *bitmap_impl;
    ID2D1CommandList *command_list;
    D3D11_BLEND_DESC blend_desc;
    ID2D1Bitmap *bitmap;
    HRESULT hr;

    TRACE("iface %p, target %p.\n", iface, target);

    d2d_device_context_flush_lines(context);

    if (!target)
    {
        d2d_device_context_reset_target(context);
        return;
    }

    if (SUCCEEDED(ID2D1Image_QueryInterface(target, &IID_ID2D1Bitmap, (void **)&bitmap)))
    {
        bitmap_impl = unsafe_impl_from_ID2D1Bitmap(bitmap);

        if (!(bitmap_impl->options & D2D1_BITMAP_OPTIONS_TARGET))
        {
            ID2D1Bitmap_Release(bitmap);
            d2d_device_context_set_error(context, D2DERR_INVALID_TARGET);
            return;
        }

        d2d_device_context_reset_target(context);

        /* Set sizes and pixel format. */
        context->pixel_size = bitmap_impl->pixel_size;
        context->desc.pixelFormat = bitmap_impl->format;
        context->target.bitmap = bitmap_impl;
        context->target.object = target;
        context->target.type = D2D_TARGET_BITMAP;

        memset(&blend_desc, 0, sizeof(blend_desc));
        blend_desc.IndependentBlendEnable = FALSE;
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(hr = ID3D11Device1_CreateBlendState(context->d3d_device, &blend_desc, &context->bs)))
            WARN("Failed to create blend state, hr %#lx.\n", hr);
    }
    else if (SUCCEEDED(ID2D1Image_QueryInterface(target, &IID_ID2D1CommandList, (void **)&command_list)))
    {
        command_list_impl = unsafe_impl_from_ID2D1CommandList(command_list);

        d2d_device_context_reset_target(context);

        context->target.command_list = command_list_impl;
        context->target.object = target;
        context->target.type = D2D_TARGET_COMMAND_LIST;
    }
    else
    {
        WARN("Unsupported target type.\n");
    }
}

static void STDMETHODCALLTYPE d2d_device_context_GetTarget(ID2D1DeviceContext6 *iface, ID2D1Image **target)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, target %p.\n", iface, target);

    d2d_device_context_flush_lines(context);

    *target = context->target.object ? context->target.object : NULL;
    if (*target)
        ID2D1Image_AddRef(*target);
}

static void STDMETHODCALLTYPE d2d_device_context_SetRenderingControls(ID2D1DeviceContext6 *iface,
        const D2D1_RENDERING_CONTROLS *rendering_controls)
{
    FIXME("iface %p, rendering_controls %p stub!\n", iface, rendering_controls);
}

static void STDMETHODCALLTYPE d2d_device_context_GetRenderingControls(ID2D1DeviceContext6 *iface,
        D2D1_RENDERING_CONTROLS *rendering_controls)
{
    FIXME("iface %p, rendering_controls %p stub!\n", iface, rendering_controls);
}

static void STDMETHODCALLTYPE d2d_device_context_SetPrimitiveBlend(ID2D1DeviceContext6 *iface,
        D2D1_PRIMITIVE_BLEND primitive_blend)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, primitive_blend %u.\n", iface, primitive_blend);

    d2d_device_context_flush_lines(context);

    if (primitive_blend > D2D1_PRIMITIVE_BLEND_MAX)
    {
        WARN("Unknown blend mode %u.\n", primitive_blend);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_primitive_blend(context->target.command_list, primitive_blend);

    context->drawing_state.primitiveBlend = primitive_blend;
}

static D2D1_PRIMITIVE_BLEND STDMETHODCALLTYPE d2d_device_context_GetPrimitiveBlend(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return context->drawing_state.primitiveBlend;
}

static void STDMETHODCALLTYPE d2d_device_context_SetUnitMode(ID2D1DeviceContext6 *iface, D2D1_UNIT_MODE unit_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, unit_mode %#x.\n", iface, unit_mode);

    d2d_device_context_flush_lines(context);

    if (unit_mode != D2D1_UNIT_MODE_DIPS && unit_mode != D2D1_UNIT_MODE_PIXELS)
    {
        WARN("Unknown unit mode %#x.\n", unit_mode);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_set_unit_mode(context->target.command_list, unit_mode);

    context->drawing_state.unitMode = unit_mode;
}

static D2D1_UNIT_MODE STDMETHODCALLTYPE d2d_device_context_GetUnitMode(ID2D1DeviceContext6 *iface)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p.\n", iface);

    return context->drawing_state.unitMode;
}

#define D2D_CONVOLVE_MATRIX_MAX_KERNEL_SIZE 1024

struct d2d_convolve_matrix_pass
{
    UINT32 kernel_size_x;
    UINT32 kernel_size_y;
    float kernel[D2D_CONVOLVE_MATRIX_MAX_KERNEL_SIZE];
    UINT32 kernel_size;
    float divisor;
    float bias;
};

static void d2d_device_context_draw_effect_bitmap(struct d2d_device_context *context, ID2D1Bitmap *bitmap,
        float opacity, D2D1_INTERPOLATION_MODE interpolation_mode, const D2D1_RECT_F *image_rect,
        const D2D1_POINT_2F *target_offset, D2D1_COMPOSITE_MODE composite_mode)
{
    if (composite_mode == D2D1_COMPOSITE_MODE_SOURCE_COPY)
    {
        ID3D11BlendState *prev_bs = context->bs;

        context->bs = NULL;

        d2d_device_context_draw_bitmap(context, bitmap, NULL, opacity,
                interpolation_mode, image_rect, target_offset, NULL);

        context->bs = prev_bs;
    }
    else
    {
        d2d_device_context_draw_bitmap(context, bitmap, NULL, opacity,
                interpolation_mode, image_rect, target_offset, NULL);
    }
}

static HRESULT d2d_effect_readback_bitmap(struct d2d_device_context *context,
        struct d2d_bitmap *bitmap, ID3D11Texture2D **texture, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11DeviceContext *d3d_context;
    ID3D11Texture2D *src_texture;
    HRESULT hr;

    if (FAILED(hr = ID3D11Resource_QueryInterface(bitmap->resource, &IID_ID3D11Texture2D, (void **)&src_texture)))
        return hr;

    ID3D11Texture2D_GetDesc(src_texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    if (FAILED(hr = ID3D11Device1_CreateTexture2D(context->d3d_device, &desc, NULL, texture)))
    {
        ID3D11Texture2D_Release(src_texture);
        return hr;
    }

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
    ID3D11DeviceContext_CopyResource(d3d_context, (ID3D11Resource *)*texture, bitmap->resource);
    hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)*texture, 0, D3D11_MAP_READ, 0, mapped);
    ID3D11DeviceContext_Release(d3d_context);
    ID3D11Texture2D_Release(src_texture);

    if (FAILED(hr))
    {
        ID3D11Texture2D_Release(*texture);
        *texture = NULL;
    }

    return hr;
}

static void d2d_effect_unmap_bitmap(struct d2d_device_context *context, ID3D11Texture2D *texture)
{
    ID3D11DeviceContext *d3d_context;

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
    ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)texture, 0);
    ID3D11DeviceContext_Release(d3d_context);
    ID3D11Texture2D_Release(texture);
}

static BYTE d2d_effect_clamp_byte(float value)
{
    if (value < 0.0f)
        return 0;
    if (value > 255.0f)
        return 255;
    return value + 0.5f;
}

static void d2d_convolve_matrix_apply_pass(const BYTE *src, BYTE *dst, UINT32 width, UINT32 height,
        const struct d2d_convolve_matrix_pass *pass)
{
    unsigned int x, y, kx, ky;
    float divisor = pass->divisor;

    if (divisor == 0.0f)
    {
        for (ky = 0; ky < pass->kernel_size; ++ky)
            divisor += pass->kernel[ky];
        if (divisor == 0.0f)
            divisor = 1.0f;
    }

    for (y = 0; y < height; ++y)
    {
        for (x = 0; x < width; ++x)
        {
            float sum = 0.0f;

            for (ky = 0; ky < pass->kernel_size_y; ++ky)
            {
                int sy = (int)y + (int)ky - (int)(pass->kernel_size_y / 2);

                /* SOFT border mode (the D2D default, which JUCE relies on): samples
                 * outside the input are transparent black (0), not clamped to the edge.
                 * Skipping the contribution lets the convolution fade to 0 at the image
                 * borders instead of smearing the edge alpha outward. */
                if (sy < 0 || sy >= (int)height)
                    continue;

                for (kx = 0; kx < pass->kernel_size_x; ++kx)
                {
                    int sx = (int)x + (int)kx - (int)(pass->kernel_size_x / 2);

                    if (sx < 0 || sx >= (int)width)
                        continue;

                    sum += src[sy * width + sx] * pass->kernel[ky * pass->kernel_size_x + kx];
                }
            }

            dst[y * width + x] = d2d_effect_clamp_byte(sum / divisor + pass->bias * 255.0f);
        }
    }
}

static HRESULT d2d_convolve_matrix_get_pass(ID2D1Effect *effect, struct d2d_convolve_matrix_pass *pass)
{
    UINT32 matrix_size;
    HRESULT hr;

    memset(pass, 0, sizeof(*pass));
    if (FAILED(hr = ID2D1Effect_GetValue(effect, D2D1_CONVOLVEMATRIX_PROP_KERNEL_SIZE_X,
            D2D1_PROPERTY_TYPE_UINT32, (BYTE *)&pass->kernel_size_x, sizeof(pass->kernel_size_x))))
        return hr;
    if (FAILED(hr = ID2D1Effect_GetValue(effect, D2D1_CONVOLVEMATRIX_PROP_KERNEL_SIZE_Y,
            D2D1_PROPERTY_TYPE_UINT32, (BYTE *)&pass->kernel_size_y, sizeof(pass->kernel_size_y))))
        return hr;

    if (!pass->kernel_size_x || !pass->kernel_size_y
            || pass->kernel_size_x > D2D_CONVOLVE_MATRIX_MAX_KERNEL_SIZE / pass->kernel_size_y)
        return E_INVALIDARG;

    pass->kernel_size = pass->kernel_size_x * pass->kernel_size_y;
    if (pass->kernel_size > D2D_CONVOLVE_MATRIX_MAX_KERNEL_SIZE)
        return E_INVALIDARG;

    matrix_size = ID2D1Effect_GetValueSize(effect, D2D1_CONVOLVEMATRIX_PROP_KERNEL_MATRIX);
    if (matrix_size < pass->kernel_size * sizeof(float) || matrix_size > sizeof(pass->kernel)
            || matrix_size % sizeof(float))
        return E_INVALIDARG;
    if (FAILED(hr = ID2D1Effect_GetValue(effect, D2D1_CONVOLVEMATRIX_PROP_KERNEL_MATRIX,
            D2D1_PROPERTY_TYPE_BLOB, (BYTE *)pass->kernel, matrix_size)))
        return hr;

    ID2D1Effect_GetValue(effect, D2D1_CONVOLVEMATRIX_PROP_DIVISOR,
            D2D1_PROPERTY_TYPE_FLOAT, (BYTE *)&pass->divisor, sizeof(pass->divisor));
    ID2D1Effect_GetValue(effect, D2D1_CONVOLVEMATRIX_PROP_BIAS,
            D2D1_PROPERTY_TYPE_FLOAT, (BYTE *)&pass->bias, sizeof(pass->bias));

    return S_OK;
}

/* Walk the ConvolveMatrix effect chain (JUCE's box-blur builds 2 x radius of them,
 * horizontal then vertical) into a dynamically grown pass array, so realistic blur
 * radii (radius > 8, i.e. more than the old fixed 16 passes) are no longer rejected
 * with E_NOTIMPL.  On success the caller owns *passes (free it) and a reference to
 * *bitmap (the chain's bitmap input); on failure everything is released here. */
static HRESULT d2d_convolve_matrix_collect_passes(ID2D1Effect *effect,
        struct d2d_convolve_matrix_pass **passes, unsigned int *pass_count, ID2D1Bitmap **bitmap)
{
    struct d2d_convolve_matrix_pass *array = NULL;
    unsigned int count = 0, capacity = 0;
    ID2D1Bitmap *input_bitmap = NULL;
    ID2D1Image *input;
    HRESULT hr = S_OK;

    ID2D1Effect_AddRef(effect);
    for (;;)
    {
        CLSID clsid;

        if (count == capacity)
        {
            struct d2d_convolve_matrix_pass *new_array;

            capacity = capacity ? capacity * 2 : 8;
            if (!(new_array = realloc(array, capacity * sizeof(*array))))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            array = new_array;
        }
        if (FAILED(hr = d2d_convolve_matrix_get_pass(effect, &array[count])))
            break;
        ++count;

        ID2D1Effect_GetInput(effect, 0, &input);
        if (!input)
        {
            hr = E_INVALIDARG;
            break;
        }

        if (SUCCEEDED(ID2D1Image_QueryInterface(input, &IID_ID2D1Bitmap, (void **)&input_bitmap)))
        {
            ID2D1Image_Release(input);
            break;
        }

        ID2D1Effect_Release(effect);
        if (FAILED(hr = ID2D1Image_QueryInterface(input, &IID_ID2D1Effect, (void **)&effect)))
        {
            ID2D1Image_Release(input);
            effect = NULL;
            break;
        }
        ID2D1Image_Release(input);

        if (FAILED(hr = ID2D1Effect_GetValue(effect, D2D1_PROPERTY_CLSID, D2D1_PROPERTY_TYPE_CLSID,
                (BYTE *)&clsid, sizeof(clsid))))
            break;
        if (!IsEqualGUID(&clsid, &CLSID_D2D1ConvolveMatrix))
        {
            hr = E_NOTIMPL;
            break;
        }
    }
    if (effect)
        ID2D1Effect_Release(effect);

    if (FAILED(hr))
    {
        free(array);
        return hr;
    }

    *passes = array;
    *pass_count = count;
    *bitmap = input_bitmap;
    return S_OK;
}

static HRESULT d2d_device_context_draw_convolve_matrix(struct d2d_device_context *context, ID2D1Effect *effect,
        const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode, D2D1_COMPOSITE_MODE composite_mode)
{
    struct d2d_convolve_matrix_pass *passes;
    struct d2d_bitmap *src_bitmap, *result_impl;
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *staging = NULL;
    D2D1_SIZE_U size;
    D2D1_PIXEL_FORMAT format;
    ID2D1Bitmap *bitmap;
    BYTE *buffers[2];
    float dpi_x, dpi_y;
    unsigned int pass_count, i;
    UINT32 y;
    HRESULT hr;

    if (FAILED(hr = d2d_convolve_matrix_collect_passes(effect, &passes, &pass_count, &bitmap)))
        return hr;

    src_bitmap = unsafe_impl_from_ID2D1Bitmap(bitmap);
    size = src_bitmap->pixel_size;
    format = src_bitmap->format;
    dpi_x = src_bitmap->dpi_x;
    dpi_y = src_bitmap->dpi_y;
    if (!size.width || !size.height)
    {
        ID2D1Bitmap_Release(bitmap);
        free(passes);
        return E_INVALIDARG;
    }

    /* The pass loop below convolves single bytes, so the single channel format
     * JUCE's box blur uses is the only one it can consume. */
    if (format.format != DXGI_FORMAT_A8_UNORM)
    {
        FIXME("Unhandled ConvolveMatrix bitmap format %#x.\n", format.format);
        ID2D1Bitmap_Release(bitmap);
        free(passes);
        return E_NOTIMPL;
    }

    if (FAILED(hr = d2d_effect_readback_bitmap(context, src_bitmap, &staging, &mapped)))
    {
        ID2D1Bitmap_Release(bitmap);
        free(passes);
        return hr;
    }

    if (size.height && size.width > ~(size_t)0 / size.height)
    {
        d2d_effect_unmap_bitmap(context, staging);
        ID2D1Bitmap_Release(bitmap);
        free(passes);
        return E_OUTOFMEMORY;
    }

    if (!(buffers[0] = malloc(size.width * size.height)) || !(buffers[1] = malloc(size.width * size.height)))
    {
        d2d_effect_unmap_bitmap(context, staging);
        ID2D1Bitmap_Release(bitmap);
        free(buffers[0]);
        free(passes);
        return E_OUTOFMEMORY;
    }

    for (y = 0; y < size.height; ++y)
        memcpy(buffers[0] + y * size.width, (BYTE *)mapped.pData + y * mapped.RowPitch, size.width);

    d2d_effect_unmap_bitmap(context, staging);
    ID2D1Bitmap_Release(bitmap);

    i = pass_count - 1;
    for (;;)
    {
        d2d_convolve_matrix_apply_pass(buffers[0], buffers[1], size.width, size.height, &passes[i]);
        memcpy(buffers[0], buffers[1], size.width * size.height);
        if (!i)
            break;
        --i;
    }
    free(passes);

    bitmap_desc.pixelFormat = format;
    bitmap_desc.dpiX = dpi_x;
    bitmap_desc.dpiY = dpi_y;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    bitmap_desc.colorContext = NULL;

    hr = d2d_bitmap_create(context, size, buffers[0], size.width, &bitmap_desc, &result_impl);
    free(buffers[0]);
    free(buffers[1]);
    if (FAILED(hr))
        return hr;

    d2d_device_context_draw_effect_bitmap(context, (ID2D1Bitmap *)&result_impl->ID2D1Bitmap1_iface,
            1.0f, interpolation_mode, image_rect, target_offset, composite_mode);
    ID2D1Bitmap1_Release(&result_impl->ID2D1Bitmap1_iface);

    return S_OK;
}

/* GaussianBlur is evaluated on the CPU for the same reason ConvolveMatrix above
 * is: the effect runs on a bitmap the application hands us, a handful of times
 * per paint rather than per primitive, so a readback costs less than a
 * dedicated shader plus the intermediate render targets a separable GPU blur
 * would need. */
#define D2D_GAUSSIAN_BLUR_MAX_SIGMA 100.0f

/* Only the 8 bits per channel formats an application is going to blur. The
 * samples are treated as opaque bytes, so BGRA and RGBA differ merely in which
 * channel is which and both work unchanged. */
static unsigned int d2d_effect_bitmap_channels(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 4;
        default:
            return 0;
    }
}

/* D2D derives the kernel extent from the standard deviation. Three sigma covers
 * 99.7% of the distribution and is also the amount by which the effect inflates
 * its own output rectangle. */
static unsigned int d2d_gaussian_blur_kernel_radius(float sigma)
{
    return ceilf(3.0f * sigma);
}

static float *d2d_gaussian_blur_build_kernel(float sigma, unsigned int radius)
{
    unsigned int i, size = 2 * radius + 1;
    float *kernel, sum = 0.0f, denominator;

    if (!(kernel = malloc(size * sizeof(*kernel))))
        return NULL;

    denominator = 2.0f * sigma * sigma;
    for (i = 0; i < size; ++i)
    {
        float d = (float)i - (float)radius;

        kernel[i] = expf(-(d * d) / denominator);
        sum += kernel[i];
    }
    for (i = 0; i < size; ++i)
        kernel[i] /= sum;

    return kernel;
}

/* One pass of the separable convolution over interleaved 8 bit channels.
 * Samples outside the input count as transparent black, so the blur fades out
 * at the image border instead of smearing the edge pixels outward - the same
 * convention the ConvolveMatrix path uses. Blurring the channels independently
 * is what premultiplied alpha is for. */
static void d2d_gaussian_blur_apply_pass(const BYTE *src, BYTE *dst, unsigned int width,
        unsigned int height, unsigned int channels, const float *kernel, unsigned int radius,
        BOOL horizontal)
{
    unsigned int x, y, c, i, limit = horizontal ? width : height;
    int stride = (horizontal ? 1 : (int)width) * (int)channels;

    for (y = 0; y < height; ++y)
    {
        for (x = 0; x < width; ++x)
        {
            const BYTE *centre = &src[(y * width + x) * channels];
            BYTE *out = &dst[(y * width + x) * channels];
            int pos = horizontal ? (int)x : (int)y;
            float sum[4] = {0.0f};

            for (i = 0; i <= 2 * radius; ++i)
            {
                int offset = (int)i - (int)radius;
                const BYTE *sample;

                if (pos + offset < 0 || pos + offset >= (int)limit)
                    continue;

                sample = centre + offset * stride;
                for (c = 0; c < channels; ++c)
                    sum[c] += sample[c] * kernel[i];
            }

            for (c = 0; c < channels; ++c)
                out[c] = d2d_effect_clamp_byte(sum[c]);
        }
    }
}

static HRESULT d2d_device_context_draw_gaussian_blur(struct d2d_device_context *context, ID2D1Effect *effect,
        const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode, D2D1_COMPOSITE_MODE composite_mode)
{
    D2D1_GAUSSIANBLUR_OPTIMIZATION optimization = D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED;
    D2D1_BORDER_MODE border_mode = D2D1_BORDER_MODE_SOFT;
    struct d2d_bitmap *src_bitmap, *result_impl;
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *staging = NULL;
    unsigned int channels, radius;
    ID2D1Bitmap *bitmap = NULL;
    BYTE *buffers[2] = {NULL};
    float sigma = 3.0f;
    float *kernel = NULL;
    D2D1_PIXEL_FORMAT format;
    float dpi_x, dpi_y;
    ID2D1Image *input;
    D2D1_SIZE_U size;
    size_t pitch;
    HRESULT hr;
    UINT32 y;

    ID2D1Effect_GetValue(effect, D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
            D2D1_PROPERTY_TYPE_FLOAT, (BYTE *)&sigma, sizeof(sigma));
    ID2D1Effect_GetValue(effect, D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
            D2D1_PROPERTY_TYPE_ENUM, (BYTE *)&optimization, sizeof(optimization));
    ID2D1Effect_GetValue(effect, D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
            D2D1_PROPERTY_TYPE_ENUM, (BYTE *)&border_mode, sizeof(border_mode));

    ID2D1Effect_GetInput(effect, 0, &input);
    if (!input)
        return E_INVALIDARG;
    hr = ID2D1Image_QueryInterface(input, &IID_ID2D1Bitmap, (void **)&bitmap);
    if (FAILED(hr))
        FIXME("Unhandled GaussianBlur input %p, only bitmaps are supported.\n", input);
    ID2D1Image_Release(input);
    if (FAILED(hr))
        return E_NOTIMPL;

    if (!(src_bitmap = unsafe_impl_from_ID2D1Bitmap(bitmap)))
    {
        ID2D1Bitmap_Release(bitmap);
        return E_INVALIDARG;
    }

    size = src_bitmap->pixel_size;
    format = src_bitmap->format;
    dpi_x = src_bitmap->dpi_x;
    dpi_y = src_bitmap->dpi_y;

    if (!size.width || !size.height)
    {
        ID2D1Bitmap_Release(bitmap);
        return E_INVALIDARG;
    }

    if (!(channels = d2d_effect_bitmap_channels(format.format)))
    {
        FIXME("Unhandled GaussianBlur bitmap format %#x.\n", format.format);
        ID2D1Bitmap_Release(bitmap);
        return E_NOTIMPL;
    }

    /* Three deliberate simplifications, reported once per process rather than
     * per paint. The output rectangle is not inflated by the blur extent, so
     * the halo D2D1_BORDER_MODE_SOFT would let spill past the input rectangle
     * is cut off - which is what an application blurring an image in place
     * expects, and what the ConvolveMatrix path already does. The optimization
     * property only picks between approximations of the same curve, and the
     * full kernel is always evaluated. Straight alpha would have to be
     * premultiplied before the channels may be blurred separately. */
    if (border_mode != D2D1_BORDER_MODE_HARD || optimization != D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY
            || (channels > 1 && format.alphaMode == D2D1_ALPHA_MODE_STRAIGHT))
    {
        static BOOL warned;

        if (!warned)
        {
            warned = TRUE;
            WARN("Approximating GaussianBlur: border mode %#x, optimization %#x, alpha mode %#x.\n",
                    border_mode, optimization, format.alphaMode);
        }
    }

    if (!(sigma > 0.0f))
        sigma = 0.0f;
    else if (sigma > D2D_GAUSSIAN_BLUR_MAX_SIGMA)
        sigma = D2D_GAUSSIAN_BLUR_MAX_SIGMA;

    /* A standard deviation of zero passes the input through unchanged. */
    if (!(radius = d2d_gaussian_blur_kernel_radius(sigma)))
    {
        d2d_device_context_draw_effect_bitmap(context, bitmap, 1.0f, interpolation_mode,
                image_rect, target_offset, composite_mode);
        ID2D1Bitmap_Release(bitmap);
        return S_OK;
    }

    if (FAILED(hr = d2d_effect_readback_bitmap(context, src_bitmap, &staging, &mapped)))
    {
        ID2D1Bitmap_Release(bitmap);
        return hr;
    }

    pitch = (size_t)size.width * channels;
    if (size.height > ~(size_t)0 / pitch)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    if (!(buffers[0] = malloc(pitch * size.height)) || !(buffers[1] = malloc(pitch * size.height))
            || !(kernel = d2d_gaussian_blur_build_kernel(sigma, radius)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    for (y = 0; y < size.height; ++y)
        memcpy(buffers[0] + y * pitch, (BYTE *)mapped.pData + y * mapped.RowPitch, pitch);

    d2d_effect_unmap_bitmap(context, staging);
    staging = NULL;

    d2d_gaussian_blur_apply_pass(buffers[0], buffers[1], size.width, size.height,
            channels, kernel, radius, TRUE);
    d2d_gaussian_blur_apply_pass(buffers[1], buffers[0], size.width, size.height,
            channels, kernel, radius, FALSE);

    bitmap_desc.pixelFormat = format;
    bitmap_desc.dpiX = dpi_x;
    bitmap_desc.dpiY = dpi_y;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    bitmap_desc.colorContext = NULL;

    if (SUCCEEDED(hr = d2d_bitmap_create(context, size, buffers[0], (UINT32)pitch, &bitmap_desc, &result_impl)))
    {
        d2d_device_context_draw_effect_bitmap(context, (ID2D1Bitmap *)&result_impl->ID2D1Bitmap1_iface,
                1.0f, interpolation_mode, image_rect, target_offset, composite_mode);
        ID2D1Bitmap1_Release(&result_impl->ID2D1Bitmap1_iface);
    }

done:
    if (staging)
        d2d_effect_unmap_bitmap(context, staging);
    ID2D1Bitmap_Release(bitmap);
    free(kernel);
    free(buffers[0]);
    free(buffers[1]);

    return hr;
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_DrawGlyphRun(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION *glyph_run_desc, ID2D1Brush *brush, DWRITE_MEASURING_MODE measuring_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, baseline_origin %s, glyph_run %p, glyph_run_desc %p, brush %p, measuring_mode %#x.\n",
            iface, debug_d2d_point_2f(&baseline_origin), glyph_run, glyph_run_desc, brush, measuring_mode);

    d2d_device_context_flush_lines(context);

    d2d_device_context_draw_glyph_run(context, baseline_origin, glyph_run, glyph_run_desc, brush, measuring_mode);
}

static BOOL d2d_format_is_float(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return TRUE;
        default:
            return FALSE;
    }
}

static BOOL d2d_format_is_srgb(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return TRUE;
        default:
            return FALSE;
    }
}

/* D2D treats floating point buffers as scRGB (linear) and 8 bits per channel
 * unsigned normalised buffers as sRGB. That is the colour space a surface lives
 * in when nothing says otherwise. */
static D2D1_COLOR_SPACE d2d_color_space_from_format(DXGI_FORMAT format)
{
    return d2d_format_is_float(format) ? D2D1_COLOR_SPACE_SCRGB : D2D1_COLOR_SPACE_SRGB;
}

/* The ColorManagement effect converts its input from the source colour space to
 * the destination colour space. Applications describe those spaces with colour
 * contexts; where they leave one unset the space is derived from the pixel
 * format instead. A linear source drawn to a plain UNORM target is the one case
 * that needs the sRGB transfer function applied — everything else keeps passing
 * the source through unconverted. Targets that already carry an _SRGB format
 * are excluded because the hardware encodes those on write, and float targets
 * because they stay linear.
 *
 * The encoding happens in the shape pixel shader; the effect is drawn hundreds
 * of times per frame, which rules out a CPU side conversion like the one
 * ConvolveMatrix uses. */
static void d2d_device_context_draw_color_management(struct d2d_device_context *context, ID2D1Effect *effect,
        const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode, D2D1_COMPOSITE_MODE composite_mode)
{
    D2D1_COLOR_SPACE source_space = D2D1_COLOR_SPACE_CUSTOM;
    D2D1_COLOR_SPACE dest_space = D2D1_COLOR_SPACE_CUSTOM;
    struct d2d_bitmap *bitmap_impl;
    ID2D1Bitmap *bitmap;
    ID2D1Image *input;
    BOOL prev_encode;

    ID2D1Effect_GetInput(effect, 0, &input);
    if (!input)
        return;

    d2d_effect_get_color_management_spaces(effect, &source_space, &dest_space);

    if (SUCCEEDED(ID2D1Image_QueryInterface(input, &IID_ID2D1Bitmap, (void **)&bitmap)))
    {
        prev_encode = context->srgb_encode;
        if ((bitmap_impl = unsafe_impl_from_ID2D1Bitmap(bitmap))
                && context->target.type == D2D_TARGET_BITMAP && context->target.bitmap)
        {
            DXGI_FORMAT target_format = context->target.bitmap->format.format;

            if (source_space == D2D1_COLOR_SPACE_CUSTOM)
                source_space = d2d_color_space_from_format(bitmap_impl->format.format);
            if (dest_space == D2D1_COLOR_SPACE_CUSTOM)
                dest_space = d2d_color_space_from_format(target_format);

            context->srgb_encode = source_space == D2D1_COLOR_SPACE_SCRGB
                    && dest_space == D2D1_COLOR_SPACE_SRGB
                    && !d2d_format_is_float(target_format) && !d2d_format_is_srgb(target_format);
        }

        d2d_device_context_draw_effect_bitmap(context, bitmap, 1.0f,
                interpolation_mode, image_rect, target_offset, composite_mode);

        context->srgb_encode = prev_encode;
        ID2D1Bitmap_Release(bitmap);
    }

    ID2D1Image_Release(input);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawImage(ID2D1DeviceContext6 *iface, ID2D1Image *image,
        const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect, D2D1_INTERPOLATION_MODE interpolation_mode,
        D2D1_COMPOSITE_MODE composite_mode)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    ID2D1Bitmap *bitmap;

    TRACE("iface %p, image %p, target_offset %s, image_rect %s, interpolation_mode %#x, composite_mode %#x.\n",
            iface, image, debug_d2d_point_2f(target_offset), debug_d2d_rect_f(image_rect),
            interpolation_mode, composite_mode);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_image(context->target.command_list, image, target_offset, image_rect,
                interpolation_mode, composite_mode);
        return;
    }

    if (composite_mode != D2D1_COMPOSITE_MODE_SOURCE_OVER
            && composite_mode != D2D1_COMPOSITE_MODE_SOURCE_COPY)
        FIXME("Unhandled composite mode %#x.\n", composite_mode);

    if (SUCCEEDED(ID2D1Image_QueryInterface(image, &IID_ID2D1Bitmap, (void **)&bitmap)))
    {
        d2d_device_context_draw_bitmap(context, bitmap, NULL, 1.0f, interpolation_mode, image_rect, target_offset, NULL);

        ID2D1Bitmap_Release(bitmap);
        return;
    }

    /* If the image is an effect output, dispatch basic effect processing by
     * CLSID. SOURCE_COPY draws with blending disabled so the processed pixels
     * replace the render target directly. */
    {
        ID2D1Effect *effect;

        if (SUCCEEDED(ID2D1Image_QueryInterface(image, &IID_ID2D1Effect, (void **)&effect)))
        {
            CLSID clsid;

            if (SUCCEEDED(ID2D1Effect_GetValue(effect, D2D1_PROPERTY_CLSID, D2D1_PROPERTY_TYPE_CLSID,
                    (BYTE *)&clsid, sizeof(clsid))))
            {
                if (IsEqualGUID(&clsid, &CLSID_D2D1Opacity))
                {
                    ID2D1Image *input = NULL;
                    float opacity = 1.0f;

                    ID2D1Effect_GetValue(effect, D2D1_OPACITY_PROP_OPACITY, D2D1_PROPERTY_TYPE_FLOAT,
                            (BYTE *)&opacity, sizeof(opacity));

                    ID2D1Effect_GetInput(effect, 0, &input);
                    if (input)
                    {
                        if (SUCCEEDED(ID2D1Image_QueryInterface(input, &IID_ID2D1Bitmap, (void **)&bitmap)))
                        {
                            d2d_device_context_draw_effect_bitmap(context, bitmap, opacity,
                                    interpolation_mode, image_rect, target_offset, composite_mode);
                            ID2D1Bitmap_Release(bitmap);
                        }
                        ID2D1Image_Release(input);
                    }
                }
                else if (IsEqualGUID(&clsid, &CLSID_D2D1ConvolveMatrix))
                {
                    HRESULT hr;

                    if (FAILED(hr = d2d_device_context_draw_convolve_matrix(context, effect,
                            target_offset, image_rect, interpolation_mode, composite_mode)))
                    {
                        FIXME("Failed to draw ConvolveMatrix effect, hr %#lx.\n", hr);
                    }
                }
                else if (IsEqualGUID(&clsid, &CLSID_D2D1GaussianBlur))
                {
                    HRESULT hr;

                    if (FAILED(hr = d2d_device_context_draw_gaussian_blur(context, effect,
                            target_offset, image_rect, interpolation_mode, composite_mode)))
                    {
                        FIXME("Failed to draw GaussianBlur effect, hr %#lx.\n", hr);
                    }
                }
                else if (IsEqualGUID(&clsid, &CLSID_D2D1ColorManagement))
                {
                    d2d_device_context_draw_color_management(context, effect,
                            target_offset, image_rect, interpolation_mode, composite_mode);
                }
                else
                {
                    FIXME("Unhandled effect %s.\n", debugstr_guid(&clsid));
                }
            }
            ID2D1Effect_Release(effect);
            return;
        }
    }

    FIXME("Unhandled image %p.\n", image);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawGdiMetafile(ID2D1DeviceContext6 *iface,
        ID2D1GdiMetafile *metafile, const D2D1_POINT_2F *target_offset)
{
    FIXME("iface %p, metafile %p, target_offset %s stub!\n",
            iface, metafile, debug_d2d_point_2f(target_offset));
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_DrawBitmap(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *bitmap, const D2D1_RECT_F *dst_rect, float opacity, D2D1_INTERPOLATION_MODE interpolation_mode,
        const D2D1_RECT_F *src_rect, const D2D1_MATRIX_4X4_F *perspective_transform)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, bitmap %p, dst_rect %s, opacity %.8e, interpolation_mode %#x, "
            "src_rect %s, perspective_transform %p.\n",
            iface, bitmap, debug_d2d_rect_f(dst_rect), opacity, interpolation_mode,
            debug_d2d_rect_f(src_rect), perspective_transform);

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_draw_bitmap(context->target.command_list, bitmap, dst_rect, opacity, interpolation_mode,
                src_rect, perspective_transform);
    }
    else
    {
        d2d_device_context_draw_bitmap(context, bitmap, dst_rect, opacity, interpolation_mode, src_rect,
                NULL, perspective_transform);
    }
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_PushLayer(ID2D1DeviceContext6 *iface,
        const D2D1_LAYER_PARAMETERS1 *layer_parameters, ID2D1Layer *layer)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, layer_parameters %p, layer %p.\n", iface, layer_parameters, layer);

    d2d_device_context_flush_lines(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_push_layer(context->target.command_list, context, layer_parameters);

    if (context->target.type == D2D_TARGET_BITMAP)
    {
        D2D1_BITMAP_PROPERTIES1 bitmap_desc;
        D2D1_RECT_F transformed_rect;
        struct d2d_layer_info info;
        struct d2d_bitmap *layer_bitmap;
        float x_scale, y_scale;
        D2D1_POINT_2F point;
        HRESULT hr;

        /* Track contexts that use element-layer rendering (PushLayer with mask=NULL). */
        if (!layer_parameters->geometricMask)
            context->has_element_layers = TRUE;

        /* Element-layer bypass: PushLayer with no mask, opacity=1.0 and no
         * opacity brush is a no-op on Windows D2D1. Skip layer_bitmap creation
         * and render directly on the current target. Without this, element layers
         * render into a transparent layer_bitmap and composite with Over, causing
         * black rectangles from the pre-clear Source Copy in PopLayer. */
        if (!layer_parameters->geometricMask
                && layer_parameters->opacity >= 1.0f
                && !layer_parameters->opacityBrush)
        {
            struct d2d_layer_info info;
            memset(&info, 0, sizeof(info));
            info.opacity = 1.0f;
            info.bypass_layer = TRUE;
            info.clip_push_count = 0; /* element-layer bypass pushes no clip */

            if (!d2d_layer_stack_push(&context->layer_stack, &info))
                WARN("Failed to push layer.\n");
            return;
        }

        /* Transform contentBounds to device coordinates for scissor clip. */
        x_scale = context->desc.dpiX / 96.0f;
        y_scale = context->desc.dpiY / 96.0f;
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.left * x_scale,
                layer_parameters->contentBounds.top * y_scale);
        d2d_rect_set(&transformed_rect, point.x, point.y, point.x, point.y);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.left * x_scale,
                layer_parameters->contentBounds.bottom * y_scale);
        d2d_rect_expand(&transformed_rect, &point);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.right * x_scale,
                layer_parameters->contentBounds.top * y_scale);
        d2d_rect_expand(&transformed_rect, &point);
        d2d_point_transform(&point, &context->drawing_state.transform,
                layer_parameters->contentBounds.right * x_scale,
                layer_parameters->contentBounds.bottom * y_scale);
        d2d_rect_expand(&transformed_rect, &point);

        if (!d2d_clip_stack_push(&context->clip_stack, &transformed_rect))
            WARN("Failed to push clip rect.\n");

        /* Clip-only bypass for non-element-layer contexts (e.g. popup menus).
         * Render directly on backbuffer with stencil-based geometry clipping. */
        if (layer_parameters->geometricMask && !context->has_element_layers
                && layer_parameters->opacity >= 1.0f && !layer_parameters->opacityBrush
                && layer_parameters->maskAntialiasMode != D2D1_ANTIALIAS_MODE_PER_PRIMITIVE)
        {
            D2D1_RECT_F mask_clip;

            ID2D1Geometry_GetBounds(layer_parameters->geometricMask, NULL, &mask_clip);

            if (!d2d_clip_stack_push(&context->clip_stack, &mask_clip))
                WARN("Failed to push mask clip rect.\n");

            /* Stencil-based clipping with nesting support (same as PushLayer path above). */
            if (SUCCEEDED(d2d_device_context_ensure_stencil(context)))
            {
                ID3D11DeviceContext *d3d_context;

                ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);

                if (context->stencil_depth == 0)
                    ID3D11DeviceContext_ClearDepthStencilView(d3d_context,
                            context->stencil_dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);

                context->stencil_depth++;
                context->stencil_writing = TRUE;

                d2d_device_context_render_mask_to_stencil(context, layer_parameters->geometricMask);

                context->stencil_writing = FALSE;
                ID3D11DeviceContext_Release(d3d_context);
            }

            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.bypass_layer = TRUE;
            info.clip_push_count = 2; /* transformed_rect + mask BBox clip */
            info.stencil_geometry = layer_parameters->geometricMask;
            if (info.stencil_geometry)
                ID2D1Geometry_AddRef(info.stencil_geometry);

            if (!d2d_layer_stack_push(&context->layer_stack, &info))
                WARN("Failed to push layer.\n");
            return;
        }

        /* Create a temporary render target for layer content. */
        memset(&bitmap_desc, 0, sizeof(bitmap_desc));
        bitmap_desc.pixelFormat = context->desc.pixelFormat;
        bitmap_desc.dpiX = context->desc.dpiX;
        bitmap_desc.dpiY = context->desc.dpiY;
        bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

        layer_bitmap = NULL;
        hr = d2d_bitmap_create(context, context->pixel_size, NULL, 0, &bitmap_desc, &layer_bitmap);
        if (SUCCEEDED(hr) && layer_bitmap)
        {
            ID3D11DeviceContext *d3d_context;
            static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            /* Save current state. */
            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.mask_aa_mode = layer_parameters->maskAntialiasMode;
            info.prev_target = context->target.bitmap;
            ID2D1Bitmap1_AddRef(&info.prev_target->ID2D1Bitmap1_iface);
            info.prev_pixel_size = context->pixel_size;
            info.prev_bs = context->bs;
            if (context->bs)
                ID3D11BlendState_AddRef(context->bs);
            info.layer_bitmap = layer_bitmap;
            info.clip_push_count = 1; /* transformed_rect clip */

            /* Save geometric mask for compositing in PopLayer.
             * Skip only simple full-frame rectangles (face_count <= 2).
             * Complex geometries (rounded rects, paths) must always be
             * applied as masks even with full-frame bounding box. */
            if (layer_parameters->geometricMask)
            {
                D2D1_RECT_F mb;
                const struct d2d_geometry *mask_geo;

                ID2D1Geometry_GetBounds(layer_parameters->geometricMask, NULL, &mb);
                mask_geo = unsafe_impl_from_ID2D1Geometry(layer_parameters->geometricMask);

                if (mb.left <= 1.0f && mb.top <= 1.0f
                        && mb.right >= context->pixel_size.width - 1
                        && mb.bottom >= context->pixel_size.height - 1
                        && mask_geo->fill.face_count <= 2)
                {
                    /* Full-frame simple rectangle — no clipping needed. */
                }
                else
                {
                    info.mask_geometry = layer_parameters->geometricMask;
                    ID2D1Geometry_AddRef(info.mask_geometry);
                    info.mask_transform = layer_parameters->maskTransform;
                }
            }

            /* Save opacity brush for per-pixel masking in PopLayer. */
            if (layer_parameters->opacityBrush)
            {
                info.opacity_brush = layer_parameters->opacityBrush;
                ID2D1Brush_AddRef(info.opacity_brush);
            }

            /* Initialize layer bitmap: copy background or clear to transparent. */
            ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
            if (layer_parameters->layerOptions & D2D1_LAYER_OPTIONS1_INITIALIZE_FROM_BACKGROUND)
            {
                TRACE("Layer initialized from background (options %#x).\n",
                        layer_parameters->layerOptions);
                ID3D11DeviceContext_CopyResource(d3d_context,
                        layer_bitmap->resource, context->target.bitmap->resource);
            }
            else
            {
                ID3D11DeviceContext_ClearRenderTargetView(d3d_context, layer_bitmap->rtv, transparent);
            }
            ID3D11DeviceContext_Release(d3d_context);

            if (layer_parameters->layerOptions & D2D1_LAYER_OPTIONS1_IGNORE_ALPHA)
                info.ignore_alpha = TRUE;

            /* Switch render target to the layer bitmap. */
            context->target.bitmap = layer_bitmap;
        }
        else
        {
            WARN("Failed to create layer bitmap, hr %#lx. Rendering without layer.\n", hr);
            memset(&info, 0, sizeof(info));
            info.opacity = layer_parameters->opacity;
            info.clip_push_count = 1; /* transformed_rect clip pushed before bitmap creation */
        }

        if (!d2d_layer_stack_push(&context->layer_stack, &info))
            WARN("Failed to push layer.\n");
    }
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_InvalidateEffectInputRectangle(ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, UINT32 input, const D2D1_RECT_F *input_rect)
{
    FIXME("iface %p, effect %p, input %u, input_rect %s stub!\n",
            iface, effect, input, debug_d2d_rect_f(input_rect));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetEffectInvalidRectangleCount(ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, UINT32 *rect_count)
{
    FIXME("iface %p, effect %p, rect_count %p stub!\n", iface, effect, rect_count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetEffectInvalidRectangles(ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, D2D1_RECT_F *rectangles, UINT32 rect_count)
{
    FIXME("iface %p, effect %p, rectangles %p, rect_count %u stub!\n", iface, effect, rectangles, rect_count);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetEffectRequiredInputRectangles(ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, const D2D1_RECT_F *image_rect, const D2D1_EFFECT_INPUT_DESCRIPTION *desc,
        D2D1_RECT_F *input_rect, UINT32 input_count)
{
    FIXME("iface %p, effect %p, image_rect %s, desc %p, input_rect %p, input_count %u stub!\n",
            iface, effect, debug_d2d_rect_f(image_rect), desc, input_rect, input_count);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext_FillOpacityMask(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *mask, ID2D1Brush *brush, const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    FIXME("iface %p, mask %p, brush %p, dst_rect %s, src_rect %s stub!\n",
            iface, mask, brush, debug_d2d_rect_f(dst_rect), debug_d2d_rect_f(src_rect));

    d2d_device_context_flush_lines(context);

    if (FAILED(context->error.code))
        return;

    if (context->drawing_state.antialiasMode != D2D1_ANTIALIAS_MODE_ALIASED)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_fill_opacity_mask(context->target.command_list, context, mask, brush, dst_rect, src_rect);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateFilledGeometryRealization(ID2D1DeviceContext6 *iface,
        ID2D1Geometry *geometry, float tolerance, ID2D1GeometryRealization **realization)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_geometry_realization *object;
    HRESULT hr;

    TRACE("iface %p, geometry %p, tolerance %.8e, realization %p.\n", iface, geometry, tolerance,
            realization);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d2d_geometry_realization_init(object, context->factory, geometry)))
    {
        WARN("Failed to initialise geometry realization, hr %#lx.\n", hr);
        free(object);
        return hr;
    }
    object->filled = true;

    TRACE("Created geometry realization %p.\n", object);
    *realization = &object->ID2D1GeometryRealization_iface;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateStrokedGeometryRealization(
        ID2D1DeviceContext6 *iface, ID2D1Geometry *geometry, float tolerance, float stroke_width,
        ID2D1StrokeStyle *stroke_style, ID2D1GeometryRealization **realization)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_geometry_realization *object;
    HRESULT hr;

    TRACE("iface %p, geometry %p, tolerance %.8e, stroke_width %.8e, stroke_style %p, realization %p.\n",
            iface, geometry, tolerance, stroke_width, stroke_style, realization);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d2d_geometry_realization_init(object, context->factory, geometry)))
    {
        WARN("Failed to initialise geometry realization, hr %#lx.\n", hr);
        free(object);
        return hr;
    }
    object->stroke_width = stroke_width;
    object->stroke_style = stroke_style;
    if (object->stroke_style)
        ID2D1StrokeStyle_AddRef(object->stroke_style);

    TRACE("Created geometry realization %p.\n", object);
    *realization = &object->ID2D1GeometryRealization_iface;

    return S_OK;
}

static void STDMETHODCALLTYPE d2d_device_context_DrawGeometryRealization(ID2D1DeviceContext6 *iface,
        ID2D1GeometryRealization *realization, ID2D1Brush *brush)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_geometry_realization *r = unsafe_impl_from_ID2D1GeometryRealization(realization);

    FIXME("iface %p, realization %p, brush %p semi-stub!\n", iface, realization, brush);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        if (r->filled)
        {
            d2d_command_list_fill_geometry(context->target.command_list, context, r->geometry, brush, NULL);
        }
        else
        {
            d2d_command_list_draw_geometry(context->target.command_list, context, r->geometry, brush,
                    r->stroke_width, r->stroke_style);
        }
        return;
    }
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateInk(ID2D1DeviceContext6 *iface,
        const D2D1_INK_POINT *start_point, ID2D1Ink **ink)
{
    FIXME("iface %p, start_point %p, ink %p stub!\n", iface, start_point, ink);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateInkStyle(ID2D1DeviceContext6 *iface,
        const D2D1_INK_STYLE_PROPERTIES *ink_style_properties, ID2D1InkStyle **ink_style)
{
    FIXME("iface %p, ink_style_properties %p, ink_style %p stub!\n", iface, ink_style_properties, ink_style);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateGradientMesh(ID2D1DeviceContext6 *iface,
        const D2D1_GRADIENT_MESH_PATCH *patches, UINT32 patches_count,
        ID2D1GradientMesh **gradient_mesh)
{
    FIXME("iface %p, patches %p, patches_count %u, gradient_mesh %p stub!\n", iface, patches,
            patches_count, gradient_mesh);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateImageSourceFromWic(ID2D1DeviceContext6 *iface,
        IWICBitmapSource *wic_bitmap_source, D2D1_IMAGE_SOURCE_LOADING_OPTIONS loading_options,
        D2D1_ALPHA_MODE alpha_mode, ID2D1ImageSourceFromWic **image_source)
{
    FIXME("iface %p, wic_bitmap_source %p, loading_options %#x, alpha_mode %u, image_source %p stub!\n",
            iface, wic_bitmap_source, loading_options, alpha_mode, image_source);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateLookupTable3D(ID2D1DeviceContext6 *iface,
        D2D1_BUFFER_PRECISION precision, const UINT32 *extents, const BYTE *data,
        UINT32 data_count, const UINT32 *strides, ID2D1LookupTable3D **lookup_table)
{
    FIXME("iface %p, precision %u, extents %p, data %p, data_count %u, strides %p, lookup_table %p stub!\n",
            iface, precision, extents, data, data_count, strides, lookup_table);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateImageSourceFromDxgi(ID2D1DeviceContext6 *iface,
        IDXGISurface **surfaces, UINT32 surface_count, DXGI_COLOR_SPACE_TYPE color_space,
        D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS options, ID2D1ImageSource **image_source)
{
    FIXME("iface %p, surfaces %p, surface_count %u, color_space %u, options %#x, image_source %p stub!\n",
            iface, surfaces, surface_count, color_space, options, image_source);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetGradientMeshWorldBounds(ID2D1DeviceContext6 *iface,
        ID2D1GradientMesh *gradient_mesh, D2D1_RECT_F *bounds)
{
    FIXME("iface %p, gradient_mesh %p, bounds %p stub!\n", iface, gradient_mesh, bounds);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_device_context_DrawInk(ID2D1DeviceContext6 *iface, ID2D1Ink *ink,
        ID2D1Brush *brush, ID2D1InkStyle *ink_style)
{
    FIXME("iface %p, ink %p, brush %p, ink_style %p stub!\n", iface, ink, brush, ink_style);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawGradientMesh(ID2D1DeviceContext6 *iface,
        ID2D1GradientMesh *gradient_mesh)
{
    FIXME("iface %p, gradient_mesh %p stub!\n", iface, gradient_mesh);
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext2_DrawGdiMetafile(
        ID2D1DeviceContext6 *iface, ID2D1GdiMetafile *gdi_metafile, const D2D1_RECT_F *dst_rect,
        const D2D1_RECT_F *src_rect)
{
    FIXME("iface %p, gdi_metafile %p, dst_rect %s, src_rect %s stub!\n", iface, gdi_metafile,
            debug_d2d_rect_f(dst_rect), debug_d2d_rect_f(src_rect));
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateTransformedImageSource(ID2D1DeviceContext6 *iface,
        ID2D1ImageSource *source, const D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES *props,
        ID2D1TransformedImageSource **transformed)
{
    FIXME("iface %p, source %p, props %p, transformed %p stub!\n", iface, source, props, transformed);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateSpriteBatch(ID2D1DeviceContext6 *iface,
        ID2D1SpriteBatch **sprite_batch)
{
    FIXME("iface %p, sprite_batch %p stub!\n", iface, sprite_batch);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_device_context_DrawSpriteBatch(ID2D1DeviceContext6 *iface,
        ID2D1SpriteBatch *sprite_batch, UINT32 start_index, UINT32 sprite_count, ID2D1Bitmap *bitmap,
        D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode, D2D1_SPRITE_OPTIONS sprite_options)
{
    FIXME("iface %p, sprite_batch %p, start_index %u, sprite_count %u, bitmap %p, interpolation_mode %u,"
            "sprite_options %u stub!\n", iface, sprite_batch, start_index, sprite_count, bitmap,
            interpolation_mode, sprite_options);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateSvgGlyphStyle(ID2D1DeviceContext6 *iface,
        ID2D1SvgGlyphStyle **svg_glyph_style)
{
    FIXME("iface %p, svg_glyph_style %p stub!\n", iface, svg_glyph_style);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext4_DrawText(ID2D1DeviceContext6 *iface,
        const WCHAR *string, UINT32 string_length, IDWriteTextFormat *text_format, const D2D1_RECT_F *layout_rect,
        ID2D1Brush *default_fill_brush, ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index,
        D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring_mode)
{
    FIXME("iface %p, string %s, string_length %u, text_format %p, layout_rect %s, default_fill_brush %p,"
            "svg_glyph_style %p, color_palette_index %u, options %#x, measuring_mode %u stub!\n",
            iface, debugstr_wn(string, string_length), string_length, text_format, debug_d2d_rect_f(layout_rect),
            default_fill_brush, svg_glyph_style, color_palette_index, options, measuring_mode);
}

static void STDMETHODCALLTYPE d2d_device_context_ID2D1DeviceContext4_DrawTextLayout(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F origin, IDWriteTextLayout *text_layout, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index, D2D1_DRAW_TEXT_OPTIONS options)
{
    FIXME("iface %p, origin %s, text_layout %p, default_fill_brush %p, svg_glyph_style %p, color_palette_index %u,"
            "options %#x stub!\n", iface, debug_d2d_point_2f(&origin), text_layout, default_fill_brush,
            svg_glyph_style, color_palette_index, options);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawColorBitmapGlyphRun(ID2D1DeviceContext6 *iface,
        DWRITE_GLYPH_IMAGE_FORMATS glyph_image_format, D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        DWRITE_MEASURING_MODE measuring_mode, D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION bitmap_snap_option)
{
    FIXME("iface %p, glyph_image_format %#x, baseline_origin %s, glyph_run %p, measuring_mode %u, bitmap_snap_option %#x stub!\n",
            iface, glyph_image_format, debug_d2d_point_2f(&baseline_origin), glyph_run, measuring_mode, bitmap_snap_option);
}

static void STDMETHODCALLTYPE d2d_device_context_DrawSvgGlyphRun(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index, DWRITE_MEASURING_MODE measuring_mode)
{
    FIXME("iface %p, baseline_origin %s, glyph_run %p, default_fill_brush %p, svg_glyph_style %p,"
            "color_palette_index %u, measuring_mode %u stub!\n", iface, debug_d2d_point_2f(&baseline_origin),
            glyph_run, default_fill_brush, svg_glyph_style, color_palette_index, measuring_mode);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetColorBitmapGlyphImage(ID2D1DeviceContext6 *iface,
        DWRITE_GLYPH_IMAGE_FORMATS glyph_image_format, D2D1_POINT_2F glyph_origin, IDWriteFontFace *font_face,
        FLOAT font_em_size, UINT16 glyph_index, BOOL is_sideways, const D2D1_MATRIX_3X2_F *world_transform,
        FLOAT dpi_x, FLOAT dpi_y, D2D1_MATRIX_3X2_F *glyph_transform, ID2D1Image **glyph_image)
{
    FIXME("iface %p, glyph_image_format %u, glyph_origin %s, font_face %p, font_em_size %f, glyph_index %u,"
            "is_sideways %d, world_transform %p, dpi_x %f, dpi_y %f, glyph_transform %p, glyph_image %p stub!\n",
            iface, glyph_image_format, debug_d2d_point_2f(&glyph_origin), font_face, font_em_size, glyph_index,
            is_sideways, world_transform, dpi_x, dpi_y, glyph_transform, glyph_image);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_GetSvgGlyphImage(ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F glyph_origin, IDWriteFontFace *font_face, FLOAT font_em_size, UINT16 glyph_index,
        BOOL is_sideways, const D2D1_MATRIX_3X2_F *world_transform, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index, D2D1_MATRIX_3X2_F *glyph_transform,
        ID2D1CommandList **glyph_image)
{
    FIXME("iface %p, glyph_origin %s, font_face %p, font_em_size %f, glyph_index %u, is_sideways %d,"
            "world_transform %p, default_fill_brush %p, svg_glyph_style %p, color_palette_index %u,"
            "glyph_transform %p, glyph_image %p stub!\n", iface, debug_d2d_point_2f(&glyph_origin),
            font_face, font_em_size, glyph_index, is_sideways, world_transform, default_fill_brush,
            svg_glyph_style, color_palette_index, glyph_transform, glyph_image);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateSvgDocument(ID2D1DeviceContext6 *iface,
        IStream *input_xml_stream, D2D1_SIZE_F viewport_size, ID2D1SvgDocument **svg_document)
{
    FIXME("iface %p, input_xml_stream %p, svg_document %p stub!\n", iface, input_xml_stream,
            svg_document);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_device_context_DrawSvgDocument(ID2D1DeviceContext6 *iface,
        ID2D1SvgDocument *svg_document)
{
    FIXME("iface %p, svg_document %p stub!\n", iface, svg_document);
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateColorContextFromDxgiColorSpace(
        ID2D1DeviceContext6 *iface, DXGI_COLOR_SPACE_TYPE color_space, ID2D1ColorContext1 **color_context)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_color_context *object;
    HRESULT hr;

    TRACE("iface %p, color_space %u, color_context %p.\n", iface, color_space, color_context);

    if (SUCCEEDED(hr = d2d_color_context_create_from_dxgi_space(context->factory, color_space, &object)))
        *color_context = &object->ID2D1ColorContext1_iface;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_CreateColorContextFromSimpleColorProfile(
        ID2D1DeviceContext6 *iface, const D2D1_SIMPLE_COLOR_PROFILE *simple_profile, ID2D1ColorContext1 **color_context)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    struct d2d_color_context *object;
    HRESULT hr;

    TRACE("iface %p, simple_profile %p, color_context %p.\n", iface, simple_profile, color_context);

    if (SUCCEEDED(hr = d2d_color_context_create_from_simple_profile(context->factory, simple_profile, &object)))
        *color_context = &object->ID2D1ColorContext1_iface;

    return hr;
}

static void STDMETHODCALLTYPE d2d_device_context_BlendImage(ID2D1DeviceContext6 *iface, ID2D1Image *image,
        D2D1_BLEND_MODE blend_mode, const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode)
{
    FIXME("iface %p, image %p, blend_mode %u, target_offset %s, image_rect %s, interpolation_mode %u stub!\n",
            iface, image, blend_mode, debug_d2d_point_2f(target_offset), debug_d2d_rect_f(image_rect),
            interpolation_mode);
}

static const struct ID2D1DeviceContext6Vtbl d2d_device_context_vtbl =
{
    d2d_device_context_QueryInterface,
    d2d_device_context_AddRef,
    d2d_device_context_Release,
    d2d_device_context_GetFactory,
    d2d_device_context_CreateBitmap,
    d2d_device_context_CreateBitmapFromWicBitmap,
    d2d_device_context_CreateSharedBitmap,
    d2d_device_context_CreateBitmapBrush,
    d2d_device_context_CreateSolidColorBrush,
    d2d_device_context_CreateGradientStopCollection,
    d2d_device_context_CreateLinearGradientBrush,
    d2d_device_context_CreateRadialGradientBrush,
    d2d_device_context_CreateCompatibleRenderTarget,
    d2d_device_context_CreateLayer,
    d2d_device_context_CreateMesh,
    d2d_device_context_DrawLine,
    d2d_device_context_DrawRectangle,
    d2d_device_context_FillRectangle,
    d2d_device_context_DrawRoundedRectangle,
    d2d_device_context_FillRoundedRectangle,
    d2d_device_context_DrawEllipse,
    d2d_device_context_FillEllipse,
    d2d_device_context_DrawGeometry,
    d2d_device_context_FillGeometry,
    d2d_device_context_FillMesh,
    d2d_device_context_FillOpacityMask,
    d2d_device_context_DrawBitmap,
    d2d_device_context_DrawText,
    d2d_device_context_DrawTextLayout,
    d2d_device_context_DrawGlyphRun,
    d2d_device_context_SetTransform,
    d2d_device_context_GetTransform,
    d2d_device_context_SetAntialiasMode,
    d2d_device_context_GetAntialiasMode,
    d2d_device_context_SetTextAntialiasMode,
    d2d_device_context_GetTextAntialiasMode,
    d2d_device_context_SetTextRenderingParams,
    d2d_device_context_GetTextRenderingParams,
    d2d_device_context_SetTags,
    d2d_device_context_GetTags,
    d2d_device_context_PushLayer,
    d2d_device_context_PopLayer,
    d2d_device_context_Flush,
    d2d_device_context_SaveDrawingState,
    d2d_device_context_RestoreDrawingState,
    d2d_device_context_PushAxisAlignedClip,
    d2d_device_context_PopAxisAlignedClip,
    d2d_device_context_Clear,
    d2d_device_context_BeginDraw,
    d2d_device_context_EndDraw,
    d2d_device_context_GetPixelFormat,
    d2d_device_context_SetDpi,
    d2d_device_context_GetDpi,
    d2d_device_context_GetSize,
    d2d_device_context_GetPixelSize,
    d2d_device_context_GetMaximumBitmapSize,
    d2d_device_context_IsSupported,
    d2d_device_context_ID2D1DeviceContext_CreateBitmap,
    d2d_device_context_ID2D1DeviceContext_CreateBitmapFromWicBitmap,
    d2d_device_context_CreateColorContext,
    d2d_device_context_CreateColorContextFromFilename,
    d2d_device_context_CreateColorContextFromWicColorContext,
    d2d_device_context_CreateBitmapFromDxgiSurface,
    d2d_device_context_CreateEffect,
    d2d_device_context_ID2D1DeviceContext_CreateGradientStopCollection,
    d2d_device_context_CreateImageBrush,
    d2d_device_context_ID2D1DeviceContext_CreateBitmapBrush,
    d2d_device_context_CreateCommandList,
    d2d_device_context_IsDxgiFormatSupported,
    d2d_device_context_IsBufferPrecisionSupported,
    d2d_device_context_GetImageLocalBounds,
    d2d_device_context_GetImageWorldBounds,
    d2d_device_context_GetGlyphRunWorldBounds,
    d2d_device_context_GetDevice,
    d2d_device_context_SetTarget,
    d2d_device_context_GetTarget,
    d2d_device_context_SetRenderingControls,
    d2d_device_context_GetRenderingControls,
    d2d_device_context_SetPrimitiveBlend,
    d2d_device_context_GetPrimitiveBlend,
    d2d_device_context_SetUnitMode,
    d2d_device_context_GetUnitMode,
    d2d_device_context_ID2D1DeviceContext_DrawGlyphRun,
    d2d_device_context_DrawImage,
    d2d_device_context_DrawGdiMetafile,
    d2d_device_context_ID2D1DeviceContext_DrawBitmap,
    d2d_device_context_ID2D1DeviceContext_PushLayer,
    d2d_device_context_InvalidateEffectInputRectangle,
    d2d_device_context_GetEffectInvalidRectangleCount,
    d2d_device_context_GetEffectInvalidRectangles,
    d2d_device_context_GetEffectRequiredInputRectangles,
    d2d_device_context_ID2D1DeviceContext_FillOpacityMask,
    d2d_device_context_CreateFilledGeometryRealization,
    d2d_device_context_CreateStrokedGeometryRealization,
    d2d_device_context_DrawGeometryRealization,
    d2d_device_context_CreateInk,
    d2d_device_context_CreateInkStyle,
    d2d_device_context_CreateGradientMesh,
    d2d_device_context_CreateImageSourceFromWic,
    d2d_device_context_CreateLookupTable3D,
    d2d_device_context_CreateImageSourceFromDxgi,
    d2d_device_context_GetGradientMeshWorldBounds,
    d2d_device_context_DrawInk,
    d2d_device_context_DrawGradientMesh,
    d2d_device_context_ID2D1DeviceContext2_DrawGdiMetafile,
    d2d_device_context_CreateTransformedImageSource,
    d2d_device_context_CreateSpriteBatch,
    d2d_device_context_DrawSpriteBatch,
    d2d_device_context_CreateSvgGlyphStyle,
    d2d_device_context_ID2D1DeviceContext4_DrawText,
    d2d_device_context_ID2D1DeviceContext4_DrawTextLayout,
    d2d_device_context_DrawColorBitmapGlyphRun,
    d2d_device_context_DrawSvgGlyphRun,
    d2d_device_context_GetColorBitmapGlyphImage,
    d2d_device_context_GetSvgGlyphImage,
    d2d_device_context_CreateSvgDocument,
    d2d_device_context_DrawSvgDocument,
    d2d_device_context_CreateColorContextFromDxgiColorSpace,
    d2d_device_context_CreateColorContextFromSimpleColorProfile,
    d2d_device_context_BlendImage,
};

static inline struct d2d_device_context *impl_from_IDWriteTextRenderer(IDWriteTextRenderer *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_device_context, IDWriteTextRenderer_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_QueryInterface(IDWriteTextRenderer *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IDWriteTextRenderer)
            || IsEqualGUID(iid, &IID_IDWritePixelSnapping)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        IDWriteTextRenderer_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_text_renderer_AddRef(IDWriteTextRenderer *iface)
{
    struct d2d_device_context *context = impl_from_IDWriteTextRenderer(iface);

    TRACE("iface %p.\n", iface);

    return d2d_device_context_AddRef(&context->ID2D1DeviceContext6_iface);
}

static ULONG STDMETHODCALLTYPE d2d_text_renderer_Release(IDWriteTextRenderer *iface)
{
    struct d2d_device_context *context = impl_from_IDWriteTextRenderer(iface);

    TRACE("iface %p.\n", iface);

    return d2d_device_context_Release(&context->ID2D1DeviceContext6_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_IsPixelSnappingDisabled(IDWriteTextRenderer *iface,
        void *ctx, BOOL *disabled)
{
    struct d2d_draw_text_layout_ctx *context = ctx;

    TRACE("iface %p, ctx %p, disabled %p.\n", iface, ctx, disabled);

    *disabled = context->options & D2D1_DRAW_TEXT_OPTIONS_NO_SNAP;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_GetCurrentTransform(IDWriteTextRenderer *iface,
        void *ctx, DWRITE_MATRIX *transform)
{
    struct d2d_device_context *context = impl_from_IDWriteTextRenderer(iface);

    TRACE("iface %p, ctx %p, transform %p.\n", iface, ctx, transform);

    d2d_device_context_GetTransform(&context->ID2D1DeviceContext6_iface, (D2D1_MATRIX_3X2_F *)transform);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_GetPixelsPerDip(IDWriteTextRenderer *iface, void *ctx, float *ppd)
{
    struct d2d_device_context *render_target = impl_from_IDWriteTextRenderer(iface);

    TRACE("iface %p, ctx %p, ppd %p.\n", iface, ctx, ppd);

    *ppd = render_target->desc.dpiY / 96.0f;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_DrawGlyphRun(IDWriteTextRenderer *iface, void *ctx,
        float baseline_origin_x, float baseline_origin_y, DWRITE_MEASURING_MODE measuring_mode,
        const DWRITE_GLYPH_RUN *glyph_run, const DWRITE_GLYPH_RUN_DESCRIPTION *glyph_run_desc, IUnknown *effect)
{
    struct d2d_device_context *render_target = impl_from_IDWriteTextRenderer(iface);
    D2D1_POINT_2F baseline_origin = {baseline_origin_x, baseline_origin_y};
    struct d2d_draw_text_layout_ctx *context = ctx;
    BOOL color_font = FALSE;
    ID2D1Brush *brush;

    TRACE("iface %p, ctx %p, baseline_origin_x %.8e, baseline_origin_y %.8e, "
            "measuring_mode %#x, glyph_run %p, glyph_run_desc %p, effect %p.\n",
            iface, ctx, baseline_origin_x, baseline_origin_y,
            measuring_mode, glyph_run, glyph_run_desc, effect);

    /* CLIP is handled by the caller via PushAxisAlignedClip(). */
    if (context->options & ~(D2D1_DRAW_TEXT_OPTIONS_NO_SNAP
            | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP))
        FIXME("Ignoring options %#x.\n", context->options);

    brush = d2d_draw_get_text_brush(context, effect);

    TRACE("%s\n", debugstr_wn(glyph_run_desc->string, glyph_run_desc->stringLength));

    if (context->options & D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT)
    {
        IDWriteFontFace2 *fontface;

        if (SUCCEEDED(IDWriteFontFace_QueryInterface(glyph_run->fontFace,
                &IID_IDWriteFontFace2, (void **)&fontface)))
        {
            color_font = IDWriteFontFace2_IsColorFont(fontface);
            IDWriteFontFace2_Release(fontface);
        }
    }

    if (color_font)
    {
        IDWriteColorGlyphRunEnumerator *layers;
        IDWriteFactory2 *dwrite_factory;
        HRESULT hr;

        if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2,
                (IUnknown **)&dwrite_factory)))
        {
            ERR("Failed to create dwrite factory, hr %#lx.\n", hr);
            ID2D1Brush_Release(brush);
            return hr;
        }

        hr = IDWriteFactory2_TranslateColorGlyphRun(dwrite_factory, baseline_origin_x, baseline_origin_y,
                glyph_run, glyph_run_desc, measuring_mode, (DWRITE_MATRIX *)&render_target->drawing_state.transform, 0, &layers);
        IDWriteFactory2_Release(dwrite_factory);
        if (FAILED(hr))
        {
            ERR("Failed to create colour glyph run enumerator, hr %#lx.\n", hr);
            ID2D1Brush_Release(brush);
            return hr;
        }

        for (;;)
        {
            const DWRITE_COLOR_GLYPH_RUN *color_run;
            ID2D1Brush *color_brush;
            D2D1_POINT_2F origin;
            BOOL has_run = FALSE;

            if (FAILED(hr = IDWriteColorGlyphRunEnumerator_MoveNext(layers, &has_run)))
            {
                ERR("Failed to switch colour glyph layer, hr %#lx.\n", hr);
                break;
            }

            if (!has_run)
                break;

            if (FAILED(hr = IDWriteColorGlyphRunEnumerator_GetCurrentRun(layers, &color_run)))
            {
                ERR("Failed to get current colour run, hr %#lx.\n", hr);
                break;
            }

            if (color_run->paletteIndex == 0xffff)
                color_brush = brush;
            else
            {
                if (FAILED(hr = d2d_device_context_CreateSolidColorBrush(&render_target->ID2D1DeviceContext6_iface,
                        &color_run->runColor, NULL, (ID2D1SolidColorBrush **)&color_brush)))
                {
                    ERR("Failed to create solid colour brush, hr %#lx.\n", hr);
                    break;
                }
            }

            origin.x = color_run->baselineOriginX;
            origin.y = color_run->baselineOriginY;
            d2d_device_context_draw_glyph_run(render_target, origin, &color_run->glyphRun,
                    color_run->glyphRunDescription, color_brush, measuring_mode);

            if (color_brush != brush)
                ID2D1Brush_Release(color_brush);
        }

        IDWriteColorGlyphRunEnumerator_Release(layers);
    }
    else
        d2d_device_context_draw_glyph_run(render_target, baseline_origin, glyph_run, glyph_run_desc,
                brush, measuring_mode);

    ID2D1Brush_Release(brush);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_DrawUnderline(IDWriteTextRenderer *iface, void *ctx,
        float baseline_origin_x, float baseline_origin_y, const DWRITE_UNDERLINE *underline, IUnknown *effect)
{
    struct d2d_device_context *render_target = impl_from_IDWriteTextRenderer(iface);
    const D2D1_MATRIX_3X2_F *m = &render_target->drawing_state.transform;
    struct d2d_draw_text_layout_ctx *context = ctx;
    D2D1_ANTIALIAS_MODE prev_antialias_mode;
    D2D1_POINT_2F start, end;
    ID2D1Brush *brush;
    float thickness;

    TRACE("iface %p, ctx %p, baseline_origin_x %.8e, baseline_origin_y %.8e, underline %p, effect %p\n",
            iface, ctx, baseline_origin_x, baseline_origin_y, underline, effect);

    /* minimal thickness in DIPs that will result in at least 1 pixel thick line */
    thickness = max(96.0f / (render_target->desc.dpiY * sqrtf(m->_21 * m->_21 + m->_22 * m->_22)),
            underline->thickness);

    brush = d2d_draw_get_text_brush(context, effect);

    start.x = baseline_origin_x;
    start.y = baseline_origin_y + underline->offset + thickness / 2.0f;
    end.x = start.x + underline->width;
    end.y = start.y;
    prev_antialias_mode = d2d_device_context_set_aa_mode_from_text_aa_mode(render_target);
    d2d_device_context_DrawLine(&render_target->ID2D1DeviceContext6_iface, start, end, brush, thickness, NULL);
    render_target->drawing_state.antialiasMode = prev_antialias_mode;

    ID2D1Brush_Release(brush);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_DrawStrikethrough(IDWriteTextRenderer *iface, void *ctx,
        float baseline_origin_x, float baseline_origin_y, const DWRITE_STRIKETHROUGH *strikethrough, IUnknown *effect)
{
    struct d2d_device_context *render_target = impl_from_IDWriteTextRenderer(iface);
    const D2D1_MATRIX_3X2_F *m = &render_target->drawing_state.transform;
    struct d2d_draw_text_layout_ctx *context = ctx;
    D2D1_ANTIALIAS_MODE prev_antialias_mode;
    D2D1_POINT_2F start, end;
    ID2D1Brush *brush;
    float thickness;

    TRACE("iface %p, ctx %p, baseline_origin_x %.8e, baseline_origin_y %.8e, strikethrough %p, effect %p.\n",
            iface, ctx, baseline_origin_x, baseline_origin_y, strikethrough, effect);

    /* minimal thickness in DIPs that will result in at least 1 pixel thick line */
    thickness = max(96.0f / (render_target->desc.dpiY * sqrtf(m->_21 * m->_21 + m->_22 * m->_22)),
            strikethrough->thickness);

    brush = d2d_draw_get_text_brush(context, effect);

    start.x = baseline_origin_x;
    start.y = baseline_origin_y + strikethrough->offset + thickness / 2.0f;
    end.x = start.x + strikethrough->width;
    end.y = start.y;
    prev_antialias_mode = d2d_device_context_set_aa_mode_from_text_aa_mode(render_target);
    d2d_device_context_DrawLine(&render_target->ID2D1DeviceContext6_iface, start, end, brush, thickness, NULL);
    render_target->drawing_state.antialiasMode = prev_antialias_mode;

    ID2D1Brush_Release(brush);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_text_renderer_DrawInlineObject(IDWriteTextRenderer *iface, void *ctx,
        float origin_x, float origin_y, IDWriteInlineObject *object, BOOL is_sideways, BOOL is_rtl, IUnknown *effect)
{
    struct d2d_draw_text_layout_ctx *context = ctx;
    ID2D1Brush *brush;
    HRESULT hr;

    TRACE("iface %p, ctx %p, origin_x %.8e, origin_y %.8e, object %p, is_sideways %#x, is_rtl %#x, effect %p.\n",
            iface, ctx, origin_x, origin_y, object, is_sideways, is_rtl, effect);

    /* Inline objects may not pass effects all the way down, when using layout object internally for example.
       This is how default trimming sign object in DirectWrite works - it does not use effect passed to Draw(),
       and resulting DrawGlyphRun() is always called with NULL effect, however original effect is used and correct
       brush is selected at Direct2D level. */
    brush = context->brush;
    context->brush = d2d_draw_get_text_brush(context, effect);

    hr = IDWriteInlineObject_Draw(object, ctx, iface, origin_x, origin_y, is_sideways, is_rtl, effect);

    ID2D1Brush_Release(context->brush);
    context->brush = brush;

    return hr;
}

static const struct IDWriteTextRendererVtbl d2d_text_renderer_vtbl =
{
    d2d_text_renderer_QueryInterface,
    d2d_text_renderer_AddRef,
    d2d_text_renderer_Release,
    d2d_text_renderer_IsPixelSnappingDisabled,
    d2d_text_renderer_GetCurrentTransform,
    d2d_text_renderer_GetPixelsPerDip,
    d2d_text_renderer_DrawGlyphRun,
    d2d_text_renderer_DrawUnderline,
    d2d_text_renderer_DrawStrikethrough,
    d2d_text_renderer_DrawInlineObject,
};

static inline struct d2d_device_context *impl_from_ID2D1GdiInteropRenderTarget(ID2D1GdiInteropRenderTarget *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_device_context, ID2D1GdiInteropRenderTarget_iface);
}

static HRESULT STDMETHODCALLTYPE d2d_gdi_interop_render_target_QueryInterface(ID2D1GdiInteropRenderTarget *iface,
        REFIID iid, void **out)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    return IUnknown_QueryInterface(render_target->outer_unknown, iid, out);
}

static ULONG STDMETHODCALLTYPE d2d_gdi_interop_render_target_AddRef(ID2D1GdiInteropRenderTarget *iface)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);

    TRACE("iface %p.\n", iface);

    return IUnknown_AddRef(render_target->outer_unknown);
}

static ULONG STDMETHODCALLTYPE d2d_gdi_interop_render_target_Release(ID2D1GdiInteropRenderTarget *iface)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);

    TRACE("iface %p.\n", iface);

    return IUnknown_Release(render_target->outer_unknown);
}

static HRESULT d2d_gdi_interop_get_surface(struct d2d_device_context *context, IDXGISurface1 **surface)
{
    ID3D11Resource *resource;
    HRESULT hr;

    if (context->target.type != D2D_TARGET_BITMAP)
    {
        FIXME("Unimplemented for target type %u.\n", context->target.type);
        return E_NOTIMPL;
    }

    if (!(context->target.bitmap->options & D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE))
        return D2DERR_TARGET_NOT_GDI_COMPATIBLE;

    ID3D11RenderTargetView_GetResource(context->target.bitmap->rtv, &resource);
    hr = ID3D11Resource_QueryInterface(resource, &IID_IDXGISurface1, (void **)surface);
    ID3D11Resource_Release(resource);
    if (FAILED(hr))
    {
        *surface = NULL;
        WARN("Failed to get DXGI surface, %#lx.\n", hr);
        return hr;
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_gdi_interop_render_target_GetDC(ID2D1GdiInteropRenderTarget *iface,
        D2D1_DC_INITIALIZE_MODE mode, HDC *dc)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);
    IDXGISurface1 *surface;
    HRESULT hr;

    TRACE("iface %p, mode %d, dc %p.\n", iface, mode, dc);

    d2d_device_context_flush_lines(render_target);

    *dc = NULL;

    if (render_target->target.hdc)
        return D2DERR_WRONG_STATE;

    if (FAILED(hr = d2d_gdi_interop_get_surface(render_target, &surface)))
        return hr;

    hr = IDXGISurface1_GetDC(surface, mode != D2D1_DC_INITIALIZE_MODE_COPY, &render_target->target.hdc);
    IDXGISurface1_Release(surface);

    if (SUCCEEDED(hr))
        *dc = render_target->target.hdc;

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_gdi_interop_render_target_ReleaseDC(ID2D1GdiInteropRenderTarget *iface,
        const RECT *update)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);
    IDXGISurface1 *surface;
    RECT update_rect;
    HRESULT hr;

    TRACE("iface %p, update rect %s.\n", iface, wine_dbgstr_rect(update));

    if (!render_target->target.hdc)
        return D2DERR_WRONG_STATE;

    if (FAILED(hr = d2d_gdi_interop_get_surface(render_target, &surface)))
        return hr;

    render_target->target.hdc = NULL;
    if (update)
        update_rect = *update;
    hr = IDXGISurface1_ReleaseDC(surface, update ? &update_rect : NULL);
    IDXGISurface1_Release(surface);

    return hr;
}

static const struct ID2D1GdiInteropRenderTargetVtbl d2d_gdi_interop_render_target_vtbl =
{
    d2d_gdi_interop_render_target_QueryInterface,
    d2d_gdi_interop_render_target_AddRef,
    d2d_gdi_interop_render_target_Release,
    d2d_gdi_interop_render_target_GetDC,
    d2d_gdi_interop_render_target_ReleaseDC,
};


static const D3D11_INPUT_ELEMENT_DESC shape_il_desc_outline[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"PREV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NEXT", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const D3D11_INPUT_ELEMENT_DESC shape_il_desc_curve_outline[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"P", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"P", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"P", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"PREV", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NEXT", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const D3D11_INPUT_ELEMENT_DESC shape_il_desc_triangle[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const D3D11_INPUT_ELEMENT_DESC shape_il_desc_curve[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const D3D11_INPUT_ELEMENT_DESC shape_il_desc_fill_aa[] =
{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const char shape_vs_code_outline[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "/* The lines PₚᵣₑᵥP₀ and P₀Pₙₑₓₜ, both offset by ±½w, intersect each other at:\n"
    " *\n"
    " *   Pᵢ = P₀ ± w · ½q⃑ᵢ.\n"
    " *\n"
    " * Where:\n"
    " *\n"
    " *   q⃑ᵢ = q̂ₚᵣₑᵥ⊥ + tan(½θ) · -q̂ₚᵣₑᵥ\n"
    " *   θ  = ∠PₚᵣₑᵥP₀Pₙₑₓₜ\n"
    " *   q⃑ₚᵣₑᵥ = P₀ - Pₚᵣₑᵥ */\n"
    "void main(float2 position : POSITION, float2 prev : PREV, float2 next : NEXT, out struct output o)\n"
    "{\n"
    "    float2 q_prev, q_next, v_p, q_i;\n"
    "    float2x2 geom;\n"
    "\n"
    "    o.stroke_transform = float2x2(transform_rtx.xy, transform_rty.xy) * transform_geometry_r1.w * 0.5f;\n"
    "\n"
    "    geom = float2x2(transform_geometry_r0.xy, transform_geometry_r1.xy);\n"
    "    q_prev = normalize(mul(geom, prev));\n"
    "    q_next = normalize(mul(geom, next));\n"
    "\n"
    "    /* tan(½θ) = sin(θ) / (1 + cos(θ))\n"
    "     *         = (q̂ₚᵣₑᵥ⊥ · q̂ₙₑₓₜ) / (1 + (q̂ₚᵣₑᵥ · q̂ₙₑₓₜ)) */\n"
    "    v_p = float2(-q_prev.y, q_prev.x);\n"
    "    float denom = 1.0f + dot(q_prev, q_next);\n"
    "    if (denom < 1e-6f)\n"
    "        q_i = v_p;\n"
    "    else\n"
    "        q_i = (-dot(v_p, q_next) / denom) * q_prev + v_p;\n"
    "    if (transform_geometry_r0.w > 0.0f && length(q_i) > transform_geometry_r0.w)\n"
    "        q_i = normalize(q_i) * transform_geometry_r0.w;\n"
    "\n"
    "    float side = length(prev) > 1.5f ? -1.0f : 1.0f;\n"
    "    float2x2 rt = float2x2(transform_rtx.xy, transform_rty.xy);\n"
    "    float q_len = length(q_i);\n"
    "    float2 q_norm = q_len > 0.0001f ? q_i / q_len : float2(0.0f, 0.0f);\n"
    "    float hw_pixels = length(mul(rt, q_norm)) * transform_geometry_r1.w * 0.5f * q_len;\n"
    "    float expand = hw_pixels > 0.001f ? 0.75f / hw_pixels : 0.0f;\n"
    "    o.b = float4(side * (1.0f + expand), 0.0, 0.0, 1.0);\n"
    "\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz)) + transform_geometry_r1.w * 0.5f * (1.0f + expand) * q_i;\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
/*     ⎡p0.x p0.y 1⎤
 * A = ⎢p1.x p1.y 1⎥
 *     ⎣p2.x p2.y 1⎦
 *
 *     ⎡0 0⎤
 * B = ⎢½ 0⎥
 *     ⎣1 1⎦
 *
 * A' = ⎡p1.x-p0.x p1.y-p0.y⎤
 *      ⎣p2.x-p0.x p2.y-p0.y⎦
 *
 * B' = ⎡½ 0⎤
 *      ⎣1 1⎦
 *
 * A'T = B'
 * T = A'⁻¹B'
 */
static const char shape_vs_code_bezier_outline[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float2 p0 : P0, float2 p1 : P1, float2 p2 : P2,\n"
    "        float2 prev : PREV, float2 next : NEXT, out struct output o)\n"
    "{\n"
    "    float2 q_prev, q_next, v_p, q_i, p;\n"
    "    float2x2 geom, rt;\n"
    "\n"
    "    geom = float2x2(transform_geometry_r0.xy, transform_geometry_r1.xy);\n"
    "    rt = float2x2(transform_rtx.xy, transform_rty.xy);\n"
    "    o.stroke_transform = rt * transform_geometry_r1.w * 0.5f;\n"
    "\n"
    "    p = mul(geom, position);\n"
    "    p0 = mul(geom, p0);\n"
    "    p1 = mul(geom, p1);\n"
    "    p2 = mul(geom, p2);\n"
    "\n"
    "    p -= p0;\n"
    "    p1 -= p0;\n"
    "    p2 -= p0;\n"
    "\n"
    "    q_prev = normalize(mul(geom, prev));\n"
    "    q_next = normalize(mul(geom, next));\n"
    "\n"
    "    v_p = float2(-q_prev.y, q_prev.x);\n"
    "    float denom = 1.0f + dot(q_prev, q_next);\n"
    "    if (denom < 1e-6f)\n"
    "        q_i = v_p;\n"
    "    else\n"
    "        q_i = (-dot(v_p, q_next) / denom) * q_prev + v_p;\n"
    "    if (transform_geometry_r0.w > 0.0f && length(q_i) > transform_geometry_r0.w)\n"
    "        q_i = normalize(q_i) * transform_geometry_r0.w;\n"
    "    p += 0.5f * transform_geometry_r1.w * q_i;\n"
    "\n"
    "    v_p = mul(rt, p2);\n"
    "    v_p = normalize(float2(-v_p.y, v_p.x));\n"
    "    if (abs(dot(mul(rt, p1), v_p)) < 1.0f)\n"
    "    {\n"
    "        o.b.xzw = float3(0.0f, 0.0f, 0.0f);\n"
    "        o.b.y = dot(mul(rt, p), v_p);\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        o.b.zw = sign(dot(mul(rt, p1), v_p)) * v_p;\n"
    "        v_p = -float2(-p.y, p.x) / dot(float2(-p1.y, p1.x), p2);\n"
    "        o.b.x = dot(v_p, p1 - 0.5f * p2);\n"
    "        o.b.y = dot(v_p, p1);\n"
    "    }\n"
    "\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz)) + 0.5f * transform_geometry_r1.w * q_i;\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
/*     ⎡p0.x p0.y 1⎤
 * A = ⎢p1.x p1.y 1⎥
 *     ⎣p2.x p2.y 1⎦
 *
 *     ⎡1 0⎤
 * B = ⎢1 1⎥
 *     ⎣0 1⎦
 *
 * A' = ⎡p1.x-p0.x p1.y-p0.y⎤
 *      ⎣p2.x-p0.x p2.y-p0.y⎦
 *
 * B' = ⎡ 0 1⎤
 *      ⎣-1 1⎦
 *
 * A'T = B'
 * T = A'⁻¹B' = (B'⁻¹A')⁻¹
 */
static const char shape_vs_code_arc_outline[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float2 p0 : P0, float2 p1 : P1, float2 p2 : P2,\n"
    "        float2 prev : PREV, float2 next : NEXT, out struct output o)\n"
    "{\n"
    "    float2 q_prev, q_next, v_p, q_i, p;\n"
    "    float2x2 geom, rt, p_inv;\n"
    "    float a;\n"
    "    float2 bc;\n"
    "\n"
    "    geom = float2x2(transform_geometry_r0.xy, transform_geometry_r1.xy);\n"
    "    rt = float2x2(transform_rtx.xy, transform_rty.xy);\n"
    "    o.stroke_transform = rt * transform_geometry_r1.w * 0.5f;\n"
    "\n"
    "    p = mul(geom, position);\n"
    "    p0 = mul(geom, p0);\n"
    "    p1 = mul(geom, p1);\n"
    "    p2 = mul(geom, p2);\n"
    "\n"
    "    p -= p0;\n"
    "    p1 -= p0;\n"
    "    p2 -= p0;\n"
    "\n"
    "    q_prev = normalize(mul(geom, prev));\n"
    "    q_next = normalize(mul(geom, next));\n"
    "\n"
    "    v_p = float2(-q_prev.y, q_prev.x);\n"
    "    float denom = 1.0f + dot(q_prev, q_next);\n"
    "    if (denom < 1e-6f)\n"
    "        q_i = v_p;\n"
    "    else\n"
    "        q_i = (-dot(v_p, q_next) / denom) * q_prev + v_p;\n"
    "    if (transform_geometry_r0.w > 0.0f && length(q_i) > transform_geometry_r0.w)\n"
    "        q_i = normalize(q_i) * transform_geometry_r0.w;\n"
    "    p += 0.5f * transform_geometry_r1.w * q_i;\n"
    "\n"
    "    p_inv = float2x2(p1.y, -p1.x, p2.y - p1.y, p1.x - p2.x) / (p1.x * p2.y - p2.x * p1.y);\n"
    "    o.b.xy = mul(p_inv, p) + float2(1.0f, 0.0f);\n"
    "    o.b.zw = 0.0f;\n"
    "\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz)) + 0.5f * transform_geometry_r1.w * q_i;\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_triangle[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, out struct output o)\n"
    "{\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "    o.b = float4(1.0, 0.0, 1.0, 1.0);\n"
    "    o.stroke_transform = float2x2(1.0, 0.0, 0.0, 1.0);\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_fill_aa[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float3 texcoord : TEXCOORD0,\n"
    "          float2 expand_dir : TEXCOORD1, out struct output o)\n"
    "{\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz),\n"
    "                 dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "    /* Expand boundary skirt vertices outward by ~0.75 screen pixels. */\n"
    "    float exp_len = length(expand_dir);\n"
    "    if (exp_len > 0.001f)\n"
    "    {\n"
    "        float2x2 rt = float2x2(transform_rtx.xy, transform_rty.xy);\n"
    "        float2 exp_world = float2(\n"
    "            dot(expand_dir, float2(transform_geometry_r0.x, transform_geometry_r0.y)),\n"
    "            dot(expand_dir, float2(transform_geometry_r1.x, transform_geometry_r1.y)));\n"
    "        float2 exp_screen = mul(rt, exp_world);\n"
    "        float screen_px = length(exp_screen);\n"
    "        if (screen_px > 0.001f)\n"
    "            o.p += exp_world * (0.75f / screen_px);\n"
    "    }\n"
    /* Use b.w=2.0 as an explicit fill-AA marker (distinct from curve b.w=1.0 and arc b.w=1.0). */
    "    o.b = float4(texcoord, 2.0);\n"
    "    o.stroke_transform = float2x2(1.0, 0.0, 0.0, 1.0);\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_curve[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float3 texcoord : TEXCOORD0, out struct output o)\n"
    "{\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "    o.b = float4(texcoord, 1.0);\n"
    "    o.stroke_transform = float2x2(1.0, 0.0, 0.0, 1.0);\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_ps_code[] =
    "#define BRUSH_TYPE_SOLID    0\n"
    "#define BRUSH_TYPE_LINEAR   1\n"
    "#define BRUSH_TYPE_RADIAL   2\n"
    "#define BRUSH_TYPE_BITMAP   3\n"
    "#define BRUSH_TYPE_COUNT    4\n"
    "\n"
    "bool outline;\n"
    "bool is_arc;\n"
    "bool aa_mode;\n"
    "bool srgb_encode;\n"
    "bool linear_text;\n"
    "float dst_scale_x;\n"
    "float dst_offset_x;\n"
    "float dst_scale_y;\n"
    "float dst_offset_y;\n"
    "struct brush\n"
    "{\n"
    "    uint type;\n"
    "    float opacity;\n"
    "    float4 data[3];\n"
    "} colour_brush, opacity_brush;\n"
    "\n"
    /* Slots are fixed by what the brushes bind: bitmap brushes take t0/t1 and
     * s0/s1, gradient brushes take t2/t3. The destination copy for linear text
     * blending goes after those. */
    "SamplerState s0 : register(s0);\n"
    "SamplerState s1 : register(s1);\n"
    "SamplerState s_dst : register(s2);\n"
    "Texture2D t0 : register(t0);\n"
    "Texture2D t1 : register(t1);\n"
    "Buffer<float4> b0 : register(t2);\n"
    "Buffer<float4> b1 : register(t3);\n"
    "Texture2D t_dst : register(t4);\n"
    "\n"
    "struct input\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "};\n"
    "\n"
    "float4 sample_gradient(Buffer<float4> gradient, uint stop_count, float position)\n"
    "{\n"
    "    float4 c_low, c_high;\n"
    "    float p_low, p_high;\n"
    "    uint i;\n"
    "\n"
    "    p_low = gradient.Load(0).x;\n"
    "    c_low = gradient.Load(1);\n"
    "    c_high = c_low;\n"
    "\n"
    "    if (position < p_low)\n"
    "        return c_low;\n"
    "\n"
    "    [loop]\n"
    "    for (i = 1; i < stop_count; ++i)\n"
    "    {\n"
    "        p_high = gradient.Load(i * 2).x;\n"
    "        c_high = gradient.Load(i * 2 + 1);\n"
    "\n"
    "        if (position >= p_low && position <= p_high)\n"
    "            return lerp(c_low, c_high, (position - p_low) / (p_high - p_low));\n"
    "\n"
    "        p_low = p_high;\n"
    "        c_low = c_high;\n"
    "    }\n"
    "\n"
    "    return c_high;\n"
    "}\n"
    "\n"
    "float4 brush_linear(struct brush brush, Buffer<float4> gradient, float2 position)\n"
    "{\n"
    "    float2 start, end, v_p, v_q;\n"
    "    uint stop_count;\n"
    "    float p;\n"
    "\n"
    "    start = brush.data[0].xy;\n"
    "    end = brush.data[0].zw;\n"
    "    stop_count = asuint(brush.data[1].x);\n"
    "\n"
    "    v_p = position - start;\n"
    "    v_q = end - start;\n"
    "    p = dot(v_q, v_p) / dot(v_q, v_q);\n"
    "\n"
    "    return sample_gradient(gradient, stop_count, p);\n"
    "}\n"
    "\n"
    "float4 brush_radial(struct brush brush, Buffer<float4> gradient, float2 position)\n"
    "{\n"
    "    float2 centre, offset, ra, rb, v_p, v_q, r;\n"
    "    float b, c, l, t;\n"
    "    uint stop_count;\n"
    "\n"
    "    centre = brush.data[0].xy;\n"
    "    offset = brush.data[0].zw;\n"
    "    ra = brush.data[1].xy;\n"
    "    rb = brush.data[1].zw;\n"
    "    stop_count = asuint(brush.data[2].x);\n"
    "\n"
    "    /* Project onto ra, rb. */\n"
    "    r = float2(dot(ra, ra), dot(rb, rb));\n"
    "    v_p = position - (centre + offset);\n"
    "    v_p = float2(dot(v_p, ra), dot(v_p, rb)) / r;\n"
    "    v_q = float2(dot(offset, ra), dot(offset, rb)) / r;\n"
    "\n"
    "    /* ‖t·p̂ + q⃑‖ = 1\n"
    "     * (t·p̂ + q⃑) · (t·p̂ + q⃑) = 1\n"
    "     * t² + 2·(p̂·q⃑)·t + (q⃑·q⃑) = 1\n"
    "     *\n"
    "     * b = p̂·q⃑\n"
    "     * c = q⃑·q⃑ - 1\n"
    "     * t = -b + √(b² - c) */\n"
    "    l = length(v_p);\n"
    "    b = dot(v_p, v_q) / l;\n"
    "    c = dot(v_q, v_q) - 1.0;\n"
    "    t = -b + sqrt(b * b - c);\n"
    "\n"
    "    return sample_gradient(gradient, stop_count, l / t);\n"
    "}\n"
    "\n"
    "/* Apply a D2D1_EXTEND_MODE to one texture coordinate, relative to the\n"
    " * source rectangle [lo, hi]: 0 clamps to the rectangle, 1 (WRAP) tiles\n"
    " * it, 2 (MIRROR) alternates between it and its reflection. For a bitmap\n"
    " * brush the rectangle is the whole texture and this reduces to what the\n"
    " * sampler's address mode would do on its own. */\n"
    "float extend_texcoord(float t, float lo, float hi, uint mode)\n"
    "{\n"
    "    float size, u;\n"
    "\n"
    "    size = hi - lo;\n"
    "    if (size <= 0.0f)\n"
    "        return lo;\n"
    "    u = (t - lo) / size;\n"
    "    if (mode == 1)\n"
    "        u = frac(u);\n"
    "    else if (mode == 2)\n"
    "        u = 1.0f - abs(frac(u * 0.5f) * 2.0f - 1.0f);\n"
    "    else\n"
    "        u = saturate(u);\n"
    "    return lo + u * size;\n"
    "}\n"
    "\n"
    "float4 brush_bitmap(struct brush brush, Texture2D t, SamplerState s, float2 position)\n"
    "{\n"
    "    float3 transform[2];\n"
    "    bool ignore_alpha;\n"
    "    uint extend_modes;\n"
    "    float2 texcoord;\n"
    "    float4 colour;\n"
    "    float4 src_rect;\n"
    "\n"
    "    transform[0] = brush.data[0].xyz;\n"
    "    transform[1] = brush.data[1].xyz;\n"
    "    extend_modes = asuint(brush.data[0].w);\n"
    "    ignore_alpha = asuint(brush.data[1].w);\n"
    "    src_rect = brush.data[2];\n"
    "\n"
    "    texcoord.x = dot(position.xy, transform[0].xy) + transform[0].z;\n"
    "    texcoord.y = dot(position.xy, transform[1].xy) + transform[1].z;\n"
    "    /* D2D1 maps source_rect to the brush origin: the inverse transform\n"
    "     * yields source_rect-relative coordinates, so add its origin to get\n"
    "     * absolute texture coordinates, then extend within the rectangle. */\n"
    "    texcoord.x = extend_texcoord(texcoord.x + src_rect.x, src_rect.x, src_rect.z, extend_modes & 0xff);\n"
    "    texcoord.y = extend_texcoord(texcoord.y + src_rect.y, src_rect.y, src_rect.w, (extend_modes >> 8) & 0xff);\n"
    "    colour = t.Sample(s, texcoord);\n"
    "    if (ignore_alpha)\n"
    "        colour.a = 1.0;\n"
    "    return colour;\n"
    "}\n"
    "\n"
    "/* scRGB (linear) -> sRGB transfer function, applied to a premultiplied\n"
    " * colour: the curve is non-linear, so the colour has to be divided by\n"
    " * alpha before encoding and multiplied by it again afterwards. */\n"
    "float3 encode_srgb(float3 c)\n"
    "{\n"
    "    float3 lo, hi;\n"
    "\n"
    "    c = saturate(c);\n"
    "    lo = c * 12.92f;\n"
    "    hi = 1.055f * pow(max(c, 1.0e-8f), 1.0f / 2.4f) - 0.055f;\n"
    "    return lerp(lo, hi, step(0.0031308f, c));\n"
    "}\n"
    "\n"
    "/* sRGB -> scRGB (linear), the inverse of the above. */\n"
    "float3 decode_srgb(float3 c)\n"
    "{\n"
    "    float3 lo, hi;\n"
    "\n"
    "    c = saturate(c);\n"
    "    lo = c / 12.92f;\n"
    "    hi = pow(max((c + 0.055f) / 1.055f, 1.0e-8f), 2.4f);\n"
    "    return lerp(lo, hi, step(0.04045f, c));\n"
    "}\n"
    "\n"
    "float4 srgb_encode_colour(float4 colour)\n"
    "{\n"
    "    float3 c;\n"
    "\n"
    "    c = colour.a > 0.0f ? colour.rgb / colour.a : colour.rgb;\n"
    "\n"
    "    return float4(encode_srgb(c) * colour.a, colour.a);\n"
    "}\n"
    "\n"
    "/* Combine a glyph run with the destination in linear space.\n"
    " *\n"
    " * The output merger can only compute f(src) * A + g(dst) * B, so it cannot\n"
    " * apply a transfer function to the destination; a linear blend has to be\n"
    " * finished here, from a copy of the destination, and written straight out.\n"
    " * Coverage stays outside the transfer function on purpose: it is a\n"
    " * geometric area, not a colour, so it belongs in the linear domain where\n"
    " * areas add up. */\n"
    "float4 blend_text_linear(float4 colour, float coverage, float2 position)\n"
    "{\n"
    "    float3 src, dst;\n"
    "    float2 uv;\n"
    "\n"
    "    src = colour.a > 0.0f ? colour.rgb / colour.a : colour.rgb;\n"
    "    uv = float2(position.x * dst_scale_x + dst_offset_x,\n"
    "            position.y * dst_scale_y + dst_offset_y);\n"
    "    dst = t_dst.Sample(s_dst, uv).rgb;\n"
    "\n"
    "    src = lerp(decode_srgb(dst), decode_srgb(src), saturate(coverage * colour.a));\n"
    "\n"
    "    return float4(encode_srgb(src), 1.0f);\n"
    "}\n"
    "\n"
    "float4 sample_brush(struct brush brush, Texture2D t, SamplerState s, Buffer<float4> b, float2 position)\n"
    "{\n"
    "    if (brush.type == BRUSH_TYPE_SOLID)\n"
    "        return brush.data[0] * brush.opacity;\n"
    "    if (brush.type == BRUSH_TYPE_LINEAR)\n"
    "        return brush_linear(brush, b, position) * brush.opacity;\n"
    "    if (brush.type == BRUSH_TYPE_RADIAL)\n"
    "        return brush_radial(brush, b, position) * brush.opacity;\n"
    "    if (brush.type == BRUSH_TYPE_BITMAP)\n"
    "        return brush_bitmap(brush, t, s, position) * brush.opacity;\n"
    "    return float4(0.0, 0.0, 0.0, brush.opacity);\n"
    "}\n"
    "\n"
    "float4 main(struct input i) : SV_Target\n"
    "{\n"
    "    float4 colour;\n"
    "    float4 src_rect;\n"
    "\n"
    "    colour = sample_brush(colour_brush, t0, s0, b0, i.p);\n"
    "    if (srgb_encode)\n"
    "        colour = srgb_encode_colour(colour);\n"
    "    if (opacity_brush.type < BRUSH_TYPE_COUNT)\n"
    "    {\n"
    "        float coverage = sample_brush(opacity_brush, t1, s1, b1, i.p).a;\n"
    "\n"
    "        if (linear_text)\n"
    "            colour = blend_text_linear(colour, coverage, i.p);\n"
    "        else\n"
    "            colour *= coverage;\n"
    "    }\n"
    "\n"
    "    if (outline)\n"
    "    {\n"
    "        float2 du, dv, df;\n"
    "        float4 uv;\n"
    "\n"
    "        /* Evaluate the implicit form of the curve (u² - v = 0\n"
    "         * for Béziers, u² + v² - 1 = 0 for arcs) in texture\n"
    "         * space, using the screen-space partial derivatives\n"
    "         * to convert the calculated distance to object space.\n"
    "         *\n"
    "         * d(x, y) = |f(x, y)| / ‖∇f(x, y)‖\n"
    "         *         = |f(x, y)| / √((∂f/∂x)² + (∂f/∂y)²)\n"
    "         *\n"
    "         * For Béziers:\n"
    "         * f(x, y) = u(x, y)² - v(x, y)\n"
    "         * ∂f/∂x = 2u · ∂u/∂x - ∂v/∂x\n"
    "         * ∂f/∂y = 2u · ∂u/∂y - ∂v/∂y\n"
    "         *\n"
    "         * For arcs:\n"
    "         * f(x, y) = u(x, y)² + v(x, y)² - 1\n"
    "         * ∂f/∂x = 2u · ∂u/∂x + 2v · ∂v/∂x\n"
    "         * ∂f/∂y = 2u · ∂u/∂y + 2v · ∂v/∂y\n"
    "         *\n"
    "         * When aa_mode is set, instead of hard clip() we use:\n"
    "         * aa = saturate(edge / pixel_width + 0.5)\n"
    "         * for smooth ~1px alpha transitions. */\n"
    "        uv = i.b;\n"
    "\n"
    "        /* Straight line outline: b.x interpolates from -(1+expand) to +(1+expand)\n"
    "         * across stroke width, where |b.x|=1 is the nominal stroke edge.\n"
    "         * b.w=1 marks this as a straight line (vs bezier/arc). */\n"
    "        if (uv.w == 1.0f)\n"
    "        {\n"
    "            if (aa_mode)\n"
    "            {\n"
    "                float edge_dist = 1.0f - abs(uv.x);\n"
    "                float grad = length(float2(ddx(uv.x), ddy(uv.x)));\n"
    "                float edge_pixels = edge_dist / max(grad, 0.0001f);\n"
    "                float aa = saturate(edge_pixels + 0.5f);\n"
    "                colour.a *= aa;\n"
    "                colour.rgb *= aa;\n"
    "            }\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "\n"
    "        du = float2(ddx(uv.x), ddy(uv.x));\n"
    "        dv = float2(ddx(uv.y), ddy(uv.y));\n"
    "\n"
    "        if (!is_arc)\n"
    "        {\n"
    "            df = 2.0f * uv.x * du - dv;\n"
    "            float side = dot(df, uv.zw);\n"
    "            float val = uv.x * uv.x - uv.y;\n"
    "            float grad_len = length(mul(i.stroke_transform, df));\n"
    "\n"
    "            if (aa_mode)\n"
    "            {\n"
    "                if (side < 0.0f) discard;\n"
    "                float edge = grad_len - abs(val);\n"
    "                float pixel_w = length(df);\n"
    "                float aa = saturate(edge / max(pixel_w, 0.0001f) + 0.5f);\n"
    "                colour.a *= aa;\n"
    "                colour.rgb *= aa;\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                clip(side);\n"
    "                clip(grad_len - abs(val));\n"
    "            }\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            df = 2.0f * uv.x * du + 2.0f * uv.y * dv;\n"
    "            float side = dot(df, uv.zw);\n"
    "            float val = uv.x * uv.x + uv.y * uv.y - 1.0f;\n"
    "            float grad_len = length(mul(i.stroke_transform, df));\n"
    "\n"
    "            if (aa_mode)\n"
    "            {\n"
    "                if (side < 0.0f) discard;\n"
    "                float edge = grad_len - abs(val);\n"
    "                float pixel_w = length(df);\n"
    "                float aa = saturate(edge / max(pixel_w, 0.0001f) + 0.5f);\n"
    "                colour.a *= aa;\n"
    "                colour.rgb *= aa;\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                clip(side);\n"
    "                clip(grad_len - abs(val));\n"
    "            }\n"
    "        }\n"
    "        }\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        /* Fill-edge AA: b.w==2.0 is an explicit marker for fill triangles with edge weights.\n"
    "         * b.xyz encode barycentric proximity to each triangle edge.\n"
    "         * min(b.xyz/gradient) = pixel distance to nearest boundary edge. */\n"
    "        if (i.b.w > 1.5f && aa_mode)\n"
    "        {\n"
    "            float3 edges = i.b.xyz;\n"
    "            float3 grad_e = float3(\n"
    "                length(float2(ddx(edges.x), ddy(edges.x))),\n"
    "                length(float2(ddx(edges.y), ddy(edges.y))),\n"
    "                length(float2(ddx(edges.z), ddy(edges.z))));\n"
    "            float3 edge_px = edges / max(grad_e, float3(0.0001f, 0.0001f, 0.0001f));\n"
    "            float min_edge_px = min(edge_px.x, min(edge_px.y, edge_px.z));\n"
    "            float fa = saturate(min_edge_px + 0.5f);\n"
    "            colour.a *= fa;\n"
    "            colour.rgb *= fa;\n"
    "        }\n"
    "        /* Evaluate the implicit form of the curve in texture space.\n"
    "         * \"i.b.z\" determines which side of the curve is shaded. */\n"
    "        else if (!is_arc)\n"
    "        {\n"
    "            float fval = (i.b.x * i.b.x - i.b.y) * i.b.z;\n"
    "            if (aa_mode)\n"
    "            {\n"
    "                float2 fdu = float2(ddx(i.b.x), ddy(i.b.x));\n"
    "                float2 fdv = float2(ddx(i.b.y), ddy(i.b.y));\n"
    "                float2 fdf = 2.0f * i.b.x * fdu - fdv;\n"
    "                float fgrad = length(fdf);\n"
    "                float fa = saturate(fval / max(fgrad, 0.0001f) + 0.5f);\n"
    "                colour.a *= fa;\n"
    "                colour.rgb *= fa;\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                clip(fval);\n"
    "            }\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            float fval = (i.b.x * i.b.x + i.b.y * i.b.y - 1.0) * i.b.z;\n"
    "            if (aa_mode)\n"
    "            {\n"
    "                float2 fdu = float2(ddx(i.b.x), ddy(i.b.x));\n"
    "                float2 fdv = float2(ddx(i.b.y), ddy(i.b.y));\n"
    "                float2 fdf = 2.0f * i.b.x * fdu + 2.0f * i.b.y * fdv;\n"
    "                float fgrad = length(fdf);\n"
    "                float fa = saturate(fval / max(fgrad, 0.0001f) + 0.5f);\n"
    "                colour.a *= fa;\n"
    "                colour.rgb *= fa;\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                clip(fval);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return colour;\n"
    "}\n";
static const struct shape_info
{
    enum d2d_shape_type shape_type;
    const D3D11_INPUT_ELEMENT_DESC *il_desc;
    unsigned int il_element_count;
    const char *name;
    const char *vs_code;
    size_t vs_code_size;
}
shape_info[] =
{
    {D2D_SHAPE_TYPE_OUTLINE,        shape_il_desc_outline,        ARRAY_SIZE(shape_il_desc_outline),
     "outline",                     shape_vs_code_outline,        sizeof(shape_vs_code_outline) - 1},
    {D2D_SHAPE_TYPE_BEZIER_OUTLINE, shape_il_desc_curve_outline,  ARRAY_SIZE(shape_il_desc_curve_outline),
     "bezier_outline",              shape_vs_code_bezier_outline, sizeof(shape_vs_code_bezier_outline) - 1},
    {D2D_SHAPE_TYPE_ARC_OUTLINE,    shape_il_desc_curve_outline,  ARRAY_SIZE(shape_il_desc_curve_outline),
     "arc_outline",                 shape_vs_code_arc_outline,    sizeof(shape_vs_code_arc_outline) - 1},
    {D2D_SHAPE_TYPE_TRIANGLE,       shape_il_desc_triangle,       ARRAY_SIZE(shape_il_desc_triangle),
     "triangle",                    shape_vs_code_triangle,       sizeof(shape_vs_code_triangle) - 1},
    {D2D_SHAPE_TYPE_CURVE,          shape_il_desc_curve,          ARRAY_SIZE(shape_il_desc_curve),
     "curve",                       shape_vs_code_curve,          sizeof(shape_vs_code_curve) - 1},
    {D2D_SHAPE_TYPE_FILL_AA,        shape_il_desc_fill_aa,        ARRAY_SIZE(shape_il_desc_fill_aa),
     "fill_aa",                     shape_vs_code_fill_aa,        sizeof(shape_vs_code_fill_aa) - 1},
};

/* Lazily create the device-wide shared shape input layouts / vertex + pixel
 * shaders from the precompiled blobs. Called from every context creation but does
 * the actual D3D work only once per device; subsequent contexts just AddRef the
 * cached objects. All contexts of a device share the same ID3D11Device (queried
 * from the device's dxgi_device), so the objects are valid across contexts. */
static HRESULT d2d_device_ensure_shape_resources(struct d2d_device *device, ID3D11Device1 *d3d_device)
{
    struct d2d_shape_resources resources[D2D_SHAPE_TYPE_COUNT] = {0};
    ID3D11PixelShader *ps = NULL;
    ID3D10Blob *precompiled;
    unsigned int i;
    HRESULT hr = S_OK;

    EnterCriticalSection(&device->shape_cs);
    if (device->shape_resources_ready)
    {
        LeaveCriticalSection(&device->shape_cs);
        return S_OK;
    }

    for (i = 0; i < ARRAY_SIZE(shape_info); ++i)
    {
        const struct shape_info *si = &shape_info[i];

        precompiled = device->precompiled_shape_vs[i];
        if (FAILED(hr = ID3D11Device1_CreateInputLayout(d3d_device, si->il_desc, si->il_element_count,
                ID3D10Blob_GetBufferPointer(precompiled), ID3D10Blob_GetBufferSize(precompiled),
                &resources[si->shape_type].il)))
        {
            WARN("Failed to create input layout for shape type %#x, hr %#lx.\n", si->shape_type, hr);
            goto fail;
        }

        if (FAILED(hr = ID3D11Device1_CreateVertexShader(d3d_device,
                ID3D10Blob_GetBufferPointer(precompiled), ID3D10Blob_GetBufferSize(precompiled),
                NULL, &resources[si->shape_type].vs)))
        {
            WARN("Failed to create vertex shader for shape type %#x, hr %#lx.\n", si->shape_type, hr);
            goto fail;
        }
    }

    precompiled = device->precompiled_shape_ps;
    if (FAILED(hr = ID3D11Device1_CreatePixelShader(d3d_device,
            ID3D10Blob_GetBufferPointer(precompiled), ID3D10Blob_GetBufferSize(precompiled),
            NULL, &ps)))
    {
        WARN("Failed to create pixel shader, hr %#lx.\n", hr);
        goto fail;
    }

    for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
        device->shape_resources[i] = resources[i];
    device->shape_ps = ps;
    device->shape_resources_ready = TRUE;
    LeaveCriticalSection(&device->shape_cs);
    return S_OK;

fail:
    for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
    {
        if (resources[i].vs)
            ID3D11VertexShader_Release(resources[i].vs);
        if (resources[i].il)
            ID3D11InputLayout_Release(resources[i].il);
    }
    LeaveCriticalSection(&device->shape_cs);
    return hr;
}

static HRESULT d2d_device_context_init(struct d2d_device_context *render_target,
        struct d2d_device *device, IUnknown *outer_unknown, const struct d2d_device_context_ops *ops)
{
    D3D11_SUBRESOURCE_DATA buffer_data;
    IDWriteFactory *dwrite_factory;
    D3D11_RASTERIZER_DESC rs_desc;
    D3D11_BUFFER_DESC buffer_desc;
    struct d2d_factory *factory;
    unsigned int i;
    HRESULT hr;

    static const struct
    {
        float x, y;
    }
    quad[] =
    {
        {-1.0f,  1.0f},
        {-1.0f, -1.0f},
        { 1.0f,  1.0f},
        { 1.0f, -1.0f},
    };
    static const UINT16 indices[] = {0, 1, 2, 2, 1, 3};
    static const D3D_FEATURE_LEVEL feature_levels = D3D_FEATURE_LEVEL_10_0;

    render_target->ID2D1DeviceContext6_iface.lpVtbl = &d2d_device_context_vtbl;
    render_target->ID2D1GdiInteropRenderTarget_iface.lpVtbl = &d2d_gdi_interop_render_target_vtbl;
    render_target->IDWriteTextRenderer_iface.lpVtbl = &d2d_text_renderer_vtbl;
    render_target->IUnknown_iface.lpVtbl = &d2d_device_context_inner_unknown_vtbl;
    render_target->refcount = 1;
    ID2D1Device1_GetFactory((ID2D1Device1 *)&device->ID2D1Device6_iface, &render_target->factory);
    render_target->device = device;
    ID2D1Device6_AddRef(&render_target->device->ID2D1Device6_iface);

    factory = unsafe_impl_from_ID2D1Factory(render_target->factory);
    if (factory->factory_type == D2D1_FACTORY_TYPE_MULTI_THREADED)
        render_target->cs = &factory->cs;

    render_target->outer_unknown = outer_unknown ? outer_unknown : &render_target->IUnknown_iface;
    render_target->ops = ops;

    if (FAILED(hr = IDXGIDevice_QueryInterface(device->dxgi_device,
            &IID_ID3D11Device1, (void **)&render_target->d3d_device)))
    {
        WARN("Failed to query ID3D11Device1 interface, hr %#lx.\n", hr);
        goto err;
    }

    if (FAILED(hr = ID3D11Device1_CreateDeviceContextState(render_target->d3d_device,
            0, &feature_levels, 1, D3D11_SDK_VERSION, &IID_ID3D11Device1, NULL,
            &render_target->d3d_state)))
    {
        WARN("Failed to create device context state, hr %#lx.\n", hr);
        goto err;
    }

    if (FAILED(hr = d2d_device_ensure_shape_resources(device, render_target->d3d_device)))
        goto err;

    for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
    {
        render_target->shape_resources[i].il = device->shape_resources[i].il;
        ID3D11InputLayout_AddRef(render_target->shape_resources[i].il);
        render_target->shape_resources[i].vs = device->shape_resources[i].vs;
        ID3D11VertexShader_AddRef(render_target->shape_resources[i].vs);
    }

    buffer_desc.ByteWidth = sizeof(struct d2d_vs_cb);
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    buffer_desc.MiscFlags = 0;

    if (FAILED(hr = ID3D11Device1_CreateBuffer(render_target->d3d_device, &buffer_desc, NULL,
            &render_target->vs_cb)))
    {
        WARN("Failed to create constant buffer, hr %#lx.\n", hr);
        goto err;
    }

    render_target->ps = device->shape_ps;
    ID3D11PixelShader_AddRef(render_target->ps);

    buffer_desc.ByteWidth = sizeof(struct d2d_ps_cb);
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    buffer_desc.MiscFlags = 0;

    if (FAILED(hr = ID3D11Device1_CreateBuffer(render_target->d3d_device, &buffer_desc, NULL,
            &render_target->ps_cb)))
    {
        WARN("Failed to create constant buffer, hr %#lx.\n", hr);
        goto err;
    }

    buffer_desc.ByteWidth = sizeof(indices);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    buffer_desc.CPUAccessFlags = 0;
    buffer_desc.MiscFlags = 0;

    buffer_data.pSysMem = indices;
    buffer_data.SysMemPitch = 0;
    buffer_data.SysMemSlicePitch = 0;

    if (FAILED(hr = ID3D11Device1_CreateBuffer(render_target->d3d_device,
            &buffer_desc, &buffer_data, &render_target->ib)))
    {
        WARN("Failed to create clear index buffer, hr %#lx.\n", hr);
        goto err;
    }

    buffer_desc.ByteWidth = sizeof(quad);
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_data.pSysMem = quad;

    render_target->vb_stride = sizeof(*quad);
    if (FAILED(hr = ID3D11Device1_CreateBuffer(render_target->d3d_device,
            &buffer_desc, &buffer_data, &render_target->vb)))
    {
        WARN("Failed to create clear vertex buffer, hr %#lx.\n", hr);
        goto err;
    }

    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.FrontCounterClockwise = FALSE;
    rs_desc.DepthBias = 0;
    rs_desc.DepthBiasClamp = 0.0f;
    rs_desc.SlopeScaledDepthBias = 0.0f;
    rs_desc.DepthClipEnable = TRUE;
    rs_desc.ScissorEnable = TRUE;
    rs_desc.MultisampleEnable = FALSE;
    rs_desc.AntialiasedLineEnable = FALSE;
    if (FAILED(hr = ID3D11Device1_CreateRasterizerState(render_target->d3d_device, &rs_desc, &render_target->rs)))
    {
        WARN("Failed to create clear rasteriser state, hr %#lx.\n", hr);
        goto err;
    }

    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            &IID_IDWriteFactory, (IUnknown **)&dwrite_factory)))
    {
        ERR("Failed to create dwrite factory, hr %#lx.\n", hr);
        goto err;
    }

    hr = IDWriteFactory_CreateRenderingParams(dwrite_factory, &render_target->default_text_rendering_params);
    IDWriteFactory_Release(dwrite_factory);
    if (FAILED(hr))
    {
        ERR("Failed to create default text rendering parameters, hr %#lx.\n", hr);
        goto err;
    }

    render_target->drawing_state.transform = identity;

    if (!d2d_clip_stack_init(&render_target->clip_stack))
    {
        WARN("Failed to initialize clip stack.\n");
        hr = E_FAIL;
        goto err;
    }

    if (!d2d_layer_stack_init(&render_target->layer_stack))
    {
        WARN("Failed to initialize layer stack.\n");
        hr = E_FAIL;
        goto err;
    }

    render_target->desc.dpiX = 96.0f;
    render_target->desc.dpiY = 96.0f;

    return S_OK;

err:
    if (render_target->default_text_rendering_params)
        IDWriteRenderingParams_Release(render_target->default_text_rendering_params);
    if (render_target->rs)
        ID3D11RasterizerState_Release(render_target->rs);
    if (render_target->vb)
        ID3D11Buffer_Release(render_target->vb);
    if (render_target->ib)
        ID3D11Buffer_Release(render_target->ib);
    if (render_target->ps_cb)
        ID3D11Buffer_Release(render_target->ps_cb);
    if (render_target->ps)
        ID3D11PixelShader_Release(render_target->ps);
    if (render_target->vs_cb)
        ID3D11Buffer_Release(render_target->vs_cb);
    for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
    {
        if (render_target->shape_resources[i].vs)
            ID3D11VertexShader_Release(render_target->shape_resources[i].vs);
        if (render_target->shape_resources[i].il)
            ID3D11InputLayout_Release(render_target->shape_resources[i].il);
    }
    if (render_target->d3d_state)
        ID3DDeviceContextState_Release(render_target->d3d_state);
    if (render_target->d3d_device)
        ID3D11Device1_Release(render_target->d3d_device);
    ID2D1Device6_Release(&render_target->device->ID2D1Device6_iface);
    ID2D1Factory_Release(render_target->factory);
    return hr;
}

HRESULT d2d_d3d_create_render_target(struct d2d_device *device, IDXGISurface *surface, IUnknown *outer_unknown,
        const struct d2d_device_context_ops *ops, const D2D1_RENDER_TARGET_PROPERTIES *desc, void **render_target)
{
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    struct d2d_device_context *object;
    ID2D1Bitmap1 *bitmap;
    HRESULT hr;

    if (desc->type != D2D1_RENDER_TARGET_TYPE_DEFAULT && desc->type != D2D1_RENDER_TARGET_TYPE_HARDWARE)
        WARN("Ignoring render target type %#x.\n", desc->type);
    if (desc->usage != D2D1_RENDER_TARGET_USAGE_NONE)
        FIXME("Ignoring render target usage %#x.\n", desc->usage);
    if (desc->minLevel != D2D1_FEATURE_LEVEL_DEFAULT)
        WARN("Ignoring feature level %#x.\n", desc->minLevel);

    bitmap_desc.dpiX = desc->dpiX;
    bitmap_desc.dpiY = desc->dpiY;

    if (bitmap_desc.dpiX == 0.0f && bitmap_desc.dpiY == 0.0f)
    {
        bitmap_desc.dpiX = 96.0f;
        bitmap_desc.dpiY = 96.0f;
    }
    else if (bitmap_desc.dpiX <= 0.0f || bitmap_desc.dpiY <= 0.0f)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d2d_device_context_init(object, device, outer_unknown, ops)))
    {
        WARN("Failed to initialise render target, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    ID2D1DeviceContext6_SetDpi(&object->ID2D1DeviceContext6_iface, bitmap_desc.dpiX, bitmap_desc.dpiY);

    if (surface)
    {
        bitmap_desc.pixelFormat = desc->pixelFormat;
        bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if (desc->usage & D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE)
            bitmap_desc.bitmapOptions |= D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
        bitmap_desc.colorContext = NULL;

        if (FAILED(hr = ID2D1DeviceContext6_CreateBitmapFromDxgiSurface(&object->ID2D1DeviceContext6_iface,
                surface, &bitmap_desc, &bitmap)))
        {
            WARN("Failed to create target bitmap, hr %#lx.\n", hr);
            IUnknown_Release(&object->IUnknown_iface);
            return hr;
        }

        ID2D1DeviceContext6_SetTarget(&object->ID2D1DeviceContext6_iface, (ID2D1Image *)bitmap);
        ID2D1Bitmap1_Release(bitmap);
    }
    else
        object->desc.pixelFormat = desc->pixelFormat;

    TRACE("Created render target %p.\n", object);
    *render_target = outer_unknown ? &object->IUnknown_iface : (IUnknown *)&object->ID2D1DeviceContext6_iface;

    return S_OK;
}

static HRESULT WINAPI d2d_device_QueryInterface(ID2D1Device6 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1Device6)
            || IsEqualGUID(iid, &IID_ID2D1Device5)
            || IsEqualGUID(iid, &IID_ID2D1Device4)
            || IsEqualGUID(iid, &IID_ID2D1Device3)
            || IsEqualGUID(iid, &IID_ID2D1Device2)
            || IsEqualGUID(iid, &IID_ID2D1Device1)
            || IsEqualGUID(iid, &IID_ID2D1Device)
            || IsEqualGUID(iid, &IID_ID2D1Resource)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1Device6_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI d2d_device_AddRef(ID2D1Device6 *iface)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);
    ULONG refcount = InterlockedIncrement(&device->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

void d2d_device_indexed_objects_clear(struct d2d_indexed_objects *objects)
{
    size_t i;

    for (i = 0; i < objects->count; ++i)
        IUnknown_Release(objects->elements[i].object);
    free(objects->elements);
    objects->elements = NULL;
}

static ULONG WINAPI d2d_device_Release(ID2D1Device6 *iface)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);
    ULONG refcount = InterlockedDecrement(&device->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        IDXGIDevice_Release(device->dxgi_device);
        ID2D1Factory1_Release(device->factory);
        d2d_device_indexed_objects_clear(&device->shaders);
        for (unsigned int i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
        {
            if (device->precompiled_shape_vs[i])
                ID3D10Blob_Release(device->precompiled_shape_vs[i]);
        }
        if (device->precompiled_shape_ps)
            ID3D10Blob_Release(device->precompiled_shape_ps);
        if (device->shape_resources_ready)
        {
            for (unsigned int i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
            {
                if (device->shape_resources[i].vs)
                    ID3D11VertexShader_Release(device->shape_resources[i].vs);
                if (device->shape_resources[i].il)
                    ID3D11InputLayout_Release(device->shape_resources[i].il);
            }
            if (device->shape_ps)
                ID3D11PixelShader_Release(device->shape_ps);
        }
        DeleteCriticalSection(&device->shape_cs);
        free(device);
    }

    return refcount;
}

static void WINAPI d2d_device_GetFactory(ID2D1Device6 *iface, ID2D1Factory **factory)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);

    *factory = (ID2D1Factory *)device->factory;
    ID2D1Factory1_AddRef(device->factory);
}

static HRESULT d2d_device_create_device_context(struct d2d_device *device,
        D2D1_DEVICE_CONTEXT_OPTIONS options, REFIID iid, void **context)
{
    struct d2d_device_context *object;
    HRESULT hr;

    if (options)
        FIXME("Options are ignored %#x.\n", options);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d2d_device_context_init(object, device, NULL, NULL)))
    {
        WARN("Failed to initialise device context, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    TRACE("Created device context %p.\n", object);

    hr = ID2D1DeviceContext6_QueryInterface(&object->ID2D1DeviceContext6_iface, iid, context);
    ID2D1DeviceContext6_Release(&object->ID2D1DeviceContext6_iface);

    return hr;
}

static HRESULT WINAPI d2d_device_CreateDeviceContext(ID2D1Device6 *iface, D2D1_DEVICE_CONTEXT_OPTIONS options,
        ID2D1DeviceContext **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext, (void **)context);
}

static HRESULT WINAPI d2d_device_CreatePrintControl(ID2D1Device6 *iface, IWICImagingFactory *wic_factory,
        IPrintDocumentPackageTarget *document_target, const D2D1_PRINT_CONTROL_PROPERTIES *desc,
        ID2D1PrintControl **print_control)
{
    FIXME("iface %p, wic_factory %p, document_target %p, desc %p, print_control %p stub!\n", iface, wic_factory,
            document_target, desc, print_control);

    return E_NOTIMPL;
}

static void WINAPI d2d_device_SetMaximumTextureMemory(ID2D1Device6 *iface, UINT64 max_texture_memory)
{
    FIXME("iface %p, max_texture_memory %s stub!\n", iface, wine_dbgstr_longlong(max_texture_memory));
}

static UINT64 WINAPI d2d_device_GetMaximumTextureMemory(ID2D1Device6 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return 0;
}

static HRESULT WINAPI d2d_device_ClearResources(ID2D1Device6 *iface, UINT msec_since_use)
{
    FIXME("iface %p, msec_since_use %u stub!\n", iface, msec_since_use);

    return E_NOTIMPL;
}

static D2D1_RENDERING_PRIORITY WINAPI d2d_device_GetRenderingPriority(ID2D1Device6 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return D2D1_RENDERING_PRIORITY_NORMAL;
}

static void WINAPI d2d_device_SetRenderingPriority(ID2D1Device6 *iface, D2D1_RENDERING_PRIORITY priority)
{
    FIXME("iface %p, priority %#x stub!\n", iface, priority);
}

static HRESULT WINAPI d2d_device_CreateDeviceContext1(ID2D1Device6 *iface, D2D1_DEVICE_CONTEXT_OPTIONS options,
        ID2D1DeviceContext1 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext1, (void **)context);
}

static HRESULT STDMETHODCALLTYPE d2d_device_ID2D1Device2_CreateDeviceContext(ID2D1Device6 *iface,
        D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext2 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext2, (void **)context);
}

static void STDMETHODCALLTYPE d2d_device_FlushDeviceContexts(ID2D1Device6 *iface,
        ID2D1Bitmap *bitmap)
{
    FIXME("iface %p, bitmap %p stub!\n", iface, bitmap);
}

static HRESULT STDMETHODCALLTYPE d2d_device_GetDxgiDevice(ID2D1Device6 *iface,
        IDXGIDevice **dxgi_device)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, dxgi_device %p.\n", iface, dxgi_device);

    if (!device->allow_get_dxgi_device)
    {
        *dxgi_device = NULL;
        return D2DERR_INVALID_CALL;
    }

    IDXGIDevice_AddRef(device->dxgi_device);
    *dxgi_device = device->dxgi_device;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d2d_device_ID2D1Device3_CreateDeviceContext(ID2D1Device6 *iface,
        D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext3 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext3, (void **)context);
}

static HRESULT STDMETHODCALLTYPE d2d_device_ID2D1Device4_CreateDeviceContext(ID2D1Device6 *iface,
        D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext4 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext4, (void **)context);
}

static void STDMETHODCALLTYPE d2d_device_SetMaximumColorGlyphCacheMemory(ID2D1Device6 *iface,
        UINT64 size)
{
    FIXME("iface %p, size %s stub!\n", iface, wine_dbgstr_longlong(size));
}

static UINT64 STDMETHODCALLTYPE d2d_device_GetMaximumColorGlyphCacheMemory(ID2D1Device6 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return 0;
}

static HRESULT STDMETHODCALLTYPE d2d_device_ID2D1Device5_CreateDeviceContext(ID2D1Device6 *iface,
        D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext5 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext5, (void **)context);
}

static HRESULT STDMETHODCALLTYPE d2d_device_ID2D1Device6_CreateDeviceContext(ID2D1Device6 *iface,
        D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext6 **context)
{
    struct d2d_device *device = impl_from_ID2D1Device(iface);

    TRACE("iface %p, options %#x, context %p.\n", iface, options, context);

    return d2d_device_create_device_context(device, options, &IID_ID2D1DeviceContext6, (void **)context);
}

static const struct ID2D1Device6Vtbl d2d_device_vtbl =
{
    d2d_device_QueryInterface,
    d2d_device_AddRef,
    d2d_device_Release,
    d2d_device_GetFactory,
    d2d_device_CreateDeviceContext,
    d2d_device_CreatePrintControl,
    d2d_device_SetMaximumTextureMemory,
    d2d_device_GetMaximumTextureMemory,
    d2d_device_ClearResources,
    d2d_device_GetRenderingPriority,
    d2d_device_SetRenderingPriority,
    d2d_device_CreateDeviceContext1,
    d2d_device_ID2D1Device2_CreateDeviceContext,
    d2d_device_FlushDeviceContexts,
    d2d_device_GetDxgiDevice,
    d2d_device_ID2D1Device3_CreateDeviceContext,
    d2d_device_ID2D1Device4_CreateDeviceContext,
    d2d_device_SetMaximumColorGlyphCacheMemory,
    d2d_device_GetMaximumColorGlyphCacheMemory,
    d2d_device_ID2D1Device5_CreateDeviceContext,
    d2d_device_ID2D1Device6_CreateDeviceContext,
};

struct d2d_device *unsafe_impl_from_ID2D1Device(ID2D1Device1 *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == (ID2D1Device1Vtbl *)&d2d_device_vtbl);
    return CONTAINING_RECORD(iface, struct d2d_device, ID2D1Device6_iface);
}

HRESULT d2d_device_init(struct d2d_device *device, ID2D1Factory1 *factory, IDXGIDevice *dxgi_device,
    bool allow_get_dxgi_device)
{
    HRESULT hr;
    ID3D10Blob *compiled;

    device->ID2D1Device6_iface.lpVtbl = &d2d_device_vtbl;
    device->refcount = 1;
    /* Initialise first so the destructor can always DeleteCriticalSection, even
     * if this function fails below (the device is then released via the vtbl). */
    InitializeCriticalSection(&device->shape_cs);
    device->factory = factory;
    ID2D1Factory1_AddRef(device->factory);
    device->dxgi_device = dxgi_device;
    IDXGIDevice_AddRef(device->dxgi_device);
    device->allow_get_dxgi_device = allow_get_dxgi_device;

    for (unsigned int i = 0; i < ARRAY_SIZE(shape_info); ++i)
    {
        const struct shape_info *si = &shape_info[i];

        if (FAILED(hr = D3DCompile(si->vs_code, si->vs_code_size, si->name, NULL, NULL,
                "main", "vs_4_0", 0, 0, &compiled, NULL)))
        {
            WARN("Failed to compile shader for shape type %#x, hr %#lx.\n", si->shape_type, hr);
            return hr;
        }
        device->precompiled_shape_vs[i] = compiled;
    }

    if (FAILED(hr = D3DCompile(shape_ps_code, sizeof(shape_ps_code) - 1, "ps", NULL, NULL,
            "main", "ps_4_0", 0, 0, &compiled, NULL)))
    {
        WARN("Failed to compile the pixel shader, hr %#lx.\n", hr);
        return hr;
    }
    device->precompiled_shape_ps = compiled;

    return S_OK;
}

HRESULT d2d_device_add_indexed_object(struct d2d_indexed_objects *objects,
        const GUID *id, IUnknown *object)
{
    if (!d2d_array_reserve((void **)&objects->elements, &objects->size, objects->count + 1,
            sizeof(*objects->elements)))
    {
        WARN("Failed to resize elements array.\n");
        return E_OUTOFMEMORY;
    }

    objects->elements[objects->count].id = *id;
    objects->elements[objects->count].object = object;
    IUnknown_AddRef(object);
    objects->count++;

    return S_OK;
}

BOOL d2d_device_get_indexed_object(struct d2d_indexed_objects *objects, const GUID *id,
        IUnknown **object)
{
    size_t i;

    for (i = 0; i < objects->count; ++i)
    {
        if (IsEqualGUID(id, &objects->elements[i].id))
        {
            if (object)
            {
                *object = objects->elements[i].object;
                IUnknown_AddRef(*object);
            }
            return TRUE;
        }
    }

    if (object) *object = NULL;
    return FALSE;
}
