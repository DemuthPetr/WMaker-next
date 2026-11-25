/* ========================================================================= */
/**
 * @file window_list.c
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

#include "window_list.h"

#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#undef WLR_USE_UNSTABLE

#include "input/manager.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the window list menu. */
struct _wlmaker_window_list_t {
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** Window holding the menu. */
    wlmtk_window_t            *window_ptr;
    /** The menu. */
    wlmtk_menu_t              *menu_ptr;
    /** Items, @ref wlmaker_window_list_item_t::dlnode. */
    bs_dllist_t               items;

    /** Listener for @ref wlmtk_menu_events_t::open_changed. */
    struct wl_listener        menu_open_changed_listener;
    /** Listener for @ref wlmtk_menu_events_t::request_close. */
    struct wl_listener        menu_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;
    /** Listener for @ref wlmtk_root_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;
};

/** An item of the window list: Refers to one window. */
typedef struct {
    /** Element of @ref wlmaker_window_list_t::items. */
    bs_dllist_node_t          dlnode;
    /** Back-link to the window list. */
    wlmaker_window_list_t     *window_list_ptr;
    /** The menu item. */
    wlmtk_menu_item_t         *menu_item_ptr;
    /** The window it refers to. NULL once the window is unmapped. */
    wlmtk_window_t            *target_window_ptr;
    /** Listener for @ref wlmtk_menu_item_events_t::triggered. */
    struct wl_listener        triggered_listener;
} wlmaker_window_list_item_t;

/** Argument for @ref _wlmaker_window_list_add_workspace_items. */
typedef struct {
    /** The window list. */
    wlmaker_window_list_t     *window_list_ptr;
    /** The current workspace. */
    wlmtk_workspace_t         *current_workspace_ptr;
} wlmaker_window_list_build_arg_t;

static void _wlmaker_window_list_clear(wlmaker_window_list_t *window_list_ptr);
static void _wlmaker_window_list_add_workspace_items(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr);
static void _wlmaker_window_list_add_item(
    wlmaker_window_list_t *window_list_ptr,
    wlmtk_window_t *window_ptr,
    const char *workspace_name_ptr,
    bool on_current_workspace);
static void _wlmaker_window_list_item_destroy(
    wlmaker_window_list_item_t *item_ptr);

static void _wlmaker_window_list_handle_item_triggered(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_window_list_handle_menu_open_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_window_list_handle_menu_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_window_list_handle_window_request_close(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_window_list_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    void *data_ptr);
static void _wlmaker_window_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_window_list_t *wlmaker_window_list_create(wlmaker_server_t *server_ptr)
{
    wlmaker_window_list_t *window_list_ptr = logged_calloc(
        1, sizeof(wlmaker_window_list_t));
    if (NULL == window_list_ptr) return NULL;
    window_list_ptr->server_ptr = server_ptr;

    window_list_ptr->menu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == window_list_ptr->menu_ptr) {
        wlmaker_window_list_destroy(window_list_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(window_list_ptr->menu_ptr)->open_changed,
        &window_list_ptr->menu_open_changed_listener,
        _wlmaker_window_list_handle_menu_open_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(window_list_ptr->menu_ptr)->request_close,
        &window_list_ptr->menu_request_close_listener,
        _wlmaker_window_list_handle_menu_request_close);

    window_list_ptr->window_ptr = wlmtk_window_create(
        wlmtk_menu_element(window_list_ptr->menu_ptr),
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == window_list_ptr->window_ptr) {
        wlmaker_window_list_destroy(window_list_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(window_list_ptr->window_ptr)->request_close,
        &window_list_ptr->window_request_close_listener,
        _wlmaker_window_list_handle_window_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(window_list_ptr->window_ptr)->set_activated,
        &window_list_ptr->window_set_activated_listener,
        _wlmaker_window_list_handle_window_set_activated);
    wlmtk_window_set_title(window_list_ptr->window_ptr, "Windows");
    wlmtk_window_set_server_side_decorated(window_list_ptr->window_ptr, true);
    wlmtk_window_set_properties(window_list_ptr->window_ptr,
                                WLMTK_WINDOW_PROPERTY_CLOSABLE);

    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->window_unmapped,
        &window_list_ptr->window_unmapped_listener,
        _wlmaker_window_list_handle_window_unmapped);
    return window_list_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_window_list_destroy(wlmaker_window_list_t *window_list_ptr)
{
    wlmtk_util_disconnect_listener(&window_list_ptr->window_unmapped_listener);

    if (NULL != window_list_ptr->window_ptr) {
        wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
            window_list_ptr->window_ptr);
        if (NULL != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr,
                                          window_list_ptr->window_ptr);
        }
        wlmtk_util_disconnect_listener(
            &window_list_ptr->window_set_activated_listener);
        wlmtk_util_disconnect_listener(
            &window_list_ptr->window_request_close_listener);
        wlmtk_window_destroy(window_list_ptr->window_ptr);
        window_list_ptr->window_ptr = NULL;
    }

    if (NULL != window_list_ptr->menu_ptr) {
        _wlmaker_window_list_clear(window_list_ptr);
        wlmtk_util_disconnect_listener(
            &window_list_ptr->menu_request_close_listener);
        wlmtk_util_disconnect_listener(
            &window_list_ptr->menu_open_changed_listener);
        wlmtk_menu_destroy(window_list_ptr->menu_ptr);
        window_list_ptr->menu_ptr = NULL;
    }
    free(window_list_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_window_list_open(wlmaker_window_list_t *window_list_ptr)
{
    wlmaker_server_t *server_ptr = window_list_ptr->server_ptr;
    if (NULL != wlmtk_window_get_workspace(window_list_ptr->window_ptr)) return;

    _wlmaker_window_list_clear(window_list_ptr);
    wlmaker_window_list_build_arg_t arg = {
        .window_list_ptr = window_list_ptr,
        .current_workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr),
    };
    wlmtk_root_for_each_workspace(
        server_ptr->root_ptr,
        _wlmaker_window_list_add_workspace_items,
        &arg);
    if (NULL == window_list_ptr->items.head_ptr) {
        _wlmaker_window_list_add_item(window_list_ptr, NULL, NULL, true);
    }

    wlmtk_workspace_t *workspace_ptr = arg.current_workspace_ptr;
    struct wlr_cursor *wlr_cursor_ptr = wlmim_wlr_cursor(
        server_ptr->input_manager_ptr);
    wlmtk_workspace_map_window(workspace_ptr, window_list_ptr->window_ptr);
    wlmtk_workspace_set_window_position(
        workspace_ptr, window_list_ptr->window_ptr,
        wlr_cursor_ptr->x, wlr_cursor_ptr->y);
    wlmtk_workspace_confine_within(workspace_ptr, window_list_ptr->window_ptr);
    wlmtk_menu_set_mode(window_list_ptr->menu_ptr, WLMTK_MENU_MODE_NORMAL);
    wlmtk_menu_set_open(window_list_ptr->menu_ptr, true);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Removes and destroys all items. */
void _wlmaker_window_list_clear(wlmaker_window_list_t *window_list_ptr)
{
    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = window_list_ptr->items.head_ptr)) {
        _wlmaker_window_list_item_destroy(BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_window_list_item_t, dlnode));
    }
}

/* ------------------------------------------------------------------------- */
/** Adds an item for each window of the workspace at `dlnode_ptr`. */
void _wlmaker_window_list_add_workspace_items(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    wlmaker_window_list_build_arg_t *arg_ptr = ud_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_workspace_from_dlnode(dlnode_ptr);
    const char *name_ptr;
    int index;
    wlmtk_workspace_get_details(workspace_ptr, &name_ptr, &index);

    for (bs_dllist_node_t *wdlnode_ptr =
             wlmtk_workspace_get_windows_dllist(workspace_ptr)->head_ptr;
         NULL != wdlnode_ptr;
         wdlnode_ptr = wdlnode_ptr->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(wdlnode_ptr);
        // Skip the root menu and other menus (their window has no client).
        if (window_ptr == wlmaker_root_menu_window(
                arg_ptr->window_list_ptr->server_ptr->root_menu_ptr)) continue;
        _wlmaker_window_list_add_item(
            arg_ptr->window_list_ptr, window_ptr, name_ptr,
            workspace_ptr == arg_ptr->current_workspace_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Adds an item for `window_ptr`; or a disabled placeholder for NULL. */
void _wlmaker_window_list_add_item(
    wlmaker_window_list_t *window_list_ptr,
    wlmtk_window_t *window_ptr,
    const char *workspace_name_ptr,
    bool on_current_workspace)
{
    wlmaker_window_list_item_t *item_ptr = logged_calloc(
        1, sizeof(wlmaker_window_list_item_t));
    if (NULL == item_ptr) return;
    item_ptr->window_list_ptr = window_list_ptr;
    item_ptr->target_window_ptr = window_ptr;

    item_ptr->menu_item_ptr = wlmtk_menu_item_create(
        wlmtk_menu_style_to_ref(
            window_list_ptr->server_ptr->style_ptr->menu_style_ptr));
    if (NULL == item_ptr->menu_item_ptr) {
        free(item_ptr);
        return;
    }
    bs_dllist_push_back(&window_list_ptr->items, &item_ptr->dlnode);

    char text[160];
    if (NULL == window_ptr) {
        snprintf(text, sizeof(text), "(no windows)");
        wlmtk_menu_item_set_enabled(item_ptr->menu_item_ptr, false);
    } else {
        const char *title_ptr = wlmtk_window_get_title(window_ptr);
        snprintf(text, sizeof(text), "%s%s%s%s",
                 wlmtk_window_is_minimized(window_ptr) ? "* " : "",
                 (NULL != title_ptr && *title_ptr) ? title_ptr : "(untitled)",
                 on_current_workspace ? "" : "  @ ",
                 on_current_workspace ? "" : workspace_name_ptr);
        wlmtk_util_connect_listener_signal(
            &wlmtk_menu_item_events(item_ptr->menu_item_ptr)->triggered,
            &item_ptr->triggered_listener,
            _wlmaker_window_list_handle_item_triggered);
    }
    wlmtk_menu_item_set_text(item_ptr->menu_item_ptr, text);
    wlmtk_menu_add_item(window_list_ptr->menu_ptr, item_ptr->menu_item_ptr);
}

/* ------------------------------------------------------------------------- */
/** Removes the item from the menu and destroys it. */
void _wlmaker_window_list_item_destroy(wlmaker_window_list_item_t *item_ptr)
{
    wlmaker_window_list_t *window_list_ptr = item_ptr->window_list_ptr;
    bs_dllist_remove(&window_list_ptr->items, &item_ptr->dlnode);
    wlmtk_util_disconnect_listener(&item_ptr->triggered_listener);
    wlmtk_menu_remove_item(window_list_ptr->menu_ptr, item_ptr->menu_item_ptr);
    wlmtk_menu_item_destroy(item_ptr->menu_item_ptr);
    free(item_ptr);
}

/* ------------------------------------------------------------------------- */
/** Switches to the item's window: workspace, restore, raise, activate. */
void _wlmaker_window_list_handle_item_triggered(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_window_list_item_t *item_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_item_t, triggered_listener);
    wlmaker_window_list_t *window_list_ptr = item_ptr->window_list_ptr;
    wlmtk_window_t *window_ptr = item_ptr->target_window_ptr;

    wlmtk_menu_set_open(window_list_ptr->menu_ptr, false);
    if (NULL == window_ptr) return;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (NULL == workspace_ptr) return;

    wlmtk_root_switch_to_workspace(
        window_list_ptr->server_ptr->root_ptr, workspace_ptr);
    // Activating a minimized window restores it.
    wlmtk_workspace_activate_window(workspace_ptr, window_ptr);
    wlmtk_workspace_raise_window(workspace_ptr, window_ptr);
}

/* ------------------------------------------------------------------------- */
/** Unmaps the menu's window when the menu closes. */
void _wlmaker_window_list_handle_menu_open_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_window_list_t *window_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_t, menu_open_changed_listener);
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        window_list_ptr->window_ptr);
    if (!wlmtk_menu_is_open(window_list_ptr->menu_ptr) &&
        NULL != workspace_ptr) {
        wlmtk_workspace_unmap_window(workspace_ptr,
                                      window_list_ptr->window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Handles @ref wlmtk_menu_events_t::request_close: Closes the menu. */
void _wlmaker_window_list_handle_menu_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_window_list_t *window_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_t, menu_request_close_listener);
    wlmtk_menu_set_open(window_list_ptr->menu_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** Handles the window's close button: Closes the menu. */
void _wlmaker_window_list_handle_window_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_window_list_t *window_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_t, window_request_close_listener);
    wlmtk_menu_set_open(window_list_ptr->menu_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** Gives the menu keyboard focus while its window is activated. */
void _wlmaker_window_list_handle_window_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_window_list_t *window_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_t, window_set_activated_listener);
    wlmtk_element_t *e = wlmtk_menu_element(window_list_ptr->menu_ptr);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_set_keyboard_focus_element(
            e->parent_container_ptr,
            e,
            wlmtk_window_is_activated(window_list_ptr->window_ptr));
    }
}

/* ------------------------------------------------------------------------- */
/** Drops references to windows that got unmapped while the list exists. */
void _wlmaker_window_list_handle_window_unmapped(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_window_list_t *window_list_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_list_t, window_unmapped_listener);
    wlmtk_window_t *window_ptr = data_ptr;

    for (bs_dllist_node_t *dlnode_ptr = window_list_ptr->items.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_window_list_item_t *item_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_window_list_item_t, dlnode);
        if (item_ptr->target_window_ptr == window_ptr) {
            item_ptr->target_window_ptr = NULL;
            wlmtk_menu_item_set_enabled(item_ptr->menu_item_ptr, false);
        }
    }
}

/* == End of window_list.c ================================================= */
