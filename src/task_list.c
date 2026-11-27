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
#include <drm_fourcc.h>
#include <wlr/util/edges.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#undef WLR_USE_UNSTABLE

#include "app_icon.h"
#include "backend/backend.h"
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
    /** Window thumbnails, atop @ref wlmaker_task_list_t::buffer. */
    wlmtk_buffer_t            thumbs[9];
    /** Whether the respective @ref wlmaker_task_list_t::thumbs is set up. */
    bool                      thumbs_initialized[9];
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
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_window_t *window_ptr,
    bool active,
    int x,
    int y);
static void _wlmaker_task_list_tile_position(int index, int *x_ptr, int *y_ptr);
static bool _wlmaker_task_list_want_thumbnails(wlmaker_server_t *server_ptr);
static struct wlr_buffer *_wlmaker_task_list_render_thumbnail(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *window_ptr,
    int max_width,
    int max_height);
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

/** With thumbnails: Tile size, and the thumbnail's area within. */
static const int _wlmaker_task_list_thumb_tile_w = 224;
/** With thumbnails: Tile height (thumbnail, then icon & name). */
static const int _wlmaker_task_list_thumb_tile_h = 172;
/** With thumbnails: Size of the thumbnail area. */
static const int _wlmaker_task_list_thumb_w = 208;
/** With thumbnails: Size of the thumbnail area. */
static const int _wlmaker_task_list_thumb_h = 130;
/** With thumbnails: Icon size, below the thumbnail. */
static const int _wlmaker_task_list_thumb_icon = 24;

/** Current layout: Tile size, columns, and whether showing thumbnails. */
static int _wlmaker_task_list_tile_w = 72;
/** Current layout: Tile height. */
static int _wlmaker_task_list_tile_h = 72;
/** Current layout: Tiles per row. */
static int _wlmaker_task_list_cols = 9;
/** Current layout: Whether tiles show window thumbnails. */
static bool _wlmaker_task_list_thumbnails = false;

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

    // Still shown (eg. quitting while Alt+Tab is held): Leave the layer.
    if (NULL != wlmtk_panel_get_layer(&task_list_ptr->super_panel)) {
        wlmtk_layer_remove_panel(
            wlmtk_panel_get_layer(&task_list_ptr->super_panel),
            &task_list_ptr->super_panel);
    }
    for (int i = 0; i < _wlmaker_task_list_max_tiles; ++i) {
        if (!task_list_ptr->thumbs_initialized[i]) continue;
        wlmtk_container_remove_element(
            &task_list_ptr->super_panel.super_container,
            wlmtk_buffer_element(&task_list_ptr->thumbs[i]));
        wlmtk_buffer_fini(&task_list_ptr->thumbs[i]);
        task_list_ptr->thumbs_initialized[i] = false;
    }
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
    wlmaker_server_t *server_ptr = task_list_ptr->server_ptr;
    wlmtk_workspace_t *workspace_ptr =
        wlmtk_root_get_current_workspace(server_ptr->root_ptr);

    // Layout: Thumbnails if so configured, in as many columns as fit.
    _wlmaker_task_list_thumbnails = _wlmaker_task_list_want_thumbnails(
        server_ptr);
    _wlmaker_task_list_tile_w = _wlmaker_task_list_thumbnails ?
        _wlmaker_task_list_thumb_tile_w : _wlmaker_task_list_tile;
    _wlmaker_task_list_tile_h = _wlmaker_task_list_thumbnails ?
        _wlmaker_task_list_thumb_tile_h : _wlmaker_task_list_tile;
    _wlmaker_task_list_cols = _wlmaker_task_list_max_tiles;
    struct wlr_output *wlr_output_ptr =
        wlmaker_server_get_output_at_cursor(server_ptr);
    if (NULL != wlr_output_ptr) {
        struct wlr_box o;
        wlr_output_layout_get_box(server_ptr->wlr_output_layout_ptr,
                                  wlr_output_ptr, &o);
        _wlmaker_task_list_cols = BS_MAX(1, BS_MIN(
            _wlmaker_task_list_max_tiles,
            (o.width - 2 * _wlmaker_task_list_margin - 40) /
            _wlmaker_task_list_tile_w));
    }

    struct wlr_buffer *wlr_buffer_ptr = create_wlr_buffer(
        workspace_ptr, style_ptr);
    if (NULL == wlr_buffer_ptr) return false;

    wlmtk_buffer_set(&task_list_ptr->buffer, wlr_buffer_ptr);
    wlr_buffer_drop(wlr_buffer_ptr);
    // The size changed with the windows shown: Re-center the panel.
    wlmtk_panel_commit(&task_list_ptr->super_panel, 0,
                       &_wlmaker_task_list_positioning);

    // Thumbnails: One buffer element per window, atop its tile.
    wlmtk_window_t *windows[_wlmaker_task_list_max_tiles];
    int active = -1, n = 0;
    if (_wlmaker_task_list_thumbnails && NULL != workspace_ptr) {
        n = _wlmaker_task_list_windows(
            workspace_ptr, windows, _wlmaker_task_list_max_tiles, &active);
    }
    for (int i = 0; i < _wlmaker_task_list_max_tiles; ++i) {
        wlmtk_buffer_t *thumb_ptr = &task_list_ptr->thumbs[i];
        if (i >= n) {
            if (task_list_ptr->thumbs_initialized[i]) {
                wlmtk_buffer_set(thumb_ptr, NULL);
                wlmtk_element_set_visible(wlmtk_buffer_element(thumb_ptr), false);
            }
            continue;
        }
        if (!task_list_ptr->thumbs_initialized[i]) {
            if (!wlmtk_buffer_init(thumb_ptr)) continue;
            task_list_ptr->thumbs_initialized[i] = true;
            wlmtk_container_add_element(
                &task_list_ptr->super_panel.super_container,
                wlmtk_buffer_element(thumb_ptr));
        }
        struct wlr_buffer *tb_ptr = _wlmaker_task_list_render_thumbnail(
            server_ptr, windows[i],
            _wlmaker_task_list_thumb_w, _wlmaker_task_list_thumb_h);
        wlmtk_buffer_set(thumb_ptr, tb_ptr);
        wlmtk_element_set_visible(wlmtk_buffer_element(thumb_ptr),
                                  NULL != tb_ptr);
        if (NULL == tb_ptr) continue;
        int x, y;
        _wlmaker_task_list_tile_position(i, &x, &y);
        int pad = (_wlmaker_task_list_tile_w - _wlmaker_task_list_thumb_w) / 2;
        wlmtk_element_set_position(
            wlmtk_buffer_element(thumb_ptr),
            x + pad + (_wlmaker_task_list_thumb_w - tb_ptr->width) / 2,
            y + pad + (_wlmaker_task_list_thumb_h - tb_ptr->height) / 2);
        wlr_buffer_drop(tb_ptr);
    }
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
    int cols = BS_MIN(tiles, _wlmaker_task_list_cols);
    int rows = (tiles + cols - 1) / cols;
    _wlmaker_task_list_positioning.desired_width =
        cols * _wlmaker_task_list_tile_w + 2 * _wlmaker_task_list_margin;
    _wlmaker_task_list_positioning.desired_height =
        rows * _wlmaker_task_list_tile_h + 2 * _wlmaker_task_list_margin +
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
            !wlmtk_window_is_iconifiable(window_ptr) ||
            (wlmtk_window_get_attributes(window_ptr) &
             (WLMTK_WINDOW_ATTR_SKIP_SWITCHER | WLMTK_WINDOW_ATTR_NO_FOCUS))) continue;
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
            !wlmtk_window_is_iconifiable(window_ptr) ||
            (wlmtk_window_get_attributes(window_ptr) &
             (WLMTK_WINDOW_ATTR_SKIP_SWITCHER | WLMTK_WINDOW_ATTR_NO_FOCUS))) continue;
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
        int x, y;
        _wlmaker_task_list_tile_position(i, &x, &y);
        _wlmaker_task_list_draw_tile(cairo_ptr, style_ptr, windows[i],
                                     i == active, x, y);
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
        _wlmaker_task_list_positioning.desired_height -
        _wlmaker_task_list_margin - _wlmaker_task_list_title_height / 2 +
        extents.height / 2);
    cairo_show_text(cairo_ptr, title);
}

/* ------------------------------------------------------------------------- */
/**
 * Draws one window's tile: Bevelled square with the application's icon (or a
 * stylized window), raised for the active window.
 */
void _wlmaker_task_list_draw_tile(
    cairo_t *cairo_ptr,
    const struct wlmaker_task_list_style *style_ptr,
    wlmtk_window_t *window_ptr,
    bool active,
    int x,
    int y)
{
    const int t = _wlmaker_task_list_tile;
    const int pad = 4;

    if (_wlmaker_task_list_thumbnails) {
        const int w = _wlmaker_task_list_tile_w, h = _wlmaker_task_list_tile_h;
        if (active) {
            cairo_set_source_argb8888(cairo_ptr, 0xffd8d8d8);
            cairo_rectangle(cairo_ptr, x + pad, y + pad, w - 2 * pad, h - 2 * pad);
            cairo_fill(cairo_ptr);
            wlmaker_primitives_draw_bezel_at(
                cairo_ptr, x + pad, y + pad, w - 2 * pad, h - 2 * pad, 2.0, true);
        }
        // Below the thumbnail: The app's icon and the window's title.
        const int icon = _wlmaker_task_list_thumb_icon;
        int iy = y + (w - _wlmaker_task_list_thumb_w) / 2 +
            _wlmaker_task_list_thumb_h + 6;
        int ix = x + (w - _wlmaker_task_list_thumb_w) / 2;
        char *path_ptr = wlmaker_app_icon_find(
            wlmtk_window_get_app_id(window_ptr));
        if (NULL != path_ptr) {
            cairo_surface_t *icon_ptr = wlmaker_app_icon_load(path_ptr, icon);
            free(path_ptr);
            if (NULL != icon_ptr) {
                cairo_set_source_surface(cairo_ptr, icon_ptr, ix, iy);
                cairo_paint(cairo_ptr);
                cairo_surface_destroy(icon_ptr);
            }
        }
        const char *title_ptr = wlmtk_window_get_title(window_ptr);
        cairo_save(cairo_ptr);
        cairo_rectangle(cairo_ptr, ix + icon + 6, iy,
                        _wlmaker_task_list_thumb_w - icon - 6, icon);
        cairo_clip(cairo_ptr);
        cairo_set_source_argb8888(
            cairo_ptr, active ? 0xff000000 : style_ptr->text_color);
        cairo_select_font_face(cairo_ptr, style_ptr->font.face,
                               CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cairo_ptr, 12);
        cairo_move_to(cairo_ptr, ix + icon + 6, iy + icon / 2 + 4);
        cairo_show_text(cairo_ptr, NULL != title_ptr ? title_ptr : "");
        cairo_restore(cairo_ptr);
        return;
    }

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
/** Position of tile `index` in the panel, by the current layout. */
void _wlmaker_task_list_tile_position(int index, int *x_ptr, int *y_ptr)
{
    *x_ptr = _wlmaker_task_list_margin +
        (index % _wlmaker_task_list_cols) * _wlmaker_task_list_tile_w;
    *y_ptr = _wlmaker_task_list_margin +
        (index / _wlmaker_task_list_cols) * _wlmaker_task_list_tile_h;
}

/* ------------------------------------------------------------------------- */
/** Whether the configuration asks for thumbnails: "Switcher" "Thumbnails". */
bool _wlmaker_task_list_want_thumbnails(wlmaker_server_t *server_ptr)
{
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(
        server_ptr->config_dict_ptr, "Switcher");
    if (NULL == dict_ptr) return false;
    struct { bool thumbnails; } c = {};
    const bspl_desc_t desc[] = {
        BSPL_DESC_BOOL("Thumbnails", false, __typeof__(c), thumbnails,
                       thumbnails, false),
        BSPL_DESC_SENTINEL()
    };
    return bspl_decode_dict(dict_ptr, desc, &c) && c.thumbnails;
}

/** Context for @ref _wlmaker_task_list_render_buffer. */
typedef struct {
    /** The renderer. */
    struct wlr_renderer       *wlr_renderer_ptr;
    /** The pass rendering into the thumbnail. */
    struct wlr_render_pass    *pass_ptr;
    /** Origin of the window's bounds, in the window node's coordinates. */
    int                       x0, y0;
    /** Scale factor. */
    double                    scale;
    /** Textures created for this pass; destroyed once it's submitted. */
    struct wlr_texture        *textures[32];
    /** Number of textures in @ref _wlmaker_task_list_thumb_ctx_t::textures. */
    int                       num_textures;
} _wlmaker_task_list_thumb_ctx_t;

/* ------------------------------------------------------------------------- */
/** Renders one of the window's scene buffers into the thumbnail. */
static void _wlmaker_task_list_render_buffer(
    struct wlr_scene_buffer *scene_buffer_ptr,
    int sx,
    int sy,
    void *ud_ptr)
{
    _wlmaker_task_list_thumb_ctx_t *ctx_ptr = ud_ptr;
    struct wlr_buffer *buffer_ptr = scene_buffer_ptr->buffer;
    if (NULL == buffer_ptr) return;

    // Client buffers keep their texture; others get a temporary one.
    struct wlr_texture *texture_ptr = NULL;
    struct wlr_client_buffer *client_buffer_ptr =
        wlr_client_buffer_get(buffer_ptr);
    if (NULL != client_buffer_ptr) texture_ptr = client_buffer_ptr->texture;
    if (NULL == texture_ptr) {
        if (ctx_ptr->num_textures >= 32) return;
        texture_ptr = wlr_texture_from_buffer(ctx_ptr->wlr_renderer_ptr,
                                              buffer_ptr);
        if (NULL == texture_ptr) return;
        ctx_ptr->textures[ctx_ptr->num_textures++] = texture_ptr;
    }

    int w = scene_buffer_ptr->dst_width, h = scene_buffer_ptr->dst_height;
    if (0 >= w || 0 >= h) {
        w = buffer_ptr->width;
        h = buffer_ptr->height;
        if (scene_buffer_ptr->transform & WL_OUTPUT_TRANSFORM_90) {
            int t = w; w = h; h = t;
        }
    }
    struct wlr_box dst = {
        .x = (int)((sx - ctx_ptr->x0) * ctx_ptr->scale),
        .y = (int)((sy - ctx_ptr->y0) * ctx_ptr->scale),
        .width = BS_MAX(1, (int)(w * ctx_ptr->scale + 0.5)),
        .height = BS_MAX(1, (int)(h * ctx_ptr->scale + 0.5)),
    };
    wlr_render_pass_add_texture(
        ctx_ptr->pass_ptr,
        &(struct wlr_render_texture_options){
            .texture = texture_ptr,
            .src_box = scene_buffer_ptr->src_box,
            .dst_box = dst,
            .transform = scene_buffer_ptr->transform,
            .filter_mode = WLR_SCALE_FILTER_BILINEAR,
        });
}

/* ------------------------------------------------------------------------- */
/**
 * Renders a scaled-down picture of the window -- its surfaces and
 * decorations -- fitting `max_width` x `max_height`.
 *
 * @return A buffer, to drop after use, or NULL if not available.
 */
struct wlr_buffer *_wlmaker_task_list_render_thumbnail(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *window_ptr,
    int max_width,
    int max_height)
{
    wlmtk_element_t *element_ptr = wlmtk_window_element(window_ptr);
    struct wlr_scene_node *node_ptr = element_ptr->wlr_scene_node_ptr;
    struct wlr_renderer *renderer_ptr = wlmbe_backend_renderer(
        server_ptr->backend_ptr);
    struct wlr_allocator *allocator_ptr = wlmbe_backend_allocator(
        server_ptr->backend_ptr);
    if (NULL == node_ptr || NULL == renderer_ptr || NULL == allocator_ptr) {
        return NULL;
    }

    struct wlr_box b = wlmtk_element_get_dimensions_box(element_ptr);
    if (0 >= b.width || 0 >= b.height) return NULL;
    double scale = BS_MIN(1.0, BS_MIN((double)max_width / b.width,
                                      (double)max_height / b.height));
    int w = BS_MAX(1, (int)(b.width * scale + 0.5));
    int h = BS_MAX(1, (int)(b.height * scale + 0.5));

    // ARGB8888 with the implicit modifier: Renderable for GBM, and accepted
    // by the shm allocator (pixman) as well.
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    const struct wlr_drm_format format = {
        .format = DRM_FORMAT_ARGB8888, .len = 1, .capacity = 1,
        .modifiers = &modifier };
    struct wlr_buffer *buffer_ptr = wlr_allocator_create_buffer(
        allocator_ptr, w, h, &format);
    if (NULL == buffer_ptr) return NULL;

    struct wlr_render_pass *pass_ptr = wlr_renderer_begin_buffer_pass(
        renderer_ptr, buffer_ptr, NULL);
    if (NULL == pass_ptr) {
        wlr_buffer_drop(buffer_ptr);
        return NULL;
    }
    wlr_render_pass_add_rect(
        pass_ptr, &(struct wlr_render_rect_options){
            .box = { .width = w, .height = h },
            .color = { .r = 0, .g = 0, .b = 0, .a = 0 },
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        });
    // The iterator's coordinates include the window node's own position.
    _wlmaker_task_list_thumb_ctx_t ctx = {
        .wlr_renderer_ptr = renderer_ptr,
        .pass_ptr = pass_ptr,
        .x0 = node_ptr->x + b.x,
        .y0 = node_ptr->y + b.y,
        .scale = scale,
    };
    wlr_scene_node_for_each_buffer(node_ptr, _wlmaker_task_list_render_buffer,
                                   &ctx);
    bool ok = wlr_render_pass_submit(pass_ptr);
    for (int i = 0; i < ctx.num_textures; ++i) {
        wlr_texture_destroy(ctx.textures[i]);
    }
    if (!ok) {
        wlr_buffer_drop(buffer_ptr);
        return NULL;
    }
    return buffer_ptr;
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
