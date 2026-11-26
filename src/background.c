/* ========================================================================= */
/**
 * @file background.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "background.h"

#include <cairo.h>
#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

struct wlr_output;

/* == Declarations ========================================================= */

/** Background state. */
struct wlmaker_background {
    /** List node, linked via @ref wlmaker_server_t::backgrounds. */
    bs_dllist_node_t          dlnode;
    /** Links to layer. */
    wlmtk_layer_t             *layer_ptr;
    /** color of the background. */
    uint32_t                  color;
    /** Tracks the outputs available. */
    wlmtk_output_tracker_t    *output_tracker_ptr;
    /** The wallpaper image, or NULL. Drawn over the color. */
    cairo_surface_t           *image_surface_ptr;
    /** How the wallpaper image is fit to the output. */
    wlmaker_background_mode_t mode;
};

/** Background panel: The workspace's backgrund for the output. */
typedef struct  {
    /** A layer background for one output is a panel. */
    wlmtk_panel_t             super_panel;
    /** Initial implementation: The background is a uni-color rectangle. */
    wlmtk_rectangle_t         *rectangle_ptr;
    /** The wallpaper, rendered at the output's size. Above the rectangle. */
    wlmtk_buffer_t            image_buffer;
    /** Whether @ref wlmaker_background_panel_t::image_buffer is initialized. */
    bool                      image_buffer_initialized;
    /** Back-link to the background. */
    struct wlmaker_background *background_ptr;
    /** Current size of the panel. */
    int                       width;
    /** Current size of the panel. */
    int                       height;
} wlmaker_background_panel_t;

static uint32_t _wlmaker_background_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height);

static void *_wlmaker_background_panel_create(
    struct wlr_output *wlr_output_ptr,
    void *ud_ptr);
static void _wlmaker_background_panel_destroy(
    struct wlr_output *wlr_output_ptr,
    void *ud_ptr,
    void *output_ptr);
static void _wlmaker_background_panel_element_destroy(
    wlmtk_element_t *element_ptr);
static void _wlmaker_backgrund_panel_set_color(
    struct wlr_output *wlr_output_ptr,
    void *ud_ptr,
    void *output_ptr,
    void *arg_ptr);
static void _wlmaker_background_panel_render_image(
    struct wlr_output *wlr_output_ptr,
    void *ud_ptr,
    void *output_ptr,
    void *arg_ptr);

/* == Data ================================================================= */

/** The background panels' virtual method table. */
static const wlmtk_panel_vmt_t _wlmaker_background_panel_vmt = {
    .request_size = _wlmaker_background_panel_request_size
};

/** The panel's superclass element virtual method table. */
static const wlmtk_element_vmt_t _wlmaker_background_panel_element_vmt = {
    .destroy = _wlmaker_background_panel_element_destroy
};

/** Panel's position: Anchored to all 4 edges, and auto-sized. */
static const wlmtk_panel_positioning_t _wlmaker_background_panel_position = {
    .desired_width = 0,
    .desired_height = 0,
    .anchor = WLR_EDGE_LEFT | WLR_EDGE_TOP | WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM,
};

/** Names of @ref wlmaker_background_mode_t, for the "ImageMode" key. */
const bspl_enum_desc_t wlmaker_background_mode_desc[] = {
    BSPL_ENUM("Fill", WLMAKER_BACKGROUND_FILL),
    BSPL_ENUM("Scale", WLMAKER_BACKGROUND_SCALE),
    BSPL_ENUM("Stretch", WLMAKER_BACKGROUND_STRETCH),
    BSPL_ENUM("Center", WLMAKER_BACKGROUND_CENTER),
    BSPL_ENUM("Tile", WLMAKER_BACKGROUND_TILE),
    BSPL_ENUM_SENTINEL()
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
bs_dllist_node_t *wlmaker_background_create(
    wlmtk_workspace_t *workspace_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr,
    uint32_t color)
{
    struct wlmaker_background *bg_ptr = logged_calloc(1, sizeof(*bg_ptr));
    if (NULL == bg_ptr) return NULL;
    bg_ptr->layer_ptr = wlmtk_workspace_get_layer(
        workspace_ptr, WLMTK_WORKSPACE_LAYER_BACKGROUND),
    bg_ptr->color = color;

    bg_ptr->output_tracker_ptr = wlmtk_output_tracker_create(
        wlr_output_layout_ptr,
        bg_ptr,
        _wlmaker_background_panel_create,
        NULL,
        _wlmaker_background_panel_destroy);

    return &bg_ptr->dlnode;
}

/* ------------------------------------------------------------------------- */
void wlmaker_background_dlnode_destroy(
    bs_dllist_node_t *dlnode_ptr,
    __UNUSED__ void *ud_ptr)
{
    struct wlmaker_background *bg_ptr = BS_CONTAINER_OF(
        dlnode_ptr, struct wlmaker_background, dlnode);

    if (NULL != bg_ptr->output_tracker_ptr) {
        wlmtk_output_tracker_destroy(bg_ptr->output_tracker_ptr);
        bg_ptr->output_tracker_ptr = NULL;
    }
    if (NULL != bg_ptr->image_surface_ptr) {
        cairo_surface_destroy(bg_ptr->image_surface_ptr);
        bg_ptr->image_surface_ptr = NULL;
    }
    free(bg_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_background_dlnode_set_image(
    bs_dllist_node_t *dlnode_ptr,
    const char *path_ptr,
    wlmaker_background_mode_t mode)
{
    struct wlmaker_background *bg_ptr = BS_CONTAINER_OF(
        dlnode_ptr, struct wlmaker_background, dlnode);

    if (NULL != bg_ptr->image_surface_ptr) {
        cairo_surface_destroy(bg_ptr->image_surface_ptr);
        bg_ptr->image_surface_ptr = NULL;
    }
    bg_ptr->mode = mode;

    if (NULL != path_ptr && 0 < strlen(path_ptr)) {
        cairo_surface_t *s_ptr = cairo_image_surface_create_from_png(path_ptr);
        if (CAIRO_STATUS_SUCCESS != cairo_surface_status(s_ptr)) {
            bs_log(BS_WARNING, "Failed to load wallpaper \"%s\": %s",
                   path_ptr,
                   cairo_status_to_string(cairo_surface_status(s_ptr)));
            cairo_surface_destroy(s_ptr);
        } else {
            bg_ptr->image_surface_ptr = s_ptr;
        }
    }

    wlmtk_output_tracker_for_each(
        bg_ptr->output_tracker_ptr,
        _wlmaker_background_panel_render_image,
        NULL);
}

/* ------------------------------------------------------------------------- */
void wlmaker_background_dlnode_set_color(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    struct wlmaker_background *bg_ptr = BS_CONTAINER_OF(
        dlnode_ptr, struct wlmaker_background, dlnode);

    wlmtk_output_tracker_for_each(
        bg_ptr->output_tracker_ptr,
        _wlmaker_backgrund_panel_set_color,
        ud_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_panel_vmt_t::request_size. Updates the panel size. */
uint32_t _wlmaker_background_panel_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height)
{
    wlmaker_background_panel_t *background_ptr = BS_CONTAINER_OF(
        panel_ptr, wlmaker_background_panel_t, super_panel);

    wlmtk_rectangle_set_size(background_ptr->rectangle_ptr, width, height);
    if (width != background_ptr->width || height != background_ptr->height) {
        background_ptr->width = width;
        background_ptr->height = height;
        _wlmaker_background_panel_render_image(
            NULL, NULL, background_ptr, NULL);
    }

    wlmtk_panel_commit(
        &background_ptr->super_panel, 0,
        &_wlmaker_background_panel_position);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Ctor. */
void *_wlmaker_background_panel_create(
    struct wlr_output *wlr_output_ptr,
    void *ud_ptr)
{
    struct wlmaker_background *background_ptr = ud_ptr;

    wlmaker_background_panel_t *background_panel_ptr = logged_calloc(
        1, sizeof(wlmaker_background_panel_t));
    if (NULL == background_panel_ptr) return NULL;
    background_panel_ptr->background_ptr = background_ptr;

    if (!wlmtk_panel_init(&background_panel_ptr->super_panel,
                          &_wlmaker_background_panel_position)) {
        _wlmaker_background_panel_destroy(
            NULL, NULL, background_panel_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&background_panel_ptr->super_panel,
                       &_wlmaker_background_panel_vmt);
    wlmtk_element_extend(
        wlmtk_panel_element(&background_panel_ptr->super_panel),
        &_wlmaker_background_panel_element_vmt);

    background_panel_ptr->rectangle_ptr = wlmtk_rectangle_create(
        0, 0, background_ptr->color);
    if (NULL == background_panel_ptr->rectangle_ptr) {
        _wlmaker_background_panel_destroy(
            NULL, NULL, background_panel_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_rectangle_element(background_panel_ptr->rectangle_ptr),
        true);
    wlmtk_container_add_element(
        &background_panel_ptr->super_panel.super_container,
        wlmtk_rectangle_element(background_panel_ptr->rectangle_ptr));

    if (!wlmtk_buffer_init(&background_panel_ptr->image_buffer)) {
        _wlmaker_background_panel_destroy(
            NULL, NULL, background_panel_ptr);
        return NULL;
    }
    background_panel_ptr->image_buffer_initialized = true;
    wlmtk_container_add_element(
        &background_panel_ptr->super_panel.super_container,
        wlmtk_buffer_element(&background_panel_ptr->image_buffer));

    wlmtk_element_set_visible(
        wlmtk_panel_element(&background_panel_ptr->super_panel),
        true);

    wlmtk_layer_add_panel(
        background_ptr->layer_ptr,
        &background_panel_ptr->super_panel,
        wlr_output_ptr);

    return background_panel_ptr;
}

/* ------------------------------------------------------------------------- */
/** Dtor. */
void _wlmaker_background_panel_destroy(
    __UNUSED__ struct wlr_output *wlr_output_ptr,
    __UNUSED__ void *ud_ptr,
    void *output_ptr)
{
    wlmaker_background_panel_t *background_panel_ptr = output_ptr;

    _wlmaker_background_panel_element_destroy(
        wlmtk_panel_element(&background_panel_ptr->super_panel));
    free(background_panel_ptr);
}

/* ------------------------------------------------------------------------- */
/** Dtor for the panel's element. Leaves the output handle intact. */
void _wlmaker_background_panel_element_destroy(wlmtk_element_t *element_ptr)
{
    wlmaker_background_panel_t *background_panel_ptr = BS_CONTAINER_OF(
        element_ptr,
        wlmaker_background_panel_t,
        super_panel.super_container.super_element);

    if (NULL != wlmtk_panel_get_layer(
            &background_panel_ptr->super_panel)) {
        wlmtk_layer_remove_panel(
            wlmtk_panel_get_layer(&background_panel_ptr->super_panel),
            &background_panel_ptr->super_panel);
    }

    if (background_panel_ptr->image_buffer_initialized) {
        wlmtk_container_remove_element(
            &background_panel_ptr->super_panel.super_container,
            wlmtk_buffer_element(&background_panel_ptr->image_buffer));
        wlmtk_buffer_fini(&background_panel_ptr->image_buffer);
        background_panel_ptr->image_buffer_initialized = false;
    }

    if (NULL != background_panel_ptr->rectangle_ptr) {
        wlmtk_container_remove_element(
            &background_panel_ptr->super_panel.super_container,
            wlmtk_rectangle_element(background_panel_ptr->rectangle_ptr));

        wlmtk_rectangle_destroy(background_panel_ptr->rectangle_ptr);
        background_panel_ptr->rectangle_ptr = NULL;
    }

    wlmtk_panel_fini(&background_panel_ptr->super_panel);
}

/* ------------------------------------------------------------------------- */
/** Sets the background color for the panel on this output. */
void _wlmaker_backgrund_panel_set_color(
    __UNUSED__ struct wlr_output *wlr_output_ptr,
    __UNUSED__ void *ud_ptr,
    void *output_ptr,
    void *arg_ptr)
{
    wlmaker_background_panel_t *background_panel_ptr = output_ptr;
    uint32_t *color_ptr = arg_ptr;

    if (NULL != background_panel_ptr->rectangle_ptr) {
        wlmtk_rectangle_set_color(
            background_panel_ptr->rectangle_ptr,
            *color_ptr);
    }
    background_panel_ptr->background_ptr->color = *color_ptr;
    _wlmaker_background_panel_render_image(NULL, NULL, output_ptr, NULL);
}

/* ------------------------------------------------------------------------- */
/**
 * (Re-)renders the wallpaper at the panel's size, or clears it if there is
 * no image. Signature fits @ref wlmtk_output_tracker_for_each.
 */
void _wlmaker_background_panel_render_image(
    __UNUSED__ struct wlr_output *wlr_output_ptr,
    __UNUSED__ void *ud_ptr,
    void *output_ptr,
    __UNUSED__ void *arg_ptr)
{
    wlmaker_background_panel_t *panel_ptr = output_ptr;
    struct wlmaker_background *bg_ptr = panel_ptr->background_ptr;
    if (!panel_ptr->image_buffer_initialized) return;

    cairo_surface_t *img_ptr = bg_ptr->image_surface_ptr;
    int w = panel_ptr->width, h = panel_ptr->height;
    if (NULL == img_ptr || 0 >= w || 0 >= h) {
        wlmtk_buffer_set(&panel_ptr->image_buffer, NULL);
        wlmtk_element_set_visible(
            wlmtk_buffer_element(&panel_ptr->image_buffer), false);
        return;
    }

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(w, h);
    if (NULL == wlr_buffer_ptr) return;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return;
    }

    // The color shows through where the image doesn't cover.
    uint32_t c = bg_ptr->color;
    cairo_set_source_rgba(cairo_ptr,
                          ((c >> 16) & 0xff) / 255.0,
                          ((c >> 8) & 0xff) / 255.0,
                          (c & 0xff) / 255.0,
                          ((c >> 24) & 0xff) / 255.0);
    cairo_paint(cairo_ptr);

    double iw = cairo_image_surface_get_width(img_ptr);
    double ih = cairo_image_surface_get_height(img_ptr);
    double sx = 1.0, sy = 1.0;
    switch (bg_ptr->mode) {
    case WLMAKER_BACKGROUND_SCALE:
        sx = sy = BS_MIN(w / iw, h / ih);
        break;
    case WLMAKER_BACKGROUND_STRETCH:
        sx = w / iw;
        sy = h / ih;
        break;
    case WLMAKER_BACKGROUND_CENTER:
    case WLMAKER_BACKGROUND_TILE:
        break;
    case WLMAKER_BACKGROUND_FILL:
    default:
        sx = sy = BS_MAX(w / iw, h / ih);
        break;
    }

    if (WLMAKER_BACKGROUND_TILE == bg_ptr->mode) {
        cairo_pattern_t *pattern_ptr =
            cairo_pattern_create_for_surface(img_ptr);
        cairo_pattern_set_extend(pattern_ptr, CAIRO_EXTEND_REPEAT);
        cairo_set_source(cairo_ptr, pattern_ptr);
        cairo_paint(cairo_ptr);
        cairo_pattern_destroy(pattern_ptr);
    } else {
        cairo_translate(cairo_ptr, (w - iw * sx) / 2.0, (h - ih * sy) / 2.0);
        cairo_scale(cairo_ptr, sx, sy);
        cairo_set_source_surface(cairo_ptr, img_ptr, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cairo_ptr),
                                 CAIRO_FILTER_GOOD);
        cairo_paint(cairo_ptr);
    }
    cairo_destroy(cairo_ptr);

    wlmtk_buffer_set(&panel_ptr->image_buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&panel_ptr->image_buffer), true);
}

/* == End of background.c ================================================== */
