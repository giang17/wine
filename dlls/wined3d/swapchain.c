/*
 * Copyright 2002-2003 Jason Edmeades
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2007-2008 Stefan Dösinger for CodeWeavers
 * Copyright 2011 Henri Verbeet for CodeWeavers
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

#include "wined3d_private.h"
#include "wined3d_gl.h"
#include "wined3d_vk.h"
#include "wine/dcomp_layer.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);

static void wined3d_swapchain_set_layer_sink(struct wined3d_swapchain *swapchain, BOOL add);
WINE_DECLARE_DEBUG_CHANNEL(d3d_perf);

/* Route top-level window presents through glXSwapBuffers instead of the GDI
 * blit: the GDI path reads the whole backbuffer off the GPU into system
 * memory every present (~650 MB/s of display-server traffic and more than a
 * core during continuous UI activity in Ableton Live), and as a 60 Hz GDI
 * painter it fights every other painter sharing the top level's drawable —
 * the dcomp tree timer's WebView2 composite in particular (issue 121).
 * WINE_DISABLE_GL_PRESENT=1 restores the GDI path for every window; the
 * value is read, not just the presence, so =0 keeps the GL path.
 * Adopted from shibco/ableton-linux patch 0055 (diagnosis and measurements:
 * Lucas Gillingham/ClickSentinel, their issue 91). */
static BOOL wined3d_gl_present_disabled(void)
{
    static int disabled = -1;

    if (disabled < 0)
    {
        const char *e = getenv("WINE_DISABLE_GL_PRESENT");
        disabled = (e && e[0] != '0') ? 1 : 0;
    }
    return disabled > 0;
}

static BOOL set_window_present_rect(HWND hwnd, UINT x, UINT y, UINT width, UINT height)
{
    RECT rect = {x, y, x + width, y + height};
    D3DKMT_ESCAPE escape = {0};

    escape.Type = D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE;
    escape.hContext = HandleToULong(hwnd);
    escape.pPrivateDriverData = &rect;
    escape.PrivateDriverDataSize = sizeof(rect);

    return !D3DKMTEscape(&escape);
}

void wined3d_swapchain_cleanup(struct wined3d_swapchain *swapchain)
{
    HRESULT hr;
    UINT i;

    TRACE("Destroying swapchain %p.\n", swapchain);

    if (swapchain->comp_dc)
    {
        SelectObject(swapchain->comp_dc, swapchain->comp_old_bitmap);
        DeleteObject(swapchain->comp_bitmap);
        DeleteDC(swapchain->comp_dc);
        swapchain->comp_dc = NULL;
        swapchain->comp_bits = NULL;
    }

    /* Remove the composition props this swapchain set on its window during
     * Present (see SetPropW of __wine_dcomp_comp_dc/_size/_bits). The comp_dc
     * and bits they reference are freed above; leaving the props behind lets
     * the child-compositing path read a stale HDC/bits pointer after the
     * swapchain is gone. Only our own props are touched — __wine_dcomp_child_*
     * and __wine_dcomp_is_child are owned by dcomp/dxgi and removed there. */
    if (swapchain->win_handle)
    {
        RemovePropW(swapchain->win_handle, L"__wine_dcomp_comp_dc");
        RemovePropW(swapchain->win_handle, L"__wine_dcomp_comp_size");
        RemovePropW(swapchain->win_handle, L"__wine_dcomp_comp_bits");
    }

    /* The dcomp leaf layer (issue 206): count out of the sink so dcomp stops
     * publishing for a window nobody composites for any more, and drop the
     * texture.  The layer itself belongs to dcomp and is not ours to touch. */
    if (swapchain->layer_sink)
        wined3d_swapchain_set_layer_sink(swapchain, FALSE);
    if (swapchain->layer_texture)
    {
        wined3d_texture_decref(swapchain->layer_texture);
        swapchain->layer_texture = NULL;
        swapchain->layer_tex_width = swapchain->layer_tex_height = 0;
    }
    swapchain->layer = NULL;

    if (swapchain->surface_bits)
    {
        HeapFree(GetProcessHeap(), 0, swapchain->surface_bits);
        swapchain->surface_bits = NULL;
        swapchain->surface_width = 0;
        swapchain->surface_height = 0;
        swapchain->surface_valid = FALSE;
    }

    wined3d_swapchain_state_cleanup(&swapchain->state);
    wined3d_swapchain_set_gamma_ramp(swapchain, 0, &swapchain->orig_gamma);

    /* Release the swapchain's draw buffers. Make sure swapchain->back_buffers[0]
     * is the last buffer to be destroyed, FindContext() depends on that. */
    if (swapchain->front_buffer)
    {
        wined3d_texture_set_swapchain(swapchain->front_buffer, NULL);
        if (wined3d_texture_decref(swapchain->front_buffer))
            WARN("Something's still holding the front buffer (%p).\n", swapchain->front_buffer);
        swapchain->front_buffer = NULL;
    }

    if (swapchain->back_buffers)
    {
        i = swapchain->state.desc.backbuffer_count;

        while (i--)
        {
            wined3d_texture_set_swapchain(swapchain->back_buffers[i], NULL);
            if (wined3d_texture_decref(swapchain->back_buffers[i]))
                WARN("Something's still holding back buffer %u (%p).\n", i, swapchain->back_buffers[i]);
        }
        free(swapchain->back_buffers);
        swapchain->back_buffers = NULL;
    }

    /* Restore the screen resolution if we rendered in fullscreen.
     * This will restore the screen resolution to what it was before creating
     * the swapchain. In case of d3d8 and d3d9 this will be the original
     * desktop resolution. In case of d3d7 this will be a NOP because ddraw
     * sets the resolution before starting up Direct3D, thus orig_width and
     * orig_height will be equal to the modes in the presentation params. */
    if (!swapchain->state.desc.windowed)
    {
        if (swapchain->state.desc.auto_restore_display_mode)
        {
            if (FAILED(hr = wined3d_restore_display_modes(swapchain->device->wined3d)))
                ERR("Failed to restore display mode, hr %#lx.\n", hr);

            if (swapchain->state.desc.flags & WINED3D_SWAPCHAIN_RESTORE_WINDOW_RECT)
            {
                wined3d_swapchain_state_restore_from_fullscreen(&swapchain->state,
                        swapchain->state.device_window, &swapchain->state.original_window_rect);
                wined3d_device_release_focus_window(swapchain->device);
            }
        }
        else
        {
            wined3d_swapchain_state_restore_from_fullscreen(&swapchain->state, swapchain->state.device_window, NULL);
        }
    }
}

void wined3d_swapchain_gl_cleanup(struct wined3d_swapchain_gl *swapchain_gl)
{
    wined3d_swapchain_cleanup(&swapchain_gl->s);
}

static void wined3d_swapchain_vk_destroy_vulkan_swapchain(struct wined3d_swapchain_vk *swapchain_vk)
{
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_vk_info *vk_info;
    unsigned int i;
    VkResult vr;

    TRACE("swapchain_vk %p.\n", swapchain_vk);

    vk_info = &wined3d_adapter_vk(device_vk->d.adapter)->vk_info;

    if ((vr = VK_CALL(vkQueueWaitIdle(device_vk->graphics_queue.vk_queue))) < 0)
        ERR("Failed to wait on queue, vr %s.\n", wined3d_debug_vkresult(vr));
    free(swapchain_vk->vk_images);
    for (i = 0; i < swapchain_vk->image_count; ++i)
    {
        VK_CALL(vkDestroySemaphore(device_vk->vk_device, swapchain_vk->vk_semaphores[i].available, NULL));
        VK_CALL(vkDestroySemaphore(device_vk->vk_device, swapchain_vk->vk_semaphores[i].presentable, NULL));
    }
    free(swapchain_vk->vk_semaphores);
    VK_CALL(vkDestroySwapchainKHR(device_vk->vk_device, swapchain_vk->vk_swapchain, NULL));
    VK_CALL(vkDestroySurfaceKHR(vk_info->instance, swapchain_vk->vk_surface, NULL));
}

static void wined3d_swapchain_vk_destroy_object(void *object)
{
    wined3d_swapchain_vk_destroy_vulkan_swapchain(object);
}

void wined3d_swapchain_vk_cleanup(struct wined3d_swapchain_vk *swapchain_vk)
{
    struct wined3d_cs *cs = swapchain_vk->s.device->cs;

    wined3d_cs_destroy_object(cs, wined3d_swapchain_vk_destroy_object, swapchain_vk);
    wined3d_cs_finish(cs, WINED3D_CS_QUEUE_DEFAULT);

    wined3d_swapchain_cleanup(&swapchain_vk->s);
}

ULONG CDECL wined3d_swapchain_incref(struct wined3d_swapchain *swapchain)
{
    unsigned int refcount = InterlockedIncrement(&swapchain->ref);

    TRACE("%p increasing refcount to %u.\n", swapchain, refcount);

    return refcount;
}

ULONG CDECL wined3d_swapchain_decref(struct wined3d_swapchain *swapchain)
{
    unsigned int refcount = InterlockedDecrement(&swapchain->ref);

    TRACE("%p decreasing refcount to %u.\n", swapchain, refcount);

    if (!refcount)
    {
        struct wined3d_device *device;

        wined3d_mutex_lock();

        device = swapchain->device;
        if (device->swapchain_count && device->swapchains[0] == swapchain)
            wined3d_device_uninit_3d(device);
        wined3d_cs_finish(device->cs, WINED3D_CS_QUEUE_DEFAULT);

        if (swapchain->dc)
            wined3d_release_dc(swapchain->win_handle, swapchain->dc);

        CloseHandle(swapchain->frame_latency_semaphore);

        swapchain->parent_ops->wined3d_object_destroyed(swapchain->parent);
        swapchain->device->adapter->adapter_ops->adapter_destroy_swapchain(swapchain);

        wined3d_mutex_unlock();
    }

    return refcount;
}

void * CDECL wined3d_swapchain_get_parent(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->parent;
}

void CDECL wined3d_swapchain_set_window(struct wined3d_swapchain *swapchain, HWND window)
{
    if (!window)
        window = swapchain->state.device_window;
    if (window == swapchain->win_handle)
        return;

    TRACE("Setting swapchain %p window from %p to %p.\n",
            swapchain, swapchain->win_handle, window);

    wined3d_cs_finish(swapchain->device->cs, WINED3D_CS_QUEUE_DEFAULT);

    if (swapchain->dc)
        wined3d_release_dc(swapchain->win_handle, swapchain->dc);

    swapchain->win_handle = window;

    if (!(swapchain->dc = GetDCEx(swapchain->win_handle, 0, DCX_USESTYLE | DCX_CACHE)))
        WARN("Failed to retrieve device context, trying swapchain backup.\n");
}

void CDECL wined3d_swapchain_set_device_window(struct wined3d_swapchain *swapchain, HWND window)
{
    TRACE("Setting swapchain %p device window from %p to %p.\n",
            swapchain, swapchain->state.device_window, window);

    swapchain->state.device_window = window;
    swapchain->state.desc.device_window = window;
    wined3d_swapchain_set_window(swapchain, window);
}

/* Composition present-state setters, called from the DXGI/client thread.
 *
 * set_dirty_rects() writes the per-frame dirty-rect buffer that the present
 * path consumes on the CS thread.  To avoid the client/CS data race on that
 * buffer (a pipelined next-frame set_dirty_rects() overwriting rects while a
 * CS-thread present of the previous frame is still reading them), the rects are
 * handed off through the present CS op: emit snapshots present_dirty_rects[]
 * into the op (app thread, after this setter), and wined3d_cs_exec_present
 * copies the op snapshot into cs_present_dirty_rects[], which is the only buffer
 * the present path (swapchain_blit_gdi) reads.  present_dirty_rects[] is thus
 * touched only on the app thread, cs_present_dirty_rects[] only on the CS thread.
 *
 * set_force_gdi_present() / set_premultiplied_alpha() write state.desc.flags
 * bits that are also read on the CS-thread present path, but these are
 * configured once at swapchain/target creation (see dlls/dxgi/factory.c) before
 * any Present, never per frame, so they are not raced in practice and do not
 * need the per-frame handoff. */
void CDECL wined3d_swapchain_set_force_gdi_present(struct wined3d_swapchain *swapchain, BOOL force)
{
    TRACE("swapchain %p, force %d.\n", swapchain, force);

    if (force)
        swapchain->state.desc.flags |= WINED3D_SWAPCHAIN_FORCE_GDI_PRESENT;
    else
        swapchain->state.desc.flags &= ~WINED3D_SWAPCHAIN_FORCE_GDI_PRESENT;
}

void CDECL wined3d_swapchain_set_prefer_gl_present(struct wined3d_swapchain *swapchain, BOOL prefer)
{
    TRACE("swapchain %p, prefer %d.\n", swapchain, prefer);

    if (prefer)
        swapchain->state.desc.flags |= WINED3D_SWAPCHAIN_PREFER_GL_PRESENT;
    else
        swapchain->state.desc.flags &= ~WINED3D_SWAPCHAIN_PREFER_GL_PRESENT;
}

void CDECL wined3d_swapchain_set_premultiplied_alpha(struct wined3d_swapchain *swapchain, BOOL premultiplied)
{
    TRACE("swapchain %p, premultiplied %d.\n", swapchain, premultiplied);

    if (premultiplied)
        swapchain->state.desc.flags |= WINED3D_SWAPCHAIN_PREMULTIPLIED_ALPHA;
    else
        swapchain->state.desc.flags &= ~WINED3D_SWAPCHAIN_PREMULTIPLIED_ALPHA;
}

void CDECL wined3d_swapchain_set_dirty_rects(struct wined3d_swapchain *swapchain,
        const RECT *rects, unsigned int count)
{
    if (count > ARRAY_SIZE(swapchain->present_dirty_rects))
        count = ARRAY_SIZE(swapchain->present_dirty_rects);

    if (rects && count)
        memcpy(swapchain->present_dirty_rects, rects, count * sizeof(*rects));
    swapchain->present_dirty_rect_count = count;
}

HRESULT CDECL wined3d_swapchain_present(struct wined3d_swapchain *swapchain,
        const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override,
        unsigned int swap_interval, uint32_t flags)
{
    const struct wined3d_swapchain_desc *desc = &swapchain->state.desc;
    RECT s, d;

    TRACE("swapchain %p, src_rect %s, dst_rect %s, dst_window_override %p, swap_interval %u, flags %#x.\n",
            swapchain, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect),
            dst_window_override, swap_interval, flags);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

    if (!(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        /* Limit input latency by limiting the number of presents that we can
         * get ahead of the worker thread. Avoid holding the D3D mutex across
         * to not block other threads. */
        WaitForSingleObject(swapchain->frame_latency_semaphore, INFINITE);
    }

    wined3d_mutex_lock();

    if (!swapchain->back_buffers)
    {
        WARN("Swapchain doesn't have a backbuffer, returning WINED3DERR_INVALIDCALL.\n");
        wined3d_mutex_unlock();
        return WINED3DERR_INVALIDCALL;
    }

    if (!src_rect)
    {
        SetRect(&s, 0, 0, desc->backbuffer_width, desc->backbuffer_height);
        src_rect = &s;
    }

    if (!dst_rect)
    {
        if (!desc->windowed)
            SetRect(&d, 0, 0, desc->backbuffer_width, desc->backbuffer_height);
        else
            GetClientRect(swapchain->win_handle, &d);
        dst_rect = &d;
    }

    wined3d_cs_emit_present(swapchain->device->cs, swapchain, src_rect,
            dst_rect, dst_window_override, swap_interval, flags);

    wined3d_mutex_unlock();

    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_get_front_buffer_data(const struct wined3d_swapchain *swapchain,
        struct wined3d_texture *dst_texture, unsigned int sub_resource_idx)
{
    RECT src_rect, dst_rect;

    TRACE("swapchain %p, dst_texture %p, sub_resource_idx %u.\n", swapchain, dst_texture, sub_resource_idx);

    SetRect(&src_rect, 0, 0, swapchain->front_buffer->resource.width, swapchain->front_buffer->resource.height);
    dst_rect = src_rect;

    if (swapchain->state.desc.windowed)
    {
        MapWindowPoints(swapchain->win_handle, NULL, (POINT *)&dst_rect, 2);
        FIXME("Using destination rect %s in windowed mode, this is likely wrong.\n",
                wine_dbgstr_rect(&dst_rect));
    }

    return wined3d_device_context_blt(&swapchain->device->cs->c, dst_texture, sub_resource_idx, &dst_rect,
            swapchain->front_buffer, 0, &src_rect, 0, NULL, WINED3D_TEXF_POINT);
}

struct wined3d_texture * CDECL wined3d_swapchain_get_back_buffer(const struct wined3d_swapchain *swapchain,
        UINT back_buffer_idx)
{
    TRACE("swapchain %p, back_buffer_idx %u.\n",
            swapchain, back_buffer_idx);

    /* Return invalid if there is no backbuffer array, otherwise it will
     * crash when ddraw is used (there swapchain->back_buffers is always
     * NULL). We need this because this function is called from
     * stateblock_init_default_state() to get the default scissorrect
     * dimensions. */
    if (!swapchain->back_buffers || back_buffer_idx >= swapchain->state.desc.backbuffer_count)
    {
        WARN("Invalid back buffer index.\n");
        /* Native d3d9 doesn't set NULL here, just as wine's d3d9. But set it
         * here in wined3d to avoid problems in other libs. */
        return NULL;
    }

    TRACE("Returning back buffer %p.\n", swapchain->back_buffers[back_buffer_idx]);

    return swapchain->back_buffers[back_buffer_idx];
}

struct wined3d_texture * CDECL wined3d_swapchain_get_front_buffer(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->front_buffer;
}

struct wined3d_output * wined3d_swapchain_get_output(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->state.desc.output;
}

HRESULT CDECL wined3d_swapchain_get_raster_status(const struct wined3d_swapchain *swapchain,
        struct wined3d_raster_status *raster_status)
{
    struct wined3d_output *output;

    TRACE("swapchain %p, raster_status %p.\n", swapchain, raster_status);

    output = wined3d_swapchain_get_output(swapchain);
    if (!output)
    {
        ERR("Failed to get output from swapchain %p.\n", swapchain);
        return E_FAIL;
    }

    return wined3d_output_get_raster_status(output, raster_status);
}

struct wined3d_swapchain_state * CDECL wined3d_swapchain_get_state(struct wined3d_swapchain *swapchain)
{
    return &swapchain->state;
}

HRESULT CDECL wined3d_swapchain_get_display_mode(const struct wined3d_swapchain *swapchain,
        struct wined3d_display_mode *mode, enum wined3d_display_rotation *rotation)
{
    struct wined3d_output *output;
    HRESULT hr;

    TRACE("swapchain %p, mode %p, rotation %p.\n", swapchain, mode, rotation);

    if (!(output = wined3d_swapchain_get_output(swapchain)))
    {
        ERR("Failed to get output from swapchain %p.\n", swapchain);
        return E_FAIL;
    }

    hr = wined3d_output_get_display_mode(output, mode, rotation);

    TRACE("Returning w %u, h %u, refresh rate %u, format %s.\n",
            mode->width, mode->height, mode->refresh_rate, debug_d3dformat(mode->format_id));

    return hr;
}

struct wined3d_device * CDECL wined3d_swapchain_get_device(const struct wined3d_swapchain *swapchain)
{
    TRACE("swapchain %p.\n", swapchain);

    return swapchain->device;
}

void CDECL wined3d_swapchain_get_desc(const struct wined3d_swapchain *swapchain,
        struct wined3d_swapchain_desc *desc)
{
    TRACE("swapchain %p, desc %p.\n", swapchain, desc);

    *desc = swapchain->state.desc;
}

HRESULT CDECL wined3d_swapchain_set_gamma_ramp(const struct wined3d_swapchain *swapchain,
        uint32_t flags, const struct wined3d_gamma_ramp *ramp)
{
    struct wined3d_output *output;

    TRACE("swapchain %p, flags %#x, ramp %p.\n", swapchain, flags, ramp);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

    if (!(output = wined3d_swapchain_get_output(swapchain)))
    {
        ERR("Failed to get output from swapchain %p.\n", swapchain);
        return E_FAIL;
    }

    return wined3d_output_set_gamma_ramp(output, ramp);
}

void CDECL wined3d_swapchain_set_palette(struct wined3d_swapchain *swapchain, struct wined3d_palette *palette)
{
    TRACE("swapchain %p, palette %p.\n", swapchain, palette);

    wined3d_cs_finish(swapchain->device->cs, WINED3D_CS_QUEUE_DEFAULT);

    swapchain->palette = palette;
}

HRESULT CDECL wined3d_swapchain_get_gamma_ramp(const struct wined3d_swapchain *swapchain,
        struct wined3d_gamma_ramp *ramp)
{
    struct wined3d_output *output;

    TRACE("swapchain %p, ramp %p.\n", swapchain, ramp);

    if (!(output = wined3d_swapchain_get_output(swapchain)))
    {
        ERR("Failed to get output from swapchain %p.\n", swapchain);
        return E_FAIL;
    }

    return wined3d_output_get_gamma_ramp(output, ramp);
}

/* The is a fallback for cases where we e.g. can't create a GL context or
 * Vulkan swapchain for the swapchain window. */

/* Merge pixels from src into dst, copying only where source alpha is non-zero.
 * This preserves existing composition buffer content under transparent areas
 * left by Clear(transparent) + partial redraw (JUCE/Serum2 pattern).
 * Used in the "no-dirty full blit" path for SEQUENTIAL swapchains where the
 * app calls Present() without Present1() dirty rects. */
static void comp_buffer_alpha_merge(DWORD *dst_bits, unsigned int dst_pitch,
        const DWORD *src_bits, unsigned int src_pitch,
        int width, int height)
{
    int x, y;

    for (y = 0; y < height; ++y)
    {
        const DWORD *src_row = (const DWORD *)((const char *)src_bits
                + (unsigned int)y * src_pitch);
        DWORD *dst_row = (DWORD *)((char *)dst_bits
                + (unsigned int)y * dst_pitch);

        for (x = 0; x < width; ++x)
        {
            if (src_row[x] & 0xff000000)
                dst_row[x] = src_row[x];
        }
    }
}

/* Copy pixels from src to dst, but only where source alpha is non-zero.
 * This preserves existing composition buffer content under transparent
 * areas of the back buffer (after Clear(transparent) + partial redraw).
 * DComp plugins rely on the compositor preserving unmodified regions. */
static void comp_buffer_alpha_copy(DWORD *dst_bits, unsigned int dst_pitch,
        const DWORD *src_bits, unsigned int src_pitch,
        int dst_x, int dst_y, int src_x, int src_y,
        int width, int height)
{
    /* Copy all pixels from backbuffer to comp buffer, including transparent
     * ones. The old code skipped alpha==0 pixels to preserve comp buffer
     * content, but this caused stale content because Clear(transparent) +
     * partial redraw left transparent pixels that never overwrote the old
     * comp buffer. */
    int y;

    for (y = 0; y < height; ++y)
    {
        const DWORD *src_row = (const DWORD *)((const char *)src_bits
                + (unsigned int)(src_y + y) * src_pitch) + src_x;
        DWORD *dst_row = (DWORD *)((char *)dst_bits
                + (unsigned int)(dst_y + y) * dst_pitch) + dst_x;

        memcpy(dst_row, src_row, width * sizeof(DWORD));
    }
}

/* Porter-Duff Over: C_out = C_src + C_dst * (1 - alpha_src)
 * Both src and dst are premultiplied alpha BGRA. */
static void composite_over_premul(DWORD *dst, const DWORD *src,
        unsigned int dst_stride, unsigned int src_stride,
        int dst_x, int dst_y, int src_w, int src_h,
        int dst_total_w, int dst_total_h)
{
    int y, x;
    int x_end = dst_x + src_w;
    int y_end = dst_y + src_h;

    /* Clip to destination bounds */
    int sx_start = 0, sy_start = 0;
    if (dst_x < 0) { sx_start = -dst_x; dst_x = 0; }
    if (dst_y < 0) { sy_start = -dst_y; dst_y = 0; }
    if (x_end > dst_total_w) x_end = dst_total_w;
    if (y_end > dst_total_h) y_end = dst_total_h;

    for (y = dst_y; y < y_end; ++y)
    {
        DWORD *dp = dst + y * dst_stride + dst_x;
        const DWORD *sp = src + (sy_start + y - dst_y) * src_stride + sx_start;
        int width = x_end - dst_x;

        for (x = 0; x < width; ++x)
        {
            DWORD s = sp[x];
            unsigned int sa = (s >> 24) & 0xff;

            if (sa == 0)
                continue;  /* Fully transparent — skip */

            if (sa == 255)
            {
                dp[x] = s;  /* Fully opaque — overwrite */
                continue;
            }

            /* Blend: premultiplied over */
            {
                DWORD d = dp[x];
                unsigned int inv_sa = 255 - sa;
                unsigned int rb = ((d & 0x00ff00ff) * inv_sa + 128);
                unsigned int g  = ((d & 0x0000ff00) * inv_sa + 128);
                unsigned int da = ((d >> 24) * inv_sa + 128);

                rb = (rb + ((rb >> 8) & 0x00ff00ff)) >> 8;
                g  = (g  + ((g  >> 8) & 0x0000ff00)) >> 8;
                da = (da + (da >> 8)) >> 8;

                dp[x] = (s & 0x00ff00ff) + (rb & 0x00ff00ff)
                      + ((s & 0x0000ff00) + (g & 0x0000ff00))
                      + (((sa + da) > 255 ? 255 : (sa + da)) << 24);
            }
        }
    }
}

/* Must match DCOMP_MAX_SERIALIZED_LEAVES in dlls/dcomp/device.c -- the two are
 * separate copies of the same contract, and nothing checks that they agree. */
#define WINED3D_DCOMP_MAX_LEAVES 16

/* Composite child visuals onto root's comp buffer.
 * Reads child info from window properties set by dcomp Commit(). */
static void swapchain_composite_children(struct wined3d_swapchain *swapchain,
        unsigned int dst_w, unsigned int dst_h)
{
    ULONG_PTR child_count_val;
    unsigned int child_count, i;
    WCHAR prop_name[64];

    if (!swapchain->comp_bits)
        return;

    child_count_val = (ULONG_PTR)GetPropW(swapchain->win_handle,
            L"__wine_dcomp_child_count");
    child_count = (unsigned int)child_count_val;
    if (!child_count)
        return;

    if (child_count > WINED3D_DCOMP_MAX_LEAVES)
    {
        /* dcomp caps at the same number, so this means the two halves disagree
         * -- a half-deployed build.  The leaves past the limit are dropped. */
        static unsigned int over_count;

        if (++over_count <= 5 || !(over_count % 200))
            FIXME("Leaf count %u exceeds the reader limit of %u on %p #%u: dropping %u leaves.\n",
                    child_count, WINED3D_DCOMP_MAX_LEAVES, swapchain->win_handle,
                    over_count, child_count - WINED3D_DCOMP_MAX_LEAVES);
    }

    for (i = 0; i < child_count && i < WINED3D_DCOMP_MAX_LEAVES; ++i)
    {
        HWND child_wnd;
        DWORD *child_bits;
        LPARAM child_dims, child_offset;
        unsigned int cw, ch;
        int ox, oy;

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_wnd", i);
        child_wnd = (HWND)GetPropW(swapchain->win_handle, prop_name);
        if (!child_wnd)
        {
            /* dcomp serializes DComp surfaces and composition textures with a
             * null window handle and their bits in __wine_dcomp_child_%u_bits.
             * This continue drops them before the direct-bits branch below can
             * read them, which is why that branch has never run since it was
             * added (e99905af0dd, 31.03.2026).  The leaf is simply absent from
             * the frame -- no error anywhere, and the search starts in dcomp,
             * where everything looks right. */
            static unsigned int no_wnd_count;

            if (++no_wnd_count <= 5 || !(no_wnd_count % 200))
                FIXME("Leaf #%u dropped on %p (report #%u): no window handle, so it is a "
                        "DComp surface or composition texture; the direct-bits path is "
                        "unreachable.\n", i, swapchain->win_handle, no_wnd_count);
            continue;
        }

        /* Two paths: swapchain children have a window with comp_bits,
         * surface children have direct bits stored as properties on target. */
        if (child_wnd)
        {
            child_bits = (DWORD *)GetPropW(child_wnd, L"__wine_dcomp_comp_bits");
            child_dims = (LPARAM)GetPropW(child_wnd, L"__wine_dcomp_comp_size");
        }
        else
        {
            /* Direct-bits path for DComp surface children */
            swprintf(prop_name, ARRAY_SIZE(prop_name),
                    L"__wine_dcomp_child_%u_bits", i);
            child_bits = (DWORD *)GetPropW(swapchain->win_handle, prop_name);
            swprintf(prop_name, ARRAY_SIZE(prop_name),
                    L"__wine_dcomp_child_%u_size", i);
            child_dims = (LPARAM)GetPropW(swapchain->win_handle, prop_name);
        }
        if (!child_bits || !child_dims)
        {
            /* The window is there but carries no composite buffer yet -- the
             * child swapchain has not presented once.  Persisting past the first
             * frames means the leaf never arrives. */
            static unsigned int no_bits_count;

            if (++no_bits_count <= 5 || !(no_bits_count % 200))
                FIXME("Leaf #%u dropped on %p (report #%u): child window %p has bits=%p "
                        "dims=%#Ix.\n", i, swapchain->win_handle, no_bits_count,
                        child_wnd, child_bits, (ULONG_PTR)child_dims);
            continue;
        }

        cw = LOWORD(child_dims);
        ch = HIWORD(child_dims);

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_offset", i);
        child_offset = (LPARAM)GetPropW(swapchain->win_handle, prop_name);
        ox = (short)LOWORD(child_offset);
        oy = (short)HIWORD(child_offset);

        {
            static unsigned int comp_log_count;
            ++comp_log_count;
            if (comp_log_count <= 10 || !(comp_log_count % 500))
                TRACE("Composite child #%u: wnd=%p bits=%p %ux%u at (%d,%d) onto %ux%u.\n",
                        i, child_wnd, child_bits, cw, ch, ox, oy, dst_w, dst_h);
        }

        /* Flush GDI operations on child's comp_dc before reading bits */
        if (child_wnd)
        {
            HDC child_dc = (HDC)GetPropW(child_wnd, L"__wine_dcomp_comp_dc");
            if (child_dc)
                GdiFlush();
        }

        composite_over_premul(swapchain->comp_bits, child_bits,
                dst_w, cw,
                ox, oy, cw, ch,
                dst_w, dst_h);
    }
}

/* --- The published dcomp leaf layer, drawn into the frame (issue 206) -------
 *
 * A DirectComposition tree covering only a sliver of its window delivers just
 * the rectangles it covers and leaves the window to the application.  Those
 * pixels used to be blitted onto the window after the present that produced the
 * frame -- two writers, no ordering, and every swap landing between two
 * deliveries showed a window without them (48.2% of frames on Fender Studio
 * Pro 8; 0.2% with this).
 *
 * dcomp publishes them as a layer with alpha instead and we draw them into the
 * frame we are about to present.  See include/wine/dcomp_layer.h for the
 * contract, the locking and the lifetime rules.
 *
 * We tell dcomp that we are here by counting ourselves into the sink property:
 * without a sink dcomp keeps blitting, which is what has to happen for a window
 * this swapchain does not composite for -- a Vulkan swapchain, say, whose
 * present path has no composite step at all. */
static void wined3d_swapchain_set_layer_sink(struct wined3d_swapchain *swapchain, BOOL add)
{
    ULONG_PTR count;

    if (!swapchain->win_handle)
        return;

    /* Counted, because more than one swapchain may present to the same window
     * and the last one out has to be the one that clears it. */
    count = (ULONG_PTR)GetPropW(swapchain->win_handle, WINE_DCOMP_LAYER_SINK_PROP);
    if (add)
        ++count;
    else if (count)
        --count;

    if (count)
        SetPropW(swapchain->win_handle, WINE_DCOMP_LAYER_SINK_PROP, (HANDLE)count);
    else
        RemovePropW(swapchain->win_handle, WINE_DCOMP_LAYER_SINK_PROP);
    swapchain->layer_sink = add;

    TRACE("swapchain %p, hwnd %p: layer sink %s, count %Iu.\n",
            swapchain, swapchain->win_handle, add ? "added" : "removed", count);
}

/* Presents between two lookups while none has succeeded.  GetPropW is a
 * wineserver round trip -- NtUserGetProp is a server request, not a read of
 * anything local -- and the GL present path makes none at all otherwise.  One
 * per present would be a thousand round trips a second in an application
 * presenting at a thousand frames, for a window that will never have a
 * composition target.  Every eighth present is an eighth of that, and delays
 * picking up a target that appears later by at most eight frames. */
#define WINED3D_LAYER_LOOKUP_INTERVAL 8

/* The layer of this swapchain's window, or NULL.  Looked up at most once:
 * dcomp never frees the structure and never removes the property, so a
 * successful lookup holds for the life of the swapchain. */
static struct wine_dcomp_layer *swapchain_poll_layer(struct wined3d_swapchain *swapchain)
{
    if (!swapchain->layer_sink)
        return NULL;
    if (!swapchain->layer)
    {
        if (swapchain->layer_lookup)
        {
            --swapchain->layer_lookup;
            return NULL;
        }
        swapchain->layer_lookup = WINED3D_LAYER_LOOKUP_INTERVAL - 1;
        swapchain->layer = (struct wine_dcomp_layer *)GetPropW(swapchain->win_handle,
                WINE_DCOMP_LAYER_PROP);
    }
    return swapchain->layer;
}

/* Take the shared lock and clip the published rectangles to everything they
 * have to fit into.  Returns 0 with the lock released when there is nothing to
 * draw; otherwise the caller owns the shared lock and must release it.
 *
 * The clip is what this present actually writes: a swapchain presenting to a
 * part of its window must not put leaves outside it, and two swapchains on one
 * window then each draw the layer into their own frame -- which is what they
 * should do, since either frame is a whole frame. */
static unsigned int swapchain_layer_lock_rects(struct wined3d_swapchain *swapchain,
        struct wine_dcomp_layer **layer, RECT *rects, const RECT *clip)
{
    struct wine_dcomp_layer *l;
    unsigned int i, n = 0;
    RECT extent, c;

    /* Resolved by swapchain_poll_layer() earlier in this present. */
    if (!(l = swapchain->layer))
        return 0;

    AcquireSRWLockShared(&l->lock);
    if (l->bits && l->rect_count)
    {
        SetRect(&extent, 0, 0, l->width, l->height);
        IntersectRect(&extent, &extent, clip);
        for (i = 0; i < l->rect_count && i < WINE_DCOMP_LAYER_MAX_RECTS; ++i)
            if (IntersectRect(&c, &l->rects[i], &extent))
                rects[n++] = c;
    }
    if (!n)
    {
        ReleaseSRWLockShared(&l->lock);
        return 0;
    }
    *layer = l;
    return n;
}

/* CPU side, for the GDI present branch: composite the layer into the buffer
 * that is about to go to the window. */
static BOOL swapchain_composite_layer(struct wined3d_swapchain *swapchain, DWORD *dst,
        unsigned int dst_stride_dwords, unsigned int dst_w, unsigned int dst_h)
{
    RECT rects[WINE_DCOMP_LAYER_MAX_RECTS], clip;
    struct wine_dcomp_layer *layer;
    unsigned int i, n;

    if (!dst || !swapchain_poll_layer(swapchain))
        return FALSE;

    SetRect(&clip, 0, 0, dst_w, dst_h);
    if (!(n = swapchain_layer_lock_rects(swapchain, &layer, rects, &clip)))
        return FALSE;

    for (i = 0; i < n; ++i)
        composite_over_premul(dst, layer->bits + (SIZE_T)rects[i].top * layer->width + rects[i].left,
                dst_stride_dwords, layer->width, rects[i].left, rects[i].top,
                rects[i].right - rects[i].left, rects[i].bottom - rects[i].top, dst_w, dst_h);
    ReleaseSRWLockShared(&layer->lock);
    InterlockedIncrement(&layer->drawn);

    TRACE("Composited %u layer rect(s) into a %ux%u buffer for hwnd %p.\n",
            n, dst_w, dst_h, swapchain->win_handle);
    return TRUE;
}

static void swapchain_blit_gdi(struct wined3d_swapchain *swapchain,
        struct wined3d_context *context, const RECT *src_rect, const RECT *dst_rect)
{
    struct wined3d_texture *back_buffer = swapchain->back_buffers[0];
    D3DKMT_DESTROYDCFROMMEMORY destroy_desc;
    D3DKMT_CREATEDCFROMMEMORY create_desc;
    const struct wined3d_format *format;
    unsigned int row_pitch, slice_pitch;
    BOOL composited_layer = FALSE;
    NTSTATUS status;
    HBITMAP bitmap;
    HDC src_dc;

    static unsigned int once;

    TRACE("swapchain %p, context %p, src_rect %s, dst_rect %s.\n",
            swapchain, context, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect));

    if (!once++)
        FIXME("Using GDI present.\n");


    format = back_buffer->resource.format;
    if (!format->ddi_format)
    {
        WARN("Cannot create a DC for format %s.\n", debug_d3dformat(format->id));
        return;
    }

    wined3d_texture_load_location(back_buffer, 0, context, WINED3D_LOCATION_SYSMEM);
    wined3d_texture_get_pitch(back_buffer, 0, &row_pitch, &slice_pitch);

    create_desc.pMemory = back_buffer->resource.heap_memory;
    create_desc.Format = format->ddi_format;
    create_desc.Width = wined3d_texture_get_level_width(back_buffer, 0);
    create_desc.Height = wined3d_texture_get_level_height(back_buffer, 0);
    create_desc.Pitch = row_pitch;
    create_desc.hDeviceDc = CreateCompatibleDC(NULL);
    create_desc.pColorTable = NULL;

    status = D3DKMTCreateDCFromMemory(&create_desc);
    DeleteDC(create_desc.hDeviceDc);
    if (status)
    {
        WARN("Failed to create DC, status %#lx.\n", status);
        return;
    }

    src_dc = create_desc.hDc;
    bitmap = create_desc.hBitmap;

    TRACE("Created source DC %p, bitmap %p for backbuffer %p.\n", src_dc, bitmap, back_buffer);

    if ((swapchain->state.desc.flags & WINED3D_SWAPCHAIN_PREMULTIPLIED_ALPHA)
            || swapchain->state.desc.swap_effect == WINED3D_SWAP_EFFECT_FLIP_SEQUENTIAL
            || swapchain->state.desc.swap_effect == WINED3D_SWAP_EFFECT_SEQUENTIAL)
    {
        unsigned int dst_w = dst_rect->right - dst_rect->left;
        unsigned int dst_h = dst_rect->bottom - dst_rect->top;
        unsigned int src_w = src_rect->right - src_rect->left;
        unsigned int src_h = src_rect->bottom - src_rect->top;

        /* Ensure persistent composition buffer exists and is the right size. */
        if (!swapchain->comp_dc || swapchain->comp_width != dst_w
                || swapchain->comp_height != dst_h
                || swapchain->last_blit_window != swapchain->win_handle)
        {
            BITMAPINFO bmi;

            if (swapchain->comp_dc)
            {
                SelectObject(swapchain->comp_dc, swapchain->comp_old_bitmap);
                DeleteObject(swapchain->comp_bitmap);
                DeleteDC(swapchain->comp_dc);
            }

            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = dst_w;
            bmi.bmiHeader.biHeight = -(int)dst_h; /* top-down */
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            if (!(swapchain->comp_dc = CreateCompatibleDC(swapchain->dc)))
            {
                WARN("Failed to create composition DC.\n");
                swapchain->comp_bits = NULL;
                goto done;
            }
            if (!(swapchain->comp_bitmap = CreateDIBSection(swapchain->comp_dc, &bmi,
                    DIB_RGB_COLORS, (void **)&swapchain->comp_bits, NULL, 0)))
            {
                WARN("Failed to create composition DIB section.\n");
                DeleteDC(swapchain->comp_dc);
                swapchain->comp_dc = NULL;
                swapchain->comp_bits = NULL;
                goto done;
            }
            if (!(swapchain->comp_old_bitmap = SelectObject(swapchain->comp_dc, swapchain->comp_bitmap)))
            {
                WARN("Failed to select composition bitmap into DC.\n");
                DeleteObject(swapchain->comp_bitmap);
                swapchain->comp_bitmap = NULL;
                DeleteDC(swapchain->comp_dc);
                swapchain->comp_dc = NULL;
                swapchain->comp_bits = NULL;
                goto done;
            }
            swapchain->comp_width = dst_w;
            swapchain->comp_height = dst_h;
            swapchain->last_blit_window = swapchain->win_handle;

            /* Allocate/resize per-visual surface buffer BEFORE copying into it.
             * Must happen before the memcpy below to avoid buffer overflow when
             * the new comp buffer is larger than the old surface_bits allocation. */
            if (!swapchain->surface_bits || swapchain->surface_width != dst_w
                    || swapchain->surface_height != dst_h)
            {
                HeapFree(GetProcessHeap(), 0, swapchain->surface_bits);
                swapchain->surface_bits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                        dst_w * dst_h * sizeof(DWORD));
                swapchain->surface_width = dst_w;
                swapchain->surface_height = dst_h;
                swapchain->surface_valid = FALSE;
            }

            /* No content guess on recreate: copying the full backbuffer pulls
             * stale frames after a resize or window switch (flip buffers still
             * hold the previous window's frame — FL Studio tab-stale), and
             * stretching the present's src_rect over the whole buffer distorts
             * dirty-rect presents (issue 88 resize).  The DIB is
             * zero-initialized; the regular dirty-/full-present paths below
             * fill it from this present's actual content. */
        }
        if (swapchain->cs_present_dirty_rect_count > 0)
        {
            /* Dirty rects: accumulate only changed regions into comp buffer.
             * Use alpha-aware copy to preserve existing content under transparent
             * areas — DComp plugins Clear(transparent) every frame and only redraw
             * changed elements, relying on the compositor to preserve the rest. */
            unsigned int i;
            BOOL is_full_frame = FALSE;
            BOOL use_alpha_copy = swapchain->comp_bits && src_w == dst_w
                    && src_h == dst_h && format->byte_count == 4;

            if (use_alpha_copy)
                GdiFlush();

            for (i = 0; i < swapchain->cs_present_dirty_rect_count; ++i)
            {
                const RECT *dr = &swapchain->cs_present_dirty_rects[i];
                int sx = dr->left;
                int sy = dr->top;
                int sw = dr->right - dr->left;
                int sh = dr->bottom - dr->top;
                int dx = src_w ? sx * (int)dst_w / (int)src_w : sx;
                int dy = src_h ? sy * (int)dst_h / (int)src_h : sy;
                int dw = src_w ? sw * (int)dst_w / (int)src_w : sw;
                int dh = src_h ? sh * (int)dst_h / (int)src_h : sh;

                if (sx == 0 && sy == 0 && (unsigned int)sw >= src_w && (unsigned int)sh >= src_h)
                    is_full_frame = TRUE;

                if (use_alpha_copy)
                {
                    /* Write to per-visual surface_bits if available. Clamp the
                     * dirty rect to both the source (backbuffer) and the target
                     * buffer bounds before comp_buffer_alpha_copy's raw memcpy:
                     * present_dirty_rects come from the app via Present1 and are
                     * only count-clamped upstream (wined3d_swapchain_set_dirty_rects),
                     * never coordinate-clamped, so a rect larger than the surface
                     * or with a negative origin would read past the backbuffer and
                     * write past the comp/surface buffer. */
                    DWORD *target = swapchain->surface_bits ? swapchain->surface_bits : swapchain->comp_bits;
                    unsigned int stride = swapchain->surface_bits ? swapchain->surface_width * 4 : swapchain->comp_width * 4;
                    int tgt_w = swapchain->surface_bits ? (int)swapchain->surface_width : (int)swapchain->comp_width;
                    int tgt_h = swapchain->surface_bits ? (int)swapchain->surface_height : (int)swapchain->comp_height;
                    int csx = sx, csy = sy, cdx = dx, cdy = dy, cw = dw, ch = dh;

                    /* Trim negative origins, advancing the paired origin by the
                     * overshoot so src/dst columns stay aligned. */
                    if (csx < 0) { cw += csx; cdx -= csx; csx = 0; }
                    if (cdx < 0) { cw += cdx; csx -= cdx; cdx = 0; }
                    if (csy < 0) { ch += csy; cdy -= csy; csy = 0; }
                    if (cdy < 0) { ch += cdy; csy -= cdy; cdy = 0; }

                    /* Trim extent against source (src_w/src_h) and target bounds. */
                    if (csx + cw > (int)src_w) cw = (int)src_w - csx;
                    if (cdx + cw > tgt_w)      cw = tgt_w - cdx;
                    if (csy + ch > (int)src_h) ch = (int)src_h - csy;
                    if (cdy + ch > tgt_h)      ch = tgt_h - cdy;

                    /* Skip degenerate rects (fully clipped or inverted). */
                    if (cw > 0 && ch > 0)
                        comp_buffer_alpha_copy(target, stride,
                                back_buffer->resource.heap_memory, row_pitch,
                                cdx, cdy, csx, csy, cw, ch);
                }
                else
                {
                    StretchBlt(swapchain->comp_dc, dx, dy, dw, dh,
                            src_dc, sx, sy, sw, sh, SRCCOPY);
                }
            }
            {
                static unsigned int dirty_blit_count;
                ++dirty_blit_count;
                if (dirty_blit_count <= 10 || !(dirty_blit_count % 500))
                {
                    const RECT *dr0 = &swapchain->cs_present_dirty_rects[0];
                    TRACE("Dirty blit #%u: win %p, %u rects, r0=(%ld,%ld)-(%ld,%ld) %s, buf=%ux%u.\n",
                            dirty_blit_count, swapchain->win_handle,
                            swapchain->cs_present_dirty_rect_count,
                            dr0->left, dr0->top, dr0->right, dr0->bottom,
                            is_full_frame ? "FULL-FRAME" : "partial",
                            src_w, src_h);
                }
                /* Log all rects for multi-rect blits (> 2 rects = likely UI change, not cursor blink). */
                if (swapchain->cs_present_dirty_rect_count > 2
                        && (dirty_blit_count <= 50 || !(dirty_blit_count % 100)))
                {
                    unsigned int j;
                    LONG union_l = LONG_MAX, union_t = LONG_MAX, union_r = 0, union_b = 0;
                    unsigned long total_area = 0;
                    for (j = 0; j < swapchain->cs_present_dirty_rect_count; ++j)
                    {
                        const RECT *r = &swapchain->cs_present_dirty_rects[j];
                        if (r->left < union_l) union_l = r->left;
                        if (r->top < union_t) union_t = r->top;
                        if (r->right > union_r) union_r = r->right;
                        if (r->bottom > union_b) union_b = r->bottom;
                        total_area += (unsigned long)(r->right - r->left) * (r->bottom - r->top);
                        TRACE("  r[%u]=(%ld,%ld)-(%ld,%ld) %ldx%ld\n",
                                j, r->left, r->top, r->right, r->bottom,
                                r->right - r->left, r->bottom - r->top);
                    }
                    TRACE("  union=(%ld,%ld)-(%ld,%ld) area=%lu/%lu (%.0f%%)\n",
                            union_l, union_t, union_r, union_b,
                            total_area, (unsigned long)src_w * src_h,
                            100.0 * total_area / ((unsigned long)src_w * src_h));
                }
            }
            /* Ensure surface_bits has latest root visual data after dirty-rect update. */
            if (swapchain->surface_bits && swapchain->comp_bits)
            {
                if (!use_alpha_copy)
                {
                    /* StretchBlt wrote to comp_dc — sync back to surface_bits. */
                    GdiFlush();
                    memcpy(swapchain->surface_bits, swapchain->comp_bits,
                            swapchain->surface_width * swapchain->surface_height * sizeof(DWORD));
                }
                swapchain->surface_valid = TRUE;
            }
        }
        else
        {
            /* No dirty rects (Present without Present1, or timer-triggered).
             * Do a full blit from backbuffer to comp buffer so that any
             * D3D11/D2D1 rendering since the last Present becomes visible.
             * This is the composition-swapchain equivalent of a
             * full-window present. */
            BOOL use_alpha_merge = (swapchain->state.desc.swap_effect == WINED3D_SWAP_EFFECT_SEQUENTIAL)
                    && !(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_PREMULTIPLIED_ALPHA)
                    && swapchain->comp_bits && swapchain->surface_bits
                    && src_w == dst_w && src_h == dst_h
                    && format->byte_count == 4;

            if (use_alpha_merge)
            {
                /* SEQUENTIAL swapchain without dirty rects: the app (JUCE/Serum2)
                 * calls Clear(transparent) every frame and only redraws dirty
                 * elements. Merge only non-transparent pixels into surface_bits
                 * so that the previous frame content is preserved where the app
                 * cleared to transparent. Phase 3 will copy surface_bits to
                 * comp_bits for the final BitBlt to the window. */
                GdiFlush();
                comp_buffer_alpha_merge(swapchain->surface_bits, swapchain->surface_width * 4,
                        back_buffer->resource.heap_memory, row_pitch,
                        dst_w, dst_h);
                swapchain->surface_valid = TRUE;
            }
            else
            {
                StretchBlt(swapchain->comp_dc, 0, 0, dst_w, dst_h,
                        src_dc, src_rect->left, src_rect->top, src_w, src_h, SRCCOPY);
                if (swapchain->surface_bits && swapchain->comp_bits)
                {
                    GdiFlush();
                    memcpy(swapchain->surface_bits, swapchain->comp_bits,
                            dst_w * dst_h * sizeof(DWORD));
                    swapchain->surface_valid = TRUE;
                }
            }
        }

        /* Phase 3 Compositor: rebuild comp_bits from clean root surface_bits,
         * then composite child visuals on top. */
        if (!GetPropW(swapchain->win_handle, L"__wine_dcomp_is_child"))
        {
            if (swapchain->surface_valid && swapchain->surface_bits && swapchain->comp_bits)
            {
                GdiFlush();
                memcpy(swapchain->comp_bits, swapchain->surface_bits,
                        swapchain->surface_width * swapchain->surface_height * sizeof(DWORD));
            }
            swapchain_composite_children(swapchain, dst_w, dst_h);
            /* The dcomp leaf layer, see above. */
            swapchain_composite_layer(swapchain, swapchain->comp_bits,
                    swapchain->comp_width, dst_w, dst_h);
        }

        /* Child visuals: only update comp buffer, skip window blit.
         * The root visual reads our comp_bits directly. */
        if (GetPropW(swapchain->win_handle, L"__wine_dcomp_is_child"))
        {
            /* Child visuals: only update comp buffer, skip window blit. */
        }
        else
        {
            /* No area is kept out of this blit on account of another DComp
             * target sharing the top level.  That exclusion used to live here
             * (for issue 121) and was measured to subtract nothing: it keys off
             * the foreign target's window rect, and Chromium parks that window
             * exactly one client width to the side, so the rect it excludes and
             * the area we paint never meet.  Both the flicker it was written
             * for and the black block a later refinement addressed are handled
             * elsewhere now — top levels take the GL present path and never
             * reach this function, and targets of other top levels were already
             * filtered out by their differing GA_ROOT.  What remained was a
             * walk of the 16-slot desktop registry on every present, each slot
             * a wineserver round trip, that declined to act every time.  If a
             * present ever does need to spare a target's area, the source for
             * it cannot be the window rect; it has to be the geometry the
             * target is actually composed at, which nothing publishes today. */
            /* Always BitBlt full comp buffer to window — survives any surface reset. */
            if (!BitBlt(swapchain->dc, dst_rect->left, dst_rect->top, dst_w, dst_h,
                    swapchain->comp_dc, 0, 0, SRCCOPY))
                WARN("Failed to blit composition buffer to window.\n");
        }

        /* Store comp_dc as window property so WM_PAINT can re-blit. Don't
         * publish a partially-initialized composition state via props. */
        if (!SetPropW(swapchain->win_handle, L"__wine_dcomp_comp_dc", (HANDLE)swapchain->comp_dc))
            WARN("Failed to set __wine_dcomp_comp_dc property.\n");
        {
            /* Store dimensions as a single LPARAM-sized value. */
            LPARAM dims = MAKELPARAM(dst_w, dst_h);
            if (!SetPropW(swapchain->win_handle, L"__wine_dcomp_comp_size", (HANDLE)dims))
                WARN("Failed to set __wine_dcomp_comp_size property.\n");
        }
        /* Expose surface_bits so root can composite per-visual layers. */
        if (!SetPropW(swapchain->win_handle, L"__wine_dcomp_comp_bits",
                (HANDLE)(swapchain->surface_bits ? swapchain->surface_bits : swapchain->comp_bits)))
            WARN("Failed to set __wine_dcomp_comp_bits property.\n");

        swapchain->cs_present_dirty_rect_count = 0;
    }
    else
    {
        /* The dcomp leaf layer goes into the source memory BEFORE the blit --
         * that is the back buffer copy this branch is about to write to the
         * window (issue 206). */
        composited_layer = format->byte_count == 4 && swapchain_composite_layer(swapchain,
                (DWORD *)back_buffer->resource.heap_memory, row_pitch / 4,
                create_desc.Width, create_desc.Height);
        if (composited_layer)
            GdiFlush();

        if (!StretchBlt(swapchain->dc, dst_rect->left, dst_rect->top,
                dst_rect->right - dst_rect->left, dst_rect->bottom - dst_rect->top,
                src_dc, src_rect->left, src_rect->top,
                src_rect->right - src_rect->left, src_rect->bottom - src_rect->top, SRCCOPY))
            ERR("Failed to blit.\n");

        /* Do not leave the leaves in that copy: a later present skipping the
         * download would blit them a second time and the leaf would trail. */
        if (composited_layer)
            wined3d_texture_invalidate_location(back_buffer, 0, WINED3D_LOCATION_SYSMEM);
    }

    /* Suppress spurious WM_PAINT after writing to the window surface. */
    ValidateRect(swapchain->win_handle, NULL);

done:
    destroy_desc.hDc = src_dc;
    destroy_desc.hBitmap = bitmap;
    if ((status = D3DKMTDestroyDCFromMemory(&destroy_desc)))
        ERR("Failed to destroy src dc, status %#lx.\n", status);
}

/* A GL context is provided by the caller */
static void swapchain_blit(const struct wined3d_swapchain *swapchain,
        struct wined3d_context *context, const RECT *src_rect, const RECT *dst_rect)
{
    struct wined3d_texture *texture = swapchain->back_buffers[0];
    struct wined3d_device *device = swapchain->device;
    enum wined3d_texture_filter_type filter;
    DWORD location;

    TRACE("swapchain %p, context %p, src_rect %s, dst_rect %s.\n",
            swapchain, context, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect));

    if ((src_rect->right - src_rect->left == dst_rect->right - dst_rect->left
            && src_rect->bottom - src_rect->top == dst_rect->bottom - dst_rect->top)
            || is_complex_fixup(texture->resource.format->color_fixup))
        filter = WINED3D_TEXF_NONE;
    else
        filter = WINED3D_TEXF_LINEAR;

    location = WINED3D_LOCATION_TEXTURE_RGB;
    if (texture->resource.multisample_type)
        location = WINED3D_LOCATION_RB_RESOLVED;

    wined3d_texture_validate_location(texture, 0, WINED3D_LOCATION_DRAWABLE);
    device->blitter->ops->blitter_blit(device->blitter, WINED3D_BLIT_OP_COLOR_BLIT, context, texture, 0,
            location, src_rect, texture, 0, WINED3D_LOCATION_DRAWABLE, dst_rect, NULL, filter, NULL);
    wined3d_texture_invalidate_location(texture, 0, WINED3D_LOCATION_DRAWABLE);
}

/* --- The leaf layer in the GL present frame (issue 206) ---------------------
 *
 * swapchain_composite_layer() above composites on the CPU, into the buffer the
 * GDI branch writes to the window.  A FLIP_DISCARD swapchain on a top-level
 * window does not take that branch: it presents through swapchain_blit() plus
 * wglSwapBuffers().  For the layer to be part of that frame it has to be drawn
 * into the drawable between those two calls -- after which it leaves with the
 * swap and there is no queue left to lose a race against.
 *
 * The layer lives in a persistent texture of ours (only the published box is
 * uploaded per present, not the whole layer) and is drawn through wined3d's own
 * blitter chain.  Three places this could have gone wrong:
 *
 * 1. THE STATE CACHE.  Foreign GL leaves wined3d's cache inconsistent and the
 *    damage shows up somewhere else entirely.  So nothing but the blend is set
 *    by hand: wined3d_context_gl_apply_blit_state() establishes the known blit
 *    state AND marks the affected STATE_* invalid (STATE_BLEND among them), and
 *    the rest is the blitter chain's own business.  The blend is switched off
 *    again afterwards so the blit state keeps the invariant apply_blit_state()
 *    relies on the next time round -- it returns early when last_was_blit is
 *    already set, and would then not switch it off.
 *
 * 2. WHICH BLITTER.  device->blitter is a chain, raw -> fbo -> glsl -> ffp ->
 *    cpu.  A WINED3D_BLIT_OP_COLOR_BLIT is taken by the fbo blitter, which
 *    serves it with glBlitFramebuffer -- and that ignores GL_BLEND entirely, so
 *    the layer would overwrite the frame in its box instead of lying over it.
 *    With WINED3D_BLIT_OP_COLOR_BLIT_ALPHATEST the fbo blitter does not take it
 *    (fbo_blitter_supported() knows COLOR_BLIT and DEPTH_BLIT and nothing else)
 *    and the request falls through to the glsl blitter, which draws a shader
 *    quad -- and a quad respects blending.  The alpha test itself is a side
 *    effect, discarding the fully transparent pixels blending would have passed
 *    through anyway.
 *
 *    That is a quiet coupling to somebody else's code and the one place a
 *    reviewer should look.  A blit op that asks for blending outright would be
 *    the honest form of it.
 *
 * 3. THE Y DIRECTION.  Not computed here: glsl_blitter_blit() calls
 *    wined3d_texture_translate_drawable_coords() itself for
 *    WINED3D_LOCATION_DRAWABLE, so the destination rectangle goes in as client
 *    coordinates.  And because the box is uploaded to the same place in the
 *    texture that it occupies in the window, source and destination are the
 *    same rectangle.
 *
 * Alpha is PREMULTIPLIED (dcomp lays the layer down that way), hence GL_ONE /
 * GL_ONE_MINUS_SRC_ALPHA and not GL_SRC_ALPHA. */

/* Create or resize the texture.  Deliberately NOT part of drawing: creating it
 * inside the present path made wined3d activate a context with a NULL DC, which
 * cost five wglSetPixelFormatWINE errors per session and a context switch in a
 * path that is supposed to be quick.  Called before the present acquires its
 * context, so this is an ordinary texture creation like any other.
 *
 * It is not done at swapchain creation either, which would look tidier: the
 * layer is as large as the window, and a texture that size for every top-level
 * swapchain would cost tens of megabytes in every 3D application, the vast
 * majority of which never see a composition target.  Here it costs nothing
 * until a layer is actually published. */
static void swapchain_gl_prepare_layer_texture(struct wined3d_swapchain *swapchain)
{
    struct wined3d_resource_desc desc;
    struct wine_dcomp_layer *layer;
    struct wined3d_sub_resource_data data;
    unsigned int w = 0, h = 0;
    DWORD *zero;
    HRESULT hr;

    if (!(layer = swapchain_poll_layer(swapchain)))
        return;

    AcquireSRWLockShared(&layer->lock);
    if (layer->bits)
    {
        w = layer->width;
        h = layer->height;
    }
    ReleaseSRWLockShared(&layer->lock);

    if (!w || !h)
        return;
    if (swapchain->layer_texture && swapchain->layer_tex_width == w
            && swapchain->layer_tex_height == h)
        return;

    if (swapchain->layer_texture)
    {
        wined3d_texture_decref(swapchain->layer_texture);
        swapchain->layer_texture = NULL;
        swapchain->layer_tex_width = swapchain->layer_tex_height = 0;
    }

    /* Zero-initialised, not merely allocated.  Only the published box is ever
     * uploaded, so everything outside it is whatever the texture happened to
     * come with -- transparent is the only value that draws nothing.  Today
     * only the box is drawn as well, so it would not show; the moment a real
     * region is drawn instead of a box, it would. */
    if (!(zero = calloc((size_t)w * h, sizeof(DWORD))))
    {
        ERR("Failed to allocate the layer texture initialiser.\n");
        return;
    }

    desc.resource_type = WINED3D_RTYPE_TEXTURE_2D;
    desc.format = WINED3DFMT_B8G8R8A8_UNORM;
    desc.multisample_type = WINED3D_MULTISAMPLE_NONE;
    desc.multisample_quality = 0;
    desc.usage = WINED3DUSAGE_CS;
    desc.bind_flags = WINED3D_BIND_SHADER_RESOURCE;
    desc.access = WINED3D_RESOURCE_ACCESS_GPU;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.size = 0;

    data.data = zero;
    data.row_pitch = w * sizeof(DWORD);
    data.slice_pitch = (unsigned int)((size_t)w * h * sizeof(DWORD));

    if (FAILED(hr = wined3d_texture_create(swapchain->device, &desc, 1, 1, 0,
            &data, NULL, &wined3d_null_parent_ops, &swapchain->layer_texture)))
    {
        ERR("Failed to create the %ux%u layer texture, hr %#lx.\n", w, h, hr);
        swapchain->layer_texture = NULL;
    }
    else
    {
        swapchain->layer_tex_width = w;
        swapchain->layer_tex_height = h;
        TRACE("Created a %ux%u layer texture for hwnd %p.\n", w, h, swapchain->win_handle);
    }
    free(zero);
}

static BOOL swapchain_gl_draw_layer(struct wined3d_swapchain *swapchain,
        struct wined3d_context *context, const RECT *dst_rect)
{
    struct wined3d_context_gl *context_gl = wined3d_context_gl(context);
    struct wined3d_texture *back_buffer = swapchain->back_buffers[0];
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;
    struct wined3d_device *device = swapchain->device;
    const struct wined3d_format_gl *format_gl;
    struct wined3d_texture_gl *texture_gl;
    RECT rects[WINE_DCOMP_LAYER_MAX_RECTS];
    struct wine_dcomp_layer *layer;
    unsigned int i, n;

    if (!swapchain->layer_texture)
        return FALSE;
    if (!(n = swapchain_layer_lock_rects(swapchain, &layer, rects, dst_rect)))
        return FALSE;

    /* The texture is prepared before the context is acquired, so a layer that
     * has just changed size has to wait one present for its texture.  Drawing
     * against the old one would put the rectangles in the wrong place. */
    if (layer->width != swapchain->layer_tex_width || layer->height != swapchain->layer_tex_height)
    {
        ReleaseSRWLockShared(&layer->lock);
        return FALSE;
    }

    texture_gl = wined3d_texture_gl(swapchain->layer_texture);
    format_gl = wined3d_format_gl(swapchain->layer_texture->resource.format);

    if (!wined3d_texture_prepare_location(swapchain->layer_texture, 0, context, WINED3D_LOCATION_TEXTURE_RGB))
    {
        ERR("Failed to prepare the layer texture.\n");
        ReleaseSRWLockShared(&layer->lock);
        return FALSE;
    }

    /* Only the published rectangles go up.  What sits outside them in the
     * texture is older, and outside them nothing is drawn. */
    wined3d_texture_gl_bind_and_dirtify(texture_gl, context_gl, FALSE);
    GL_EXTCALL(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0));
    gl_info->gl_ops.gl.p_glPixelStorei(GL_UNPACK_ROW_LENGTH, layer->width);
    for (i = 0; i < n; ++i)
        gl_info->gl_ops.gl.p_glTexSubImage2D(texture_gl->target, 0, rects[i].left, rects[i].top,
                rects[i].right - rects[i].left, rects[i].bottom - rects[i].top,
                format_gl->format, format_gl->type,
                layer->bits + (SIZE_T)rects[i].top * layer->width + rects[i].left);
    gl_info->gl_ops.gl.p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    checkGLcall("dcomp layer upload");
    wined3d_texture_validate_location(swapchain->layer_texture, 0, WINED3D_LOCATION_TEXTURE_RGB);

    /* Everything below reads the texture, not the layer. */
    ReleaseSRWLockShared(&layer->lock);

    /* swapchain_blit() has just written the drawable and then marked it invalid
     * (the swap discards it).  For the duration of these blits it is valid --
     * without that the blitter would load the whole back buffer into the
     * drawable a second time for the sake of a partial rectangle. */
    wined3d_texture_validate_location(back_buffer, 0, WINED3D_LOCATION_DRAWABLE);

    wined3d_context_gl_apply_blit_state(context_gl, device);
    gl_info->gl_ops.gl.p_glEnable(GL_BLEND);
    if (gl_info->supported[EXT_BLEND_FUNC_SEPARATE])
        GL_EXTCALL(glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    else
        gl_info->gl_ops.gl.p_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (gl_info->supported[EXT_BLEND_EQUATION_SEPARATE])
        GL_EXTCALL(glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD));
    checkGLcall("dcomp layer blend state");

    for (i = 0; i < n; ++i)
        device->blitter->ops->blitter_blit(device->blitter, WINED3D_BLIT_OP_COLOR_BLIT_ALPHATEST,
                context, swapchain->layer_texture, 0, WINED3D_LOCATION_TEXTURE_RGB, &rects[i],
                back_buffer, 0, WINED3D_LOCATION_DRAWABLE, &rects[i], NULL, WINED3D_TEXF_NONE, NULL);

    gl_info->gl_ops.gl.p_glDisable(GL_BLEND);
    checkGLcall("dcomp layer blend reset");
    /* apply_blit_state() marked STATE_BLEND invalid; the application's next
     * real draw restores its own function and equation. */
    context_invalidate_state(context, STATE_BLEND);

    wined3d_texture_invalidate_location(back_buffer, 0, WINED3D_LOCATION_DRAWABLE);

    /* dcomp watches this to tell a sink that draws from one that does not. */
    InterlockedIncrement(&layer->drawn);

    TRACE("Drew %u layer rect(s) into the frame for hwnd %p.\n", n, swapchain->win_handle);
    return TRUE;
}

static void swapchain_gl_set_swap_interval(struct wined3d_swapchain *swapchain,
        struct wined3d_context_gl *context_gl, unsigned int swap_interval)
{
    const struct wined3d_gl_info *gl_info = context_gl->gl_info;

    swap_interval = swap_interval <= 4 ? swap_interval : 1;
    if (swapchain->swap_interval == swap_interval)
        return;

    swapchain->swap_interval = swap_interval;

    if (!gl_info->supported[WGL_EXT_SWAP_CONTROL])
        return;

    if (!GL_EXTCALL(wglSwapIntervalEXT(swap_interval)))
    {
        ERR("Failed to set swap interval %u for context %p, last error %#lx.\n",
                swap_interval, context_gl, GetLastError());
    }
}

/* Whether this swapchain's back buffers may be left alone on a present.
 *
 * A present rotates the back buffer textures, and for the flip model it copies
 * the presented frame back into back buffer 0 so an application can keep
 * rendering incrementally (dirty rects).  The two halves are only correct
 * together: rotating without copying hands the application a buffer holding an
 * older frame, copying without rotating overwrites what it just drew.
 *
 * Doing neither is equivalent for the application and cheaper than both -- back
 * buffer 0 simply keeps the frame that was just presented.  That equivalence
 * holds wherever the application cannot address the other buffers as distinct
 * surfaces:
 *
 *   DISCARD                        the content of a back buffer after a present
 *                                  is undefined by specification, so nothing may
 *                                  rely on which buffer it is handed
 *   FLIP_DISCARD, FLIP_SEQUENTIAL  the DXGI flip model allows only back buffer 0
 *                                  as a render target
 *
 * SEQUENTIAL is deliberately not included: that is D3D9's D3DSWAPEFFECT_FLIP,
 * where rotating through the buffer chain is the documented behaviour and an
 * application may render into the other buffers.
 *
 * Fender Studio Pro 8 renders incrementally into a DISCARD swapchain with more
 * than one back buffer.  It therefore drew onto a buffer two frames old every
 * other frame, which showed as the window's tool and transport bars alternating
 * between two states while the arrangement area stood still (issue 185). */
bool wined3d_swapchain_keeps_back_buffers(const struct wined3d_swapchain *swapchain)
{
    switch (swapchain->state.desc.swap_effect)
    {
        case WINED3D_SWAP_EFFECT_DISCARD:
        case WINED3D_SWAP_EFFECT_FLIP_DISCARD:
        case WINED3D_SWAP_EFFECT_FLIP_SEQUENTIAL:
            return true;

        default:
            return false;
    }
}

/* Context activation is done by the caller. */
static void wined3d_swapchain_gl_rotate(struct wined3d_swapchain *swapchain, struct wined3d_context *context)
{
    struct wined3d_texture_sub_resource *sub_resource;
    struct wined3d_texture_gl *texture, *texture_prev;
    struct gl_texture tex0;
    GLuint rb0;
    DWORD locations0;
    unsigned int i;
    static const DWORD supported_locations = WINED3D_LOCATION_TEXTURE_RGB | WINED3D_LOCATION_RB_MULTISAMPLE;

    if (swapchain->state.desc.backbuffer_count < 2
            || wined3d_swapchain_keeps_back_buffers(swapchain))
        return;

    texture_prev = wined3d_texture_gl(swapchain->back_buffers[0]);

    /* Back buffer 0 is already in the draw binding. */
    tex0 = texture_prev->texture_rgb;
    rb0 = texture_prev->rb_multisample;
    locations0 = texture_prev->t.sub_resources[0].locations;

    for (i = 1; i < swapchain->state.desc.backbuffer_count; ++i)
    {
        texture = wined3d_texture_gl(swapchain->back_buffers[i]);
        sub_resource = &texture->t.sub_resources[0];

        if (!(sub_resource->locations & supported_locations))
            wined3d_texture_load_location(&texture->t, 0, context, texture->t.resource.draw_binding);

        texture_prev->texture_rgb = texture->texture_rgb;
        texture_prev->rb_multisample = texture->rb_multisample;

        wined3d_texture_validate_location(&texture_prev->t, 0, sub_resource->locations & supported_locations);
        wined3d_texture_invalidate_location(&texture_prev->t, 0, ~(sub_resource->locations & supported_locations));

        texture_prev = texture;
    }

    texture_prev->texture_rgb = tex0;
    texture_prev->rb_multisample = rb0;

    wined3d_texture_validate_location(&texture_prev->t, 0, locations0 & supported_locations);
    wined3d_texture_invalidate_location(&texture_prev->t, 0, ~(locations0 & supported_locations));

    device_invalidate_state(swapchain->device, STATE_FRAMEBUFFER);
}

static bool swapchain_present_is_partial_copy(struct wined3d_swapchain *swapchain, const RECT *dst_rect)
{
    enum wined3d_swap_effect swap_effect = swapchain->state.desc.swap_effect;
    const struct wined3d_swapchain_desc *desc = &swapchain->state.desc;
    unsigned int width, height;

    if (swap_effect != WINED3D_SWAP_EFFECT_COPY && swap_effect != WINED3D_SWAP_EFFECT_COPY_VSYNC)
        return false;

    if (!desc->windowed)
    {
        width = desc->backbuffer_width;
        height = desc->backbuffer_height;
    }
    else
    {
        RECT client_rect;
        GetClientRect(swapchain->win_handle, &client_rect);
        width = client_rect.right - client_rect.left;
        height = client_rect.bottom - client_rect.top;
    }

    if ((dst_rect->left && dst_rect->right) || abs(dst_rect->right - dst_rect->left) != width)
        return true;
    if ((dst_rect->top && dst_rect->bottom) || abs(dst_rect->bottom - dst_rect->top) != height)
        return true;

    return false;
}

static void swapchain_gl_present(struct wined3d_swapchain *swapchain,
        const RECT *src_rect, const RECT *dst_rect, unsigned int swap_interval, uint32_t flags)
{
    struct wined3d_texture *back_buffer = swapchain->back_buffers[0];
    const struct wined3d_pixel_format *pixel_format;
    const struct wined3d_gl_info *gl_info;
    struct wined3d_context_gl *context_gl;
    struct wined3d_context *context;

    /* Before the context is acquired, see the comment on the function. */
    swapchain_gl_prepare_layer_texture(swapchain);

    context = context_acquire(swapchain->device, swapchain->front_buffer, 0);
    context_gl = wined3d_context_gl(context);
    if (!context_gl->valid)
    {
        context_release(context);
        WARN("Invalid context, skipping present.\n");
        return;
    }

    TRACE("Presenting DC %p.\n", context_gl->dc);

    pixel_format = &wined3d_adapter_gl(swapchain->device->adapter)->pixel_formats[context_gl->pixel_format - 1];

    /* PREFER_GL_PRESENT overrides FLIP_SEQUENTIAL/SEQUENTIAL/partial checks
     * for top-level popup windows with a GL-capable X11 drawable.  The GL
     * context must have successfully bound (not backup_dc) and FORCE_GDI
     * must not be set.  This eliminates the GPU readback + CPU copies that
     * the GDI present path requires (~5-10ms per frame). */
    if ((swapchain->state.desc.flags & WINED3D_SWAPCHAIN_PREFER_GL_PRESENT)
            && !(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FORCE_GDI_PRESENT)
            && context_gl->dc != wined3d_device_gl(swapchain->device)->backup_dc)
    {
        static unsigned int gl_present_count;
        gl_info = context_gl->gl_info;

        if (!(gl_present_count++ % 60))
            TRACE("GL popup present #%u: dc %p, win %p.\n",
                    gl_present_count, context_gl->dc, swapchain->win_handle);

        swapchain_gl_set_swap_interval(swapchain, context_gl, swap_interval);
        wined3d_texture_load_location(back_buffer, 0, context, back_buffer->resource.draw_binding);
        swapchain_blit(swapchain, context, src_rect, dst_rect);
        /* The dcomp leaf layer into the frame, before the swap (issue 206). */
        swapchain_gl_draw_layer(swapchain, context, dst_rect);
        gl_info->gl_ops.wgl.p_wglSwapBuffers(context_gl->dc);
    }
    else if (context_gl->dc == wined3d_device_gl(swapchain->device)->backup_dc
            || (swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FORCE_GDI_PRESENT)
            || (pixel_format->swap_method != WGL_SWAP_COPY_ARB
            && swapchain_present_is_partial_copy(swapchain, dst_rect))
            || swapchain->state.desc.swap_effect == WINED3D_SWAP_EFFECT_FLIP_SEQUENTIAL
            || swapchain->state.desc.swap_effect == WINED3D_SWAP_EFFECT_SEQUENTIAL)
    {
        swapchain_blit_gdi(swapchain, context, src_rect, dst_rect);
    }
    else
    {
        gl_info = context_gl->gl_info;

        swapchain_gl_set_swap_interval(swapchain, context_gl, swap_interval);

        wined3d_texture_load_location(back_buffer, 0, context, back_buffer->resource.draw_binding);

        swapchain_blit(swapchain, context, src_rect, dst_rect);

        /* The dcomp leaf layer into the frame, before the swap (issue 206). */
        swapchain_gl_draw_layer(swapchain, context, dst_rect);

        if (swapchain->device->context_count > 1)
        {
            WARN_(d3d_perf)("Multiple contexts, calling glFinish() to enforce ordering.\n");
            gl_info->gl_ops.gl.p_glFinish();
        }

        /* call wglSwapBuffers through the gl table to avoid confusing the Steam overlay */
        gl_info->gl_ops.wgl.p_wglSwapBuffers(context_gl->dc);
    }


    if (context->d3d_info->fences)
        wined3d_context_gl_submit_command_fence(context_gl);

    wined3d_swapchain_gl_rotate(swapchain, context);

    TRACE("SwapBuffers called, Starting new frame\n");

    wined3d_texture_validate_location(swapchain->front_buffer, 0, WINED3D_LOCATION_DRAWABLE);
    wined3d_texture_invalidate_location(swapchain->front_buffer, 0, ~WINED3D_LOCATION_DRAWABLE);

    context_release(context);
}

static void swapchain_frontbuffer_updated(struct wined3d_swapchain *swapchain)
{
    struct wined3d_texture *front_buffer = swapchain->front_buffer;
    struct wined3d_context *context;

    context = context_acquire(swapchain->device, front_buffer, 0);
    wined3d_texture_load_location(front_buffer, 0, context, front_buffer->resource.draw_binding);
    context_release(context);
    SetRectEmpty(&swapchain->front_buffer_update);
}

static const struct wined3d_swapchain_ops swapchain_gl_ops =
{
    swapchain_gl_present,
    swapchain_frontbuffer_updated,
};

static bool wined3d_swapchain_vk_present_mode_supported(struct wined3d_swapchain_vk *swapchain_vk,
        VkPresentModeKHR vk_present_mode)
{
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_vk_info *vk_info;
    struct wined3d_adapter_vk *adapter_vk;
    VkPhysicalDevice vk_physical_device;
    VkPresentModeKHR *vk_modes;
    bool supported = false;
    uint32_t count, i;
    VkResult vr;

    adapter_vk = wined3d_adapter_vk(device_vk->d.adapter);
    vk_physical_device = adapter_vk->physical_device;
    vk_info = &adapter_vk->vk_info;

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device,
            swapchain_vk->vk_surface, &count, NULL))) < 0)
    {
        ERR("Failed to get supported present mode count, vr %s.\n", wined3d_debug_vkresult(vr));
        return false;
    }

    if (!(vk_modes = calloc(count, sizeof(*vk_modes))))
        return false;

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device,
            swapchain_vk->vk_surface, &count, vk_modes))) < 0)
    {
        ERR("Failed to get supported present modes, vr %s.\n", wined3d_debug_vkresult(vr));
        goto done;
    }

    for (i = 0; i < count; ++i)
    {
        if (vk_modes[i] == vk_present_mode)
        {
            supported = true;
            goto done;
        }
    }

done:
    free(vk_modes);
    return supported;
}

static VkFormat get_swapchain_fallback_format(VkFormat vk_format)
{
    switch (vk_format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            WARN("Unhandled format %#x.\n", vk_format);
            return VK_FORMAT_UNDEFINED;
    }
}

static VkFormat wined3d_swapchain_vk_select_vk_format(struct wined3d_swapchain_vk *swapchain_vk,
        VkSurfaceKHR vk_surface)
{
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_swapchain_desc *desc = &swapchain_vk->s.state.desc;
    const struct wined3d_vk_info *vk_info;
    struct wined3d_adapter_vk *adapter_vk;
    const struct wined3d_format *format;
    VkPhysicalDevice vk_physical_device;
    VkSurfaceFormatKHR *vk_formats;
    uint32_t format_count, i;
    VkFormat vk_format;
    VkResult vr;

    adapter_vk = wined3d_adapter_vk(device_vk->d.adapter);
    vk_physical_device = adapter_vk->physical_device;
    vk_info = &adapter_vk->vk_info;

    if ((format = wined3d_get_format(&adapter_vk->a, desc->backbuffer_format, WINED3D_BIND_RENDER_TARGET)))
        vk_format = wined3d_format_vk(format)->vk_format;
    else
        vk_format = VK_FORMAT_B8G8R8A8_UNORM;

    vr = VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, NULL));
    if (vr < 0 || !format_count)
    {
        WARN("Failed to get supported surface format count, vr %s.\n", wined3d_debug_vkresult(vr));
        return VK_FORMAT_UNDEFINED;
    }

    if (!(vk_formats = calloc(format_count, sizeof(*vk_formats))))
        return VK_FORMAT_UNDEFINED;

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device,
            vk_surface, &format_count, vk_formats))) < 0)
    {
        WARN("Failed to get supported surface formats, vr %s.\n", wined3d_debug_vkresult(vr));
        free(vk_formats);
        return VK_FORMAT_UNDEFINED;
    }

    for (i = 0; i < format_count; ++i)
    {
        if (vk_formats[i].format == vk_format && vk_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            break;
    }
    if (i == format_count)
    {
        /* Try to create a swapchain with format conversion. */
        vk_format = get_swapchain_fallback_format(vk_format);
        WARN("Failed to find Vulkan swapchain format for %s.\n", debug_d3dformat(desc->backbuffer_format));
        for (i = 0; i < format_count; ++i)
        {
            if (vk_formats[i].format == vk_format && vk_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                break;
        }
    }
    free(vk_formats);
    if (i == format_count)
    {
        FIXME("Failed to find Vulkan swapchain format for %s.\n", debug_d3dformat(desc->backbuffer_format));
        return VK_FORMAT_UNDEFINED;
    }

    TRACE("Using Vulkan swapchain format %#x.\n", vk_format);

    return vk_format;
}

static bool wined3d_swapchain_vk_create_vulkan_swapchain_images(struct wined3d_swapchain_vk *swapchain_vk,
        VkSwapchainKHR vk_swapchain)
{
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_vk_info *vk_info;
    VkSemaphoreCreateInfo semaphore_info;
    uint32_t image_count, i;
    VkResult vr;

    vk_info = &wined3d_adapter_vk(device_vk->d.adapter)->vk_info;

    if ((vr = VK_CALL(vkGetSwapchainImagesKHR(device_vk->vk_device, vk_swapchain, &image_count, NULL))) < 0)
    {
        ERR("Failed to get image count, vr %s\n", wined3d_debug_vkresult(vr));
        return false;
    }

    if (!(swapchain_vk->vk_images = calloc(image_count, sizeof(*swapchain_vk->vk_images))))
    {
        ERR("Failed to allocate images array.\n");
        return false;
    }

    if ((vr = VK_CALL(vkGetSwapchainImagesKHR(device_vk->vk_device,
            vk_swapchain, &image_count, swapchain_vk->vk_images))) < 0)
    {
        ERR("Failed to get swapchain images, vr %s.\n", wined3d_debug_vkresult(vr));
        free(swapchain_vk->vk_images);
        return false;
    }

    if (!(swapchain_vk->vk_semaphores = calloc(image_count, sizeof(*swapchain_vk->vk_semaphores))))
    {
        ERR("Failed to allocate semaphores array.\n");
        free(swapchain_vk->vk_images);
        return false;
    }

    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = NULL;
    semaphore_info.flags = 0;
    for (i = 0; i < image_count; ++i)
    {
        if ((vr = VK_CALL(vkCreateSemaphore(device_vk->vk_device,
                &semaphore_info, NULL, &swapchain_vk->vk_semaphores[i].available))) < 0)
        {
            ERR("Failed to create semaphore, vr %s.\n", wined3d_debug_vkresult(vr));
            goto fail;
        }

        if ((vr = VK_CALL(vkCreateSemaphore(device_vk->vk_device,
                &semaphore_info, NULL, &swapchain_vk->vk_semaphores[i].presentable))) < 0)
        {
            ERR("Failed to create semaphore, vr %s.\n", wined3d_debug_vkresult(vr));
            goto fail;
        }
    }
    swapchain_vk->image_count = image_count;

    return true;

fail:
    for (i = 0; i < image_count; ++i)
    {
        if (swapchain_vk->vk_semaphores[i].available)
            VK_CALL(vkDestroySemaphore(device_vk->vk_device, swapchain_vk->vk_semaphores[i].available, NULL));
        if (swapchain_vk->vk_semaphores[i].presentable)
            VK_CALL(vkDestroySemaphore(device_vk->vk_device, swapchain_vk->vk_semaphores[i].presentable, NULL));
    }
    free(swapchain_vk->vk_semaphores);
    free(swapchain_vk->vk_images);
    return false;
}

static HRESULT wined3d_swapchain_vk_create_vulkan_swapchain(struct wined3d_swapchain_vk *swapchain_vk)
{
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_swapchain_desc *desc = &swapchain_vk->s.state.desc;
    VkSwapchainCreateInfoKHR vk_swapchain_desc;
    VkWin32SurfaceCreateInfoKHR surface_desc;
    unsigned int width, height, image_count;
    const struct wined3d_vk_info *vk_info;
    VkSurfaceCapabilitiesKHR surface_caps;
    struct wined3d_adapter_vk *adapter_vk;
    VkPresentModeKHR vk_present_mode;
    VkSwapchainKHR vk_swapchain;
    VkImageUsageFlags usage;
    VkSurfaceKHR vk_surface;
    VkBool32 supported;
    VkFormat vk_format;
    RECT client_rect;
    VkResult vr;

    adapter_vk = wined3d_adapter_vk(device_vk->d.adapter);
    vk_info = &adapter_vk->vk_info;

    surface_desc.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_desc.pNext = NULL;
    surface_desc.flags = 0;
    surface_desc.hinstance = (HINSTANCE)GetWindowLongPtrW(swapchain_vk->s.win_handle, GWLP_HINSTANCE);
    surface_desc.hwnd = swapchain_vk->s.win_handle;
    if ((vr = VK_CALL(vkCreateWin32SurfaceKHR(vk_info->instance, &surface_desc, NULL, &vk_surface))) < 0)
    {
        ERR("Failed to create Vulkan surface, vr %s.\n", wined3d_debug_vkresult(vr));
        return E_FAIL;
    }
    swapchain_vk->vk_surface = vk_surface;

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceSupportKHR(adapter_vk->physical_device,
            device_vk->graphics_queue.vk_queue_family_index, vk_surface, &supported))) < 0 || !supported)
    {
        ERR("Queue family does not support presentation on this surface, vr %s.\n", wined3d_debug_vkresult(vr));
        goto fail;
    }

    if ((vk_format = wined3d_swapchain_vk_select_vk_format(swapchain_vk, vk_surface)) == VK_FORMAT_UNDEFINED)
    {
        ERR("Failed to select swapchain format.\n");
        goto fail;
    }

    if ((vr = VK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(adapter_vk->physical_device,
            swapchain_vk->vk_surface, &surface_caps))) < 0)
    {
        ERR("Failed to get surface capabilities, vr %s.\n", wined3d_debug_vkresult(vr));
        goto fail;
    }

    image_count = desc->backbuffer_count;
    if (image_count < surface_caps.minImageCount)
        image_count = surface_caps.minImageCount;
    else if (surface_caps.maxImageCount && image_count > surface_caps.maxImageCount)
        image_count = surface_caps.maxImageCount;

    if (image_count != desc->backbuffer_count)
        WARN("Image count %u is not supported (%u-%u).\n", desc->backbuffer_count,
                surface_caps.minImageCount, surface_caps.maxImageCount);

    GetClientRect(swapchain_vk->s.win_handle, &client_rect);

    width = client_rect.right - client_rect.left;
    if (width < surface_caps.minImageExtent.width)
        width = surface_caps.minImageExtent.width;
    else if (width > surface_caps.maxImageExtent.width)
        width = surface_caps.maxImageExtent.width;

    height = client_rect.bottom - client_rect.top;
    if (height < surface_caps.minImageExtent.height)
        height = surface_caps.minImageExtent.height;
    else if (height > surface_caps.maxImageExtent.height)
        height = surface_caps.maxImageExtent.height;

    if (width != client_rect.right - client_rect.left || height != client_rect.bottom - client_rect.top)
        WARN("Swapchain dimensions %lux%lu are not supported (%u-%u x %u-%u).\n",
                client_rect.right - client_rect.left, client_rect.bottom - client_rect.top,
                surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width,
                surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);

    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    usage |= surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        WARN("Transfer not supported for swapchain images.\n");

    if (!(surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
    {
        FIXME("Unsupported alpha mode, %#x.\n", surface_caps.supportedCompositeAlpha);
        goto fail;
    }

    vk_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!swapchain_vk->s.swap_interval)
    {
        if (wined3d_swapchain_vk_present_mode_supported(swapchain_vk, VK_PRESENT_MODE_IMMEDIATE_KHR))
            vk_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else
            FIXME("Unsupported swap interval %u.\n", swapchain_vk->s.swap_interval);
    }

    vk_swapchain_desc.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    vk_swapchain_desc.pNext = NULL;
    vk_swapchain_desc.flags = 0;
    vk_swapchain_desc.surface = vk_surface;
    vk_swapchain_desc.minImageCount = image_count;
    vk_swapchain_desc.imageFormat = vk_format;
    vk_swapchain_desc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    vk_swapchain_desc.imageExtent.width = width;
    vk_swapchain_desc.imageExtent.height = height;
    vk_swapchain_desc.imageArrayLayers = 1;
    vk_swapchain_desc.imageUsage = usage;
    vk_swapchain_desc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_swapchain_desc.queueFamilyIndexCount = 0;
    vk_swapchain_desc.pQueueFamilyIndices = NULL;
    vk_swapchain_desc.preTransform = surface_caps.currentTransform;
    vk_swapchain_desc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    vk_swapchain_desc.presentMode = vk_present_mode;
    vk_swapchain_desc.clipped = VK_TRUE;
    vk_swapchain_desc.oldSwapchain = VK_NULL_HANDLE;
    if ((vr = VK_CALL(vkCreateSwapchainKHR(device_vk->vk_device, &vk_swapchain_desc, NULL, &vk_swapchain))) < 0)
    {
        ERR("Failed to create Vulkan swapchain, vr %s.\n", wined3d_debug_vkresult(vr));
        goto fail;
    }
    swapchain_vk->vk_swapchain = vk_swapchain;

    if (!wined3d_swapchain_vk_create_vulkan_swapchain_images(swapchain_vk, vk_swapchain))
    {
        VK_CALL(vkDestroySwapchainKHR(device_vk->vk_device, vk_swapchain, NULL));
        goto fail;
    }

    swapchain_vk->width = width;
    swapchain_vk->height = height;

    return WINED3D_OK;

fail:
    VK_CALL(vkDestroySurfaceKHR(vk_info->instance, vk_surface, NULL));
    swapchain_vk->vk_surface = 0;
    return E_FAIL;
}

static HRESULT wined3d_swapchain_vk_recreate(struct wined3d_swapchain_vk *swapchain_vk)
{
    TRACE("swapchain_vk %p.\n", swapchain_vk);

    wined3d_swapchain_vk_destroy_vulkan_swapchain(swapchain_vk);

    return wined3d_swapchain_vk_create_vulkan_swapchain(swapchain_vk);
}

static void wined3d_swapchain_vk_set_swap_interval(struct wined3d_swapchain_vk *swapchain_vk,
        unsigned int swap_interval)
{
    if (swap_interval > 1)
    {
        if (swap_interval <= 4)
            FIXME("Unsupported swap interval %u.\n", swap_interval);
        swap_interval = 1;
    }

    if (swapchain_vk->s.swap_interval == swap_interval)
        return;

    swapchain_vk->s.swap_interval = swap_interval;
    wined3d_swapchain_vk_recreate(swapchain_vk);
}

static VkResult wined3d_swapchain_vk_blit(struct wined3d_swapchain_vk *swapchain_vk,
        struct wined3d_context_vk *context_vk, const RECT *src_rect, const RECT *dst_rect, unsigned int swap_interval)
{
    struct wined3d_texture_vk *back_buffer_vk = wined3d_texture_vk(swapchain_vk->s.back_buffers[0]);
    struct wined3d_device_vk *device_vk = wined3d_device_vk(swapchain_vk->s.device);
    const struct wined3d_swapchain_desc *desc = &swapchain_vk->s.state.desc;
    const struct wined3d_vk_info *vk_info = context_vk->vk_info;
    VkCommandBuffer vk_command_buffer;
    VkImageSubresourceRange vk_range;
    VkPresentInfoKHR present_desc;
    unsigned int present_idx;
    VkImageLayout vk_layout;
    uint32_t image_idx;
    RECT dst_rect_tmp;
    VkImageBlit blit;
    VkFilter filter;
    VkResult vr;

    static const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    TRACE("swapchain_vk %p, context_vk %p, src_rect %s, dst_rect %s, swap_interval %u.\n",
            swapchain_vk, context_vk, wine_dbgstr_rect(src_rect), wine_dbgstr_rect(dst_rect), swap_interval);

    wined3d_swapchain_vk_set_swap_interval(swapchain_vk, swap_interval);

    present_idx = swapchain_vk->current++ % swapchain_vk->image_count;
    wined3d_context_vk_wait_command_buffer(context_vk, swapchain_vk->vk_semaphores[present_idx].command_buffer_id);
    if ((vr = VK_CALL(vkAcquireNextImageKHR(device_vk->vk_device, swapchain_vk->vk_swapchain, UINT64_MAX,
            swapchain_vk->vk_semaphores[present_idx].available, VK_NULL_HANDLE, &image_idx))) < 0)
    {
        WARN("Failed to acquire image, vr %s.\n", wined3d_debug_vkresult(vr));
        return vr;
    }

    if (dst_rect->right > swapchain_vk->width || dst_rect->bottom > swapchain_vk->height)
    {
        dst_rect_tmp = *dst_rect;
        if (dst_rect->right > swapchain_vk->width)
            dst_rect_tmp.right = swapchain_vk->width;
        if (dst_rect->bottom > swapchain_vk->height)
            dst_rect_tmp.bottom = swapchain_vk->height;
        dst_rect = &dst_rect_tmp;
    }
    filter = src_rect->right - src_rect->left != dst_rect->right - dst_rect->left
            || src_rect->bottom - src_rect->top != dst_rect->bottom - dst_rect->top
            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    vk_command_buffer = wined3d_context_vk_get_command_buffer(context_vk);

    wined3d_context_vk_end_current_render_pass(context_vk);

    vk_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_range.baseMipLevel = 0;
    vk_range.levelCount = 1;
    vk_range.baseArrayLayer = 0;
    vk_range.layerCount = 1;

    wined3d_context_vk_image_barrier(context_vk, vk_command_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            vk_access_mask_from_bind_flags(back_buffer_vk->t.resource.bind_flags),
            VK_ACCESS_TRANSFER_READ_BIT,
            back_buffer_vk->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            back_buffer_vk->image.vk_image, &vk_range);

    wined3d_context_vk_image_barrier(context_vk, vk_command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            swapchain_vk->vk_images[image_idx], &vk_range);

    blit.srcSubresource.aspectMask = vk_range.aspectMask;
    blit.srcSubresource.mipLevel = vk_range.baseMipLevel;
    blit.srcSubresource.baseArrayLayer = vk_range.baseArrayLayer;
    blit.srcSubresource.layerCount = vk_range.layerCount;
    blit.srcOffsets[0].x = src_rect->left;
    blit.srcOffsets[0].y = src_rect->top;
    blit.srcOffsets[0].z = 0;
    blit.srcOffsets[1].x = src_rect->right;
    blit.srcOffsets[1].y = src_rect->bottom;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = dst_rect->left;
    blit.dstOffsets[0].y = dst_rect->top;
    blit.dstOffsets[0].z = 0;
    blit.dstOffsets[1].x = dst_rect->right;
    blit.dstOffsets[1].y = dst_rect->bottom;
    blit.dstOffsets[1].z = 1;
    VK_CALL(vkCmdBlitImage(vk_command_buffer,
            back_buffer_vk->image.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapchain_vk->vk_images[image_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, filter));

    wined3d_context_vk_reference_texture(context_vk, back_buffer_vk);
    wined3d_context_vk_image_barrier(context_vk, vk_command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            swapchain_vk->vk_images[image_idx], &vk_range);

    if (desc->swap_effect == WINED3D_SWAP_EFFECT_DISCARD || desc->swap_effect == WINED3D_SWAP_EFFECT_FLIP_DISCARD)
        vk_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    else
        vk_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    wined3d_context_vk_image_barrier(context_vk, vk_command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            vk_access_mask_from_bind_flags(back_buffer_vk->t.resource.bind_flags),
            vk_layout, back_buffer_vk->layout,
            back_buffer_vk->image.vk_image, &vk_range);
    back_buffer_vk->bind_mask = 0;

    swapchain_vk->vk_semaphores[present_idx].command_buffer_id = context_vk->current_command_buffer.id;
    wined3d_context_vk_submit_command_buffer(context_vk,
            1, &swapchain_vk->vk_semaphores[present_idx].available, &wait_stage,
            1, &swapchain_vk->vk_semaphores[present_idx].presentable);

    present_desc.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_desc.pNext = NULL;
    present_desc.waitSemaphoreCount = 1;
    present_desc.pWaitSemaphores = &swapchain_vk->vk_semaphores[present_idx].presentable;
    present_desc.swapchainCount = 1;
    present_desc.pSwapchains = &swapchain_vk->vk_swapchain;
    present_desc.pImageIndices = &image_idx;
    present_desc.pResults = NULL;
    if ((vr = VK_CALL(vkQueuePresentKHR(device_vk->graphics_queue.vk_queue, &present_desc))))
        WARN("Present returned vr %s.\n", wined3d_debug_vkresult(vr));
    return vr;
}

static void wined3d_swapchain_vk_rotate(struct wined3d_swapchain *swapchain, struct wined3d_context_vk *context_vk)
{
    struct wined3d_texture_sub_resource *sub_resource;
    struct wined3d_texture_vk *texture, *texture_prev;
    struct wined3d_image_vk image0;
    VkDescriptorImageInfo vk_info0;
    VkImageLayout vk_layout0;
    uint32_t bind_mask0;
    DWORD locations0;
    unsigned int i;

    static const DWORD supported_locations = WINED3D_LOCATION_TEXTURE_RGB | WINED3D_LOCATION_RB_MULTISAMPLE;

    if (swapchain->state.desc.backbuffer_count < 2
            || wined3d_swapchain_keeps_back_buffers(swapchain))
        return;

    texture_prev = wined3d_texture_vk(swapchain->back_buffers[0]);

    /* Back buffer 0 is already in the draw binding. */
    image0 = texture_prev->image;
    vk_layout0 = texture_prev->layout;
    bind_mask0 = texture_prev->bind_mask;
    vk_info0 = texture_prev->default_image_info;
    locations0 = texture_prev->t.sub_resources[0].locations;

    for (i = 1; i < swapchain->state.desc.backbuffer_count; ++i)
    {
        texture = wined3d_texture_vk(swapchain->back_buffers[i]);
        sub_resource = &texture->t.sub_resources[0];

        if (!(sub_resource->locations & supported_locations))
            wined3d_texture_load_location(&texture->t, 0, &context_vk->c, texture->t.resource.draw_binding);

        texture_prev->image = texture->image;
        texture_prev->layout = texture->layout;
        texture_prev->bind_mask = texture->bind_mask;
        texture_prev->default_image_info = texture->default_image_info;

        wined3d_texture_validate_location(&texture_prev->t, 0, sub_resource->locations & supported_locations);
        wined3d_texture_invalidate_location(&texture_prev->t, 0, ~(sub_resource->locations & supported_locations));

        texture_prev = texture;
    }

    texture_prev->image = image0;
    texture_prev->layout = vk_layout0;
    texture_prev->bind_mask = bind_mask0;
    texture_prev->default_image_info = vk_info0;

    wined3d_texture_validate_location(&texture_prev->t, 0, locations0 & supported_locations);
    wined3d_texture_invalidate_location(&texture_prev->t, 0, ~(locations0 & supported_locations));

    device_invalidate_state(swapchain->device, STATE_FRAMEBUFFER);
}

static void swapchain_vk_present(struct wined3d_swapchain *swapchain, const RECT *src_rect,
        const RECT *dst_rect, unsigned int swap_interval, uint32_t flags)
{
    struct wined3d_swapchain_vk *swapchain_vk = wined3d_swapchain_vk(swapchain);
    struct wined3d_texture *back_buffer = swapchain->back_buffers[0];
    struct wined3d_context_vk *context_vk;
    VkResult vr;
    HRESULT hr;

    context_vk = wined3d_context_vk(context_acquire(swapchain->device, back_buffer, 0));

    if (!swapchain_vk->vk_swapchain || swapchain_present_is_partial_copy(swapchain, dst_rect))
    {
        swapchain_blit_gdi(swapchain, &context_vk->c, src_rect, dst_rect);
    }
    else
    {
        wined3d_texture_load_location(back_buffer, 0, &context_vk->c, back_buffer->resource.draw_binding);

        if ((vr = wined3d_swapchain_vk_blit(swapchain_vk, context_vk, src_rect, dst_rect, swap_interval)))
        {
            if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR)
            {
                if (FAILED(hr = wined3d_swapchain_vk_recreate(swapchain_vk)))
                    ERR("Failed to recreate swapchain, hr %#lx.\n", hr);
                else if (vr == VK_ERROR_OUT_OF_DATE_KHR && (vr = wined3d_swapchain_vk_blit(
                        swapchain_vk, context_vk, src_rect, dst_rect, swap_interval)))
                    ERR("Failed to blit image, vr %s.\n", wined3d_debug_vkresult(vr));
            }
            else
            {
                ERR("Failed to blit image, vr %s.\n", wined3d_debug_vkresult(vr));
            }
        }
    }

    wined3d_swapchain_vk_rotate(swapchain, context_vk);

    wined3d_texture_validate_location(swapchain->front_buffer, 0, WINED3D_LOCATION_DRAWABLE);
    wined3d_texture_invalidate_location(swapchain->front_buffer, 0, ~WINED3D_LOCATION_DRAWABLE);

    TRACE("Starting new frame.\n");

    context_release(&context_vk->c);
}

static const struct wined3d_swapchain_ops swapchain_vk_ops =
{
    swapchain_vk_present,
    swapchain_frontbuffer_updated,
};

static void swapchain_gdi_frontbuffer_updated(struct wined3d_swapchain *swapchain)
{
    struct wined3d_dc_info *front;
    POINT offset = {0, 0};
    RECT draw_rect;
    HWND window;
    HDC src_dc;

    TRACE("swapchain %p.\n", swapchain);

    front = &swapchain->front_buffer->dc_info[0];
    if (swapchain->palette)
        wined3d_palette_apply_to_dc(swapchain->palette, front->dc);

    if (swapchain->front_buffer->resource.map_count)
        ERR("Trying to blit a mapped surface.\n");

    TRACE("Copying surface %p to screen.\n", front);

    src_dc = front->dc;
    window = swapchain->win_handle;

    /* Front buffer coordinates are screen coordinates. Map them to the
     * destination window if not fullscreened. */
    if (swapchain->state.desc.windowed)
        ClientToScreen(window, &offset);

    TRACE("offset %s.\n", wine_dbgstr_point(&offset));

    SetRect(&draw_rect, 0, 0, swapchain->front_buffer->resource.width,
            swapchain->front_buffer->resource.height);
    IntersectRect(&draw_rect, &draw_rect, &swapchain->front_buffer_update);

    BitBlt(swapchain->dc, draw_rect.left - offset.x, draw_rect.top - offset.y,
            draw_rect.right - draw_rect.left, draw_rect.bottom - draw_rect.top,
            src_dc, draw_rect.left, draw_rect.top, SRCCOPY);

    SetRectEmpty(&swapchain->front_buffer_update);
}

static void swapchain_gdi_present(struct wined3d_swapchain *swapchain,
        const RECT *src_rect, const RECT *dst_rect, unsigned int swap_interval, uint32_t flags)
{
    struct wined3d_dc_info *front, *back;
    HBITMAP bitmap;
    void *heap_pointer;
    void *heap_memory;
    HDC dc;

    front = &swapchain->front_buffer->dc_info[0];
    back = &swapchain->back_buffers[0]->dc_info[0];

    /* Flip the surface data. */
    dc = front->dc;
    bitmap = front->bitmap;
    heap_pointer = swapchain->front_buffer->resource.heap_pointer;
    heap_memory = swapchain->front_buffer->resource.heap_memory;

    front->dc = back->dc;
    front->bitmap = back->bitmap;
    swapchain->front_buffer->resource.heap_pointer = swapchain->back_buffers[0]->resource.heap_pointer;
    swapchain->front_buffer->resource.heap_memory = swapchain->back_buffers[0]->resource.heap_memory;

    back->dc = dc;
    back->bitmap = bitmap;
    swapchain->back_buffers[0]->resource.heap_pointer = heap_pointer;
    swapchain->back_buffers[0]->resource.heap_memory = heap_memory;

    SetRect(&swapchain->front_buffer_update, 0, 0,
            swapchain->front_buffer->resource.width,
            swapchain->front_buffer->resource.height);
    swapchain_gdi_frontbuffer_updated(swapchain);
}

static const struct wined3d_swapchain_ops swapchain_no3d_ops =
{
    swapchain_gdi_present,
    swapchain_gdi_frontbuffer_updated,
};

static void wined3d_swapchain_apply_sample_count_override(const struct wined3d_swapchain *swapchain,
        enum wined3d_format_id format_id, enum wined3d_multisample_type *type, unsigned int *quality)
{
    const struct wined3d_adapter *adapter;
    const struct wined3d_format *format;
    enum wined3d_multisample_type t;

    if (wined3d_settings.sample_count == ~0u)
        return;

    adapter = swapchain->device->adapter;
    if (!(format = wined3d_get_format(adapter, format_id, WINED3D_BIND_RENDER_TARGET)))
        return;

    if ((t = min(wined3d_settings.sample_count, adapter->d3d_info.limits.sample_count)))
        while (!(format->multisample_types & 1u << (t - 1)))
            ++t;
    TRACE("Using sample count %u.\n", t);
    *type = t;
    *quality = 0;
}

HRESULT CDECL wined3d_swapchain_set_max_frame_latency(struct wined3d_swapchain *swapchain, unsigned int latency)
{
    TRACE("swapchain %p, latency %u.\n", swapchain, latency);

    if (!(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FRAME_LATENCY_WAITABLE_OBJECT))
        return WINED3DERR_INVALIDCALL;

    if (!latency)
        return WINED3DERR_INVALIDCALL;

    if (latency > swapchain->max_frame_latency)
    {
        if (!ReleaseSemaphore(swapchain->frame_latency_semaphore, latency - swapchain->max_frame_latency, NULL))
        {
            ERR("Failed to release semaphore, error %lu.\n", GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    swapchain->max_frame_latency = latency;
    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_get_max_frame_latency(struct wined3d_swapchain *swapchain, unsigned int *latency)
{
    TRACE("swapchain %p, latency %p.\n", swapchain, latency);

    if (!(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FRAME_LATENCY_WAITABLE_OBJECT))
        return WINED3DERR_INVALIDCALL;

    *latency = swapchain->max_frame_latency;
    return WINED3D_OK;
}

HANDLE CDECL wined3d_swapchain_get_frame_latency_waitable_object(struct wined3d_swapchain *swapchain)
{
    HANDLE handle;

    TRACE("swapchain %p.\n", swapchain);

    if (!(swapchain->state.desc.flags & WINED3D_SWAPCHAIN_FRAME_LATENCY_WAITABLE_OBJECT))
        return NULL;

    if (!DuplicateHandle(GetCurrentProcess(), swapchain->frame_latency_semaphore, GetCurrentProcess(),
            &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        ERR("Failed to duplicate handle, error %lu.\n", GetLastError());
        return NULL;
    }

    return handle;
}

static enum wined3d_format_id adapter_format_from_backbuffer_format(const struct wined3d_adapter *adapter,
        enum wined3d_format_id format_id)
{
    const struct wined3d_format *backbuffer_format;

    backbuffer_format = wined3d_get_format(adapter, format_id, WINED3D_BIND_RENDER_TARGET);
    return pixelformat_for_depth(backbuffer_format->byte_count * CHAR_BIT);
}

static HRESULT wined3d_swapchain_state_init(struct wined3d_swapchain_state *state,
        const struct wined3d_swapchain_desc *desc, HWND window, struct wined3d *wined3d,
        struct wined3d_swapchain_state_parent *parent)
{
    HRESULT hr;

    state->desc = *desc;

    if (FAILED(hr = wined3d_output_get_display_mode(desc->output, &state->original_mode, NULL)))
    {
        ERR("Failed to get current display mode, hr %#lx.\n", hr);
        return hr;
    }

    if (state->desc.windowed)
    {
        RECT client_rect;

        GetClientRect(window, &client_rect);
        TRACE("Client rect %s.\n", wine_dbgstr_rect(&client_rect));

        if (!state->desc.backbuffer_width)
        {
            state->desc.backbuffer_width = client_rect.right ? client_rect.right : 8;
            TRACE("Updating width to %u.\n", state->desc.backbuffer_width);
        }
        if (!state->desc.backbuffer_height)
        {
            state->desc.backbuffer_height = client_rect.bottom ? client_rect.bottom : 8;
            TRACE("Updating height to %u.\n", state->desc.backbuffer_height);
        }

        if (state->desc.backbuffer_format == WINED3DFMT_UNKNOWN)
        {
            state->desc.backbuffer_format = state->original_mode.format_id;
            TRACE("Updating format to %s.\n", debug_d3dformat(state->original_mode.format_id));
        }
    }
    else
    {
        if (desc->flags & WINED3D_SWAPCHAIN_ALLOW_MODE_SWITCH)
        {
            state->d3d_mode.width = desc->backbuffer_width;
            state->d3d_mode.height = desc->backbuffer_height;
            state->d3d_mode.format_id = adapter_format_from_backbuffer_format(desc->output->adapter,
                    desc->backbuffer_format);
            state->d3d_mode.refresh_rate = desc->refresh_rate;
            state->d3d_mode.scanline_ordering = WINED3D_SCANLINE_ORDERING_UNKNOWN;
        }
        else
        {
            state->d3d_mode = state->original_mode;
        }
    }

    GetWindowRect(window, &state->original_window_rect);
    state->wined3d = wined3d;
    state->device_window = window;
    state->desc.device_window = window;
    state->parent = parent;

    if (desc->flags & WINED3D_SWAPCHAIN_REGISTER_STATE)
        wined3d_swapchain_state_register(state);

    return hr;
}

static HRESULT swapchain_create_texture(struct wined3d_swapchain *swapchain,
        bool front, bool depth, struct wined3d_texture **texture)
{
    const struct wined3d_swapchain_desc *swapchain_desc = &swapchain->state.desc;
    struct wined3d_device *device = swapchain->device;
    struct wined3d_resource_desc texture_desc;
    uint32_t texture_flags = 0;
    HRESULT hr;

    texture_desc.resource_type = WINED3D_RTYPE_TEXTURE_2D;
    texture_desc.format = depth ? swapchain_desc->auto_depth_stencil_format : swapchain_desc->backbuffer_format;
    texture_desc.multisample_type = swapchain_desc->multisample_type;
    texture_desc.multisample_quality = swapchain_desc->multisample_quality;
    texture_desc.usage = 0;
    if (!depth && (device->wined3d->flags & WINED3D_NO3D))
        texture_desc.usage |= WINED3DUSAGE_OWNDC;
    if (device->wined3d->flags & WINED3D_NO3D)
        texture_desc.access = WINED3D_RESOURCE_ACCESS_CPU;
    else
        texture_desc.access = WINED3D_RESOURCE_ACCESS_GPU;
    if (!depth && (swapchain_desc->flags & WINED3D_SWAPCHAIN_LOCKABLE_BACKBUFFER) && !swapchain_desc->multisample_type)
        texture_desc.access |= WINED3D_RESOURCE_ACCESS_MAP_R | WINED3D_RESOURCE_ACCESS_MAP_W;
    texture_desc.width = swapchain_desc->backbuffer_width;
    texture_desc.height = swapchain_desc->backbuffer_height;
    texture_desc.depth = 1;
    texture_desc.size = 0;

    if (front)
        texture_desc.bind_flags = 0;
    else if (depth)
        texture_desc.bind_flags = WINED3D_BIND_DEPTH_STENCIL;
    else
        texture_desc.bind_flags = swapchain_desc->backbuffer_bind_flags;

    if (swapchain_desc->flags & WINED3D_SWAPCHAIN_GDI_COMPATIBLE)
        texture_flags |= WINED3D_TEXTURE_CREATE_GET_DC;

    if (FAILED(hr = wined3d_texture_create(device, &texture_desc, 1, 1,
            texture_flags, NULL, NULL, &wined3d_null_parent_ops, texture)))
    {
        WARN("Failed to create texture, hr %#lx.\n", hr);
        return hr;
    }

    if (!depth)
        wined3d_texture_set_swapchain(*texture, swapchain);

    return S_OK;
}

HRESULT wined3d_swapchain_desc_validate_flags(const struct wined3d_swapchain_desc *desc)
{
    /* d3d8 allows the lockable flag even though the backbuffer is not lockable. */
    if ((desc->flags & WINED3D_SWAPCHAIN_LOCKABLE_BACKBUFFER) && desc->multisample_type
            && !(desc->flags & WINED3D_SWAPCHAIN_ALLOW_MS_LOCKABLE_BACKBUFFER))
        return WINED3DERR_INVALIDCALL;

    return WINED3D_OK;
}

static HRESULT wined3d_swapchain_init(struct wined3d_swapchain *swapchain, struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops,
        const struct wined3d_swapchain_ops *swapchain_ops)
{
    struct wined3d_output_desc output_desc;
    BOOL displaymode_set = FALSE;
    HRESULT hr = E_FAIL;
    unsigned int i;
    HWND window;

    wined3d_mutex_lock();

    if (desc->swap_effect != WINED3D_SWAP_EFFECT_DISCARD
            && desc->swap_effect != WINED3D_SWAP_EFFECT_SEQUENTIAL
            && desc->swap_effect != WINED3D_SWAP_EFFECT_COPY)
        FIXME("Unimplemented swap effect %#x.\n", desc->swap_effect);

    if (FAILED(hr = wined3d_swapchain_desc_validate_flags(desc)))
        return hr;

    window = desc->device_window ? desc->device_window : device->create_parms.focus_window;
    TRACE("Using target window %p.\n", window);

    if (FAILED(hr = wined3d_swapchain_state_init(&swapchain->state, desc, window, device->wined3d, state_parent)))
    {
        ERR("Failed to initialise swapchain state, hr %#lx.\n", hr);
        wined3d_mutex_unlock();
        return hr;
    }

    swapchain->swapchain_ops = swapchain_ops;
    swapchain->device = device;
    swapchain->parent = parent;
    swapchain->parent_ops = parent_ops;
    swapchain->ref = 1;
    swapchain->win_handle = window;
    swapchain->swap_interval = WINED3D_SWAP_INTERVAL_DEFAULT;
    if (desc->flags & WINED3D_SWAPCHAIN_FRAME_LATENCY_WAITABLE_OBJECT)
        swapchain->max_frame_latency = 1;
    else
        swapchain->max_frame_latency = device->max_frame_latency;

    if (!(swapchain->frame_latency_semaphore = CreateSemaphoreW(NULL, swapchain->max_frame_latency, LONG_MAX, NULL)))
    {
        ERR("Failed to create frame latency semaphore, error %lu.\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    /* WS_CHILD and WS_POPUP swapchains keep the GDI path: dcomp composition
     * windows depend on it, and WS_POPUP windows (settings dialogs, context
     * menus) paint black on the GL path until first interaction (empirical,
     * shibco patch 0055). A swapchain that dxgi later marks FORCE_GDI_PRESENT
     * still takes the GDI branch at present time — swapchain_gl_present()
     * checks that flag first. Swapchains are routinely created before the
     * first ShowWindow(), so visibility is not part of the test.
     *
     * ID2D1HwndRenderTarget top-level windows stay on the GDI path too: on
     * the GL path their first present never becomes visible (the window
     * stays black until an expose forces a second present — resize, minimize
     * and restore both fix it, a plain move does not; issue 208).  d2d1 sets
     * __wine_d3d_hwnd_target on the window before creating the swapchain,
     * so the marker is already there when we decide.  The GDI path renders
     * these windows correctly from the first present on. */
    if (!wined3d_gl_present_disabled() && window)
    {
        LONG style = GetWindowLongW(window, GWL_STYLE);

        if (!(style & (WS_CHILD | WS_POPUP))
                && !GetPropW(window, L"__wine_d3d_hwnd_target"))
        {
            swapchain->state.desc.flags |= WINED3D_SWAPCHAIN_PREFER_GL_PRESENT;
            TRACE("Preferring GL present for top-level window %p (style %#lx).\n", window, style);
        }
    }

    if (!(swapchain->dc = GetDCEx(swapchain->win_handle, 0, DCX_USESTYLE | DCX_CACHE)))
        WARN("Failed to retrieve device context, trying swapchain backup.\n");

    if (!swapchain->state.desc.windowed)
    {
        if (FAILED(hr = wined3d_output_get_desc(desc->output, &output_desc)))
        {
            ERR("Failed to get output description, hr %#lx.\n", hr);
            goto err;
        }

        wined3d_swapchain_state_setup_fullscreen(&swapchain->state, window,
                output_desc.desktop_rect.left, output_desc.desktop_rect.top, desc->backbuffer_width,
                desc->backbuffer_height);
    }
    wined3d_swapchain_apply_sample_count_override(swapchain, swapchain->state.desc.backbuffer_format,
            &swapchain->state.desc.multisample_type, &swapchain->state.desc.multisample_quality);

    TRACE("Creating front buffer.\n");

    if (FAILED(hr = swapchain_create_texture(swapchain, true, false, &swapchain->front_buffer)))
    {
        WARN("Failed to create front buffer, hr %#lx.\n", hr);
        goto err;
    }

    if (!(device->wined3d->flags & WINED3D_NO3D))
    {
        wined3d_texture_validate_location(swapchain->front_buffer, 0, WINED3D_LOCATION_DRAWABLE);
        wined3d_texture_invalidate_location(swapchain->front_buffer, 0, ~WINED3D_LOCATION_DRAWABLE);
    }

    /* MSDN says we're only allowed a single fullscreen swapchain per device,
     * so we should really check to see if there is a fullscreen swapchain
     * already. Does a single head count as full screen? */
    if (!desc->windowed && desc->flags & WINED3D_SWAPCHAIN_ALLOW_MODE_SWITCH)
    {
        /* Change the display settings */
        if (FAILED(hr = wined3d_output_set_display_mode(desc->output,
                &swapchain->state.d3d_mode)))
        {
            WARN("Failed to set display mode, hr %#lx.\n", hr);
            goto err;
        }
        displaymode_set = TRUE;
    }

    if (swapchain->state.desc.backbuffer_count > 0)
    {
        if (!(swapchain->back_buffers = calloc(swapchain->state.desc.backbuffer_count,
                sizeof(*swapchain->back_buffers))))
        {
            ERR("Failed to allocate backbuffer array memory.\n");
            hr = E_OUTOFMEMORY;
            goto err;
        }

        for (i = 0; i < swapchain->state.desc.backbuffer_count; ++i)
        {
            TRACE("Creating back buffer %u.\n", i);
            if (FAILED(hr = swapchain_create_texture(swapchain, false, false, &swapchain->back_buffers[i])))
            {
                WARN("Failed to create back buffer %u, hr %#lx.\n", i, hr);
                swapchain->state.desc.backbuffer_count = i;
                goto err;
            }
        }
    }

    /* Swapchains share the depth/stencil buffer, so only create a single depthstencil surface. */
    if (desc->enable_auto_depth_stencil)
    {
        TRACE("Creating depth/stencil buffer.\n");
        if (!device->auto_depth_stencil_view)
        {
            struct wined3d_view_desc desc;
            struct wined3d_texture *ds;

            if (FAILED(hr = swapchain_create_texture(swapchain, false, true, &ds)))
            {
                WARN("Failed to create the auto depth/stencil surface, hr %#lx.\n", hr);
                goto err;
            }

            desc.format_id = ds->resource.format->id;
            desc.flags = 0;
            desc.u.texture.level_idx = 0;
            desc.u.texture.level_count = 1;
            desc.u.texture.layer_idx = 0;
            desc.u.texture.layer_count = 1;
            hr = wined3d_rendertarget_view_create(&desc, &ds->resource, NULL, &wined3d_null_parent_ops,
                    &device->auto_depth_stencil_view);
            wined3d_texture_decref(ds);
            if (FAILED(hr))
            {
                ERR("Failed to create rendertarget view, hr %#lx.\n", hr);
                goto err;
            }
        }
    }

    wined3d_swapchain_get_gamma_ramp(swapchain, &swapchain->orig_gamma);

    wined3d_mutex_unlock();

    return WINED3D_OK;

err:
    if (displaymode_set)
    {
        if (FAILED(wined3d_restore_display_modes(device->wined3d)))
            ERR("Failed to restore display mode.\n");
    }

    if (swapchain->back_buffers)
    {
        for (i = 0; i < swapchain->state.desc.backbuffer_count; ++i)
        {
            if (swapchain->back_buffers[i])
            {
                wined3d_texture_set_swapchain(swapchain->back_buffers[i], NULL);
                wined3d_texture_decref(swapchain->back_buffers[i]);
            }
        }
        free(swapchain->back_buffers);
    }

    if (swapchain->front_buffer)
    {
        wined3d_texture_set_swapchain(swapchain->front_buffer, NULL);
        wined3d_texture_decref(swapchain->front_buffer);
    }

    if (swapchain->dc)
        wined3d_release_dc(swapchain->win_handle, swapchain->dc);

    CloseHandle(swapchain->frame_latency_semaphore);

    wined3d_swapchain_state_cleanup(&swapchain->state);
    wined3d_mutex_unlock();

    return hr;
}

HRESULT wined3d_swapchain_no3d_init(struct wined3d_swapchain *swapchain_no3d, struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops)
{
    TRACE("swapchain_no3d %p, device %p, desc %p, state_parent %p, parent %p, parent_ops %p.\n",
            swapchain_no3d, device, desc, state_parent, parent, parent_ops);

    return wined3d_swapchain_init(swapchain_no3d, device, desc, state_parent, parent, parent_ops,
            &swapchain_no3d_ops);
}

HRESULT wined3d_swapchain_gl_init(struct wined3d_swapchain_gl *swapchain_gl, struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops)
{
    HRESULT hr;

    TRACE("swapchain_gl %p, device %p, desc %p, state_parent %p, parent %p, parent_ops %p.\n",
            swapchain_gl, device, desc, state_parent, parent, parent_ops);

    if (FAILED(hr = wined3d_swapchain_init(&swapchain_gl->s, device, desc, state_parent, parent,
            parent_ops, &swapchain_gl_ops)))
        return hr;

    /* This swapchain composites in both its present branches, so tell dcomp it
     * may publish a layer for this window (issue 206).  A Vulkan swapchain does
     * not, and deliberately does not say so. */
    wined3d_swapchain_set_layer_sink(&swapchain_gl->s, TRUE);
    return hr;
}

HRESULT wined3d_swapchain_vk_init(struct wined3d_swapchain_vk *swapchain_vk, struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops)
{
    HRESULT hr;

    TRACE("swapchain_vk %p, device %p, desc %p, parent %p, parent_ops %p.\n",
            swapchain_vk, device, desc, parent, parent_ops);

    if (FAILED(hr = wined3d_swapchain_init(&swapchain_vk->s, device, desc, state_parent, parent,
            parent_ops, &swapchain_vk_ops)))
        return hr;

    if (swapchain_vk->s.win_handle == GetDesktopWindow())
    {
        WARN("Creating a desktop window swapchain.\n");
        return hr;
    }

    if (FAILED(hr = wined3d_swapchain_vk_create_vulkan_swapchain(swapchain_vk)))
    {
        WARN("Failed to create a Vulkan swapchain, hr %#lx.\n", hr);
        return hr;
    }

    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_create(struct wined3d_device *device,
        const struct wined3d_swapchain_desc *desc, struct wined3d_swapchain_state_parent *state_parent,
        void *parent, const struct wined3d_parent_ops *parent_ops,
        struct wined3d_swapchain **swapchain)
{
    struct wined3d_swapchain *object;
    HRESULT hr;

    if (FAILED(hr = device->adapter->adapter_ops->adapter_create_swapchain(device,
            desc, state_parent, parent, parent_ops, &object)))
        return hr;

    if (desc->flags & WINED3D_SWAPCHAIN_IMPLICIT)
    {
        wined3d_mutex_lock();
        if (FAILED(hr = wined3d_device_set_implicit_swapchain(device, object)))
        {
            wined3d_cs_finish(device->cs, WINED3D_CS_QUEUE_DEFAULT);
            device->adapter->adapter_ops->adapter_destroy_swapchain(object);
            wined3d_mutex_unlock();
            return hr;
        }
        wined3d_mutex_unlock();
    }

    *swapchain = object;

    return hr;
}

static struct wined3d_context_gl *wined3d_swapchain_gl_create_context(struct wined3d_swapchain_gl *swapchain_gl)
{
    struct wined3d_device *device = swapchain_gl->s.device;
    struct wined3d_context_gl *context_gl;

    TRACE("Creating a new context for swapchain %p, thread %lu.\n", swapchain_gl, GetCurrentThreadId());

    wined3d_from_cs(device->cs);

    if (!(context_gl = calloc(1, sizeof(*context_gl))))
    {
        ERR("Failed to allocate context memory.\n");
        return NULL;
    }

    if (FAILED(wined3d_context_gl_init(context_gl, swapchain_gl)))
    {
        WARN("Failed to initialise context.\n");
        free(context_gl);
        return NULL;
    }

    if (!device_context_add(device, &context_gl->c))
    {
        ERR("Failed to add the newly created context to the context list.\n");
        wined3d_context_gl_destroy(context_gl);
        return NULL;
    }

    TRACE("Created context %p.\n", context_gl);

    context_release(&context_gl->c);

    return context_gl;
}

struct wined3d_context_gl *wined3d_swapchain_gl_get_context(struct wined3d_swapchain_gl *swapchain_gl)
{
    struct wined3d_device *device = swapchain_gl->s.device;
    DWORD tid = GetCurrentThreadId();
    unsigned int i;

    for (i = 0; i < device->context_count; ++i)
    {
        if (wined3d_context_gl(device->contexts[i])->tid == tid)
            return wined3d_context_gl(device->contexts[i]);
    }

    /* Create a new context for the thread. */
    return wined3d_swapchain_gl_create_context(swapchain_gl);
}

void swapchain_update_draw_bindings(struct wined3d_swapchain *swapchain)
{
    UINT i;

    wined3d_resource_update_draw_binding(&swapchain->front_buffer->resource);

    for (i = 0; i < swapchain->state.desc.backbuffer_count; ++i)
    {
        wined3d_resource_update_draw_binding(&swapchain->back_buffers[i]->resource);
    }
}

void wined3d_swapchain_activate(struct wined3d_swapchain *swapchain, BOOL activate)
{
    struct wined3d_device *device = swapchain->device;
    HWND window = swapchain->state.device_window;
    struct wined3d_output_desc output_desc;
    unsigned int screensaver_active;
    struct wined3d_output *output;
    BOOL focus_messages, filter;
    HRESULT hr;

    /* This code is not protected by the wined3d mutex, so it may run while
     * wined3d_device_reset is active. Testing on Windows shows that changing
     * focus during resets and resetting during focus change events causes
     * the application to crash with an invalid memory access. */

    if (!(focus_messages = device->wined3d->flags & WINED3D_FOCUS_MESSAGES))
        filter = wined3d_filter_messages(window, TRUE);

    if (activate)
    {
        SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &screensaver_active, 0);
        if ((device->restore_screensaver = !!screensaver_active))
            SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);

        if (!(device->create_parms.flags & WINED3DCREATE_NOWINDOWCHANGES))
        {
            /* The d3d versions do not agree on the exact messages here. D3d8 restores
             * the window but leaves the size untouched, d3d9 sets the size on an
             * invisible window, generates messages but doesn't change the window
             * properties. The implementation follows d3d9.
             *
             * Guild Wars 1 wants a WINDOWPOSCHANGED message on the device window to
             * resume drawing after a focus loss. */
            output = wined3d_swapchain_get_output(swapchain);
            if (!output)
            {
                ERR("Failed to get output from swapchain %p.\n", swapchain);
                return;
            }

            if (SUCCEEDED(hr = wined3d_output_get_desc(output, &output_desc)))
                SetWindowPos(window, NULL, output_desc.desktop_rect.left,
                        output_desc.desktop_rect.top, swapchain->state.desc.backbuffer_width,
                        swapchain->state.desc.backbuffer_height, SWP_NOACTIVATE | SWP_NOZORDER);
            else
                ERR("Failed to get output description, hr %#lx.\n", hr);
        }

        if (device->wined3d->flags & WINED3D_RESTORE_MODE_ON_ACTIVATE)
        {
            output = wined3d_swapchain_get_output(swapchain);
            if (!output)
            {
                ERR("Failed to get output from swapchain %p.\n", swapchain);
                return;
            }

            if (FAILED(hr = wined3d_output_set_display_mode(output,
                    &swapchain->state.d3d_mode)))
                ERR("Failed to set display mode, hr %#lx.\n", hr);
        }

        if (swapchain == device->swapchains[0])
            device->device_parent->ops->activate(device->device_parent, TRUE);
    }
    else
    {
        if (device->restore_screensaver)
        {
            SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, NULL, 0);
            device->restore_screensaver = FALSE;
        }

        if (FAILED(hr = wined3d_restore_display_modes(device->wined3d)))
            ERR("Failed to restore display modes, hr %#lx.\n", hr);

        swapchain->reapply_mode = TRUE;

        /* Some DDraw apps (Deus Ex: GOTY, and presumably all UT 1 based games) destroy the device
         * during window minimization. Do our housekeeping now, as the device may not exist after
         * the ShowWindow call.
         *
         * In d3d9, the device is marked lost after the window is minimized. If we find an app
         * that needs this behavior (e.g. because it calls TestCooperativeLevel in the window proc)
         * we'll have to control this via a create flag. Note that the device and swapchain are not
         * safe to access after the ShowWindow call. */
        if (swapchain == device->swapchains[0])
            device->device_parent->ops->activate(device->device_parent, FALSE);

        if (!(device->create_parms.flags & WINED3DCREATE_NOWINDOWCHANGES) && IsWindowVisible(window))
            ShowWindow(window, SW_MINIMIZE);
    }

    if (!focus_messages)
        wined3d_filter_messages(window, filter);
}

HRESULT CDECL wined3d_swapchain_resize_buffers(struct wined3d_swapchain *swapchain, unsigned int buffer_count,
        unsigned int width, unsigned int height, enum wined3d_format_id format_id,
        enum wined3d_multisample_type multisample_type, unsigned int multisample_quality,
        unsigned int flags)
{
    struct wined3d_swapchain_desc *desc = &swapchain->state.desc;
    bool recreate = false;

    TRACE("swapchain %p, buffer_count %u, width %u, height %u, format %s, "
            "multisample_type %#x, multisample_quality %#x.\n",
            swapchain, buffer_count, width, height, debug_d3dformat(format_id),
            multisample_type, multisample_quality);

    wined3d_swapchain_apply_sample_count_override(swapchain, format_id, &multisample_type, &multisample_quality);

    if (buffer_count && buffer_count != desc->backbuffer_count)
        FIXME("Cannot change the back buffer count yet.\n");

    wined3d_cs_finish(swapchain->device->cs, WINED3D_CS_QUEUE_DEFAULT);

    if (!width || !height)
    {
        RECT client_rect;

        /* The application is requesting that either the swapchain width or
         * height be set to the corresponding dimension in the window's
         * client rect. */

        if (!GetClientRect(swapchain->state.device_window, &client_rect))
        {
            ERR("Failed to get client rect, last error %#lx.\n", GetLastError());
            return WINED3DERR_INVALIDCALL;
        }

        if (!width)
            width = client_rect.right;

        if (!height)
            height = client_rect.bottom;
    }

    if (width != desc->backbuffer_width || height != desc->backbuffer_height)
    {
        desc->backbuffer_width = width;
        desc->backbuffer_height = height;
        recreate = true;
    }

    if (format_id == WINED3DFMT_UNKNOWN)
    {
        if (!desc->windowed)
            return WINED3DERR_INVALIDCALL;
        format_id = swapchain->state.original_mode.format_id;
    }

    if (format_id != desc->backbuffer_format)
    {
        desc->backbuffer_format = format_id;
        recreate = true;
    }

    if (multisample_type != desc->multisample_type
            || multisample_quality != desc->multisample_quality)
    {
        desc->multisample_type = multisample_type;
        desc->multisample_quality = multisample_quality;
        recreate = true;
    }

    if (flags)
    {
        if ((desc->flags ^ flags) & WINED3D_SWAPCHAIN_GDI_COMPATIBLE)
            recreate = true;
        desc->flags = flags;
    }

    if (recreate)
    {
        struct wined3d_texture *new_texture;
        HRESULT hr;
        UINT i;

        TRACE("Recreating swapchain textures.\n");

        if (FAILED(hr = swapchain_create_texture(swapchain, true, false, &new_texture)))
            return hr;
        wined3d_texture_set_swapchain(swapchain->front_buffer, NULL);
        if (wined3d_texture_decref(swapchain->front_buffer))
            ERR("Something's still holding the front buffer (%p).\n", swapchain->front_buffer);
        swapchain->front_buffer = new_texture;

        if (!(swapchain->device->wined3d->flags & WINED3D_NO3D))
        {
            wined3d_texture_validate_location(swapchain->front_buffer, 0, WINED3D_LOCATION_DRAWABLE);
            wined3d_texture_invalidate_location(swapchain->front_buffer, 0, ~WINED3D_LOCATION_DRAWABLE);
        }

        for (i = 0; i < desc->backbuffer_count; ++i)
        {
            if (FAILED(hr = swapchain_create_texture(swapchain, false, false, &new_texture)))
                return hr;
            wined3d_texture_set_swapchain(swapchain->back_buffers[i], NULL);
            if (wined3d_texture_decref(swapchain->back_buffers[i]))
                ERR("Something's still holding back buffer %u (%p).\n", i, swapchain->back_buffers[i]);
            swapchain->back_buffers[i] = new_texture;
        }
    }

    swapchain_update_draw_bindings(swapchain);

    return WINED3D_OK;
}

static HRESULT wined3d_swapchain_state_set_display_mode(struct wined3d_swapchain_state *state,
        struct wined3d_output *output, struct wined3d_display_mode *mode)
{
    HRESULT hr;

    if (state->desc.flags & WINED3D_SWAPCHAIN_USE_CLOSEST_MATCHING_MODE)
    {
        if (FAILED(hr = wined3d_output_find_closest_matching_mode(output, mode)))
        {
            WARN("Failed to find closest matching mode, hr %#lx.\n", hr);
        }
    }

    if (output != state->desc.output)
    {
        if (FAILED(hr = wined3d_restore_display_modes(state->wined3d)))
        {
            WARN("Failed to restore display modes, hr %#lx.\n", hr);
            return hr;
        }

        if (FAILED(hr = wined3d_output_get_display_mode(output, &state->original_mode, NULL)))
        {
            WARN("Failed to get current display mode, hr %#lx.\n", hr);
            return hr;
        }
    }

    if (FAILED(hr = wined3d_output_set_display_mode(output, mode)))
    {
        WARN("Failed to set display mode, hr %#lx.\n", hr);
        return WINED3DERR_INVALIDCALL;
    }

    return WINED3D_OK;
}

HRESULT CDECL wined3d_swapchain_state_resize_target(struct wined3d_swapchain_state *state,
        const struct wined3d_display_mode *mode)
{
    struct wined3d_display_mode actual_mode;
    struct wined3d_output_desc output_desc;
    RECT original_window_rect, window_rect;
    int x, y, width, height;
    HWND window;
    HRESULT hr;

    TRACE("state %p, mode %p.\n", state, mode);

    wined3d_mutex_lock();

    window = state->device_window;

    if (state->desc.windowed)
    {
        SetRect(&window_rect, 0, 0, mode->width, mode->height);
        AdjustWindowRectEx(&window_rect,
                GetWindowLongW(window, GWL_STYLE), FALSE,
                GetWindowLongW(window, GWL_EXSTYLE));
        GetWindowRect(window, &original_window_rect);

        x = original_window_rect.left;
        y = original_window_rect.top;
        width = window_rect.right - window_rect.left;
        height = window_rect.bottom - window_rect.top;
    }
    else
    {
        if (FAILED(hr = wined3d_output_get_desc(state->desc.output, &output_desc)))
        {
            ERR("Failed to get output description, hr %#lx.\n", hr);
            wined3d_mutex_unlock();
            return hr;
        }
        width = output_desc.desktop_rect.right - output_desc.desktop_rect.left;
        height = output_desc.desktop_rect.bottom - output_desc.desktop_rect.top;

        GetWindowRect(window, &window_rect);
        if (width != window_rect.right - window_rect.left || height != window_rect.bottom - window_rect.top)
        {
            TRACE("Update saved window state.\n");
            state->original_window_rect = window_rect;
        }

        if (state->desc.flags & WINED3D_SWAPCHAIN_ALLOW_MODE_SWITCH)
        {
            actual_mode = *mode;
            if (FAILED(hr = wined3d_swapchain_state_set_display_mode(state, state->desc.output,
                    &actual_mode)))
            {
                ERR("Failed to set display mode, hr %#lx.\n", hr);
                wined3d_mutex_unlock();
                return hr;
            }
            if (FAILED(hr = wined3d_output_get_desc(state->desc.output, &output_desc)))
            {
                ERR("Failed to get output description, hr %#lx.\n", hr);
                wined3d_mutex_unlock();
                return hr;
            }

            width = output_desc.desktop_rect.right - output_desc.desktop_rect.left;
            height = output_desc.desktop_rect.bottom - output_desc.desktop_rect.top;
        }
        x = output_desc.desktop_rect.left;
        y = output_desc.desktop_rect.top;
    }

    wined3d_mutex_unlock();

    MoveWindow(window, x, y, width, height, TRUE);

    return WINED3D_OK;
}

static LONG fullscreen_style(LONG style)
{
    /* Make sure the window is managed, otherwise we won't get keyboard input. */
    style |= WS_POPUP | WS_SYSMENU;
    style &= ~(WS_CAPTION | WS_THICKFRAME);

    return style;
}

static LONG fullscreen_exstyle(LONG exstyle)
{
    /* Filter out window decorations. */
    exstyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);

    return exstyle;
}

struct wined3d_window_state
{
    HWND window;
    HWND window_pos_after;
    LONG style, exstyle;
    int x, y, width, height;
    uint32_t flags;
    bool set_style;
    bool register_topmost_timer;
    bool set_topmost_timer;
};

#define WINED3D_WINDOW_TOPMOST_TIMER_ID 0x4242

static void CALLBACK topmost_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    if (!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    KillTimer(hwnd, WINED3D_WINDOW_TOPMOST_TIMER_ID);
}

static DWORD WINAPI set_window_state_thread(void *ctx)
{
    struct wined3d_window_state *s = ctx;
    bool filter;

    filter = wined3d_filter_messages(s->window, TRUE);

    if (s->set_style)
    {
        SetWindowLongW(s->window, GWL_STYLE, s->style);
        SetWindowLongW(s->window, GWL_EXSTYLE, s->exstyle);
    }
    SetWindowPos(s->window, s->window_pos_after, s->x, s->y, s->width, s->height, s->flags);

    wined3d_filter_messages(s->window, filter);

    free(s);

    return 0;
}

static void set_window_state(struct wined3d_window_state *s)
{
    static const UINT timeout = 1500;
    DWORD window_tid = GetWindowThreadProcessId(s->window, NULL);
    DWORD tid = GetCurrentThreadId();
    HANDLE thread;

    TRACE("Window %p belongs to thread %#lx.\n", s->window, window_tid);
    /* If the window belongs to a different thread, modifying the style and/or
     * position can potentially deadlock if that thread isn't processing
     * messages. */
    if (window_tid == tid)
    {
        /* Deus Ex: Game of the Year Edition removes WS_EX_TOPMOST after changing resolutions in
         * exclusive fullscreen mode. Tests show that WS_EX_TOPMOST will be restored when a ~1.5s
         * timer times out */
        if (s->register_topmost_timer)
        {
            if (s->set_topmost_timer)
                SetTimer(s->window, WINED3D_WINDOW_TOPMOST_TIMER_ID, timeout, topmost_timer_proc);
            else
                KillTimer(s->window, WINED3D_WINDOW_TOPMOST_TIMER_ID);
        }

        set_window_state_thread(s);
    }
    else if ((thread = CreateThread(NULL, 0, set_window_state_thread, s, 0, NULL)))
    {
        SetThreadDescription(thread, L"wined3d_set_window_state");
        CloseHandle(thread);
    }
    else
    {
        ERR("Failed to create thread.\n");
    }
}

HRESULT wined3d_swapchain_state_setup_fullscreen(struct wined3d_swapchain_state *state,
        HWND window, int x, int y, int width, int height)
{
    struct wined3d_window_state *s;

    TRACE("Setting up window %p for fullscreen mode.\n", window);

    if (!IsWindow(window))
    {
        WARN("%p is not a valid window.\n", window);
        return WINED3DERR_NOTAVAILABLE;
    }

    set_window_present_rect(window, x, y, width, height);

    if (!(s = malloc(sizeof(*s))))
        return E_OUTOFMEMORY;
    s->window = window;
    s->window_pos_after = HWND_TOPMOST;
    s->x = x;
    s->y = y;
    s->width = width;
    s->height = height;

    if (state->style || state->exstyle)
    {
        ERR("Changing the window style for window %p, but another style (%08lx, %08lx) is already stored.\n",
                window, state->style, state->exstyle);
    }

    s->flags = SWP_FRAMECHANGED | SWP_NOACTIVATE;
    if (state->desc.flags & WINED3D_SWAPCHAIN_NO_WINDOW_CHANGES)
        s->flags |= SWP_NOZORDER;
    else
        s->flags |= SWP_SHOWWINDOW;

    state->style = GetWindowLongW(window, GWL_STYLE);
    state->exstyle = GetWindowLongW(window, GWL_EXSTYLE);

    s->style = fullscreen_style(state->style);
    s->exstyle = fullscreen_exstyle(state->exstyle);
    s->set_style = true;
    s->register_topmost_timer = !!(state->desc.flags & WINED3D_SWAPCHAIN_REGISTER_TOPMOST_TIMER);
    s->set_topmost_timer = true;

    TRACE("Old style was %08lx, %08lx, setting to %08lx, %08lx.\n",
            state->style, state->exstyle, s->style, s->exstyle);

    set_window_state(s);
    return WINED3D_OK;
}

void wined3d_swapchain_state_restore_from_fullscreen(struct wined3d_swapchain_state *state,
        HWND window, const RECT *window_rect)
{
    struct wined3d_window_state *s;
    LONG style, exstyle;

    set_window_present_rect(window, 0, 0, 0, 0);

    if (!state->style && !state->exstyle)
        return;

    if (!(s = malloc(sizeof(*s))))
        return;

    s->window = window;
    s->window_pos_after = NULL;
    s->flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE;

    if ((state->desc.flags & WINED3D_SWAPCHAIN_RESTORE_WINDOW_STATE)
            && !(state->desc.flags & WINED3D_SWAPCHAIN_NO_WINDOW_CHANGES))
    {
        s->window_pos_after = (state->exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST;
        s->flags |= (state->style & WS_VISIBLE) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        s->flags &= ~SWP_NOZORDER;
    }

    style = GetWindowLongW(window, GWL_STYLE);
    exstyle = GetWindowLongW(window, GWL_EXSTYLE);

    /* These flags are set by wined3d_device_setup_fullscreen_window, not the
     * application, and we want to ignore them in the test below, since it's
     * not the application's fault that they changed. Additionally, we want to
     * preserve the current status of these flags (i.e. don't restore them) to
     * more closely emulate the behavior of Direct3D, which leaves these flags
     * alone when returning to windowed mode. */
    state->style ^= (state->style ^ style) & WS_VISIBLE;
    state->exstyle ^= (state->exstyle ^ exstyle) & WS_EX_TOPMOST;

    TRACE("Restoring window style of window %p to %08lx, %08lx.\n",
            window, state->style, state->exstyle);

    s->style = state->style;
    s->exstyle = state->exstyle;
    /* Only restore the style if the application didn't modify it during the
     * fullscreen phase. Some applications change it before calling Reset()
     * when switching between windowed and fullscreen modes (HL2), some
     * depend on the original style (Eve Online). */
    s->set_style = style == fullscreen_style(state->style) && exstyle == fullscreen_exstyle(state->exstyle);
    s->register_topmost_timer = !!(state->desc.flags & WINED3D_SWAPCHAIN_REGISTER_TOPMOST_TIMER);
    s->set_topmost_timer = false;

    if (window_rect)
    {
        s->x = window_rect->left;
        s->y = window_rect->top;
        s->width = window_rect->right - window_rect->left;
        s->height = window_rect->bottom - window_rect->top;
    }
    else
    {
        s->x = s->y = s->width = s->height = 0;
        s->flags |= (SWP_NOMOVE | SWP_NOSIZE);
    }

    set_window_state(s);

    /* Delete the old values. */
    state->style = 0;
    state->exstyle = 0;
}

HRESULT CDECL wined3d_swapchain_state_set_fullscreen(struct wined3d_swapchain_state *state,
        const struct wined3d_swapchain_desc *swapchain_desc,
        const struct wined3d_display_mode *mode)
{
    struct wined3d_display_mode actual_mode;
    struct wined3d_output_desc output_desc;
    BOOL windowed = state->desc.windowed;
    HRESULT hr;

    TRACE("state %p, swapchain_desc %p, mode %p.\n", state, swapchain_desc, mode);

    if (state->desc.flags & WINED3D_SWAPCHAIN_ALLOW_MODE_SWITCH)
    {
        if (mode)
        {
            actual_mode = *mode;
            if (FAILED(hr = wined3d_swapchain_state_set_display_mode(state, swapchain_desc->output,
                    &actual_mode)))
                return hr;
        }
        else
        {
            if (!swapchain_desc->windowed)
            {
                actual_mode.width = swapchain_desc->backbuffer_width;
                actual_mode.height = swapchain_desc->backbuffer_height;
                actual_mode.refresh_rate = swapchain_desc->refresh_rate;
                actual_mode.format_id = adapter_format_from_backbuffer_format(swapchain_desc->output->adapter,
                        swapchain_desc->backbuffer_format);
                actual_mode.scanline_ordering = WINED3D_SCANLINE_ORDERING_UNKNOWN;
                if (FAILED(hr = wined3d_swapchain_state_set_display_mode(state, swapchain_desc->output,
                        &actual_mode)))
                    return hr;
            }
            else
            {
                if (FAILED(hr = wined3d_restore_display_modes(state->wined3d)))
                {
                    WARN("Failed to restore display modes for all outputs, hr %#lx.\n", hr);
                    return hr;
                }
            }
        }
    }
    else
    {
        if (mode)
            WARN("WINED3D_SWAPCHAIN_ALLOW_MODE_SWITCH is not set, ignoring mode.\n");

        if (FAILED(hr = wined3d_output_get_display_mode(swapchain_desc->output, &actual_mode,
                NULL)))
        {
            ERR("Failed to get display mode, hr %#lx.\n", hr);
            return WINED3DERR_INVALIDCALL;
        }
    }

    if (!swapchain_desc->windowed)
    {
        unsigned int width = actual_mode.width;
        unsigned int height = actual_mode.height;

        if (FAILED(hr = wined3d_output_get_desc(swapchain_desc->output, &output_desc)))
        {
            ERR("Failed to get output description, hr %#lx.\n", hr);
            return hr;
        }

        if (state->desc.windowed)
        {
            /* Switch from windowed to fullscreen */
            if (FAILED(hr = wined3d_swapchain_state_setup_fullscreen(state, state->device_window,
                    output_desc.desktop_rect.left, output_desc.desktop_rect.top, width, height)))
                return hr;
        }
        else
        {
            HWND window = state->device_window;
            BOOL filter;

            set_window_present_rect(state->device_window, output_desc.desktop_rect.left,
                    output_desc.desktop_rect.top, width, height);

            /* Fullscreen -> fullscreen mode change */
            filter = wined3d_filter_messages(window, TRUE);
            MoveWindow(window, output_desc.desktop_rect.left, output_desc.desktop_rect.top, width,
                    height, TRUE);
            ShowWindow(window, SW_SHOW);
            wined3d_filter_messages(window, filter);
        }
        state->d3d_mode = actual_mode;
    }
    else if (!state->desc.windowed)
    {
        /* Fullscreen -> windowed switch */
        RECT *window_rect = NULL;
        if (state->desc.flags & WINED3D_SWAPCHAIN_RESTORE_WINDOW_RECT)
            window_rect = &state->original_window_rect;
        wined3d_swapchain_state_restore_from_fullscreen(state, state->device_window, window_rect);
    }

    state->desc.output = swapchain_desc->output;
    state->desc.windowed = swapchain_desc->windowed;

    if (windowed != state->desc.windowed)
        state->parent->ops->windowed_state_changed(state->parent, state->desc.windowed);

    return WINED3D_OK;
}

BOOL CDECL wined3d_swapchain_state_is_windowed(const struct wined3d_swapchain_state *state)
{
    TRACE("state %p.\n", state);

    return state->desc.windowed;
}

void CDECL wined3d_swapchain_state_get_size(const struct wined3d_swapchain_state *state,
        unsigned int *width, unsigned int *height)
{
    TRACE("state %p.\n", state);

    *width = state->desc.backbuffer_width;
    *height = state->desc.backbuffer_height;
}

void CDECL wined3d_swapchain_state_destroy(struct wined3d_swapchain_state *state)
{
    wined3d_swapchain_state_cleanup(state);
    free(state);
}

HRESULT CDECL wined3d_swapchain_state_create(const struct wined3d_swapchain_desc *desc,
        HWND window, struct wined3d *wined3d, struct wined3d_swapchain_state_parent *state_parent,
        struct wined3d_swapchain_state **state)
{
    struct wined3d_swapchain_state *s;
    HRESULT hr;

    TRACE("desc %p, window %p, wined3d %p, state %p.\n", desc, window, wined3d, state);

    if (!(s = calloc(1, sizeof(*s))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = wined3d_swapchain_state_init(s, desc, window, wined3d, state_parent)))
    {
        free(s);
        return hr;
    }

    *state = s;

    return hr;
}
