/* ========================================================================= */
/**
 * @file icon_area.c
 *
 * @copyright
 * Copyright 2026 Petr Demuth
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

#include "icon_area.h"

#include <cairo.h>
#include <libbase/libbase.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "app_icon.h"
#include "backend/backend.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** Icon area handle. */
struct _wlmaker_icon_area_t {
    /** Toolkit dock, holding the mini-window tiles. */
    wlmtk_dock_t              *wlmtk_dock_ptr;
    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;
    /** Tile style, for creating mini-windows. */
    struct wlmtk_tile_style   tile_style;

    /** Mini-windows, @ref wlmaker_miniwindow_t::dlnode. */
    bs_dllist_t               miniwindows;

    /** Listener for @ref wlmtk_root_events_t::window_minimize_changed. */
    struct wl_listener        window_minimize_changed_listener;
    /** Listener for @ref wlmtk_root_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;
    /** Listener for wlr_output_layout::events.change. */
    struct wl_listener        output_layout_change_listener;
    /** Listener for @ref wlmaker_server_t::theme_changed_event. */
    struct wl_listener        theme_changed_listener;
};

/** A mini-window: The tile representing a minimized window. */
typedef struct {
    /** The mini-window is a tile. */
    wlmtk_tile_t              super_tile;
    /** Original virtual method table of the tile's element. */
    wlmtk_element_vmt_t       orig_element_vmt;
    /** Element of @ref wlmaker_icon_area_t::miniwindows. */
    bs_dllist_node_t          dlnode;

    /** Back-link to the icon area. */
    wlmaker_icon_area_t       *icon_area_ptr;
    /** The minimized window. */
    wlmtk_window_t            *window_ptr;
    /** Whether the tile is currently added to the dock. */
    bool                      docked;

    /** Content: A stylized window. */
    wlmtk_buffer_t            content_buffer;
    /** Overlay: The window's title, along the top. */
    wlmtk_buffer_t            overlay_buffer;

    /** Pending restore, see @ref _wlmaker_miniwindow_restore_idle. */
    struct wl_event_source    *restore_idle_ptr;
} wlmaker_miniwindow_t;

static wlmaker_miniwindow_t *_wlmaker_miniwindow_create(
    wlmaker_icon_area_t *icon_area_ptr,
    wlmtk_window_t *window_ptr);
static void _wlmaker_miniwindow_destroy(wlmaker_miniwindow_t *mw_ptr);
static void _wlmaker_miniwindow_redraw(wlmaker_miniwindow_t *mw_ptr);
static struct wlr_buffer *_wlmaker_miniwindow_create_content(
    int size,
    const char *app_id_ptr);
static struct wlr_buffer *_wlmaker_miniwindow_create_overlay(
    int size,
    const char *title_ptr);
static void _wlmaker_miniwindow_restore_idle(void *data_ptr);
static void _wlmaker_miniwindow_element_destroy(wlmtk_element_t *element_ptr);
static bool _wlmaker_miniwindow_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr);
static bool _wlmaker_miniwindow_set_content_size(
    wlmtk_tile_t *tile_ptr,
    uint64_t content_size);

static wlmaker_miniwindow_t *_wlmaker_icon_area_find(
    wlmaker_icon_area_t *icon_area_ptr,
    wlmtk_window_t *window_ptr);
static void _wlmaker_icon_area_sync(wlmaker_icon_area_t *icon_area_ptr);
static void _wlmaker_icon_area_attach(wlmaker_icon_area_t *icon_area_ptr);

static void _wlmaker_icon_area_handle_window_minimize_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_icon_area_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_icon_area_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_icon_area_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

/** The mini-window's extension to @ref wlmtk_element_vmt_t. */
static const wlmtk_element_vmt_t _wlmaker_miniwindow_element_vmt = {
    .destroy = _wlmaker_miniwindow_element_destroy,
    .pointer_button = _wlmaker_miniwindow_pointer_button,
};

/** The mini-window's extension to @ref wlmtk_tile_vmt_t. */
static const wlmtk_tile_vmt_t _wlmaker_miniwindow_tile_vmt = {
    .set_content_size = _wlmaker_miniwindow_set_content_size,
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_icon_area_t *wlmaker_icon_area_create(
    wlmaker_server_t *server_ptr,
    const wlmaker_config_style_t *style_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = logged_calloc(
        1, sizeof(wlmaker_icon_area_t));
    if (NULL == icon_area_ptr) return NULL;
    icon_area_ptr->server_ptr = server_ptr;
    icon_area_ptr->tile_style = style_ptr->tile;

    wlmtk_dock_positioning_t positioning = {
        .edge = WLR_EDGE_BOTTOM,
        .anchor = WLR_EDGE_LEFT,
    };
    icon_area_ptr->wlmtk_dock_ptr = wlmtk_dock_create(
        &positioning, &style_ptr->dock);
    if (NULL == icon_area_ptr->wlmtk_dock_ptr) {
        wlmaker_icon_area_destroy(icon_area_ptr);
        return NULL;
    }
    // Shown only while holding mini-windows. See _wlmaker_icon_area_sync.
    wlmtk_element_set_visible(
        wlmtk_dock_element(icon_area_ptr->wlmtk_dock_ptr), false);
    _wlmaker_icon_area_attach(icon_area_ptr);

    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->window_minimize_changed,
        &icon_area_ptr->window_minimize_changed_listener,
        _wlmaker_icon_area_handle_window_minimize_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->workspace_changed,
        &icon_area_ptr->workspace_changed_listener,
        _wlmaker_icon_area_handle_workspace_changed);
    wlmtk_util_connect_listener_signal(
        &server_ptr->theme_changed_event,
        &icon_area_ptr->theme_changed_listener,
        _wlmaker_icon_area_handle_theme_changed);
    // As in dock.c: Re-attach before the layer's handler drops our panel.
    icon_area_ptr->output_layout_change_listener.notify =
        _wlmaker_icon_area_handle_output_layout_change;
    wl_list_insert(
        server_ptr->wlr_output_layout_ptr->events.change.listener_list.next,
        &icon_area_ptr->output_layout_change_listener.link);

    bs_log(BS_INFO, "Created icon area %p", icon_area_ptr);
    return icon_area_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_icon_area_destroy(wlmaker_icon_area_t *icon_area_ptr)
{
    wlmtk_util_disconnect_listener(
        &icon_area_ptr->output_layout_change_listener);
    wlmtk_util_disconnect_listener(&icon_area_ptr->theme_changed_listener);
    wlmtk_util_disconnect_listener(&icon_area_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(
        &icon_area_ptr->window_minimize_changed_listener);

    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = icon_area_ptr->miniwindows.head_ptr)) {
        _wlmaker_miniwindow_destroy(BS_CONTAINER_OF(
                                        dlnode_ptr, wlmaker_miniwindow_t, dlnode));
    }

    if (NULL != icon_area_ptr->wlmtk_dock_ptr) {
        wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(
            icon_area_ptr->wlmtk_dock_ptr);
        if (NULL != wlmtk_panel_get_layer(panel_ptr)) {
            wlmtk_layer_remove_panel(wlmtk_panel_get_layer(panel_ptr),
                                     panel_ptr);
        }
        wlmtk_dock_destroy(icon_area_ptr->wlmtk_dock_ptr);
        icon_area_ptr->wlmtk_dock_ptr = NULL;
    }
    free(icon_area_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Creates a mini-window for `window_ptr`. Not yet docked. */
wlmaker_miniwindow_t *_wlmaker_miniwindow_create(
    wlmaker_icon_area_t *icon_area_ptr,
    wlmtk_window_t *window_ptr)
{
    wlmaker_miniwindow_t *mw_ptr = logged_calloc(
        1, sizeof(wlmaker_miniwindow_t));
    if (NULL == mw_ptr) return NULL;
    mw_ptr->icon_area_ptr = icon_area_ptr;
    mw_ptr->window_ptr = window_ptr;

    if (!wlmtk_tile_init(&mw_ptr->super_tile, &icon_area_ptr->tile_style)) {
        free(mw_ptr);
        return NULL;
    }
    mw_ptr->orig_element_vmt = wlmtk_element_extend(
        wlmtk_tile_element(&mw_ptr->super_tile),
        &_wlmaker_miniwindow_element_vmt);
    wlmtk_tile_extend(&mw_ptr->super_tile, &_wlmaker_miniwindow_tile_vmt);
    wlmtk_element_set_visible(wlmtk_tile_element(&mw_ptr->super_tile), true);
    bs_dllist_push_back(&icon_area_ptr->miniwindows, &mw_ptr->dlnode);

    if (!wlmtk_buffer_init(&mw_ptr->content_buffer) ||
        !wlmtk_buffer_init(&mw_ptr->overlay_buffer)) {
        _wlmaker_miniwindow_destroy(mw_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&mw_ptr->content_buffer), true);
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&mw_ptr->overlay_buffer), true);
    _wlmaker_miniwindow_redraw(mw_ptr);
    wlmtk_tile_set_content(
        &mw_ptr->super_tile, wlmtk_buffer_element(&mw_ptr->content_buffer));
    wlmtk_tile_set_overlay(
        &mw_ptr->super_tile, wlmtk_buffer_element(&mw_ptr->overlay_buffer));
    bs_log(BS_INFO, "Created mini-window %p for window %p", mw_ptr, window_ptr);
    return mw_ptr;
}

/* ------------------------------------------------------------------------- */
/** Undocks (if needed) and destroys the mini-window. */
void _wlmaker_miniwindow_destroy(wlmaker_miniwindow_t *mw_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = mw_ptr->icon_area_ptr;
    if (NULL != mw_ptr->restore_idle_ptr) {
        wl_event_source_remove(mw_ptr->restore_idle_ptr);
        mw_ptr->restore_idle_ptr = NULL;
    }
    if (mw_ptr->docked) {
        wlmtk_dock_remove_tile(icon_area_ptr->wlmtk_dock_ptr,
                               &mw_ptr->super_tile);
        mw_ptr->docked = false;
    }
    bs_dllist_remove(&icon_area_ptr->miniwindows, &mw_ptr->dlnode);
    bs_log(BS_INFO, "Destroying mini-window %p for window %p",
           mw_ptr, mw_ptr->window_ptr);

    wlmtk_tile_set_overlay(&mw_ptr->super_tile, NULL);
    wlmtk_tile_set_content(&mw_ptr->super_tile, NULL);
    wlmtk_buffer_fini(&mw_ptr->overlay_buffer);
    wlmtk_buffer_fini(&mw_ptr->content_buffer);
    wlmtk_tile_fini(&mw_ptr->super_tile);
    free(mw_ptr);
}

/* ------------------------------------------------------------------------- */
/** Re-draws content and overlay, at the tile's current style. */
void _wlmaker_miniwindow_redraw(wlmaker_miniwindow_t *mw_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = _wlmaker_miniwindow_create_content(
        mw_ptr->super_tile.style.content_size,
        wlmtk_window_get_app_id(mw_ptr->window_ptr));
    if (NULL != wlr_buffer_ptr) {
        wlmtk_buffer_set(&mw_ptr->content_buffer, wlr_buffer_ptr);
        wlr_buffer_drop(wlr_buffer_ptr);
    }

    wlr_buffer_ptr = _wlmaker_miniwindow_create_overlay(
        mw_ptr->super_tile.style.size,
        wlmtk_window_get_title(mw_ptr->window_ptr));
    if (NULL != wlr_buffer_ptr) {
        wlmtk_buffer_set(&mw_ptr->overlay_buffer, wlr_buffer_ptr);
        wlr_buffer_drop(wlr_buffer_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the application's icon, below the overlay's title bar. Without an
 * icon: A stylized window, with frame, titlebar, buttons and content.
 */
struct wlr_buffer *_wlmaker_miniwindow_create_content(
    int s,
    const char *app_id_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(s, s);
    if (NULL == wlr_buffer_ptr) return NULL;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }

    char *icon_path_ptr = wlmaker_app_icon_find(app_id_ptr);
    if (NULL != icon_path_ptr) {
        int icon_size = 0.72 * s;
        cairo_surface_t *icon_ptr = wlmaker_app_icon_load(
            icon_path_ptr, icon_size);
        free(icon_path_ptr);
        if (NULL != icon_ptr) {
            cairo_set_source_surface(
                cairo_ptr, icon_ptr, (s - icon_size) / 2, s - icon_size - 1);
            cairo_paint(cairo_ptr);
            cairo_surface_destroy(icon_ptr);
            cairo_destroy(cairo_ptr);
            return wlr_buffer_ptr;
        }
    }

    // Leaves the top ~20% for the overlay's title.
    double x = 0.14 * s, y = 0.30 * s, w = 0.72 * s, h = 0.58 * s;
    double tb = 0.12 * s;

    cairo_set_source_argb8888(cairo_ptr, 0xff000000);
    cairo_rectangle(cairo_ptr, x - 1, y - 1, w + 2, h + 2);
    cairo_fill(cairo_ptr);

    cairo_set_source_argb8888(cairo_ptr, 0xffd8d8d8);
    cairo_rectangle(cairo_ptr, x, y + tb, w, h - tb);
    cairo_fill(cairo_ptr);

    cairo_set_source_argb8888(cairo_ptr, 0xff2a2a2a);
    cairo_rectangle(cairo_ptr, x, y, w, tb);
    cairo_fill(cairo_ptr);

    // Miniaturize and close buttons, as on the titlebar.
    cairo_set_source_argb8888(cairo_ptr, 0xffa0a0a0);
    cairo_rectangle(cairo_ptr, x + 1, y + 1, tb - 2, tb - 2);
    cairo_rectangle(cairo_ptr, x + w - tb + 1, y + 1, tb - 2, tb - 2);
    cairo_fill(cairo_ptr);

    // A few lines of "content".
    cairo_set_source_argb8888(cairo_ptr, 0xff8a8a8a);
    for (int i = 0; i < 3; ++i) {
        cairo_rectangle(cairo_ptr, x + 0.1 * w, y + tb + (0.2 + 0.25 * i) * (h - tb),
                        (i == 2 ? 0.5 : 0.8) * w, 0.06 * s);
    }
    cairo_fill(cairo_ptr);

    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
/** Draws the title bar along the tile's top, with the (truncated) title. */
struct wlr_buffer *_wlmaker_miniwindow_create_overlay(
    int s,
    const char *title_ptr)
{
    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(s, s);
    if (NULL == wlr_buffer_ptr) return NULL;
    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }

    double bar = 12.0 * s / 64.0;
    cairo_set_source_argb8888(cairo_ptr, 0xff000000);
    cairo_rectangle(cairo_ptr, 0, 0, s, bar);
    cairo_fill(cairo_ptr);

    char text[128];
    snprintf(text, sizeof(text), "%s",
             (NULL != title_ptr && *title_ptr) ? title_ptr : "(untitled)");
    cairo_select_font_face(cairo_ptr, "Helvetica",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cairo_ptr, 9.0 * s / 64.0);
    double max_width = s - 4.0 * s / 64.0;
    cairo_text_extents_t extents;
    cairo_text_extents(cairo_ptr, text, &extents);
    for (size_t len = strlen(text);
         extents.x_advance > max_width && len > 3;
         --len) {
        // Drop one more character (and any UTF-8 continuation bytes).
        size_t cut = len - 1;
        while (cut > 0 && (text[cut] & 0xc0) == 0x80) --cut;
        len = cut + 1;
        strcpy(&text[cut], "..");
        cairo_text_extents(cairo_ptr, text, &extents);
    }

    cairo_set_source_argb8888(cairo_ptr, 0xffffffff);
    cairo_move_to(cairo_ptr, 2.0 * s / 64.0, bar - 3.0 * s / 64.0);
    cairo_show_text(cairo_ptr, text);

    cairo_destroy(cairo_ptr);
    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::destroy. Destroys the mini-window. */
void _wlmaker_miniwindow_element_destroy(wlmtk_element_t *element_ptr)
{
    wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_miniwindow_t,
        super_tile.super_container.super_element);
    // Only reached if the dock is destroyed while holding the tile.
    mw_ptr->docked = false;
    _wlmaker_miniwindow_destroy(mw_ptr);
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_element_vmt_t::pointer_button. */
bool _wlmaker_miniwindow_pointer_button(
    wlmtk_element_t *element_ptr,
    const wlmtk_button_event_t *button_event_ptr)
{
    wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
        element_ptr, wlmaker_miniwindow_t,
        super_tile.super_container.super_element);

    // Double-click restores, as in Window Maker.
    if (BTN_LEFT != button_event_ptr->button ||
        WLMTK_BUTTON_DOUBLE_CLICK != button_event_ptr->type) return true;

    // Restoring destroys this mini-window (see window_minimize_changed), so
    // it must not happen while the toolkit is still dispatching this event.
    if (NULL == mw_ptr->restore_idle_ptr) {
        mw_ptr->restore_idle_ptr = wl_event_loop_add_idle(
            wl_display_get_event_loop(
                mw_ptr->icon_area_ptr->server_ptr->wl_display_ptr),
            _wlmaker_miniwindow_restore_idle,
            mw_ptr);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/** Restores the mini-window's window. Destroys `data_ptr`. */
void _wlmaker_miniwindow_restore_idle(void *data_ptr)
{
    wlmaker_miniwindow_t *mw_ptr = data_ptr;
    // Idle sources are one-shot: Already removed when dispatched.
    mw_ptr->restore_idle_ptr = NULL;

    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        mw_ptr->window_ptr);
    if (NULL != workspace_ptr) {
        wlmtk_workspace_restore_window(workspace_ptr, mw_ptr->window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Implements @ref wlmtk_tile_vmt_t::set_content_size. Redraws. */
bool _wlmaker_miniwindow_set_content_size(
    wlmtk_tile_t *tile_ptr,
    __UNUSED__ uint64_t content_size)
{
    wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
        tile_ptr, wlmaker_miniwindow_t, super_tile);
    _wlmaker_miniwindow_redraw(mw_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** @return the mini-window for `window_ptr`, or NULL. */
wlmaker_miniwindow_t *_wlmaker_icon_area_find(
    wlmaker_icon_area_t *icon_area_ptr,
    wlmtk_window_t *window_ptr)
{
    for (bs_dllist_node_t *dlnode_ptr = icon_area_ptr->miniwindows.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_miniwindow_t, dlnode);
        if (mw_ptr->window_ptr == window_ptr) return mw_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/**
 * Docks the mini-windows of the current workspace, undocks all others.
 * The icon area is visible only while holding mini-windows.
 */
void _wlmaker_icon_area_sync(wlmaker_icon_area_t *icon_area_ptr)
{
    wlmtk_workspace_t *current_ptr = wlmtk_root_get_current_workspace(
        icon_area_ptr->server_ptr->root_ptr);
    bool any_docked = false;

    for (bs_dllist_node_t *dlnode_ptr = icon_area_ptr->miniwindows.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_miniwindow_t, dlnode);
        bool show = wlmtk_window_get_workspace(mw_ptr->window_ptr) ==
            current_ptr;
        if (show && !mw_ptr->docked) {
            wlmtk_dock_add_tile(icon_area_ptr->wlmtk_dock_ptr,
                                &mw_ptr->super_tile);
        } else if (!show && mw_ptr->docked) {
            wlmtk_dock_remove_tile(icon_area_ptr->wlmtk_dock_ptr,
                                   &mw_ptr->super_tile);
        }
        mw_ptr->docked = show;
        any_docked |= show;
    }

    wlmtk_element_set_visible(
        wlmtk_dock_element(icon_area_ptr->wlmtk_dock_ptr), any_docked);
}

/* ------------------------------------------------------------------------- */
/** (Re-)attaches the icon area to the current workspace's top layer. */
void _wlmaker_icon_area_attach(wlmaker_icon_area_t *icon_area_ptr)
{
    wlmaker_server_t *server_ptr = icon_area_ptr->server_ptr;
    wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(icon_area_ptr->wlmtk_dock_ptr);
    wlmtk_layer_t *current_layer_ptr = wlmtk_panel_get_layer(panel_ptr);
    wlmtk_layer_t *new_layer_ptr = wlmtk_workspace_get_layer(
        wlmtk_root_get_current_workspace(server_ptr->root_ptr),
        WLMTK_WORKSPACE_LAYER_TOP);

    struct wlr_output *wlr_output_ptr = wlmbe_primary_output(
        server_ptr->wlr_output_layout_ptr);
    if (NULL != current_layer_ptr) {
        wlmtk_layer_remove_panel(current_layer_ptr, panel_ptr);
    }
    if (NULL == wlr_output_ptr) return;
    if (!wlmtk_layer_add_panel(new_layer_ptr, panel_ptr, wlr_output_ptr)) {
        bs_log(BS_WARNING, "Failed to add icon area to layer %p",
               new_layer_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Creates or destroys the window's mini-window. */
void _wlmaker_icon_area_handle_window_minimize_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_icon_area_t, window_minimize_changed_listener);
    wlmtk_window_t *window_ptr = data_ptr;

    wlmaker_miniwindow_t *mw_ptr = _wlmaker_icon_area_find(
        icon_area_ptr, window_ptr);
    if (wlmtk_window_is_minimized(window_ptr)) {
        if (NULL == mw_ptr) {
            _wlmaker_miniwindow_create(icon_area_ptr, window_ptr);
        }
    } else if (NULL != mw_ptr) {
        _wlmaker_miniwindow_destroy(mw_ptr);
    }
    _wlmaker_icon_area_sync(icon_area_ptr);
}

/* ------------------------------------------------------------------------- */
/** Moves the icon area to the new workspace, shows its mini-windows. */
void _wlmaker_icon_area_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_icon_area_t, workspace_changed_listener);
    _wlmaker_icon_area_attach(icon_area_ptr);
    _wlmaker_icon_area_sync(icon_area_ptr);
}

/* ------------------------------------------------------------------------- */
/** Re-attaches to the (possibly new) primary output. */
void _wlmaker_icon_area_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_icon_area_t, output_layout_change_listener);
    _wlmaker_icon_area_attach(icon_area_ptr);
}

/* ------------------------------------------------------------------------- */
/** Applies the new theme to the dock and mini-windows. */
void _wlmaker_icon_area_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_icon_area_t *icon_area_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_icon_area_t, theme_changed_listener);
    wlmaker_config_style_t *style_ptr = data_ptr;

    icon_area_ptr->tile_style = style_ptr->tile;
    // Applies to docked tiles only; undocked ones pick it up when docked.
    wlmtk_dock_set_style(
        icon_area_ptr->wlmtk_dock_ptr, &style_ptr->dock, &style_ptr->tile);
    for (bs_dllist_node_t *dlnode_ptr = icon_area_ptr->miniwindows.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_miniwindow_t *mw_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_miniwindow_t, dlnode);
        if (!mw_ptr->docked) {
            wlmtk_tile_set_style(&mw_ptr->super_tile, &style_ptr->tile);
        }
    }
}

/* == End of icon_area.c =================================================== */
