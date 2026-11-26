/* ========================================================================= */
/**
 * @file tl_menu.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2025 Google LLC
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

#include "tl_menu.h"

#include <libbase/libbase.h>
#include <stddef.h>
#include <stdlib.h>
#include <wayland-server-core.h>

#include "action.h"
#include "action_item.h"
#include "server.h"

/* == Declarations ========================================================= */

/** State of a toplevel's window menu. */
struct _wlmaker_tl_menu_t {
    /** Pointer to the window's @ref wlmtk_menu_t. */
    wlmtk_menu_t              *menu_ptr;
    /** Pointer to the submenu of `move_to_ws_ai_ptr`. */
    wlmtk_menu_t              *workspaces_submenu_ptr;

    /** Holds @ref wlmaker_tl_menu_ws_item_t::dlnode items. */
    bs_dllist_t               submenu_items;

    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;
    /** Back-link to the window. */
    wlmtk_window_t           *window_ptr;

    /** Listener for @ref wlmtk_window_events_t::state_changed. */
    struct wl_listener        window_state_changed_listener;
    /** Listener for @ref wlmtk_root_events_t::workspace_changed. */
    struct wl_listener        workspace_changed_listener;

    /** Action item for 'Minimize'. */
    wlmaker_action_item_t     *minimize_ai_ptr;
    /** Action item for 'Hide': Minimizes all windows of the application. */
    wlmaker_action_item_t     *hide_ai_ptr;
    /** Action item for 'Hide Others'. */
    wlmaker_action_item_t     *hide_others_ai_ptr;
    /** Action item for 'Maximize'. */
    wlmaker_action_item_t     *maximize_ai_ptr;
    /** Action item for 'Unmaximize'. */
    wlmaker_action_item_t     *unmaximize_ai_ptr;
    /** Action item for 'Fullscreen'. */
    wlmaker_action_item_t     *fullscreen_ai_ptr;
    /** Action item for 'Shade'. */
    wlmaker_action_item_t     *shade_ai_ptr;
    /** Action item for 'Unshade'. */
    wlmaker_action_item_t     *unshade_ai_ptr;
    /** Menu item for attaching the workspaces submenu. */
    wlmaker_action_item_t     *move_to_ws_ai_ptr;
    /** Action item for 'close'. */
    wlmaker_action_item_t     *close_ai_ptr;
    /** Action item for 'Kill': Ends the application's process. */
    wlmaker_action_item_t     *kill_ai_ptr;

    /** Menu item for attaching the 'Other maximization' submenu. */
    wlmaker_action_item_t     *tile_ai_ptr;
    /** The 'Other maximization' submenu: Halves, quarters, center. */
    wlmtk_menu_t              *tile_submenu_ptr;
    /** Its items. */
    wlmaker_action_item_t     *tile_items_ptr[10];
    /** Menu item for attaching the 'Options' submenu. */
    wlmaker_action_item_t     *options_ai_ptr;
    /** The 'Options' submenu: Keep on top, at bottom, on all workspaces. */
    wlmtk_menu_t              *options_submenu_ptr;
    /** 'Keep on top' (toggles). */
    wlmaker_action_item_t     *on_top_ai_ptr;
    /** 'Keep at bottom' (toggles). */
    wlmaker_action_item_t     *at_bottom_ai_ptr;
    /** 'On all workspaces' (toggles). */
    wlmaker_action_item_t     *omnipresent_ai_ptr;
};

/** Item holder. */
typedef struct {
    /** Element of @ref wlmaker_tl_menu_t::submenu_items. */
    bs_dllist_node_t          dlnode;

    /** Composed from a menu item. */
    wlmtk_menu_item_t         *menu_item_ptr;

    /** Window to move. */
    wlmtk_window_t           *window_ptr;
    /** Workspace to move it to. */
    wlmtk_workspace_t         *workspace_ptr;

    /** Listener for @ref wlmtk_menu_item_events_t::triggered. */
    struct wl_listener        triggered_listener;
    /** Listener for @ref wlmtk_menu_item_events_t::destroy. */
    struct wl_listener        destroy_listener;

} wlmaker_tl_menu_ws_item_t;

static void _wlmaker_tl_menu_workspace_iterator_create_item(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmaker_tl_menu_ws_items_iterator_enable_workspace(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmaker_tl_menu_handle_window_state_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_tl_menu_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);

static void _destroy(wlmaker_tl_menu_ws_item_t *ws_item_ptr);
static void _item_handle_triggered(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _item_handle_destroy(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Data ================================================================= */

/** Menu items for the XDG toplevel's window menu. */
static const wlmaker_action_item_desc_t _tl_menu_items[] = {
    {
        "Minimize",
        WLMAKER_ACTION_WINDOW_MINIMIZE,
        NULL,
        offsetof(wlmaker_tl_menu_t, minimize_ai_ptr)
    },
    {
        "Hide",
        WLMAKER_ACTION_HIDE_APPLICATION,
        NULL,
        offsetof(wlmaker_tl_menu_t, hide_ai_ptr)
    },
    {
        "Hide Others",
        WLMAKER_ACTION_HIDE_OTHERS,
        NULL,
        offsetof(wlmaker_tl_menu_t, hide_others_ai_ptr)
    },
    {
        "Maximize",
        WLMAKER_ACTION_WINDOW_MAXIMIZE,
        NULL,
        offsetof(wlmaker_tl_menu_t, maximize_ai_ptr)
    },
    {
        "Unmaximize",
        WLMAKER_ACTION_WINDOW_UNMAXIMIZE,
        NULL,
        offsetof(wlmaker_tl_menu_t, unmaximize_ai_ptr)
    },
    {
        "Other maximization",
        WLMAKER_ACTION_NONE,
        NULL,
        offsetof(wlmaker_tl_menu_t, tile_ai_ptr)
    },
    {
        "Fullscreen",
        WLMAKER_ACTION_WINDOW_TOGGLE_FULLSCREEN,
        NULL,
        offsetof(wlmaker_tl_menu_t, fullscreen_ai_ptr)
    },
    {
        "Shade",
        WLMAKER_ACTION_WINDOW_SHADE,
        NULL,
        offsetof(wlmaker_tl_menu_t, shade_ai_ptr)

    },
    {
        "Unshade",
        WLMAKER_ACTION_WINDOW_UNSHADE,
        NULL,
        offsetof(wlmaker_tl_menu_t, unshade_ai_ptr)

    },
    {
        "Move to workspace ...",
        WLMAKER_ACTION_NONE,
        NULL,
        offsetof(wlmaker_tl_menu_t, move_to_ws_ai_ptr)
    },
    {
        "Options",
        WLMAKER_ACTION_NONE,
        NULL,
        offsetof(wlmaker_tl_menu_t, options_ai_ptr)
    },
    {
        "Close",
        WLMAKER_ACTION_WINDOW_CLOSE,
        NULL,
        offsetof(wlmaker_tl_menu_t, close_ai_ptr)

    },
    {
        "Kill",
        WLMAKER_ACTION_WINDOW_KILL,
        NULL,
        offsetof(wlmaker_tl_menu_t, kill_ai_ptr)
    },
    { NULL, 0, NULL, 0 }  // Sentinel.
};

/** The 'Other maximization' submenu, as Window Maker's. */
static const wlmaker_action_item_desc_t _tl_menu_tile_items[] = {
    { "Left half", WLMAKER_ACTION_WINDOW_TILE_LEFT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[0]) },
    { "Right half", WLMAKER_ACTION_WINDOW_TILE_RIGHT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[1]) },
    { "Top half", WLMAKER_ACTION_WINDOW_TILE_TOP, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[2]) },
    { "Bottom half", WLMAKER_ACTION_WINDOW_TILE_BOTTOM, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[3]) },
    { "Top left", WLMAKER_ACTION_WINDOW_TILE_TOP_LEFT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[4]) },
    { "Top right", WLMAKER_ACTION_WINDOW_TILE_TOP_RIGHT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[5]) },
    { "Bottom left", WLMAKER_ACTION_WINDOW_TILE_BOTTOM_LEFT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[6]) },
    { "Bottom right", WLMAKER_ACTION_WINDOW_TILE_BOTTOM_RIGHT, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[7]) },
    { "Center", WLMAKER_ACTION_WINDOW_CENTER, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[8]) },
    { "Restore size", WLMAKER_ACTION_WINDOW_UNTILE, NULL,
      offsetof(wlmaker_tl_menu_t, tile_items_ptr[9]) },
    { NULL, 0, NULL, 0 }  // Sentinel.
};

/** The 'Options' submenu. Texts change with the state, see below. */
static const wlmaker_action_item_desc_t _tl_menu_options_items[] = {
    { "Keep on top", WLMAKER_ACTION_WINDOW_TOGGLE_ON_TOP, NULL,
      offsetof(wlmaker_tl_menu_t, on_top_ai_ptr) },
    { "Keep at bottom", WLMAKER_ACTION_WINDOW_TOGGLE_AT_BOTTOM, NULL,
      offsetof(wlmaker_tl_menu_t, at_bottom_ai_ptr) },
    { "Show on all workspaces", WLMAKER_ACTION_WINDOW_TOGGLE_OMNIPRESENT, NULL,
      offsetof(wlmaker_tl_menu_t, omnipresent_ai_ptr) },
    { NULL, 0, NULL, 0 }  // Sentinel.
};

/* ------------------------------------------------------------------------- */
/** Creates a submenu with the items of `descs`, attached to `ai_ptr`. */
static wlmtk_menu_t *_wlmaker_tl_menu_create_submenu(
    wlmaker_tl_menu_t *tl_menu_ptr,
    const wlmaker_action_item_desc_t *descs,
    wlmaker_action_item_t *ai_ptr)
{
    wlmaker_server_t *server_ptr = tl_menu_ptr->server_ptr;
    wlmtk_menu_t *menu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == menu_ptr) return NULL;
    for (const wlmaker_action_item_desc_t *desc_ptr = descs;
         NULL != desc_ptr->text_ptr;
         ++desc_ptr) {
        wlmaker_action_item_t *item_ptr = wlmaker_action_item_create_from_desc(
            desc_ptr,
            tl_menu_ptr,
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr),
            server_ptr);
        if (NULL == item_ptr) {
            bs_log(BS_ERROR, "Failed wlmaker_action_item_create_from_desc()");
            return menu_ptr;
        }
        wlmtk_menu_add_item(menu_ptr, wlmaker_action_item_menu_item(item_ptr));
    }
    wlmtk_menu_item_set_submenu(wlmaker_action_item_menu_item(ai_ptr), menu_ptr);
    return menu_ptr;
}

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_tl_menu_t *wlmaker_tl_menu_create(
    wlmtk_window_t *window_ptr,
    wlmaker_server_t *server_ptr)
{
    wlmaker_tl_menu_t *tl_menu_ptr = logged_calloc(
        1, sizeof(wlmaker_tl_menu_t));
    if (NULL == tl_menu_ptr) return NULL;
    tl_menu_ptr->server_ptr = server_ptr;
    tl_menu_ptr->menu_ptr = wlmtk_window_menu(window_ptr);
    tl_menu_ptr->window_ptr = window_ptr;

    for (const wlmaker_action_item_desc_t *desc_ptr = &_tl_menu_items[0];
         NULL != desc_ptr->text_ptr;
         ++desc_ptr) {

        wlmaker_action_item_t *ai_ptr = wlmaker_action_item_create_from_desc(
            desc_ptr,
            tl_menu_ptr,
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr),
            server_ptr);
        if (NULL == ai_ptr) {
            bs_log(BS_ERROR, "Failed wlmaker_action_item_create_from_desc()");
            wlmaker_tl_menu_destroy(tl_menu_ptr);
            return NULL;
        }

        wlmtk_menu_add_item(
            tl_menu_ptr->menu_ptr,
            wlmaker_action_item_menu_item(ai_ptr));
    }

    tl_menu_ptr->workspaces_submenu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == tl_menu_ptr->workspaces_submenu_ptr) {
        wlmaker_tl_menu_destroy(tl_menu_ptr);
        return NULL;
    }
    wlmtk_menu_item_set_submenu(
        wlmaker_action_item_menu_item(tl_menu_ptr->move_to_ws_ai_ptr),
        tl_menu_ptr->workspaces_submenu_ptr);

    tl_menu_ptr->tile_submenu_ptr = _wlmaker_tl_menu_create_submenu(
        tl_menu_ptr, _tl_menu_tile_items, tl_menu_ptr->tile_ai_ptr);
    tl_menu_ptr->options_submenu_ptr = _wlmaker_tl_menu_create_submenu(
        tl_menu_ptr, _tl_menu_options_items, tl_menu_ptr->options_ai_ptr);
    if (NULL == tl_menu_ptr->tile_submenu_ptr ||
        NULL == tl_menu_ptr->options_submenu_ptr ||
        NULL == tl_menu_ptr->on_top_ai_ptr ||
        NULL == tl_menu_ptr->omnipresent_ai_ptr ||
        NULL == tl_menu_ptr->tile_items_ptr[9]) {
        wlmaker_tl_menu_destroy(tl_menu_ptr);
        return NULL;
    }

    // Connect state listener and initialize state.
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(window_ptr)->state_changed,
        &tl_menu_ptr->window_state_changed_listener,
        _wlmaker_tl_menu_handle_window_state_changed);
    _wlmaker_tl_menu_handle_window_state_changed(
        &tl_menu_ptr->window_state_changed_listener,
        window_ptr);

    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->workspace_changed,
        &tl_menu_ptr->workspace_changed_listener,
        _wlmaker_tl_menu_handle_workspace_changed);
    _wlmaker_tl_menu_handle_workspace_changed(
        &tl_menu_ptr->workspace_changed_listener,
        NULL);

    return tl_menu_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_tl_menu_destroy(wlmaker_tl_menu_t *tl_menu_ptr)
{
    wlmtk_util_disconnect_listener(
        &tl_menu_ptr->workspace_changed_listener);
    wlmtk_util_disconnect_listener(
        &tl_menu_ptr->window_state_changed_listener);

    free(tl_menu_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Handles state changes: Updates the menu items accordingly. */
void _wlmaker_tl_menu_handle_window_state_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_tl_menu_t *tl_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_tl_menu_t, window_state_changed_listener);
    wlmtk_window_t *window_ptr = data_ptr;

    wlmtk_menu_item_set_enabled(
        wlmaker_action_item_menu_item(tl_menu_ptr->shade_ai_ptr),
        !wlmtk_window_is_shaded(window_ptr));
    wlmtk_menu_item_set_enabled(
        wlmaker_action_item_menu_item(tl_menu_ptr->unshade_ai_ptr),
        wlmtk_window_is_shaded(window_ptr));

    wlmtk_menu_item_set_enabled(
        wlmaker_action_item_menu_item(tl_menu_ptr->fullscreen_ai_ptr),
        !wlmtk_window_is_fullscreen(window_ptr));

    wlmtk_menu_item_set_enabled(
        wlmaker_action_item_menu_item(tl_menu_ptr->maximize_ai_ptr),
        !wlmtk_window_is_maximized(window_ptr));
    wlmtk_menu_item_set_enabled(
        wlmaker_action_item_menu_item(tl_menu_ptr->unmaximize_ai_ptr),
        wlmtk_window_is_maximized(window_ptr));

    // Tiling needs a resizable window; 'Restore size' a tiled one.
    if (NULL != tl_menu_ptr->tile_ai_ptr) {
        wlmtk_menu_item_set_enabled(
            wlmaker_action_item_menu_item(tl_menu_ptr->tile_ai_ptr),
            wlmtk_window_is_resizable(window_ptr) &&
            !wlmtk_window_is_fullscreen(window_ptr));
    }
    if (NULL != tl_menu_ptr->tile_items_ptr[9]) {
        wlmtk_menu_item_set_enabled(
            wlmaker_action_item_menu_item(tl_menu_ptr->tile_items_ptr[9]),
            0 != wlmtk_window_get_tiled_edges(window_ptr));
    }
    if (NULL != tl_menu_ptr->on_top_ai_ptr) {
        wlmtk_menu_item_set_text(
            wlmaker_action_item_menu_item(tl_menu_ptr->on_top_ai_ptr),
            wlmtk_window_is_keep_on_top(window_ptr) ?
            "Don't keep on top" : "Keep on top");
    }
    if (NULL != tl_menu_ptr->at_bottom_ai_ptr) {
        wlmtk_menu_item_set_text(
            wlmaker_action_item_menu_item(tl_menu_ptr->at_bottom_ai_ptr),
            wlmtk_window_is_keep_at_bottom(window_ptr) ?
            "Don't keep at bottom" : "Keep at bottom");
    }
    if (NULL != tl_menu_ptr->omnipresent_ai_ptr) {
        wlmtk_menu_item_set_text(
            wlmaker_action_item_menu_item(tl_menu_ptr->omnipresent_ai_ptr),
            wlmtk_window_is_omnipresent(window_ptr) ?
            "Show on this workspace only" : "Show on all workspaces");
    }

    // Refresh the list of workspaces.
    bs_dllist_for_each(
        &tl_menu_ptr->submenu_items,
        _wlmaker_tl_menu_ws_items_iterator_enable_workspace,
        NULL);

}

/* ------------------------------------------------------------------------- */
/** Handles workspace changes: Refreshes the workspace menu. */
void _wlmaker_tl_menu_handle_workspace_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_tl_menu_t *tl_menu_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_tl_menu_t, workspace_changed_listener);

    // Flush, then rebuild the workspaces submenu.
    while (0 < wlmtk_menu_items_size(tl_menu_ptr->workspaces_submenu_ptr)) {
        wlmtk_menu_remove_item(
            tl_menu_ptr->workspaces_submenu_ptr,
            wlmtk_menu_item_at(tl_menu_ptr->workspaces_submenu_ptr, 0));
    }
    wlmtk_root_for_each_workspace(
        tl_menu_ptr->server_ptr->root_ptr,
        _wlmaker_tl_menu_workspace_iterator_create_item,
        tl_menu_ptr);

    // Disable the currently-active workspace.
    bs_dllist_for_each(
        &tl_menu_ptr->submenu_items,
        _wlmaker_tl_menu_ws_items_iterator_enable_workspace,
        NULL);
}

/* ------------------------------------------------------------------------- */
/** Destroys the item holder. */
void _destroy(wlmaker_tl_menu_ws_item_t *ws_item_ptr)
{
    if (NULL != ws_item_ptr->menu_item_ptr) {
        wlmtk_menu_item_destroy(ws_item_ptr->menu_item_ptr);
        ws_item_ptr->menu_item_ptr = NULL;
    }
    free(ws_item_ptr);
}

/* ------------------------------------------------------------------------- */
/** Creates a menu item for each workspace. */
void _wlmaker_tl_menu_workspace_iterator_create_item(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_workspace_from_dlnode(dlnode_ptr);
    wlmaker_tl_menu_t *tl_menu_ptr = ud_ptr;

    const char *name_ptr;
    int index;
    wlmtk_workspace_get_details(workspace_ptr, &name_ptr, &index);

    wlmaker_tl_menu_ws_item_t *ws_item_ptr = logged_calloc(
        1, sizeof(wlmaker_tl_menu_ws_item_t));
    if (NULL == ws_item_ptr) return;
    ws_item_ptr->workspace_ptr = workspace_ptr;
    ws_item_ptr->window_ptr = tl_menu_ptr->window_ptr;

    ws_item_ptr->menu_item_ptr = wlmtk_menu_item_create(
        wlmtk_menu_style_to_ref(
            tl_menu_ptr->server_ptr->style_ptr->menu_style_ptr));
    if (NULL == ws_item_ptr->menu_item_ptr) {
        _destroy(ws_item_ptr);
        return;
    }

    wlmtk_menu_item_set_text(ws_item_ptr->menu_item_ptr, name_ptr);

    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(ws_item_ptr->menu_item_ptr)->triggered,
        &ws_item_ptr->triggered_listener,
        _item_handle_triggered);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_item_events(ws_item_ptr->menu_item_ptr)->destroy,
        &ws_item_ptr->destroy_listener,
        _item_handle_destroy);

    wlmtk_menu_add_item(
        tl_menu_ptr->workspaces_submenu_ptr,
        ws_item_ptr->menu_item_ptr);
    bs_dllist_push_back(&tl_menu_ptr->submenu_items, &ws_item_ptr->dlnode);
}

/* ------------------------------------------------------------------------- */
/** Enables workspace items, except the one the window is currently on. */
void _wlmaker_tl_menu_ws_items_iterator_enable_workspace(
    bs_dllist_node_t *dlnode_ptr,
    __UNUSED__ void *ud_ptr)
{
    wlmaker_tl_menu_ws_item_t *ws_item_ptr = BS_CONTAINER_OF(
        dlnode_ptr, wlmaker_tl_menu_ws_item_t, dlnode);

    wlmtk_menu_item_set_enabled(
        ws_item_ptr->menu_item_ptr,
        (wlmtk_window_get_workspace(ws_item_ptr->window_ptr) !=
         ws_item_ptr->workspace_ptr));
}

/* ------------------------------------------------------------------------- */
/** Handler for @ref wlmtk_menu_item_events_t::triggered. Moves the window. */
void _item_handle_triggered(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_tl_menu_ws_item_t *ws_item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_tl_menu_ws_item_t, triggered_listener);

    wlmtk_workspace_unmap_window(
        wlmtk_window_get_workspace(ws_item_ptr->window_ptr),
        ws_item_ptr->window_ptr);
    wlmtk_workspace_map_window(
        ws_item_ptr->workspace_ptr,
        ws_item_ptr->window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Handler for @ref wlmtk_menu_item_events_t::destroy. Destroy. */
void _item_handle_destroy(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_tl_menu_ws_item_t *ws_item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_tl_menu_ws_item_t, destroy_listener);

    ws_item_ptr->menu_item_ptr = NULL;
    _destroy(ws_item_ptr);
}

/* == End of tl_menu.c ===================================================== */
