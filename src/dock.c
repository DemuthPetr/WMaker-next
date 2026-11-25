/* ========================================================================= */
/**
 * @file dock.c
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

#include "dock.h"

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "backend/backend.h"
#include "backend/output_config.h"
#include "config.h"
#include "default_state.h"
#include "files.h"
#include "launcher.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** Dock handle. */
struct _wlmaker_dock_t {
    /** Toolkit dock. */
    wlmtk_dock_t              *wlmtk_dock_ptr;

    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;

    /** Description of the desired output, if any. */
    wlmbe_output_description_t output_description;

    /** Listener for @ref wlmtk_root_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;
    /** Listener for wlr_output_layout::events.change. */
    struct wl_listener        output_layout_change_listener;
    /** Listener for @ref wlmaker_server_t::theme_changed_event. */
    struct wl_listener        theme_changed_listener;

    /** The state, to save the dock's launcher order into. */
    bspl_dict_t               *state_dict_ptr;
    /** Launchers, in dock order. */
    wlmaker_launcher_t        **launchers_ptr;
    /** Number of launchers. */
    size_t                    launchers;
    /** Tile size from the state's `TileSize`, or 0 for the theme's. */
    uint64_t                  tile_size;
};

static bool _wlmaker_dock_decode_launchers(
    bspl_object_t *object_ptr,
    const union bspl_desc_value *desc_value_ptr,
    void *value_ptr);

static void _wlmaker_dock_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dock_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_dock_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static struct wlmtk_tile_style _wlmaker_dock_tile_style(
    wlmaker_dock_t *dock_ptr,
    const struct wlmtk_tile_style *theme_tile_style_ptr);
static void _wlmaker_dock_handle_drop(
    void *ud_ptr,
    wlmaker_launcher_t *launcher_ptr,
    double x,
    double y);
static bool _wlmaker_dock_save(wlmaker_dock_t *dock_ptr);

/* == Data ================================================================= */

/** TODO: Replace this. */
typedef struct {
    /** Positioning data. */
    wlmtk_dock_positioning_t  positioning;
    /** Launchers. */
    bspl_array_t            *launchers_array_ptr;
    /** Number of lines (rows or columns) of tiles. */
    int64_t                   lines;
    /** Order of the lines: Snake (1) or Straight (0). */
    int                       order;
    /** Sorting: Manual (0, as listed) or by date Added (1). */
    int                       sort;
    /** Tile size override, 0 for the theme's. */
    uint64_t                  tile_size;
} parse_args;

/** Enum descriptor for the order of lines. */
static const bspl_enum_desc_t _wlmaker_dock_orders[] = {
    BSPL_ENUM("Straight", 0),
    BSPL_ENUM("Snake", 1),
    BSPL_ENUM_SENTINEL(),
};

/** Enum descriptor for sorting the launchers. */
static const bspl_enum_desc_t _wlmaker_dock_sorts[] = {
    BSPL_ENUM("Manual", 0),
    BSPL_ENUM("Added", 1),
    BSPL_ENUM_SENTINEL(),
};

/** Enum descriptor for `enum wlr_edges`. */
static const bspl_enum_desc_t _wlmaker_dock_edges[] = {
    BSPL_ENUM("TOP", WLR_EDGE_TOP),
    BSPL_ENUM("BOTTOM", WLR_EDGE_BOTTOM),
    BSPL_ENUM("LEFT", WLR_EDGE_LEFT),
    BSPL_ENUM("RIGHT", WLR_EDGE_RIGHT),
    BSPL_ENUM_SENTINEL(),
};

/** Descriptor for the dock's plist. */
const bspl_desc_t _wlmaker_dock_desc[] = {
    BSPL_DESC_ENUM("Edge", true, parse_args,
                   positioning.edge, positioning.edge,
                   WLR_EDGE_NONE, _wlmaker_dock_edges),
    BSPL_DESC_ENUM("Anchor", true, parse_args,
                   positioning.anchor, positioning.anchor,
                   WLR_EDGE_NONE, _wlmaker_dock_edges),
    BSPL_DESC_CUSTOM("Launchers", true, parse_args, launchers_array_ptr,
                     launchers_array_ptr,
                     _wlmaker_dock_decode_launchers,
                     NULL,
                     NULL,
                     NULL),
    BSPL_DESC_INT64("Lines", false, parse_args, lines, lines, 1),
    BSPL_DESC_ENUM("Order", false, parse_args, order, order, 1,
                   _wlmaker_dock_orders),
    BSPL_DESC_ENUM("Sort", false, parse_args, sort, sort, 0,
                   _wlmaker_dock_sorts),
    BSPL_DESC_UINT64("TileSize", false, parse_args, tile_size, tile_size, 0),
    BSPL_DESC_SENTINEL(),
};

/** Descriptor for a pane's layout, see @ref wlmaker_pane_layout_decode. */
static const bspl_desc_t _wlmaker_pane_layout_desc[] = {
    BSPL_DESC_INT64("Lines", false, parse_args, lines, lines, 1),
    BSPL_DESC_ENUM("Order", false, parse_args, order, order, 1,
                   _wlmaker_dock_orders),
    BSPL_DESC_UINT64("TileSize", false, parse_args, tile_size, tile_size, 0),
    BSPL_DESC_SENTINEL(),
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlmaker_pane_layout_decode(
    bspl_dict_t *dict_ptr,
    wlmaker_pane_layout_t *layout_ptr)
{
    parse_args args = { .lines = 1, .order = 1 };
    if (NULL != dict_ptr &&
        !bspl_decode_dict(dict_ptr, _wlmaker_pane_layout_desc, &args)) {
        bs_log(BS_WARNING, "Invalid Lines, Order or TileSize, using defaults.");
        args = (parse_args){ .lines = 1, .order = 1 };
    }
    layout_ptr->lines = BS_MIN(BS_MAX(args.lines, 1), WLMTK_DOCK_MAX_LINES);
    layout_ptr->snake = 1 == args.order;
    layout_ptr->tile_size = args.tile_size;
}

/* ------------------------------------------------------------------------- */
struct wlmtk_tile_style wlmaker_pane_tile_style(
    uint64_t tile_size,
    const struct wlmtk_tile_style *theme_tile_style_ptr)
{
    struct wlmtk_tile_style style = *theme_tile_style_ptr;
    if (0 < tile_size && 0 < style.size) {
        uint64_t size = BS_MIN(BS_MAX(tile_size, 24u), 256u);
        style.content_size = style.content_size * size / style.size;
        style.size = size;
    }
    return style;
}

/* ------------------------------------------------------------------------- */
wlmaker_dock_t *wlmaker_dock_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr,
    const wlmaker_config_style_t *style_ptr)
{
    wlmaker_dock_t *dock_ptr = logged_calloc(1, sizeof(wlmaker_dock_t));
    if (NULL == dock_ptr) return NULL;
    dock_ptr->server_ptr = server_ptr;
    dock_ptr->state_dict_ptr = bspl_dict_ref(state_dict_ptr);

    parse_args args = {};
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(state_dict_ptr, "Dock");
    if (NULL == dict_ptr) {
        bs_log(BS_ERROR, "No 'Dock' dict found in configuration.");
        wlmaker_dock_destroy(dock_ptr);
        return NULL;
    }
    bspl_decode_dict(dict_ptr, _wlmaker_dock_desc, &args);
    bspl_dict_t *output_dict_ptr = bspl_dict_get_dict(dict_ptr, "Output");
    if (NULL != output_dict_ptr) {
        if (!wlmbe_output_description_init_from_plist(
                &dock_ptr->output_description, output_dict_ptr)) {
            wlmaker_dock_destroy(dock_ptr);
            return NULL;
        }
    }

    dock_ptr->wlmtk_dock_ptr = wlmtk_dock_create(
        &args.positioning, &style_ptr->dock);
    if (NULL == dock_ptr->wlmtk_dock_ptr) {
        wlmaker_dock_destroy(dock_ptr);
        return NULL;
    }
    wlmtk_element_set_visible(
        wlmtk_dock_element(dock_ptr->wlmtk_dock_ptr),
        true);

    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &dock_ptr->output_description, server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(
            server_ptr->wlr_output_layout_ptr);
    }

    wlmtk_workspace_t *workspace_ptr =
        wlmtk_root_get_current_workspace(server_ptr->root_ptr);
    wlmtk_layer_t *layer_ptr = wlmtk_workspace_get_layer(
        workspace_ptr, WLMTK_WORKSPACE_LAYER_TOP);
    if (!wlmtk_layer_add_panel(
            layer_ptr,
            wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr),
            wlr_output_ptr)) {
        wlmaker_dock_destroy(dock_ptr);
        return NULL;
    }

    dock_ptr->tile_size = args.tile_size;
    struct wlmtk_tile_style tile_style = _wlmaker_dock_tile_style(
        dock_ptr, &style_ptr->tile);

    // Launcher definitions in dock order: As listed, or by date added.
    size_t n = bspl_array_size(args.launchers_array_ptr);
    bspl_dict_t **dicts_ptr = logged_calloc(n > 0 ? n : 1, sizeof(bspl_dict_t*));
    dock_ptr->launchers_ptr = logged_calloc(
        n > 0 ? n : 1, sizeof(wlmaker_launcher_t*));
    if (NULL == dicts_ptr || NULL == dock_ptr->launchers_ptr) {
        free(dicts_ptr);
        wlmaker_dock_destroy(dock_ptr);
        return NULL;
    }
    for (size_t i = 0; i < n; ++i) {
        dicts_ptr[i] = bspl_dict_from_object(
            bspl_array_at(args.launchers_array_ptr, i));
        if (NULL == dicts_ptr[i]) {
            bs_log(BS_ERROR, "Elements of 'Launchers' must be dicts.");
            free(dicts_ptr);
            wlmaker_dock_destroy(dock_ptr);
            return NULL;
        }
    }
    if (1 == args.sort) {
        // Stable insertion sort by "Added" (seconds since epoch); missing: 0.
        for (size_t i = 1; i < n; ++i) {
            bspl_dict_t *d = dicts_ptr[i];
            const char *a = bspl_dict_get_string_value(d, "Added");
            long long added = NULL != a ? atoll(a) : 0;
            size_t j = i;
            for (; j > 0; --j) {
                const char *b = bspl_dict_get_string_value(
                    dicts_ptr[j - 1], "Added");
                if ((NULL != b ? atoll(b) : 0) <= added) break;
                dicts_ptr[j] = dicts_ptr[j - 1];
            }
            dicts_ptr[j] = d;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        wlmaker_launcher_t *launcher_ptr = wlmaker_launcher_create_from_plist(
            &tile_style, dicts_ptr[i],
            server_ptr->monitor_ptr,
            server_ptr->files_ptr);
        if (NULL == launcher_ptr) {
            free(dicts_ptr);
            wlmaker_dock_destroy(dock_ptr);
            return NULL;
        }
        wlmaker_launcher_set_server(launcher_ptr, server_ptr);
        wlmaker_launcher_set_drop_handler(
            launcher_ptr, _wlmaker_dock_handle_drop, dock_ptr);
        dock_ptr->launchers_ptr[dock_ptr->launchers++] = launcher_ptr;
        wlmtk_dock_add_tile(
            dock_ptr->wlmtk_dock_ptr,
            wlmaker_launcher_tile(launcher_ptr));
    }
    free(dicts_ptr);
    wlmtk_dock_set_lines(dock_ptr->wlmtk_dock_ptr, args.lines, 1 == args.order);

    // FIXME: This is leaky.
    if (NULL != args.launchers_array_ptr) {
        bspl_array_unref(args.launchers_array_ptr);
        args.launchers_array_ptr = NULL;
    }

    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->workspace_changed,
        &dock_ptr->workspace_changed_listener,
        _wlmaker_dock_handle_workspace_changed);
    wlmtk_util_connect_listener_signal(
        &server_ptr->theme_changed_event,
        &dock_ptr->theme_changed_listener,
        _wlmaker_dock_handle_theme_changed);

    // TODO(kaeser@gubbe.ch): This is a very hacky way of updating the output
    // before the layer's handler removes all associated panels. Should be
    // a native method of wlmtk_dock_t or wlmtk_panel_t.
    dock_ptr->output_layout_change_listener.notify =
        _wlmaker_dock_handle_output_layout_change;
    wl_list_insert(
        server_ptr->wlr_output_layout_ptr->events.change.listener_list.next,
        &dock_ptr->output_layout_change_listener.link);

    bs_log(BS_INFO, "Created dock %p", dock_ptr);
    return dock_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_dock_destroy(wlmaker_dock_t *dock_ptr)
{
    wlmtk_util_disconnect_listener(&dock_ptr->output_layout_change_listener);
    wlmtk_util_disconnect_listener(&dock_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(&dock_ptr->theme_changed_listener);

    if (NULL != dock_ptr->wlmtk_dock_ptr) {
        if (NULL != wlmtk_panel_get_layer(
                wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr))) {
            wlmtk_layer_remove_panel(
                wlmtk_panel_get_layer(wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr)),
                wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr));
        }

        wlmtk_dock_destroy(dock_ptr->wlmtk_dock_ptr);
        dock_ptr->wlmtk_dock_ptr = NULL;
    }

    wlmbe_output_description_fini(&dock_ptr->output_description);
    free(dock_ptr->launchers_ptr);
    if (NULL != dock_ptr->state_dict_ptr) bspl_dict_unref(dock_ptr->state_dict_ptr);
    free(dock_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Decoder for the "Launchers" array. Currently just stores a reference. */
bool _wlmaker_dock_decode_launchers(
    bspl_object_t *object_ptr,
    __UNUSED__ const union bspl_desc_value *desc_value_ptr,
    void *value_ptr)
{
    bspl_array_t **array_ptr_ptr = value_ptr;

    *array_ptr_ptr = bspl_array_from_object(object_ptr);
    if (NULL == *array_ptr_ptr) return false;

    bspl_object_ref(bspl_object_from_array(*array_ptr_ptr));
    return true;
}

/* ------------------------------------------------------------------------- */
/** Re-attaches the dock to the new "current" workspace. */
void _wlmaker_dock_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dock_t, workspace_changed_listener);
    wlmtk_panel_t *panel_ptr = wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr);

    wlmtk_layer_t *current_layer_ptr = wlmtk_panel_get_layer(panel_ptr);
    wlmtk_workspace_t *workspace_ptr =
        wlmtk_root_get_current_workspace(dock_ptr->server_ptr->root_ptr);
    wlmtk_layer_t *new_layer_ptr = wlmtk_workspace_get_layer(
        workspace_ptr, WLMTK_WORKSPACE_LAYER_TOP);

    if (current_layer_ptr == new_layer_ptr) return;

    if (NULL != current_layer_ptr) {
        wlmtk_layer_remove_panel(current_layer_ptr, panel_ptr);
    }
    BS_ASSERT(wlmtk_layer_add_panel(
                  new_layer_ptr,
                  panel_ptr,
                  wlmbe_primary_output(
                      dock_ptr->server_ptr->wlr_output_layout_ptr)));
}

/* ------------------------------------------------------------------------- */
/** Handles when output layout changes; Re-computes the output to attach. */
void _wlmaker_dock_handle_output_layout_change(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_dock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dock_t, output_layout_change_listener);

    struct wlr_output *wlr_output_ptr = wlmbe_output_description_first_fnmatch(
        &dock_ptr->output_description, dock_ptr->server_ptr->wlr_output_layout_ptr);
    if (NULL == wlr_output_ptr) {
        wlr_output_ptr = wlmbe_primary_output(
            dock_ptr->server_ptr->wlr_output_layout_ptr);
    }
    wlmtk_layer_t *layer_ptr = wlmtk_panel_get_layer(
        wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr));
    wlmtk_layer_remove_panel(layer_ptr, wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr));
    if (NULL != wlr_output_ptr) {
        BS_ASSERT(wlmtk_layer_add_panel(
                      layer_ptr,
                      wlmtk_dock_panel(dock_ptr->wlmtk_dock_ptr),
                      wlr_output_ptr));
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Handler for the `theme_changed_listener`: Updates the style
 *
 * @param listener_ptr
 * @param data_ptr
 */
void _wlmaker_dock_handle_theme_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
   wlmaker_dock_t *dock_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_dock_t, theme_changed_listener);
   wlmaker_config_style_t *style_ptr = data_ptr;

   struct wlmtk_tile_style tile_style = _wlmaker_dock_tile_style(
       dock_ptr, &style_ptr->tile);
   wlmtk_dock_set_style(
       dock_ptr->wlmtk_dock_ptr,
       &style_ptr->dock,
       &tile_style);
}

/* ------------------------------------------------------------------------- */
/** @return The theme's tile style, scaled to the dock's `TileSize`. */
struct wlmtk_tile_style _wlmaker_dock_tile_style(
    wlmaker_dock_t *dock_ptr,
    const struct wlmtk_tile_style *theme_tile_style_ptr)
{
    return wlmaker_pane_tile_style(dock_ptr->tile_size, theme_tile_style_ptr);
}

/* ------------------------------------------------------------------------- */
/** A launcher was dragged and dropped: Move it to the tile it's over. */
void _wlmaker_dock_handle_drop(
    void *ud_ptr,
    wlmaker_launcher_t *launcher_ptr,
    double x,
    double y)
{
    wlmaker_dock_t *dock_ptr = ud_ptr;
    wlmtk_tile_t *target_ptr = wlmtk_dock_tile_at(dock_ptr->wlmtk_dock_ptr, x, y);
    wlmtk_tile_t *tile_ptr = wlmaker_launcher_tile(launcher_ptr);
    if (NULL == target_ptr || tile_ptr == target_ptr) return;

    size_t to = wlmtk_dock_tile_index(dock_ptr->wlmtk_dock_ptr, target_ptr);
    if (SIZE_MAX == to) return;
    wlmtk_dock_move_tile(dock_ptr->wlmtk_dock_ptr, tile_ptr, to);

    // Mirror the move in our list.
    size_t from = 0;
    while (from < dock_ptr->launchers &&
           dock_ptr->launchers_ptr[from] != launcher_ptr) ++from;
    if (from >= dock_ptr->launchers) return;
    for (size_t i = from; i + 1 < dock_ptr->launchers; ++i) {
        dock_ptr->launchers_ptr[i] = dock_ptr->launchers_ptr[i + 1];
    }
    if (to > dock_ptr->launchers - 1) to = dock_ptr->launchers - 1;
    for (size_t i = dock_ptr->launchers - 1; i > to; --i) {
        dock_ptr->launchers_ptr[i] = dock_ptr->launchers_ptr[i - 1];
    }
    dock_ptr->launchers_ptr[to] = launcher_ptr;

    if (!_wlmaker_dock_save(dock_ptr)) {
        bs_log(BS_WARNING, "Failed to save the dock's order.");
    }
}

/** Arguments for @ref _wlmaker_dock_copy_entry. */
typedef struct {
    /** Dict to copy into. */
    bspl_dict_t               *dict_ptr;
    /** Keys to skip. NULL-terminated. */
    const char                **skip_ptr;
} _wlmaker_dock_copy_arg_t;

/* ------------------------------------------------------------------------- */
/** Iterator for bspl_dict_foreach: Copies entries not listed in `skip`. */
static bool _wlmaker_dock_copy_entry(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *ud_ptr)
{
    _wlmaker_dock_copy_arg_t *arg_ptr = ud_ptr;
    for (const char **s = arg_ptr->skip_ptr; NULL != *s; ++s) {
        if (0 == strcmp(*s, key_ptr)) return true;
    }
    return bspl_dict_add(arg_ptr->dict_ptr, key_ptr, object_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Saves the state with the dock's launchers in their current order, and
 * `Sort = Manual` (the order was arranged by hand).
 *
 * @return true on success.
 */
bool _wlmaker_dock_save(wlmaker_dock_t *dock_ptr)
{
    bspl_dict_t *old_dock_ptr = bspl_dict_get_dict(
        dock_ptr->state_dict_ptr, "Dock");
    if (NULL == old_dock_ptr) return false;

    bool rv = false;
    bspl_dict_t *root_ptr = bspl_dict_create();
    bspl_dict_t *new_dock_ptr = bspl_dict_create();
    bspl_array_t *launchers_ptr = bspl_array_create();
    bspl_string_t *manual_ptr = bspl_string_create("Manual");
    char *fname_ptr = wlmaker_files_xdg_config_fname(
        dock_ptr->server_ptr->files_ptr, "State.plist");
    bs_dynbuf_t dynbuf = {};
    if (NULL == root_ptr || NULL == new_dock_ptr || NULL == launchers_ptr ||
        NULL == manual_ptr || NULL == fname_ptr ||
        !bs_dynbuf_init(&dynbuf, 4096, 1 << 20)) goto done;

    for (size_t i = 0; i < dock_ptr->launchers; ++i) {
        if (!bspl_array_push_back(
                launchers_ptr,
                bspl_object_from_dict(
                    wlmaker_launcher_dict(dock_ptr->launchers_ptr[i])))) {
            goto done;
        }
    }
    const char *skip_dock[] = { "Launchers", "Sort", NULL };
    _wlmaker_dock_copy_arg_t arg = { new_dock_ptr, skip_dock };
    if (!bspl_dict_foreach(old_dock_ptr, _wlmaker_dock_copy_entry, &arg) ||
        !bspl_dict_add(new_dock_ptr, "Sort",
                       bspl_object_from_string(manual_ptr)) ||
        !bspl_dict_add(new_dock_ptr, "Launchers",
                       bspl_object_from_array(launchers_ptr))) goto done;
    const char *skip_root[] = { "Dock", NULL };
    arg = (_wlmaker_dock_copy_arg_t){ root_ptr, skip_root };
    if (!bspl_dict_foreach(dock_ptr->state_dict_ptr,
                           _wlmaker_dock_copy_entry, &arg) ||
        !bspl_dict_add(root_ptr, "Dock",
                       bspl_object_from_dict(new_dock_ptr))) goto done;

    const char *header = "// Written by wlmaker (dock order).\n";
    if (!bs_dynbuf_append(&dynbuf, header, strlen(header)) ||
        !bspl_object_write_indented(
            bspl_object_from_dict(root_ptr), &dynbuf, 2, 0) ||
        !bs_dynbuf_append(&dynbuf, "\n", 1) ||
        !bs_dynbuf_write_file(&dynbuf, fname_ptr, 0600)) goto done;

    bspl_dict_unref(dock_ptr->state_dict_ptr);
    dock_ptr->state_dict_ptr = bspl_dict_ref(root_ptr);
    rv = true;
    bs_log(BS_INFO, "Saved dock order to %s", fname_ptr);

done:
    bs_dynbuf_fini(&dynbuf);
    free(fname_ptr);
    if (NULL != manual_ptr) bspl_string_unref(manual_ptr);
    if (NULL != launchers_ptr) bspl_array_unref(launchers_ptr);
    if (NULL != new_dock_ptr) bspl_dict_unref(new_dock_ptr);
    if (NULL != root_ptr) bspl_dict_unref(root_ptr);
    return rv;
}

/* == Unit tests =========================================================== */

static void test_create_destroy(bs_test_t *test_ptr);

/** Unit test cases. */
static const bs_test_case_t wlmaker_dock_test_cases[] = {
    { true, "create_destroy", test_create_destroy },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_dock_test_set = BS_TEST_SET(
    true, "dock", wlmaker_dock_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests ctor and dtor; to help fix leaks. */
void test_create_destroy(bs_test_t *test_ptr)
{
    bs_test_setenv(test_ptr, "XDG_DATA_DIRS", WLMAKER_SOURCE_DIR "/share");

    struct wlr_scene *wlr_scene_ptr = wlr_scene_create();
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, wlr_scene_ptr);
    wlmaker_server_t server = {
        .wlr_scene_ptr = wlr_scene_ptr,
        .wl_display_ptr = wl_display_create(),
        .files_ptr = wlmaker_files_create("wlmaker"),
    };
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.files_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.wl_display_ptr);
    wl_signal_init(&server.theme_changed_event);
    server.wlr_output_layout_ptr = wlr_output_layout_create(
        server.wl_display_ptr);
    struct wlr_output output = { .width = 1024, .height = 768, .scale = 1 };
    wlmtk_test_wlr_output_init(&output);
    wlr_output_layout_add_auto(server.wlr_output_layout_ptr, &output);

    bspl_dict_t *dict_ptr = bspl_dict_from_object(
        bspl_create_object_from_plist_data(
            embedded_binary_default_state_data,
            embedded_binary_default_state_size));
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dict_ptr);

    server.root_ptr = wlmtk_root_create(
        server.wlr_scene_ptr,
        server.wlr_output_layout_ptr);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, server.root_ptr);

    struct wlmtk_tile_style ts = {};
    wlmtk_workspace_t *ws_ptr = wlmtk_workspace_create(
        server.wlr_output_layout_ptr, "1", &ts);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, ws_ptr);
    wlmtk_root_add_workspace(server.root_ptr, ws_ptr);

    wlmaker_config_style_t style = {};

    wlmaker_dock_t *dock_ptr = wlmaker_dock_create(&server, dict_ptr, &style);
    BS_TEST_VERIFY_NEQ_OR_RETURN(test_ptr, NULL, dock_ptr);

    wlmaker_dock_destroy(dock_ptr);

    bspl_dict_unref(dict_ptr);
    wlmtk_root_remove_workspace(server.root_ptr, ws_ptr);
    wlmtk_workspace_destroy(ws_ptr);
    wlmtk_root_destroy(server.root_ptr);
    wl_display_destroy(server.wl_display_ptr);
    wlr_scene_node_destroy(&wlr_scene_ptr->tree.node);
    wlmaker_files_destroy(server.files_ptr);
}

/* == End of dock.c ======================================================== */
