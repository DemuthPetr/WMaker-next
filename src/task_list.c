/* ========================================================================= */
/**
 * @file task_list.c
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

/// for PATH_MAX.
#define _POSIX_C_SOURCE 1

#include "task_list.h"

#include <cairo.h>
#include <inttypes.h>
#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <wayland-server-core.h>
/// Include unstable interfaces of wlroots.
#define WLR_USE_UNSTABLE
#include <wlr/util/edges.h>
#include <wlr/interfaces/wlr_buffer.h>
#undef WLR_USE_UNSTABLE

#include "app_icon.h"
#include "config.h"
#include "server.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the task list. */
struct _wlmaker_task_list_t {
    /** Derived from a toolkit panel. */
    wlmtk_panel_t             super_panel;

    /** Buffer that shows the tasklist's content. */
    wlmtk_buffer_t            buffer;

    /** Backlink to the server. */
    wlmaker_server_t          *server_ptr;

    /** Listener for the `task_list_enabled` signal by `wlmaker_server_t`. */
    struct wl_listener        task_list_enabled_listener;
    /** Listener for the `task_list_disabled` signal by `wlmaker_server_t`. */
    struct wl_listener        task_list_disabled_listener;

    /** Listener for @ref wlmtk_root_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Listener for @ref wlmtk_root_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;

    /** Listener for @ref wlmaker_server_t::theme_changed_event. */
    struct wl_listener        theme_changed_listener;

    /** Whether the task list is currently enabled (mapped). */
    bool                      enabled;
    /** Visual style. */
    struct wlmaker_task_list_style style;
};

static bool _wlmaker_task_list_refresh(
    wlmaker_task_list_t *task_list_ptr,
    const struct wlmaker_task_list_style *style_ptr);
static struct wlr_buffer *create_wlr_buffer(
    wlmtk_workspace_t *workspace_ptr,
    const struct wlmaker_task_list_style *style_ptr);
static void _wlmaker_task_list_draw_into_cairo(
    cairo_t *cairo_ptr,
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_workspace_t *workspace_ptr);
static void _wlmaker_task_list_draw_tile(
    cairo_t *cairo_ptr,
    wlmtk_window_t *window_ptr,
    bool active,
    int x,
    int y);
static int _wlmaker_task_list_windows(
    wlmtk_workspace_t *workspace_ptr,
    wlmtk_window_t **windows_ptr,
    int max_windows,
    int *active_index_ptr);

static uint32_t _wlmaker_task_list_request_size(
    wlmtk_panel_t *panel_ptr,
    int width,
    int height);

static void _wlmaker_task_list_handle_task_list_enabled(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_task_list_disabled(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_window_mapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_task_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

const bspl_desc_t wlmaker_task_list_style_desc[] = {
    BSPL_DESC_CUSTOM(
        "Fill", true, struct wlmaker_task_list_style, fill, fill,
        wlmtk_style_decode_fill, NULL, NULL, NULL),
    BSPL_DESC_DICT(
        "Font", true, struct wlmaker_task_list_style, font, font,
        wlmtk_style_font_desc),
    BSPL_DESC_ARGB32(
        "TextColor", true, struct wlmaker_task_list_style,
        text_color, text_color, 0),
    BSPL_DESC_SENTINEL()
};

/** Task list positioning: At center of layer. Sized to the windows shown. */
static wlmtk_panel_positioning_t _wlmaker_task_list_positioning = {
    .desired_width = 400,
    .desired_height = 200,
    .anchor = WLR_EDGE_BOTTOM | WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
};

/** Size of a window's tile in the switcher (Window Maker's switch panel). */
static const int _wlmaker_task_list_tile = 72;
/** Size of the app icon within the tile. */
static const int _wlmaker_task_list_icon = 52;
/** Margin around the tiles, and height of the title line. */
static const int _wlmaker_task_list_margin = 12;
/** Height of the title line below the tiles. */
static const int _wlmaker_task_list_title_height = 28;
/** Maximum number of tiles shown; the active one stays visible. */
static const int _wlmaker_task_list_max_tiles = 9;

/** Virtual method table for the task list. */
static const wlmtk_panel_vmt_t _wlmaker_task_list_vmt = {
    .request_size = _wlmaker_task_list_request_size
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_task_list_t *wlmaker_task_list_create(
    wlmaker_server_t *server_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    wlmaker_task_list_t *task_list_ptr = logged_calloc(
        1, sizeof(wlmaker_task_list_t));
    task_list_ptr->server_ptr = server_ptr;
    task_list_ptr->style = *style_ptr;

    if (!wlmtk_panel_init(&task_list_ptr->super_panel,
                          &_wlmaker_task_list_positioning)) {
        wlmaker_task_list_destroy(task_list_ptr);
        return NULL;
    }
    wlmtk_panel_extend(&task_list_ptr->super_panel,
                       &_wlmaker_task_list_vmt);
    wlmtk_element_set_visible(
        wlmtk_panel_element(&task_list_ptr->super_panel), true);

    if (!wlmtk_buffer_init(&task_list_ptr->buffer)) {
        wlmaker_task_list_destroy(task_list_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_buffer_element(&task_list_ptr->buffer), true);
    wlmtk_container_add_element(
        &task_list_ptr->super_panel.super_container,
        wlmtk_buffer_element(&task_list_ptr->buffer));

    wlmtk_util_connect_listener_signal(
        &server_ptr->task_list_enabled_event,
        &task_list_ptr->task_list_enabled_listener,
        _wlmaker_task_list_handle_task_list_enabled);
    wlmtk_util_connect_listener_signal(
        &server_ptr->task_list_disabled_event,
        &task_list_ptr->task_list_disabled_listener,
        _wlmaker_task_list_handle_task_list_disabled);

    wlmtk_util_connect_listener_signal(
        &server_ptr->theme_changed_event,
        &task_list_ptr->theme_changed_listener,
        _wlmaker_task_list_handle_theme_changed);

    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->window_mapped,
        &task_list_ptr->window_mapped_listener,
        _wlmaker_task_list_handle_window_mapped);
    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->window_unmapped,
        &task_list_ptr->window_unmapped_listener,
        _wlmaker_task_list_handle_window_unmapped);

    return task_list_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_task_list_destroy(wlmaker_task_list_t *task_list_ptr)
{
    wlmtk_util_disconnect_listener(&task_list_ptr->window_unmapped_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->window_mapped_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->theme_changed_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->task_list_disabled_listener);
    wlmtk_util_disconnect_listener(&task_list_ptr->task_list_enabled_listener);

    if (wlmtk_buffer_element(&task_list_ptr->buffer)->parent_container_ptr) {
        wlmtk_container_remove_element(
            &task_list_ptr->super_panel.super_container,
            wlmtk_buffer_element(&task_list_ptr->buffer));
    }
    wlmtk_buffer_fini(&task_list_ptr->buffer);
    wlmtk_panel_fini(&task_list_ptr->super_panel);

    free(task_list_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * Refreshes the task list. Should be done whenever a list is mapped/unmapped.
 *
 * @param task_list_ptr
 * @param style_ptr
 *
 * @return true on success.
 */
bool _wlmaker_task_list_refresh(
    wlmaker_task_list_t *task_list_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    wlmtk_workspace_t *workspace_ptr =
        wlmtk_root_get_current_workspace(task_list_ptr->server_ptr->root_ptr);

    struct wlr_buffer *wlr_buffer_ptr = create_wlr_buffer(
        workspace_ptr, style_ptr);
    if (NULL == wlr_buffer_ptr) return false;

    wlmtk_buffer_set(&task_list_ptr->buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Creates a `struct wlr_buffer` with windows of `workspace_ptr` drawn into.
 *
 * @param workspace_ptr
 * @param style_ptr
 *
 * @return A pointer to the `struct wlr_buffer` with the list of windows
 *     (tasks), or NULL on error.
 */
struct wlr_buffer *create_wlr_buffer(
    wlmtk_workspace_t *workspace_ptr,
    const struct wlmaker_task_list_style *style_ptr)
{
    int tiles = 1;
    if (NULL != workspace_ptr) {
        wlmtk_window_t *windows[_wlmaker_task_list_max_tiles];
        int active;
        tiles = BS_MAX(1, _wlmaker_task_list_windows(
                           workspace_ptr, windows,
                           _wlmaker_task_list_max_tiles, &active));
    }
    _wlmaker_task_list_positioning.desired_width =
        tiles * _wlmaker_task_list_tile + 2 * _wlmaker_task_list_margin;
    _wlmaker_task_list_positioning.desired_height =
        _wlmaker_task_list_tile + 2 * _wlmaker_task_list_margin +
        _wlmaker_task_list_title_height;

    struct wlr_buffer *wlr_buffer_ptr = bs_gfxbuf_create_wlr_buffer(
        _wlmaker_task_list_positioning.desired_width,
        _wlmaker_task_list_positioning.desired_height);
    if (NULL == wlr_buffer_ptr) return NULL;

    cairo_t *cairo_ptr = cairo_create_from_wlr_buffer(wlr_buffer_ptr);
    if (NULL == cairo_ptr) {
        wlr_buffer_drop(wlr_buffer_ptr);
        return NULL;
    }
    _wlmaker_task_list_draw_into_cairo(cairo_ptr, style_ptr, workspace_ptr);
    cairo_destroy(cairo_ptr);

    return wlr_buffer_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Collects up to `max_windows` non-minimized windows of the workspace, in
 * switching order, keeping the active window within the selection.
 *
 * @return Number of windows stored in `windows_ptr`.
 */
int _wlmaker_task_list_windows(
    wlmtk_workspace_t *workspace_ptr,
    wlmtk_window_t **windows_ptr,
    int max_windows,
    int *active_index_ptr)
{
    wlmtk_window_t *active_ptr =
        wlmtk_workspace_get_activated_window(workspace_ptr);
    *active_index_ptr = -1;

    // All candidates first, to find where the active window is.
    int total = 0, active_pos = 0;
    for (bs_dllist_node_t *dlnode_ptr =
             wlmtk_workspace_get_windows_dllist(workspace_ptr)->head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
        if (wlmtk_window_is_minimized(window_ptr) ||
            !wlmtk_window_is_iconifiable(window_ptr)) continue;
        if (window_ptr == active_ptr) active_pos = total;
        ++total;
    }
    int first = 0;
    if (total > max_windows) {
        first = BS_MIN(BS_MAX(0, active_pos - max_windows / 2),
                       total - max_windows);
    }

    int n = 0, i = 0;
    for (bs_dllist_node_t *dlnode_ptr =
             wlmtk_workspace_get_windows_dllist(workspace_ptr)->head_ptr;
         NULL != dlnode_ptr && n < max_windows;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
        if (wlmtk_window_is_minimized(window_ptr) ||
            !wlmtk_window_is_iconifiable(window_ptr)) continue;
        if (i++ < first) continue;
        if (window_ptr == active_ptr) *active_index_ptr = n;
        windows_ptr[n++] = window_ptr;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/**
 * Draws the switcher: A row of window tiles with their application icons,
 * the active one highlighted, and its title below. As Window Maker's
 * switch panel.
 *
 * @param cairo_ptr
 * @param style_ptr
 * @param workspace_ptr
 */
void _wlmaker_task_list_draw_into_cairo(
    cairo_t *cairo_ptr,
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_workspace_t *workspace_ptr)
{
    wlmaker_primitives_cairo_fill(cairo_ptr, &style_ptr->fill);
    if (NULL == workspace_ptr) return;

    wlmtk_window_t *windows[_wlmaker_task_list_max_tiles];
    int active = -1;
    int n = _wlmaker_task_list_windows(
        workspace_ptr, windows, _wlmaker_task_list_max_tiles, &active);
    for (int i = 0; i < n; ++i) {
        _wlmaker_task_list_draw_tile(
            cairo_ptr, windows[i], i == active,
            _wlmaker_task_list_margin + i * _wlmaker_task_list_tile,
            _wlmaker_task_list_margin);
    }
    if (0 > active) return;

    // Title of the active window, centered and shortened to fit.
    char title[256];
    const char *title_ptr = wlmtk_window_get_title(windows[active]);
    snprintf(title, sizeof(title), "%s", NULL != title_ptr ? title_ptr : "");
    cairo_set_source_argb8888(cairo_ptr, style_ptr->text_color);
    cairo_select_font_face(cairo_ptr, style_ptr->font.face,
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cairo_ptr, style_ptr->font.size);
    int width = _wlmaker_task_list_positioning.desired_width -
        2 * _wlmaker_task_list_margin;
    cairo_text_extents_t extents;
    cairo_text_extents(cairo_ptr, title, &extents);
    // Too wide: Drop characters (whole UTF-8 sequences) and append "..",
    // until it fits. `full` keeps the original; `title` holds the attempt.
    char full[sizeof(title)];
    memcpy(full, title, sizeof(full));
    for (size_t n = strlen(full); extents.x_advance > width && 0 < n; ) {
        do { --n; } while (0 < n && (full[n] & 0xc0) == 0x80);
        snprintf(title, sizeof(title), "%.*s..", (int)n, full);
        cairo_text_extents(cairo_ptr, title, &extents);
    }
    cairo_move_to(
        cairo_ptr,
        (_wlmaker_task_list_positioning.desired_width - extents.x_advance) / 2,
        _wlmaker_task_list_margin + _wlmaker_task_list_tile +
        _wlmaker_task_list_title_height / 2 + extents.height / 2);
    cairo_show_text(cairo_ptr, title);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws one window's tile: Bevelled square with the application's icon (or a
 * stylized window), raised for the active window.
 */
void _wlmaker_task_list_draw_tile(
    cairo_t *cairo_ptr,
    wlmtk_window_t *window_ptr,
    bool active,
    int x,
    int y)
{
    const int t = _wlmaker_task_list_tile;
    const int pad = 4;

    if (active) {
        // Raised, lighter tile with a bevel, as a selected dock tile.
        cairo_set_source_argb8888(cairo_ptr, 0xffd8d8d8);
        cairo_rectangle(cairo_ptr, x + pad, y + pad, t - 2 * pad, t - 2 * pad);
        cairo_fill(cairo_ptr);
        wlmaker_primitives_draw_bezel_at(
            cairo_ptr, x + pad, y + pad, t - 2 * pad, t - 2 * pad, 2.0, true);
    }

    int icon = _wlmaker_task_list_icon;
    char *path_ptr = wlmaker_app_icon_find(wlmtk_window_get_app_id(window_ptr));
    cairo_surface_t *icon_ptr = NULL;
    if (NULL != path_ptr) {
        icon_ptr = wlmaker_app_icon_load(path_ptr, icon);
        free(path_ptr);
    }
    int ix = x + (t - icon) / 2, iy = y + (t - icon) / 2;
    if (NULL != icon_ptr) {
        cairo_set_source_surface(cairo_ptr, icon_ptr, ix, iy);
        cairo_paint_with_alpha(cairo_ptr, active ? 1.0 : 0.75);
        cairo_surface_destroy(icon_ptr);
        return;
    }

    // No icon known: A stylized window.
    cairo_set_source_argb8888(cairo_ptr, 0xff000000);
    cairo_rectangle(cairo_ptr, ix + 3, iy + 7, icon - 6, icon - 14);
    cairo_fill(cairo_ptr);
    cairo_set_source_argb8888(cairo_ptr, 0xffdadada);
    cairo_rectangle(cairo_ptr, ix + 4, iy + 15, icon - 8, icon - 23);
    cairo_fill(cairo_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Implements @ref wlmtk_panel_vmt_t::request_size.
 *
 * @param panel_ptr
 * @param width
 * @param height
 *
 * @return 0 always.
 */
uint32_t _wlmaker_task_list_request_size(
    wlmtk_panel_t *panel_ptr,
    __UNUSED__ int width,
    __UNUSED__ int height)
{
    wlmtk_panel_commit(panel_ptr, 0, &_wlmaker_task_list_positioning);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `task_list_enabled_listener`.
 *
 * Enables the task listener: Creates the task list for the currently-active
 * workspace and enables the task list on that workspace.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_task_list_enabled(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, task_list_enabled_listener);

    _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);

    if (task_list_ptr->enabled) {
        BS_ASSERT(NULL != wlmtk_panel_get_layer(&task_list_ptr->super_panel));
        return;
    }

    wlmtk_workspace_t *workspace_ptr =
        wlmtk_root_get_current_workspace(task_list_ptr->server_ptr->root_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_workspace_get_layer(
        workspace_ptr, WLMTK_WORKSPACE_LAYER_OVERLAY);
    wlmtk_layer_add_panel(
        layer_ptr,
        &task_list_ptr->super_panel,
        wlmaker_server_get_output_at_cursor(task_list_ptr->server_ptr));
    task_list_ptr->enabled = true;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `task_list_disabled_listener`: Hides the list.
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_task_list_disabled(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, task_list_disabled_listener);

    BS_ASSERT(NULL != wlmtk_panel_get_layer(&task_list_ptr->super_panel));
    wlmtk_layer_remove_panel(
        wlmtk_panel_get_layer(&task_list_ptr->super_panel),
        &task_list_ptr->super_panel);
    task_list_ptr->enabled = false;
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `theme_changed_listener`: Updates the style
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, theme_changed_listener);
    wlmaker_config_style_t *style_ptr = data_ptr;

    if (_wlmaker_task_list_refresh(task_list_ptr, &style_ptr->task_list)) {
        task_list_ptr->style = style_ptr->task_list;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `window_mapped_listener`: Refreshes the list (if enabled).
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_window_mapped(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, window_mapped_listener);
    if (task_list_ptr->enabled) {
        _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `window_unmapped_listener`: Refreshes the list (if enabled).
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_task_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_task_list_t *task_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_task_list_t, window_unmapped_listener);
    if (task_list_ptr->enabled) {
        _wlmaker_task_list_refresh(task_list_ptr, &task_list_ptr->style);
    }
}

/* == End of task_list.c =================================================== */
