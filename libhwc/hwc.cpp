/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
//#define LOG_NDEBUG 0
//#include <utils/Log.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <s3c-fb.h>

#include <EGL/egl.h>
#include "SecHWCUtils.h"

#define HWC_REMOVE_DEPRECATED_VERSIONS 1

#include <cutils/compiler.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware_legacy/uevent.h>
#include <utils/String8.h>
#include <utils/Vector.h>

#include <sync/sync.h>

//#include "ion.h"
#include "gralloc_priv.h"
//#include "exynos_fimc.h"
//#include "exynos_format.h"
//#include "exynos_v4l2.h"
#include "s5p_tvout_v4l2.h"

#if defined(BOARD_USES_HDMI)
#include "SecHdmiClient.h"
#include "SecTVOutService.h"

#include "SecHdmi.h"

//#define CHECK_EGL_FPS
#ifdef CHECK_EGL_FPS
extern void check_fps();
#endif

static int lcd_width, lcd_height;
static int prev_usage = 0;

#define CHECK_TIME_DEBUG 0
#define SUPPORT_AUTO_UI_ROTATE
#endif


static void dump_handle(private_handle_t *h)
{
    ALOGV("\t\tfd = %d, base = 0x%x, format = %d, width = %u, height = %u, stride = %u, vstride = %u",
            h->fd, h->base, h->format, h->width, h->height, h->stride, h->stride);
}

static void dump_layer(hwc_layer_1_t const *l)
{
    ALOGV("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, "
            "{%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform,
            l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);

    if(l->handle && !(l->flags & HWC_SKIP_LAYER))
        dump_handle(private_handle_t::dynamicCast(l->handle));
}

static void dump_config(s3c_fb_win_config &c)
{
    ALOGV("\tstate = %u", c.state);
    if (c.state == c.S3C_FB_WIN_STATE_BUFFER) {
        ALOGV("\t\tfd = %d, offset = %u, stride = %u, "
                "x = %d, y = %d, w = %u, h = %u, "
                "format = %u, blending = %u",
                c.fd, c.offset, c.stride,
                c.x, c.y, c.w, c.h,
                c.format, c.blending);
    }
    else if (c.state == c.S3C_FB_WIN_STATE_COLOR) {
        ALOGV("\t\tcolor = %u", c.color);
    }
}

inline int WIDTH(const hwc_rect &rect) { return rect.right - rect.left; }
inline int HEIGHT(const hwc_rect &rect) { return rect.bottom - rect.top; }

void calculate_rect(struct hwc_win_info_t *win, hwc_layer_1_t *cur,
        sec_rect *rect)
{
    rect->x = cur->displayFrame.left;
    rect->y = cur->displayFrame.top;
    rect->w = cur->displayFrame.right - cur->displayFrame.left;
    rect->h = cur->displayFrame.bottom - cur->displayFrame.top;

    if (rect->x < 0) {
        if (rect->w + rect->x > win->lcd_info.xres)
            rect->w = win->lcd_info.xres;
        else
            rect->w = rect->w + rect->x;
        rect->x = 0;
    } else {
        if (rect->w + rect->x > win->lcd_info.xres)
            rect->w = win->lcd_info.xres - rect->x;
    }
    if (rect->y < 0) {
        if (rect->h + rect->y > win->lcd_info.yres)
            rect->h = win->lcd_info.yres;
        else
            rect->h = rect->h + rect->y;
        rect->y = 0;
    } else {
        if (rect->h + rect->y > win->lcd_info.yres)
            rect->h = win->lcd_info.yres - rect->y;
    }
}

static int set_src_dst_img_rect(hwc_layer_1_t *cur,
        struct hwc_win_info_t *win,
        struct sec_img *src_img,
        struct sec_img *dst_img,
        struct sec_rect *src_rect,
        struct sec_rect *dst_rect,
        int win_idx)
{
    private_handle_t *prev_handle = (private_handle_t *)(cur->handle);
    sec_rect rect;

    /* 1. Set src_img from prev_handle */
    src_img->f_w     = prev_handle->width;
    src_img->f_h     = prev_handle->height;
    src_img->w       = prev_handle->width;
    src_img->h       = prev_handle->height;
    src_img->format  = prev_handle->format;
    src_img->base    = (uint32_t)prev_handle->base;
    src_img->offset  = prev_handle->offset;
    src_img->mem_id  = prev_handle->fd;
    src_img->paddr  = prev_handle->paddr;
    src_img->usage  = prev_handle->usage;
    src_img->uoffset  = prev_handle->uoffset;
    src_img->voffset  = prev_handle->voffset;

    src_img->mem_type = HWC_VIRT_MEM_TYPE;

    switch (src_img->format) {
    case HAL_PIXEL_FORMAT_YV12:             /* To support video editor */
    case HAL_PIXEL_FORMAT_YCbCr_420_P:      /* To support SW codec     */
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_422_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_422_SP:
    case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_CbYCrY_422_I:
    case HAL_PIXEL_FORMAT_CUSTOM_CrYCbY_422_I:
        src_img->f_w = (src_img->f_w + 15) & ~15;
        src_img->f_h = (src_img->f_h + 1) & ~1;
        break;
    default:
        src_img->f_w = src_img->w;
        src_img->f_h = src_img->h;
        break;
    }

    /* 2. Set dst_img from window(lcd) */
    calculate_rect(win, cur, &rect);
    dst_img->f_w = win->lcd_info.xres;
    dst_img->f_h = win->lcd_info.yres;
    dst_img->w = rect.w;
    dst_img->h = rect.h;

    switch (win->lcd_info.bits_per_pixel) {
    case 32:
        dst_img->format = HAL_PIXEL_FORMAT_RGBX_8888;
        break;
    default:
        dst_img->format = HAL_PIXEL_FORMAT_RGB_565;
        break;
    }

    dst_img->base     = win->addr[win->buf_index];
    dst_img->offset   = 0;
    dst_img->mem_id   = 0;
    dst_img->mem_type = HWC_PHYS_MEM_TYPE;

    /* 3. Set src_rect(crop rect) */
    if (cur->displayFrame.left < 0) {
        src_rect->x =
            (0 - cur->displayFrame.left)
            *(src_img->w)
            /(cur->displayFrame.right - cur->displayFrame.left);
        if (cur->displayFrame.right > win->lcd_info.xres) {
            src_rect->w =
                (cur->sourceCrop.right - cur->sourceCrop.left) -
                src_rect->x -
                (cur->displayFrame.right - win->lcd_info.xres)
                *(src_img->w)
                /(cur->displayFrame.right - cur->displayFrame.left);
        } else {
            src_rect->w =
                (cur->sourceCrop.right - cur->sourceCrop.left) -
                src_rect->x;
        }
    } else {
        src_rect->x = cur->sourceCrop.left;
        if (cur->displayFrame.right > win->lcd_info.xres) {
            src_rect->w =
                (cur->sourceCrop.right - cur->sourceCrop.left) -
                src_rect->x -
                (cur->displayFrame.right - win->lcd_info.xres)
                *(src_img->w)
                /(cur->displayFrame.right - cur->displayFrame.left);
        } else {
            src_rect->w =
                (cur->sourceCrop.right - cur->sourceCrop.left);
        }
    }
    if (cur->displayFrame.top < 0) {
        src_rect->y =
            (0 - cur->displayFrame.top)
            *(src_img->h)
            /(cur->displayFrame.bottom - cur->displayFrame.top);
        if (cur->displayFrame.bottom > win->lcd_info.yres) {
            src_rect->h =
                (cur->sourceCrop.bottom - cur->sourceCrop.top) -
                src_rect->y -
                (cur->displayFrame.bottom - win->lcd_info.yres)
                *(src_img->h)
                /(cur->displayFrame.bottom - cur->displayFrame.top);
        } else {
            src_rect->h =
                (cur->sourceCrop.bottom - cur->sourceCrop.top) -
                src_rect->y;
        }
    } else {
        src_rect->y = cur->sourceCrop.top;
        if (cur->displayFrame.bottom > win->lcd_info.yres) {
            src_rect->h =
                (cur->sourceCrop.bottom - cur->sourceCrop.top) -
                src_rect->y -
                (cur->displayFrame.bottom - win->lcd_info.yres)
                *(src_img->h)
                /(cur->displayFrame.bottom - cur->displayFrame.top);
        } else {
            src_rect->h =
                (cur->sourceCrop.bottom - cur->sourceCrop.top);
        }
    }

    SEC_HWC_Log(HWC_LOG_DEBUG,
            "crop information()::"
            "sourceCrop left(%d),top(%d),right(%d),bottom(%d),"
            "src_rect x(%d),y(%d),w(%d),h(%d),"
            "prev_handle w(%d),h(%d)",
            cur->sourceCrop.left,
            cur->sourceCrop.top,
            cur->sourceCrop.right,
            cur->sourceCrop.bottom,
            src_rect->x, src_rect->y, src_rect->w, src_rect->h,
            prev_handle->width, prev_handle->height);

    src_rect->x = SEC_MAX(src_rect->x, 0);
    src_rect->y = SEC_MAX(src_rect->y, 0);
    src_rect->w = SEC_MAX(src_rect->w, 0);
    src_rect->w = SEC_MIN(src_rect->w, prev_handle->width);
    src_rect->h = SEC_MAX(src_rect->h, 0);
    src_rect->h = SEC_MIN(src_rect->h, prev_handle->height);

    /* 4. Set dst_rect(fb or lcd)
     *    fimc dst image will be stored from left top corner
     */
    dst_rect->x = 0;
    dst_rect->y = 0;
    dst_rect->w = win->rect_info.w;
    dst_rect->h = win->rect_info.h;

    /* Summery */
    SEC_HWC_Log(HWC_LOG_DEBUG,
            "exynos4_set_src_dst_img_rect()::"
            "SRC w(%d),h(%d),f_w(%d),f_h(%d),fmt(0x%x),"
            "base(0x%x),offset(%d),paddr(0x%X),mem_id(%d),mem_type(%d)=>\r\n"
            "   DST w(%d),h(%d),f(0x%x),base(0x%x),"
            "offset(%d),mem_id(%d),mem_type(%d),"
            "rot(%d),win_idx(%d)"
            "   SRC_RECT x(%d),y(%d),w(%d),h(%d)=>"
            "DST_RECT x(%d),y(%d),w(%d),h(%d)",
            src_img->w, src_img->h, src_img->f_w, src_img->f_h, src_img->format,
            src_img->base, src_img->offset, src_img->paddr, src_img->mem_id, src_img->mem_type,
            dst_img->w, dst_img->h,  dst_img->format, dst_img->base,
            dst_img->offset, dst_img->mem_id, dst_img->mem_type,
            cur->transform, win_idx,
            src_rect->x, src_rect->y, src_rect->w, src_rect->h,
            dst_rect->x, dst_rect->y, dst_rect->w, dst_rect->h);

    return 0;
}

static int get_hwc_compos_decision(hwc_layer_1_t *cur, int iter, int win_cnt)
{
  return HWC_FRAMEBUFFER;
    if(cur->flags & HWC_SKIP_LAYER  || !cur->handle) {
        ALOGV("%s::is_skip_layer  %d  cur->handle %x ",  __func__, cur->flags & HWC_SKIP_LAYER, cur->handle);
        return HWC_FRAMEBUFFER;
    }

    private_handle_t *prev_handle = (private_handle_t *)(cur->handle);
    int compositionType = HWC_FRAMEBUFFER;

    if (iter == 0) {
    /* check here....if we have any resolution constraints */
        if (((cur->sourceCrop.right - cur->sourceCrop.left + 1) < 16) ||
            ((cur->sourceCrop.bottom - cur->sourceCrop.top + 1) < 8))
            return compositionType;

        if ((cur->transform == HAL_TRANSFORM_ROT_90) ||
            (cur->transform == HAL_TRANSFORM_ROT_270)) {
            if (((cur->displayFrame.right - cur->displayFrame.left + 1) < 4) ||
                ((cur->displayFrame.bottom - cur->displayFrame.top + 1) < 8))
                return compositionType;
        } else if (((cur->displayFrame.right - cur->displayFrame.left + 1) < 8) ||
                   ((cur->displayFrame.bottom - cur->displayFrame.top + 1) < 4)) {
            return compositionType;
        }

        switch (prev_handle->format) {
        case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED:
            compositionType = HWC_OVERLAY;
            break;
        case HAL_PIXEL_FORMAT_YV12:                 /* YCrCb_420_P */
        case HAL_PIXEL_FORMAT_YCbCr_420_P:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            if ((prev_handle->usage & GRALLOC_USAGE_HWC_HWOVERLAY) &&
                 (cur->blending == HWC_BLENDING_NONE))
                compositionType = HWC_OVERLAY;
            else
                compositionType = HWC_FRAMEBUFFER;
            break;
        default:
            compositionType = HWC_FRAMEBUFFER;
            break;
        }
    }

    ALOGV("%s::compositionType(%d)=>0:FB,1:OVERLAY \r\n"
            "   format(0x%x),magic(0x%x),flags(%d),size(%d),offset(%d)"
            "b_addr(0x%x),usage(0x%x),w(%d),h(%d),bpp(%d)",
            "get_hwc_compos_decision()", compositionType,
            prev_handle->format, prev_handle->magic, prev_handle->flags,
            prev_handle->size, prev_handle->offset, prev_handle->base,
            prev_handle->usage, prev_handle->width, prev_handle->height,
            prev_handle->bpp);

    return  compositionType;
}

static void reset_win_rect_info(hwc_win_info_t *win)
{
    win->rect_info.x = 0;
    win->rect_info.y = 0;
    win->rect_info.w = 0;
    win->rect_info.h = 0;
    return;
}

static int assign_overlay_window(exynos4_hwc_composer_device_1_t *ctx, hwc_layer_1_t *cur,
        int win_idx, int layer_idx)
{
    struct hwc_win_info_t   *win;
    sec_rect   rect;
    int ret = 0;

    if (NUM_OF_WIN <= win_idx)
        return -1;

    win = &ctx->win[win_idx];

    ALOGV("%s:: left(%d),top(%d),right(%d),bottom(%d),transform(%d)"
            "lcd_info.xres(%d),lcd_info.yres(%d)",
            "++assign_overlay_window()",
            cur->displayFrame.left, cur->displayFrame.top,
            cur->displayFrame.right, cur->displayFrame.bottom, cur->transform,
            win->lcd_info.xres, win->lcd_info.yres);

    calculate_rect(win, cur, &rect);

    if ((rect.x != win->rect_info.x) || (rect.y != win->rect_info.y) ||
        (rect.w != win->rect_info.w) || (rect.h != win->rect_info.h)){
        win->rect_info.x = rect.x;
        win->rect_info.y = rect.y;
        win->rect_info.w = rect.w;
        win->rect_info.h = rect.h;
            //turnoff the window and set the window position with new conf...
        if (window_set_pos(win) < 0) {
            ALOGE("%s::window_set_pos is failed : %s",
                    __func__, strerror(errno));
            ret = -1;
        }
        ctx->layer_prev_buf[win_idx] = 0;
    }

    win->layer_index = layer_idx;
    win->status = HWC_WIN_RESERVED;

    ALOGV("%s:: win_x %d win_y %d win_w %d win_h %d  lay_idx %d win_idx %d\n",
            "--assign_overlay_window()",
            win->rect_info.x, win->rect_info.y, win->rect_info.w,
            win->rect_info.h, win->layer_index, win_idx );

    return 0;
}

#ifdef SKIP_DUMMY_UI_LAY_DRAWING
static void get_hwc_ui_lay_skipdraw_decision(exynos4_hwc_composer_device_1_t *ctx,
                               hwc_display_contents_1_t* list)
{
    private_handle_t *prev_handle;
    hwc_layer_1_t* cur;
    int num_of_fb_lay_skip = 0;
    int fb_lay_tot = ctx->num_of_fb_layer + ctx->num_of_fb_lay_skip;

    if (fb_lay_tot > NUM_OF_DUMMY_WIN)
        return;

    if (fb_lay_tot < 1) {
#ifdef GL_WA_OVLY_ALL
        ctx->ui_skip_frame_cnt++;
        if (ctx->ui_skip_frame_cnt >= THRES_FOR_SWAP) {
            ctx->ui_skip_frame_cnt = 0;
            ctx->num_of_fb_layer_prev = 1;
        }
#endif
        return;
    }

    if (ctx->fb_lay_skip_initialized) {
        for (int cnt = 0; cnt < fb_lay_tot; cnt++) {
            cur = &list->hwLayers[ctx->win_virt[cnt].layer_index];
            if (ctx->win_virt[cnt].layer_prev_buf == (uint32_t)cur->handle)
                num_of_fb_lay_skip++;
        }
#ifdef GL_WA_OVLY_ALL
        if (ctx->ui_skip_frame_cnt >= THRES_FOR_SWAP)
            num_of_fb_lay_skip = 0;
#endif
        if (num_of_fb_lay_skip != fb_lay_tot) {
            ctx->num_of_fb_layer = fb_lay_tot;
            ctx->num_of_fb_lay_skip = 0;
#ifdef GL_WA_OVLY_ALL
            ctx->ui_skip_frame_cnt = 0;
#endif
            for (int cnt = 0; cnt < fb_lay_tot; cnt++) {
                cur = &list->hwLayers[ctx->win_virt[cnt].layer_index];
                ctx->win_virt[cnt].layer_prev_buf = (uint32_t)cur->handle;
                cur->compositionType = HWC_FRAMEBUFFER;
                ctx->win_virt[cnt].status = HWC_WIN_FREE;
            }
        } else {
            ctx->num_of_fb_layer = 0;
            ctx->num_of_fb_lay_skip = fb_lay_tot;
#ifdef GL_WA_OVLY_ALL
            ctx->ui_skip_frame_cnt++;
#endif
            for (int cnt = 0; cnt < fb_lay_tot; cnt++) {
                cur = &list->hwLayers[ctx->win_virt[cnt].layer_index];
                cur->compositionType = HWC_OVERLAY;
                ctx->win_virt[cnt].status = HWC_WIN_RESERVED;
            }
        }
    } else {
        ctx->num_of_fb_lay_skip = 0;
        for (int i = 0; i < list->numHwLayers ; i++) {
            if(num_of_fb_lay_skip >= NUM_OF_DUMMY_WIN)
                break;

            cur = &list->hwLayers[i];
            if (cur->handle) {
                prev_handle = (private_handle_t *)(cur->handle);

                switch (prev_handle->format) {
                case HAL_PIXEL_FORMAT_RGBA_8888:
                case HAL_PIXEL_FORMAT_BGRA_8888:
                case HAL_PIXEL_FORMAT_RGBX_8888:
                case HAL_PIXEL_FORMAT_RGB_565:
                    cur->compositionType = HWC_FRAMEBUFFER;
                    ctx->win_virt[num_of_fb_lay_skip].layer_prev_buf =
                        (uint32_t)cur->handle;
                    ctx->win_virt[num_of_fb_lay_skip].layer_index = i;
                    ctx->win_virt[num_of_fb_lay_skip].status = HWC_WIN_FREE;
                    num_of_fb_lay_skip++;
                    break;
                default:
                    break;
                }
            } else {
                cur->compositionType = HWC_FRAMEBUFFER;
            }
        }

        if (num_of_fb_lay_skip == fb_lay_tot)
            ctx->fb_lay_skip_initialized = 1;
    }

    return;

}
#endif

static int exynos4_prepare_fimd(exynos4_hwc_composer_device_1_t *pdev,
        hwc_display_contents_1_t* contents)
{
    ALOGV("preparing %u layers for FIMD", contents->numHwLayers);

    bool force_fb = pdev->force_gpu;
    int overlay_win_cnt = 0;
    int compositionType = 0;
    int ret;

#ifdef SKIP_DUMMY_UI_LAY_DRAWING
    if ((contents && (!(contents->flags & HWC_GEOMETRY_CHANGED))) &&
            (pdev->num_of_hwc_layer > 0)) {
        get_hwc_ui_lay_skipdraw_decision(pdev, contents);
        return 0;
    }
    pdev->fb_lay_skip_initialized = 0;
    pdev->num_of_fb_lay_skip = 0;
#ifdef GL_WA_OVLY_ALL
    pdev->ui_skip_frame_cnt = 0;
#endif

    for (int i = 0; i < NUM_OF_DUMMY_WIN; i++) {
        pdev->win_virt[i].layer_prev_buf = 0;
        pdev->win_virt[i].layer_index = -1;
        pdev->win_virt[i].status = HWC_WIN_FREE;
    }
#endif

    //if geometry is not changed, there is no need to do any work here
    if (!contents || (!(contents->flags & HWC_GEOMETRY_CHANGED)))
        return 0;

    //all the windows are free here....
    for (int i = 0 ; i < NUM_OF_WIN; i++) {
        pdev->win[i].status = HWC_WIN_FREE;
        pdev->win[i].buf_index = 0;
    }

    pdev->num_of_hwc_layer = 0;
    pdev->num_of_fb_layer = 0;
    pdev->num_2d_blit_layer = 0;


    // find unsupported overlays
    for (size_t i = 0; i < contents->numHwLayers; i++) {
        hwc_layer_1_t *layer = &contents->hwLayers[i];
        
        if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
            ALOGV("\tlayer %u: framebuffer target", i);
            continue;
        }

        if(layer->compositionType == HWC_BACKGROUND || force_fb) {
            //HWC_BACKGROUND can't directly set to kernel 3.0, use fb
            layer->compositionType = HWC_FRAMEBUFFER;
            pdev->num_of_fb_layer++;
            dump_layer(&contents->hwLayers[i]);
            continue;
        }
        
        if (overlay_win_cnt < NUM_OF_WIN) {
            compositionType = get_hwc_compos_decision(layer, 0, overlay_win_cnt);

            if (compositionType == HWC_FRAMEBUFFER) {
                layer->compositionType = HWC_FRAMEBUFFER;
                pdev->num_of_fb_layer++;
            } else {
                ret = assign_overlay_window(pdev, layer, overlay_win_cnt, i);
                if (ret != 0) {
                    LOGE("assign_overlay_window fail, change to frambuffer");
                    layer->compositionType = HWC_FRAMEBUFFER;
                    pdev->num_of_fb_layer++;
                    continue;
                }

                layer->compositionType = HWC_OVERLAY;
                layer->hints = HWC_HINT_CLEAR_FB;
                overlay_win_cnt++;
                pdev->num_of_hwc_layer++;
            }
        } else {
            layer->compositionType = HWC_FRAMEBUFFER;
            pdev->num_of_fb_layer++;
        }

        dump_layer(&contents->hwLayers[i]);
    }



    #if defined(BOARD_USES_HDMI)
    android::SecHdmiClient *mHdmiClient = android::SecHdmiClient::getInstance();
    mHdmiClient->setHdmiHwcLayer(pdev->num_of_hwc_layer);
    #endif

    if (contents->numHwLayers < (pdev->num_of_fb_layer + pdev->num_of_hwc_layer))
        ALOGD("%s:: numHwLayers %d num_of_fb_layer %d num_of_hwc_layer %d ",
                __func__, contents->numHwLayers, pdev->num_of_fb_layer,
                pdev->num_of_hwc_layer);

    if (overlay_win_cnt < NUM_OF_WIN) {
        //turn off the free windows
        for (int i = overlay_win_cnt; i < NUM_OF_WIN; i++) {
            window_hide(&pdev->win[i]);
            reset_win_rect_info(&pdev->win[i]);
        }
    }

    return 0;
}

static int exynos4_prepare(hwc_composer_device_1_t *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || !displays)
        return 0;

    exynos4_hwc_composer_device_1_t *pdev =
            (exynos4_hwc_composer_device_1_t *)dev;
    hwc_display_contents_1_t *fimd_contents = displays[HWC_DISPLAY_PRIMARY];
    hwc_display_contents_1_t *hdmi_contents = displays[HWC_DISPLAY_EXTERNAL];

    if (fimd_contents) {
        int err = exynos4_prepare_fimd(pdev, fimd_contents);
        if (err)
            return err;
    }

    return 0;
}

static int exynos4_post_fimd(exynos4_hwc_composer_device_1_t *pdev,
        hwc_display_contents_1_t* contents)
{
    int skipped_window_mask = 0;
    hwc_layer_1_t* cur;
    struct hwc_win_info_t   *win;
    int ret = 0;
    struct sec_img src_img;
    struct sec_img dst_img;
    struct sec_rect src_work_rect;
    struct sec_rect dst_work_rect;
    bool need_fb = pdev->num_of_fb_layer > 0;
    hwc_layer_1_t *fb_layer = NULL;

    memset(&src_img, 0, sizeof(src_img));
    memset(&dst_img, 0, sizeof(dst_img));
    memset(&src_work_rect, 0, sizeof(src_work_rect));
    memset(&dst_work_rect, 0, sizeof(dst_work_rect));

    #if defined(BOARD_USES_HDMI)
    int skip_hdmi_rendering = 0;
    int rotVal = 0;
    #endif

    if (!contents) {
        //turn off the all windows
        for (int i = 0; i < NUM_OF_WIN; i++) {
            window_hide(&pdev->win[i]);
            reset_win_rect_info(&pdev->win[i]);
            pdev->win[i].status = HWC_WIN_FREE;
        }
        pdev->num_of_hwc_layer = 0;
        ALOGE("%s: NULL contents!", __func__);
        return -1;
    }

    // if has framebuffer_target layer, post it
    if(need_fb) {
        for (size_t i = 0; i < contents->numHwLayers; i++) {
            if (contents->hwLayers[i].compositionType == HWC_FRAMEBUFFER_TARGET) {
                fb_layer = &contents->hwLayers[i];
                break;
            }
        }

        if (fb_layer == NULL) {
            ALOGE("framebuffer target expected, but not provided");
            ret = -1;
        } else {
            ALOGV("framebuffer target buffer:");
            dump_layer(fb_layer);
            
            pdev->fb_device->post(pdev->fb_device, fb_layer->handle);
        }
    }

    if(pdev->num_of_hwc_layer > NUM_OF_WIN)
        pdev->num_of_hwc_layer = NUM_OF_WIN;

    //compose overlay layers here
    for (int i = 0; i < pdev->num_of_hwc_layer - pdev->num_2d_blit_layer; i++) {
        win = &pdev->win[i];
        if (win->status == HWC_WIN_RESERVED) {
            cur = &contents->hwLayers[win->layer_index];

            if (cur->compositionType == HWC_OVERLAY) {
                if (pdev->layer_prev_buf[i] == (uint32_t)cur->handle) {
                    /*
                     * In android platform, all the graphic buffer are at least
                     * double buffered (2 or more) this buffer is already rendered.
                     * It is the redundant src buffer for FIMC rendering.
                     */
                    ALOGV("%s:: Same buffer, no need to pan display!", __func__);
                    #if defined(BOARD_USES_HDMI)
                    skip_hdmi_rendering = 1;
                    #endif
                    continue;
                }
                pdev->layer_prev_buf[i] = (uint32_t)cur->handle;
                // initialize the src & dist context for fimc
                set_src_dst_img_rect(cur, win, &src_img, &dst_img,
                                &src_work_rect, &dst_work_rect, i);

                ALOGV("%s:: Overlay runFimc!", __func__);
                ret = runFimc(pdev,
                            &src_img, &src_work_rect,
                            &dst_img, &dst_work_rect,
                            cur->transform);

                if (ret < 0) {
                    ALOGE("%s::runFimc fail : ret=%d\n", __func__, ret);
                    skipped_window_mask |= (1 << i);
                    continue;
                }

                window_pan_display(win);

                win->buf_index = (win->buf_index + 1) % NUM_OF_WIN_BUF;
                if (win->power_state == 0)
                    window_show(win);
            } else {
                ALOGE("%s:: error : layer %d compositionType should have been"
                        " HWC_OVERLAY ", __func__, win->layer_index);
                skipped_window_mask |= (1 << i);
                continue;
            }
        } else {
            ALOGE("%s:: error : window status should have "
                    "been HWC_WIN_RESERVED by now... ", __func__);
             skipped_window_mask |= (1 << i);
             continue;
        }
    }

    if (skipped_window_mask) {
        //turn off the free windows
        for (int i = 0; i < NUM_OF_WIN; i++) {
            if (skipped_window_mask & (1 << i)) {
                window_hide(&pdev->win[i]);
                reset_win_rect_info(&pdev->win[i]);
            }
        }
    }

#if defined(BOARD_USES_HDMI)
        android::SecHdmiClient *mHdmiClient = android::SecHdmiClient::getInstance();
    
        if (skip_hdmi_rendering == 1)
            return 0;
    
        if (contents == NULL) {
            // Don't display unnecessary image
            mHdmiClient->setHdmiEnable(0);
            return 0;
        } else {
            mHdmiClient->setHdmiEnable(1);
        }

#ifdef SUPPORT_AUTO_UI_ROTATE
#if 0 //yqf, move to FramebufferNativeWindow
        cur = &list->hwLayers[0];
    //LOGE("%s, cur->tran:%d \n",__func__,cur->transform); //added yqf
        if (cur->transform == HAL_TRANSFORM_ROT_90 )//added yqf for test
            mHdmiClient->setHdmiRotate(90, ctx->num_of_hwc_layer);
        else if(cur->transform == HAL_TRANSFORM_ROT_270)
            mHdmiClient->setHdmiRotate(270, ctx->num_of_hwc_layer);
        else if(cur->transform == HAL_TRANSFORM_ROT_180)  
            mHdmiClient->setHdmiRotate(180, ctx->num_of_hwc_layer);
        else /*if(cur->transform == HAL_TRANSFORM_ROT_0)*/
        mHdmiClient->setHdmiRotate(0, ctx->num_of_hwc_layer);
#endif
#endif
    
    
#if 1 //added yqf, for flip ops for TV flush with camera preview by front camera  on TC4
     if (pdev->num_of_hwc_layer == 1) {
        win = &pdev->win[0];
        cur = &contents->hwLayers[win->layer_index];
     LOGE("%s, cur->tran:%d \n",__func__,cur->transform); 
        if ((cur->transform == HAL_TRANSFORM_FLIP_H) ||(cur->transform == (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_ROT_90)) )
            mHdmiClient->setHdmiFlip(1, pdev->num_of_hwc_layer); //hflip
        else if((cur->transform == HAL_TRANSFORM_FLIP_V)||(cur->transform == (HAL_TRANSFORM_FLIP_V | HAL_TRANSFORM_ROT_90)) )
            mHdmiClient->setHdmiFlip(2, pdev->num_of_hwc_layer); //vflip
        else    
        mHdmiClient->setHdmiFlip(0, pdev->num_of_hwc_layer); //none flip
     }
 #endif
     
        // To support S3D video playback (automatic TV mode change to 3D mode)
        if (pdev->num_of_hwc_layer == 1) {
            if (src_img.usage != prev_usage)
                mHdmiClient->setHdmiResolution(DEFAULT_HDMI_RESOLUTION_VALUE);    // V4L2_STD_1080P_60
    
            if ((src_img.usage & GRALLOC_USAGE_PRIVATE_SBS_LR) ||
                (src_img.usage & GRALLOC_USAGE_PRIVATE_SBS_RL))
                mHdmiClient->setHdmiResolution(7209601);    // V4L2_STD_TVOUT_720P_60_SBS_HALF
            else if ((src_img.usage & GRALLOC_USAGE_PRIVATE_TB_LR) ||
                (src_img.usage & GRALLOC_USAGE_PRIVATE_TB_RL))
                mHdmiClient->setHdmiResolution(1080924);    // V4L2_STD_TVOUT_1080P_24_TB
    
            prev_usage = src_img.usage;
        } else {
            if ((prev_usage & GRALLOC_USAGE_PRIVATE_SBS_LR) ||
                (prev_usage & GRALLOC_USAGE_PRIVATE_SBS_RL) ||
                (prev_usage & GRALLOC_USAGE_PRIVATE_TB_LR) ||
                (prev_usage & GRALLOC_USAGE_PRIVATE_TB_RL))
                mHdmiClient->setHdmiResolution(DEFAULT_HDMI_RESOLUTION_VALUE);    // V4L2_STD_1080P_60
            prev_usage = 0;
        }
    
    //LOGI("num_of_hwc_layer:%d\n",pdev->num_of_hwc_layer); 
        if (pdev->num_of_hwc_layer == 1) {
            if ((src_img.format == HAL_PIXEL_FORMAT_CUSTOM_YCbCr_420_SP_TILED)||
                    (src_img.format == HAL_PIXEL_FORMAT_CUSTOM_YCrCb_420_SP)) {
                ADDRS * addr = (ADDRS *)(src_img.base);
    
                mHdmiClient->blit2Hdmi(src_img.w, src_img.h,
                                        src_img.format,
                                        (unsigned int)addr->addr_y, (unsigned int)addr->addr_cbcr, (unsigned int)addr->addr_cbcr,
                                        0, 0,
                                        android::SecHdmiClient::HDMI_MODE_VIDEO,
                                        pdev->num_of_hwc_layer);
            } else if ((src_img.format == HAL_PIXEL_FORMAT_YCbCr_420_SP) ||
                        (src_img.format == HAL_PIXEL_FORMAT_YCrCb_420_SP) ||
                        (src_img.format == HAL_PIXEL_FORMAT_YCbCr_420_P) ||
                        (src_img.format == HAL_PIXEL_FORMAT_YV12)) {
                mHdmiClient->blit2Hdmi(src_img.w, src_img.h,
                                        src_img.format,
                                        (unsigned int)pdev->fimc.params.src.buf_addr_phy_rgb_y,
                                        (unsigned int)pdev->fimc.params.src.buf_addr_phy_cb,
                                        (unsigned int)pdev->fimc.params.src.buf_addr_phy_cr,
                                        0, 0,
                                        android::SecHdmiClient::HDMI_MODE_VIDEO,
                                        pdev->num_of_hwc_layer);
            } else {
                ALOGE("%s: Unsupported format = %d", __func__, src_img.format);
            }
        }
#endif

    return ret;
}

static int exynos4_set_fimd(exynos4_hwc_composer_device_1_t *pdev,
        hwc_display_contents_1_t* contents)
{
    if (!contents->dpy || !contents->sur)
        return 0;

    int err = 0;
    err = exynos4_post_fimd(pdev, contents);

    return err;
}

static int exynos4_set(struct hwc_composer_device_1 *dev,
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    if (!numDisplays || !displays)
        return 0;

    exynos4_hwc_composer_device_1_t *pdev =
            (exynos4_hwc_composer_device_1_t *)dev;
    hwc_display_contents_1_t *fimd_contents = displays[HWC_DISPLAY_PRIMARY];
    int fimd_err = 0, hdmi_err = 0;

    if (fimd_contents)
        fimd_err = exynos4_set_fimd(pdev, fimd_contents);

    if (fimd_err)
        return fimd_err;

    return hdmi_err;
}

static void exynos4_registerProcs(struct hwc_composer_device_1* dev,
        hwc_procs_t const* procs)
{
    struct exynos4_hwc_composer_device_1_t* pdev =
            (struct exynos4_hwc_composer_device_1_t*)dev;
    pdev->procs = procs;
}

static int exynos4_query(struct hwc_composer_device_1* dev, int what, int *value)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)dev;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we support the background layer
        value[0] = 1;
        break;
    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = pdev->vsync_period;
        break;
    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static int exynos4_eventControl(struct hwc_composer_device_1 *dev, int dpy,
        int event, int enabled)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
        __u32 val = !!enabled;
        int err = ioctl(pdev->fd, S3CFB_SET_VSYNC_INT, &val);
        if (err < 0) {
            ALOGE("vsync ioctl failed");
            return -errno;
        }

        return 0;
    }

    return -EINVAL;
}

static void handle_vsync_event(struct exynos4_hwc_composer_device_1_t *pdev)
{
    if (!pdev->procs)
        return;

    int err = lseek(pdev->vsync_fd, 0, SEEK_SET);
    if (err < 0) {
        ALOGE("error seeking to vsync timestamp: %s", strerror(errno));
        return;
    }

    char buf[4096];
    err = read(pdev->vsync_fd, buf, sizeof(buf));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return;
    }
    buf[sizeof(buf) - 1] = '\0';

    errno = 0;
    uint64_t timestamp = strtoull(buf, NULL, 0);
    if (!errno)
        pdev->procs->vsync(pdev->procs, 0, timestamp);
}

static void *hwc_vsync_thread(void *data)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)data;
    char uevent_desc[4096];
    memset(uevent_desc, 0, sizeof(uevent_desc));

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    uevent_init();

    char temp[4096];
    int err = read(pdev->vsync_fd, temp, sizeof(temp));
    if (err < 0) {
        ALOGE("error reading vsync timestamp: %s", strerror(errno));
        return NULL;
    }

    struct pollfd fds[2];
    fds[0].fd = pdev->vsync_fd;
    fds[0].events = POLLPRI;
    fds[1].fd = uevent_get_fd();
    fds[1].events = POLLIN;

    while (true) {
        int err = poll(fds, 2, -1);

        if (err > 0) {
            if (fds[0].revents & POLLPRI) {
                handle_vsync_event(pdev);
            }
            else if (fds[1].revents & POLLIN) {
            }
        }
        else if (err == -1) {
            if (errno == EINTR)
                break;
            ALOGE("error in vsync thread: %s", strerror(errno));
        }
    }

    return NULL;
}

static int exynos4_blank(struct hwc_composer_device_1 *dev, int disp, int blank)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)dev;

    switch (disp) {
    case HWC_DISPLAY_PRIMARY: {
        int fb_blank = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        int err = 0; //ioctl(pdev->fd, FBIOBLANK, fb_blank);    //wjj closed until kernel 3.0 blank api is correct.
        if (err < 0) {
            if (errno == EBUSY)
                ALOGI("%sblank ioctl failed (display already %sblanked)",
                        blank ? "" : "un", blank ? "" : "un");
            else
                ALOGE("%sblank ioctl failed: %s", blank ? "" : "un",
                        strerror(errno));
            return -errno;
        }
        break;
    }

    case HWC_DISPLAY_EXTERNAL:
        break;

    default:
        return -EINVAL;

    }

    return 0;
}

static void exynos4_dump(hwc_composer_device_1* dev, char *buff, int buff_len)
{
    if (buff_len <= 0)
        return;

    struct exynos4_hwc_composer_device_1_t *pdev =
            (struct exynos4_hwc_composer_device_1_t *)dev;

    android::String8 result;

    /*
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        struct s3c_fb_win_config &config = pdev->last_config[i];
        if (config.state == config.S3C_FB_WIN_STATE_DISABLED) {
            result.appendFormat(" %8s | %8s | %8s | %5s | %6s | %13s | %13s",
                    "DISABLED", "-", "-", "-", "-", "-", "-");
        }
        else {
            if (config.state == config.S3C_FB_WIN_STATE_COLOR)
                result.appendFormat(" %8s | %8s | %8x | %5s | %6s", "COLOR",
                        "-", config.color, "-", "-");
            else
                result.appendFormat(" %8s | %8x | %8s | %5x | %6x",
                        pdev->last_fb_window == i ? "FB" : "OVERLAY",
                        intptr_t(pdev->last_handles[i]),
                        "-", config.blending, config.format);

            result.appendFormat(" | [%5d,%5d] | [%5u,%5u]", config.x, config.y,
                    config.w, config.h);
        }

        if (pdev->last_fimc_map[i].mode == exynos4_fimc_map_t::FIMC_NONE)
            result.appendFormat(" | %3s", "-");
        else
            result.appendFormat(" | %3d",
                    AVAILABLE_FIMC_UNITS[pdev->last_fimc_map[i].idx]);

        result.append("\n");
    }
    */
    strlcpy(buff, result.string(), buff_len);
}

static int exynos4_getDisplayConfigs(struct hwc_composer_device_1 *dev,
        int disp, uint32_t *configs, size_t *numConfigs)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
               (struct exynos4_hwc_composer_device_1_t *)dev;

    if (*numConfigs == 0)
        return 0;

    if (disp == HWC_DISPLAY_PRIMARY) {
        configs[0] = 0;
        *numConfigs = 1;
        return 0;
    } else if (disp == HWC_DISPLAY_EXTERNAL) {
    }

    return -EINVAL;
}

static int32_t exynos4_fimd_attribute(struct exynos4_hwc_composer_device_1_t *pdev,
        const uint32_t attribute)
{
    switch(attribute) {
    case HWC_DISPLAY_VSYNC_PERIOD:
        return pdev->vsync_period;

    case HWC_DISPLAY_WIDTH:
        return pdev->xres;

    case HWC_DISPLAY_HEIGHT:
        return pdev->yres;

    case HWC_DISPLAY_DPI_X:
        return pdev->xdpi;

    case HWC_DISPLAY_DPI_Y:
        return pdev->ydpi;

    default:
        ALOGE("unknown display attribute %u", attribute);
        return -EINVAL;
    }
}

static int exynos4_getDisplayAttributes(struct hwc_composer_device_1 *dev,
        int disp, uint32_t config, const uint32_t *attributes, int32_t *values)
{
    struct exynos4_hwc_composer_device_1_t *pdev =
                   (struct exynos4_hwc_composer_device_1_t *)dev;

    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
        if (disp == HWC_DISPLAY_PRIMARY) {
            values[i] = exynos4_fimd_attribute(pdev, attributes[i]);
        }
        else if (disp == HWC_DISPLAY_EXTERNAL) {
        }
        else {
            ALOGE("unknown display type %u", disp);
            return -EINVAL;
        }
    }

    return 0;
}

static int exynos4_close(hw_device_t* device);

static int exynos4_open(const struct hw_module_t *module, const char *name,
        struct hw_device_t **device)
{
    int ret;
    int refreshRate;
    int sw_fd;
    struct fb_var_screeninfo const* info;
    struct hwc_win_info_t   *win;

    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        return -EINVAL;
    }

    struct exynos4_hwc_composer_device_1_t *dev;
    dev = (struct exynos4_hwc_composer_device_1_t *)malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));

    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
            (const struct hw_module_t **)&dev->gralloc_module)) {
        ALOGE("failed to get gralloc hw module");
        ret = -EINVAL;
        goto err_get_module;
    }

    if (gralloc_open((const hw_module_t *)dev->gralloc_module,
            &dev->alloc_device)) {
        ALOGE("failed to open gralloc");
        ret = -EINVAL;
        goto err_get_module;
    }

    //open framebuffer in FIMD
    if (framebuffer_open((const hw_module_t *)dev->gralloc_module, 
            &dev->fb_device)) {
        ALOGE("framebuffer_open failed");
        ret = -EINVAL;
        goto err_open_fb;
    }

    dev->fd = dev->gralloc_module->framebuffer->fd;
    info = &(dev->gralloc_module->info);
    refreshRate = dev->fb_device->fps;

    dev->xres = dev->fb_device->width;
    dev->yres = dev->fb_device->height;
    dev->xdpi = 1000 * dev->fb_device->xdpi;
    dev->ydpi = 1000 * dev->fb_device->ydpi;
    dev->vsync_period  = 1000000000 / refreshRate;

    ALOGI("using (fd=%d)\n"
          "xres         = %d px\n"
          "yres         = %d px\n"
          "width        = %d mm (%f dpi)\n"
          "height       = %d mm (%f dpi)\n"
          "refresh rate = %d Hz\n",
          dev->fd, dev->xres, dev->yres, info->width, dev->xdpi / 1000.0,
          info->height, dev->ydpi / 1000.0, refreshRate);

    dev->vsync_fd = open("/sys/devices/platform/exynos4-fb.0/vsync", O_RDONLY);
    if (dev->vsync_fd < 0) {
        ALOGI("failed to open vsync attribute, use SW vsync in HWComposer");
        //ret = dev->vsync_fd;
        //goto err_hdmi1;
    }

    dev->base.common.tag = HARDWARE_DEVICE_TAG;
    dev->base.common.version = HWC_DEVICE_API_VERSION_1_1;
    dev->base.common.module = const_cast<hw_module_t *>(module);
    dev->base.common.close = exynos4_close;

    dev->base.prepare = exynos4_prepare;
    dev->base.set = exynos4_set;
    dev->base.eventControl = exynos4_eventControl;
    dev->base.blank = exynos4_blank;
    dev->base.query = exynos4_query;
    dev->base.registerProcs = exynos4_registerProcs;
    dev->base.dump = exynos4_dump;
    dev->base.getDisplayConfigs = exynos4_getDisplayConfigs;
    dev->base.getDisplayAttributes = exynos4_getDisplayAttributes;

    *device = &dev->base.common;

    //initializing
    memset(&(dev->fimc), 0, sizeof(s5p_fimc_t));

    /* open WIN0 & WIN1 here, WIN0 & WIN1 is overlay in FIMD */
     for (int i = 0; i < NUM_OF_WIN; i++) {
        if (window_open(&(dev->win[i]), i)  < 0) {
            ALOGE("%s:: Failed to open window %d device ", __func__, i);
             ret = -EINVAL;
             goto err_open_overlay;
        }
     }

    if (window_get_global_lcd_info(dev->win[0].fd, &dev->lcd_info) < 0) {
        ALOGE("%s::window_get_global_lcd_info is failed : %s", __func__, strerror(errno));
        ret = -EINVAL;
        goto err_open_overlay;
    }

    /* initialize the window context */
    for (int i = 0; i < NUM_OF_WIN; i++) {
        win = &dev->win[i];
        memcpy(&win->lcd_info, &dev->lcd_info, sizeof(struct fb_var_screeninfo));
        memcpy(&win->var_info, &dev->lcd_info, sizeof(struct fb_var_screeninfo));

        win->rect_info.x = 0;
        win->rect_info.y = 0;
        win->rect_info.w = win->var_info.xres;
        win->rect_info.h = win->var_info.yres;

       if (window_set_pos(win) < 0) {
            ALOGE("%s::window_set_pos is failed : %s", __func__, strerror(errno));
            ret = -EINVAL;
            goto err_open_overlay;
        }

        if (window_get_info(win, i) < 0) {
            ALOGE("%s::window_get_info is failed : %s",__func__, strerror(errno));
            ret = -EINVAL;
            goto err_open_overlay;
        }

    }

    //create PP
    if (createFimc(&dev->fimc) < 0) {
        ALOGE("%s::creatFimc() fail", __func__);
        ret = -EINVAL;
        goto err_open_overlay;
    }

    if(dev->vsync_fd >= 0){
        ret = pthread_create(&dev->vsync_thread, NULL, hwc_vsync_thread, dev);
        if (ret) {
            ALOGE("failed to start vsync thread: %s", strerror(ret));
            ret = -ret;
            goto err_vsync;
        }
    }

    char value[PROPERTY_VALUE_MAX];
    property_get("debug.hwc.force_gpu", value, "0");
    dev->force_gpu = atoi(value);

    return 0;

err_vsync:
    if(dev->vsync_fd >= 0)
        close(dev->vsync_fd);
    
err_open_overlay:
    if (destroyFimc(&dev->fimc) < 0)
        ALOGE("%s::destroyFimc() fail", __func__);

    for (int i = 0; i < NUM_OF_WIN; i++) {
        if (window_close(&dev->win[i]) < 0)
            ALOGE("%s::window_close() fail", __func__);
    }

err_ioctl:
    framebuffer_close(dev->fb_device);
    if(dev->fd > 0) {
        close(dev->fd);
        dev->fd = 0;
    }
err_open_fb:
    gralloc_close(dev->alloc_device);
err_get_module:
    free(dev);
    
    return ret;
}

static int exynos4_close(hw_device_t *device)
{
    struct exynos4_hwc_composer_device_1_t *dev =
            (struct exynos4_hwc_composer_device_1_t *)device;
    pthread_kill(dev->vsync_thread, SIGTERM);
    pthread_join(dev->vsync_thread, NULL);
    int i;

    if (destroyFimc(&dev->fimc) < 0) {
        ALOGE("%s::destroyFimc fail", __func__);
    }

    for (i = 0; i < NUM_OF_WIN; i++) {
        if (window_close(&dev->win[i]) < 0)
            ALOGE("%s::window_close() fail", __func__);
    }

    framebuffer_close(dev->fb_device);
    if(dev->fd > 0) {
        close(dev->fd);
        dev->fd = 0;
    }
    gralloc_close(dev->alloc_device);
    if(dev->vsync_fd >= 0)
        close(dev->vsync_fd);
    return 0;
}

static struct hw_module_methods_t exynos4_hwc_module_methods = {
    open: exynos4_open,
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        module_api_version: HWC_MODULE_API_VERSION_0_1,
        hal_api_version: HARDWARE_HAL_API_VERSION,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Samsung exynos4 hwcomposer module",
        author: "Google",
        methods: &exynos4_hwc_module_methods,
    }
};
