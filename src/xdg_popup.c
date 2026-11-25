/* ========================================================================= */
/**
 * @file xdg_popup.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2023 Google LLC
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

#include "xdg_popup.h"

#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#undef WLR_USE_UNSTABLE

/* == Declarations ========================================================= */

/** State of toolkit popup. */
struct _wlmaker_xdg_popup_t {
    /** Base element for the popup. Surface is the content. */
    wlmtk_base_t              base;

    /** Listener for the `reposition` signal of `wlr_xdg_popup::events` */
    struct wl_listener        reposition_listener;
    /** Listener for the `destroy` signal of `wlr_xdg_surface::events`. */
    struct wl_listener        destroy_listener;
    /** Listener for the `new_popup` signal of `wlr_xdg_surface::events`. */
    struct wl_listener        new_popup_listener;
    /** Listener for the `commit` signal of the `wlr_surface`. */
    struct wl_listener        surface_commit_listener;

    /** Seat. */
    struct wlr_seat           *wlr_seat_ptr;
    /** Output layout, for keeping the popup on its output. May be NULL. */
    struct wlr_output_layout  *wlr_output_layout_ptr;
    /** The WLR popup. */
    struct wlr_xdg_popup      *wlr_xdg_popup_ptr;
};

static void handle_reposition(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void handle_new_popup(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _wlmaker_xdg_popup_element_destroy(
    wlmtk_element_t *element_ptr);

/* == Data ================================================================= */

/** Virtual method table of the parent's @ref wlmtk_element_t. */
static const wlmtk_element_vmt_t _wlmaker_xdg_popup_element_vmt = {
    .destroy = _wlmaker_xdg_popup_element_destroy
};

static void handle_surface_commit(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_xdg_popup_t *wlmaker_xdg_popup_create(
    struct wlr_xdg_popup *wlr_xdg_popup_ptr,
    struct wlr_seat *wlr_seat_ptr,
    struct wlr_output_layout *wlr_output_layout_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = logged_calloc(
        1, sizeof(wlmaker_xdg_popup_t));
    if (NULL == wlmaker_xdg_popup_ptr) return NULL;
    wlmaker_xdg_popup_ptr->wlr_seat_ptr = wlr_seat_ptr;
    wlmaker_xdg_popup_ptr->wlr_output_layout_ptr = wlr_output_layout_ptr;
    wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr = wlr_xdg_popup_ptr;

    if (!wlmtk_base_init(&wlmaker_xdg_popup_ptr->base, NULL)) {
        wlmaker_xdg_popup_destroy(wlmaker_xdg_popup_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmaker_xdg_popup_element(wlmaker_xdg_popup_ptr), true);

    wlmtk_surface_t *surface_ptr = wlmtk_surface_create(
        wlr_xdg_popup_ptr->base->surface,
        wlr_seat_ptr);
    if (NULL == surface_ptr) {
        wlmaker_xdg_popup_destroy(wlmaker_xdg_popup_ptr);
        return NULL;
    }
    wlmtk_base_set_content_element(
        &wlmaker_xdg_popup_ptr->base,
        wlmtk_surface_element(surface_ptr));
    wlmtk_util_connect_listener_signal(
        &wlr_xdg_popup_ptr->base->surface->events.commit,
        &wlmaker_xdg_popup_ptr->surface_commit_listener,
        handle_surface_commit);

    wlmtk_element_extend(
        wlmtk_base_element(&wlmaker_xdg_popup_ptr->base),
        &_wlmaker_xdg_popup_element_vmt);
    wlmtk_element_set_position(
        wlmtk_base_element(&wlmaker_xdg_popup_ptr->base),
        wlr_xdg_popup_ptr->scheduled.geometry.x,
        wlr_xdg_popup_ptr->scheduled.geometry.y);

    wlmtk_util_connect_listener_signal(
        &wlr_xdg_popup_ptr->events.reposition,
        &wlmaker_xdg_popup_ptr->reposition_listener,
        handle_reposition);
    wlmtk_util_connect_listener_signal(
        &wlr_xdg_popup_ptr->events.destroy,
        &wlmaker_xdg_popup_ptr->destroy_listener,
        handle_destroy);
    wlmtk_util_connect_listener_signal(
        &wlr_xdg_popup_ptr->base->events.new_popup,
        &wlmaker_xdg_popup_ptr->new_popup_listener,
        handle_new_popup);

    bs_log(BS_INFO, "Created XDG popup %p", wlmaker_xdg_popup_ptr);
    return wlmaker_xdg_popup_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_xdg_popup_destroy(wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr)
{
    bs_log(BS_INFO, "Destroying XDG popup %p", wlmaker_xdg_popup_ptr);

    // For XDG popups, we don't keep reference when adding to a container. So
    // we need to remove from a potential parent container.
    wlmtk_element_t *e = wlmtk_base_element(&wlmaker_xdg_popup_ptr->base);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_remove_element(e->parent_container_ptr, e);
    }

    wlmtk_util_disconnect_listener(
        &wlmaker_xdg_popup_ptr->new_popup_listener);
    wlmtk_util_disconnect_listener(
        &wlmaker_xdg_popup_ptr->destroy_listener);
    wlmtk_util_disconnect_listener(
        &wlmaker_xdg_popup_ptr->reposition_listener);
    wlmtk_util_disconnect_listener(
        &wlmaker_xdg_popup_ptr->surface_commit_listener);

    wlmtk_base_fini(&wlmaker_xdg_popup_ptr->base);
    free(wlmaker_xdg_popup_ptr);
}

/* ------------------------------------------------------------------------- */
wlmtk_element_t *wlmaker_xdg_popup_element(
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr)
{
    return wlmtk_base_element(&wlmaker_xdg_popup_ptr->base);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Keeps the popup on the output it opens on: Flips or slides it there, as
 * far as its positioner allows (eg. a menu from a panel at the bottom opens
 * upwards). Updates the scheduled geometry and the element's position.
 */
static void _wlmaker_xdg_popup_unconstrain(
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr)
{
    struct wlr_xdg_popup *popup_ptr = wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr;
    wlmtk_element_t *element_ptr = wlmaker_xdg_popup_element(
        wlmaker_xdg_popup_ptr);
    if (NULL == wlmaker_xdg_popup_ptr->wlr_output_layout_ptr ||
        NULL == element_ptr->wlr_scene_node_ptr) return;

    // The popup shows at its parent's origin, offset by its geometry.
    int lx, ly;
    if (!wlr_scene_node_coords(element_ptr->wlr_scene_node_ptr, &lx, &ly)) {
        return;
    }
    int parent_x = lx - popup_ptr->scheduled.geometry.x;
    int parent_y = ly - popup_ptr->scheduled.geometry.y;

    // The output holding the anchor; else the one closest to it.
    const struct wlr_box *anchor_ptr = &popup_ptr->scheduled.rules.anchor_rect;
    double ax = parent_x + anchor_ptr->x + anchor_ptr->width / 2.0;
    double ay = parent_y + anchor_ptr->y + anchor_ptr->height / 2.0;
    struct wlr_output *wlr_output_ptr = wlr_output_layout_output_at(
        wlmaker_xdg_popup_ptr->wlr_output_layout_ptr, ax, ay);
    if (NULL == wlr_output_ptr) {
        double cx, cy;
        wlr_output_layout_closest_point(
            wlmaker_xdg_popup_ptr->wlr_output_layout_ptr, NULL, ax, ay,
            &cx, &cy);
        wlr_output_ptr = wlr_output_layout_output_at(
            wlmaker_xdg_popup_ptr->wlr_output_layout_ptr, cx, cy);
    }
    if (NULL == wlr_output_ptr) return;
    struct wlr_box output_box;
    wlr_output_layout_get_box(
        wlmaker_xdg_popup_ptr->wlr_output_layout_ptr, wlr_output_ptr,
        &output_box);

    // wlroots takes the box relative to the root surface, and subtracts the
    // parent's offset in there: Pre-add it, to land in the parent's space.
    int root_x, root_y;
    wlr_xdg_popup_get_toplevel_coords(popup_ptr, 0, 0, &root_x, &root_y);
    struct wlr_box box = {
        .x = output_box.x - parent_x + root_x,
        .y = output_box.y - parent_y + root_y,
        .width = output_box.width,
        .height = output_box.height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup_ptr, &box);
    wlmtk_element_set_position(
        element_ptr,
        popup_ptr->scheduled.geometry.x,
        popup_ptr->scheduled.geometry.y);
}

/* ------------------------------------------------------------------------- */
/** Handles repositioning. */
void handle_reposition(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_popup_t, reposition_listener);

    wlmtk_element_set_position(
        wlmtk_base_element(&wlmaker_xdg_popup_ptr->base),
        wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr->scheduled.geometry.x,
        wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr->scheduled.geometry.y);
    _wlmaker_xdg_popup_unconstrain(wlmaker_xdg_popup_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles popup destruction: Removes from parent, and destroy. */
void handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_popup_t, destroy_listener);

    wlmaker_xdg_popup_destroy(wlmaker_xdg_popup_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles further popups. Creates them and adds them to parent's content. */
void handle_new_popup(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_popup_t, new_popup_listener);
    struct wlr_xdg_popup *wlr_xdg_popup_ptr = data_ptr;

    wlmaker_xdg_popup_t *new_popup_ptr = wlmaker_xdg_popup_create(
        wlr_xdg_popup_ptr,
        wlmaker_xdg_popup_ptr->wlr_seat_ptr,
        wlmaker_xdg_popup_ptr->wlr_output_layout_ptr);
    if (NULL == new_popup_ptr) {
        wl_resource_post_error(
            wlr_xdg_popup_ptr->resource,
            WL_DISPLAY_ERROR_NO_MEMORY,
            "Failed wlmtk_xdg_popup_create.");
        return;
    }

    wlmtk_base_push_element(
        &wlmaker_xdg_popup_ptr->base,
        wlmaker_xdg_popup_element(new_popup_ptr));

    bs_log(BS_INFO, "XDG popup %p: New popup %p",
           wlmaker_xdg_popup_ptr, wlr_xdg_popup_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handles `commit` for the popup's surface. */
void handle_surface_commit(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_xdg_popup_t, surface_commit_listener);

    if (wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr->base->initial_commit) {
        // Initial commit: Ensure a configure is responded with.
        wlr_xdg_surface_schedule_configure(
            wlmaker_xdg_popup_ptr->wlr_xdg_popup_ptr->base);
        _wlmaker_xdg_popup_unconstrain(wlmaker_xdg_popup_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Implementation of @ref wlmtk_element_vmt_t::destroy. Virtual dtor.
 *
 * @param element_ptr
 */
void _wlmaker_xdg_popup_element_destroy(
    wlmtk_element_t *element_ptr)
{
    wlmaker_xdg_popup_t *wlmaker_xdg_popup_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_xdg_popup_t,
        base.super_container.super_element);

    wlmaker_xdg_popup_destroy(wlmaker_xdg_popup_ptr);
}

/* == End of xdg_popup.c ==================================================== */
