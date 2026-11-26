/* ========================================================================= */
/**
 * @file popup_menu.c
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

#include "popup_menu.h"

#include <libbase/libbase.h>
#include <stdlib.h>
#include <wayland-server-core.h>

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#undef WLR_USE_UNSTABLE

#include "input/manager.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the popup menu. */
struct _wlmaker_popup_menu_t {
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** Window holding the menu. */
    wlmtk_window_t            *window_ptr;
    /** The menu. */
    wlmtk_menu_t              *menu_ptr;
    /** Items, @ref wlmaker_popup_menu_entry_t::dlnode. */
    bs_dllist_t               entries;
    /** Callback of the current opening. */
    wlmaker_popup_menu_callback_t callback;
    /** Argument to `callback`. NULL when closed. */
    void                      *ud_ptr;

    /** Listener for @ref wlmtk_menu_events_t::open_changed. */
    struct wl_listener        menu_open_changed_listener;
    /** Listener for @ref wlmtk_menu_events_t::request_close. */
    struct wl_listener        menu_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::request_close. */
    struct wl_listener        window_request_close_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        window_set_activated_listener;
};

/** An item of the menu. */
typedef struct {
    /** Element of @ref wlmaker_popup_menu_t::entries. */
    bs_dllist_node_t          dlnode;
    /** Back-link. */
    wlmaker_popup_menu_t      *popup_menu_ptr;
    /** The menu item. */
    wlmtk_menu_item_t         *menu_item_ptr;
    /** Id, for the callback. */
    int                       id;
    /** Listener for @ref wlmtk_menu_item_events_t::triggered. */
    struct wl_listener        triggered_listener;
} wlmaker_popup_menu_entry_t;

static void _clear(wlmaker_popup_menu_t *popup_menu_ptr);
static void _handle_item_triggered(struct wl_listener *l, void *data_ptr);
static void _handle_menu_open_changed(struct wl_listener *l, void *data_ptr);
static void _handle_menu_request_close(struct wl_listener *l, void *data_ptr);
static void _handle_window_request_close(struct wl_listener *l, void *data_ptr);
static void _handle_window_set_activated(struct wl_listener *l, void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_popup_menu_t *wlmaker_popup_menu_create(wlmaker_server_t *server_ptr)
{
    wlmaker_popup_menu_t *pm_ptr = logged_calloc(1, sizeof(*pm_ptr));
    if (NULL == pm_ptr) return NULL;
    pm_ptr->server_ptr = server_ptr;

    pm_ptr->menu_ptr = wlmtk_menu_create(
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == pm_ptr->menu_ptr) {
        wlmaker_popup_menu_destroy(pm_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(pm_ptr->menu_ptr)->open_changed,
        &pm_ptr->menu_open_changed_listener, _handle_menu_open_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_menu_events(pm_ptr->menu_ptr)->request_close,
        &pm_ptr->menu_request_close_listener, _handle_menu_request_close);

    pm_ptr->window_ptr = wlmtk_window_create(
        wlmtk_menu_element(pm_ptr->menu_ptr),
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == pm_ptr->window_ptr) {
        wlmaker_popup_menu_destroy(pm_ptr);
        return NULL;
    }
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(pm_ptr->window_ptr)->request_close,
        &pm_ptr->window_request_close_listener, _handle_window_request_close);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(pm_ptr->window_ptr)->set_activated,
        &pm_ptr->window_set_activated_listener, _handle_window_set_activated);
    wlmtk_window_set_server_side_decorated(pm_ptr->window_ptr, true);
    wlmtk_window_set_properties(pm_ptr->window_ptr,
                                WLMTK_WINDOW_PROPERTY_CLOSABLE);
    return pm_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_popup_menu_destroy(wlmaker_popup_menu_t *pm_ptr)
{
    if (NULL != pm_ptr->window_ptr) {
        wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
            pm_ptr->window_ptr);
        if (NULL != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr, pm_ptr->window_ptr);
        }
        wlmtk_util_disconnect_listener(&pm_ptr->window_set_activated_listener);
        wlmtk_util_disconnect_listener(&pm_ptr->window_request_close_listener);
        wlmtk_window_destroy(pm_ptr->window_ptr);
        pm_ptr->window_ptr = NULL;
    }
    if (NULL != pm_ptr->menu_ptr) {
        _clear(pm_ptr);
        wlmtk_util_disconnect_listener(&pm_ptr->menu_request_close_listener);
        wlmtk_util_disconnect_listener(&pm_ptr->menu_open_changed_listener);
        wlmtk_menu_destroy(pm_ptr->menu_ptr);
        pm_ptr->menu_ptr = NULL;
    }
    free(pm_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_popup_menu_open(
    wlmaker_popup_menu_t *pm_ptr,
    const char *title_ptr,
    const wlmaker_popup_menu_item_t *items_ptr,
    size_t n,
    wlmaker_popup_menu_callback_t callback,
    void *ud_ptr)
{
    if (NULL != wlmtk_window_get_workspace(pm_ptr->window_ptr)) {
        pm_ptr->ud_ptr = NULL;
        wlmtk_menu_set_open(pm_ptr->menu_ptr, false);
    }
    _clear(pm_ptr);
    for (size_t i = 0; i < n; ++i) {
        wlmaker_popup_menu_entry_t *e_ptr = logged_calloc(1, sizeof(*e_ptr));
        if (NULL == e_ptr) break;
        e_ptr->popup_menu_ptr = pm_ptr;
        e_ptr->id = items_ptr[i].id;
        e_ptr->menu_item_ptr = wlmtk_menu_item_create(wlmtk_menu_style_to_ref(
            pm_ptr->server_ptr->style_ptr->menu_style_ptr));
        if (NULL == e_ptr->menu_item_ptr) {
            free(e_ptr);
            break;
        }
        bs_dllist_push_back(&pm_ptr->entries, &e_ptr->dlnode);
        wlmtk_menu_item_set_text(e_ptr->menu_item_ptr, items_ptr[i].text_ptr);
        wlmtk_menu_item_set_enabled(e_ptr->menu_item_ptr, items_ptr[i].enabled);
        wlmtk_util_connect_listener_signal(
            &wlmtk_menu_item_events(e_ptr->menu_item_ptr)->triggered,
            &e_ptr->triggered_listener, _handle_item_triggered);
        wlmtk_menu_add_item(pm_ptr->menu_ptr, e_ptr->menu_item_ptr);
    }
    pm_ptr->callback = callback;
    pm_ptr->ud_ptr = ud_ptr;

    wlmtk_window_set_title(pm_ptr->window_ptr, title_ptr);
    wlmtk_workspace_t *workspace_ptr = wlmtk_root_get_current_workspace(
        pm_ptr->server_ptr->root_ptr);
    struct wlr_cursor *wlr_cursor_ptr = wlmim_wlr_cursor(
        pm_ptr->server_ptr->input_manager_ptr);
    wlmtk_workspace_map_window(workspace_ptr, pm_ptr->window_ptr);
    wlmtk_workspace_set_window_position(
        workspace_ptr, pm_ptr->window_ptr, wlr_cursor_ptr->x, wlr_cursor_ptr->y);
    wlmtk_workspace_confine_within(workspace_ptr, pm_ptr->window_ptr);
    wlmtk_menu_set_mode(pm_ptr->menu_ptr, WLMTK_MENU_MODE_NORMAL);
    wlmtk_menu_set_open(pm_ptr->menu_ptr, true);
}

/* ------------------------------------------------------------------------- */
void wlmaker_popup_menu_cancel(wlmaker_popup_menu_t *pm_ptr, void *ud_ptr)
{
    if (NULL == pm_ptr || NULL == ud_ptr || pm_ptr->ud_ptr != ud_ptr) return;
    pm_ptr->ud_ptr = NULL;
    pm_ptr->callback = NULL;
    wlmtk_menu_set_open(pm_ptr->menu_ptr, false);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Removes and destroys all items. */
void _clear(wlmaker_popup_menu_t *pm_ptr)
{
    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = pm_ptr->entries.head_ptr)) {
        wlmaker_popup_menu_entry_t *e_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_popup_menu_entry_t, dlnode);
        bs_dllist_remove(&pm_ptr->entries, dlnode_ptr);
        wlmtk_util_disconnect_listener(&e_ptr->triggered_listener);
        wlmtk_menu_remove_item(pm_ptr->menu_ptr, e_ptr->menu_item_ptr);
        wlmtk_menu_item_destroy(e_ptr->menu_item_ptr);
        free(e_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** An item was triggered: Close, then call back. */
void _handle_item_triggered(struct wl_listener *listener_ptr,
                            __UNUSED__ void *data_ptr)
{
    wlmaker_popup_menu_entry_t *e_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_popup_menu_entry_t, triggered_listener);
    wlmaker_popup_menu_t *pm_ptr = e_ptr->popup_menu_ptr;
    wlmaker_popup_menu_callback_t callback = pm_ptr->callback;
    void *ud_ptr = pm_ptr->ud_ptr;
    int id = e_ptr->id;
    pm_ptr->callback = NULL;
    pm_ptr->ud_ptr = NULL;
    wlmtk_menu_set_open(pm_ptr->menu_ptr, false);
    if (NULL != callback && NULL != ud_ptr) callback(ud_ptr, id);
}

/* ------------------------------------------------------------------------- */
/** The menu closed: Unmap its window. */
void _handle_menu_open_changed(struct wl_listener *listener_ptr,
                               __UNUSED__ void *data_ptr)
{
    wlmaker_popup_menu_t *pm_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_popup_menu_t, menu_open_changed_listener);
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(
        pm_ptr->window_ptr);
    if (!wlmtk_menu_is_open(pm_ptr->menu_ptr) && NULL != workspace_ptr) {
        pm_ptr->ud_ptr = NULL;
        wlmtk_workspace_unmap_window(workspace_ptr, pm_ptr->window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** The menu asks to be closed (eg. Escape). */
void _handle_menu_request_close(struct wl_listener *listener_ptr,
                                __UNUSED__ void *data_ptr)
{
    wlmaker_popup_menu_t *pm_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_popup_menu_t, menu_request_close_listener);
    wlmtk_menu_set_open(pm_ptr->menu_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** The window's close button. */
void _handle_window_request_close(struct wl_listener *listener_ptr,
                                  __UNUSED__ void *data_ptr)
{
    wlmaker_popup_menu_t *pm_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_popup_menu_t, window_request_close_listener);
    wlmtk_menu_set_open(pm_ptr->menu_ptr, false);
}

/* ------------------------------------------------------------------------- */
/** Keyboard focus follows the window's activation. */
void _handle_window_set_activated(struct wl_listener *listener_ptr,
                                  __UNUSED__ void *data_ptr)
{
    wlmaker_popup_menu_t *pm_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_popup_menu_t, window_set_activated_listener);
    wlmtk_element_t *e = wlmtk_menu_element(pm_ptr->menu_ptr);
    if (NULL != e->parent_container_ptr) {
        wlmtk_container_set_keyboard_focus_element(
            e->parent_container_ptr, e,
            wlmtk_window_is_activated(pm_ptr->window_ptr));
    }
}

/* == End of popup_menu.c ================================================== */
