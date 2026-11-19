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
#include <float.h>

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

#define INITIAL_CLIP_STACK_SIZE 4

static const D2D1_MATRIX_3X2_F identity =
{{{
    1.0f, 0.0f,
    0.0f, 1.0f,
    0.0f, 0.0f,
}}};

static void d2d_device_context_flush_lines(struct d2d_device_context *context);
static void d2d_device_context_flush_fills(struct d2d_device_context *context);
static void d2d_device_context_flush_batches(struct d2d_device_context *context);
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
/* Swapping the private ID3DDeviceContextState in and out around every
 * primitive costs wined3d a full state emission each way (~20 command
 * stream packets), and the SynthEdit canvas draws ~1740 primitives per resize
 * step (issue 414). The state is swapped in once at BeginDraw() and back at
 * EndDraw() instead; a draw only swaps itself when its context does not hold
 * the state, so nested or foreign contexts keep the per-draw behaviour. The
 * draw setup sets every state it depends on, so the state object only has to
 * keep the application's own state out of the way. */
static bool d2d_device_context_state_is_current(const struct d2d_device_context *context)
{
    return context->held_prev_state && context->device && context->device->state_owner == context;
}

static void d2d_device_context_hold_state(struct d2d_device_context *context)
{
    ID3D11DeviceContext1 *d3d_context;

    if (!context->d3d_state || !context->device || context->held_prev_state)
        return;

    if (context->cs)
        EnterCriticalSection(context->cs);
    /* A nested BeginDraw() on another context of the same device takes the
     * state over and hands it back at its EndDraw(). */
    ID3D11Device1_GetImmediateContext1(context->d3d_device, &d3d_context);
    ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, context->d3d_state, &context->held_prev_state);
    ID3D11DeviceContext1_Release(d3d_context);
    context->prev_state_owner = context->device->state_owner;
    context->device->state_owner = context;
    TRACE("context %p holds the state, previous owner %p.\n", context, context->prev_state_owner);
    if (context->cs)
        LeaveCriticalSection(context->cs);
}

static void d2d_device_context_release_state(struct d2d_device_context *context)
{
    struct d2d_device *device = context->device;
    struct d2d_device_context *inner;
    ID3D11DeviceContext1 *d3d_context;

    if (!context->held_prev_state)
        return;

    if (context->cs)
        EnterCriticalSection(context->cs);
    if (device->state_owner == context)
    {
        ID3D11Device1_GetImmediateContext1(context->d3d_device, &d3d_context);
        ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, context->held_prev_state, NULL);
        ID3D11DeviceContext1_Release(d3d_context);
        ID3DDeviceContextState_Release(context->held_prev_state);
        device->state_owner = context->prev_state_owner;
    }
    else
    {
        /* EndDraw() out of order: a context that began after this one still
         * holds the state. Let it restore this context's predecessor instead. */
        for (inner = device->state_owner; inner && inner->prev_state_owner != context; inner = inner->prev_state_owner)
            ;
        if (inner)
        {
            ID3DDeviceContextState_Release(inner->held_prev_state);
            inner->held_prev_state = context->held_prev_state;
            inner->prev_state_owner = context->prev_state_owner;
        }
        else
        {
            ERR("context %p is not in the state owner chain.\n", context);
            ID3DDeviceContextState_Release(context->held_prev_state);
        }
    }
    TRACE("context %p released the state, owner now %p.\n", context, device->state_owner);
    context->held_prev_state = NULL;
    context->prev_state_owner = NULL;
    if (context->cs)
        LeaveCriticalSection(context->cs);
}

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
    if (d2d_device_context_state_is_current(render_target))
        *prev_state = NULL;
    else
        ID3D11DeviceContext1_SwapDeviceContextState(context, render_target->d3d_state, prev_state);

    ID3D11DeviceContext1_IASetInputLayout(context, shape_resources->il);
    ID3D11DeviceContext1_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_IASetIndexBuffer(context, ib, DXGI_FORMAT_R32_UINT, 0);
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
    if (prev_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(context, prev_state, NULL);
        ID3DDeviceContextState_Release(prev_state);
    }
    ID3D11DeviceContext1_Release(context);

    if (render_target->cs)
        LeaveCriticalSection(render_target->cs);
}

static void d2d_device_context_set_error(struct d2d_device_context *context, HRESULT code);

static void d2d_device_context_draw(struct d2d_device_context *render_target, enum d2d_shape_type shape_type,
        ID3D11Buffer *ib, unsigned int index_count, ID3D11Buffer *vb, unsigned int vb_stride,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *context;

    /* A context without a target has nothing to draw into; the entry points
     * report that as D2DERR_WRONG_STATE with the tags of the first call that
     * tried. Any path that gets here anyway would bind the render target
     * view of a bitmap that does not exist. */
    if (render_target->target.type != D2D_TARGET_BITMAP)
    {
        if (!FAILED(render_target->error.code))
            d2d_device_context_set_error(render_target, D2DERR_WRONG_STATE);
        return;
    }

    /* Lines and fills recorded before this primitive have to land underneath it. */
    d2d_device_context_flush_batches(render_target);

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
        if (context->effect_vs)
            ID3D11VertexShader_Release(context->effect_vs);
        if (context->effect_vs_cb)
            ID3D11Buffer_Release(context->effect_vs_cb);
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
        free(context->fill_batch.fill_vertices);
        free(context->fill_batch.curve_vertices);
        free(context->fill_batch.runs);
        /* Session 6 (C1): free cached rectangle geometry. The geometry object
         * was never registered with the COM factory (we bypassed the refcount
         * path), so direct cleanup + free is correct. */
        if (context->rect_geometry_cache)
        {
            d2d_geometry_cleanup(context->rect_geometry_cache);
            free(context->rect_geometry_cache);
        }
        if (context->rounded_rect_geometry_cache)
        {
            d2d_geometry_cleanup(context->rounded_rect_geometry_cache);
            free(context->rounded_rect_geometry_cache);
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
        d2d_device_context_release_state(context);
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

    d2d_device_context_flush_batches(context);

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
static void d2d_device_context_get_fill_clip(const struct d2d_device_context *context,
        const struct d2d_geometry *geometry, D2D1_RECT_F *clip);

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

/* The rounded rectangle geometry FillRoundedRectangle() reuses, claimed and
 * returned the way the rectangle geometry is. It is re-initialised fill only. */
static struct d2d_geometry *d2d_device_context_get_rounded_rect_geometry(struct d2d_device_context *context,
        const D2D1_ROUNDED_RECT *rect)
{
    struct d2d_geometry *geometry;

    if ((geometry = InterlockedExchangePointer((void **)&context->rounded_rect_geometry_cache, NULL)))
    {
        d2d_rounded_rectangle_geometry_reinit_fill(geometry, rect);
        return geometry;
    }

    if (!(geometry = calloc(1, sizeof(*geometry))))
    {
        ERR("Failed to allocate rounded rectangle geometry.\n");
        return NULL;
    }
    if (FAILED(d2d_rounded_rectangle_geometry_init(geometry, context->factory, rect)))
    {
        free(geometry);
        return NULL;
    }

    return geometry;
}

static void d2d_device_context_put_rounded_rect_geometry(struct d2d_device_context *context,
        struct d2d_geometry *geometry)
{
    struct d2d_geometry *displaced;

    if ((displaced = InterlockedExchangePointer((void **)&context->rounded_rect_geometry_cache, geometry)))
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

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

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

    d2d_device_context_flush_batches(render_target);

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
    struct d2d_geometry *cached;
    HRESULT hr;

    TRACE("iface %p, rect %p, brush %p.\n", iface, rect, brush);

    d2d_device_context_flush_lines(render_target);

    if (FAILED(render_target->error.code))
        return;

    /* A bitmap target fills the retained geometry; a command list records the
     * geometry itself and needs a factory object it can hold on to. */
    if (render_target->target.type == D2D_TARGET_BITMAP)
    {
        if (!(cached = d2d_device_context_get_rounded_rect_geometry(render_target, rect)))
            return;
        d2d_device_context_fill_geometry(render_target, cached, unsafe_impl_from_ID2D1Brush(brush), NULL);
        d2d_device_context_put_rounded_rect_geometry(render_target, cached);
        return;
    }

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

    d2d_device_context_flush_batches(render_target);

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
        const D2D_MATRIX_3X2_F *geometry_transform, float stroke_width, float miter_limit,
        D2D1_LINE_JOIN line_join, D2D1_CAP_STYLE start_cap, D2D1_CAP_STYLE end_cap, D2D1_CAP_STYLE dash_cap)
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
    new_cb.stroke_params.x = line_join;
    new_cb.stroke_params.y = start_cap;
    new_cb.stroke_params.z = end_cap;
    new_cb.stroke_params.w = dash_cap;

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

    /* Dynamic, written through a WRITE_DISCARD map: wined3d serves such a map
     * from the client thread (a sub-allocated, persistently mapped buffer
     * object), whereas an UpdateSubresource() on a DEFAULT buffer has to wait
     * for the command stream to drain every queued draw that references the
     * buffer and then round-trips to it - one synchronisation per primitive,
     * 1090-1940 per 20 px resize step of the SynthEdit canvas (issue 414). */
    desc.ByteWidth = min_size + min_size / 2;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;

    if (FAILED(hr = ID3D11Device1_CreateBuffer(ctx->d3d_device, &desc, NULL, &scratch->buffer)))
        return hr;
    scratch->size = desc.ByteWidth;

update:
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext *context;

        ID3D11Device1_GetImmediateContext(ctx->d3d_device, &context);
        if (FAILED(hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)scratch->buffer, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            WARN("Failed to map scratch buffer, hr %#lx.\n", hr);
            ID3D11DeviceContext_Release(context);
            return hr;
        }
        memcpy(mapped.pData, data, min_size);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)scratch->buffer, 0);
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
    ID3D11Buffer *geo_ib, *geo_vb;
    const struct d2d_fill *fill;
    D2D1_RECT_F clip;
    HRESULT hr;

    mask_geo = unsafe_impl_from_ID2D1Geometry(geometry);

    saved_transform = context->drawing_state.transform;
    context->drawing_state.transform._11 = 1.0f;
    context->drawing_state.transform._12 = 0.0f;
    context->drawing_state.transform._21 = 0.0f;
    context->drawing_state.transform._22 = 1.0f;
    context->drawing_state.transform._31 = 0.0f;
    context->drawing_state.transform._32 = 0.0f;

    d2d_device_context_get_fill_clip(context, mask_geo, &clip);
    fill = d2d_geometry_acquire_fill(mask_geo, &clip);
    if (!fill->face_count)
        goto done;

    /* Bail on CB map failure rather than drawing the stencil mask with stale
     * constants, which would yield an undefined clip mask. */
    if (FAILED(hr = d2d_device_context_update_vs_cb(context, &mask_geo->transform, 0.0f, 0.0f, D2D1_LINE_JOIN_MITER, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT))
            || FAILED(hr = d2d_device_context_update_ps_cb(context, NULL, NULL, FALSE, FALSE)))
    {
        WARN("Failed to update constant buffers, hr %#lx.\n", hr);
        goto done;
    }

    /* The mask goes through the same dynamic scratch buffers as the fills. A
     * DEFAULT buffer created with initial data cannot be filled from the client
     * thread, and wined3d drains the command stream for each one: a list of
     * cards clipped to rounded rectangles pushed about 50 masks per frame and
     * waited for the command stream twice for each of them. */
    if (FAILED(hr = d2d_device_context_get_scratch_buffer(context, D3D11_BIND_INDEX_BUFFER,
            fill->face_count * sizeof(*fill->faces),
            &context->scratch_ib[D2D_SHAPE_TYPE_TRIANGLE], fill->faces, &geo_ib))
            || FAILED(hr = d2d_device_context_get_scratch_buffer(context, D3D11_BIND_VERTEX_BUFFER,
            fill->vertex_count * sizeof(*fill->vertices),
            &context->scratch_vb[D2D_SHAPE_TYPE_TRIANGLE], fill->vertices, &geo_vb)))
    {
        WARN("Failed to upload the mask geometry, hr %#lx.\n", hr);
        goto done;
    }

    d2d_device_context_draw(context, D2D_SHAPE_TYPE_TRIANGLE, geo_ib,
            3 * fill->face_count, geo_vb, sizeof(*fill->vertices), NULL, NULL);

done:
    d2d_geometry_release_fill(mask_geo);
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

    if (FAILED(hr = d2d_device_context_update_vs_cb(context, &identity, batch->stroke_width, batch->miter_limit, D2D1_LINE_JOIN_MITER, batch->start_cap, batch->end_cap, D2D1_CAP_STYLE_FLAT)))
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

/* The line and fill batches are never open at the same time: opening one flushes
 * the other, so the order between lines and fills is kept and flushing both here
 * draws at most one of them. */
static void d2d_device_context_flush_batches(struct d2d_device_context *context)
{
    d2d_device_context_flush_lines(context);
    d2d_device_context_flush_fills(context);
}

static BOOL d2d_stroke_style_is_dashed(const struct d2d_stroke_style *stroke_style)
{
    return stroke_style && stroke_style->desc.dashStyle != D2D1_DASH_STYLE_SOLID;
}

/* The stroke width in world units. A fixed stroke keeps its width on the
 * device, a hairline is one pixel wide whatever the width asked for. */
static float d2d_device_context_stroke_width(const struct d2d_device_context *context,
        const struct d2d_stroke_style *stroke_style, float stroke_width)
{
    const D2D1_MATRIX_3X2_F *w = &context->drawing_state.transform;
    float scale;

    if (!stroke_style || stroke_style->desc.transformType == D2D1_STROKE_TRANSFORM_TYPE_NORMAL)
        return stroke_width;

    if (!((scale = w->m11) > 0.0f) && !((scale = sqrtf(fabsf(w->m11 * w->m22 - w->m12 * w->m21))) > 0.0f))
        scale = 1.0f;
    if (stroke_style->desc.transformType == D2D1_STROKE_TRANSFORM_TYPE_HAIRLINE)
        return 96.0f / context->desc.dpiX / scale;
    return stroke_width / scale;
}

/* Appends a line to the batch. Returns FALSE when the line cannot be batched;
 * the caller then draws it the ordinary way, which flushes the batch first. The
 * geometry is what d2d_geometry_outline_add_line_segment() produces for an open
 * two-point figure: a quad the outline vertex shader widens to the stroke. */
static BOOL d2d_device_context_batch_line(struct d2d_device_context *context, const D2D1_POINT_2F *p0,
        const D2D1_POINT_2F *p1, struct d2d_brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style)
{
    struct d2d_stroke_style *stroke_style_impl = unsafe_impl_from_ID2D1StrokeStyle(stroke_style);
    D2D1_CAP_STYLE start_cap = D2D1_CAP_STYLE_FLAT, end_cap = D2D1_CAP_STYLE_FLAT;
    struct d2d_line_batch *batch = &context->line_batch;
    struct d2d_outline_vertex *v;
    float qx, qy, len, miter_limit;
    struct d2d_line_run *run;
    struct d2d_face *f;
    size_t base, extra;

    /* A dashed line goes through DrawGeometry(), which cuts it into dashes. */
    if (!brush || brush->type != D2D_BRUSH_TYPE_SOLID
            || context->target.type != D2D_TARGET_BITMAP || batch->flushing
            || d2d_stroke_style_is_dashed(stroke_style_impl))
        return FALSE;

    /* Fills recorded before this line have to land underneath it. */
    d2d_device_context_flush_fills(context);

    /* What DrawGeometry() takes from a stroke style for a two-point figure. */
    miter_limit = 10.0f;
    stroke_width = d2d_device_context_stroke_width(context, stroke_style_impl, stroke_width);
    if (stroke_style_impl)
    {
        miter_limit = stroke_style_impl->desc.miterLimit;
        start_cap = stroke_style_impl->desc.startCap;
        end_cap = stroke_style_impl->desc.endCap;
    }
    extra = (start_cap != D2D1_CAP_STYLE_FLAT ? 4 : 0) + (end_cap != D2D1_CAP_STYLE_FLAT ? 4 : 0);

    /* One vertex buffer per batch, flushed before it grows past 65,535 vertices. */
    if (batch->run_count && (batch->vertex_count + 4 + extra > 0xffff || batch->stroke_width != stroke_width
            || batch->miter_limit != miter_limit
            || batch->start_cap != start_cap || batch->end_cap != end_cap
            || memcmp(&batch->transform, &context->drawing_state.transform, sizeof(batch->transform))))
        d2d_device_context_flush_lines(context);

    if (!d2d_array_reserve((void **)&batch->vertices, &batch->vertices_size,
            batch->vertex_count + 4 + extra, sizeof(*batch->vertices))
            || !d2d_array_reserve((void **)&batch->faces, &batch->faces_size,
            batch->face_count + 2 + extra / 2, sizeof(*batch->faces))
            || !d2d_array_reserve((void **)&batch->runs, &batch->runs_size,
            batch->run_count + 1, sizeof(*batch->runs)))
        return FALSE;

    if (!batch->run_count)
    {
        batch->transform = context->drawing_state.transform;
        batch->stroke_width = stroke_width;
        batch->miter_limit = miter_limit;
        batch->start_cap = start_cap;
        batch->end_cap = end_cap;
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
    d2d_outline_line_vertices(v, p0, p1, D2D_OUTLINE_END_START_CAP, D2D_OUTLINE_END_END_CAP);
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

    if (start_cap != D2D1_CAP_STYLE_FLAT)
    {
        base = batch->vertex_count;
        d2d_outline_cap_vertices(&batch->vertices[base], p0, -qx, -qy, TRUE);
        batch->vertex_count += 4;
        f = &batch->faces[batch->face_count];
        f[0].v[0] = base + 2;
        f[0].v[1] = base + 3;
        f[0].v[2] = base + 0;
        f[1].v[0] = base + 0;
        f[1].v[1] = base + 3;
        f[1].v[2] = base + 1;
        batch->face_count += 2;
        run->face_count += 2;
    }
    if (end_cap != D2D1_CAP_STYLE_FLAT)
    {
        base = batch->vertex_count;
        d2d_outline_cap_vertices(&batch->vertices[base], p1, qx, qy, FALSE);
        batch->vertex_count += 4;
        f = &batch->faces[batch->face_count];
        f[0].v[0] = base + 2;
        f[0].v[1] = base + 3;
        f[0].v[2] = base + 0;
        f[1].v[0] = base + 0;
        f[1].v[1] = base + 3;
        f[1].v[2] = base + 1;
        batch->face_count += 2;
        run->face_count += 2;
    }

    return TRUE;
}

static void d2d_device_context_draw_outline(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_outline *outline, struct d2d_brush *brush,
        float stroke_width, float miter_limit, D2D1_LINE_JOIN line_join, D2D1_CAP_STYLE start_cap,
        D2D1_CAP_STYLE end_cap, D2D1_CAP_STYLE dash_cap)
{
    ID3D11Buffer *ib, *vb;
    HRESULT hr;

    if (FAILED(hr = d2d_device_context_update_vs_cb(render_target, &geometry->transform, stroke_width, miter_limit, line_join, start_cap, end_cap, dash_cap)))
    {
        WARN("Failed to update vs constant buffer, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = d2d_device_context_update_ps_cb(render_target, brush, NULL, TRUE, FALSE)))
    {
        WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
        return;
    }

    if (outline->face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                outline->face_count * sizeof(*outline->faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_OUTLINE],
                outline->faces, &ib)))
        {
            WARN("Failed to create index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                outline->vertex_count * sizeof(*outline->vertices),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_OUTLINE],
                outline->vertices, &vb)))
        {
            ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
            return;
        }

        d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_OUTLINE, ib, 3 * outline->face_count, vb,
                sizeof(*outline->vertices), brush, NULL);
    }

    if (outline->bezier_face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                outline->bezier_face_count * sizeof(*outline->bezier_faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_BEZIER_OUTLINE],
                outline->bezier_faces, &ib)))
        {
            WARN("Failed to create curves index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                outline->bezier_count * sizeof(*outline->beziers),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_BEZIER_OUTLINE],
                outline->beziers, &vb)))
        {
            ERR("Failed to create curves vertex buffer, hr %#lx.\n", hr);
            return;
        }

        d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_BEZIER_OUTLINE, ib,
                3 * outline->bezier_face_count, vb,
                sizeof(*outline->beziers), brush, NULL);
    }

    if (outline->arc_face_count)
    {
        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_INDEX_BUFFER,
                outline->arc_face_count * sizeof(*outline->arc_faces),
                &render_target->scratch_ib[D2D_SHAPE_TYPE_ARC_OUTLINE],
                outline->arc_faces, &ib)))
        {
            WARN("Failed to create arcs index buffer, hr %#lx.\n", hr);
            return;
        }

        if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
                D3D11_BIND_VERTEX_BUFFER,
                outline->arc_count * sizeof(*outline->arcs),
                &render_target->scratch_vb[D2D_SHAPE_TYPE_ARC_OUTLINE],
                outline->arcs, &vb)))
        {
            ERR("Failed to create arcs vertex buffer, hr %#lx.\n", hr);
            return;
        }

        if (SUCCEEDED(d2d_device_context_update_ps_cb(render_target, brush, NULL, TRUE, TRUE)))
            d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_ARC_OUTLINE, ib,
                    3 * outline->arc_face_count, vb,
                    sizeof(*outline->arcs), brush, NULL);
    }
}

static void d2d_device_context_draw_geometry(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, float stroke_width, float miter_limit,
        D2D1_LINE_JOIN line_join, D2D1_CAP_STYLE start_cap, D2D1_CAP_STYLE end_cap, D2D1_CAP_STYLE dash_cap)
{
    const struct d2d_outline *outline = d2d_geometry_acquire_outline(geometry);

    d2d_device_context_draw_outline(render_target, geometry, outline, brush, stroke_width, miter_limit,
            line_join, start_cap, end_cap, dash_cap);
    d2d_geometry_release_outline(geometry);
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

    d2d_device_context_flush_batches(context);

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

    {
        D2D1_CAP_STYLE start_cap = D2D1_CAP_STYLE_FLAT, end_cap = D2D1_CAP_STYLE_FLAT;
        D2D1_LINE_JOIN line_join = D2D1_LINE_JOIN_MITER;
        struct d2d_geometry *dashed = NULL;
        float miter_limit = 10.0f;

        if (stroke_style_impl)
        {
            miter_limit = stroke_style_impl->desc.miterLimit;
            line_join = stroke_style_impl->desc.lineJoin;
            start_cap = stroke_style_impl->desc.startCap;
            end_cap = stroke_style_impl->desc.endCap;
        }

        if (d2d_stroke_style_is_dashed(stroke_style_impl))
        {
            D2D1_MATRIX_3X2_F metric = {{{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}}};
            float unit = stroke_width;

            /* The pattern is a multiple of the width the stroke has where it
             * is laid out: in geometry space for a normal stroke, on the
             * device for a fixed or hairline one. */
            if (stroke_style_impl->desc.transformType != D2D1_STROKE_TRANSFORM_TYPE_NORMAL)
            {
                metric = context->drawing_state.transform;
                if (stroke_style_impl->desc.transformType == D2D1_STROKE_TRANSFORM_TYPE_HAIRLINE)
                    unit = 96.0f / context->desc.dpiX;
            }
            dashed = d2d_geometry_create_dashed_outline(geometry, stroke_style, unit, &metric);
        }

        stroke_width = d2d_device_context_stroke_width(context, stroke_style_impl, stroke_width);
        if (dashed)
        {
            d2d_device_context_draw_geometry(context, dashed, brush_impl, stroke_width, miter_limit,
                    line_join, start_cap, end_cap, stroke_style_impl->desc.dashCap);
            d2d_geometry_destroy_dashed_outline(dashed);
            return;
        }

        d2d_device_context_draw_geometry(context, geometry_impl, brush_impl, stroke_width, miter_limit,
                line_join, start_cap, end_cap, D2D1_CAP_STYLE_FLAT);
    }
}

/* Draw filled triangles without anti-aliasing (plain index+vertex buffer path). */
static void d2d_device_context_fill_triangles(struct d2d_device_context *render_target,
        const struct d2d_fill *fill, struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    ID3D11Buffer *ib, *vb;
    HRESULT hr;

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_INDEX_BUFFER,
            fill->face_count * sizeof(*fill->faces),
            &render_target->scratch_ib[D2D_SHAPE_TYPE_TRIANGLE],
            fill->faces, &ib)))
    {
        WARN("Failed to create index buffer, hr %#lx.\n", hr);
        return;
    }

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_VERTEX_BUFFER,
            fill->vertex_count * sizeof(*fill->vertices),
            &render_target->scratch_vb[D2D_SHAPE_TYPE_TRIANGLE],
            fill->vertices, &vb)))
    {
        ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
        return;
    }

    d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_TRIANGLE, ib, 3 * fill->face_count, vb,
            sizeof(*fill->vertices), brush, opacity_brush);
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
static size_t d2d_fill_aa_find_vertex(const struct d2d_fill *fill,
        const UINT32 *vertex_map, size_t map_size, const D2D1_POINT_2F *p)
{
    UINT32 slot = d2d_fill_aa_position_hash(p) % map_size;
    size_t probe = 0;

    while (probe < map_size && vertex_map[slot])
    {
        if (!memcmp(&fill->vertices[vertex_map[slot] - 1], p, sizeof(*p)))
            return vertex_map[slot] - 1;
        slot = (slot + 1) % map_size;
        ++probe;
    }
    return ~(size_t)0;
}

/* The vertex of the mesh an end of a curve lies on, or a fill vertex that is
 * not in the mesh, or ~0.
 *
 * A vertex that lies a float or two from another one can be left out of the
 * mesh: the slivers along the line between the two count as outside, and the
 * outline runs through the other one. Rounded rectangles have such lines
 * where two corners take up a whole side (y + radius and y + height - radius
 * round differently), and so has a figure closed by its last segment a float
 * away from its start. The vertices are sorted, so the two are close in the
 * array as well; the one that is in the mesh stands for the other. */
static size_t d2d_fill_aa_find_mesh_vertex(const struct d2d_fill *fill, const UINT32 *vertex_map,
        size_t vertex_map_size, const BYTE *used, const D2D1_POINT_2F *p)
{
    size_t idx = d2d_fill_aa_find_vertex(fill, vertex_map, vertex_map_size, p), j;
    float limit;
    int d;

    if (idx == ~(size_t)0 || used[idx])
        return idx;
    limit = 2.0f * FLT_EPSILON * max(fabsf(p->x), fabsf(p->y));
    for (d = 1; d <= 8; ++d)
    {
        j = d & 1 ? idx + (d + 1) / 2 : idx - d / 2;
        if (j < fill->vertex_count && used[j] && fabsf(fill->vertices[j].x - p->x) <= limit
                && fabsf(fill->vertices[j].y - p->y) <= limit)
            return j;
    }
    return idx;
}

/* Where a curve runs relative to a mesh edge it shares a triangle with: off the
 * edge by c1 * t + c2 * t^2 in geometry units, t running from the vertex start
 * (0) to the other end of the edge (1). start is ~0u for a seam without it.
 * chord is set when the edge is a chord, so that both of its ends lie on the
 * curve; the far end of a control edge is the control point, which does not. */
struct d2d_fill_aa_seam
{
    UINT32 start;
    float c1, c2;
    BOOL chord;
    /* Of a chord: where the tangents at its two ends meet. */
    D2D1_POINT_2F control;
};

static float d2d_fill_aa_line_distance(const D2D1_POINT_2F *p, const D2D1_POINT_2F *a, const D2D1_POINT_2F *b)
{
    float dx = b->x - a->x, dy = b->y - a->y, l = sqrtf(dx * dx + dy * dy);

    if (!(l > 0.0f))
        return 0.0f;
    return fabsf(dx * (p->y - a->y) - dy * (p->x - a->x)) / l;
}

/* The linear part of the mapping from geometry space to render target pixels,
 * as the vertex shader applies it: pixel = dpi * world * geometry * p. Stored
 * row major for column vectors, px = m[0] * x + m[1] * y, py = m[2] * x + m[3] * y. */
static void d2d_device_context_get_pixel_matrix(const struct d2d_device_context *context,
        const D2D1_MATRIX_3X2_F *g, float *m)
{
    const D2D1_MATRIX_3X2_F *w = &context->drawing_state.transform;
    float sx = context->desc.dpiX / 96.0f, sy = context->desc.dpiY / 96.0f;

    m[0] = sx * (w->_11 * g->_11 + w->_21 * g->_12);
    m[1] = sx * (w->_11 * g->_21 + w->_21 * g->_22);
    m[2] = sy * (w->_12 * g->_11 + w->_22 * g->_12);
    m[3] = sy * (w->_12 * g->_21 + w->_22 * g->_22);
}

/* The part of geometry space a fill of geometry can reach the target from: the
 * target with a margin for the antialiasing ramps, mapped back through the
 * transforms of the draw. A path too large to tessellate in Close() is
 * tessellated clipped to it (d2d_geometry_acquire_fill()). A transform without
 * an inverse maps everything onto a line, so nothing is in view. */
static void d2d_device_context_get_fill_clip(const struct d2d_device_context *context,
        const struct d2d_geometry *geometry, D2D1_RECT_F *clip)
{
    static const float margin = 4.0f;
    D2D1_MATRIX_3X2_F m = geometry->transform, inv;
    float sx = context->desc.dpiX / 96.0f, sy = context->desc.dpiY / 96.0f;
    float w = context->pixel_size.width, h = context->pixel_size.height;
    D2D1_POINT_2F p;
    unsigned int i;

    d2d_matrix_multiply(&m, &context->drawing_state.transform);
    m._11 *= sx;
    m._21 *= sx;
    m._31 *= sx;
    m._12 *= sy;
    m._22 *= sy;
    m._32 *= sy;

    memset(clip, 0, sizeof(*clip));
    if (!d2d_matrix_invert(&inv, &m))
        return;

    clip->left = clip->top = FLT_MAX;
    clip->right = clip->bottom = -FLT_MAX;
    for (i = 0; i < 4; ++i)
    {
        d2d_point_transform(&p, &inv, i & 1 ? w + margin : -margin, i & 2 ? h + margin : -margin);
        d2d_rect_expand(clip, &p);
    }
}

/* Whether the control point of the quadratic Bézier triangle v lies within 1/16 px
 * of its chord, m being the pixel matrix. Such a triangle is filled as that chord:
 * it is thinner than the precision of its own texture coordinates, and the skirt
 * that carries its antialiasing ramp past the curve extrapolates them another
 * 0.75 px, so the ramp came out wide and uneven. Straight edges that are added
 * as Béziers, which is how SVG exports often store them, end up here. The stroke
 * shader draws such pieces as their chord below the same threshold. */
static BOOL d2d_curve_is_flat(const struct d2d_curve_vertex *v, const float *m)
{
    float cx = v[1].position.x - v[0].position.x, cy = v[1].position.y - v[0].position.y;
    float ex = v[2].position.x - v[0].position.x, ey = v[2].position.y - v[0].position.y;
    float px, py, qx, qy, l;

    px = m[0] * cx + m[1] * cy;
    py = m[2] * cx + m[3] * cy;
    qx = m[0] * ex + m[1] * ey;
    qy = m[2] * ex + m[3] * ey;
    if (!((l = sqrtf(qx * qx + qy * qy)) > 0.0f))
        return FALSE;
    return fabsf(qx * py - qy * px) / l < 0.0625f;
}

/* The slot at which the search for an edge starts in the edge map of the fill
 * antialiasing. The key of an edge is (lo << 32) + hi + 1 for its two vertex
 * indices, and most edges of a triangulation join vertices with indices close
 * to each other: taken modulo the map size, the keys of a mesh fall into runs
 * of adjacent slots, and the linear search behind a run walks to its end. The
 * multiplication spreads them; the upper half of the product, scaled to the
 * map size, needs no division. */
static UINT32 d2d_fill_aa_edge_slot(UINT64 key, size_t map_size)
{
    return (((key * 0x9e3779b97f4a7c15ull) >> 32) * map_size) >> 32;
}

static UINT32 d2d_fill_aa_next_slot(UINT32 slot, size_t map_size)
{
    return slot + 1 == map_size ? 0 : slot + 1;
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
 * an interior diagonal is simply not found. A corner of a curve triangle that
 * lies on a vertex the triangulation left out of the mesh stands for the vertex
 * a float or two away that is in it, see d2d_fill_aa_find_mesh_vertex(): the
 * mesh has the edge between those. Flat Bézier triangles are not drawn
 * (see d2d_curve_is_flat()), so their edges are left to be straight boundary
 * edges with a skirt of their own.
 *
 * For each seam, seams also records where the curve runs off the edge, which the
 * fill ramps towards. A quadratic Bézier runs half way between its chord and its control
 * point, a quarter circle - which is what the arc triangles hold - sqrt(2) - 1 of
 * the way, both following a parabola along the chord. Along a control edge the
 * curve starts tangent at its end point and departs as (h / 4) * t^2, h being the
 * distance of the other end point from that edge.
 *
 * curve_ends, if not NULL, receives for every curve triangle, the Béziers
 * before the arcs, the vertices of the mesh the two ends of its curve lie on,
 * if both do; see d2d_fill_aa_find_mesh_vertex(). The caller sets it to ~0u.
 *
 * Returns FALSE if the marking could not be completed. Half-marked data is
 * worse than none — an unmarked seam would get a skirt and be drawn twice — so
 * the caller falls back to the plain fill in that case. */
static BOOL d2d_fill_aa_mark_curve_edges(const struct d2d_fill *fill,
        const UINT64 *edge_keys, BYTE *edge_curve, struct d2d_fill_aa_seam *seams, size_t map_size,
        const float *m, UINT32 *curve_ends)
{
    const struct d2d_curve_vertex *curves[2];
    size_t counts[2], vertex_map_size, i, c;
    UINT32 *vertex_map;
    BYTE *used;

    curves[0] = fill->bezier_vertices;
    counts[0] = fill->bezier_vertex_count;
    curves[1] = fill->arc_vertices;
    counts[1] = fill->arc_vertex_count;

    vertex_map_size = fill->vertex_count * 2 + 16;
    vertex_map = calloc(vertex_map_size, sizeof(*vertex_map));
    used = calloc(fill->vertex_count, sizeof(*used));
    if (!vertex_map || !used)
    {
        free(vertex_map);
        free(used);
        return FALSE;
    }
    for (i = 0; i < fill->face_count; ++i)
        used[fill->faces[i].v[0]] = used[fill->faces[i].v[1]] = used[fill->faces[i].v[2]] = 1;

    for (i = 0; i < fill->vertex_count; ++i)
    {
        const D2D1_POINT_2F *p = &fill->vertices[i];
        UINT32 slot = d2d_fill_aa_position_hash(p) % vertex_map_size;
        size_t probe = 0;

        while (probe < vertex_map_size && vertex_map[slot]
                && memcmp(&fill->vertices[vertex_map[slot] - 1], p, sizeof(*p)))
        {
            slot = (slot + 1) % vertex_map_size;
            ++probe;
        }
        if (probe == vertex_map_size)
        {
            free(vertex_map);
            free(used);
            return FALSE;
        }
        if (!vertex_map[slot])
            vertex_map[slot] = i + 1;
    }

    for (c = 0; c < 2; ++c)
    {
        for (i = 0; i + 2 < counts[c]; i += 3)
        {
            const D2D1_POINT_2F *p0, *p1, *p2;
            size_t idx[3], missing = 0;
            int a, b;

            /* A corner on a vertex that was left out of the mesh is looked up as
             * the vertex next to it that the outline runs through: the edge
             * between the two ends of the curve is a seam all the same, and
             * unmarked it got a skirt on top of the curve triangle. */
            for (a = 0; a < 3; ++a)
            {
                idx[a] = d2d_fill_aa_find_mesh_vertex(fill, vertex_map, vertex_map_size, used,
                        &curves[c][i + a].position);
                if (idx[a] == ~(size_t)0)
                    ++missing;
            }

            if (curve_ends && idx[0] != ~(size_t)0 && idx[2] != ~(size_t)0 && used[idx[0]] && used[idx[2]])
            {
                curve_ends[2 * ((c ? counts[0] / 3 : 0) + i / 3)] = idx[0];
                curve_ends[2 * ((c ? counts[0] / 3 : 0) + i / 3) + 1] = idx[2];
            }

            if (!c && d2d_curve_is_flat(&curves[c][i], m))
                continue;

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
                free(used);
                return FALSE;
            }

            for (a = 0; a < 3; ++a)
            {
                for (b = a + 1; b < 3; ++b)
                {
                    UINT32 lo, hi, slot;
                    size_t probe = 0;
                    UINT64 key;

                    if (idx[a] == idx[b])
                        continue;
                    lo = idx[a] < idx[b] ? idx[a] : idx[b];
                    hi = idx[a] < idx[b] ? idx[b] : idx[a];
                    key = ((UINT64)lo << 32) + hi + 1u;
                    slot = d2d_fill_aa_edge_slot(key, map_size);
                    while (probe < map_size && edge_keys[slot] && edge_keys[slot] != key)
                    {
                        slot = d2d_fill_aa_next_slot(slot, map_size);
                        ++probe;
                    }
                    if (probe == map_size)
                    {
                        free(vertex_map);
                        free(used);
                        return FALSE;
                    }
                    if (edge_keys[slot] != key)
                        continue;
                    edge_curve[slot] = 1;

                    p0 = &curves[c][i].position;
                    p1 = &curves[c][i + 1].position;
                    p2 = &curves[c][i + 2].position;
                    if (curves[c][i].texcoord.sign < 0.0f && a == 0 && b == 2)
                    {
                        float ratio = c ? 0.41421356f : 0.5f;
                        float sag = ratio * d2d_fill_aa_line_distance(p1, p0, p2);

                        seams[slot].start = idx[0];
                        seams[slot].c1 = 4.0f * sag;
                        seams[slot].c2 = -4.0f * sag;
                        seams[slot].chord = TRUE;
                        seams[slot].control = *p1;
                    }
                    else if (curves[c][i].texcoord.sign > 0.0f && !c && b == 1)
                    {
                        seams[slot].start = idx[0];
                        seams[slot].c1 = 0.0f;
                        seams[slot].c2 = 0.25f * d2d_fill_aa_line_distance(p2, p0, p1);
                        seams[slot].chord = FALSE;
                    }
                    else if (curves[c][i].texcoord.sign > 0.0f && !c && a == 1)
                    {
                        seams[slot].start = idx[2];
                        seams[slot].c1 = 0.0f;
                        seams[slot].c2 = 0.25f * d2d_fill_aa_line_distance(p0, p1, p2);
                        seams[slot].chord = FALSE;
                    }
                }
            }
        }
    }

    free(vertex_map);
    free(used);
    return TRUE;
}

/* The ramp of face f towards its seam e: t of the three corners along the seam,
 * and the curve's distance from it in units of the edge weight, which reaches 1
 * at the opposite corner. */
static BOOL d2d_fill_aa_seam_params(const struct d2d_fill *fill, const struct d2d_face *f, int e,
        const struct d2d_fill_aa_seam *seam, float *t, float *c1, float *c2)
{
    UINT32 a = f->v[(e + 1) % 3], b = f->v[(e + 2) % 3], end;
    const D2D1_POINT_2F *s, *n, *o;
    float dx, dy, l2, h;
    int j;

    if (seam->start == a)
        end = b;
    else if (seam->start == b)
        end = a;
    else
        return FALSE;

    s = &fill->vertices[seam->start];
    n = &fill->vertices[end];
    o = &fill->vertices[f->v[e]];
    dx = n->x - s->x;
    dy = n->y - s->y;
    if (!((l2 = dx * dx + dy * dy) > 0.0f))
        return FALSE;
    if (!((h = d2d_fill_aa_line_distance(o, s, n)) > 1e-6f))
        return FALSE;

    for (j = 0; j < 3; ++j)
    {
        if (f->v[j] == seam->start)
            t[j] = 0.0f;
        else if (f->v[j] == end)
            t[j] = 1.0f;
        else
            t[j] = ((o->x - s->x) * dx + (o->y - s->y) * dy) / l2;
    }
    *c1 = seam->c1 / h;
    *c2 = seam->c2 / h;
    return TRUE;
}

/* The outline edges next to an edge of the outline.
 *
 * A triangle of the fill mesh is antialiased against its own outline edges:
 * each of its three weights is linear over the triangle, the pixel shader
 * divides it by the length of its gradient and takes the smallest coverage,
 * and the weight of an edge inside the fill is a constant that never limits
 * it. An outline edge that belongs to the next triangle and passes a pixel
 * within half a pixel is not seen, so where the border between two triangles
 * runs flat along the outline the pixels beside it come out too bright: the
 * fan of a bar 2 wide in Minimal Audio Current's tap display divides two
 * pieces of a cap along a line to the centre of the bar, and the two pixels
 * below the cap were drawn at 255 for a coverage of 244 (wine-dev issue 616).
 *
 * The weight of an interior edge can be any linear function, so it carries
 * the distance to the line of the outline edge that meets the triangle's
 * own outline edge at their common vertex. That is done where the outline
 * turns towards the fill at that vertex and at the far end of the
 * neighbouring edge, and the triangle lies on the inner side of the line:
 * the fill around the triangle then lies inside both half planes, and the
 * smaller of the two coverages is still an upper bound of the coverage of
 * the pixel. For a neighbour that is the chord of a curve the line is the
 * tangent of the curve at the vertex.
 *
 * Only the triangles of a fan get it, as d2d_figure_fill_fan() in geometry.c
 * makes them for a convex figure (d2d_fill_aa_mark_fans()). On the faces of
 * the triangulation the same weights darkened single pixels of glyph paths,
 * where the outline turns away from the fill a few edges on (22 of 422
 * changed pixels of 450 glyphs more than 16 of 255 worse, 86 better).
 * WINE_D2D_FILL_AA_NEIGHBOURS=0 disables the weights. */
static BOOL d2d_fill_aa_neighbours(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_NEIGHBOURS", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

struct d2d_fill_aa_outline
{
    const struct d2d_fill *fill;
    const UINT64 *edge_keys;
    const BYTE *edge_curve;
    const struct d2d_fill_aa_seam *seams;
    size_t map_size;
    /* The two outline neighbours of every vertex, plus one, and their
     * number, which is more than two where figures touch. */
    UINT32 *nb;
    BYTE *nb_cnt;
    /* For each of the two, the third vertex of the face the outline edge
     * to it belongs to: the side of the edge the fill lies on. */
    UINT32 *opp;
    /* For d2d_fill_aa_faces(): every vertex in pixels, and for each of the
     * two neighbours the point that gives the direction of the outline edge
     * to it, in pixels, and whether its line is a bound of the fill. */
    double *px, *tangent;
    BYTE *tangent_ok;
};

/* tangent is the point that gives the direction of the edge at a, b or the
 * control point of the curve the edge is the chord of, or NULL for an edge
 * whose line is no bound of the fill. */
static void d2d_fill_aa_outline_add(struct d2d_fill_aa_outline *o, UINT32 a, UINT32 b, UINT32 opp,
        const D2D1_POINT_2F *tangent, const double *m)
{
    if (o->nb_cnt[a] < 2)
    {
        unsigned int k = 2 * a + o->nb_cnt[a];

        o->nb[k] = b + 1;
        if (o->opp)
        {
            o->opp[k] = opp;
            if ((o->tangent_ok[k] = !!tangent))
            {
                o->tangent[2 * k] = m[0] * tangent->x + m[1] * tangent->y + m[2];
                o->tangent[2 * k + 1] = m[3] * tangent->x + m[4] * tangent->y + m[5];
            }
        }
    }
    if (o->nb_cnt[a] < 3)
        ++o->nb_cnt[a];
}

static BOOL d2d_fill_aa_outline_other(const struct d2d_fill_aa_outline *o, UINT32 a, UINT32 b, UINT32 *other)
{
    if (o->nb_cnt[a] != 2)
        return FALSE;
    if (o->nb[2 * a] == b + 1)
        *other = o->nb[2 * a + 1] - 1;
    else if (o->nb[2 * a + 1] == b + 1)
        *other = o->nb[2 * a] - 1;
    else
        return FALSE;
    return *other != b;
}

/* For the triangle (k, a, b) with the outline edge (a, b): the half plane of
 * the outline edge that ends in a, as the unit normal n pointing into it,
 * so that n . (p - a) is the distance of p from its line. own_control is
 * the control point of (a, b) if that is the chord of a curve. */
static BOOL d2d_fill_aa_neighbour_plane(const struct d2d_fill_aa_outline *o, UINT32 k, UINT32 a, UINT32 b,
        const D2D1_POINT_2F *own_control, D2D1_POINT_2F *n, float *dist_k, float *dist_b)
{
    const D2D1_POINT_2F *v = o->fill->vertices, *pa = &v[a], *pb = &v[b], *pk = &v[k], *t;
    float dx, dy, len, tol, side_k, side_t;
    UINT32 x, w, lo, hi, slot;
    UINT64 key;

    if (!d2d_fill_aa_outline_other(o, a, b, &x))
        return FALSE;

    lo = min(a, x);
    hi = max(a, x);
    key = ((UINT64)lo << 32) + hi + 1u;
    slot = d2d_fill_aa_edge_slot(key, o->map_size);
    while (o->edge_keys[slot] != key)
        slot = d2d_fill_aa_next_slot(slot, o->map_size);

    t = &v[x];
    if (o->edge_curve[slot])
    {
        /* A seam whose curve is not known. */
        if (o->seams[slot].start == ~0u)
            return FALSE;
        if (o->seams[slot].chord)
            t = &o->seams[slot].control;
    }

    tol = 1e-4f * (fabsf(pk->x - pa->x) + fabsf(pk->y - pa->y) + fabsf(pb->x - pa->x) + fabsf(pb->y - pa->y));

    /* The outline turns towards the fill at a: the point it comes from lies
     * on the side of (a, b) the triangle lies on. */
    side_k = (pb->x - pa->x) * (pk->y - pa->y) - (pb->y - pa->y) * (pk->x - pa->x);
    side_t = (pb->x - pa->x) * (t->y - pa->y) - (pb->y - pa->y) * (t->x - pa->x);
    if (side_k < 0.0f)
    {
        side_k = -side_k;
        side_t = -side_t;
    }
    if (!(side_k > 0.0f) || !(side_t >= 0.0f))
        return FALSE;

    dx = pa->x - t->x;
    dy = pa->y - t->y;
    if (!((len = sqrtf(dx * dx + dy * dy)) > 0.0f))
        return FALSE;
    n->x = -dy / len;
    n->y = dx / len;
    *dist_k = n->x * (pk->x - pa->x) + n->y * (pk->y - pa->y);
    if (*dist_k < 0.0f)
    {
        n->x = -n->x;
        n->y = -n->y;
        *dist_k = -*dist_k;
    }
    *dist_b = n->x * (pb->x - pa->x) + n->y * (pb->y - pa->y);
    if (!(*dist_k > tol) || !(*dist_b >= -tol))
        return FALSE;

    /* The curve of the triangle's own edge stays inside. */
    if (own_control && !(n->x * (own_control->x - pa->x) + n->y * (own_control->y - pa->y) >= -tol))
        return FALSE;

    /* So does the other end of the neighbouring edge, and what follows it. */
    if (!(n->x * (v[x].x - pa->x) + n->y * (v[x].y - pa->y) >= -tol))
        return FALSE;
    if (!d2d_fill_aa_outline_other(o, x, a, &w))
        return FALSE;
    if (!(n->x * (v[w].x - pa->x) + n->y * (v[w].y - pa->y) >= -tol))
        return FALSE;

    return TRUE;
}

/* The outline edges at the corners of a face of the triangulation.
 *
 * d2d_fill_aa_neighbours() gives the triangles of a fan the outline edges
 * next to their own. A face of the triangulation needs them just as much: one
 * that touches the outline in a corner only has no outline edge of its own at
 * all, and the pixels along the side of a bar that is triangulated with other
 * figures were drawn at 255 for a coverage of 151 (wine-dev issue 638). The
 * weight is the distance from a whole line, though, and the line goes on
 * where the edge ends: where the outline leaves the half plane a few edges
 * on, fill lies beyond the line, and the weight would darken it.
 *
 * So a face takes the half plane of an outline edge that ends in one of its
 * corners only where the fill of that edge lies on the side of the face, and
 * where the outline is followed and found inside the half plane for as far
 * as it matters: along the line, over the part of the face that lies within
 * a pixel of the line. The outline is followed from the corner in both
 * directions; it has to turn towards the fill in the corner, and it may
 * leave the line into the fill for good. A margin past the end of the face
 * was measured and dropped: with one of a pixel a face 12 pixels long lost
 * the half plane of the side of its bar over another figure that meets the
 * bar 0.3 pixels further on. The edges are taken in the order of the angle
 * at which the face leaves them, as many as the face has weights left.
 * WINE_D2D_FILL_AA_FACES=0 leaves the faces of the triangulation with their
 * own edges. */
#define D2D_FILL_AA_FACE_TOLERANCE 0.02
#define D2D_FILL_AA_FACE_STEPS 24

static BOOL d2d_fill_aa_faces(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_FACES", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

/* The vertices in pixels, once for all faces; m is the pixel transform. */
static void d2d_fill_aa_outline_prepare(struct d2d_fill_aa_outline *o, const double *m)
{
    size_t count = o->fill->vertex_count, i;

    for (i = 0; i < count; ++i)
    {
        const D2D1_POINT_2F *p = &o->fill->vertices[i];

        o->px[2 * i] = m[0] * p->x + m[1] * p->y + m[2];
        o->px[2 * i + 1] = m[3] * p->x + m[4] * p->y + m[5];
    }
}

/* Follows the outline from prev through cur and tells whether it stays in
 * the half plane with the unit normal n through q0, all in pixels, until it
 * has reached limit along dir or gone more than a pixel and a half into the
 * half plane. */
static BOOL d2d_fill_aa_outline_inside(const struct d2d_fill_aa_outline *o, UINT32 prev, UINT32 cur,
        const double *q0, const double *dir, const double *n, double limit)
{
    double reach = 0.0;
    unsigned int step;
    UINT32 next;

    for (step = 0; step < D2D_FILL_AA_FACE_STEPS; ++step)
    {
        unsigned int k = o->nb[2 * prev] - 1 == cur ? 0 : 1;
        const double *t = &o->tangent[4 * prev + 2 * k];
        double q[2], s, d;

        q[0] = o->px[2 * cur] - q0[0];
        q[1] = o->px[2 * cur + 1] - q0[1];
        s = q[0] * dir[0] + q[1] * dir[1];
        d = q[0] * n[0] + q[1] * n[1];
        if (d < -D2D_FILL_AA_FACE_TOLERANCE)
            return FALSE;
        /* On the line and back along it: the outline folds over, as it does around a slot between two
         * figures, and what lies beyond the fold is not seen from here. */
        if (d <= D2D_FILL_AA_FACE_TOLERANCE && s < reach - D2D_FILL_AA_FACE_TOLERANCE)
            return FALSE;
        reach = max(reach, s);
        /* The curve between the two lies within the triangle of its control point. */
        if (!o->tangent_ok[2 * prev + k])
            return FALSE;
        if ((t[0] - q0[0]) * n[0] + (t[1] - q0[1]) * n[1] < -D2D_FILL_AA_FACE_TOLERANCE)
            return FALSE;
        /* The limit is as a rule a corner of the face that lies on the outline itself, and the outline
         * arrives right there. */
        if (s >= limit - 1.0e-6 || d > 1.5)
            return TRUE;
        if (!d2d_fill_aa_outline_other(o, cur, prev, &next))
            return FALSE;
        prev = cur;
        cur = next;
    }
    return FALSE;
}

/* Gives the weights of the face f that are not taken, free, the half planes
 * of the outline edges at its corners; see d2d_fill_aa_faces().
 *
 * For every corner on the outline and each of the two outline edges there:
 * the edge is not one of the face, its line is a bound of the fill, and the
 * face lies on the side of the line the fill of the edge lies on. That takes
 * no square root, nor does the order: the square of the sine of the angle at
 * which the face leaves the line, with its sign. The outline is followed for as many of
 * them, in that order, as the face has weights left. */
static void d2d_fill_aa_face_planes(const struct d2d_fill_aa_outline *o, const struct d2d_face *f,
        const BOOL *free, float (*w)[3])
{
    struct
    {
        double sine2, dir[2], len2, c[2], s[2], side;
        UINT32 a;
        unsigned int i, k;
    } cand[6];
    unsigned int count = 0, left = free[0] + free[1] + free[2], i, j, k, slot = 0, order[6];
    const double tol = D2D_FILL_AA_FACE_TOLERANCE;
    double q[3][2];

    if (!left || (o->nb_cnt[f->v[0]] != 2 && o->nb_cnt[f->v[1]] != 2 && o->nb_cnt[f->v[2]] != 2))
        return;
    for (i = 0; i < 3; ++i)
    {
        q[i][0] = o->px[2 * f->v[i]];
        q[i][1] = o->px[2 * f->v[i] + 1];
    }
    for (i = 0; i < 3; ++i)
    {
        UINT32 a = f->v[i];
        unsigned int j1 = (i + 1) % 3, j2 = (i + 2) % 3;

        if (o->nb_cnt[a] != 2)
            continue;
        for (k = 0; k < 2; ++k)
        {
            UINT32 x = o->nb[2 * a + k] - 1;
            const double *t = &o->tangent[4 * a + 2 * k], *po = &o->px[2 * o->opp[2 * a + k]];
            double dx, dy, side, c1, c2, s1, s2, tol2, e1, e2;

            /* An edge of the face itself has its own weight. */
            if (x == f->v[j1] || x == f->v[j2] || x == o->nb[2 * a + 1 - k] - 1 || !o->tangent_ok[2 * a + k])
                continue;
            dx = t[0] - q[i][0];
            dy = t[1] - q[i][1];
            /* Twice the areas the corners span with the edge, signed towards its fill. */
            side = dx * (po[1] - q[i][1]) - dy * (po[0] - q[i][0]);
            if (side == 0.0)
                continue;
            side = side < 0.0 ? -1.0 : 1.0;
            c1 = side * (dx * (q[j1][1] - q[i][1]) - dy * (q[j1][0] - q[i][0]));
            c2 = side * (dx * (q[j2][1] - q[i][1]) - dy * (q[j2][0] - q[i][0]));
            tol2 = tol * tol * (dx * dx + dy * dy);
            if ((c1 < 0.0 && c1 * c1 > tol2) || (c2 < 0.0 && c2 * c2 > tol2))
                continue;
            if (!(max(c1, c2) > 0.0 && max(c1, c2) * max(c1, c2) > tol2))
                continue;
            s1 = dx * (q[j1][0] - q[i][0]) + dy * (q[j1][1] - q[i][1]);
            s2 = dx * (q[j2][0] - q[i][0]) + dy * (q[j2][1] - q[i][1]);
            e1 = c1 * fabs(c1) / (s1 * s1 + c1 * c1 + 1.0e-30);
            e2 = c2 * fabs(c2) / (s2 * s2 + c2 * c2 + 1.0e-30);

            cand[count].sine2 = min(e1, e2);
            cand[count].dir[0] = dx;
            cand[count].dir[1] = dy;
            cand[count].len2 = dx * dx + dy * dy;
            cand[count].c[0] = c1;
            cand[count].c[1] = c2;
            cand[count].s[0] = s1;
            cand[count].s[1] = s2;
            cand[count].side = side;
            cand[count].a = a;
            cand[count].i = i;
            cand[count].k = k;
            ++count;
        }
    }
    for (i = 0; i < count; ++i)
    {
        for (j = i; j && cand[order[j - 1]].sine2 > cand[i].sine2; --j)
            order[j] = order[j - 1];
        order[j] = i;
    }

    for (j = 0; j < count && left; ++j)
    {
        const double len = sqrt(cand[order[j]].len2), inv = 1.0 / len, *c = cand[order[j]].c, *cs = cand[order[j]].s;
        double dir[2], back[2], n[2], s[3], d[3], lo = 0.0, hi = 0.0;
        UINT32 a = cand[order[j]].a;
        unsigned int l;

        if (!(len > 1.0e-6))
            continue;
        i = cand[order[j]].i;
        k = cand[order[j]].k;
        dir[0] = cand[order[j]].dir[0] * inv;
        dir[1] = cand[order[j]].dir[1] * inv;
        n[0] = -cand[order[j]].side * dir[1];
        n[1] = cand[order[j]].side * dir[0];
        s[i] = d[i] = 0.0;
        s[(i + 1) % 3] = cs[0] * inv;
        s[(i + 2) % 3] = cs[1] * inv;
        d[(i + 1) % 3] = c[0] * inv;
        d[(i + 2) % 3] = c[1] * inv;

        /* How far the part of the face within a pixel of the line reaches along it. */
        for (l = 0; l < 3; ++l)
        {
            unsigned int e = (l + 1) % 3;

            if (d[l] <= 1.0)
            {
                lo = min(lo, s[l]);
                hi = max(hi, s[l]);
            }
            if ((d[l] < 1.0) != (d[e] < 1.0))
            {
                double x = s[l] + (s[e] - s[l]) * (1.0 - d[l]) / (d[e] - d[l]);

                lo = min(lo, x);
                hi = max(hi, x);
            }
        }

        /* The outline turns towards the fill in the corner and stays inside from there on, in both directions. */
        back[0] = -dir[0];
        back[1] = -dir[1];
        if (!d2d_fill_aa_outline_inside(o, a, o->nb[2 * a + k] - 1, q[i], dir, n, hi)
                || !d2d_fill_aa_outline_inside(o, a, o->nb[2 * a + 1 - k] - 1, q[i], back, n, -lo))
            continue;

        while (!free[slot])
            ++slot;
        for (l = 0; l < 3; ++l)
            w[slot][l] = d[l];
        ++slot;
        --left;
    }
}

/* Marks the faces of the fans: runs of consecutive faces around the same
 * first vertex whose opposite edges follow each other and close. The caller
 * checks that those edges lie on the outline and the vertex does not. */
static void d2d_fill_aa_mark_fans(const struct d2d_fill *fill, BYTE *fan)
{
    const struct d2d_face *f = fill->faces;
    size_t fc = fill->face_count, i, j, k;

    for (i = 0; i < fc; i = j)
    {
        BOOL forward = TRUE, backward = TRUE;

        for (j = i + 1; j < fc && f[j].v[0] == f[i].v[0]; ++j)
        {
            if (f[j].v[1] != f[j - 1].v[2])
                forward = FALSE;
            if (f[j].v[2] != f[j - 1].v[1])
                backward = FALSE;
        }
        if (j - i < 3)
            continue;
        if (!(forward && f[i].v[1] == f[j - 1].v[2]) && !(backward && f[i].v[2] == f[j - 1].v[1]))
            continue;
        for (k = i; k < j; ++k)
            fan[k] = 1;
    }
}

/* The wedge between the skirts of two straight outline edges.
 *
 * Every straight outline edge gets a skirt, a quad that reaches 0.75 pixels
 * outward at a right angle to the edge. Where two such edges meet in a corner
 * of the fill that points outward, the skirts end on two different normals,
 * and beyond the corner a wedge as wide as the turn belongs to neither: a
 * pixel centre in it got no coverage. Next to a right angle that is a quarter
 * of a pixel at most. Next to the corners of a polygon that stands for a
 * curve it is close to a half: a 24-gon of radius 6 had the pixel beside a
 * corner at 0 of 255 for a coverage of 119, and so had the polygons a path
 * of very small arcs is drawn as, whose pieces are filled as their chords
 * (d2d_curve_is_flat(); wine-dev issue 635).
 *
 * The wedge is filled with one triangle between the corner and the two outer
 * vertices the skirts have there. Its vertices are those of the skirts, the
 * corner and the same two directions for the vertex shader to move it along,
 * so that each of its two long edges is an edge of a skirt as well and the
 * rasteriser gives a pixel centre on it to exactly one of the two. A triangle
 * that holds no pixel centre is left out, and so is a turn of more than 143
 * degrees, where the fill ends in a spike.
 *
 * The one weight of the triangle is the distance in pixels from a line across
 * the wedge, at a right angle to the direction halfway between the two
 * normals: the line through the corner, moved inward by what the outline
 * falls back from it on average over the width of a pixel, half a pixel
 * along the outline to either side. The pixel shader takes that line for an
 * edge across the whole pixel.
 *
 * A straight outline edge that meets a curve has the same wedge between its
 * skirt and the skirt of the curve triangle (d2d_device_context_curve_skirts()),
 * as wide as the curve piece is off the direction of the edge where it starts:
 * the first quadratic piece of an arc segment that goes on from a line does
 * not start in its direction, and a pixel centre on the normal through the
 * point where the two meet came out at 0 of 255 for a coverage of 102. The
 * triangle then has the outer vertex of the curve skirt for its third corner,
 * at the position d2d_curve_add_skirt() gives it, and the curve falls back
 * from the line by its slope and by 1/48 of its curvature over half a pixel.
 * Only the Bézier triangles that are drawn with skirts across their control
 * edges count. WINE_D2D_FILL_AA_CORNERS=0 leaves all the wedges open. */
#define D2D_FILL_AA_CORNER_COS -0.8
#define D2D_FILL_AA_CORNER_STEPS 8

struct d2d_fill_aa_corner
{
    /* The straight outline edges with a skirt that end in the vertex: their
     * other ends, the directions their skirts reach out in, and their number,
     * which is more than two where figures touch. */
    UINT32 other[2];
    D2D1_POINT_2F n[2];
    /* Their places among the bridged edges, see d2d_fill_aa_bridges(). */
    UINT32 edge[2];
    BYTE count;
};

static BOOL d2d_fill_aa_corners(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_CORNERS", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

static void d2d_fill_aa_corner_add(struct d2d_fill_aa_corner *c, UINT32 other, const D2D1_POINT_2F *n, UINT32 edge)
{
    if (c->count < 2)
    {
        c->other[c->count] = other;
        c->n[c->count] = *n;
        c->edge[c->count] = edge;
    }
    if (c->count < 3)
        ++c->count;
}

/* How far the outline falls back from the line through the vertex v with the
 * normal nb, in pixels, summed over half a pixel along that line: along the
 * outline from v through its neighbour next. q0 is v in pixels, m the pixel
 * transform. Where the outline ends or turns back before half a pixel, the
 * last edge is taken to go on. */
static double d2d_fill_aa_corner_depth(const struct d2d_fill *fill, const struct d2d_fill_aa_outline *o,
        const double *m, UINT32 v, UINT32 next, const double *q0, const double *nb)
{
    double xa = 0.0, da = 0.0, slope = 0.0, sum = 0.0, sign = 0.0, w;
    UINT32 prev = v, cur = next, other;
    unsigned int step;

    for (step = 0; step < D2D_FILL_AA_CORNER_STEPS; ++step)
    {
        const D2D1_POINT_2F *p = &fill->vertices[cur];
        double px = m[0] * p->x + m[1] * p->y + m[2] - q0[0], py = m[3] * p->x + m[4] * p->y + m[5] - q0[1];
        double xb = px * -nb[1] + py * nb[0], db = -(px * nb[0] + py * nb[1]);

        if (!step)
            sign = xb < 0.0 ? -1.0 : 1.0;
        xb *= sign;
        if (!(xb > xa + 1.0e-9))
            break;
        slope = (db - da) / (xb - xa);
        if (xb >= 0.5)
            return sum + (da + 0.5 * slope * (0.5 - xa)) * (0.5 - xa);
        sum += 0.5 * (da + db) * (xb - xa);
        xa = xb;
        da = db;
        if (!o->nb || !d2d_fill_aa_outline_other(o, cur, prev, &other))
            break;
        prev = cur;
        cur = other;
    }
    w = 0.5 - xa;
    return sum + (da + 0.5 * slope * w) * w;
}

static BOOL d2d_curve_triangle_holds_centre(const double *a, const double *b, const double *c);
static unsigned int d2d_curve_add_skirt(struct d2d_curve_vertex *out, const struct d2d_curve_vertex *v,
        unsigned int i0, unsigned int i1, unsigned int io, const float *m, const float *inv, float *normal);

/* A part of a fill thinner than a pixel.
 *
 * The coverage of a fragment is the smallest of the areas its triangle's
 * outline edges leave of the pixel, each taken as if that edge were the only
 * one. For two edges on opposite sides of a figure that pass through the same
 * pixel that is not the area between them: a pixel centre inside a bar 0.09
 * high lies within 0.05 of both long edges, each of them leaves a little more
 * than half of the pixel, and the pixel came out at 134 of 255 for a coverage
 * of 21. A centre just outside got what the nearer edge leaves, 102 for a bar
 * 0.045 high at a distance of 0.1 (Minimal Audio Current's tap display,
 * wine-dev issue 615).
 *
 * The area a fill covers of a pixel is the sum of the areas its triangles
 * cover of it, and a triangle is clipped to a pixel in four steps. So for a
 * connected part of the fill mesh that lies within a strip
 * D2D_FILL_AA_THIN_WIDTH pixels wide, the pixels it touches are computed
 * here, and each is drawn as a quad of its own instead of the triangles and
 * skirts of that part: the quad covers the pixel, and its one weight rises by
 * 1 per pixel from left to right and is the area less a half at the centre,
 * which the pixel shader turns into that area. Outside such a strip two
 * opposite edges do not meet in one pixel.
 *
 * The curve triangles along such a part belong to its area: the piece
 * between a curve and its chord where the control point lies outside the
 * fill, the piece between the curve and its two control edges where it lies
 * inside. They are added as the triangles of the curve cut into chords, the
 * flat ones included, which the fill otherwise draws as their chord
 * (d2d_curve_is_flat()), and the curve triangles are not drawn.
 *
 * Left as they are: a part of more than D2D_FILL_AA_THIN_MAX_VERTICES
 * vertices, one whose bounds hold more than D2D_FILL_AA_THIN_MAX_CELLS pixels
 * or that touches more than D2D_FILL_AA_THIN_MAX_PIXELS, and one with an
 * arc whose corner lies inside the fill. WINE_D2D_FILL_AA_THIN=0 leaves all
 * of them. */
#define D2D_FILL_AA_THIN_WIDTH 1.5
#define D2D_FILL_AA_THIN_MAX_VERTICES 256
#define D2D_FILL_AA_THIN_MAX_CELLS 4096
#define D2D_FILL_AA_THIN_MAX_PIXELS 256
#define D2D_FILL_AA_THIN_MAX_CHORDS 16

static BOOL d2d_fill_aa_thin(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_THIN", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

struct d2d_fill_aa_pixel
{
    int x, y;
    float coverage;
};

/* The mapping from geometry space to render target pixels with its
 * translation, pixel i covering [i, i + 1]: px = m[0] * x + m[1] * y + m[2],
 * py = m[3] * x + m[4] * y + m[5]. */
static void d2d_device_context_get_pixel_transform(const struct d2d_device_context *context,
        const D2D1_MATRIX_3X2_F *g, double *m)
{
    double sx = context->desc.dpiX / 96.0f, sy = context->desc.dpiY / 96.0f;
    D2D1_MATRIX_3X2_F t = *g;

    d2d_matrix_multiply(&t, &context->drawing_state.transform);
    m[0] = sx * t._11;
    m[1] = sx * t._21;
    m[2] = sx * t._31;
    m[3] = sy * t._12;
    m[4] = sy * t._22;
    m[5] = sy * t._32;
}

/* Clips a convex polygon to one side of an axis-parallel line, which adds at
 * most one vertex; out has room for D2D_FILL_AA_THIN_CLIP_SIZE of them, more
 * than a triangle clipped to a pixel has. Returns the number of vertices
 * written. */
#define D2D_FILL_AA_THIN_CLIP_SIZE 16
static size_t d2d_fill_aa_clip_polygon(const double (*in)[2], size_t count, unsigned int axis, double bound,
        BOOL below, double (*out)[2])
{
    size_t i, n = 0;

    for (i = 0; i < count; ++i)
    {
        const double *p = in[i], *q = in[i + 1 == count ? 0 : i + 1];
        double dp = below ? bound - p[axis] : p[axis] - bound;
        double dq = below ? bound - q[axis] : q[axis] - bound;

        if (n + 2 > D2D_FILL_AA_THIN_CLIP_SIZE)
            break;
        if (dp >= 0.0)
        {
            out[n][0] = p[0];
            out[n][1] = p[1];
            ++n;
        }
        if ((dp >= 0.0) != (dq >= 0.0))
        {
            double t = dp / (dp - dq);

            out[n][0] = p[0] + t * (q[0] - p[0]);
            out[n][1] = p[1] + t * (q[1] - p[1]);
            out[n][axis] = bound;
            ++n;
        }
    }
    return n;
}

/* Adds the area the triangle t covers of each pixel to grid, a map of
 * cw x ch pixels from (gx, gy). */
static void d2d_fill_aa_add_triangle(const double (*t)[2], int gx, int gy, int cw, int ch, double *grid)
{
    double row[2][D2D_FILL_AA_THIN_CLIP_SIZE][2], cell[2][D2D_FILL_AA_THIN_CLIP_SIZE][2];
    double left, right, top, bottom, area;
    int x, y, x0, x1, y0, y1;
    size_t i, k, n;

    top = min(t[0][1], min(t[1][1], t[2][1]));
    bottom = max(t[0][1], max(t[1][1], t[2][1]));
    y0 = max((int)floor(top), gy);
    y1 = min((int)floor(bottom), gy + ch - 1);
    for (y = y0; y <= y1; ++y)
    {
        n = d2d_fill_aa_clip_polygon(t, 3, 1, y, FALSE, row[0]);
        n = d2d_fill_aa_clip_polygon(row[0], n, 1, y + 1.0, TRUE, row[1]);
        if (n < 3)
            continue;
        left = right = row[1][0][0];
        for (i = 1; i < n; ++i)
        {
            left = min(left, row[1][i][0]);
            right = max(right, row[1][i][0]);
        }
        x0 = max((int)floor(left), gx);
        x1 = min((int)floor(right), gx + cw - 1);
        for (x = x0; x <= x1; ++x)
        {
            k = d2d_fill_aa_clip_polygon(row[1], n, 0, x, FALSE, cell[0]);
            k = d2d_fill_aa_clip_polygon(cell[0], k, 0, x + 1.0, TRUE, cell[1]);
            if (k < 3)
                continue;
            area = 0.0;
            for (i = 0; i < k; ++i)
            {
                const double *a = cell[1][i], *b = cell[1][i + 1 == k ? 0 : i + 1];

                area += (a[0] - x) * (b[1] - y) - (b[0] - x) * (a[1] - y);
            }
            grid[(size_t)(y - gy) * cw + (x - gx)] += 0.5 * fabs(area);
        }
    }
}

static int __cdecl d2d_fill_aa_pixel_compare(const void *a, const void *b)
{
    const struct d2d_fill_aa_pixel *pa = a, *pb = b;

    if (pa->y != pb->y)
        return pa->y < pb->y ? -1 : 1;
    return pa->x < pb->x ? -1 : pa->x > pb->x;
}

/* Two parts that reach into the same pixel cover the sum of their areas of
 * it. Drawn as two quads they would be blended, which comes out darker: two
 * bars 0.385 high in one pixel row gave 158 for a coverage of 196. */
static size_t d2d_fill_aa_merge_pixels(struct d2d_fill_aa_pixel *pixels, size_t count)
{
    size_t i, n;

    if (!count)
        return 0;
    qsort(pixels, count, sizeof(*pixels), d2d_fill_aa_pixel_compare);
    for (i = 1, n = 0; i < count; ++i)
    {
        if (pixels[i].x == pixels[n].x && pixels[i].y == pixels[n].y)
            pixels[n].coverage = min(pixels[n].coverage + pixels[i].coverage, 1.0f);
        else
            pixels[++n] = pixels[i];
    }
    return n + 1;
}

static UINT32 d2d_fill_aa_find_part(UINT32 *parent, UINT32 i)
{
    while (parent[i] != i)
    {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

/* The corners of curve triangle v in pixels and the number of chords its
 * curve is cut into, which leaves them within 1/64 of a pixel of it. The
 * curve is farthest from its chord half way to the control point for a
 * Bézier and sqrt(2) - 1 of the way for an arc. */
static unsigned int d2d_fill_aa_curve_chords(const struct d2d_curve_vertex *v, BOOL arc, const double *m,
        double (*q)[2])
{
    double dx, dy, l, sag;
    unsigned int k;

    for (k = 0; k < 3; ++k)
    {
        q[k][0] = m[0] * v[k].position.x + m[1] * v[k].position.y + m[2];
        q[k][1] = m[3] * v[k].position.x + m[4] * v[k].position.y + m[5];
    }
    dx = q[2][0] - q[0][0];
    dy = q[2][1] - q[0][1];
    if (!((l = sqrt(dx * dx + dy * dy)) > 0.0))
        return 1;
    sag = (arc ? 0.41421356 : 0.5) * fabs(dx * (q[1][1] - q[0][1]) - dy * (q[1][0] - q[0][0])) / l;
    /* Negated so that a distance that is not a number ends up with one chord. */
    if (!(sag > 1.0 / 64.0))
        return 1;
    if (!(sag < D2D_FILL_AA_THIN_MAX_CHORDS * D2D_FILL_AA_THIN_MAX_CHORDS / 64.0))
        return D2D_FILL_AA_THIN_MAX_CHORDS;
    return (unsigned int)ceil(sqrt(64.0 * sag));
}

/* Point i of count on the curve of a curve triangle with the corners q. The
 * arc is the quarter of an ellipse that touches the two edges at the corner
 * q[1] in q[0] and q[2]. */
static void d2d_fill_aa_curve_point(const double (*q)[2], BOOL arc, unsigned int i, unsigned int count, double *p)
{
    double t = (double)i / count, a, b, c;

    if (arc)
    {
        a = 1.0 - sin(t * M_PI / 2.0);
        b = 1.0 - cos(t * M_PI / 2.0);
        p[0] = q[1][0] - a * (q[1][0] - q[0][0]) - b * (q[1][0] - q[2][0]);
        p[1] = q[1][1] - a * (q[1][1] - q[0][1]) - b * (q[1][1] - q[2][1]);
        return;
    }
    a = (1.0 - t) * (1.0 - t);
    b = 2.0 * t * (1.0 - t);
    c = t * t;
    p[0] = a * q[0][0] + b * q[1][0] + c * q[2][0];
    p[1] = a * q[0][1] + b * q[1][1] + c * q[2][1];
}

/* Marks the faces of the parts of the fill that are drawn by their pixels and
 * appends those pixels; see d2d_fill_aa_thin(). m is the pixel transform and
 * mf its linear part as d2d_curve_is_flat() takes it. curve_ends is what
 * d2d_fill_aa_mark_curve_edges() returns, and curve_skip receives the curve
 * triangles that are not to be drawn; both are NULL for a fill without
 * curves. */
static void d2d_fill_aa_mark_thin(const struct d2d_device_context *context, const struct d2d_fill *fill,
        const UINT64 *edge_keys, const BYTE *edge_cnt, size_t map_size,
        const double *m, const float *mf, const UINT32 *curve_ends, BYTE *curve_skip, BYTE *thin,
        struct d2d_fill_aa_pixel **pixels, size_t *pixel_count, size_t *pixels_size)
{
    size_t bezier_count = fill->bezier_vertex_count / 3, curve_count = bezier_count + fill->arc_vertex_count / 3;
    int target_w = context->pixel_size.width, target_h = context->pixel_size.height;
    UINT32 *parent, *head, *next, *stamp, *curve_head = NULL, *curve_next = NULL, serial = 0, r;
    size_t vc = fill->vertex_count, fc = fill->face_count, i, k, n;
    const struct d2d_face *f = fill->faces;
    double *grid;

    if (!curve_ends || !curve_skip)
        curve_count = 0;

    parent = malloc(vc * sizeof(*parent));
    head = calloc(vc, sizeof(*head));
    next = malloc(fc * sizeof(*next));
    stamp = calloc(vc, sizeof(*stamp));
    grid = malloc(D2D_FILL_AA_THIN_MAX_CELLS * sizeof(*grid));
    if (curve_count)
    {
        curve_head = calloc(vc, sizeof(*curve_head));
        curve_next = malloc(curve_count * sizeof(*curve_next));
    }
    if (!parent || !head || !next || !stamp || !grid || vc >= 1u << 22
            || (curve_count && (!curve_head || !curve_next)))
        goto done;

    /* The parts: faces that share a vertex. head and next hold face indices
     * plus one. */
    for (i = 0; i < vc; ++i)
        parent[i] = i;
    for (i = 0; i < fc; ++i)
    {
        UINT32 a = d2d_fill_aa_find_part(parent, f[i].v[0]), b;

        for (k = 1; k < 3; ++k)
        {
            if ((b = d2d_fill_aa_find_part(parent, f[i].v[k])) != a)
                parent[b] = a;
        }
    }
    for (i = fc; i--;)
    {
        r = d2d_fill_aa_find_part(parent, f[i].v[0]);
        next[i] = head[r];
        head[r] = i + 1;
    }
    /* The curve triangles of a part, likewise: those whose curve ends on two
     * of its vertices. A curve triangle that is drawn and whose part is not
     * known would be drawn on top of the pixels of that part, so nothing is
     * drawn by its pixels then. */
    for (i = curve_count; i--;)
    {
        if (curve_ends[2 * i] == ~0u || (r = d2d_fill_aa_find_part(parent, curve_ends[2 * i]))
                != d2d_fill_aa_find_part(parent, curve_ends[2 * i + 1]))
        {
            if (i >= bezier_count || !d2d_curve_is_flat(&fill->bezier_vertices[3 * i], mf))
                goto done;
            continue;
        }
        curve_next[i] = curve_head[r];
        curve_head[r] = i + 1;
    }

    for (r = 0; r < vc; ++r)
    {
        double p[D2D_FILL_AA_THIN_MAX_VERTICES][2], left, top, right, bottom, width = DBL_MAX, area = 0.0;
        UINT32 edges[3 * D2D_FILL_AA_THIN_MAX_VERTICES][2];
        size_t vertex_count = 0, edge_count = 0, count;
        int gx, gy, cw = 0, ch = 0;
        BOOL ok = TRUE;

        if (!head[r])
            continue;
        ++serial;

        /* The vertices of the part in pixels and its outline edges. stamp
         * holds the number of the part and the place of the vertex in p. */
        for (i = head[r]; i && ok; i = next[i - 1])
        {
            int e;

            for (e = 0; e < 3; ++e)
            {
                UINT32 a = f[i - 1].v[(e + 1) % 3], b = f[i - 1].v[(e + 2) % 3], c = f[i - 1].v[e], slot;
                UINT64 key = ((UINT64)min(a, b) << 32) + max(a, b) + 1u;

                if (stamp[c] >> 9 != serial)
                {
                    const D2D1_POINT_2F *v = &fill->vertices[c];

                    if (vertex_count == D2D_FILL_AA_THIN_MAX_VERTICES)
                    {
                        ok = FALSE;
                        break;
                    }
                    stamp[c] = (serial << 9) | vertex_count;
                    p[vertex_count][0] = m[0] * v->x + m[1] * v->y + m[2];
                    p[vertex_count][1] = m[3] * v->x + m[4] * v->y + m[5];
                    ++vertex_count;
                }

                slot = d2d_fill_aa_edge_slot(key, map_size);
                while (edge_keys[slot] != key)
                    slot = d2d_fill_aa_next_slot(slot, map_size);
                if (edge_cnt[slot] != 1)
                    continue;
                if (edge_count == ARRAY_SIZE(edges))
                {
                    ok = FALSE;
                    break;
                }
                edges[edge_count][0] = a;
                edges[edge_count][1] = b;
                ++edge_count;
            }
        }
        if (!ok || !edge_count)
            continue;

        left = right = p[0][0];
        top = bottom = p[0][1];
        for (k = 1; k < vertex_count; ++k)
        {
            left = min(left, p[k][0]);
            right = max(right, p[k][0]);
            top = min(top, p[k][1]);
            bottom = max(bottom, p[k][1]);
        }

        /* The curve triangles of the part reach as far as their control
         * points. An arc with its corner inside the fill is not handled. */
        for (i = curve_count ? curve_head[r] : 0; i; i = curve_next[i - 1])
        {
            BOOL arc = i - 1 >= bezier_count;
            const struct d2d_curve_vertex *v = arc ? &fill->arc_vertices[3 * (i - 1 - bezier_count)]
                    : &fill->bezier_vertices[3 * (i - 1)];
            double q[3][2];

            if (arc && v->texcoord.sign > 0.0f)
                break;
            d2d_fill_aa_curve_chords(v, arc, m, q);
            left = min(left, q[1][0]);
            right = max(right, q[1][0]);
            top = min(top, q[1][1]);
            bottom = max(bottom, q[1][1]);
        }
        if (i)
            continue;
        /* Negated so that coordinates that are not numbers end up here. */
        if (!(fabs(left) < 1.0e6) || !(fabs(right) < 1.0e6) || !(fabs(top) < 1.0e6) || !(fabs(bottom) < 1.0e6))
            continue;

        /* What lies within a strip is no larger than the strip is wide times
         * the diagonal of the bounds. */
        for (i = head[r]; i; i = next[i - 1])
        {
            const double *a = p[stamp[f[i - 1].v[0]] & 511], *b = p[stamp[f[i - 1].v[1]] & 511];
            const double *c = p[stamp[f[i - 1].v[2]] & 511];

            area += 0.5 * fabs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
        }
        if (!(area <= D2D_FILL_AA_THIN_WIDTH * sqrt((right - left) * (right - left) + (bottom - top) * (bottom - top))))
            continue;

        /* The part lies within a strip along one of its outline edges. With
         * no edge longer than a thousandth of a pixel it is a point. */
        ok = FALSE;
        for (k = 0; k < edge_count && !(width < D2D_FILL_AA_THIN_WIDTH); ++k)
        {
            const double *a = p[stamp[edges[k][0]] & 511], *b = p[stamp[edges[k][1]] & 511];
            double dx = b[0] - a[0], dy = b[1] - a[1], l = sqrt(dx * dx + dy * dy), w = 0.0;

            if (!(l > 1.0e-3))
                continue;
            ok = TRUE;
            for (n = 0; n < vertex_count; ++n)
                w = max(w, fabs(dx * (p[n][1] - a[1]) - dy * (p[n][0] - a[0])) / l);
            width = min(width, w);
        }
        if (ok && !(width < D2D_FILL_AA_THIN_WIDTH))
            continue;

        gx = max((int)floor(left), 0);
        gy = max((int)floor(top), 0);
        if ((int)floor(right) >= gx && (int)floor(bottom) >= gy && gx < target_w && gy < target_h)
        {
            cw = min((int)floor(right), target_w - 1) - gx + 1;
            ch = min((int)floor(bottom), target_h - 1) - gy + 1;
            if ((size_t)cw * ch > D2D_FILL_AA_THIN_MAX_CELLS)
                continue;
            memset(grid, 0, (size_t)cw * ch * sizeof(*grid));
            for (i = head[r]; i; i = next[i - 1])
            {
                double t[3][2];

                for (k = 0; k < 3; ++k)
                {
                    t[k][0] = p[stamp[f[i - 1].v[k]] & 511][0];
                    t[k][1] = p[stamp[f[i - 1].v[k]] & 511][1];
                }
                d2d_fill_aa_add_triangle(t, gx, gy, cw, ch, grid);
            }
            for (i = curve_count ? curve_head[r] : 0; i; i = curve_next[i - 1])
            {
                BOOL arc = i - 1 >= bezier_count;
                const struct d2d_curve_vertex *v = arc ? &fill->arc_vertices[3 * (i - 1 - bezier_count)]
                        : &fill->bezier_vertices[3 * (i - 1)];
                double q[3][2], t[3][2];
                unsigned int chords, c;

                /* Around the first corner for a piece between the curve and
                 * its chord, around the control point for one between the
                 * curve and the control edges. */
                chords = d2d_fill_aa_curve_chords(v, arc, m, q);
                t[0][0] = q[v->texcoord.sign > 0.0f][0];
                t[0][1] = q[v->texcoord.sign > 0.0f][1];
                for (c = v->texcoord.sign > 0.0f ? 0 : 1; c < chords; ++c)
                {
                    d2d_fill_aa_curve_point(q, arc, c, chords, t[1]);
                    d2d_fill_aa_curve_point(q, arc, c + 1, chords, t[2]);
                    d2d_fill_aa_add_triangle(t, gx, gy, cw, ch, grid);
                }
            }
        }

        /* A pixel covered by less than a quarter of a step of an 8 bit
         * channel is left out. */
        for (k = 0, count = 0; k < (size_t)cw * ch; ++k)
            count += grid[k] > 0.001;
        if (count > D2D_FILL_AA_THIN_MAX_PIXELS
                || !d2d_array_reserve((void **)pixels, pixels_size, *pixel_count + count, sizeof(**pixels)))
            continue;
        for (k = 0; k < (size_t)cw * ch; ++k)
        {
            if (!(grid[k] > 0.001))
                continue;
            (*pixels)[*pixel_count].x = gx + k % cw;
            (*pixels)[*pixel_count].y = gy + k / cw;
            (*pixels)[*pixel_count].coverage = min(grid[k], 1.0);
            ++*pixel_count;
        }
        for (i = head[r]; i; i = next[i - 1])
            thin[i - 1] = 1;
        for (i = curve_count ? curve_head[r] : 0; i; i = curve_next[i - 1])
            curve_skip[i - 1] = 1;
    }

done:
    free(parent);
    free(head);
    free(next);
    free(stamp);
    free(grid);
    free(curve_head);
    free(curve_next);
}

/* Build and draw fill-AA geometry. Returns TRUE on success, FALSE if caller should fall back. */
/* Fill-edge AA: per-face vertices with boundary edge weights + skirt expansion.
 * Inner triangles: bary coords identify boundary edges.
 * Skirt quads: extend boundary edges ~0.75px outward for full AA transition. */
struct d2d_fill_aa_vertex
{
    D2D1_POINT_2F pos;
    float u, v, s;
    float ex, ey;
    float seam_t[3], seam_c1[3], seam_c2[3];
    /* corner_on is 0 for a corner off the outline, 1 for one on it, and the
     * code of the notch, at least 2, at a corner that points inward; see
     * d2d_fill_aa_notch_code(). */
    float corner[3][2], corner_on[3];
};

/* The skirt of the straight outline edge from a to b: two triangles, the
 * outer vertices are moved 0.75 pixels along outward by the vertex shader. */
static void d2d_fill_aa_write_skirt(struct d2d_fill_aa_vertex *v, const D2D1_POINT_2F *a, const D2D1_POINT_2F *b,
        const D2D1_POINT_2F *outward)
{
    static const BYTE end[6] = {0, 1, 0, 1, 1, 0}, outer[6] = {0, 0, 1, 0, 1, 1};
    unsigned int k;

    for (k = 0; k < 6; ++k)
    {
        v[k].pos = end[k] ? *b : *a;
        v[k].u = outer[k] ? -1.0f : 0.0f;
        v[k].v = 10.0f;
        v[k].s = 10.0f;
        v[k].ex = outer[k] ? outward->x : 0.0f;
        v[k].ey = outer[k] ? outward->y : 0.0f;
    }
}

/* Bridges over the places where the outline turns inward by less than
 * D2D_FILL_AA_BRIDGE_DEPTH pixels.
 *
 * The skirts of two straight outline edges that meet in a corner pointing
 * inward overlap beyond it, and a pixel centre in the overlap is drawn by
 * both. Next to a deep corner that is close to what the two edges cover of
 * the pixel together. Where the outline is merely jagged below the size of a
 * pixel it is not: the union of figures that overlap with almost the same
 * border, as the dots of Minimal Audio Current's tap display that stand on
 * one baseline, has edges of a hundredth of a pixel there whose skirts, 0.75
 * pixels long, fan out in every direction and lie on top of each other, and
 * so do the triangles that fill the wedges at the corners in between
 * (d2d_fill_aa_corners()). Four such dots that cover 60 of 255 of a pixel
 * came out at 107 (wine-dev issue 637).
 *
 * So a vertex at which the outline turns inward is left out of the outline
 * the skirts are made for, as long as it and what was left out before lie
 * within that depth of the line between its two neighbours: the two edges
 * become one, with one skirt. What is left is the outline without its dents,
 * straight where it was jagged. The triangle between the vertex and its two
 * neighbours lies outside the fill and inside the new edge; it is drawn with
 * the distance from the edge it ends up behind, so the ramp of the skirt goes
 * on up to the fill.
 *
 * At a corner that still points inward afterwards, by less than an eighth of
 * a turn, the two skirts end on the line halfway between their normals
 * instead of each on its own: they have that edge in common and no longer
 * overlap. The pixel below such a corner of a polygon with a polygon for a
 * hole came out 43 of 255 too bright. Each skirt gives up 0.75 tan(turn / 2)
 * pixels of its length for that, so both edges have to be twice as long;
 * across a shorter edge the skirt would fold over. A deeper corner is left
 * with the overlap, which is close to what its two edges cover of a pixel
 * together. WINE_D2D_FILL_AA_BRIDGES=0 keeps every vertex and every overlap.
 *
 * The skirt of an edge that goes takes the place of the first of the two it
 * replaces in the vertex array, the triangle that of the second. */
#define D2D_FILL_AA_BRIDGE_DEPTH 0.0625

struct d2d_fill_aa_edge
{
    UINT32 a, b;
    /* Where its skirt stands in the vertex array. */
    UINT32 slot;
    /* The edge it became a part of, ~0u for none. */
    UINT32 parent;
    /* How far the vertices left out between its ends lie from it at most, in pixels. */
    float depth;
    D2D1_POINT_2F n;
};

struct d2d_fill_aa_sliver
{
    UINT32 a, v, b, edge, slot;
};

static BOOL d2d_fill_aa_bridges(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_BRIDGES", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

/* edges holds edge_count edges with room for as many again, slivers has room
 * for edge_count, stack for the vertices of the fill; m is the pixel
 * transform. */
static void d2d_fill_aa_bridge(const struct d2d_fill *fill, struct d2d_fill_aa_corner *corners,
        struct d2d_fill_aa_edge *edges, size_t edge_count, struct d2d_fill_aa_sliver *slivers, UINT32 *stack,
        const double *m, struct d2d_fill_aa_vertex *expanded)
{
    size_t sliver_count = 0, top = 0, i;
    unsigned int k;

    for (i = 0; i < fill->vertex_count; ++i)
    {
        if (corners[i].count == 2)
            stack[top++] = i;
    }

    while (top)
    {
        UINT32 v = stack[--top], a, b;
        struct d2d_fill_aa_corner *c = &corners[v];
        const D2D1_POINT_2F *p = &fill->vertices[v], *pa, *pb;
        struct d2d_fill_aa_edge *e0, *e1, *e;
        double qa[2], qb[2], qv[2], dx, dy, len, depth, along;
        D2D1_POINT_2F outward;
        float flen;

        if (c->count != 2 || (a = c->other[0]) == (b = c->other[1]))
            continue;
        pa = &fill->vertices[a];
        pb = &fill->vertices[b];
        /* The corner points inward where neither skirt reaches on along the other edge. */
        if (!(c->n[1].x * (p->x - pa->x) + c->n[1].y * (p->y - pa->y) < 0.0f)
                || !(c->n[0].x * (p->x - pb->x) + c->n[0].y * (p->y - pb->y) < 0.0f))
            continue;

        e0 = &edges[c->edge[0]];
        e1 = &edges[c->edge[1]];
        qa[0] = m[0] * pa->x + m[1] * pa->y + m[2];
        qa[1] = m[3] * pa->x + m[4] * pa->y + m[5];
        qb[0] = m[0] * pb->x + m[1] * pb->y + m[2];
        qb[1] = m[3] * pb->x + m[4] * pb->y + m[5];
        qv[0] = m[0] * p->x + m[1] * p->y + m[2];
        qv[1] = m[3] * p->x + m[4] * p->y + m[5];
        dx = qb[0] - qa[0];
        dy = qb[1] - qa[1];
        if (!((len = sqrt(dx * dx + dy * dy)) > 1.0e-4))
            continue;
        /* The vertex lies behind the new edge, not behind the line it is on
         * past one of its ends: the bottom of a slot between two figures a
         * hundredth of a pixel apart is no dent. */
        along = (dx * (qv[0] - qa[0]) + dy * (qv[1] - qa[1])) / len;
        if (!(along > 0.0 && along < len))
            continue;
        depth = fabs(dx * (qv[1] - qa[1]) - dy * (qv[0] - qa[0])) / len + max(e0->depth, e1->depth);
        if (!(depth <= D2D_FILL_AA_BRIDGE_DEPTH))
            continue;

        /* At a right angle to the new edge, away from the vertex. */
        outward.x = -(pb->y - pa->y);
        outward.y = pb->x - pa->x;
        if (!((flen = sqrtf(outward.x * outward.x + outward.y * outward.y)) > 0.0f))
            continue;
        outward.x /= flen;
        outward.y /= flen;
        if (outward.x * (pa->x - p->x) + outward.y * (pa->y - p->y) < 0.0f)
        {
            outward.x = -outward.x;
            outward.y = -outward.y;
        }

        e = &edges[edge_count];
        e->a = a;
        e->b = b;
        e->slot = e0->slot;
        e->parent = ~0u;
        e->depth = depth;
        e->n = outward;
        e0->parent = e1->parent = edge_count;
        d2d_fill_aa_write_skirt(&expanded[e->slot], pa, pb, &outward);

        slivers[sliver_count].a = a;
        slivers[sliver_count].v = v;
        slivers[sliver_count].b = b;
        slivers[sliver_count].edge = edge_count;
        slivers[sliver_count].slot = e1->slot;
        ++sliver_count;

        c->count = 0;
        for (k = 0; k < 2; ++k)
        {
            UINT32 end = k ? b : a, far = k ? a : b;
            struct d2d_fill_aa_corner *o = &corners[end];
            unsigned int j;

            for (j = 0; j < min(o->count, 2); ++j)
            {
                if (o->other[j] == v)
                {
                    o->other[j] = far;
                    o->n[j] = outward;
                    o->edge[j] = edge_count;
                    break;
                }
            }
            if (o->count == 2)
                stack[top++] = end;
        }
        ++edge_count;
    }

    for (i = 0; i < sliver_count; ++i)
    {
        const struct d2d_fill_aa_sliver *sl = &slivers[i];
        struct d2d_fill_aa_vertex *o = &expanded[sl->slot];
        const UINT32 idx[3] = {sl->a, sl->v, sl->b};
        const struct d2d_fill_aa_edge *e = &edges[sl->edge];
        const D2D1_POINT_2F *pa, *pb;
        double qa[2], nx, ny, len, ox, oy;

        while (e->parent != ~0u)
            e = &edges[e->parent];
        pa = &fill->vertices[e->a];
        pb = &fill->vertices[e->b];
        qa[0] = m[0] * pa->x + m[1] * pa->y + m[2];
        qa[1] = m[3] * pa->x + m[4] * pa->y + m[5];
        /* The normal of the edge in pixels, on the side its skirt reaches out to. */
        nx = -(m[3] * (pb->x - pa->x) + m[4] * (pb->y - pa->y));
        ny = m[0] * (pb->x - pa->x) + m[1] * (pb->y - pa->y);
        ox = m[0] * e->n.x + m[1] * e->n.y;
        oy = m[3] * e->n.x + m[4] * e->n.y;
        len = sqrt(nx * nx + ny * ny);
        if (nx * ox + ny * oy < 0.0)
            len = -len;

        memset(o, 0, 6 * sizeof(*o));
        for (k = 0; k < 6; ++k)
        {
            const D2D1_POINT_2F *q = &fill->vertices[idx[k < 3 ? k : 0]];

            o[k].pos = *q;
            if (k < 3 && len != 0.0)
            {
                o[k].u = max(-((m[0] * q->x + m[1] * q->y + m[2] - qa[0]) * nx
                        + (m[3] * q->x + m[4] * q->y + m[5] - qa[1]) * ny) / len, 0.0);
                o[k].v = 10.0f;
                o[k].s = 10.0f;
            }
        }
    }
}

/* The corner bound at the corners of the outline that point inward.
 *
 * A face that touches the outline in a corner only, with no half plane of an
 * outline edge for the pixels near that corner, is limited by the pixel shader
 * by the distance to the corner, taken as the distance to an edge through it
 * at a right angle to the direction of the pixel. At a corner that points
 * inward that is wrong both ways. The fill reaches around the corner, and the
 * bound is too dark in the notch: the two pixels that hold the inner corners
 * of an "M" of DejaVu Sans at 6 pixels, where the diagonals meet the stems,
 * came out at 150 of 255 for a coverage of 238. Beside one of the two edges
 * the distance to the corner is larger than the distance to the edge, and the
 * bound is too bright there: the pixel beside the stem of a "T" at 24 pixels
 * right under its bar, its centre 0.09 pixels inside the stem, came out at
 * 252 for 143. Moving the bound outward fixes the first and makes the second
 * worse, and which of the two a pixel is in does not depend on the face
 * (wine-dev issue 637). d2d_fill_aa_face_planes() gives such a face neither
 * edge: beyond the corner the outline leaves both half planes.
 *
 * Near the corner the fill is the union of the half planes of its two outline
 * edges, the notch what lies outside both. The faces at the corner carry the
 * inward normals of the two edges in pixels (d2d_fill_aa_notch_code()), and
 * the pixel shader takes the area of the notch in the pixel from the parts u1
 * and u2 of the pixel that lie outside each half plane: min(u1, u2) where the
 * outline goes on straight, u1 u2 at a square notch, max(u1 + u2 - 1, 0) in a
 * slot, blended by the angle between the normals; for a pixel centred on the
 * corner that is the share of a disc the notch takes (notch_coverage()).
 *
 * A curve leaves the corner along its tangent, to the control point of a
 * chord or along a control edge, and the edges are lines only so far: the
 * model holds as long as both stay within an eighth of a pixel of their lines,
 * the reach of the notch, and beyond that the bound runs out as the plain one
 * does. Corners with an edge shorter than D2D_FILL_AA_NOTCH_MIN_EDGE pixels,
 * as the risers of an outline in steps of 1/64 to 1/8 pixel, or a turn of
 * less than D2D_FILL_AA_NOTCH_MIN_TURN degrees, as where two pieces of a
 * curve meet and the outline follows the curve rather than the tangent, keep
 * the plain bound. WINE_D2D_FILL_AA_NOTCHES=0 keeps it at every corner. */
#define D2D_FILL_AA_NOTCH_MIN_EDGE 0.15f
#define D2D_FILL_AA_NOTCH_MIN_TURN 15.0

struct d2d_fill_aa_turn
{
    /* The points the outline heads for from the vertex along its first two
     * boundary edges, the outward normals of those lines, and how far, in
     * pixels, the edges follow them. */
    D2D1_POINT_2F to[2], n[2];
    float len[2];
    /* The number of boundary edges at the vertex, up to 3. */
    BYTE count;
    /* At a corner that points inward: the inward normals in pixels, as
     * angles, and the reach of the notch in pixels; 0 elsewhere. */
    float angle[2], reach;
};

static BOOL d2d_fill_aa_notches(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_NOTCHES", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

/* Adds the boundary edge from a to b, whose face has its third vertex c, to the
 * turns of its two ends. seam is the seam of a curve edge, NULL for a straight
 * one; m is the pixel transform, pdet its determinant. */
static void d2d_fill_aa_turn_add(const struct d2d_fill *fill, struct d2d_fill_aa_turn *turns, UINT32 a, UINT32 b,
        UINT32 c, const struct d2d_fill_aa_seam *seam, const double *m, double pdet)
{
    const D2D1_POINT_2F *pa = &fill->vertices[a], *pb = &fill->vertices[b], *pc = &fill->vertices[c];
    D2D1_POINT_2F n;
    unsigned int j;
    float len;

    /* A curve that is not known gives no line. */
    if (seam && seam->start == ~0u)
        return;
    n.x = -(pb->y - pa->y);
    n.y = pb->x - pa->x;
    if (!((len = sqrtf(n.x * n.x + n.y * n.y)) > 0.0f))
        return;
    if (n.x * (pa->x - pc->x) + n.y * (pa->y - pc->y) < 0.0f)
        len = -len;
    n.x /= len;
    n.y /= len;

    for (j = 0; j < 2; ++j)
    {
        UINT32 v = j ? b : a, o = j ? a : b;
        const D2D1_POINT_2F *pv = &fill->vertices[v], *pe = &fill->vertices[o], *to = pe;
        struct d2d_fill_aa_turn *t = &turns[v];
        double ex, ey, r;
        D2D1_POINT_2F nt;
        float tl;

        if (seam && seam->chord)
            to = &seam->control;
        nt.x = -(to->y - pv->y);
        nt.y = to->x - pv->x;
        if (!((tl = sqrtf(nt.x * nt.x + nt.y * nt.y)) > 0.0f))
            continue;
        if (nt.x * n.x + nt.y * n.y < 0.0f)
            tl = -tl;
        nt.x /= tl;
        nt.y /= tl;
        if (t->count < 2)
        {
            /* A curve departs from its tangent as h (s / 2 |to - v|)^2, h
             * being the distance of its other end from the tangent; it stays
             * within an eighth of a pixel of it for s up to
             * 2 |to - v| sqrt(1 / 8h). */
            ex = m[0] * (pe->x - pv->x) + m[1] * (pe->y - pv->y);
            ey = m[3] * (pe->x - pv->x) + m[4] * (pe->y - pv->y);
            r = sqrt(ex * ex + ey * ey);
            if (seam)
            {
                double cx = m[0] * (to->x - pv->x) + m[1] * (to->y - pv->y);
                double cy = m[3] * (to->x - pv->x) + m[4] * (to->y - pv->y);
                double lc = sqrt(cx * cx + cy * cy), h;

                if (seam->chord)
                    h = lc > 0.0 ? fabs(cx * ey - cy * ex) / lc : 0.0;
                else
                    h = 4.0 * fabs(seam->c2) * sqrt(fabs(pdet));
                if (h > 0.0)
                    r = min(r, 2.0 * lc * sqrt(1.0 / (8.0 * h)));
            }
            t->to[t->count] = *to;
            t->n[t->count] = nt;
            t->len[t->count] = min(r, 1000.0);
        }
        if (t->count < 3)
            ++t->count;
    }
}

/* Sets the angles and the reach of the notch at vertex v, if the outline
 * points inward there; see d2d_fill_aa_notches(). m is the pixel transform. */
static void d2d_fill_aa_notch(const struct d2d_fill *fill, struct d2d_fill_aa_turn *t, UINT32 v, const double *m)
{
    const D2D1_POINT_2F *p = &fill->vertices[v];
    float angle[2];
    unsigned int k;

    if (t->count != 2)
        return;
    /* The corner points inward where neither line reaches on along the other edge. */
    if (!(t->n[1].x * (p->x - t->to[0].x) + t->n[1].y * (p->y - t->to[0].y) < 0.0f)
            || !(t->n[0].x * (p->x - t->to[1].x) + t->n[0].y * (p->y - t->to[1].y) < 0.0f))
        return;
    if (min(t->len[0], t->len[1]) < D2D_FILL_AA_NOTCH_MIN_EDGE)
        return;
    for (k = 0; k < 2; ++k)
    {
        /* The outward normal mapped to pixels keeps its side of the line; the
         * inward normal in pixels is at a right angle to the line in pixels. */
        double ox = m[0] * t->n[k].x + m[1] * t->n[k].y, oy = m[3] * t->n[k].x + m[4] * t->n[k].y;
        double ex = m[0] * (t->to[k].x - p->x) + m[1] * (t->to[k].y - p->y);
        double ey = m[3] * (t->to[k].x - p->x) + m[4] * (t->to[k].y - p->y);
        double nx = -ey, ny = ex;

        if (!(sqrt(ox * ox + oy * oy) > 1.0e-12))
            return;
        if (nx * ox + ny * oy > 0.0)
        {
            nx = -nx;
            ny = -ny;
        }
        angle[k] = atan2(ny, nx);
    }
    if (cos(angle[0] - angle[1]) > cos(D2D_FILL_AA_NOTCH_MIN_TURN * M_PI / 180.0))
        return;
    t->angle[0] = angle[0];
    t->angle[1] = angle[1];
    t->reach = min(t->len[0], t->len[1]);
}

/* The notch at a corner as the pixel shader takes it, in the bits of
 * corner_on: the angles of the two inward normals in 4096ths of a turn from
 * -pi, twelve bits each, and above them six bits of the reach, in eighths of
 * a pixel up to 4 pixels and in halves up to 19, all added to the bits of
 * 2.0f. That is a finite float of at least 2, which the shader tells from
 * 0 and 1 by its value. A normal off by at most 0.044 degrees moves its line
 * by 0.015 pixels at the largest reach. Giving the vertex the normals as two
 * floats each made it 24 bytes larger, and filling the 32 paths of a frame of
 * Minimal Audio Current's tap display took 24.9 ms at best for 22.5 even
 * with the notches off. */
static float d2d_fill_aa_notch_code(const struct d2d_fill_aa_turn *t)
{
    unsigned int a[2], r, k;
    UINT32 bits;
    float code;

    for (k = 0; k < 2; ++k)
        a[k] = (unsigned int)floor((t->angle[k] + M_PI) * (4096.0 / (2.0 * M_PI)) + 0.5) & 4095;
    if (t->reach < 3.9375f)
        r = (unsigned int)(t->reach * 8.0f + 0.5f);
    else
        r = min(32 + (unsigned int)((t->reach - 4.0f) * 2.0f + 0.5f), 62);
    bits = 0x40000000u + ((r << 24) | (a[0] << 12) | a[1]);
    memcpy(&code, &bits, sizeof(code));
    return code;
}

/* An outline edge of a face that ends at a corner that points inward.
 *
 * A face of the fill mesh ramps down towards its own outline edges as if each
 * went on beyond its ends. Where an edge ends at a notch, the outline turns
 * into the fill there: beyond the corner the edge bounds nothing, and close to
 * the corner the fill is the union of the half planes of the notch's two edges
 * (d2d_fill_aa_notches()), not the half plane of the face's edge alone. A
 * pixel at an inner corner of a "+" of DejaVu Sans at 13 pixels came out at 154
 * of 255 for a coverage of 204, one in a captured frame of Minimal Audio
 * Current's tap display at 144 for 238 (wine-dev issue 639). The pixel shader
 * takes the union up to a quarter of a pixel from the corner and fades back to
 * the edge over the next quarter: further out the tangent of a curve at the
 * corner no longer stands for the curve, and the inner corner of an "m" of
 * DejaVu Sans at 6 pixels came out at 231 for 185 with the whole reach of the
 * notch. A straight outline edge of
 * the face is marked by a seam_c1 of -1 for that; a seam has 0 or more there,
 * and a straight edge has no seam_t to multiply it with.
 * WINE_D2D_FILL_AA_NOTCH_EDGES=0 leaves the edges as they are. */
static BOOL d2d_fill_aa_notch_edges(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_NOTCH_EDGES", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

/* Hairline gaps between two outline edges.
 *
 * Two figures of a path that stand a hundredth of a pixel apart leave a slot
 * in the outline of their union. Each side was antialiased as an edge of its
 * own, the face on one side ramping down towards its wall and the skirt of the
 * other wall reaching over it, and the two partial coverages a and b were
 * blended to a + b - ab where the path covers a + b: 190 of 255 for a coverage
 * of 253 between a bar and the figure beside it in Minimal Audio Current's tap
 * display (wine-dev issue 639).
 *
 * Straight outline edges that face each other across a gap narrower than
 * D2D_FILL_AA_GAP_WIDTH pixels for at least D2D_FILL_AA_GAP_MIN_LENGTH pixels
 * are found on a grid of one pixel, and the mesh is built again with the gap
 * zipped: at each end of it the two walls meet in one vertex. A wall that
 * goes on is split at the vertex the other one ends at, both are split at a
 * new vertex halfway where neither ends, and where both end at a vertex of
 * their own a triangle closes the gap. The walls become interior edges, and
 * the rest of the antialiasing runs on the new mesh as on any other.
 *
 * A closed gap adds its width to every pixel along it, while the blend is far
 * off only near its middle: closing gaps of up to 1/16 pixel made the pixels
 * of twelve captured frames of the tap display farther from their coverage
 * by 23,456 in all for 26,482 closer, of up to 1/64 pixel by 4,287 for
 * 25,286; the gaps of the tap display are 0.007 and 0.014 pixels wide. Vertices
 * of the mesh are never merged: one of them may be a corner of a curve
 * triangle, and d2d_fill_aa_mark_thin() then gives up on the whole path. A
 * shorter gap is left open: splitting a wall a fraction of a pixel before its
 * end takes its half plane (d2d_fill_aa_face_planes()) from the faces at that
 * end. WINE_D2D_FILL_AA_GAPS=0 leaves all gaps open. */
#define D2D_FILL_AA_GAP_WIDTH (1.0 / 64.0)
#define D2D_FILL_AA_GAP_MIN_LENGTH 0.25
#define D2D_FILL_AA_GAP_SNAP (1.0 / 64.0)

static BOOL d2d_fill_aa_gaps(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_FILL_AA_GAPS", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

struct d2d_fill_aa_gap_edge
{
    /* The face, the index of its vertex opposite the edge, the edge from a to
     * b, a in pixels, the direction and the normal towards the side without
     * fill, and the length in pixels. */
    UINT32 face, e, a, b;
    double p[2], d[2], n[2], len;
    /* The cells of the grid it comes near, none if cx1 is below cx0. */
    INT64 cx0, cx1, cy0, cy1;
};

struct d2d_fill_aa_gap_cell
{
    UINT32 edge, next;
};

struct d2d_fill_aa_gap_bucket
{
    UINT64 key;
    UINT32 first;
};

struct d2d_fill_aa_gap_split
{
    UINT32 edge, v;
    double t;
};

struct d2d_fill_aa_gap_merge
{
    UINT32 from, to;
};

struct d2d_fill_aa_gap
{
    /* The two walls, and the ends of the gap along each, in pixels from a;
     * where both walls end at a vertex, those vertices. */
    UINT32 e1, e2;
    double t1[2], t2[2];
    UINT32 v1[2], v2[2];
};

static int __cdecl d2d_fill_aa_gap_split_cmp(const void *a, const void *b)
{
    const struct d2d_fill_aa_gap_split *x = a, *y = b;

    if (x->edge != y->edge)
        return x->edge < y->edge ? -1 : 1;
    return x->t < y->t ? -1 : x->t > y->t;
}

/* Narrows [*lo, *hi] to where v0 + (v1 - v0) * lambda lies in [lower, upper]. */
static BOOL d2d_fill_aa_gap_clip(double v0, double v1, double lower, double upper, double *lo, double *hi)
{
    double dv = v1 - v0, x, y;

    if (fabs(dv) < 1.0e-12)
        return v0 >= lower && v0 <= upper && *lo <= *hi;
    x = (lower - v0) / dv;
    y = (upper - v0) / dv;
    if (x > y)
    {
        double t = x;
        x = y;
        y = t;
    }
    *lo = max(*lo, x);
    *hi = min(*hi, y);
    return *lo <= *hi;
}

/* Whether e2 faces e1 across a gap no wider than D2D_FILL_AA_GAP_WIDTH, and where. */
static BOOL d2d_fill_aa_gap_pair(const struct d2d_fill_aa_gap_edge *e1, const struct d2d_fill_aa_gap_edge *e2,
        struct d2d_fill_aa_gap *gap)
{
    double lo = 0.0, hi = 1.0, qx, qy, t0, t1, s0, s1;

    if (!(e1->n[0] * e2->n[0] + e1->n[1] * e2->n[1] < -0.99))
        return FALSE;
    if (e1->a == e2->a || e1->a == e2->b || e1->b == e2->a || e1->b == e2->b)
        return FALSE;
    qx = e2->p[0] + e2->d[0] * e2->len;
    qy = e2->p[1] + e2->d[1] * e2->len;
    t0 = (e2->p[0] - e1->p[0]) * e1->d[0] + (e2->p[1] - e1->p[1]) * e1->d[1];
    t1 = (qx - e1->p[0]) * e1->d[0] + (qy - e1->p[1]) * e1->d[1];
    s0 = (e2->p[0] - e1->p[0]) * e1->n[0] + (e2->p[1] - e1->p[1]) * e1->n[1];
    s1 = (qx - e1->p[0]) * e1->n[0] + (qy - e1->p[1]) * e1->n[1];
    if (!d2d_fill_aa_gap_clip(t0, t1, 0.0, e1->len, &lo, &hi) || !d2d_fill_aa_gap_clip(s0, s1, -1.0e-6, D2D_FILL_AA_GAP_WIDTH, &lo, &hi))
        return FALSE;
    if ((hi - lo) * e2->len < D2D_FILL_AA_GAP_MIN_LENGTH)
        return FALSE;
    gap->t1[0] = t0 + (t1 - t0) * lo;
    gap->t1[1] = t0 + (t1 - t0) * hi;
    gap->t2[0] = lo * e2->len;
    gap->t2[1] = hi * e2->len;
    return TRUE;
}

/* Builds in out a copy of the mesh of fill with its hairline gaps zipped; see
 * above. Returns FALSE if there is none or it cannot be built. The edge map
 * is that of fill, and face_slots holds the slots of the three edges of each
 * face in it. m is the pixel transform. On
 * success *replaced marks the faces of fill that out no longer has and *added
 * the faces of out that fill does not have; the caller frees them and
 * out->vertices and out->faces. */
static BOOL d2d_fill_aa_close_gaps(const struct d2d_fill *fill, const BYTE *edge_cnt,
        const BYTE *edge_curve, const UINT32 *face_slots, const double *m, struct d2d_fill *out,
        BYTE **replaced, BYTE **added)
{
    size_t edge_count = 0, cell_count, table_size, gap_count = 0, gaps_size = 0, split_count, i, k, l;
    struct d2d_fill_aa_gap_bucket *table = NULL;
    struct d2d_fill_aa_gap_merge *merges = NULL;
    double width = D2D_FILL_AA_GAP_WIDTH;
    size_t merge_count, edges_size = 0;
    UINT32 *parent = NULL;
    struct d2d_fill_aa_gap_split *splits = NULL;
    struct d2d_fill_aa_gap_edge *edges = NULL;
    struct d2d_fill_aa_gap_cell *cells = NULL;
    struct d2d_fill_aa_gap *gaps = NULL;
    D2D1_POINT_2F *vertices = NULL;
    struct d2d_face *faces = NULL;
    BYTE *was = NULL, *is_new = NULL;
    size_t vertex_count, face_count, faces_size;
    BOOL ret = FALSE;

    /* The straight outline edges long enough to face another one along a gap,
     * in the order of their faces. */
    for (i = 0; i < fill->face_count; ++i)
    {
        const struct d2d_face *f = &fill->faces[i];
        unsigned int e;

        for (e = 0; e < 3; ++e)
        {
            UINT32 slot = face_slots[3 * i + e];
            const D2D1_POINT_2F *pa, *pb, *po;
            struct d2d_fill_aa_gap_edge *g;
            double qx, qy, ox, oy;

            if (edge_cnt[slot] != 1 || edge_curve[slot])
                continue;
            pa = &fill->vertices[f->v[(e + 1) % 3]];
            pb = &fill->vertices[f->v[(e + 2) % 3]];
            if (!d2d_array_reserve((void **)&edges, &edges_size, edge_count + 1, sizeof(*edges)))
                goto done;
            g = &edges[edge_count];
            g->p[0] = m[0] * pa->x + m[1] * pa->y + m[2];
            g->p[1] = m[3] * pa->x + m[4] * pa->y + m[5];
            qx = m[0] * pb->x + m[1] * pb->y + m[2];
            qy = m[3] * pb->x + m[4] * pb->y + m[5];
            if (!((g->len = sqrt((qx - g->p[0]) * (qx - g->p[0]) + (qy - g->p[1]) * (qy - g->p[1])))
                    >= D2D_FILL_AA_GAP_MIN_LENGTH))
                continue;
            po = &fill->vertices[f->v[e]];
            g->face = i;
            g->e = e;
            g->a = f->v[(e + 1) % 3];
            g->b = f->v[(e + 2) % 3];
            ox = m[0] * po->x + m[1] * po->y + m[2];
            oy = m[3] * po->x + m[4] * po->y + m[5];
            g->d[0] = (qx - g->p[0]) / g->len;
            g->d[1] = (qy - g->p[1]) / g->len;
            g->n[0] = -g->d[1];
            g->n[1] = g->d[0];
            if ((ox - g->p[0]) * g->n[0] + (oy - g->p[1]) * g->n[1] > 0.0)
            {
                g->n[0] = -g->n[0];
                g->n[1] = -g->n[1];
            }
            ++edge_count;
        }
    }
    if (edge_count < 2)
        goto done;

    /* The edges by the cells of a grid of one pixel they come near, in a hash
     * table of the cells. */
    for (i = 0, cell_count = 0; i < edge_count; ++i)
    {
        struct d2d_fill_aa_gap_edge *g = &edges[i];
        double qx = g->p[0] + g->d[0] * g->len, qy = g->p[1] + g->d[1] * g->len;

        g->cx0 = (INT64)floor(min(g->p[0], qx) - width);
        g->cx1 = (INT64)floor(max(g->p[0], qx) + width);
        g->cy0 = (INT64)floor(min(g->p[1], qy) - width);
        g->cy1 = (INT64)floor(max(g->p[1], qy) + width);
        if ((g->cx1 - g->cx0 + 1) * (g->cy1 - g->cy0 + 1) > 4096)
            g->cx1 = g->cx0 - 1;
        else
            cell_count += (g->cx1 - g->cx0 + 1) * (g->cy1 - g->cy0 + 1);
    }
    for (table_size = 16; table_size < 2 * cell_count; table_size <<= 1);
    if (!(cells = malloc(cell_count * sizeof(*cells))) || !(table = malloc(table_size * sizeof(*table))))
        goto done;
    for (i = 0; i < table_size; ++i)
        table[i].first = ~0u;
    for (i = 0, k = 0; i < edge_count; ++i)
    {
        const struct d2d_fill_aa_gap_edge *g = &edges[i];
        INT64 cx, cy;

        for (cx = g->cx0; cx <= g->cx1; ++cx)
        {
            for (cy = g->cy0; cy <= g->cy1; ++cy)
            {
                UINT64 key = ((UINT64)(UINT32)cx << 32) | (UINT32)cy;
                size_t slot = (size_t)((key * 0x9e3779b97f4a7c15ull) >> 32) & (table_size - 1);

                while (table[slot].first != ~0u && table[slot].key != key)
                    slot = (slot + 1) & (table_size - 1);
                table[slot].key = key;
                cells[k].edge = i;
                cells[k].next = table[slot].first;
                table[slot].first = k++;
            }
        }
    }

    for (i = 0; i < table_size; ++i)
    {
        UINT32 x, y;

        for (x = table[i].first; x != ~0u; x = cells[x].next)
        {
            for (y = cells[x].next; y != ~0u; y = cells[y].next)
            {
                UINT32 n1 = min(cells[x].edge, cells[y].edge), n2 = max(cells[x].edge, cells[y].edge);
                const struct d2d_fill_aa_gap_edge *g1 = &edges[n1], *g2 = &edges[n2];
                INT64 cx = max(g1->cx0, g2->cx0), cy = max(g1->cy0, g2->cy0);

                /* Each pair once, in the first cell both come near. */
                if ((((UINT64)(UINT32)cx << 32) | (UINT32)cy) != table[i].key)
                    continue;
                if (!d2d_array_reserve((void **)&gaps, &gaps_size, gap_count + 1, sizeof(*gaps)))
                    goto done;
                if (d2d_fill_aa_gap_pair(g1, g2, &gaps[gap_count]))
                {
                    gaps[gap_count].e1 = n1;
                    gaps[gap_count].e2 = n2;
                    ++gap_count;
                }
            }
        }
    }
    if (!gap_count)
        goto done;

    /* The ends of each gap: the two walls meet in one vertex there. Where a
     * wall ends at a vertex the other one is split at it; where both end, a
     * triangle closes the gap below; where neither does, a new vertex halfway
     * between them splits both. */
    if (!(splits = malloc(gap_count * 4 * sizeof(*splits))) || !(merges = malloc(gap_count * 2 * sizeof(*merges)))
            || !(vertices = malloc((fill->vertex_count + 2 * gap_count + 4 * gap_count) * sizeof(*vertices))))
        goto done;
    memcpy(vertices, fill->vertices, fill->vertex_count * sizeof(*vertices));
    vertex_count = fill->vertex_count;
    for (i = 0, split_count = 0, merge_count = 0; i < gap_count; ++i)
    {
        const struct d2d_fill_aa_gap_edge *g1 = &edges[gaps[i].e1], *g2 = &edges[gaps[i].e2];

        for (k = 0; k < 2; ++k)
        {
            double t1 = gaps[i].t1[k], t2 = gaps[i].t2[k];
            UINT32 v1 = t1 <= D2D_FILL_AA_GAP_SNAP ? g1->a : t1 >= g1->len - D2D_FILL_AA_GAP_SNAP ? g1->b : ~0u;
            UINT32 v2 = t2 <= D2D_FILL_AA_GAP_SNAP ? g2->a : t2 >= g2->len - D2D_FILL_AA_GAP_SNAP ? g2->b : ~0u;

            gaps[i].v1[k] = gaps[i].v2[k] = ~0u;
            if (v1 != ~0u && v2 != ~0u)
            {
                /* Both walls end here: a triangle closes the gap at this end. Merging the two vertices instead
                 * took a corner of a curve triangle out of the mesh, and d2d_fill_aa_mark_thin() gave up. */
                gaps[i].v1[k] = v1;
                gaps[i].v2[k] = v2;
            }
            else if (v2 != ~0u)
            {
                splits[split_count].edge = gaps[i].e1;
                splits[split_count].t = t1;
                splits[split_count++].v = v2;
            }
            else if (v1 != ~0u)
            {
                splits[split_count].edge = gaps[i].e2;
                splits[split_count].t = t2;
                splits[split_count++].v = v1;
            }
            else
            {
                const D2D1_POINT_2F *a1 = &fill->vertices[g1->a], *b1 = &fill->vertices[g1->b];
                const D2D1_POINT_2F *a2 = &fill->vertices[g2->a], *b2 = &fill->vertices[g2->b];
                float r1 = t1 / g1->len, r2 = t2 / g2->len;

                vertices[vertex_count].x = 0.5f * (a1->x + (b1->x - a1->x) * r1 + a2->x + (b2->x - a2->x) * r2);
                vertices[vertex_count].y = 0.5f * (a1->y + (b1->y - a1->y) * r1 + a2->y + (b2->y - a2->y) * r2);
                splits[split_count].edge = gaps[i].e1;
                splits[split_count].t = t1;
                splits[split_count++].v = vertex_count;
                splits[split_count].edge = gaps[i].e2;
                splits[split_count].t = t2;
                splits[split_count++].v = vertex_count;
                ++vertex_count;
            }
        }
    }
    qsort(splits, split_count, sizeof(*splits), d2d_fill_aa_gap_split_cmp);
    /* Two splits of one wall at the same place are one vertex, if one of them
     * is new; two vertices of the mesh stay apart. */
    for (i = 1; i < split_count; ++i)
    {
        UINT32 a = splits[i - 1].v, b = splits[i].v;

        if (splits[i].edge != splits[i - 1].edge || splits[i].t - splits[i - 1].t > D2D_FILL_AA_GAP_SNAP
                || a == b || (a < fill->vertex_count && b < fill->vertex_count))
            continue;
        if (merge_count == gap_count * 2)
            goto done;
        merges[merge_count].from = b >= fill->vertex_count ? b : a;
        merges[merge_count++].to = b >= fill->vertex_count ? a : b;
        splits[i].v = splits[i - 1].v = merges[merge_count - 1].to;
    }

    /* The faces with a split wall as a fan. */
    faces_size = fill->face_count + 2 * split_count + 2 * gap_count + 16;
    if (!(faces = malloc(faces_size * sizeof(*faces))) || !(is_new = calloc(faces_size, sizeof(*is_new)))
            || !(was = calloc(fill->face_count, sizeof(*was))))
        goto done;
    face_count = 0;
    for (i = 0, k = 0; i < fill->face_count; ++i)
    {
        const struct d2d_face *f = &fill->faces[i];
        UINT32 poly[3 + 64], on_edge[3] = {0};
        size_t n = 0, first;
        unsigned int e, c;

        while (k < split_count && edges[splits[k].edge].face < i)
            ++k;
        first = k;
        for (l = first; l < split_count && edges[splits[l].edge].face == i; ++l)
            ++on_edge[edges[splits[l].edge].e];
        if (!on_edge[0] && !on_edge[1] && !on_edge[2])
        {
            faces[face_count++] = *f;
            continue;
        }
        /* The polygon v[0], the points on edge 2 (v[0] to v[1]), v[1], those on edge 0, v[2], those on edge 1;
         * the splits of an edge run from its a, which is the vertex before them. */
        for (c = 0; c < 3; ++c)
        {
            e = (c + 2) % 3;
            poly[n++] = f->v[c];
            for (l = first; l < split_count && edges[splits[l].edge].face == i; ++l)
            {
                if (edges[splits[l].edge].e != e || poly[n - 1] == splits[l].v || n >= ARRAY_SIZE(poly) - 1)
                    continue;
                poly[n++] = splits[l].v;
            }
        }
        if (face_count + n + 1 > faces_size)
            goto done;
        was[i] = 1;
        if ((on_edge[0] != 0) + (on_edge[1] != 0) + (on_edge[2] != 0) == 1)
        {
            /* A fan from the vertex opposite the split edge. */
            for (e = 0; e < 3 && !on_edge[e]; ++e);
            for (c = 0; c < n && poly[c] != f->v[e]; ++c);
            for (l = 1; l + 1 < n; ++l)
            {
                faces[face_count].v[0] = f->v[e];
                faces[face_count].v[1] = poly[(c + l) % n];
                faces[face_count].v[2] = poly[(c + l + 1) % n];
                is_new[face_count++] = 1;
            }
        }
        else
        {
            /* A fan from the centroid. */
            vertices[vertex_count].x = (fill->vertices[f->v[0]].x + fill->vertices[f->v[1]].x
                    + fill->vertices[f->v[2]].x) / 3.0f;
            vertices[vertex_count].y = (fill->vertices[f->v[0]].y + fill->vertices[f->v[1]].y
                    + fill->vertices[f->v[2]].y) / 3.0f;
            for (l = 0; l < n; ++l)
            {
                faces[face_count].v[0] = vertex_count;
                faces[face_count].v[1] = poly[l];
                faces[face_count].v[2] = poly[(l + 1) % n];
                is_new[face_count++] = 1;
            }
            ++vertex_count;
        }
    }

    /* The gaps that end at a vertex of both walls: a triangle there, two where
     * both ends do; the walls meet in a vertex at an end otherwise. */
    for (i = 0; i < gap_count; ++i)
    {
        UINT32 c[4];
        size_t m = 0;

        if (gaps[i].v1[0] == ~0u && gaps[i].v1[1] == ~0u)
            continue;
        for (k = 0; k < 2; ++k)
        {
            UINT32 shared;

            if (gaps[i].v1[k] != ~0u)
            {
                c[m++] = k ? gaps[i].v1[k] : gaps[i].v2[k];
                c[m++] = k ? gaps[i].v2[k] : gaps[i].v1[k];
                continue;
            }
            /* The vertex both walls are split at. */
            shared = ~0u;
            for (l = 0; l < split_count; ++l)
            {
                if (splits[l].edge == gaps[i].e1 && fabs(splits[l].t - gaps[i].t1[k]) <= D2D_FILL_AA_GAP_SNAP)
                    shared = splits[l].v;
            }
            if (shared == ~0u)
                break;
            c[m++] = shared;
        }
        if (k < 2 || face_count + 2 > faces_size)
            continue;
        is_new[face_count] = is_new[face_count + 1] = 1;
        /* c holds the end at lambda 0 (wall 2 then wall 1) and the end at lambda 1 (wall 1 then wall 2). */
        if (m == 3)
        {
            faces[face_count].v[0] = c[0];
            faces[face_count].v[1] = c[1];
            faces[face_count].v[2] = c[2];
            ++face_count;
        }
        else if (m == 4)
        {
            faces[face_count].v[0] = c[0];
            faces[face_count].v[1] = c[1];
            faces[face_count].v[2] = c[2];
            ++face_count;
            faces[face_count].v[0] = c[0];
            faces[face_count].v[1] = c[2];
            faces[face_count].v[2] = c[3];
            ++face_count;
        }
    }

    /* The new vertices two splits of a wall share, and the faces that collapse with them. */
    if (!(parent = malloc(vertex_count * sizeof(*parent))))
        goto done;
    for (i = 0; i < vertex_count; ++i)
        parent[i] = i;
    for (i = 0; i < merge_count; ++i)
    {
        UINT32 x = merges[i].from, y = merges[i].to;

        while (parent[x] != x)
            x = parent[x];
        while (parent[y] != y)
            y = parent[y];
        if (x != y)
            parent[x] = y;
    }
    for (i = 0, k = 0; i < face_count; ++i)
    {
        struct d2d_face f = faces[i];
        unsigned int c;

        for (c = 0; c < 3; ++c)
        {
            while (parent[f.v[c]] != f.v[c])
                f.v[c] = parent[f.v[c]];
        }
        if (f.v[0] == f.v[1] || f.v[1] == f.v[2] || f.v[0] == f.v[2])
            continue;
        is_new[k] = is_new[i];
        faces[k++] = f;
    }
    face_count = k;

    *out = *fill;
    out->vertices = vertices;
    out->vertex_count = vertex_count;
    out->faces = faces;
    out->face_count = face_count;
    out->faces_size = faces_size;
    *replaced = was;
    *added = is_new;
    vertices = NULL;
    faces = NULL;
    was = is_new = NULL;
    ret = TRUE;

done:
    free(edges);
    free(cells);
    free(table);
    free(gaps);
    free(splits);
    free(merges);
    free(parent);
    free(vertices);
    free(faces);
    free(was);
    free(is_new);
    return ret;
}

/* Builds the antialiased fill mesh of a geometry under the current transform:
 * the vertex array the D2D_SHAPE_TYPE_FILL_AA shaders take. Returns FALSE when
 * the fill has nothing to antialias or the mesh cannot be built; the plain
 * triangle path draws the geometry then. *curve_skip receives NULL or, for
 * every curve triangle of the fill, the Béziers before the arcs, whether the
 * mesh draws its area and it is not to be drawn; see d2d_fill_aa_thin(). The
 * caller frees *out and *curve_skip. */
static BOOL d2d_device_context_fill_aa_expand_mesh(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_fill *fill, struct d2d_fill *closed,
        struct d2d_fill_aa_vertex **out, size_t *out_count, BYTE **curve_skip)
{
    UINT32 *face_slots = NULL;
    size_t curve_count = (fill->bezier_vertex_count + fill->arc_vertex_count) / 3;
    UINT32 *curve_ends = NULL;
    struct d2d_fill_aa_outline outline = {0};
    struct d2d_fill_aa_corner *corners = NULL;
    struct d2d_fill_aa_turn *turns = NULL;
    struct d2d_fill_aa_sliver *slivers = NULL;
    struct d2d_fill_aa_edge *edges = NULL;
    UINT32 *corner_curves = NULL, *bridge_stack = NULL;
    size_t edge_count = 0;
    BOOL neighbours = d2d_fill_aa_neighbours();
    size_t pixel_count = 0, pixels_size = 0, thin_count = 0;
    struct d2d_fill_aa_pixel *pixels = NULL;
    BYTE *fan = NULL, *thin = NULL;
    struct d2d_fill_aa_vertex *expanded;
    double pm[6], pdet;
    BYTE *on_outline;
    struct d2d_fill_aa_seam *seams;
    size_t fc = fill->face_count;
    size_t boundary_count = 0, seam_count = 0;
    size_t expanded_count, map_size, i, vi;
    UINT64 *edge_keys;
    BYTE *edge_cnt, *edge_processed, *edge_curve;
    float m[4];

    *curve_skip = NULL;
    d2d_device_context_get_pixel_matrix(render_target, &geometry->transform, m);

    /* Use 6x multiplier to keep load factor ≤ 50%, preventing open-addressing loops
     * from running unbounded even in worst-case all-boundary-edge meshes. */
    map_size = fc * 6 + 16;
    edge_keys      = calloc(map_size, sizeof(*edge_keys));
    edge_cnt       = calloc(map_size, sizeof(*edge_cnt));
    edge_processed = calloc(map_size, sizeof(*edge_processed));
    edge_curve     = calloc(map_size, sizeof(*edge_curve));
    seams          = malloc(map_size * sizeof(*seams));
    if (!edge_keys || !edge_cnt || !edge_processed || !edge_curve || !seams)
    {
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
        return FALSE;
    }
    for (i = 0; i < map_size; ++i)
        seams[i].start = ~0u;
    /* The slots of the edges of each face; see d2d_fill_aa_close_gaps(). */
    if (d2d_fill_aa_gaps())
        face_slots = malloc(fc * 3 * sizeof(*face_slots));

    /* Pass 1: count edge occurrences via open-addressing hash. */
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &fill->faces[i];
        int e;
        for (e = 0; e < 3; e++)
        {
            UINT32 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
            UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
            UINT64 key = ((UINT64)lo << 32) + hi + 1u;
            UINT32 slot = d2d_fill_aa_edge_slot(key, map_size);
            size_t probe = 0;
            while (probe < map_size && edge_keys[slot] && edge_keys[slot] != key)
            {
                slot = d2d_fill_aa_next_slot(slot, map_size);
                probe++;
            }
            if (probe == map_size)
            {
                free(face_slots);
                free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
                return FALSE;
            }
            edge_keys[slot] = key;
            edge_cnt[slot]++;
            if (face_slots)
                face_slots[3 * i + e] = slot;
        }
    }

    /* The seams towards curve triangles get no skirt: the curve triangle continues
     * the fill there, antialiases itself and has a skirt of its own on its open
     * side. The fill ramps towards a seam by the distance to the curve instead,
     * which is where the inner half of the ramp has to come from wherever the
     * curve triangle is thinner than half a pixel. */
    if (curve_count && d2d_fill_aa_thin() && (curve_ends = malloc(curve_count * 2 * sizeof(*curve_ends))))
        memset(curve_ends, 0xff, curve_count * 2 * sizeof(*curve_ends));
    if (curve_count && !d2d_fill_aa_mark_curve_edges(fill, edge_keys, edge_curve, seams, map_size, m, curve_ends))
    {
        free(curve_ends);
        free(face_slots);
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
        return FALSE;
    }

    /* The hairline gaps. The mesh goes on with them zipped: the edges of the
     * faces that were split leave the edge map and those of the new faces
     * join it. The curve seams stay as they were, only straight outline edges
     * are split and no vertex of the mesh goes. */
    if (face_slots)
    {
        BYTE *replaced, *added;
        BOOL ok = TRUE;
        size_t j;

        d2d_device_context_get_pixel_transform(render_target, &geometry->transform, pm);
        if (fabs(pm[0] * pm[4] - pm[1] * pm[3]) > 1.0e-12 && d2d_fill_aa_close_gaps(fill, edge_cnt,
                edge_curve, face_slots, pm, closed, &replaced, &added))
        {
            for (i = 0; i < fc + closed->face_count && ok; ++i)
            {
                const struct d2d_face *f = i < fc ? &fill->faces[i] : &closed->faces[i - fc];
                int e;

                if (i < fc ? !replaced[i] : !added[i - fc])
                    continue;
                for (e = 0; e < 3; ++e)
                {
                    UINT32 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
                    UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
                    UINT64 key = ((UINT64)lo << 32) + hi + 1u;
                    UINT32 slot = d2d_fill_aa_edge_slot(key, map_size);

                    for (j = 0; j < map_size && edge_keys[slot] && edge_keys[slot] != key; ++j)
                        slot = d2d_fill_aa_next_slot(slot, map_size);
                    if (j == map_size)
                    {
                        ok = FALSE;
                        break;
                    }
                    edge_keys[slot] = key;
                    if (i < fc)
                        --edge_cnt[slot];
                    else
                        ++edge_cnt[slot];
                }
            }
            free(replaced);
            free(added);
            if (!ok)
            {
                free(curve_ends);
                free(face_slots);
                free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
                return FALSE;
            }
            fill = closed;
            fc = fill->face_count;
        }
        free(face_slots);
    }

    /* Count the boundary edges that get a skirt, i.e. everything but the seams,
     * and the seams the fill ramps towards. */
    for (i = 0; i < map_size; i++)
    {
        if (!edge_keys[i] || edge_cnt[i] != 1) continue;
        if (!edge_curve[i]) boundary_count++;
        else if (seams[i].start != ~0u) seam_count++;
    }

    /* Nothing to antialias; the plain path draws exactly the same pixels more cheaply. */
    if (!boundary_count && !seam_count)
    {
        free(curve_ends);
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
        return FALSE;
    }

    if (neighbours && (fan = calloc(fc, sizeof(*fan))))
        d2d_fill_aa_mark_fans(fill, fan);

    /* The parts that are drawn by their pixels; see d2d_fill_aa_thin(). */
    d2d_device_context_get_pixel_transform(render_target, &geometry->transform, pm);
    pdet = pm[0] * pm[4] - pm[1] * pm[3];
    if (curve_ends)
        *curve_skip = calloc(curve_count, sizeof(**curve_skip));
    /* Without the means to leave out the curve triangles of a part they
     * would be drawn on top of its pixels. */
    if (d2d_fill_aa_thin() && fabs(pdet) > 1.0e-12 && (!curve_count || *curve_skip)
            && (thin = calloc(fc, sizeof(*thin))))
    {
        d2d_fill_aa_mark_thin(render_target, fill, edge_keys, edge_cnt, map_size, pm, m,
                curve_ends, *curve_skip, thin, &pixels, &pixel_count, &pixels_size);
        pixel_count = d2d_fill_aa_merge_pixels(pixels, pixel_count);
        for (i = 0; i < fc; ++i)
            thin_count += thin[i];
    }

    /* Allocate: inner triangles (3 per face) + skirt quads (6 per boundary edge)
     * + pixel quads (6 per pixel) + a triangle for the wedge at one end of
     * every boundary edge. */
    expanded_count = (fc - thin_count) * 3 + boundary_count * 6 + pixel_count * 6;
    if (boundary_count > 1 && d2d_fill_aa_corners() && fabs(pdet) > 1.0e-12
            && (corners = calloc(fill->vertex_count, sizeof(*corners))))
    {
        expanded_count += boundary_count * 3;
        if (d2d_fill_aa_bridges())
        {
            edges = malloc(2 * boundary_count * sizeof(*edges));
            slivers = malloc(boundary_count * sizeof(*slivers));
            bridge_stack = malloc((fill->vertex_count + 2 * boundary_count) * sizeof(*bridge_stack));
            if (!edges || !slivers || !bridge_stack)
            {
                free(edges);
                free(slivers);
                free(bridge_stack);
                edges = NULL;
                slivers = NULL;
                bridge_stack = NULL;
            }
        }
    }
    expanded = calloc(expanded_count, sizeof(*expanded));
    on_outline = calloc(fill->vertex_count, sizeof(*on_outline));
    if (!expanded || !on_outline)
    {
        free(expanded);
        free(on_outline);
        free(corners);
        free(edges);
        free(slivers);
        free(bridge_stack);
        free(fan);
        free(thin);
        free(pixels);
        free(curve_ends);
        free(*curve_skip);
        *curve_skip = NULL;
        free(edge_keys); free(edge_cnt); free(edge_processed); free(edge_curve); free(seams);
        return FALSE;
    }

    /* Without it every corner keeps the plain bound. */
    if (d2d_fill_aa_notches() && fabs(pdet) > 1.0e-12)
        turns = calloc(fill->vertex_count, sizeof(*turns));

    /* Without the neighbours the triangles keep their own edges only. */
    if (neighbours)
    {
        outline.nb = calloc(fill->vertex_count, 2 * sizeof(*outline.nb));
        outline.nb_cnt = calloc(fill->vertex_count, sizeof(*outline.nb_cnt));
        /* Without it the faces of the triangulation keep their own edges. */
        if (d2d_fill_aa_faces() && fabs(pdet) > 1.0e-12)
        {
            outline.opp = calloc(fill->vertex_count, 2 * sizeof(*outline.opp));
            outline.px = malloc(fill->vertex_count * 2 * sizeof(*outline.px));
            outline.tangent = malloc(fill->vertex_count * 4 * sizeof(*outline.tangent));
            outline.tangent_ok = calloc(fill->vertex_count, 2 * sizeof(*outline.tangent_ok));
            if (!outline.opp || !outline.px || !outline.tangent || !outline.tangent_ok)
            {
                free(outline.opp);
                free(outline.px);
                free(outline.tangent);
                free(outline.tangent_ok);
                outline.opp = NULL;
                outline.px = outline.tangent = NULL;
                outline.tangent_ok = NULL;
            }
        }
        if (!outline.nb || !outline.nb_cnt || !fan)
        {
            free(outline.nb);
            free(outline.nb_cnt);
            free(outline.opp);
            free(outline.px);
            free(outline.tangent);
            free(outline.tangent_ok);
            outline.nb = NULL;
            outline.nb_cnt = NULL;
            outline.opp = NULL;
            outline.px = outline.tangent = NULL;
            outline.tangent_ok = NULL;
            neighbours = FALSE;
        }
        outline.fill = fill;
        outline.edge_keys = edge_keys;
        outline.edge_curve = edge_curve;
        outline.seams = seams;
        outline.map_size = map_size;
    }

    /* The fill vertices that lie on the outline: both ends of a straight boundary
     * edge and of a chord, the curve end of a control edge. */
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &fill->faces[i];
        int e;

        for (e = 0; e < 3; e++)
        {
            UINT32 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
            UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
            UINT64 key = ((UINT64)lo << 32) + hi + 1u;
            UINT32 slot = d2d_fill_aa_edge_slot(key, map_size);

            while (edge_keys[slot] != key)
                slot = d2d_fill_aa_next_slot(slot, map_size);
            if (edge_cnt[slot] != 1)
                continue;
            if (neighbours)
            {
                const D2D1_POINT_2F *ta = &fill->vertices[b_v], *tb = &fill->vertices[a];

                if (edge_curve[slot])
                {
                    /* A seam whose curve is not known, or a control edge, with fill on either side. */
                    if (seams[slot].start == ~0u || !seams[slot].chord)
                        ta = tb = NULL;
                    else
                        ta = tb = &seams[slot].control;
                }
                d2d_fill_aa_outline_add(&outline, a, b_v, f->v[e], ta, pm);
                d2d_fill_aa_outline_add(&outline, b_v, a, f->v[e], tb, pm);
            }
            if (!edge_curve[slot] || (seams[slot].start != ~0u && seams[slot].chord))
                on_outline[a] = on_outline[b_v] = 1;
            else if (seams[slot].start == a || seams[slot].start == b_v)
                on_outline[seams[slot].start] = 1;
            if (turns)
                d2d_fill_aa_turn_add(fill, turns, a, b_v, f->v[e], edge_curve[slot] ? &seams[slot] : NULL, pm, pdet);
        }
    }

    if (outline.opp)
        d2d_fill_aa_outline_prepare(&outline, pm);

    /* The corners that point inward; see d2d_fill_aa_notches(). */
    for (i = 0; turns && i < fill->vertex_count; ++i)
    {
        if (on_outline[i])
            d2d_fill_aa_notch(fill, &turns[i], i, pm);
    }

    /* Pass 2: build inner triangle vertices with edge weights. */
    vi = 0;
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &fill->faces[i];
        float w[3][3], t[3][3], c1[3], c2[3];
        BOOL free_weight[3];
        UINT32 slots[3];
        int j, e;

        if (thin && thin[i])
            continue;

        for (e = 0; e < 3; e++)
        {
            UINT32 a = f->v[(e + 1) % 3], b_v = f->v[(e + 2) % 3];
            UINT32 lo = a < b_v ? a : b_v, hi = a < b_v ? b_v : a;
            UINT64 key = ((UINT64)lo << 32) + hi + 1u;
            UINT32 slot = d2d_fill_aa_edge_slot(key, map_size);
            BOOL ramp;

            while (edge_keys[slot] != key)
                slot = d2d_fill_aa_next_slot(slot, map_size);
            slots[e] = slot;
            c1[e] = c2[e] = 0.0f;
            t[e][0] = t[e][1] = t[e][2] = 0.0f;
            /* The weight of a boundary edge runs from 0 on the edge to 1 at the
             * opposite corner. An interior edge gets a large value (>> 1.0), so
             * it never limits min_edge_px. A seam ramps like a boundary edge,
             * shifted by the curve's distance from it; a seam whose curve is not
             * known counts as interior, because fading the fill out along it
             * would draw a line into the shape where the curve triangle
             * continues it. */
            if (edge_cnt[slot] != 1)
                ramp = FALSE;
            else if (!edge_curve[slot])
            {
                ramp = TRUE;
                /* A straight outline edge of the face; see d2d_fill_aa_notch_edges(). */
                if (d2d_fill_aa_notch_edges())
                    c1[e] = -1.0f;
            }
            else
                ramp = seams[slot].start != ~0u
                        && d2d_fill_aa_seam_params(fill, f, e, &seams[slot], t[e], &c1[e], &c2[e]);
            for (j = 0; j < 3; j++)
                w[e][j] = ramp ? (j == e ? 1.0f : 0.0f) : 10.0f;
            free_weight[e] = !ramp;
        }

        /* The interior edges take the outline edges next to the triangle's
         * own; see d2d_fill_aa_neighbours(). */
        if (neighbours && fan[i] && edge_cnt[slots[0]] == 1 && edge_cnt[slots[1]] == 2
                && edge_cnt[slots[2]] == 2 && !on_outline[f->v[0]])
        {
            BOOL taken[3] = {FALSE, FALSE, FALSE};

            for (e = 0; e < 3; e++)
            {
                const D2D1_POINT_2F *own_control = NULL;
                int end;

                if (edge_cnt[slots[e]] != 1)
                    continue;
                if (edge_curve[slots[e]] && seams[slots[e]].start != ~0u && seams[slots[e]].chord)
                    own_control = &seams[slots[e]].control;

                /* The edge is (a, b) with a = v[e + 1] and b = v[e + 2]; the
                 * interior edge that ends in a is the one opposite b. */
                for (end = 0; end < 2; end++)
                {
                    int ia = (e + 1 + end) % 3, ib = (e + 2 - end) % 3;
                    D2D1_POINT_2F n;
                    float dk, db;

                    if (edge_cnt[slots[ib]] == 1 || taken[ib])
                        continue;
                    if (!d2d_fill_aa_neighbour_plane(&outline, f->v[e], f->v[ia], f->v[ib], own_control, &n, &dk, &db))
                        continue;
                    w[ib][e] = dk;
                    w[ib][ia] = 0.0f;
                    w[ib][ib] = db;
                    taken[ib] = TRUE;
                }
            }
        }
        else if (outline.opp)
        {
            d2d_fill_aa_face_planes(&outline, f, free_weight, w);
        }

        for (j = 0; j < 3; j++, vi++)
        {
            expanded[vi].pos = fill->vertices[f->v[j]];
            expanded[vi].u = w[0][j];
            expanded[vi].v = w[1][j];
            expanded[vi].s = w[2][j];
            for (e = 0; e < 3; e++)
            {
                const D2D1_POINT_2F *corner = &fill->vertices[f->v[e]];

                expanded[vi].seam_t[e] = t[e][j];
                expanded[vi].seam_c1[e] = c1[e];
                expanded[vi].seam_c2[e] = c2[e];
                expanded[vi].corner[e][0] = expanded[vi].pos.x - corner->x;
                expanded[vi].corner[e][1] = expanded[vi].pos.y - corner->y;
                expanded[vi].corner_on[e] = on_outline[f->v[e]];
                if (turns && turns[f->v[e]].reach > 0.0f)
                    expanded[vi].corner_on[e] = d2d_fill_aa_notch_code(&turns[f->v[e]]);
            }
        }
    }

    /* Pass 3: build skirt quads along boundary edges. */
    for (i = 0; i < fc; i++)
    {
        const struct d2d_face *f = &fill->faces[i];
        int e;

        if (thin && thin[i])
            continue;
        for (e = 0; e < 3; e++)
        {
            UINT32 ai = f->v[(e + 1) % 3], bi = f->v[(e + 2) % 3];
            UINT32 lo = ai < bi ? ai : bi, hi = ai < bi ? bi : ai;
            UINT64 key = ((UINT64)lo << 32) + hi + 1u;
            UINT32 slot = d2d_fill_aa_edge_slot(key, map_size);
            D2D1_POINT_2F pa, pb, pc, edge_dir, outward;
            float dot_test, edge_len;

            while (edge_keys[slot] != key)
                slot = d2d_fill_aa_next_slot(slot, map_size);
            /* Skip interior edges, curve seams and already-processed boundary edges. */
            if (edge_cnt[slot] != 1 || edge_curve[slot] || edge_processed[slot]) continue;

            /* Compute outward normal (away from opposite vertex). */
            pa = fill->vertices[ai];
            pb = fill->vertices[bi];
            pc = fill->vertices[f->v[e]]; /* opposite vertex */
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
            d2d_fill_aa_write_skirt(&expanded[vi], &pa, &pb, &outward);

            /* Mark this edge as processed to avoid a duplicate skirt from the adjacent face. */
            edge_processed[slot] = 1;
            if (corners)
            {
                d2d_fill_aa_corner_add(&corners[ai], bi, &outward, edge_count);
                d2d_fill_aa_corner_add(&corners[bi], ai, &outward, edge_count);
            }
            if (edges && edge_count < boundary_count)
            {
                edges[edge_count].a = ai;
                edges[edge_count].b = bi;
                edges[edge_count].slot = vi;
                edges[edge_count].parent = ~0u;
                edges[edge_count].depth = 0.0f;
                edges[edge_count].n = outward;
            }
            ++edge_count;
            vi += 6;
        }
    }

    /* The outline without its dents; see d2d_fill_aa_bridges(). */
    if (edges && edge_count <= boundary_count)
        d2d_fill_aa_bridge(fill, corners, edges, edge_count, slivers, bridge_stack, pm, expanded);

    /* The Bézier triangles drawn with skirts across their control edges, by
     * the vertex an end of their curve lies on: twice the triangle plus the
     * end, plus one, or ~0u for more than one. */
    if (corners && curve_ends && fill->bezier_vertex_count
            && (corner_curves = calloc(fill->vertex_count, sizeof(*corner_curves))))
    {
        for (i = 0; i < fill->bezier_vertex_count / 3; ++i)
        {
            const struct d2d_curve_vertex *bz = &fill->bezier_vertices[3 * i];
            unsigned int end;

            if (!(bz->texcoord.sign < 0.0f) || (*curve_skip && (*curve_skip)[i]) || d2d_curve_is_flat(bz, m))
                continue;
            for (end = 0; end < 2; ++end)
            {
                UINT32 v = curve_ends[2 * i + end];

                if (v != ~0u)
                    corner_curves[v] = corner_curves[v] ? ~0u : 2 * i + end + 1;
            }
        }
    }

    /* The wedges between two skirts; see d2d_fill_aa_corners(). */
    for (i = 0; corners && i < fill->vertex_count && vi + 3 <= expanded_count; ++i)
    {
        const struct d2d_fill_aa_corner *c = &corners[i];
        const D2D1_POINT_2F *p = &fill->vertices[i], *pa, *pb, *third = NULL;
        double q[3][2], d[2][2], nb[2], len, cosine, shift;
        unsigned int k;

        if (c->count == 2)
        {
            /* The corner points outward where each skirt reaches on along the other edge. */
            pa = &fill->vertices[c->other[0]];
            pb = &fill->vertices[c->other[1]];
            if (!(c->n[1].x * (p->x - pa->x) + c->n[1].y * (p->y - pa->y) > 0.0f)
                    || !(c->n[0].x * (p->x - pb->x) + c->n[0].y * (p->y - pb->y) > 0.0f))
            {
                /* A corner that points inward by less than an eighth of a turn: both skirts end on the
                 * line halfway between their normals; see d2d_fill_aa_bridges(). */
                if (edges
                        && c->n[1].x * (p->x - pa->x) + c->n[1].y * (p->y - pa->y) < 0.0f
                        && c->n[0].x * (p->x - pb->x) + c->n[0].y * (p->y - pb->y) < 0.0f)
                {
                    double dn[2][2], bis[2], gx, gy;

                    for (k = 0; k < 2; ++k)
                    {
                        dn[k][0] = pm[0] * c->n[k].x + pm[1] * c->n[k].y;
                        dn[k][1] = pm[3] * c->n[k].x + pm[4] * c->n[k].y;
                        if (!((len = sqrt(dn[k][0] * dn[k][0] + dn[k][1] * dn[k][1])) > 1.0e-12))
                            break;
                        dn[k][0] /= len;
                        dn[k][1] /= len;
                    }
                    cosine = k == 2 ? dn[0][0] * dn[1][0] + dn[0][1] * dn[1][1] : 0.0;
                    /* Each skirt gives up 0.75 tan(turn / 2) of its length along the edge, and may have to at
                     * its other end as well. */
                    if (cosine > 0.70710678)
                    {
                        double need = 2.0 * 0.75 * sqrt((1.0 - cosine) / (1.0 + cosine)), ex, ey;

                        ex = pm[0] * (p->x - pa->x) + pm[1] * (p->y - pa->y);
                        ey = pm[3] * (p->x - pa->x) + pm[4] * (p->y - pa->y);
                        if (!(ex * ex + ey * ey > need * need))
                            cosine = 0.0;
                        ex = pm[0] * (p->x - pb->x) + pm[1] * (p->y - pb->y);
                        ey = pm[3] * (p->x - pb->x) + pm[4] * (p->y - pb->y);
                        if (!(ex * ex + ey * ey > need * need))
                            cosine = 0.0;
                    }
                    if (cosine > 0.70710678)
                    {
                        bis[0] = dn[0][0] + dn[1][0];
                        bis[1] = dn[0][1] + dn[1][1];
                        len = sqrt(bis[0] * bis[0] + bis[1] * bis[1]);
                        bis[0] /= len;
                        bis[1] /= len;
                        /* Back to the space of the geometry; the vertex shader sets the length. */
                        gx = (pm[4] * bis[0] - pm[1] * bis[1]) / pdet;
                        gy = (pm[0] * bis[1] - pm[3] * bis[0]) / pdet;
                        len = sqrt(gx * gx + gy * gy);
                        for (k = 0; k < 2 && len > 0.0; ++k)
                        {
                            const struct d2d_fill_aa_edge *e = &edges[c->edge[k]];
                            struct d2d_fill_aa_vertex *o = &expanded[e->slot];
                            float u = -(float)(bis[0] * dn[k][0] + bis[1] * dn[k][1]);
                            static const BYTE at_a[2] = {2, 5};
                            unsigned int j;

                            for (j = 0; j < (e->a == i ? 2u : 1u); ++j)
                            {
                                struct d2d_fill_aa_vertex *w = &o[e->a == i ? at_a[j] : 4];

                                w->ex = gx / len;
                                w->ey = gy / len;
                                w->u = u;
                            }
                        }
                    }
                }
                continue;
            }
        }
        else if (c->count != 1 || !corner_curves || !corner_curves[i] || corner_curves[i] == ~0u)
        {
            continue;
        }

        /* The corner and the outer vertices of the two skirts, in pixels, as the vertex shader places them. */
        q[0][0] = pm[0] * p->x + pm[1] * p->y + pm[2];
        q[0][1] = pm[3] * p->x + pm[4] * p->y + pm[5];
        for (k = 0; k < c->count; ++k)
        {
            d[k][0] = pm[0] * c->n[k].x + pm[1] * c->n[k].y;
            d[k][1] = pm[3] * c->n[k].x + pm[4] * c->n[k].y;
            if (!((len = sqrt(d[k][0] * d[k][0] + d[k][1] * d[k][1])) > 1.0e-12))
                break;
            d[k][0] /= len;
            d[k][1] /= len;
            q[k + 1][0] = q[0][0] + 0.75 * d[k][0];
            q[k + 1][1] = q[0][1] + 0.75 * d[k][1];
        }
        if (k < c->count)
            continue;

        if (c->count == 2)
        {
            cosine = d[0][0] * d[1][0] + d[0][1] * d[1][1];
            if (!(cosine > D2D_FILL_AA_CORNER_COS) || !d2d_curve_triangle_holds_centre(q[0], q[1], q[2]))
                continue;
            nb[0] = d[0][0] + d[1][0];
            nb[1] = d[0][1] + d[1][1];
            len = sqrt(nb[0] * nb[0] + nb[1] * nb[1]);
            nb[0] /= len;
            nb[1] /= len;
            shift = d2d_fill_aa_corner_depth(fill, &outline, pm, i, c->other[0], q[0], nb)
                    + d2d_fill_aa_corner_depth(fill, &outline, pm, i, c->other[1], q[0], nb);
        }
        else
        {
            const struct d2d_curve_vertex *bz = &fill->bezier_vertices[3 * ((corner_curves[i] - 1) / 2)];
            unsigned int end = (corner_curves[i] - 1) & 1;
            double e[2], qc[2], x, area, det = (double)m[0] * m[3] - (double)m[1] * m[2];
            struct d2d_curve_vertex skirt[6];
            float inv[4], normal[2], fdet;

            /* The vertex has to be the end of the curve itself, not one a float away from it. */
            if (memcmp(&bz[2 * end].position, p, sizeof(*p)) || !(fabs(det) > 1.0e-12))
                continue;
            /* As d2d_device_context_curve_skirts() computes them. */
            fdet = m[0] * m[3] - m[1] * m[2];
            if (!(fabsf(fdet) > 1e-12f))
                continue;
            inv[0] = m[3] / fdet;
            inv[1] = -m[1] / fdet;
            inv[2] = -m[2] / fdet;
            inv[3] = m[0] / fdet;
            if (!d2d_curve_add_skirt(skirt, bz, end, end + 1, end ? 0 : 2, m, inv, normal))
                continue;
            third = &skirt[end ? 4 : 2].position;
            d[1][0] = normal[0];
            d[1][1] = normal[1];
            q[2][0] = pm[0] * third->x + pm[1] * third->y + pm[2];
            q[2][1] = pm[3] * third->x + pm[4] * third->y + pm[5];
            qc[0] = pm[0] * bz[1].position.x + pm[1] * bz[1].position.y + pm[2] - q[0][0];
            qc[1] = pm[3] * bz[1].position.x + pm[4] * bz[1].position.y + pm[5] - q[0][1];

            /* The wedge is open where each skirt reaches on along the edge of the other. */
            pa = &fill->vertices[c->other[0]];
            e[0] = pm[0] * (p->x - pa->x) + pm[1] * (p->y - pa->y);
            e[1] = pm[3] * (p->x - pa->x) + pm[4] * (p->y - pa->y);
            if (!(d[1][0] * e[0] + d[1][1] * e[1] > 0.0) || !(d[0][0] * -qc[0] + d[0][1] * -qc[1] > 0.0))
                continue;
            cosine = d[0][0] * d[1][0] + d[0][1] * d[1][1];
            if (!(cosine > D2D_FILL_AA_CORNER_COS) || !d2d_curve_triangle_holds_centre(q[0], q[1], q[2]))
                continue;
            nb[0] = d[0][0] + d[1][0];
            nb[1] = d[0][1] + d[1][1];
            len = sqrt(nb[0] * nb[0] + nb[1] * nb[1]);
            nb[0] /= len;
            nb[1] /= len;
            shift = d2d_fill_aa_corner_depth(fill, &outline, pm, i, c->other[0], q[0], nb);
            /* The curve: its control edge and, see d2d_curve_join_skirts(), its curvature at this end. */
            x = fabs(qc[0] * -nb[1] + qc[1] * nb[0]);
            len = sqrt(qc[0] * qc[0] + qc[1] * qc[1]);
            if (x > 1.0e-9)
                shift += -(qc[0] * nb[0] + qc[1] * nb[1]) / x / 8.0;
            area = 0.5 * fabs(det * ((bz[1].position.x - bz[0].position.x) * (bz[2].position.y - bz[0].position.y)
                    - (bz[1].position.y - bz[0].position.y) * (bz[2].position.x - bz[0].position.x)));
            if (len > 1.0e-6)
                shift += min(area / (len * len * len) / 48.0, 0.125);
        }
        shift = max(shift, 0.0);

        for (k = 0; k < 3; ++k, ++vi)
        {
            expanded[vi].pos = k == 2 && third ? *third : *p;
            expanded[vi].u = -((q[k][0] - q[0][0]) * nb[0] + (q[k][1] - q[0][1]) * nb[1]) - shift;
            expanded[vi].v = 10.0f;
            expanded[vi].s = 10.0f;
            if (k && !(k == 2 && third))
            {
                expanded[vi].ex = c->n[k - 1].x;
                expanded[vi].ey = c->n[k - 1].y;
            }
        }
    }

    /* The pixels of the thin figures: a quad over each, with a weight that
     * rises by 1 per pixel from left to right and is the coverage less a half
     * at the centre. */
    for (i = 0; i < pixel_count && vi + 6 <= expanded_count; ++i)
    {
        static const unsigned int corners[6] = {0, 1, 2, 1, 3, 2};
        unsigned int k;

        for (k = 0; k < 6; ++k, ++vi)
        {
            double px = pixels[i].x + (corners[k] & 1) - pm[2], py = pixels[i].y + (corners[k] >> 1) - pm[5];

            expanded[vi].pos.x = (pm[4] * px - pm[1] * py) / pdet;
            expanded[vi].pos.y = (pm[0] * py - pm[3] * px) / pdet;
            expanded[vi].u = pixels[i].coverage - 1.0f + (corners[k] & 1);
            expanded[vi].v = 10.0f;
            expanded[vi].s = 10.0f;
        }
    }
    expanded_count = vi; /* actual count (may be less if degenerate edges skipped) */
    free(thin);
    free(pixels);
    free(curve_ends);
    free(edge_keys);
    free(edge_cnt);
    free(edge_processed);
    free(edge_curve);
    free(seams);
    free(on_outline);
    free(outline.nb);
    free(outline.nb_cnt);
    free(outline.opp);
    free(outline.px);
    free(outline.tangent);
    free(outline.tangent_ok);
    free(fan);
    free(corners);
    free(corner_curves);
    free(edges);
    free(slivers);
    free(bridge_stack);
    free(turns);

    *out = expanded;
    *out_count = expanded_count;
    return TRUE;
}

static BOOL d2d_device_context_fill_aa_expand(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_fill *fill,
        struct d2d_fill_aa_vertex **out, size_t *out_count, BYTE **curve_skip)
{
    struct d2d_fill closed = {0};
    BOOL ret;

    ret = d2d_device_context_fill_aa_expand_mesh(render_target, geometry, fill, &closed, out, out_count, curve_skip);
    free(closed.vertices);
    free(closed.faces);
    return ret;
}

static BOOL d2d_device_context_fill_triangles_aa(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_fill *fill,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush, BYTE **curve_skip)
{
    struct d2d_fill_aa_vertex *expanded;
    size_t expanded_count;
    ID3D11Buffer *vb;
    HRESULT hr;

    if (!d2d_device_context_fill_aa_expand(render_target, geometry, fill, &expanded, &expanded_count, curve_skip))
        return FALSE;

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target,
            D3D11_BIND_VERTEX_BUFFER,
            expanded_count * sizeof(*expanded),
            &render_target->scratch_vb[D2D_SHAPE_TYPE_FILL_AA],
            expanded, &vb)))
    {
        free(expanded);
        free(*curve_skip);
        *curve_skip = NULL;
        return FALSE;
    }
    free(expanded);

    d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_FILL_AA, NULL, expanded_count, vb,
            sizeof(*expanded), brush, opacity_brush);
    return TRUE;
}

/* Append a skirt across the edge i0-i1 of the curve triangle v, on the side away
 * from its corner io: a quad reaching CURVE_SKIRT_PIXELS outward in screen space.
 * The texture coordinates of the outer corners continue the affine mapping of the
 * triangle, so the pixel shader evaluates the same implicit curve there and the
 * antialiasing ramp simply carries on past the triangle edge. normal, if not
 * NULL, receives the unit vector in pixels along which the skirt reaches out. */
#define D2D_CURVE_SKIRT_PIXELS 0.75f
static unsigned int d2d_curve_add_skirt(struct d2d_curve_vertex *out, const struct d2d_curve_vertex *v,
        unsigned int i0, unsigned int i1, unsigned int io, const float *m, const float *inv, float *normal)
{
    const D2D1_POINT_2F *p0 = &v[0].position, *p1 = &v[1].position, *p2 = &v[2].position;
    float ex, ey, len, nx, ny, dx, dy, det;
    struct d2d_curve_vertex outer[2];
    D2D1_POINT_2F a, b, o;
    unsigned int k;

    /* Edge and opposite corner in pixel space; translation does not matter for directions. */
    a.x = m[0] * v[i0].position.x + m[1] * v[i0].position.y;
    a.y = m[2] * v[i0].position.x + m[3] * v[i0].position.y;
    b.x = m[0] * v[i1].position.x + m[1] * v[i1].position.y;
    b.y = m[2] * v[i1].position.x + m[3] * v[i1].position.y;
    o.x = m[0] * v[io].position.x + m[1] * v[io].position.y;
    o.y = m[2] * v[io].position.x + m[3] * v[io].position.y;
    ex = b.x - a.x;
    ey = b.y - a.y;
    if (!((len = sqrtf(ex * ex + ey * ey)) > 1e-4f))
        return 0;
    nx = -ey / len;
    ny = ex / len;
    if (nx * (a.x - o.x) + ny * (a.y - o.y) < 0.0f)
    {
        nx = -nx;
        ny = -ny;
    }
    if (normal)
    {
        normal[0] = nx;
        normal[1] = ny;
    }
    nx *= D2D_CURVE_SKIRT_PIXELS;
    ny *= D2D_CURVE_SKIRT_PIXELS;
    /* Back to geometry space. */
    dx = inv[0] * nx + inv[1] * ny;
    dy = inv[2] * nx + inv[3] * ny;

    det = (p1->x - p0->x) * (p2->y - p0->y) - (p1->y - p0->y) * (p2->x - p0->x);
    if (!(fabsf(det) > 1e-12f))
        return 0;

    for (k = 0; k < 2; ++k)
    {
        const struct d2d_curve_vertex *src = &v[k ? i1 : i0];
        float qx = src->position.x + dx, qy = src->position.y + dy, l1, l2, l0;

        l1 = ((qx - p0->x) * (p2->y - p0->y) - (qy - p0->y) * (p2->x - p0->x)) / det;
        l2 = ((p1->x - p0->x) * (qy - p0->y) - (p1->y - p0->y) * (qx - p0->x)) / det;
        l0 = 1.0f - l1 - l2;
        outer[k].position.x = qx;
        outer[k].position.y = qy;
        outer[k].texcoord.u = l0 * v[0].texcoord.u + l1 * v[1].texcoord.u + l2 * v[2].texcoord.u;
        outer[k].texcoord.v = l0 * v[0].texcoord.v + l1 * v[1].texcoord.v + l2 * v[2].texcoord.v;
        outer[k].texcoord.sign = src->texcoord.sign;
    }

    out[0] = v[i0];
    out[1] = v[i1];
    out[2] = outer[0];
    out[3] = v[i1];
    out[4] = outer[1];
    out[5] = outer[0];
    return 6;
}

/* The unit vector halfway between the unit vectors a and b. */
static BOOL d2d_curve_bisect(const double *a, const double *b, double *out)
{
    double x = a[0] + b[0], y = a[1] + b[1], len;

    if (!((len = sqrt(x * x + y * y)) > 1e-4))
        return FALSE;
    out[0] = x / len;
    out[1] = y / len;
    return TRUE;
}

/* Whether the triangle a, b, c holds the centre of a pixel, in pixels with pixel
 * i covering [i, i + 1]. A centre within D2D_CURVE_CORNER_SLACK of an edge
 * counts: the rasteriser places the vertices on a grid of its own. */
#define D2D_CURVE_CORNER_SLACK 0.03
static BOOL d2d_curve_triangle_holds_centre(const double *a, const double *b, const double *c)
{
    double x0 = min(a[0], min(b[0], c[0])), x1 = max(a[0], max(b[0], c[0]));
    double y0 = min(a[1], min(b[1], c[1])), y1 = max(a[1], max(b[1], c[1]));
    double area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]), s = area < 0.0 ? -1.0 : 1.0;
    double px, py, slack = D2D_CURVE_CORNER_SLACK;

    for (py = ceil(y0 - slack - 0.5) + 0.5; py <= y1 + slack; py += 1.0)
    {
        for (px = ceil(x0 - slack - 0.5) + 0.5; px <= x1 + slack; px += 1.0)
        {
            /* Twice the areas the centre spans with each edge; an edge is at most 1.5 pixels long. */
            if (s * ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) >= -1.5 * slack
                    && s * ((c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])) >= -1.5 * slack
                    && s * ((a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])) >= -1.5 * slack)
                return TRUE;
        }
    }
    return FALSE;
}

/* Close the wedge the skirts across the two control edges of the Bézier
 * triangle v leave open at its control point. Each skirt reaches outward at a
 * right angle to its own edge, and the edges turn at the control point by as
 * much as the curve piece does: beyond the control point a wedge that wide
 * belongs to neither. As long as the control point lies more than half a pixel
 * outside the curve nothing is drawn there anyway. Next to a curve piece
 * smaller than a pixel it is closer, a corner of radius 1 has it 0.08 pixels
 * outside, and a pixel centre in the wedge got no coverage where the figure
 * covers a quarter of the pixel. The wedge is filled with a fan of up to four
 * triangles around the control point, as far out as the skirts reach; a
 * triangle that holds no pixel centre draws nothing and is left out.
 *
 * a and b are the outer vertices the skirts across the edges 0-1 and 1-2 have
 * at the control point. The fan starts and ends on them: only an edge between
 * the same two vertices is one the rasteriser gives to exactly one of its two
 * triangles, and a pixel centre on an edge that merely runs along that of a
 * skirt was drawn twice or not at all. pm maps the geometry to pixels, see
 * d2d_device_context_get_pixel_transform().
 *
 * The texture coordinates do not continue the affine mapping of the triangle
 * as those of the skirts do. The pixel shader takes the distance from the
 * curve to be the implicit function over the length of its gradient, and
 * several lengths of a small triangle away from it, which is where the fan
 * lies, that comes out at half the distance and less. With u = 0 the function
 * is -v, a plane, and the shader's distance is exact for the line where v is
 * 0. Each triangle of the fan takes for that line the tangent of the circle of
 * curvature the curve has halfway along, at the point of the circle the
 * triangle faces: next to a small piece the distance then grows in every
 * direction away from it, next to a large one the line is the tangent of the
 * curve nearest the control point.
 *
 * The triangles of an arc get no fan: their corner lies 0.41 radii outside the
 * curve, which is half a pixel at a radius of 1.2, and below that the wedge
 * holds no pixel with a coverage worth drawing. WINE_D2D_CURVE_CORNERS=0
 * leaves the wedge open for Béziers as well. */
#define D2D_CURVE_CORNER_TRIANGLES 4

static BOOL d2d_curve_corners(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_CURVE_CORNERS", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

static unsigned int d2d_curve_add_skirt_corner(struct d2d_curve_vertex *out, const struct d2d_curve_vertex *v,
        const struct d2d_curve_vertex *a, const struct d2d_curve_vertex *b, const float *na, const float *nb,
        const double *pm, const float *inv)
{
    double dir[D2D_CURVE_CORNER_TRIANGLES + 1][2], px[D2D_CURVE_CORNER_TRIANGLES + 1][2];
    double q[3][2], t[2], rel[2], ex, ey, e2, h, c, r;
    unsigned int i, j, k, count, hit = 0, written = 0;

    for (k = 0; k < 3; ++k)
    {
        q[k][0] = pm[0] * v[k].position.x + pm[1] * v[k].position.y + pm[2];
        q[k][1] = pm[3] * v[k].position.x + pm[4] * v[k].position.y + pm[5];
    }
    /* Halfway along, the curve runs parallel to the chord at half the height
     * the control point has above the chord, and nowhere is it closer to the
     * control point: beyond the reach of the antialiasing ramp the wedge holds
     * nothing to draw. h is that height times the length of the chord. */
    ex = q[2][0] - q[0][0];
    ey = q[2][1] - q[0][1];
    e2 = ex * ex + ey * ey;
    h = ex * (q[1][1] - q[0][1]) - ey * (q[1][0] - q[0][0]);
    if (!(e2 > 1e-8) || h * h > 4.0 * D2D_CURVE_SKIRT_PIXELS * D2D_CURVE_SKIRT_PIXELS * e2)
        return 0;

    /* One triangle up to a sixteenth of a turn between the normals, two up to an eighth, four beyond. */
    c = na[0] * nb[0] + na[1] * nb[1];
    count = c > 0.92387953 ? 1 : c > 0.70710678 ? 2 : 4;
    dir[0][0] = na[0];
    dir[0][1] = na[1];
    dir[count][0] = nb[0];
    dir[count][1] = nb[1];
    if (count > 1 && !d2d_curve_bisect(dir[0], dir[count], dir[count / 2]))
        return 0;
    if (count > 2 && (!d2d_curve_bisect(dir[0], dir[2], dir[1]) || !d2d_curve_bisect(dir[2], dir[4], dir[3])))
        return 0;
    for (i = 0; i <= count; ++i)
    {
        px[i][0] = q[1][0] + D2D_CURVE_SKIRT_PIXELS * dir[i][0];
        px[i][1] = q[1][1] + D2D_CURVE_SKIRT_PIXELS * dir[i][1];
    }
    for (i = 0; i < count; ++i)
    {
        if (d2d_curve_triangle_holds_centre(q[1], px[i], px[i + 1]))
            hit |= 1u << i;
    }
    if (!hit)
        return 0;

    /* The unit normal of the chord, towards the control point, the height of
     * the control point, the radius of curvature halfway along the curve, and
     * the control point as the centre of that circle sees it. */
    e2 = sqrt(e2);
    t[0] = -ey / e2;
    t[1] = ex / e2;
    if ((h /= e2) < 0.0)
    {
        t[0] = -t[0];
        t[1] = -t[1];
        h = -h;
    }
    if (!(h > 1e-6))
        return 0;
    r = e2 * e2 / (4.0 * h);
    rel[0] = 0.25 * (2.0 * q[1][0] - q[0][0] - q[2][0]) + r * t[0];
    rel[1] = 0.25 * (2.0 * q[1][1] - q[0][1] - q[2][1]) + r * t[1];

    for (i = 0; i < count; ++i)
    {
        struct d2d_curve_vertex *o = &out[written];
        double g[2], w[2], gl;

        if (!(hit & (1u << i)) || !d2d_curve_bisect(dir[i], dir[i + 1], w))
            continue;
        /* The direction in which the centre of the circle sees the middle of the triangle. */
        g[0] = rel[0] + 0.5 * D2D_CURVE_SKIRT_PIXELS * w[0];
        g[1] = rel[1] + 0.5 * D2D_CURVE_SKIRT_PIXELS * w[1];
        if (!((gl = sqrt(g[0] * g[0] + g[1] * g[1])) > 1e-12))
            continue;
        g[0] /= gl;
        g[1] /= gl;

        for (j = 0; j < 3; ++j)
        {
            double nx = j ? D2D_CURVE_SKIRT_PIXELS * dir[i + j - 1][0] : 0.0;
            double ny = j ? D2D_CURVE_SKIRT_PIXELS * dir[i + j - 1][1] : 0.0;

            if (!j)
                o[j].position = v[1].position;
            else if (i + j - 1 == 0)
                o[j].position = a->position;
            else if (i + j - 1 == count)
                o[j].position = b->position;
            else
            {
                o[j].position.x = v[1].position.x + inv[0] * (float)nx + inv[1] * (float)ny;
                o[j].position.y = v[1].position.y + inv[2] * (float)nx + inv[3] * (float)ny;
            }
            o[j].texcoord.u = 0.0f;
            o[j].texcoord.v = -(g[0] * (rel[0] + nx) + g[1] * (rel[1] + ny) - r);
            o[j].texcoord.sign = v[1].texcoord.sign;
        }
        written += 3;
    }

    return written;
}

/* Close the wedge the skirts of two curve triangles that follow each other
 * leave open at the point their curves share. Each skirt reaches outward at a
 * right angle to its own control edge and ends on the normal of that edge
 * through the common point. Where the tangent turns in that point the two
 * normals differ, and on the outside of the turn a wedge that wide belongs to
 * neither skirt. The quadratic pieces the four arc segments of an ellipse
 * with radii 3 and 3.66 are cut into turn by 1.3 degrees where two segments
 * meet: with the centre of the ellipse on a pixel centre, the pixel centres on
 * its two axes lie in such wedges, and the pixel beside its right point came
 * out at 0 of 255 for a coverage of 126 (wine-dev issue 633). Two curves that
 * go on in the same direction have the same normal in exact arithmetic only:
 * each skirt computes its outer vertex from its own edge.
 *
 * The wedge is filled with one triangle between the common point and the outer
 * vertices the two skirts have there, taken from the skirts: only an edge
 * between the same two vertices is one the rasteriser gives to exactly one of
 * its two triangles. A triangle that holds no pixel centre draws nothing and
 * is left out, and so is one where the tangent turns by more than a sixteenth
 * of a turn, which is a corner of the figure. Where the skirts overlap, on the
 * inside of a turn, nothing is added.
 *
 * As for the wedge at the control point (d2d_curve_add_skirt_corner()), the
 * texture coordinates make the implicit function a plane, u = 0 and -v the
 * distance in pixels from a line across the wedge. That line is the tangent in
 * the common point, moved inward by what the curve falls back from its tangent
 * on average over the width of a pixel, 1/24 of the curvature the two pieces
 * have there: the pixel shader takes the line for an edge across the whole
 * pixel, and with the tangent itself 172 of the 483 pixels that changed in
 * twelve frames of Minimal Audio Current's tap display came out more than 16
 * of 255 too bright.
 *
 * out holds, from joints[t].base, the triangle t of v, the skirt across its
 * edge 0-1 and the skirt across its edge 1-2, six vertices each as
 * d2d_curve_add_skirt() writes them: the outer vertex at corner 0 is the third
 * of the first skirt, the one at corner 2 the fifth of the second. m and pm
 * map the geometry to pixels, without and with the translation. Returns the
 * number of vertices appended at out[n]. WINE_D2D_CURVE_JOINTS=0 leaves the
 * wedges open. */
#define D2D_CURVE_JOINT_COS 0.92387953f
#define D2D_CURVE_JOINT_MAX_SAG 0.25

struct d2d_curve_joint
{
    /* The place of the triangle in the output, ~0u if it has no skirts across its control edges. */
    UINT32 base;
    /* The unit normals of those skirts, in pixels. */
    float n0[2], n1[2];
};

static BOOL d2d_curve_joints(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0)
        enabled = !(GetEnvironmentVariableA("WINE_D2D_CURVE_JOINTS", buf, sizeof(buf)) && !strcmp(buf, "0"));
    return enabled;
}

static size_t d2d_curve_join_skirts(struct d2d_curve_vertex *out, size_t n, const struct d2d_curve_vertex *v,
        size_t count, const struct d2d_curve_joint *joints, const float *m, const double *pm)
{
    size_t tri_count = count / 3, map_size = tri_count * 2 + 16, t, u, probe, written = 0;
    UINT32 *map = NULL, slot;
    BOOL mapped = FALSE;
    unsigned int j, k;

    for (t = 0; t < tri_count; ++t)
    {
        const D2D1_POINT_2F *p = &v[3 * t + 2].position, *ctrl = &v[3 * t + 1].position;
        struct d2d_curve_vertex *o = &out[n + written];
        const struct d2d_curve_vertex *src[3], *tri[2];
        double q[3][2], nb[2], len, sag = 0.0;
        float c, ex, ey;

        if (joints[t].base == ~0u)
            continue;

        /* The triangle that starts where this one ends is the next one, or
         * the first one of the figure. */
        u = t + 1;
        if (u == tri_count || joints[u].base == ~0u || memcmp(&v[3 * u].position, p, sizeof(*p)))
        {
            if (!mapped)
            {
                mapped = TRUE;
                if ((map = calloc(map_size, sizeof(*map))))
                {
                    for (u = 0; u < tri_count; ++u)
                    {
                        if (joints[u].base == ~0u)
                            continue;
                        slot = d2d_fill_aa_position_hash(&v[3 * u].position) % map_size;
                        while (map[slot])
                            slot = slot + 1 == map_size ? 0 : slot + 1;
                        map[slot] = u + 1;
                    }
                }
            }
            if (!map)
                continue;
            slot = d2d_fill_aa_position_hash(p) % map_size;
            for (probe = 0, u = tri_count; probe < map_size && map[slot]; ++probe)
            {
                if (!memcmp(&v[3 * (map[slot] - 1)].position, p, sizeof(*p)))
                {
                    u = map[slot] - 1;
                    break;
                }
                slot = slot + 1 == map_size ? 0 : slot + 1;
            }
            if (u == tri_count)
                continue;
        }
        if (u == t)
            continue;

        c = joints[t].n1[0] * joints[u].n0[0] + joints[t].n1[1] * joints[u].n0[1];
        if (!(c > D2D_CURVE_JOINT_COS))
            continue;
        /* The wedge is open where the normal of the second skirt points on
         * along the control edge of the first. */
        ex = m[0] * (p->x - ctrl->x) + m[1] * (p->y - ctrl->y);
        ey = m[2] * (p->x - ctrl->x) + m[3] * (p->y - ctrl->y);
        if (!(joints[u].n0[0] * ex + joints[u].n0[1] * ey > 0.0f))
            continue;

        /* The common point and the outer vertices the two skirts have there. */
        src[0] = &v[3 * t + 2];
        src[1] = &out[joints[t].base + 13];
        src[2] = &out[joints[u].base + 5];
        for (k = 0; k < 3; ++k)
        {
            q[k][0] = pm[0] * src[k]->position.x + pm[1] * src[k]->position.y + pm[2];
            q[k][1] = pm[3] * src[k]->position.x + pm[4] * src[k]->position.y + pm[5];
        }
        if (!d2d_curve_triangle_holds_centre(q[0], q[1], q[2]))
            continue;
        nb[0] = joints[t].n1[0] + joints[u].n0[0];
        nb[1] = joints[t].n1[1] + joints[u].n0[1];
        if (!((len = sqrt(nb[0] * nb[0] + nb[1] * nb[1])) > 1e-4))
            continue;

        /* The curvature of a quadratic Bézier at one of its ends is the area
         * of its triangle over the cube of the control edge that ends there;
         * the mean of the two pieces, over 24. */
        tri[0] = &v[3 * t];
        tri[1] = &v[3 * u];
        for (j = 0; j < 2; ++j)
        {
            double a[3][2], area, l;

            for (k = 0; k < 3; ++k)
            {
                a[k][0] = pm[0] * tri[j][k].position.x + pm[1] * tri[j][k].position.y;
                a[k][1] = pm[3] * tri[j][k].position.x + pm[4] * tri[j][k].position.y;
            }
            area = 0.5 * fabs((a[1][0] - a[0][0]) * (a[2][1] - a[0][1]) - (a[1][1] - a[0][1]) * (a[2][0] - a[0][0]));
            l = sqrt((a[2 - j][0] - a[1 - j][0]) * (a[2 - j][0] - a[1 - j][0])
                    + (a[2 - j][1] - a[1 - j][1]) * (a[2 - j][1] - a[1 - j][1]));
            if (l > 1e-6)
                sag += area / (l * l * l) / 48.0;
        }
        sag = min(sag, D2D_CURVE_JOINT_MAX_SAG);

        for (k = 0; k < 3; ++k)
        {
            o[k].position = src[k]->position;
            o[k].texcoord.u = 0.0f;
            o[k].texcoord.v = -((q[k][0] - q[0][0]) * nb[0] + (q[k][1] - q[0][1]) * nb[1]) / len - sag;
            o[k].texcoord.sign = v[3 * t].texcoord.sign;
        }
        written += 3;
    }
    free(map);
    return written;
}

/* Curve triangles cover the space between the curve and its control polygon
 * only. The antialiasing ramp reaches half a pixel past the curve, and next to a
 * short curve piece the triangle is thinner than that, which would cut off the
 * outer half of the ramp: small curves and scaled down artwork such as SVG logos
 * would get hard edges. Give every curve triangle a skirt on the side that is not
 * filled: across its two control edges when the control point lies outside the
 * filled area (the triangle fills towards the chord), across the chord when it
 * lies inside (the triangle fills towards the control point). Flat Bézier
 * triangles are left out altogether, the fill draws their chord.
 *
 * skip, if not NULL, marks the triangles whose area the fill mesh draws; see
 * d2d_fill_aa_thin().
 *
 * Returns a newly allocated vertex array, or NULL to draw the triangles as they are. */
static struct d2d_curve_vertex *d2d_device_context_curve_skirts(const struct d2d_device_context *context,
        const struct d2d_geometry *geometry, const struct d2d_curve_vertex *v, size_t count, size_t *out_count,
        BOOL beziers, const BYTE *skip)
{
    BOOL corners = beziers && d2d_curve_corners();
    BOOL joined = beziers && count >= 6 && d2d_curve_joints();
    struct d2d_curve_joint *joints = NULL;
    struct d2d_curve_vertex *out;
    float m[4], inv[4], det;
    size_t i, n = 0;
    double pm[6];

    d2d_device_context_get_pixel_matrix(context, &geometry->transform, m);
    if (corners || joined)
        d2d_device_context_get_pixel_transform(context, &geometry->transform, pm);
    det = m[0] * m[3] - m[1] * m[2];
    if (!(fabsf(det) > 1e-12f))
        return NULL;
    inv[0] = m[3] / det;
    inv[1] = -m[1] / det;
    inv[2] = -m[2] / det;
    inv[3] = m[0] / det;

    if (!(out = malloc((count / 3) * (18 + 3 * D2D_CURVE_CORNER_TRIANGLES) * sizeof(*out))))
        return NULL;
    /* Without the joints the wedges stay open. */
    if (joined && (joints = malloc((count / 3) * sizeof(*joints))))
    {
        for (i = 0; i < count / 3; ++i)
            joints[i].base = ~0u;
    }

    for (i = 0; i + 2 < count; i += 3)
    {
        if ((skip && skip[i / 3]) || (beziers && d2d_curve_is_flat(&v[i], m)))
            continue;
        memcpy(&out[n], &v[i], 3 * sizeof(*out));
        n += 3;
        /* Corner 1 is the control point. */
        if (v[i].texcoord.sign < 0.0f)
        {
            unsigned int s0, s1;
            float n0[2], n1[2];

            s0 = d2d_curve_add_skirt(&out[n], &v[i], 0, 1, 2, m, inv, n0);
            s1 = d2d_curve_add_skirt(&out[n + s0], &v[i], 1, 2, 0, m, inv, n1);
            if (joints && s0 && s1)
            {
                joints[i / 3].base = n - 3;
                memcpy(joints[i / 3].n0, n0, sizeof(n0));
                memcpy(joints[i / 3].n1, n1, sizeof(n1));
            }
            /* The outer vertices at corner 1: the fifth of the first skirt, the third of the second. */
            if (corners && s0 && s1)
                s1 += d2d_curve_add_skirt_corner(&out[n + s0 + s1], &v[i], &out[n + 4], &out[n + s0 + 2],
                        n0, n1, pm, inv);
            n += s0 + s1;
        }
        else
        {
            n += d2d_curve_add_skirt(&out[n], &v[i], 0, 2, 1, m, inv, NULL);
        }
    }

    if (joints)
    {
        n += d2d_curve_join_skirts(out, n, v, count, joints, m, pm);
        free(joints);
    }

    *out_count = n;
    return out;
}

static void d2d_device_context_draw_curves(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_curve_vertex *vertices, size_t count,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush, BOOL beziers, const BYTE *skip)
{
    struct d2d_curve_vertex *expanded = NULL;
    ID3D11Buffer *vb;
    HRESULT hr;

    if (render_target->drawing_state.antialiasMode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            && (expanded = d2d_device_context_curve_skirts(render_target, geometry, vertices, count, &count,
            beziers, skip)))
        vertices = expanded;
    /* Without the copy the triangles to leave out would be drawn as well. */
    else if (skip)
        count = 0;
    if (!count)
    {
        free(expanded);
        return;
    }

    if (FAILED(hr = d2d_device_context_get_scratch_buffer(render_target, D3D11_BIND_VERTEX_BUFFER,
            count * sizeof(*vertices), &render_target->scratch_vb[D2D_SHAPE_TYPE_CURVE], vertices, &vb)))
    {
        ERR("Failed to create curves vertex buffer, hr %#lx.\n", hr);
        free(expanded);
        return;
    }
    free(expanded);

    d2d_device_context_draw(render_target, D2D_SHAPE_TYPE_CURVE, NULL, count, vb,
            sizeof(*vertices), brush, opacity_brush);
}

static void d2d_device_context_fill_batch_reset(struct d2d_fill_batch *batch)
{
    batch->fill_vertex_count = 0;
    batch->curve_vertex_count = 0;
    batch->run_count = 0;
}

/* Binds the input layout, vertex shader and vertex buffer of a shape type on a
 * device context that d2d_device_context_draw_setup() has prepared; the rest
 * of the state is shared by the fill shapes. */
static void d2d_device_context_bind_shape(struct d2d_device_context *context,
        ID3D11DeviceContext1 *d3d_context, enum d2d_shape_type shape_type, ID3D11Buffer *vb, unsigned int stride)
{
    struct d2d_shape_resources *shape_resources = &context->shape_resources[shape_type];
    unsigned int offset = 0;

    ID3D11DeviceContext1_IASetInputLayout(d3d_context, shape_resources->il);
    ID3D11DeviceContext1_VSSetShader(d3d_context, shape_resources->vs, NULL, 0);
    ID3D11DeviceContext1_IASetVertexBuffers(d3d_context, 0, 1, &vb, &stride, &offset);
}

/* Draws the pending fill batch: one vertex upload for the fill meshes and one
 * for the curve triangles, one state setup, and per run a draw for its fill, its
 * Bézier and its arc triangles — the order fill_geometry() keeps for a single
 * geometry, and across runs the order the fills were issued in. Within a run
 * the fills of all its geometries precede their curves; that is a reordering
 * only where two geometries of the same colour overlap, and source-over
 * blending of one colour is commutative, so the result is the same. The
 * shaders take the world transform from the drawing state, so that is swapped
 * for the batch's own while it draws; everything else they read is unchanged
 * since the batch was opened, because the setters flush it. */
static void d2d_device_context_flush_fills(struct d2d_device_context *context)
{
    struct d2d_fill_batch *batch = &context->fill_batch;
    ID3D11Buffer *fill_vb = NULL, *curve_vb = NULL;
    enum d2d_shape_type bound = D2D_SHAPE_TYPE_COUNT;
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *d3d_context;
    D2D1_MATRIX_3X2_F saved_transform;
    struct d2d_brush brush;
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

    if (FAILED(hr = d2d_device_context_update_vs_cb(context, &batch->geometry_transform, 0.0f, 0.0f,
            D2D1_LINE_JOIN_MITER, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT)))
    {
        WARN("Failed to update vs constant buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (FAILED(hr = d2d_device_context_update_ps_cb(context, &brush, NULL, FALSE, FALSE)))
    {
        WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (batch->fill_vertex_count && FAILED(hr = d2d_device_context_get_scratch_buffer(context,
            D3D11_BIND_VERTEX_BUFFER, batch->fill_vertex_count * sizeof(*batch->fill_vertices),
            &context->scratch_vb[D2D_SHAPE_TYPE_FILL_AA], batch->fill_vertices, &fill_vb)))
    {
        ERR("Failed to create fill vertex buffer, hr %#lx.\n", hr);
        goto done;
    }
    if (batch->curve_vertex_count && FAILED(hr = d2d_device_context_get_scratch_buffer(context,
            D3D11_BIND_VERTEX_BUFFER, batch->curve_vertex_count * sizeof(*batch->curve_vertices),
            &context->scratch_vb[D2D_SHAPE_TYPE_CURVE], batch->curve_vertices, &curve_vb)))
    {
        ERR("Failed to create curve vertex buffer, hr %#lx.\n", hr);
        goto done;
    }

    d3d_context = d2d_device_context_draw_setup(context, D2D_SHAPE_TYPE_FILL_AA, NULL, fill_vb,
            sizeof(*batch->fill_vertices), &brush, NULL, &prev_state);
    bound = D2D_SHAPE_TYPE_FILL_AA;
    for (i = 0; i < batch->run_count; ++i)
    {
        const struct d2d_fill_run *run = &batch->runs[i];

        brush.opacity = run->opacity;
        brush.u.solid.color = run->colour;
        if (FAILED(hr = d2d_device_context_update_ps_cb(context, &brush, NULL, FALSE, FALSE)))
        {
            WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
            break;
        }
        if (run->fill_count)
        {
            if (bound != D2D_SHAPE_TYPE_FILL_AA)
                d2d_device_context_bind_shape(context, d3d_context, D2D_SHAPE_TYPE_FILL_AA,
                        fill_vb, sizeof(*batch->fill_vertices));
            bound = D2D_SHAPE_TYPE_FILL_AA;
            ID3D11DeviceContext1_Draw(d3d_context, run->fill_count, run->fill_start);
        }
        if (run->bezier_count)
        {
            if (bound != D2D_SHAPE_TYPE_CURVE)
                d2d_device_context_bind_shape(context, d3d_context, D2D_SHAPE_TYPE_CURVE,
                        curve_vb, sizeof(*batch->curve_vertices));
            bound = D2D_SHAPE_TYPE_CURVE;
            ID3D11DeviceContext1_Draw(d3d_context, run->bezier_count, run->bezier_start);
        }
        if (run->arc_count)
        {
            if (FAILED(hr = d2d_device_context_update_ps_cb(context, &brush, NULL, FALSE, TRUE)))
            {
                WARN("Failed to update ps constant buffer, hr %#lx.\n", hr);
                break;
            }
            if (bound != D2D_SHAPE_TYPE_CURVE)
                d2d_device_context_bind_shape(context, d3d_context, D2D_SHAPE_TYPE_CURVE,
                        curve_vb, sizeof(*batch->curve_vertices));
            bound = D2D_SHAPE_TYPE_CURVE;
            ID3D11DeviceContext1_Draw(d3d_context, run->arc_count, run->arc_start);
        }
    }
    d2d_device_context_draw_finish(context, d3d_context, prev_state);

done:
    context->drawing_state.transform = saved_transform;
    d2d_device_context_fill_batch_reset(batch);
    batch->flushing = FALSE;
}

/* Appends the curve triangles of a geometry to the batch, with the skirts
 * d2d_device_context_draw_curves() gives them. */
static BOOL d2d_device_context_fill_batch_add_curves(struct d2d_device_context *context,
        const struct d2d_geometry *geometry, const struct d2d_curve_vertex *vertices, size_t count,
        BOOL beziers, const BYTE *skip, unsigned int *start, unsigned int *added)
{
    struct d2d_fill_batch *batch = &context->fill_batch;
    struct d2d_curve_vertex *expanded;

    *start = batch->curve_vertex_count;
    *added = 0;
    if (!count)
        return TRUE;

    if ((expanded = d2d_device_context_curve_skirts(context, geometry, vertices, count, &count, beziers, skip)))
        vertices = expanded;
    /* Without the copy the triangles to leave out would be added as well. */
    else if (skip)
        count = 0;
    if (!count)
    {
        free(expanded);
        return TRUE;
    }
    if (!d2d_array_reserve((void **)&batch->curve_vertices, &batch->curve_vertices_size,
            batch->curve_vertex_count + count, sizeof(*batch->curve_vertices)))
    {
        free(expanded);
        return FALSE;
    }
    memcpy(&batch->curve_vertices[batch->curve_vertex_count], vertices, count * sizeof(*vertices));
    batch->curve_vertex_count += count;
    *added = count;
    free(expanded);
    return TRUE;
}

/* One upload per flush; past this much fill mesh the batch is drawn and a new
 * one opened. A rounded rectangle is about 4 KiB of fill mesh. */
#define D2D_FILL_BATCH_MAX_BYTES (1024 * 1024)

/* Appends a solid-colour fill to the batch: the antialiased fill mesh and the
 * curve triangles, exactly what fill_geometry() uploads for the geometry on its
 * own. Returns FALSE when the fill cannot be batched; the caller then flushes
 * the batch and draws it the ordinary way. Only source-over blending is
 * batched: it is the one mode in which fills of the same colour commute, which
 * the flush relies on where they overlap. */
static BOOL d2d_device_context_batch_fill(struct d2d_device_context *context,
        const struct d2d_geometry *geometry, const struct d2d_fill *fill,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    struct d2d_fill_batch *batch = &context->fill_batch;
    unsigned int bezier_start, bezier_count, arc_start, arc_count;
    struct d2d_fill_aa_vertex *expanded;
    size_t expanded_count, curve_mark;
    struct d2d_fill_run *run;
    BYTE *curve_skip;

    if (!brush || brush->type != D2D_BRUSH_TYPE_SOLID || opacity_brush
            || context->target.type != D2D_TARGET_BITMAP || batch->flushing
            || context->drawing_state.antialiasMode != D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            || context->drawing_state.primitiveBlend != D2D1_PRIMITIVE_BLEND_SOURCE_OVER
            || !fill->face_count)
        return FALSE;

    /* Lines recorded before this fill have to land underneath it. */
    d2d_device_context_flush_lines(context);

    if (batch->run_count && ((batch->fill_vertex_count + 3 * fill->face_count)
            * sizeof(*batch->fill_vertices) > D2D_FILL_BATCH_MAX_BYTES
            || memcmp(&batch->transform, &context->drawing_state.transform, sizeof(batch->transform))
            || memcmp(&batch->geometry_transform, &geometry->transform, sizeof(batch->geometry_transform))))
        d2d_device_context_flush_fills(context);

    if (!d2d_device_context_fill_aa_expand(context, geometry, fill, &expanded, &expanded_count, &curve_skip))
        return FALSE;

    if (!d2d_array_reserve((void **)&batch->fill_vertices, &batch->fill_vertices_size,
            batch->fill_vertex_count + expanded_count, sizeof(*batch->fill_vertices))
            || !d2d_array_reserve((void **)&batch->runs, &batch->runs_size,
            batch->run_count + 1, sizeof(*batch->runs)))
    {
        free(expanded);
        free(curve_skip);
        return FALSE;
    }

    curve_mark = batch->curve_vertex_count;
    if (!d2d_device_context_fill_batch_add_curves(context, geometry, fill->bezier_vertices,
            fill->bezier_vertex_count, TRUE, curve_skip, &bezier_start, &bezier_count)
            || !d2d_device_context_fill_batch_add_curves(context, geometry, fill->arc_vertices,
            fill->arc_vertex_count, FALSE, curve_skip ? curve_skip + fill->bezier_vertex_count / 3 : NULL,
            &arc_start, &arc_count))
    {
        batch->curve_vertex_count = curve_mark;
        free(expanded);
        free(curve_skip);
        return FALSE;
    }
    free(curve_skip);

    if (!batch->run_count)
    {
        batch->transform = context->drawing_state.transform;
        batch->geometry_transform = geometry->transform;
    }

    run = batch->run_count ? &batch->runs[batch->run_count - 1] : NULL;
    if (!run || run->opacity != brush->opacity
            || memcmp(&run->colour, &brush->u.solid.color, sizeof(run->colour)))
    {
        run = &batch->runs[batch->run_count++];
        run->colour = brush->u.solid.color;
        run->opacity = brush->opacity;
        run->fill_start = batch->fill_vertex_count;
        run->fill_count = 0;
        run->bezier_start = bezier_start;
        run->bezier_count = 0;
        run->arc_start = arc_start;
        run->arc_count = 0;
    }

    /* A run's curve ranges have to stay contiguous: its Béziers are the ones
     * appended before its arcs, geometry by geometry, so a geometry that adds
     * both kinds after another one of the same run would interleave them.
     * Such a run is split instead. */
    if (run->arc_count && bezier_count)
    {
        run = &batch->runs[batch->run_count++];
        run->colour = brush->u.solid.color;
        run->opacity = brush->opacity;
        run->fill_start = batch->fill_vertex_count;
        run->fill_count = 0;
        run->bezier_start = bezier_start;
        run->bezier_count = 0;
        run->arc_start = arc_start;
        run->arc_count = 0;
    }

    /* The ranges start with the first geometry that contributes to them. */
    if (!run->bezier_count)
        run->bezier_start = bezier_start;
    if (!run->arc_count)
        run->arc_start = arc_start;

    memcpy(&batch->fill_vertices[batch->fill_vertex_count], expanded, expanded_count * sizeof(*expanded));
    batch->fill_vertex_count += expanded_count;
    run->fill_count += expanded_count;
    run->bezier_count += bezier_count;
    run->arc_count += arc_count;
    free(expanded);
    return TRUE;
}

static void d2d_device_context_fill_geometry_data(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, const struct d2d_fill *fill,
        struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    BYTE *curve_skip = NULL;
    HRESULT hr;

    if (d2d_device_context_batch_fill(render_target, geometry, fill, brush, opacity_brush))
        return;

    /* A fill the batch declines is drawn below with the constant buffers set
     * here; the pending batch has to be drawn before that, not by the first
     * draw() below, which would overwrite them with the batch's own. */
    d2d_device_context_flush_fills(render_target);

    if (FAILED(hr = d2d_device_context_update_vs_cb(render_target, &geometry->transform, 0.0f, 0.0f, D2D1_LINE_JOIN_MITER, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT)))
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
     * antialiases the straight boundary edges, ramps towards the seams with the
     * curve segments by the distance to the curve, and declines a shape it can
     * do nothing for. */
    if (fill->face_count
            && render_target->drawing_state.antialiasMode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE)
    {
        if (!d2d_device_context_fill_triangles_aa(render_target, geometry, fill, brush, opacity_brush, &curve_skip))
            d2d_device_context_fill_triangles(render_target, fill, brush, opacity_brush);
    }
    else if (fill->face_count)
    {
        d2d_device_context_fill_triangles(render_target, fill, brush, opacity_brush);
    }

    if (fill->bezier_vertex_count)
        d2d_device_context_draw_curves(render_target, geometry, fill->bezier_vertices,
                fill->bezier_vertex_count, brush, opacity_brush, TRUE, curve_skip);

    if (fill->arc_vertex_count
            && SUCCEEDED(d2d_device_context_update_ps_cb(render_target, brush, opacity_brush, FALSE, TRUE)))
        d2d_device_context_draw_curves(render_target, geometry, fill->arc_vertices,
                fill->arc_vertex_count, brush, opacity_brush, FALSE,
                curve_skip ? curve_skip + fill->bezier_vertex_count / 3 : NULL);
    free(curve_skip);
}

static void d2d_device_context_fill_geometry(struct d2d_device_context *render_target,
        const struct d2d_geometry *geometry, struct d2d_brush *brush, struct d2d_brush *opacity_brush)
{
    const struct d2d_fill *fill;
    D2D1_RECT_F clip;

    d2d_device_context_get_fill_clip(render_target, geometry, &clip);
    fill = d2d_geometry_acquire_fill(geometry, &clip);
    d2d_device_context_fill_geometry_data(render_target, geometry, fill, brush, opacity_brush);
    d2d_geometry_release_fill(geometry);
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

    d2d_device_context_flush_batches(context);

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
        d2d_command_list_fill_mesh(context->target.command_list, context, mesh, brush);
}

/* Fill dst_rect with the brush, modulated per pixel by the alpha channel of the
 * mask bitmap, sampled from src_rect. The mask becomes the opacity brush of the
 * ordinary rectangle fill: the shader already multiplies the colour brush by the
 * opacity brush's alpha, so this is the same path FillGeometry() takes with an
 * opacity brush, only with the brush transform derived from the two rectangles
 * the way DrawBitmap() derives it. */
static void d2d_device_context_fill_opacity_mask(struct d2d_device_context *context,
        ID2D1Bitmap *mask, ID2D1Brush *brush, const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect)
{
    D2D1_IMAGE_BRUSH_PROPERTIES image_brush_desc;
    D2D1_BRUSH_PROPERTIES brush_desc;
    float dpi_x, dpi_y, px, py, sx, sy;
    struct d2d_geometry *geometry;
    struct d2d_brush *mask_brush;
    ID2D1Image *image;
    D2D1_SIZE_F size;
    D2D1_RECT_F s, d;
    HRESULT hr;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (!mask || !brush)
    {
        d2d_device_context_set_error(context, E_INVALIDARG);
        return;
    }

    size = ID2D1Bitmap_GetSize(mask);
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

    if (d.left == d.right || d.top == d.bottom)
        return;

    if (FAILED(hr = ID2D1Bitmap_QueryInterface(mask, &IID_ID2D1Image, (void **)&image)))
    {
        d2d_device_context_set_error(context, E_INVALIDARG);
        return;
    }

    /* An image brush rather than a bitmap brush, because only the image brush
     * confines sampling to a source rectangle. The masks are typically atlas
     * cells (one glyph or label per cell), and the rectangle is inset by half
     * a texel so that linear filtering at the cell edges never reaches the
     * neighbouring cell. The brush's source rectangle is in mask pixels. */
    ID2D1Bitmap_GetDpi(mask, &dpi_x, &dpi_y);
    px = dpi_x > 0.0f ? dpi_x / 96.0f : 1.0f;
    py = dpi_y > 0.0f ? dpi_y / 96.0f : 1.0f;
    image_brush_desc.sourceRectangle.left = s.left * px + 0.5f;
    image_brush_desc.sourceRectangle.top = s.top * py + 0.5f;
    image_brush_desc.sourceRectangle.right = max(s.right * px - 0.5f, image_brush_desc.sourceRectangle.left);
    image_brush_desc.sourceRectangle.bottom = max(s.bottom * py - 0.5f, image_brush_desc.sourceRectangle.top);
    image_brush_desc.extendModeX = D2D1_EXTEND_MODE_CLAMP;
    image_brush_desc.extendModeY = D2D1_EXTEND_MODE_CLAMP;
    image_brush_desc.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;

    /* The brush origin is the source rectangle's top-left corner, half a mask
     * pixel inside the requested source rectangle; place it accordingly. */
    sx = fabsf((d.right - d.left) / (s.right - s.left));
    sy = fabsf((d.bottom - d.top) / (s.bottom - s.top));
    brush_desc.opacity = 1.0f;
    brush_desc.transform._11 = sx;
    brush_desc.transform._21 = 0.0f;
    brush_desc.transform._31 = min(d.left, d.right) + 0.5f / px * sx;
    brush_desc.transform._12 = 0.0f;
    brush_desc.transform._22 = sy;
    brush_desc.transform._32 = min(d.top, d.bottom) + 0.5f / py * sy;

    hr = d2d_image_brush_create(context->factory, image, &image_brush_desc, &brush_desc, &mask_brush);
    ID2D1Image_Release(image);
    if (FAILED(hr))
    {
        ERR("Failed to create mask brush, hr %#lx.\n", hr);
        return;
    }

    if ((geometry = d2d_device_context_get_rect_geometry(context, &d)))
    {
        d2d_device_context_fill_geometry(context, geometry, unsafe_impl_from_ID2D1Brush(brush), mask_brush);
        d2d_device_context_put_rect_geometry(context, geometry);
    }

    ID2D1Brush_Release(&mask_brush->ID2D1Brush_iface);
}

static void STDMETHODCALLTYPE d2d_device_context_FillOpacityMask(ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *mask, ID2D1Brush *brush, D2D1_OPACITY_MASK_CONTENT content,
        const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);

    TRACE("iface %p, mask %p, brush %p, content %#x, dst_rect %s, src_rect %s.\n",
            iface, mask, brush, content, debug_d2d_rect_f(dst_rect), debug_d2d_rect_f(src_rect));

    d2d_device_context_flush_batches(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

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
    {
        d2d_command_list_fill_opacity_mask(context->target.command_list, context, mask, brush, dst_rect, src_rect);
        return;
    }

    d2d_device_context_fill_opacity_mask(context, mask, brush, dst_rect, src_rect);
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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (render_target->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(render_target, D2DERR_WRONG_STATE);
        return;
    }

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

    d2d_device_context_flush_batches(render_target);

    if (FAILED(render_target->error.code))
        return;

    if (render_target->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(render_target, D2DERR_WRONG_STATE);
        return;
    }

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

/* DirectWrite's alpha correction ratios for gamma 1.0 to 2.2 in steps of 0.1,
 * in the form Windows Terminal reproduces DirectWrite's own greyscale blend
 * (src/renderer/atlas/dwrite_helpers.cpp and .hlsl, MIT licence). */
static const float d2d_text_gamma_ratios[13][4] =
{
    {0.0000f,  0.0000f, 0.0000f,  0.0000f}, /* 1.0 */
    {0.0166f, -0.0807f, 0.2227f, -0.0751f},
    {0.0350f, -0.1760f, 0.4325f, -0.1370f},
    {0.0543f, -0.2821f, 0.6302f, -0.1876f},
    {0.0739f, -0.3963f, 0.8167f, -0.2287f},
    {0.0933f, -0.5161f, 0.9926f, -0.2616f}, /* 1.5 */
    {0.1121f, -0.6395f, 1.1588f, -0.2877f},
    {0.1300f, -0.7649f, 1.3159f, -0.3080f},
    {0.1469f, -0.8911f, 1.4644f, -0.3234f}, /* 1.8, Windows' default */
    {0.1627f, -1.0170f, 1.6051f, -0.3347f},
    {0.1773f, -1.1420f, 1.7385f, -0.3426f}, /* 2.0 */
    {0.1908f, -1.2652f, 1.8650f, -0.3476f},
    {0.2031f, -1.3864f, 1.9851f, -0.3501f}, /* 2.2 */
};

/* DirectWrite's enhanced contrast depends on the text colour: the full value
 * for dark text, scaled down to nothing as the colour gets light. */
static float d2d_text_contrast_for_colour(float contrast, const D2D1_COLOR_F *colour)
{
    float r, g, b, lightness;

    r = min(max(colour->r, 0.0f), 1.0f);
    g = min(max(colour->g, 0.0f), 1.0f);
    b = min(max(colour->b, 0.0f), 1.0f);
    lightness = 0.30f * r + 0.59f * g + 0.11f * b;

    return contrast * min(max(4.0f * (0.75f - lightness), 0.0f), 1.0f);
}

/* Coverage mapping for one text channel. DirectWrite applies the enhanced
 * contrast k, and then corrects the result for the gamma of a blend in encoded
 * space with the intensity of the text colour in that channel: light text
 * gains weight, dark text gives back most of what the contrast added. */
static void d2d_text_blend_ramp(float gamma, float k, float intensity, BYTE *ramp)
{
    unsigned int i, index;
    float ratios[4];

    if (!(k >= 0.0f))
        k = 0.0f;
    if (!(gamma >= 1.0f))
        gamma = 1.0f;
    else if (gamma > 2.2f)
        gamma = 2.2f;
    index = (unsigned int)(gamma * 10.0f + 0.5f) - 10;
    ratios[0] = d2d_text_gamma_ratios[index][0] * 65536.0f / (255.0f * 255.0f);
    ratios[1] = d2d_text_gamma_ratios[index][1] * 256.0f / 255.0f;
    ratios[2] = d2d_text_gamma_ratios[index][2] * 65536.0f / (255.0f * 255.0f);
    ratios[3] = d2d_text_gamma_ratios[index][3] * 256.0f / 255.0f;

    for (i = 0; i < 256; ++i)
    {
        float a = i / 255.0f;

        a = a * (k + 1.0f) / (a * k + 1.0f);
        a += a * (1.0f - a) * ((ratios[0] * intensity + ratios[1]) * a + (ratios[2] * intensity + ratios[3]));
        ramp[i] = min(max((int)(a * 255.0f + 0.5f), 0), 255);
    }
}

/* Coverage mapping for one greyscale run in a solid colour, from the
 * rendering parameters' gamma and greyscale enhanced contrast and the
 * luminance of the text colour. Measured against Windows 10 with the same
 * Segoe UI runs and its parameters (gamma 1.8, greyscale contrast 1.0), the
 * ink this adds agrees with Direct2D's to about one percentage point at 12 px
 * in both polarities; bold and larger text stay a few points above, which
 * comes from FreeType's coverage rather than from the blend. */
static void d2d_text_grayscale_ramp(IDWriteRenderingParams *params, const D2D1_COLOR_F *colour, BYTE *ramp)
{
    float r, g, b, contrast = 1.0f, intensity;
    IDWriteRenderingParams1 *params1;

    if (SUCCEEDED(IDWriteRenderingParams_QueryInterface(params, &IID_IDWriteRenderingParams1, (void **)&params1)))
    {
        contrast = IDWriteRenderingParams1_GetGrayscaleEnhancedContrast(params1);
        IDWriteRenderingParams1_Release(params1);
    }
    if (!(contrast >= 0.0f))
        contrast = 0.0f;

    r = min(max(colour->r, 0.0f), 1.0f);
    g = min(max(colour->g, 0.0f), 1.0f);
    b = min(max(colour->b, 0.0f), 1.0f);
    intensity = 0.25f * r + 0.50f * g + 0.25f * b;

    d2d_text_blend_ramp(IDWriteRenderingParams_GetGamma(params),
            d2d_text_contrast_for_colour(contrast, colour), intensity, ramp);
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
        const RECT *bounds, DWRITE_PIXEL_GEOMETRY pixel_geometry, float gamma, float cleartype_level,
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
    BOOL linear, temporary;
    BOOL dwrite_blend = FALSE;
    float scale_x, scale_y;
    D2D1_RECT_F run_rect;
    HRESULT hr = S_OK;
    BYTE ramps[3][256];
    BYTE *plane;

    if (!width || !height)
        return S_OK;

    if (!(plane = d2d_glyph_mask_get_plane(mask, width, height, &pitch)))
        return E_OUTOFMEMORY;

    scale_x = context->desc.dpiX / 96.0f;
    scale_y = context->desc.dpiY / 96.0f;
    d2d_rect_set(&run_rect, bounds->left / scale_x, bounds->top / scale_y,
            bounds->right / scale_x, bounds->bottom / scale_y);

    /* DirectWrite's own blend: the contrast scaled for the text colour, then
     * the gamma alpha correction per channel with that channel of the colour
     * as its intensity, and the result blended in encoded space — the
     * correction already accounts for that, so a linear blend on top would
     * correct twice. The mapping needs the text colour; a gradient or bitmap
     * brush has no single one and keeps the path below. */
    if (d2d_settings.text_cleartype_blend && brush_impl->type == D2D_BRUSH_TYPE_SOLID)
    {
        const D2D1_COLOR_F *colour = &brush_impl->u.solid.color;
        float k = d2d_text_contrast_for_colour(enhanced_contrast, colour);

        d2d_text_blend_ramp(gamma, k, min(max(colour->r, 0.0f), 1.0f), ramps[0]);
        d2d_text_blend_ramp(gamma, k, min(max(colour->g, 0.0f), 1.0f), ramps[1]);
        d2d_text_blend_ramp(gamma, k, min(max(colour->b, 0.0f), 1.0f), ramps[2]);
        dwrite_blend = TRUE;
    }

    /* Linear blending needs the destination, which the output merger cannot
     * supply through a transfer function. Take a copy of it and let the shader
     * finish the blend; if that is not available, stay on the ordinary path. */
    linear = !dwrite_blend && d2d_settings.text_linear_blend
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

                if (dwrite_blend)
                {
                    plane[y * pitch + x] = ramps[c][min(max((int)(value + 0.5f), 0), 255)];
                    continue;
                }

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
    struct d2d_brush *mask_brush, *brush_impl;
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
                width, height, &bounds, pixel_geometry, gamma, cleartype_level, contrast);
        if (FAILED(hr))
            d2d_device_context_set_error(context, hr);
        goto done;
    }

    if (!(plane = d2d_glyph_mask_get_plane(mask, width, height, &pitch)))
    {
        ERR("Failed to allocate glyph mask plane.\n");
        goto done;
    }

    /* The mapping needs the text colour; a gradient or bitmap brush has no
     * single one and keeps the plain coverage. */
    brush_impl = unsafe_impl_from_ID2D1Brush(brush);
    if (d2d_settings.text_grayscale_blend && brush_impl->type == D2D_BRUSH_TYPE_SOLID)
    {
        unsigned int x;
        BYTE ramp[256];

        d2d_text_grayscale_ramp(rendering_params, &brush_impl->u.solid.color, ramp);

        for (y = 0; y < height; ++y)
        {
            const BYTE *src = &mask->coverage[(size_t)y * width];
            BYTE *dst = &plane[y * pitch];

            for (x = 0; x < width; ++x)
                dst[x] = ramp[src[x]];
        }
    }
    else
    {
        for (y = 0; y < height; ++y)
            memcpy(&plane[y * pitch], &mask->coverage[(size_t)y * width], width);
    }

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

    d2d_device_context_flush_batches(context);

    if (FAILED(context->error.code))
        return;

    if (context->target.type == D2D_TARGET_UNKNOWN)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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
                        && d2d_geometry_fill_is_simple(mask_geo))
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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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
        struct d2d_vs_cb new_vs = {{0}};

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
    else
        d2d_device_context_hold_state(context);

    memset(&context->error, 0, sizeof(context->error));
}

static HRESULT STDMETHODCALLTYPE d2d_device_context_EndDraw(ID2D1DeviceContext6 *iface,
        D2D1_TAG *tag1, D2D1_TAG *tag2)
{
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface);
    HRESULT hr;

    TRACE("iface %p, tag1 %p, tag2 %p.\n", iface, tag1, tag2);

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_release_state(context);

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

    d2d_device_context_flush_batches(render_target);

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

/* "Indicates whether the format is supported by the device context": the
 * formats bitmaps and targets can be created in.  Cubase 15 asks for the whole
 * DXGI range once at start-up and for B8G8R8A8_UNORM a few times more; the
 * stub this replaces said no to all of them. */
static BOOL STDMETHODCALLTYPE d2d_device_context_IsDxgiFormatSupported(ID2D1DeviceContext6 *iface, DXGI_FORMAT format)
{
    BOOL supported = d2d_dxgi_format_supported(format);

    TRACE("iface %p, format %#x, supported %d.\n", iface, format, supported);

    return supported;
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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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


struct d2d_effect_vs_cb
{
    struct d2d_vec4 corners[4];
    struct d2d_vec4 uv_rects[4];
};

static HRESULT d2d_device_context_update_dynamic_buffer(struct d2d_device_context *context,
        ID3D11Buffer **buffer, unsigned int size, const void *data)
{
    D3D11_MAPPED_SUBRESOURCE map_desc;
    ID3D11DeviceContext *d3d_context;
    D3D11_BUFFER_DESC buffer_desc;
    HRESULT hr;

    size = (size + 15) & ~15u;

    if (*buffer)
    {
        ID3D11Buffer_GetDesc(*buffer, &buffer_desc);
        if (buffer_desc.ByteWidth < size)
        {
            ID3D11Buffer_Release(*buffer);
            *buffer = NULL;
        }
    }

    if (!*buffer)
    {
        buffer_desc.ByteWidth = size;
        buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        buffer_desc.MiscFlags = 0;
        buffer_desc.StructureByteStride = 0;
        if (FAILED(hr = ID3D11Device1_CreateBuffer(context->d3d_device, &buffer_desc, NULL, buffer)))
        {
            WARN("Failed to create constant buffer, hr %#lx.\n", hr);
            return hr;
        }
        ID3D11Buffer_GetDesc(*buffer, &buffer_desc);
    }

    ID3D11Device1_GetImmediateContext(context->d3d_device, &d3d_context);
    if (FAILED(hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)*buffer,
            0, D3D11_MAP_WRITE_DISCARD, 0, &map_desc)))
    {
        WARN("Failed to map constant buffer, hr %#lx.\n", hr);
        ID3D11DeviceContext_Release(d3d_context);
        return hr;
    }
    memset(map_desc.pData, 0, buffer_desc.ByteWidth);
    memcpy(map_desc.pData, data, size);
    ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)*buffer, 0);
    ID3D11DeviceContext_Release(d3d_context);

    return S_OK;
}

/* Returns the effect input that feeds input "node_input" of "node": the one
 * the graph connects to it, or the effect input with the same index for a
 * node input without such a connection. */
static unsigned int d2d_effect_node_input_source(const struct d2d_effect *effect,
        const struct d2d_transform_node *node, unsigned int node_input)
{
    const struct d2d_transform_graph *graph = effect->graph;
    unsigned int i;

    for (i = 0; i < graph->input_count; ++i)
    {
        if (graph->inputs[i].node == node && graph->inputs[i].index == node_input)
            return i;
    }
    return node_input;
}

/* Draw a custom effect whose transform graph is a single draw transform with
 * a pixel shader, the shape every effect written against
 * ID2D1EffectImpl/ID2D1DrawTransform with d2d1effecthelpers takes. The
 * inputs have to be bitmaps; nested effects are not rendered. */
static HRESULT d2d_device_context_draw_custom_effect(struct d2d_device_context *context,
        struct d2d_effect *effect, const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode, D2D1_COMPOSITE_MODE composite_mode)
{
    ID3D11SamplerState *samplers[4] = {NULL}, **sampler_state;
    ID3D11ShaderResourceView *srvs[4] = {NULL}, *null_srvs[4] = {NULL};
    struct d2d_bitmap *bitmaps[4] = {NULL};
    ID2D1DrawTransform *transform = NULL;
    struct d2d_render_info *render_info;
    struct d2d_transform_node *node;
    ID3DDeviceContextState *prev_state;
    ID3D11DeviceContext1 *d3d_context;
    struct d2d_effect_vs_cb vs_cb;
    ID3D11PixelShader *ps = NULL;
    unsigned int i, input_count;
    D2D1_RECT_L out_rect, in_rects[4];
    D3D11_RECT scissor_rect;
    D2D1_RECT_F rect;
    D3D11_VIEWPORT vp;
    IUnknown *unk;
    HRESULT hr;

    if (!effect->graph || !(node = effect->graph->output))
        return E_NOTIMPL;

    if (effect->graph->passthrough)
    {
        ID2D1Bitmap *bitmap;

        if (effect->graph->passthrough_input >= effect->input_count
                || !effect->inputs[effect->graph->passthrough_input]
                || FAILED(ID2D1Image_QueryInterface(effect->inputs[effect->graph->passthrough_input],
                &IID_ID2D1Bitmap, (void **)&bitmap)))
            return E_NOTIMPL;
        d2d_device_context_draw_effect_bitmap(context, bitmap, 1.0f, interpolation_mode,
                image_rect, target_offset, composite_mode);
        ID2D1Bitmap_Release(bitmap);
        return S_OK;
    }

    if (!(render_info = node->render_info) || !(render_info->mask & D2D_RENDER_INFO_PIXEL_SHADER))
    {
        FIXME("Effect %p has no pixel shader draw transform.\n", effect);
        return E_NOTIMPL;
    }

    if (!d2d_device_get_indexed_object(&context->device->shaders, &render_info->pixel_shader, &unk))
    {
        WARN("Pixel shader %s was not loaded.\n", debugstr_guid(&render_info->pixel_shader));
        return E_NOTIMPL;
    }
    hr = IUnknown_QueryInterface(unk, &IID_ID3D11PixelShader, (void **)&ps);
    IUnknown_Release(unk);
    if (FAILED(hr))
        return hr;

    /* The pixel shader samples the inputs of the output node, which may be
     * fewer than the effect has (SetSingleTransformNode() with a one-input
     * node on a two-input effect, Nuendo's ProRes video conversion). */
    input_count = min(node->input_count, ARRAY_SIZE(bitmaps));
    if (node->input_count > ARRAY_SIZE(bitmaps))
        FIXME("Node %p has %u inputs, using the first %u.\n", node, node->input_count, input_count);

    for (i = 0; i < input_count; ++i)
    {
        unsigned int src = d2d_effect_node_input_source(effect, node, i);
        ID2D1Bitmap *bitmap;

        if (src >= effect->input_count || !effect->inputs[src])
            continue;
        if (FAILED(ID2D1Image_QueryInterface(effect->inputs[src], &IID_ID2D1Bitmap, (void **)&bitmap)))
        {
            FIXME("Effect input %u is not a bitmap.\n", i);
            hr = E_NOTIMPL;
            goto done;
        }
        bitmaps[i] = unsafe_impl_from_ID2D1Bitmap(bitmap);
        ID2D1Bitmap_Release(bitmap);
        if (!bitmaps[i]->srv)
        {
            FIXME("Effect input %u has no shader resource view.\n", i);
            hr = E_NOTIMPL;
            goto done;
        }
        srvs[i] = bitmaps[i]->srv;
    }

    /* Direct2D lets the effect refresh its constants before every render. */
    if (effect->impl && FAILED(hr = ID2D1EffectImpl_PrepareForRender(effect->impl, D2D1_CHANGE_TYPE_PROPERTIES)))
    {
        WARN("PrepareForRender() failed, hr %#lx.\n", hr);
        goto done;
    }

    if (image_rect)
    {
        rect = *image_rect;
    }
    else if (bitmaps[0])
    {
        d2d_rect_set(&rect, 0.0f, 0.0f, bitmaps[0]->pixel_size.width, bitmaps[0]->pixel_size.height);
    }
    else
    {
        hr = E_INVALIDARG;
        goto done;
    }
    out_rect.left = rect.left;
    out_rect.top = rect.top;
    out_rect.right = rect.right;
    out_rect.bottom = rect.bottom;

    /* The transform says which part of each input the output rectangle
     * needs; that region maps onto the quad. */
    for (i = 0; i < input_count; ++i)
        in_rects[i] = out_rect;
    if (SUCCEEDED(ID2D1TransformNode_QueryInterface(node->object, &IID_ID2D1DrawTransform, (void **)&transform)))
    {
        if (FAILED(hr = ID2D1DrawTransform_MapOutputRectToInputRects(transform, &out_rect, in_rects, input_count)))
        {
            WARN("MapOutputRectToInputRects() failed, hr %#lx.\n", hr);
            for (i = 0; i < input_count; ++i)
                in_rects[i] = out_rect;
        }
        ID2D1DrawTransform_Release(transform);
    }

    for (i = 0; i < 4; ++i)
    {
        D2D1_POINT_2F p;
        float x, y;

        x = (i & 1) ? rect.right : rect.left;
        y = (i & 2) ? rect.bottom : rect.top;
        vs_cb.corners[i].z = x;
        vs_cb.corners[i].w = y;
        if (target_offset)
        {
            x += target_offset->x;
            y += target_offset->y;
        }
        d2d_point_transform(&p, &context->drawing_state.transform, x, y);
        p.x *= context->desc.dpiX / 96.0f;
        p.y *= context->desc.dpiY / 96.0f;
        vs_cb.corners[i].x = p.x * 2.0f / context->pixel_size.width - 1.0f;
        vs_cb.corners[i].y = 1.0f - p.y * 2.0f / context->pixel_size.height;
    }
    for (i = 0; i < ARRAY_SIZE(vs_cb.uv_rects); ++i)
    {
        if (i < input_count && bitmaps[i] && bitmaps[i]->pixel_size.width && bitmaps[i]->pixel_size.height)
        {
            vs_cb.uv_rects[i].x = (float)in_rects[i].left / bitmaps[i]->pixel_size.width;
            vs_cb.uv_rects[i].y = (float)in_rects[i].top / bitmaps[i]->pixel_size.height;
            vs_cb.uv_rects[i].z = (float)in_rects[i].right / bitmaps[i]->pixel_size.width;
            vs_cb.uv_rects[i].w = (float)in_rects[i].bottom / bitmaps[i]->pixel_size.height;
        }
        else
        {
            vs_cb.uv_rects[i].x = vs_cb.uv_rects[i].y = 0.0f;
            vs_cb.uv_rects[i].z = vs_cb.uv_rects[i].w = 1.0f;
        }
    }

    if (!context->effect_vs)
    {
        ID3D10Blob *blob = context->device->precompiled_effect_vs;

        if (!blob || FAILED(hr = ID3D11Device1_CreateVertexShader(context->d3d_device,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &context->effect_vs)))
        {
            WARN("Failed to create the effect vertex shader.\n");
            hr = E_FAIL;
            goto done;
        }
    }
    if (FAILED(hr = d2d_device_context_update_dynamic_buffer(context, &context->effect_vs_cb, sizeof(vs_cb), &vs_cb)))
        goto done;
    if (render_info->ps_cb_size && (render_info->ps_cb_dirty || !render_info->ps_cb))
    {
        if (FAILED(hr = d2d_device_context_update_dynamic_buffer(context, &render_info->ps_cb,
                render_info->ps_cb_size, render_info->ps_cb_data)))
            goto done;
        render_info->ps_cb_dirty = FALSE;
    }

    for (i = 0; i < input_count; ++i)
    {
        sampler_state = &context->sampler_states
                [interpolation_mode % D2D_SAMPLER_INTERPOLATION_MODE_COUNT]
                [D2D1_EXTEND_MODE_CLAMP % D2D_SAMPLER_EXTEND_MODE_COUNT]
                [D2D1_EXTEND_MODE_CLAMP % D2D_SAMPLER_EXTEND_MODE_COUNT];
        if (!*sampler_state)
        {
            D3D11_SAMPLER_DESC sampler_desc;

            sampler_desc.Filter = interpolation_mode == D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                    ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler_desc.MipLODBias = 0.0f;
            sampler_desc.MaxAnisotropy = 0;
            sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampler_desc.BorderColor[0] = 0.0f;
            sampler_desc.BorderColor[1] = 0.0f;
            sampler_desc.BorderColor[2] = 0.0f;
            sampler_desc.BorderColor[3] = 0.0f;
            sampler_desc.MinLOD = 0.0f;
            sampler_desc.MaxLOD = 0.0f;
            if (FAILED(hr = ID3D11Device1_CreateSamplerState(context->d3d_device, &sampler_desc, sampler_state)))
            {
                WARN("Failed to create sampler state, hr %#lx.\n", hr);
                goto done;
            }
        }
        samplers[i] = *sampler_state;
    }

    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = context->pixel_size.width;
    vp.Height = context->pixel_size.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (context->clip_stack.count)
    {
        const D2D1_RECT_F *clip_rect = &context->clip_stack.stack[context->clip_stack.count - 1];
        float l = ceilf(clip_rect->left - 0.5f), t = ceilf(clip_rect->top - 0.5f);
        float r = ceilf(clip_rect->right - 0.5f), b = ceilf(clip_rect->bottom - 0.5f);

        scissor_rect.left = l < 0.0f ? 0 : l > 2.0e9f ? 2000000000 : (LONG)l;
        scissor_rect.top = t < 0.0f ? 0 : t > 2.0e9f ? 2000000000 : (LONG)t;
        scissor_rect.right = r < 0.0f ? 0 : r > 2.0e9f ? 2000000000 : (LONG)r;
        scissor_rect.bottom = b < 0.0f ? 0 : b > 2.0e9f ? 2000000000 : (LONG)b;
    }
    else
    {
        scissor_rect.left = 0;
        scissor_rect.top = 0;
        scissor_rect.right = context->pixel_size.width;
        scissor_rect.bottom = context->pixel_size.height;
    }
    if (scissor_rect.right > (LONG)context->pixel_size.width)
        scissor_rect.right = context->pixel_size.width;
    if (scissor_rect.bottom > (LONG)context->pixel_size.height)
        scissor_rect.bottom = context->pixel_size.height;

    if (context->cs)
        EnterCriticalSection(context->cs);
    ID3D11Device1_GetImmediateContext1(context->d3d_device, &d3d_context);
    if (d2d_device_context_state_is_current(context))
        prev_state = NULL;
    else
        ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, context->d3d_state, &prev_state);

    ID3D11DeviceContext1_IASetInputLayout(d3d_context, NULL);
    ID3D11DeviceContext1_IASetPrimitiveTopology(d3d_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext1_VSSetConstantBuffers(d3d_context, 0, 1, &context->effect_vs_cb);
    ID3D11DeviceContext1_VSSetShader(d3d_context, context->effect_vs, NULL, 0);
    ID3D11DeviceContext1_PSSetConstantBuffers(d3d_context, 0, 1, &render_info->ps_cb);
    ID3D11DeviceContext1_PSSetShader(d3d_context, ps, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(d3d_context, 0, input_count, srvs);
    ID3D11DeviceContext1_PSSetSamplers(d3d_context, 0, input_count, samplers);
    ID3D11DeviceContext1_RSSetViewports(d3d_context, 1, &vp);
    ID3D11DeviceContext1_RSSetScissorRects(d3d_context, 1, &scissor_rect);
    ID3D11DeviceContext1_RSSetState(d3d_context, context->rs);
    if (context->stencil_depth > 0)
    {
        ID3D11DeviceContext1_OMSetRenderTargets(d3d_context, 1, &context->target.bitmap->rtv, context->stencil_dsv);
        ID3D11DeviceContext1_OMSetDepthStencilState(d3d_context, context->stencil_test_state, context->stencil_depth);
    }
    else
    {
        ID3D11DeviceContext1_OMSetRenderTargets(d3d_context, 1, &context->target.bitmap->rtv, NULL);
    }
    ID3D11DeviceContext1_OMSetBlendState(d3d_context,
            composite_mode == D2D1_COMPOSITE_MODE_SOURCE_COPY ? NULL : context->bs, NULL, D3D11_DEFAULT_SAMPLE_MASK);

    ID3D11DeviceContext1_Draw(d3d_context, 4, 0);

    ID3D11DeviceContext1_PSSetShaderResources(d3d_context, 0, input_count, null_srvs);
    if (prev_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(d3d_context, prev_state, NULL);
        ID3DDeviceContextState_Release(prev_state);
    }
    ID3D11DeviceContext1_Release(d3d_context);
    if (context->cs)
        LeaveCriticalSection(context->cs);

    hr = S_OK;

done:
    if (ps)
        ID3D11PixelShader_Release(ps);
    return hr;
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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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
                    struct d2d_effect *effect_impl = unsafe_impl_from_ID2D1Effect(effect);
                    HRESULT hr = E_NOTIMPL;

                    if (effect_impl && effect_impl->impl)
                        hr = d2d_device_context_draw_custom_effect(context, effect_impl,
                                target_offset, image_rect, interpolation_mode, composite_mode);
                    if (FAILED(hr))
                        FIXME("Unhandled effect %s, hr %#lx.\n", debugstr_guid(&clsid), hr);
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

    d2d_device_context_flush_batches(context);

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

    d2d_device_context_flush_batches(context);

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
                        && d2d_geometry_fill_is_simple(mask_geo))
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

    TRACE("iface %p, mask %p, brush %p, dst_rect %s, src_rect %s.\n",
            iface, mask, brush, debug_d2d_rect_f(dst_rect), debug_d2d_rect_f(src_rect));

    d2d_device_context_flush_batches(context);

    if (FAILED(context->error.code))
        return;

    if (context->drawing_state.antialiasMode != D2D1_ANTIALIAS_MODE_ALIASED)
    {
        d2d_device_context_set_error(context, D2DERR_WRONG_STATE);
        return;
    }

    if (context->target.type == D2D_TARGET_COMMAND_LIST)
    {
        d2d_command_list_fill_opacity_mask(context->target.command_list, context, mask, brush, dst_rect, src_rect);
        return;
    }

    d2d_device_context_fill_opacity_mask(context, mask, brush, dst_rect, src_rect);
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
    struct d2d_geometry_realization *r = unsafe_impl_from_ID2D1GeometryRealization(realization);

    TRACE("iface %p, realization %p, brush %p.\n", iface, realization, brush);

    /* The realization keeps the geometry it was created from rather than a
     * flattened copy, so it is drawn like that geometry on every target; the
     * flattening tolerance given at creation is not applied. */
    if (r->filled)
        d2d_device_context_FillGeometry(iface, r->geometry, brush, NULL);
    else
        d2d_device_context_DrawGeometry(iface, r->geometry, brush, r->stroke_width, r->stroke_style);
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

/* A multi-threaded factory serialises every call on the objects it creates
 * (D2D1_FACTORY_TYPE_MULTI_THREADED); applications rely on it, e.g. vatSys draws
 * the runways of a map with Parallel.ForEach onto one bitmap render target.
 * Taking the factory lock only around the D3D11 submission leaves the constant
 * buffer maps, the scratch buffers and the line and fill batches open to a
 * second thread. Contexts of such a factory therefore get this table, whose
 * entries hold the lock for the whole call; the lock is recursive, so a method
 * calling another one through the interface is fine. */
#define D2D_DEVICE_CONTEXT_MT_VOID(name, params, args) \
static void STDMETHODCALLTYPE d2d_device_context_mt_##name params \
{ \
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface); \
\
    EnterCriticalSection(context->cs); \
    d2d_device_context_##name args; \
    LeaveCriticalSection(context->cs); \
}

#define D2D_DEVICE_CONTEXT_MT(type, name, params, args) \
static type STDMETHODCALLTYPE d2d_device_context_mt_##name params \
{ \
    struct d2d_device_context *context = impl_from_ID2D1DeviceContext(iface); \
    type ret; \
\
    EnterCriticalSection(context->cs); \
    ret = d2d_device_context_##name args; \
    LeaveCriticalSection(context->cs); \
    return ret; \
}

D2D_DEVICE_CONTEXT_MT_VOID(GetFactory, (ID2D1DeviceContext6 *iface, ID2D1Factory **factory), (iface, factory))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateBitmap, (ID2D1DeviceContext6 *iface, D2D1_SIZE_U size,
        const void *src_data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap), (iface,
        size, src_data, pitch, desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateBitmapFromWicBitmap, (ID2D1DeviceContext6 *iface,
        IWICBitmapSource *bitmap_source, const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap), (iface,
        bitmap_source, desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateSharedBitmap, (ID2D1DeviceContext6 *iface, REFIID iid, void *data,
        const D2D1_BITMAP_PROPERTIES *desc, ID2D1Bitmap **bitmap), (iface, iid, data, desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateBitmapBrush, (ID2D1DeviceContext6 *iface, ID2D1Bitmap *bitmap,
        const D2D1_BITMAP_BRUSH_PROPERTIES *bitmap_brush_desc, const D2D1_BRUSH_PROPERTIES *brush_desc,
        ID2D1BitmapBrush **brush), (iface, bitmap, bitmap_brush_desc, brush_desc, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateSolidColorBrush, (ID2D1DeviceContext6 *iface, const D2D1_COLOR_F *color,
        const D2D1_BRUSH_PROPERTIES *desc, ID2D1SolidColorBrush **brush), (iface, color, desc, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateGradientStopCollection, (ID2D1DeviceContext6 *iface,
        const D2D1_GRADIENT_STOP *stops, UINT32 stop_count, D2D1_GAMMA gamma, D2D1_EXTEND_MODE extend_mode,
        ID2D1GradientStopCollection **gradient), (iface, stops, stop_count, gamma, extend_mode, gradient))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateLinearGradientBrush, (ID2D1DeviceContext6 *iface,
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES *gradient_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1GradientStopCollection *gradient,
        ID2D1LinearGradientBrush **brush), (iface, gradient_brush_desc, brush_desc, gradient, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateRadialGradientBrush, (ID2D1DeviceContext6 *iface,
        const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES *gradient_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1GradientStopCollection *gradient,
        ID2D1RadialGradientBrush **brush), (iface, gradient_brush_desc, brush_desc, gradient, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateCompatibleRenderTarget, (ID2D1DeviceContext6 *iface,
        const D2D1_SIZE_F *size, const D2D1_SIZE_U *pixel_size, const D2D1_PIXEL_FORMAT *format,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS options, ID2D1BitmapRenderTarget **rt), (iface, size,
        pixel_size, format, options, rt))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateLayer, (ID2D1DeviceContext6 *iface, const D2D1_SIZE_F *size,
        ID2D1Layer **layer), (iface, size, layer))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateMesh, (ID2D1DeviceContext6 *iface, ID2D1Mesh **mesh), (iface, mesh))
D2D_DEVICE_CONTEXT_MT_VOID(DrawLine, (ID2D1DeviceContext6 *iface, D2D1_POINT_2F p0, D2D1_POINT_2F p1,
        ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style), (iface, p0, p1, brush,
        stroke_width, stroke_style))
D2D_DEVICE_CONTEXT_MT_VOID(DrawRectangle, (ID2D1DeviceContext6 *iface, const D2D1_RECT_F *rect,
        ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style), (iface, rect, brush,
        stroke_width, stroke_style))
D2D_DEVICE_CONTEXT_MT_VOID(FillRectangle, (ID2D1DeviceContext6 *iface, const D2D1_RECT_F *rect,
        ID2D1Brush *brush), (iface, rect, brush))
D2D_DEVICE_CONTEXT_MT_VOID(DrawRoundedRectangle, (ID2D1DeviceContext6 *iface, const D2D1_ROUNDED_RECT *rect,
        ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style), (iface, rect, brush,
        stroke_width, stroke_style))
D2D_DEVICE_CONTEXT_MT_VOID(FillRoundedRectangle, (ID2D1DeviceContext6 *iface, const D2D1_ROUNDED_RECT *rect,
        ID2D1Brush *brush), (iface, rect, brush))
D2D_DEVICE_CONTEXT_MT_VOID(DrawEllipse, (ID2D1DeviceContext6 *iface, const D2D1_ELLIPSE *ellipse,
        ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style), (iface, ellipse, brush,
        stroke_width, stroke_style))
D2D_DEVICE_CONTEXT_MT_VOID(FillEllipse, (ID2D1DeviceContext6 *iface, const D2D1_ELLIPSE *ellipse,
        ID2D1Brush *brush), (iface, ellipse, brush))
D2D_DEVICE_CONTEXT_MT_VOID(DrawGeometry, (ID2D1DeviceContext6 *iface, ID2D1Geometry *geometry,
        ID2D1Brush *brush, float stroke_width, ID2D1StrokeStyle *stroke_style), (iface, geometry, brush,
        stroke_width, stroke_style))
D2D_DEVICE_CONTEXT_MT_VOID(FillGeometry, (ID2D1DeviceContext6 *iface, ID2D1Geometry *geometry,
        ID2D1Brush *brush, ID2D1Brush *opacity_brush), (iface, geometry, brush, opacity_brush))
D2D_DEVICE_CONTEXT_MT_VOID(FillMesh, (ID2D1DeviceContext6 *iface, ID2D1Mesh *mesh, ID2D1Brush *brush), (iface,
        mesh, brush))
D2D_DEVICE_CONTEXT_MT_VOID(FillOpacityMask, (ID2D1DeviceContext6 *iface, ID2D1Bitmap *mask, ID2D1Brush *brush,
        D2D1_OPACITY_MASK_CONTENT content, const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect), (iface,
        mask, brush, content, dst_rect, src_rect))
D2D_DEVICE_CONTEXT_MT_VOID(DrawBitmap, (ID2D1DeviceContext6 *iface, ID2D1Bitmap *bitmap,
        const D2D1_RECT_F *dst_rect, float opacity, D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode,
        const D2D1_RECT_F *src_rect), (iface, bitmap, dst_rect, opacity, interpolation_mode, src_rect))
D2D_DEVICE_CONTEXT_MT_VOID(DrawText, (ID2D1DeviceContext6 *iface, const WCHAR *string, UINT32 string_len,
        IDWriteTextFormat *text_format, const D2D1_RECT_F *layout_rect, ID2D1Brush *brush,
        D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring_mode), (iface, string, string_len,
        text_format, layout_rect, brush, options, measuring_mode))
D2D_DEVICE_CONTEXT_MT_VOID(DrawTextLayout, (ID2D1DeviceContext6 *iface, D2D1_POINT_2F origin,
        IDWriteTextLayout *layout, ID2D1Brush *brush, D2D1_DRAW_TEXT_OPTIONS options), (iface, origin, layout,
        brush, options))
D2D_DEVICE_CONTEXT_MT_VOID(DrawGlyphRun, (ID2D1DeviceContext6 *iface, D2D1_POINT_2F baseline_origin,
        const DWRITE_GLYPH_RUN *glyph_run, ID2D1Brush *brush, DWRITE_MEASURING_MODE measuring_mode), (iface,
        baseline_origin, glyph_run, brush, measuring_mode))
D2D_DEVICE_CONTEXT_MT_VOID(SetTransform, (ID2D1DeviceContext6 *iface, const D2D1_MATRIX_3X2_F *transform),
        (iface, transform))
D2D_DEVICE_CONTEXT_MT_VOID(GetTransform, (ID2D1DeviceContext6 *iface, D2D1_MATRIX_3X2_F *transform), (iface,
        transform))
D2D_DEVICE_CONTEXT_MT_VOID(SetAntialiasMode, (ID2D1DeviceContext6 *iface, D2D1_ANTIALIAS_MODE antialias_mode),
        (iface, antialias_mode))
D2D_DEVICE_CONTEXT_MT(D2D1_ANTIALIAS_MODE, GetAntialiasMode, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT_VOID(SetTextAntialiasMode, (ID2D1DeviceContext6 *iface,
        D2D1_TEXT_ANTIALIAS_MODE antialias_mode), (iface, antialias_mode))
D2D_DEVICE_CONTEXT_MT(D2D1_TEXT_ANTIALIAS_MODE, GetTextAntialiasMode, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT_VOID(SetTextRenderingParams, (ID2D1DeviceContext6 *iface,
        IDWriteRenderingParams *text_rendering_params), (iface, text_rendering_params))
D2D_DEVICE_CONTEXT_MT_VOID(GetTextRenderingParams, (ID2D1DeviceContext6 *iface,
        IDWriteRenderingParams **text_rendering_params), (iface, text_rendering_params))
D2D_DEVICE_CONTEXT_MT_VOID(SetTags, (ID2D1DeviceContext6 *iface, D2D1_TAG tag1, D2D1_TAG tag2), (iface, tag1,
        tag2))
D2D_DEVICE_CONTEXT_MT_VOID(GetTags, (ID2D1DeviceContext6 *iface, D2D1_TAG *tag1, D2D1_TAG *tag2), (iface,
        tag1, tag2))
D2D_DEVICE_CONTEXT_MT_VOID(PushLayer, (ID2D1DeviceContext6 *iface,
        const D2D1_LAYER_PARAMETERS *layer_parameters, ID2D1Layer *layer), (iface, layer_parameters, layer))
D2D_DEVICE_CONTEXT_MT_VOID(PopLayer, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT(HRESULT, Flush, (ID2D1DeviceContext6 *iface, D2D1_TAG *tag1, D2D1_TAG *tag2), (iface,
        tag1, tag2))
D2D_DEVICE_CONTEXT_MT_VOID(SaveDrawingState, (ID2D1DeviceContext6 *iface,
        ID2D1DrawingStateBlock *state_block), (iface, state_block))
D2D_DEVICE_CONTEXT_MT_VOID(RestoreDrawingState, (ID2D1DeviceContext6 *iface,
        ID2D1DrawingStateBlock *state_block), (iface, state_block))
D2D_DEVICE_CONTEXT_MT_VOID(PushAxisAlignedClip, (ID2D1DeviceContext6 *iface, const D2D1_RECT_F *clip_rect,
        D2D1_ANTIALIAS_MODE antialias_mode), (iface, clip_rect, antialias_mode))
D2D_DEVICE_CONTEXT_MT_VOID(PopAxisAlignedClip, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT_VOID(Clear, (ID2D1DeviceContext6 *iface, const D2D1_COLOR_F *colour), (iface, colour))
D2D_DEVICE_CONTEXT_MT_VOID(BeginDraw, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT(HRESULT, EndDraw, (ID2D1DeviceContext6 *iface, D2D1_TAG *tag1, D2D1_TAG *tag2), (iface,
        tag1, tag2))
D2D_DEVICE_CONTEXT_MT(D2D1_PIXEL_FORMAT *, GetPixelFormat, (ID2D1DeviceContext6 *iface,
        D2D1_PIXEL_FORMAT *format), (iface, format))
D2D_DEVICE_CONTEXT_MT_VOID(SetDpi, (ID2D1DeviceContext6 *iface, float dpi_x, float dpi_y), (iface, dpi_x,
        dpi_y))
D2D_DEVICE_CONTEXT_MT_VOID(GetDpi, (ID2D1DeviceContext6 *iface, float *dpi_x, float *dpi_y), (iface, dpi_x,
        dpi_y))
D2D_DEVICE_CONTEXT_MT(D2D1_SIZE_F *, GetSize, (ID2D1DeviceContext6 *iface, D2D1_SIZE_F *size), (iface, size))
D2D_DEVICE_CONTEXT_MT(D2D1_SIZE_U *, GetPixelSize, (ID2D1DeviceContext6 *iface, D2D1_SIZE_U *pixel_size),
        (iface, pixel_size))
D2D_DEVICE_CONTEXT_MT(UINT32, GetMaximumBitmapSize, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT(BOOL, IsSupported, (ID2D1DeviceContext6 *iface,
        const D2D1_RENDER_TARGET_PROPERTIES *desc), (iface, desc))
D2D_DEVICE_CONTEXT_MT(HRESULT, ID2D1DeviceContext_CreateBitmap, (ID2D1DeviceContext6 *iface, D2D1_SIZE_U size,
        const void *src_data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap),
        (iface, size, src_data, pitch, desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, ID2D1DeviceContext_CreateBitmapFromWicBitmap, (ID2D1DeviceContext6 *iface,
        IWICBitmapSource *bitmap_source, const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap), (iface,
        bitmap_source, desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateColorContext, (ID2D1DeviceContext6 *iface, D2D1_COLOR_SPACE space,
        const BYTE *profile, UINT32 profile_size, ID2D1ColorContext **color_context), (iface, space, profile,
        profile_size, color_context))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateColorContextFromFilename, (ID2D1DeviceContext6 *iface,
        const WCHAR *filename, ID2D1ColorContext **color_context), (iface, filename, color_context))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateColorContextFromWicColorContext, (ID2D1DeviceContext6 *iface,
        IWICColorContext *wic_color_context, ID2D1ColorContext **color_context), (iface, wic_color_context,
        color_context))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateBitmapFromDxgiSurface, (ID2D1DeviceContext6 *iface,
        IDXGISurface *surface, const D2D1_BITMAP_PROPERTIES1 *desc, ID2D1Bitmap1 **bitmap), (iface, surface,
        desc, bitmap))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateEffect, (ID2D1DeviceContext6 *iface, REFCLSID effect_id,
        ID2D1Effect **effect), (iface, effect_id, effect))
D2D_DEVICE_CONTEXT_MT(HRESULT, ID2D1DeviceContext_CreateGradientStopCollection, (ID2D1DeviceContext6 *iface,
        const D2D1_GRADIENT_STOP *stops, UINT32 stop_count, D2D1_COLOR_SPACE preinterpolation_space,
        D2D1_COLOR_SPACE postinterpolation_space, D2D1_BUFFER_PRECISION buffer_precision,
        D2D1_EXTEND_MODE extend_mode, D2D1_COLOR_INTERPOLATION_MODE color_interpolation_mode,
        ID2D1GradientStopCollection1 **gradient), (iface, stops, stop_count, preinterpolation_space,
        postinterpolation_space, buffer_precision, extend_mode, color_interpolation_mode, gradient))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateImageBrush, (ID2D1DeviceContext6 *iface, ID2D1Image *image,
        const D2D1_IMAGE_BRUSH_PROPERTIES *image_brush_desc, const D2D1_BRUSH_PROPERTIES *brush_desc,
        ID2D1ImageBrush **brush), (iface, image, image_brush_desc, brush_desc, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, ID2D1DeviceContext_CreateBitmapBrush, (ID2D1DeviceContext6 *iface,
        ID2D1Bitmap *bitmap, const D2D1_BITMAP_BRUSH_PROPERTIES1 *bitmap_brush_desc,
        const D2D1_BRUSH_PROPERTIES *brush_desc, ID2D1BitmapBrush1 **brush), (iface, bitmap,
        bitmap_brush_desc, brush_desc, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateCommandList, (ID2D1DeviceContext6 *iface,
        ID2D1CommandList **command_list), (iface, command_list))
D2D_DEVICE_CONTEXT_MT(BOOL, IsDxgiFormatSupported, (ID2D1DeviceContext6 *iface, DXGI_FORMAT format), (iface,
        format))
D2D_DEVICE_CONTEXT_MT(BOOL, IsBufferPrecisionSupported, (ID2D1DeviceContext6 *iface,
        D2D1_BUFFER_PRECISION buffer_precision), (iface, buffer_precision))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetImageLocalBounds, (ID2D1DeviceContext6 *iface, ID2D1Image *image,
        D2D1_RECT_F *local_bounds), (iface, image, local_bounds))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetImageWorldBounds, (ID2D1DeviceContext6 *iface, ID2D1Image *image,
        D2D1_RECT_F *world_bounds), (iface, image, world_bounds))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetGlyphRunWorldBounds, (ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        DWRITE_MEASURING_MODE measuring_mode, D2D1_RECT_F *bounds), (iface, baseline_origin, glyph_run,
        measuring_mode, bounds))
D2D_DEVICE_CONTEXT_MT_VOID(GetDevice, (ID2D1DeviceContext6 *iface, ID2D1Device **device), (iface, device))
D2D_DEVICE_CONTEXT_MT_VOID(SetTarget, (ID2D1DeviceContext6 *iface, ID2D1Image *target), (iface, target))
D2D_DEVICE_CONTEXT_MT_VOID(GetTarget, (ID2D1DeviceContext6 *iface, ID2D1Image **target), (iface, target))
D2D_DEVICE_CONTEXT_MT_VOID(SetRenderingControls, (ID2D1DeviceContext6 *iface,
        const D2D1_RENDERING_CONTROLS *rendering_controls), (iface, rendering_controls))
D2D_DEVICE_CONTEXT_MT_VOID(GetRenderingControls, (ID2D1DeviceContext6 *iface,
        D2D1_RENDERING_CONTROLS *rendering_controls), (iface, rendering_controls))
D2D_DEVICE_CONTEXT_MT_VOID(SetPrimitiveBlend, (ID2D1DeviceContext6 *iface,
        D2D1_PRIMITIVE_BLEND primitive_blend), (iface, primitive_blend))
D2D_DEVICE_CONTEXT_MT(D2D1_PRIMITIVE_BLEND, GetPrimitiveBlend, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT_VOID(SetUnitMode, (ID2D1DeviceContext6 *iface, D2D1_UNIT_MODE unit_mode), (iface,
        unit_mode))
D2D_DEVICE_CONTEXT_MT(D2D1_UNIT_MODE, GetUnitMode, (ID2D1DeviceContext6 *iface), (iface))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext_DrawGlyphRun, (ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F baseline_origin, const DWRITE_GLYPH_RUN *glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION *glyph_run_desc, ID2D1Brush *brush,
        DWRITE_MEASURING_MODE measuring_mode), (iface, baseline_origin, glyph_run, glyph_run_desc, brush,
        measuring_mode))
D2D_DEVICE_CONTEXT_MT_VOID(DrawImage, (ID2D1DeviceContext6 *iface, ID2D1Image *image,
        const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode, D2D1_COMPOSITE_MODE composite_mode), (iface, image,
        target_offset, image_rect, interpolation_mode, composite_mode))
D2D_DEVICE_CONTEXT_MT_VOID(DrawGdiMetafile, (ID2D1DeviceContext6 *iface, ID2D1GdiMetafile *metafile,
        const D2D1_POINT_2F *target_offset), (iface, metafile, target_offset))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext_DrawBitmap, (ID2D1DeviceContext6 *iface, ID2D1Bitmap *bitmap,
        const D2D1_RECT_F *dst_rect, float opacity, D2D1_INTERPOLATION_MODE interpolation_mode,
        const D2D1_RECT_F *src_rect, const D2D1_MATRIX_4X4_F *perspective_transform), (iface, bitmap,
        dst_rect, opacity, interpolation_mode, src_rect, perspective_transform))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext_PushLayer, (ID2D1DeviceContext6 *iface,
        const D2D1_LAYER_PARAMETERS1 *layer_parameters, ID2D1Layer *layer), (iface, layer_parameters, layer))
D2D_DEVICE_CONTEXT_MT(HRESULT, InvalidateEffectInputRectangle, (ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, UINT32 input, const D2D1_RECT_F *input_rect), (iface, effect, input, input_rect))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetEffectInvalidRectangleCount, (ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, UINT32 *rect_count), (iface, effect, rect_count))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetEffectInvalidRectangles, (ID2D1DeviceContext6 *iface, ID2D1Effect *effect,
        D2D1_RECT_F *rectangles, UINT32 rect_count), (iface, effect, rectangles, rect_count))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetEffectRequiredInputRectangles, (ID2D1DeviceContext6 *iface,
        ID2D1Effect *effect, const D2D1_RECT_F *image_rect, const D2D1_EFFECT_INPUT_DESCRIPTION *desc,
        D2D1_RECT_F *input_rect, UINT32 input_count), (iface, effect, image_rect, desc, input_rect,
        input_count))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext_FillOpacityMask, (ID2D1DeviceContext6 *iface, ID2D1Bitmap *mask,
        ID2D1Brush *brush, const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect), (iface, mask, brush,
        dst_rect, src_rect))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateFilledGeometryRealization, (ID2D1DeviceContext6 *iface,
        ID2D1Geometry *geometry, float tolerance, ID2D1GeometryRealization **realization), (iface, geometry,
        tolerance, realization))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateStrokedGeometryRealization, (ID2D1DeviceContext6 *iface,
        ID2D1Geometry *geometry, float tolerance, float stroke_width, ID2D1StrokeStyle *stroke_style,
        ID2D1GeometryRealization **realization), (iface, geometry, tolerance, stroke_width, stroke_style,
        realization))
D2D_DEVICE_CONTEXT_MT_VOID(DrawGeometryRealization, (ID2D1DeviceContext6 *iface,
        ID2D1GeometryRealization *realization, ID2D1Brush *brush), (iface, realization, brush))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateInk, (ID2D1DeviceContext6 *iface, const D2D1_INK_POINT *start_point,
        ID2D1Ink **ink), (iface, start_point, ink))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateInkStyle, (ID2D1DeviceContext6 *iface,
        const D2D1_INK_STYLE_PROPERTIES *ink_style_properties, ID2D1InkStyle **ink_style), (iface,
        ink_style_properties, ink_style))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateGradientMesh, (ID2D1DeviceContext6 *iface,
        const D2D1_GRADIENT_MESH_PATCH *patches, UINT32 patches_count, ID2D1GradientMesh **gradient_mesh),
        (iface, patches, patches_count, gradient_mesh))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateImageSourceFromWic, (ID2D1DeviceContext6 *iface,
        IWICBitmapSource *wic_bitmap_source, D2D1_IMAGE_SOURCE_LOADING_OPTIONS loading_options,
        D2D1_ALPHA_MODE alpha_mode, ID2D1ImageSourceFromWic **image_source), (iface, wic_bitmap_source,
        loading_options, alpha_mode, image_source))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateLookupTable3D, (ID2D1DeviceContext6 *iface,
        D2D1_BUFFER_PRECISION precision, const UINT32 *extents, const BYTE *data, UINT32 data_count,
        const UINT32 *strides, ID2D1LookupTable3D **lookup_table), (iface, precision, extents, data,
        data_count, strides, lookup_table))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateImageSourceFromDxgi, (ID2D1DeviceContext6 *iface,
        IDXGISurface **surfaces, UINT32 surface_count, DXGI_COLOR_SPACE_TYPE color_space,
        D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS options, ID2D1ImageSource **image_source), (iface, surfaces,
        surface_count, color_space, options, image_source))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetGradientMeshWorldBounds, (ID2D1DeviceContext6 *iface,
        ID2D1GradientMesh *gradient_mesh, D2D1_RECT_F *bounds), (iface, gradient_mesh, bounds))
D2D_DEVICE_CONTEXT_MT_VOID(DrawInk, (ID2D1DeviceContext6 *iface, ID2D1Ink *ink, ID2D1Brush *brush,
        ID2D1InkStyle *ink_style), (iface, ink, brush, ink_style))
D2D_DEVICE_CONTEXT_MT_VOID(DrawGradientMesh, (ID2D1DeviceContext6 *iface, ID2D1GradientMesh *gradient_mesh),
        (iface, gradient_mesh))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext2_DrawGdiMetafile, (ID2D1DeviceContext6 *iface,
        ID2D1GdiMetafile *gdi_metafile, const D2D1_RECT_F *dst_rect, const D2D1_RECT_F *src_rect), (iface,
        gdi_metafile, dst_rect, src_rect))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateTransformedImageSource, (ID2D1DeviceContext6 *iface,
        ID2D1ImageSource *source, const D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES *props,
        ID2D1TransformedImageSource **transformed), (iface, source, props, transformed))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateSpriteBatch, (ID2D1DeviceContext6 *iface,
        ID2D1SpriteBatch **sprite_batch), (iface, sprite_batch))
D2D_DEVICE_CONTEXT_MT_VOID(DrawSpriteBatch, (ID2D1DeviceContext6 *iface, ID2D1SpriteBatch *sprite_batch,
        UINT32 start_index, UINT32 sprite_count, ID2D1Bitmap *bitmap,
        D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode, D2D1_SPRITE_OPTIONS sprite_options), (iface,
        sprite_batch, start_index, sprite_count, bitmap, interpolation_mode, sprite_options))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateSvgGlyphStyle, (ID2D1DeviceContext6 *iface,
        ID2D1SvgGlyphStyle **svg_glyph_style), (iface, svg_glyph_style))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext4_DrawText, (ID2D1DeviceContext6 *iface, const WCHAR *string,
        UINT32 string_length, IDWriteTextFormat *text_format, const D2D1_RECT_F *layout_rect,
        ID2D1Brush *default_fill_brush, ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index,
        D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring_mode), (iface, string, string_length,
        text_format, layout_rect, default_fill_brush, svg_glyph_style, color_palette_index, options,
        measuring_mode))
D2D_DEVICE_CONTEXT_MT_VOID(ID2D1DeviceContext4_DrawTextLayout, (ID2D1DeviceContext6 *iface,
        D2D1_POINT_2F origin, IDWriteTextLayout *text_layout, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index, D2D1_DRAW_TEXT_OPTIONS options),
        (iface, origin, text_layout, default_fill_brush, svg_glyph_style, color_palette_index, options))
D2D_DEVICE_CONTEXT_MT_VOID(DrawColorBitmapGlyphRun, (ID2D1DeviceContext6 *iface,
        DWRITE_GLYPH_IMAGE_FORMATS glyph_image_format, D2D1_POINT_2F baseline_origin,
        const DWRITE_GLYPH_RUN *glyph_run, DWRITE_MEASURING_MODE measuring_mode,
        D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION bitmap_snap_option), (iface, glyph_image_format, baseline_origin,
        glyph_run, measuring_mode, bitmap_snap_option))
D2D_DEVICE_CONTEXT_MT_VOID(DrawSvgGlyphRun, (ID2D1DeviceContext6 *iface, D2D1_POINT_2F baseline_origin,
        const DWRITE_GLYPH_RUN *glyph_run, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index,
        DWRITE_MEASURING_MODE measuring_mode), (iface, baseline_origin, glyph_run, default_fill_brush,
        svg_glyph_style, color_palette_index, measuring_mode))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetColorBitmapGlyphImage, (ID2D1DeviceContext6 *iface,
        DWRITE_GLYPH_IMAGE_FORMATS glyph_image_format, D2D1_POINT_2F glyph_origin, IDWriteFontFace *font_face,
        FLOAT font_em_size, UINT16 glyph_index, BOOL is_sideways, const D2D1_MATRIX_3X2_F *world_transform,
        FLOAT dpi_x, FLOAT dpi_y, D2D1_MATRIX_3X2_F *glyph_transform, ID2D1Image **glyph_image), (iface,
        glyph_image_format, glyph_origin, font_face, font_em_size, glyph_index, is_sideways, world_transform,
        dpi_x, dpi_y, glyph_transform, glyph_image))
D2D_DEVICE_CONTEXT_MT(HRESULT, GetSvgGlyphImage, (ID2D1DeviceContext6 *iface, D2D1_POINT_2F glyph_origin,
        IDWriteFontFace *font_face, FLOAT font_em_size, UINT16 glyph_index, BOOL is_sideways,
        const D2D1_MATRIX_3X2_F *world_transform, ID2D1Brush *default_fill_brush,
        ID2D1SvgGlyphStyle *svg_glyph_style, UINT32 color_palette_index, D2D1_MATRIX_3X2_F *glyph_transform,
        ID2D1CommandList **glyph_image), (iface, glyph_origin, font_face, font_em_size, glyph_index,
        is_sideways, world_transform, default_fill_brush, svg_glyph_style, color_palette_index,
        glyph_transform, glyph_image))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateSvgDocument, (ID2D1DeviceContext6 *iface, IStream *input_xml_stream,
        D2D1_SIZE_F viewport_size, ID2D1SvgDocument **svg_document), (iface, input_xml_stream, viewport_size,
        svg_document))
D2D_DEVICE_CONTEXT_MT_VOID(DrawSvgDocument, (ID2D1DeviceContext6 *iface, ID2D1SvgDocument *svg_document),
        (iface, svg_document))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateColorContextFromDxgiColorSpace, (ID2D1DeviceContext6 *iface,
        DXGI_COLOR_SPACE_TYPE color_space, ID2D1ColorContext1 **color_context), (iface, color_space,
        color_context))
D2D_DEVICE_CONTEXT_MT(HRESULT, CreateColorContextFromSimpleColorProfile, (ID2D1DeviceContext6 *iface,
        const D2D1_SIMPLE_COLOR_PROFILE *simple_profile, ID2D1ColorContext1 **color_context), (iface,
        simple_profile, color_context))
D2D_DEVICE_CONTEXT_MT_VOID(BlendImage, (ID2D1DeviceContext6 *iface, ID2D1Image *image,
        D2D1_BLEND_MODE blend_mode, const D2D1_POINT_2F *target_offset, const D2D1_RECT_F *image_rect,
        D2D1_INTERPOLATION_MODE interpolation_mode), (iface, image, blend_mode, target_offset, image_rect,
        interpolation_mode))

#undef D2D_DEVICE_CONTEXT_MT_VOID
#undef D2D_DEVICE_CONTEXT_MT

static const struct ID2D1DeviceContext6Vtbl d2d_device_context_mt_vtbl =
{
    d2d_device_context_QueryInterface,
    d2d_device_context_AddRef,
    d2d_device_context_Release,
    d2d_device_context_mt_GetFactory,
    d2d_device_context_mt_CreateBitmap,
    d2d_device_context_mt_CreateBitmapFromWicBitmap,
    d2d_device_context_mt_CreateSharedBitmap,
    d2d_device_context_mt_CreateBitmapBrush,
    d2d_device_context_mt_CreateSolidColorBrush,
    d2d_device_context_mt_CreateGradientStopCollection,
    d2d_device_context_mt_CreateLinearGradientBrush,
    d2d_device_context_mt_CreateRadialGradientBrush,
    d2d_device_context_mt_CreateCompatibleRenderTarget,
    d2d_device_context_mt_CreateLayer,
    d2d_device_context_mt_CreateMesh,
    d2d_device_context_mt_DrawLine,
    d2d_device_context_mt_DrawRectangle,
    d2d_device_context_mt_FillRectangle,
    d2d_device_context_mt_DrawRoundedRectangle,
    d2d_device_context_mt_FillRoundedRectangle,
    d2d_device_context_mt_DrawEllipse,
    d2d_device_context_mt_FillEllipse,
    d2d_device_context_mt_DrawGeometry,
    d2d_device_context_mt_FillGeometry,
    d2d_device_context_mt_FillMesh,
    d2d_device_context_mt_FillOpacityMask,
    d2d_device_context_mt_DrawBitmap,
    d2d_device_context_mt_DrawText,
    d2d_device_context_mt_DrawTextLayout,
    d2d_device_context_mt_DrawGlyphRun,
    d2d_device_context_mt_SetTransform,
    d2d_device_context_mt_GetTransform,
    d2d_device_context_mt_SetAntialiasMode,
    d2d_device_context_mt_GetAntialiasMode,
    d2d_device_context_mt_SetTextAntialiasMode,
    d2d_device_context_mt_GetTextAntialiasMode,
    d2d_device_context_mt_SetTextRenderingParams,
    d2d_device_context_mt_GetTextRenderingParams,
    d2d_device_context_mt_SetTags,
    d2d_device_context_mt_GetTags,
    d2d_device_context_mt_PushLayer,
    d2d_device_context_mt_PopLayer,
    d2d_device_context_mt_Flush,
    d2d_device_context_mt_SaveDrawingState,
    d2d_device_context_mt_RestoreDrawingState,
    d2d_device_context_mt_PushAxisAlignedClip,
    d2d_device_context_mt_PopAxisAlignedClip,
    d2d_device_context_mt_Clear,
    d2d_device_context_mt_BeginDraw,
    d2d_device_context_mt_EndDraw,
    d2d_device_context_mt_GetPixelFormat,
    d2d_device_context_mt_SetDpi,
    d2d_device_context_mt_GetDpi,
    d2d_device_context_mt_GetSize,
    d2d_device_context_mt_GetPixelSize,
    d2d_device_context_mt_GetMaximumBitmapSize,
    d2d_device_context_mt_IsSupported,
    d2d_device_context_mt_ID2D1DeviceContext_CreateBitmap,
    d2d_device_context_mt_ID2D1DeviceContext_CreateBitmapFromWicBitmap,
    d2d_device_context_mt_CreateColorContext,
    d2d_device_context_mt_CreateColorContextFromFilename,
    d2d_device_context_mt_CreateColorContextFromWicColorContext,
    d2d_device_context_mt_CreateBitmapFromDxgiSurface,
    d2d_device_context_mt_CreateEffect,
    d2d_device_context_mt_ID2D1DeviceContext_CreateGradientStopCollection,
    d2d_device_context_mt_CreateImageBrush,
    d2d_device_context_mt_ID2D1DeviceContext_CreateBitmapBrush,
    d2d_device_context_mt_CreateCommandList,
    d2d_device_context_mt_IsDxgiFormatSupported,
    d2d_device_context_mt_IsBufferPrecisionSupported,
    d2d_device_context_mt_GetImageLocalBounds,
    d2d_device_context_mt_GetImageWorldBounds,
    d2d_device_context_mt_GetGlyphRunWorldBounds,
    d2d_device_context_mt_GetDevice,
    d2d_device_context_mt_SetTarget,
    d2d_device_context_mt_GetTarget,
    d2d_device_context_mt_SetRenderingControls,
    d2d_device_context_mt_GetRenderingControls,
    d2d_device_context_mt_SetPrimitiveBlend,
    d2d_device_context_mt_GetPrimitiveBlend,
    d2d_device_context_mt_SetUnitMode,
    d2d_device_context_mt_GetUnitMode,
    d2d_device_context_mt_ID2D1DeviceContext_DrawGlyphRun,
    d2d_device_context_mt_DrawImage,
    d2d_device_context_mt_DrawGdiMetafile,
    d2d_device_context_mt_ID2D1DeviceContext_DrawBitmap,
    d2d_device_context_mt_ID2D1DeviceContext_PushLayer,
    d2d_device_context_mt_InvalidateEffectInputRectangle,
    d2d_device_context_mt_GetEffectInvalidRectangleCount,
    d2d_device_context_mt_GetEffectInvalidRectangles,
    d2d_device_context_mt_GetEffectRequiredInputRectangles,
    d2d_device_context_mt_ID2D1DeviceContext_FillOpacityMask,
    d2d_device_context_mt_CreateFilledGeometryRealization,
    d2d_device_context_mt_CreateStrokedGeometryRealization,
    d2d_device_context_mt_DrawGeometryRealization,
    d2d_device_context_mt_CreateInk,
    d2d_device_context_mt_CreateInkStyle,
    d2d_device_context_mt_CreateGradientMesh,
    d2d_device_context_mt_CreateImageSourceFromWic,
    d2d_device_context_mt_CreateLookupTable3D,
    d2d_device_context_mt_CreateImageSourceFromDxgi,
    d2d_device_context_mt_GetGradientMeshWorldBounds,
    d2d_device_context_mt_DrawInk,
    d2d_device_context_mt_DrawGradientMesh,
    d2d_device_context_mt_ID2D1DeviceContext2_DrawGdiMetafile,
    d2d_device_context_mt_CreateTransformedImageSource,
    d2d_device_context_mt_CreateSpriteBatch,
    d2d_device_context_mt_DrawSpriteBatch,
    d2d_device_context_mt_CreateSvgGlyphStyle,
    d2d_device_context_mt_ID2D1DeviceContext4_DrawText,
    d2d_device_context_mt_ID2D1DeviceContext4_DrawTextLayout,
    d2d_device_context_mt_DrawColorBitmapGlyphRun,
    d2d_device_context_mt_DrawSvgGlyphRun,
    d2d_device_context_mt_GetColorBitmapGlyphImage,
    d2d_device_context_mt_GetSvgGlyphImage,
    d2d_device_context_mt_CreateSvgDocument,
    d2d_device_context_mt_DrawSvgDocument,
    d2d_device_context_mt_CreateColorContextFromDxgiColorSpace,
    d2d_device_context_mt_CreateColorContextFromSimpleColorProfile,
    d2d_device_context_mt_BlendImage,
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

static HRESULT d2d_gdi_interop_get_dc(struct d2d_device_context *render_target,
        D2D1_DC_INITIALIZE_MODE mode, HDC *dc)
{
    IDXGISurface1 *surface;
    HRESULT hr;

    d2d_device_context_flush_batches(render_target);

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

static HRESULT d2d_gdi_interop_release_dc(struct d2d_device_context *render_target, const RECT *update)
{
    IDXGISurface1 *surface;
    RECT update_rect;
    HRESULT hr;

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

static HRESULT STDMETHODCALLTYPE d2d_gdi_interop_render_target_GetDC(ID2D1GdiInteropRenderTarget *iface,
        D2D1_DC_INITIALIZE_MODE mode, HDC *dc)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);
    HRESULT hr;

    TRACE("iface %p, mode %d, dc %p.\n", iface, mode, dc);

    if (render_target->cs)
        EnterCriticalSection(render_target->cs);
    hr = d2d_gdi_interop_get_dc(render_target, mode, dc);
    if (render_target->cs)
        LeaveCriticalSection(render_target->cs);

    return hr;
}

static HRESULT STDMETHODCALLTYPE d2d_gdi_interop_render_target_ReleaseDC(ID2D1GdiInteropRenderTarget *iface,
        const RECT *update)
{
    struct d2d_device_context *render_target = impl_from_ID2D1GdiInteropRenderTarget(iface);
    HRESULT hr;

    TRACE("iface %p, update rect %s.\n", iface, wine_dbgstr_rect(update));

    if (render_target->cs)
        EnterCriticalSection(render_target->cs);
    hr = d2d_gdi_interop_release_dc(render_target, update);
    if (render_target->cs)
        LeaveCriticalSection(render_target->cs);

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
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 6, DXGI_FORMAT_R32G32_FLOAT,    0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32_FLOAT, 0, 88, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
static const char shape_vs_code_outline[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
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
    "    float2 q_prev, q_next, pos_w, off_w = 0.0f;\n"
    "    float2x2 geom, rt;\n"
    "    float2x3 to_px;\n"
    "    float hw;\n"
    "\n"
    "    rt = float2x2(transform_rtx.xy, transform_rty.xy);\n"
    "    to_px = float2x3(transform_rtx.xyz, transform_rty.xyz);\n"
    "    hw = transform_geometry_r1.w * 0.5f;\n"
    "    o.stroke_transform = rt * hw;\n"
    "    /* The outline pixel shader takes seam_t.xy as the pixel distances to\n"
    "     * the lines across the ends of a straight segment, where they meet a\n"
    "     * flat cap; anything else is far from both. */\n"
    "    o.seam_t = float3(65536.0f, 65536.0f, 0.0f);\n"
    "    o.seam_c1 = 0.0f;\n"
    "    o.seam_c2 = 0.0f;\n"
    "    o.corner01 = 0.0f;\n"
    "    o.corner2 = 0.0f;\n"
    "    o.corner_on = 0.0f;\n"
    "\n"
    "    geom = float2x2(transform_geometry_r0.xy, transform_geometry_r1.xy);\n"
    "    q_prev = normalize(mul(geom, prev));\n"
    "    q_next = normalize(mul(geom, next));\n"
    "    pos_w = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "\n"
    "    if (length(prev) > 7.5f)\n"
    "    {\n"
    "        /* A straight segment: prev is the direction along it, scaled by 8\n"
    "         * plus a role code, next is the far end point. Code bits: 1 the\n"
    "         * side, 2 the far end, 4 and 8 what the start meets, 16 and 32 what\n"
    "         * the end meets (0 a join, 1 the start cap, 2 the end cap, 3 the\n"
    "         * dash cap). The quad is widened to the half width plus 0.75 px. An\n"
    "         * end that meets a flat cap, which draws nothing of its own, is\n"
    "         * lengthened by 0.75 px as well, and the pixel shader gets the pixel\n"
    "         * distances to the lines across both ends to ramp it like the\n"
    "         * sides; an end that meets a join or another cap keeps the hard\n"
    "         * edge that the join or cap continues from. */\n"
    "        float2 n, other_w;\n"
    "        float side, len_px, px_per_unit, hw_px, expand, e_s, e_e, along, big = 65536.0f;\n"
    "        uint code, kind_s, kind_e, cap_s, cap_e;\n"
    "        bool at_end, flat_s, flat_e;\n"
    "\n"
    "        code = (uint)round(length(prev)) - 8;\n"
    "        side = (code & 1) ? -1.0f : 1.0f;\n"
    "        at_end = (code & 2) != 0;\n"
    "        kind_s = (code >> 2) & 3;\n"
    "        kind_e = (code >> 4) & 3;\n"
    "        cap_s = (uint)(kind_s == 1 ? stroke_params.y : kind_s == 2 ? stroke_params.z : stroke_params.w);\n"
    "        cap_e = (uint)(kind_e == 1 ? stroke_params.y : kind_e == 2 ? stroke_params.z : stroke_params.w);\n"
    "        flat_s = kind_s != 0 && cap_s == 0;\n"
    "        flat_e = kind_e != 0 && cap_e == 0;\n"
    "\n"
    "        n = float2(-q_prev.y, q_prev.x);\n"
    "        hw_px = length(mul(rt, n)) * hw;\n"
    "        expand = hw_px > 0.001f ? 0.75f / hw_px : 0.0f;\n"
    "        px_per_unit = max(length(mul(rt, q_prev)), 0.0001f);\n"
    "        other_w = float2(dot(float3(next, 1.0f), transform_geometry_r0.xyz), dot(float3(next, 1.0f), transform_geometry_r1.xyz));\n"
    "        len_px = length(mul(rt, other_w - pos_w));\n"
    "        e_s = flat_s ? 0.75f : 0.0f;\n"
    "        e_e = flat_e ? 0.75f : 0.0f;\n"
    "\n"
    "        off_w = side * hw * (1.0f + expand) * n + (at_end ? e_e : -e_s) / px_per_unit * q_prev;\n"
    "        along = at_end ? len_px + e_e : -e_s;\n"
    "        o.b = float4(side * (1.0f + expand), 0.0f, 0.0f, 1.0f);\n"
    "        o.seam_t = float3(flat_s ? along : big, flat_e ? len_px - along : big, 0.0f);\n"
    "    }\n"
    "    else if (dot(q_prev, q_next) < 0.9999f)\n"
    "    {\n"
    "        /* A join or a cap. The four vertices sit on the corner, prev and next\n"
    "         * are the directions into and out of it (for a cap: outward and back),\n"
    "         * and the vector lengths name the corner of the kite: |prev| 1/2 and\n"
    "         * |next| 1/2 give inner point, edge point of prev, edge point of next,\n"
    "         * tip; a cap carries 3/4 instead of 1/2 on the next vector (start cap)\n"
    "         * or on the prev vector (end cap). The kite is widened by 0.75 px like\n"
    "         * the segment quads, and the pixel shader gets what it needs to ramp\n"
    "         * the real outline: the offset from the corner and the radius for a\n"
    "         * round join or cap (b.w = 3), or the signed pixel distances to three\n"
    "         * bounding lines for the others (b.w = 4). */\n"
    "        float2 pp, pn, m0, mh, c_px, v_px, d1 = 0.0f, d2 = 0.0f, d3 = 0.0f;\n"
    "        float2 inner = 0.0f, e_p = 0.0f, e_n = 0.0f, tip = 0.0f, off = 0.0f;\n"
    "        float2 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;\n"
    "        float s, cosh, limit, mlen, hw_px, expand, marker = 4.0f;\n"
    "        uint join, cap, role;\n"
    "        bool hairpin, bevel, is_cap, start, dash;\n"
    "        float cap_split;\n"
    "\n"
    "        /* The end of a dash inside the figure carries 5/6 instead of 3/4 and\n"
    "         * takes the dash cap (stroke_params.w). */\n"
    "        is_cap = max(length(prev), length(next)) > 2.5f;\n"
    "        start = length(next) > 2.5f;\n"
    "        dash = max(length(prev), length(next)) > 4.5f;\n"
    "        cap_split = dash ? 5.5f : 3.5f;\n"
    "        if (is_cap)\n"
    "            role = (length(prev) > (start ? 1.5f : cap_split) ? 2 : 0) + (length(next) > (start ? cap_split : 1.5f) ? 1 : 0);\n"
    "        else\n"
    "            role = (length(prev) > 1.5f ? 2 : 0) + (length(next) > 1.5f ? 1 : 0);\n"
    "        join = (uint)stroke_params.x;\n"
    "        cap = (uint)(dash ? stroke_params.w : start ? stroke_params.y : stroke_params.z);\n"
    "        pp = float2(-q_prev.y, q_prev.x);\n"
    "        pn = float2(-q_next.y, q_next.x);\n"
    "        m0 = pp + pn;\n"
    "        cosh = length(m0) * 0.5f;\n"
    "        hairpin = cosh < 0.05f;\n"
    "        /* The outer side is the one the path turns away from. At a hairpin\n"
    "         * or a cap m0 is rounding noise, and the four vertices would each\n"
    "         * decide the sign for themselves, so the rectangle takes the sides\n"
    "         * as they are. */\n"
    "        s = !hairpin && dot(m0, q_next - q_prev) > 0.0f ? -1.0f : 1.0f;\n"
    "        pp *= s;\n"
    "        pn *= s;\n"
    "        m0 *= s;\n"
    "        mh = hairpin ? q_prev : m0 / (2.0f * cosh);\n"
    "        limit = max(transform_geometry_r0.w, 1.0f);\n"
    "        mlen = hairpin ? limit : min(1.0f / cosh, limit);\n"
    "        hw_px = length(mul(rt, mh)) * hw;\n"
    "        expand = hw_px > 0.001f ? 0.75f / hw_px : 0.0f;\n"
    "        c_px = mul(to_px, float3(pos_w, 1.0f));\n"
    "\n"
    "        if (is_cap)\n"
    "        {\n"
    "            /* q_prev points outward, pp and pn are the two sides of the stroke. */\n"
    "            /* Round and square caps fill the rectangle past the end, whose\n"
    "             * four corners the four vertices take: inner and e_p on the\n"
    "             * stroke's edge, tip and e_n out at the cap's end. */\n"
    "            if (cap == 2)\n"
    "            {\n"
    "                inner = pp;\n"
    "                e_p = pp + mh;\n"
    "                e_n = pn;\n"
    "                tip = pn + mh;\n"
    "                marker = 3.0f;\n"
    "            }\n"
    "            else if (cap == 1)\n"
    "            {\n"
    "                inner = pp;\n"
    "                e_p = pp + mh;\n"
    "                e_n = pn;\n"
    "                tip = pn + mh;\n"
    "                a1 = c_px + mul(rt, pp * hw);\n"
    "                a2 = c_px + mul(rt, pn * hw);\n"
    "                a3 = c_px + mul(rt, mh * hw);\n"
    "                d1 = normalize(mul(rt, mh));\n"
    "                n1 = float2(-d1.y, d1.x);\n"
    "                n1 *= sign(dot(n1, c_px - a1));\n"
    "                n2 = -n1;\n"
    "                n3 = -d1;\n"
    "            }\n"
    "            else if (cap == 3)\n"
    "            {\n"
    "                e_p = pp;\n"
    "                e_n = pn;\n"
    "                tip = mh;\n"
    "                a1 = c_px + mul(rt, pp * hw);\n"
    "                a2 = c_px + mul(rt, pn * hw);\n"
    "                d1 = normalize(mul(rt, mh - pp));\n"
    "                d2 = normalize(mul(rt, mh - pn));\n"
    "                n1 = float2(-d1.y, d1.x);\n"
    "                n1 *= sign(dot(n1, c_px - a1));\n"
    "                n2 = float2(-d2.y, d2.x);\n"
    "                n2 *= sign(dot(n2, c_px - a2));\n"
    "                a3 = a1;\n"
    "                n3 = n1;\n"
    "            }\n"
    "        }\n"
    "        else if (join == 2)\n"
    "        {\n"
    "            /* Round join; at a hairpin the half disc past the corner, covered\n"
    "             * by the rectangle corner, edge points, tip. */\n"
    "            inner = hairpin ? pp : float2(0.0f, 0.0f);\n"
    "            e_p = hairpin ? pp + mh : pp;\n"
    "            e_n = hairpin ? pn : pn;\n"
    "            tip = hairpin ? pn + mh : mh * mlen;\n"
    "            marker = 3.0f;\n"
    "        }\n"
    "        else if (!hairpin)\n"
    "        {\n"
    "            bevel = join == 1 || (join == 3 && 1.0f / cosh > limit);\n"
    "            e_p = pp;\n"
    "            e_n = pn;\n"
    "            tip = mh * (bevel ? cosh : mlen);\n"
    "            a1 = c_px + mul(rt, pp * hw);\n"
    "            a2 = c_px + mul(rt, pn * hw);\n"
    "            d1 = normalize(mul(rt, q_prev));\n"
    "            d2 = normalize(mul(rt, q_next));\n"
    "            n1 = float2(-d1.y, d1.x);\n"
    "            n1 *= sign(dot(n1, c_px - a1));\n"
    "            n2 = float2(-d2.y, d2.x);\n"
    "            n2 *= sign(dot(n2, c_px - a2));\n"
    "            if (bevel)\n"
    "            {\n"
    "                d3 = normalize(a2 - a1);\n"
    "                a3 = a1;\n"
    "                n3 = float2(-d3.y, d3.x);\n"
    "                n3 *= sign(dot(n3, c_px - a3));\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                a3 = c_px + mul(rt, mh * hw * mlen);\n"
    "                n3 = -normalize(mul(rt, mh));\n"
    "            }\n"
    "        }\n"
    "        /* A miter or bevel join at a hairpin, and a flat cap, draw nothing. */\n"
    "\n"
    "        off = role == 1 ? e_p : role == 2 ? e_n : role == 3 ? tip : inner;\n"
    "        off_w = off * hw * (1.0f + expand);\n"
    "        v_px = mul(to_px, float3(pos_w + off_w, 1.0f));\n"
    "        if (marker == 3.0f)\n"
    "            o.b = float4(v_px - c_px, hw_px, 3.0f);\n"
    "        else\n"
    "            o.b = float4(dot(n1, v_px - a1), dot(n2, v_px - a2), dot(n3, v_px - a3), 4.0f);\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        /* A straight continuation is not tessellated; the quad collapses\n"
    "         * onto the point. */\n"
    "        o.b = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
    "    }\n"
    "\n"
    "    o.p = pos_w + off_w;\n"
    "    position = mul(to_px, float3(o.p, 1.0f)) * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_bezier_outline[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
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
    "    o.seam_t = 0.0f;\n"
    "    o.seam_c1 = 0.0f;\n"
    "    o.seam_c2 = 0.0f;\n"
    "    o.corner01 = 0.0f;\n"
    "    o.corner2 = 0.0f;\n"
    "    o.corner_on = 0.0f;\n"
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
    "\n"
    "    /* Widen the strip by 0.75 px beyond the stroke edge, like the straight\n"
    "     * outline does: the pixel shader ramps the edge over a pixel centred on\n"
    "     * it, and the half of the ramp outside the stroke needs pixels to run\n"
    "     * on. The curve coordinates are an affine map of p, so moving the\n"
    "     * vertex keeps them exact. */\n"
    "    float q_len = length(q_i);\n"
    "    float2 q_norm = q_len > 0.0001f ? q_i / q_len : float2(0.0f, 0.0f);\n"
    "    float hw_pixels = length(mul(rt, q_norm)) * transform_geometry_r1.w * 0.5f * q_len;\n"
    "    float expand = hw_pixels > 0.001f ? 0.75f / hw_pixels : 0.0f;\n"
    "    q_i *= 1.0f + expand;\n"
    "\n"
    "    /* What the ends of the piece meet sits in the length of next: 1 + the\n"
    "     * start + 4 * the end (0 a join, 1 the start cap, 2 the end cap, 3 the\n"
    "     * dash cap), plus 16 on the vertices at p0 and 32 on those at p2. An\n"
    "     * end that meets a flat cap is lengthened by 0.75 px along the tangent\n"
    "     * there, and the pixel shader gets the pixel distances to the lines\n"
    "     * across both ends (seam_t.xy) to ramp them; see the straight outline\n"
    "     * shader. The curve coordinates are an affine map of p, so the moved\n"
    "     * vertex keeps them exact. */\n"
    "    float2 t0, t2, along_w = 0.0f;\n"
    "    float big = 65536.0f, s0, s2, e_s, e_e;\n"
    "    uint code = (uint)round(length(next)) - 1, kind_s = code & 3, kind_e = (code >> 2) & 3, at = (code >> 4) & 3;\n"
    "    uint cap_s = (uint)(kind_s == 1 ? stroke_params.y : kind_s == 2 ? stroke_params.z : stroke_params.w);\n"
    "    uint cap_e = (uint)(kind_e == 1 ? stroke_params.y : kind_e == 2 ? stroke_params.z : stroke_params.w);\n"
    "    bool flat_s = kind_s != 0 && cap_s == 0, flat_e = kind_e != 0 && cap_e == 0;\n"
    "\n"
    "    t0 = dot(p1, p1) > 1e-12f ? normalize(p1) : normalize(p2);\n"
    "    t2 = dot(p2 - p1, p2 - p1) > 1e-12f ? normalize(p2 - p1) : t0;\n"
    "    s0 = max(length(mul(rt, t0)), 0.0001f);\n"
    "    s2 = max(length(mul(rt, t2)), 0.0001f);\n"
    "    e_s = flat_s ? 0.75f : 0.0f;\n"
    "    e_e = flat_e ? 0.75f : 0.0f;\n"
    "    if (at == 1)\n"
    "        along_w = -e_s / s0 * t0;\n"
    "    else if (at == 2)\n"
    "        along_w = e_e / s2 * t2;\n"
    "    p += 0.5f * transform_geometry_r1.w * q_i + along_w;\n"
    "    o.seam_t = float3(flat_s ? dot(mul(rt, p), mul(rt, t0)) / s0 : big,\n"
    "            flat_e ? dot(mul(rt, p2 - p), mul(rt, t2)) / s2 : big, 0.0f);\n"
    "\n"
    "    v_p = mul(rt, p2);\n"
    "    v_p = normalize(float2(-v_p.y, v_p.x));\n"
    "    /* A piece whose control point is within 1/16 px of its chord is drawn\n"
    "     * as that chord. The threshold used to be a whole pixel: a cubic split\n"
    "     * into pieces of 30 degrees or less, which is what a path in large\n"
    "     * units comes out as, then had every piece replaced by a chord lying\n"
    "     * up to half a pixel inside the curve, and the stroke of a circle moved\n"
    "     * inwards by a fifth of a pixel. */\n"
    "    if (abs(dot(mul(rt, p1), v_p)) < 0.0625f)\n"
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
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz)) + 0.5f * transform_geometry_r1.w * q_i + along_w;\n"
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
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
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
    "    /* Arc quadrants only occur in closed figures: no flat cap to ramp. */\n"
    "    o.seam_t = float3(65536.0f, 65536.0f, 0.0f);\n"
    "    o.seam_c1 = 0.0f;\n"
    "    o.seam_c2 = 0.0f;\n"
    "    o.corner01 = 0.0f;\n"
    "    o.corner2 = 0.0f;\n"
    "    o.corner_on = 0.0f;\n"
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
    "\n"
    "    /* Widen the strip by 0.75 px beyond the stroke edge; see the Bézier\n"
    "     * outline shader. */\n"
    "    float q_len = length(q_i);\n"
    "    float2 q_norm = q_len > 0.0001f ? q_i / q_len : float2(0.0f, 0.0f);\n"
    "    float hw_pixels = length(mul(rt, q_norm)) * transform_geometry_r1.w * 0.5f * q_len;\n"
    "    float expand = hw_pixels > 0.001f ? 0.75f / hw_pixels : 0.0f;\n"
    "    q_i *= 1.0f + expand;\n"
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
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, out struct output o)\n"
    "{\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "    o.b = float4(1.0, 0.0, 1.0, 1.0);\n"
    "    o.stroke_transform = float2x2(1.0, 0.0, 0.0, 1.0);\n"
    "    o.seam_t = 0.0f;\n"
    "    o.seam_c1 = 0.0f;\n"
    "    o.seam_c2 = 0.0f;\n"
    "    o.corner01 = 0.0f;\n"
    "    o.corner2 = 0.0f;\n"
    "    o.corner_on = 0.0f;\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_fill_aa[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float3 texcoord : TEXCOORD0,\n"
    "          float2 expand_dir : TEXCOORD1, float3 seam_t : TEXCOORD2,\n"
    "          float3 seam_c1 : TEXCOORD3, float3 seam_c2 : TEXCOORD4, float4 corner01 : TEXCOORD5,\n"
    "          float2 corner2 : TEXCOORD6, float3 corner_on : TEXCOORD7, out struct output o)\n"
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
    "    o.seam_t = seam_t;\n"
    "    o.seam_c1 = seam_c1;\n"
    "    o.seam_c2 = seam_c2;\n"
    "    o.corner01 = corner01;\n"
    "    o.corner2 = corner2;\n"
    "    o.corner_on = corner_on;\n"
    "    position = mul(float2x3(transform_rtx.xyz, transform_rty.xyz), float3(o.p, 1.0f))\n"
    "            * float2(transform_rtx.w, transform_rty.w);\n"
    "    o.position = float4(position + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n"
    "}\n";
static const char shape_vs_code_curve[] =
    "float4 transform_geometry_r0;\n"
    "float4 transform_geometry_r1;\n"
    "float4 transform_rtx;\n"
    "float4 transform_rty;\n"
    "float4 stroke_params;\n"
    "\n"
    "struct output\n"
    "{\n"
    "    float2 p : WORLD_POSITION;\n"
    "    float4 b : BEZIER;\n"
    "    nointerpolation float2x2 stroke_transform : STROKE_TRANSFORM;\n"
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
    "    float4 position : SV_POSITION;\n"
    "};\n"
    "\n"
    "void main(float2 position : POSITION, float3 texcoord : TEXCOORD0, out struct output o)\n"
    "{\n"
    "    o.p = float2(dot(float3(position, 1.0f), transform_geometry_r0.xyz), dot(float3(position, 1.0f), transform_geometry_r1.xyz));\n"
    "    o.b = float4(texcoord, 1.0);\n"
    "    o.stroke_transform = float2x2(1.0, 0.0, 0.0, 1.0);\n"
    "    o.seam_t = 0.0f;\n"
    "    o.seam_c1 = 0.0f;\n"
    "    o.seam_c2 = 0.0f;\n"
    "    o.corner01 = 0.0f;\n"
    "    o.corner2 = 0.0f;\n"
    "    o.corner_on = 0.0f;\n"
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
    "    float3 seam_t : SEAM_T;\n"
    "    nointerpolation float3 seam_c1 : SEAM_C1;\n"
    "    nointerpolation float3 seam_c2 : SEAM_C2;\n"
    "    float4 corner01 : SEAM_V01;\n"
    "    float2 corner2 : SEAM_V2;\n"
    "    nointerpolation float3 corner_on : SEAM_VON;\n"
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
    "/* The part of a pixel on the inner side of a straight edge, from the\n"
    " * signed distance d of the pixel centre to the edge (pixels, positive\n"
    " * inside) and the edge normal n in pixel space, of any length and either\n"
    " * direction. Along n the pixel projects onto +-(|nx| + |ny|) / 2, and\n"
    " * the density of its area there is the convolution of two boxes of\n"
    " * widths |nx| and |ny|: a trapezoid, flat at 1 / c between +-(c - s) / 2\n"
    " * and falling off linearly beyond, with c = max(|nx|, |ny|) and\n"
    " * s = min(|nx|, |ny|) for the unit normal. The area is its integral.\n"
    " * For an edge parallel to a pixel side (s = 0) this is the ramp\n"
    " * saturate(d + 0.5); for a 45 degree edge the pixel's corners reach\n"
    " * 1 / sqrt(2) and the area is 1 - (1 / sqrt(2) - d)^2 on the inner side,\n"
    " * below the ramp on both sides of the edge (0.34 against 0.38 at\n"
    " * d = -0.12, 0.985 against 1 at d = 0.59), so a 45 degree stroke of width\n"
    " * 4 drawn with the ramp covered 5.76 of its 5.66 per pixel row. A normal\n"
    " * of length 0 ramps as on the pixel grid. */\n"
    "float edge_coverage(float d, float2 n)\n"
    "{\n"
    "    float2 a = abs(n);\n"
    "    float c = max(a.x, a.y), s = min(a.x, a.y), l = length(a), inner, outer, t;\n"
    "\n"
    "    if (l < 1.0e-6f)\n"
    "    {\n"
    "        c = 1.0f;\n"
    "        s = 0.0f;\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        c /= l;\n"
    "        s /= l;\n"
    "    }\n"
    "    inner = 0.5f * (c - s);\n"
    "    outer = 0.5f * (c + s);\n"
    "    if (abs(d) <= inner)\n"
    "        return 0.5f + d / c;\n"
    "    t = outer - abs(d);\n"
    "    if (t <= 0.0f)\n"
    "        return d > 0.0f ? 1.0f : 0.0f;\n"
    "    t = t * t / max(2.0f * c * s, 1.0e-6f);\n"
    "    return d > 0.0f ? 1.0f - t : t;\n"
    "}\n"
    "\n"
    "/* The coverage near a corner of the outline that points inward, from c, the\n"
    " * pixel centre relative to the corner, and the code of the notch: the bits\n"
    " * of 2.0f plus the angles of the inward normals of its two edges in 4096ths\n"
    " * of a turn from -pi and above them the reach r, in eighths of a pixel up\n"
    " * to 4 and in halves beyond (d2d_fill_aa_notch_code()). It is 1 minus the\n"
    " * area of the notch, the part of the pixel outside both half planes. From the parts\n"
    " * u1 and u2 outside each, that is min(u1, u2) where the edges are on one\n"
    " * line, u1 * u2 at a right angle and max(u1 + u2 - 1, 0) where they run\n"
    " * back on each other, blended by the angle between the normals. The edges\n"
    " * follow their lines for r pixels from the corner; beyond that the bound\n"
    " * runs out as the plain corner bound does. */\n"
    "float notch_union(float2 c, float code, out float r)\n"
    "{\n"
    "    uint k = asuint(code) - 0x40000000u, q = k >> 24;\n"
    "    float a1 = (float)((k >> 12) & 4095u) * (6.28318531f / 4096.0f) - 3.14159265f;\n"
    "    float a2 = (float)(k & 4095u) * (6.28318531f / 4096.0f) - 3.14159265f;\n"
    "    r = q < 32u ? (float)q * 0.125f : 4.0f + (float)(q - 32u) * 0.5f;\n"
    "    float2 n1 = float2(cos(a1), sin(a1)), n2 = float2(cos(a2), sin(a2));\n"
    "    float u1 = 1.0f - edge_coverage(dot(c, n1), n1), u2 = 1.0f - edge_coverage(dot(c, n2), n2);\n"
    "    float prod = u1 * u2, t;\n"
    "\n"
    "    t = acos(clamp(dot(n1, n2), -1.0f, 1.0f)) * (2.0f / 3.14159265f);\n"
    "    if (t <= 1.0f)\n"
    "        return 1.0f - lerp(min(u1, u2), prod, t);\n"
    "    return 1.0f - lerp(prod, max(u1 + u2 - 1.0f, 0.0f), t - 1.0f);\n"
    "}\n"
    "\n"
    "float notch_coverage(float2 c, float code)\n"
    "{\n"
    "    float r, u = notch_union(c, code, r);\n"
    "\n"
    "    return max(u, edge_coverage(length(c) - r, c));\n"
    "}\n"
    "\n"
    "/* The coverage e of an outline edge of the face that ends at a notch, c_end\n"
    " * the pixel centre relative to that corner and c_other relative to the other\n"
    " * end of the edge: beyond the corner the edge bounds nothing, and up to a\n"
    " * quarter of a pixel from it the fill is the union of the half planes of the\n"
    " * notch, fading back to the edge over the next quarter. */\n"
    "float notch_edge(float e, float2 c_end, float2 c_other, float code)\n"
    "{\n"
    "    float r, u;\n"
    "\n"
    "    if (dot(c_end, c_other - c_end) > 0.0f)\n"
    "        return 1.0f;\n"
    "    u = notch_union(c_end, code, r);\n"
    "    return lerp(e, max(e, u), saturate((min(r, 0.25f) + 0.25f - length(c_end)) * 4.0f));\n"
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
    "        /* Every outline vertex layout boils down to one signed distance to\n"
    "         * the stroke edge in pixels, positive inside; the antialiasing ramp\n"
    "         * and the aliased clip are shared below.\n"
    "         *\n"
    "         * Straight segment (b.w = 1): b.x runs from -(1 + expand) to\n"
    "         * +(1 + expand) across the stroke, |b.x| = 1 is the edge.\n"
    "         * Round join or cap (b.w = 3): b.xy is the pixel offset from the\n"
    "         * corner, b.z the half width in pixels.\n"
    "         * Mitered, beveled or square join or cap (b.w = 4): b.xyz are\n"
    "         * the signed pixel distances to three bounding lines.\n"
    "         * Curves: the implicit form (u² - v = 0 for Béziers, u² + v² - 1 = 0\n"
    "         * for arcs) in texture space, with the screen-space partial\n"
    "         * derivatives to convert the distance to pixels,\n"
    "         * d(x, y) = |f(x, y)| / ‖∇f(x, y)‖, and the stroke's half width in\n"
    "         * the same units from the stroke transform.\n"
    "         *\n"
    "         * The antialiased coverage is the area of the pixel inside the\n"
    "         * edge (edge_coverage), which needs the edge normal in pixel\n"
    "         * space as well: the screen derivatives of the across coordinate\n"
    "         * or of a line distance, the offset from the centre of a round\n"
    "         * join or cap, the gradient of the implicit form of a curve. */\n"
    "        float2 du, dv, df, n_edge;\n"
    "        float4 uv = i.b;\n"
    "        float edge, cov;\n"
    "\n"
    "        if (uv.w == 1.0f)\n"
    "        {\n"
    "            n_edge = float2(ddx(uv.x), ddy(uv.x));\n"
    "            edge = (1.0f - abs(uv.x)) / max(length(n_edge), 0.0001f);\n"
    "            cov = edge_coverage(edge, n_edge);\n"
    "        }\n"
    "        else if (uv.w == 3.0f)\n"
    "        {\n"
    "            n_edge = uv.xy;\n"
    "            edge = uv.z - length(uv.xy);\n"
    "            cov = edge_coverage(edge, n_edge);\n"
    "        }\n"
    "        else if (uv.w == 4.0f)\n"
    "        {\n"
    "            /* The three distances are affine over the kite, so their\n"
    "             * screen derivatives are the unit normals of the three lines;\n"
    "             * each line's area bounds the area left from above. */\n"
    "            n_edge = float2(ddx(uv.x), ddy(uv.x));\n"
    "            edge = min(uv.x, min(uv.y, uv.z));\n"
    "            cov = min(edge_coverage(uv.x, n_edge),\n"
    "                    min(edge_coverage(uv.y, float2(ddx(uv.y), ddy(uv.y))),\n"
    "                    edge_coverage(uv.z, float2(ddx(uv.z), ddy(uv.z)))));\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            float side, val, grad_len, g, h, disc, d;\n"
    "\n"
    "            du = float2(ddx(uv.x), ddy(uv.x));\n"
    "            dv = float2(ddx(uv.y), ddy(uv.y));\n"
    "            if (!is_arc)\n"
    "            {\n"
    "                float f = uv.x * uv.x - uv.y;\n"
    "\n"
    "                df = 2.0f * uv.x * du - dv;\n"
    "                val = abs(f);\n"
    "                /* |f|/|∇f| is the distance to the parabola to first order\n"
    "                 * only. Along the gradient f has the constant second\n"
    "                 * derivative 2 (∇u · ĝ)², and f ± |∇f| d + ½ f'' d² = 0 solved\n"
    "                 * for d is exact to second order: a single quadratic over a\n"
    "                 * 90° arc, whose control triangle is far from orthogonal, drew\n"
    "                 * its stroke a twentieth of a pixel inside the ring with the\n"
    "                 * first-order estimate. The form below reduces to |f|/|∇f|\n"
    "                 * as f'' goes to zero. */\n"
    "                g = max(length(df), 0.0001f);\n"
    "                h = dot(du, df) / g;\n"
    "                h = 2.0f * h * h;\n"
    "                disc = g * g + (f > 0.0f ? -2.0f : 2.0f) * h * val;\n"
    "                d = disc > 0.0f ? 2.0f * val / (g + sqrt(disc)) : val / g;\n"
    "                val = d * g;\n"
    "            }\n"
    "            else\n"
    "            {\n"
    "                /* |f|/|∇f| with f = r² - 1 is (r - 1)(r + 1)/(2r): short of\n"
    "                 * the distance to the circle by r - 1 over 2 outside and\n"
    "                 * long by as much inside, which for a 3.4 px stroke on a\n"
    "                 * 13 px radius is 6 % of the half width, so the outer\n"
    "                 * edge came out too bright and the inner one too dark.\n"
    "                 * Scale f back to r - 1 in the same units. */\n"
    "                float r_uv = sqrt(uv.x * uv.x + uv.y * uv.y);\n"
    "                df = 2.0f * uv.x * du + 2.0f * uv.y * dv;\n"
    "                val = abs(uv.x * uv.x + uv.y * uv.y - 1.0f) * 2.0f * r_uv / max(r_uv + 1.0f, 0.0001f);\n"
    "            }\n"
    "            side = dot(df, uv.zw);\n"
    "            grad_len = length(mul(i.stroke_transform, df));\n"
    "            if (side < 0.0f)\n"
    "                discard;\n"
    "            n_edge = df;\n"
    "            edge = (grad_len - val) / max(length(df), 0.0001f);\n"
    "            cov = edge_coverage(edge, n_edge);\n"
    "        }\n"
    "\n"
    "        /* The ends of a piece that meet a flat cap: seam_t.xy are the pixel\n"
    "         * distances to the lines across the start and the end, 65536 where\n"
    "         * the end meets a join or another cap. A piece shorter than a pixel\n"
    "         * covers no more of a pixel than its length, and a pixel on the\n"
    "         * corner of a flat end is covered by the product of the two ramps,\n"
    "         * which is exact for a right angle on the pixel grid and what\n"
    "         * Windows draws at a 45 degree corner (0.19 against 0.38 for the\n"
    "         * lesser of the two). The line across an end is perpendicular to\n"
    "         * the side, so it crosses the pixel grid at the same angle and\n"
    "         * the side's normal serves for its area as well. */\n"
    "        float along = min(i.seam_t.x, i.seam_t.y);\n"
    "        if (aa_mode)\n"
    "        {\n"
    "            float aa = cov * edge_coverage(min(along, i.seam_t.x + i.seam_t.y - 0.5f), n_edge);\n"
    "            colour.a *= aa;\n"
    "            colour.rgb *= aa;\n"
    "        }\n"
    "        else\n"
    "        {\n"
    "            clip(min(edge, along));\n"
    "        }\n"
    "    }\n"
    "    else\n"
    "    {\n"
    "        /* Fill-edge AA: b.w==2.0 is an explicit marker for fill triangles with edge weights.\n"
    "         * b.xyz encode barycentric proximity to each triangle edge.\n"
    "         * min(b.xyz/gradient) = pixel distance to nearest boundary edge.\n"
    "         * Along a seam towards a curve triangle the curve lies off the edge, by\n"
    "         * seam_c1 * t + seam_c2 * t^2 in the same units, t running along the edge;\n"
    "         * adding that turns the distance to the edge into the distance to the curve. */\n"
    "        if (i.b.w > 1.5f && aa_mode)\n"
    "        {\n"
    "            float3 edges = i.b.xyz;\n"
    "            float3 t = saturate(i.seam_t);\n"
    "            float2 g_x = float2(ddx(edges.x), ddy(edges.x));\n"
    "            float2 g_y = float2(ddx(edges.y), ddy(edges.y));\n"
    "            float2 g_z = float2(ddx(edges.z), ddy(edges.z));\n"
    "            float3 grad_e = float3(length(g_x), length(g_y), length(g_z));\n"
    "            float3 edge_px = (edges + t * (i.seam_c1 + i.seam_c2 * t))\n"
    "                    / max(grad_e, float3(0.0001f, 0.0001f, 0.0001f));\n"
    "            /* The area inside each edge, from its pixel distance and its\n"
    "             * normal (the gradient of the weight); the nearest edge bounds\n"
    "             * the area left from above. */\n"
    "            float ea = edge_coverage(edge_px.x, g_x), eb = edge_coverage(edge_px.y, g_y);\n"
    "            float ec = edge_coverage(edge_px.z, g_z), fa = min(ea, min(eb, ec));\n"
    "            /* A triangle that touches the outline only at a corner knows none of\n"
    "             * the outline's edges. corner01/corner2 run from the corners to the\n"
    "             * pixel in geometry space; mapped to pixels through their derivatives,\n"
    "             * the distance to a corner on the outline bounds the distance to the\n"
    "             * outline from above, so it can only ever lighten the ramp. The\n"
    "             * distance to a corner grows along the direction to it, so it\n"
    "             * ramps as across a line through the corner normal to that\n"
    "             * direction. */\n"
    "            float2 jx = ddx(i.corner01.xy), jy = ddy(i.corner01.xy);\n"
    "            float det = jx.x * jy.y - jy.x * jx.y;\n"
    "            if (abs(det) > 1e-20f)\n"
    "            {\n"
    "                float2x2 to_px = float2x2(jy.y, -jy.x, -jx.y, jx.x) / det;\n"
    "                float2 c0 = mul(to_px, i.corner01.xy), c1 = mul(to_px, i.corner01.zw), c2 = mul(to_px, i.corner2);\n"
    "                float3 corner_px = float3(length(c0), length(c1), length(c2))\n"
    "                        + (1.0f - min(i.corner_on, 1.0f)) * 1.0e4f;\n"
    "                float bound0 = edge_coverage(corner_px.x, c0), bound1 = edge_coverage(corner_px.y, c1);\n"
    "                float bound2 = edge_coverage(corner_px.z, c2);\n"
    "\n"
    "                /* A corner that points inward carries the code of its notch\n"
    "                 * in corner_on, at least 2. */\n"
    "                if (i.corner_on.x > 1.5f)\n"
    "                    bound0 = notch_coverage(c0, i.corner_on.x);\n"
    "                if (i.corner_on.y > 1.5f)\n"
    "                    bound1 = notch_coverage(c1, i.corner_on.y);\n"
    "                if (i.corner_on.z > 1.5f)\n"
    "                    bound2 = notch_coverage(c2, i.corner_on.z);\n"
    "                /* A straight outline edge of the face (seam_c1 of -1) that ends\n"
    "                 * at a notch. */\n"
    "                if (i.seam_c1.x < -0.5f)\n"
    "                {\n"
    "                    if (i.corner_on.z > 1.5f)\n"
    "                        ea = notch_edge(ea, c2, c1, i.corner_on.z);\n"
    "                    if (i.corner_on.y > 1.5f)\n"
    "                        ea = notch_edge(ea, c1, c2, i.corner_on.y);\n"
    "                }\n"
    "                if (i.seam_c1.y < -0.5f)\n"
    "                {\n"
    "                    if (i.corner_on.x > 1.5f)\n"
    "                        eb = notch_edge(eb, c0, c2, i.corner_on.x);\n"
    "                    if (i.corner_on.z > 1.5f)\n"
    "                        eb = notch_edge(eb, c2, c0, i.corner_on.z);\n"
    "                }\n"
    "                if (i.seam_c1.z < -0.5f)\n"
    "                {\n"
    "                    if (i.corner_on.y > 1.5f)\n"
    "                        ec = notch_edge(ec, c1, c0, i.corner_on.y);\n"
    "                    if (i.corner_on.x > 1.5f)\n"
    "                        ec = notch_edge(ec, c0, c1, i.corner_on.x);\n"
    "                }\n"
    "                fa = min(min(ea, min(eb, ec)), min(bound0, min(bound1, bound2)));\n"
    "            }\n"
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
    "                float fa = edge_coverage(fval / max(fgrad, 0.0001f), fdf);\n"
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
    "                float fa = edge_coverage(fval / max(fgrad, 0.0001f), fdf);\n"
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
static const struct
{
    float x, y;
}
d2d_quad[] =
{
    {-1.0f,  1.0f},
    {-1.0f, -1.0f},
    { 1.0f,  1.0f},
    { 1.0f, -1.0f},
};
static const UINT32 d2d_quad_indices[] = {0, 1, 2, 2, 1, 3};

static HRESULT d2d_device_ensure_shape_resources(struct d2d_device *device, ID3D11Device1 *d3d_device)
{
    struct d2d_shape_resources resources[D2D_SHAPE_TYPE_COUNT] = {0};
    ID3D11Buffer *quad_ib = NULL, *quad_vb = NULL;
    D3D11_SUBRESOURCE_DATA buffer_data;
    D3D11_BUFFER_DESC buffer_desc;
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

    buffer_desc.ByteWidth = sizeof(d2d_quad_indices);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    buffer_desc.CPUAccessFlags = 0;
    buffer_desc.MiscFlags = 0;
    buffer_desc.StructureByteStride = 0;
    buffer_data.pSysMem = d2d_quad_indices;
    buffer_data.SysMemPitch = 0;
    buffer_data.SysMemSlicePitch = 0;
    if (FAILED(hr = ID3D11Device1_CreateBuffer(d3d_device, &buffer_desc, &buffer_data, &quad_ib)))
    {
        WARN("Failed to create clear index buffer, hr %#lx.\n", hr);
        goto fail;
    }

    buffer_desc.ByteWidth = sizeof(d2d_quad);
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_data.pSysMem = d2d_quad;
    if (FAILED(hr = ID3D11Device1_CreateBuffer(d3d_device, &buffer_desc, &buffer_data, &quad_vb)))
    {
        WARN("Failed to create clear vertex buffer, hr %#lx.\n", hr);
        goto fail;
    }

    for (i = 0; i < D2D_SHAPE_TYPE_COUNT; ++i)
        device->shape_resources[i] = resources[i];
    device->shape_ps = ps;
    device->quad_ib = quad_ib;
    device->quad_vb = quad_vb;
    device->shape_resources_ready = TRUE;
    LeaveCriticalSection(&device->shape_cs);
    return S_OK;

fail:
    if (quad_ib)
        ID3D11Buffer_Release(quad_ib);
    if (ps)
        ID3D11PixelShader_Release(ps);
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
    IDWriteFactory *dwrite_factory;
    D3D11_RASTERIZER_DESC rs_desc;
    D3D11_BUFFER_DESC buffer_desc;
    struct d2d_factory *factory;
    unsigned int i;
    HRESULT hr;

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
    {
        render_target->cs = &factory->cs;
        render_target->ID2D1DeviceContext6_iface.lpVtbl = &d2d_device_context_mt_vtbl;
    }

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

    render_target->ib = device->quad_ib;
    ID3D11Buffer_AddRef(render_target->ib);
    render_target->vb = device->quad_vb;
    ID3D11Buffer_AddRef(render_target->vb);
    render_target->vb_stride = sizeof(*d2d_quad);

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
        if (device->precompiled_effect_vs)
            ID3D10Blob_Release(device->precompiled_effect_vs);
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
            if (device->quad_vb)
                ID3D11Buffer_Release(device->quad_vb);
            if (device->quad_ib)
                ID3D11Buffer_Release(device->quad_ib);
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

    /* ENABLE_MULTITHREADED_OPTIMIZATIONS is a hint; rendering is not spread
     * over threads here, so there is nothing for it to switch on. */
    if (options & ~D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS)
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

/* Feeds a custom effect's pixel shader the way Direct2D does: one quad over
 * the effect's output rectangle, SCENE_POSITION in output pixels and one
 * TEXCOORD per input, normalised to that input's bitmap. */
static const char effect_vs_code[] =
    "cbuffer effect_cb : register(b0)\n"
    "{\n"
    "    float4 corners[4];   /* xy: clip space, zw: scene position */\n"
    "    float4 uv_rects[4];  /* per input: left, top, right, bottom */\n"
    "};\n"
    "\n"
    "struct vs_out\n"
    "{\n"
    "    float4 position : SV_POSITION;\n"
    "    float4 scene : SCENE_POSITION;\n"
    "    float4 uv0 : TEXCOORD0;\n"
    "    float4 uv1 : TEXCOORD1;\n"
    "    float4 uv2 : TEXCOORD2;\n"
    "    float4 uv3 : TEXCOORD3;\n"
    "};\n"
    "\n"
    "float4 input_uv(float4 rect, float2 t)\n"
    "{\n"
    "    return float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0.0f, 1.0f);\n"
    "}\n"
    "\n"
    "vs_out main(uint id : SV_VertexID)\n"
    "{\n"
    "    float2 t = float2(id & 1, id >> 1);\n"
    "    vs_out o;\n"
    "\n"
    "    o.position = float4(corners[id].xy, 0.0f, 1.0f);\n"
    "    o.scene = float4(corners[id].zw, 0.0f, 1.0f);\n"
    "    o.uv0 = input_uv(uv_rects[0], t);\n"
    "    o.uv1 = input_uv(uv_rects[1], t);\n"
    "    o.uv2 = input_uv(uv_rects[2], t);\n"
    "    o.uv3 = input_uv(uv_rects[3], t);\n"
    "    return o;\n"
    "}\n";

/* Compiling the shaders takes tens of milliseconds, and every render target created
 * from the factory (CreateWicBitmapRenderTarget(), CreateDCRenderTarget(),
 * CreateHwndRenderTarget()) creates a device of its own. Nuendo draws each icon of a
 * menu into a WIC bitmap render target of its own, and compiling the same shaders for
 * every one of them held up its "Add Track" submenu for more than a second. The
 * compiled code does not depend on the device, so compile it once per factory. */
static HRESULT d2d_factory_get_precompiled_shaders(struct d2d_factory *factory, struct d2d_device *device)
{
    ID3D10Blob *compiled;
    HRESULT hr = S_OK;
    unsigned int i;

    EnterCriticalSection(&factory->cs);

    for (i = 0; i < ARRAY_SIZE(shape_info); ++i)
    {
        const struct shape_info *si = &shape_info[i];

        if (factory->precompiled_shape_vs[i])
            continue;
        if (FAILED(hr = D3DCompile(si->vs_code, si->vs_code_size, si->name, NULL, NULL,
                "main", "vs_4_0", 0, 0, &compiled, NULL)))
        {
            WARN("Failed to compile shader for shape type %#x, hr %#lx.\n", si->shape_type, hr);
            goto done;
        }
        factory->precompiled_shape_vs[i] = compiled;
    }

    if (!factory->precompiled_shape_ps)
    {
        if (FAILED(hr = D3DCompile(shape_ps_code, sizeof(shape_ps_code) - 1, "ps", NULL, NULL,
                "main", "ps_4_0", 0, 0, &compiled, NULL)))
        {
            WARN("Failed to compile the pixel shader, hr %#lx.\n", hr);
            goto done;
        }
        factory->precompiled_shape_ps = compiled;
    }

    if (!factory->precompiled_effect_vs)
    {
        if (FAILED(hr = D3DCompile(effect_vs_code, sizeof(effect_vs_code) - 1, "effect_vs", NULL, NULL,
                "main", "vs_4_0", 0, 0, &compiled, NULL)))
        {
            WARN("Failed to compile the effect vertex shader, hr %#lx.\n", hr);
            goto done;
        }
        factory->precompiled_effect_vs = compiled;
    }

    for (i = 0; i < ARRAY_SIZE(shape_info); ++i)
    {
        device->precompiled_shape_vs[i] = factory->precompiled_shape_vs[i];
        ID3D10Blob_AddRef(device->precompiled_shape_vs[i]);
    }
    device->precompiled_shape_ps = factory->precompiled_shape_ps;
    ID3D10Blob_AddRef(device->precompiled_shape_ps);
    device->precompiled_effect_vs = factory->precompiled_effect_vs;
    ID3D10Blob_AddRef(device->precompiled_effect_vs);

done:
    LeaveCriticalSection(&factory->cs);
    return hr;
}

HRESULT d2d_device_init(struct d2d_device *device, ID2D1Factory1 *factory, IDXGIDevice *dxgi_device,
    bool allow_get_dxgi_device)
{
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

    return d2d_factory_get_precompiled_shaders(unsafe_impl_from_ID2D1Factory((ID2D1Factory *)factory), device);
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
