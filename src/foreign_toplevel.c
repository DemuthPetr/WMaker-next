/* ========================================================================= */
/**
 * @file foreign_toplevel.c
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

#include "foreign_toplevel.h"

#include <libbase/libbase.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the foreign toplevel manager. */
struct _wlmaker_foreign_toplevel_t {
    /** Back-link to server. */
    wlmaker_server_t          *server_ptr;
    /** The wlroots manager. Destroyed with the display. */
    struct wlr_foreign_toplevel_manager_v1 *manager_ptr;
    /** Handles, @ref wlmaker_foreign_toplevel_handle_t::dlnode. */
    bs_dllist_t               handles;

    /** Listener for @ref wlmtk_root_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Listener for @ref wlmtk_root_events_t::window_unmapped. */
    struct wl_listener        window_unmapped_listener;
    /** Listener for @ref wlmtk_root_events_t::window_minimize_changed. */
    struct wl_listener        window_minimize_changed_listener;
};

/** A published window. */
typedef struct {
    /** Element of @ref wlmaker_foreign_toplevel_t::handles. */
    bs_dllist_node_t          dlnode;
    /** Back-link. */
    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr;
    /** The window. */
    wlmtk_window_t            *window_ptr;
    /** The protocol handle. */
    struct wlr_foreign_toplevel_handle_v1 *handle_ptr;

    /** Listener for @ref wlmtk_window_events_t::state_changed. */
    struct wl_listener        state_changed_listener;
    /** Listener for @ref wlmtk_window_events_t::set_activated. */
    struct wl_listener        set_activated_listener;

    /** Listener for the handle's `request_activate`. */
    struct wl_listener        request_activate_listener;
    /** Listener for the handle's `request_minimize`. */
    struct wl_listener        request_minimize_listener;
    /** Listener for the handle's `request_maximize`. */
    struct wl_listener        request_maximize_listener;
    /** Listener for the handle's `request_fullscreen`. */
    struct wl_listener        request_fullscreen_listener;
    /** Listener for the handle's `request_close`. */
    struct wl_listener        request_close_listener;
} wlmaker_foreign_toplevel_handle_t;

static wlmaker_foreign_toplevel_handle_t *_wlmaker_foreign_toplevel_find(
    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr,
    wlmtk_window_t *window_ptr);
static void _wlmaker_foreign_toplevel_handle_destroy(
    wlmaker_foreign_toplevel_handle_t *h_ptr);
static void _wlmaker_foreign_toplevel_handle_update(
    wlmaker_foreign_toplevel_handle_t *h_ptr);

static void _handle_window_mapped(struct wl_listener *l, void *data_ptr);
static void _handle_window_unmapped(struct wl_listener *l, void *data_ptr);
static void _handle_window_minimize_changed(struct wl_listener *l, void *d);
static void _handle_state_changed(struct wl_listener *l, void *data_ptr);
static void _handle_set_activated(struct wl_listener *l, void *data_ptr);
static void _handle_request_activate(struct wl_listener *l, void *data_ptr);
static void _handle_request_minimize(struct wl_listener *l, void *data_ptr);
static void _handle_request_maximize(struct wl_listener *l, void *data_ptr);
static void _handle_request_fullscreen(struct wl_listener *l, void *data_ptr);
static void _handle_request_close(struct wl_listener *l, void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_foreign_toplevel_t *wlmaker_foreign_toplevel_create(
    wlmaker_server_t *server_ptr)
{
    wlmaker_foreign_toplevel_t *ft_ptr = logged_calloc(
        1, sizeof(wlmaker_foreign_toplevel_t));
    if (NULL == ft_ptr) return NULL;
    ft_ptr->server_ptr = server_ptr;

    ft_ptr->manager_ptr = wlr_foreign_toplevel_manager_v1_create(
        server_ptr->wl_display_ptr);
    if (NULL == ft_ptr->manager_ptr) {
        wlmaker_foreign_toplevel_destroy(ft_ptr);
        return NULL;
    }

    wlmtk_root_events_t *events_ptr = wlmtk_root_events(server_ptr->root_ptr);
    wlmtk_util_connect_listener_signal(
        &events_ptr->window_mapped,
        &ft_ptr->window_mapped_listener,
        _handle_window_mapped);
    wlmtk_util_connect_listener_signal(
        &events_ptr->window_unmapped,
        &ft_ptr->window_unmapped_listener,
        _handle_window_unmapped);
    wlmtk_util_connect_listener_signal(
        &events_ptr->window_minimize_changed,
        &ft_ptr->window_minimize_changed_listener,
        _handle_window_minimize_changed);
    return ft_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_foreign_toplevel_destroy(
    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr)
{
    wlmtk_util_disconnect_listener(
        &foreign_toplevel_ptr->window_minimize_changed_listener);
    wlmtk_util_disconnect_listener(
        &foreign_toplevel_ptr->window_unmapped_listener);
    wlmtk_util_disconnect_listener(
        &foreign_toplevel_ptr->window_mapped_listener);

    bs_dllist_node_t *dlnode_ptr;
    while (NULL != (dlnode_ptr = foreign_toplevel_ptr->handles.head_ptr)) {
        _wlmaker_foreign_toplevel_handle_destroy(BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_foreign_toplevel_handle_t, dlnode));
    }
    // The manager is destroyed along with the display.
    free(foreign_toplevel_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return The handle for `window_ptr`, or NULL. */
wlmaker_foreign_toplevel_handle_t *_wlmaker_foreign_toplevel_find(
    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr,
    wlmtk_window_t *window_ptr)
{
    for (bs_dllist_node_t *dlnode_ptr = foreign_toplevel_ptr->handles.head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
            dlnode_ptr, wlmaker_foreign_toplevel_handle_t, dlnode);
        if (h_ptr->window_ptr == window_ptr) return h_ptr;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/** Destroys the protocol handle and our state. */
void _wlmaker_foreign_toplevel_handle_destroy(
    wlmaker_foreign_toplevel_handle_t *h_ptr)
{
    wlmtk_util_disconnect_listener(&h_ptr->request_close_listener);
    wlmtk_util_disconnect_listener(&h_ptr->request_fullscreen_listener);
    wlmtk_util_disconnect_listener(&h_ptr->request_maximize_listener);
    wlmtk_util_disconnect_listener(&h_ptr->request_minimize_listener);
    wlmtk_util_disconnect_listener(&h_ptr->request_activate_listener);
    wlmtk_util_disconnect_listener(&h_ptr->set_activated_listener);
    wlmtk_util_disconnect_listener(&h_ptr->state_changed_listener);
    if (NULL != h_ptr->handle_ptr) {
        wlr_foreign_toplevel_handle_v1_destroy(h_ptr->handle_ptr);
        h_ptr->handle_ptr = NULL;
    }
    bs_dllist_remove(&h_ptr->foreign_toplevel_ptr->handles, &h_ptr->dlnode);
    free(h_ptr);
}

/* ------------------------------------------------------------------------- */
/** Publishes the window's title, app ID and state. */
void _wlmaker_foreign_toplevel_handle_update(
    wlmaker_foreign_toplevel_handle_t *h_ptr)
{
    wlmtk_window_t *w = h_ptr->window_ptr;
    const char *title_ptr = wlmtk_window_get_title(w);
    const char *app_id_ptr = wlmtk_window_get_app_id(w);
    wlr_foreign_toplevel_handle_v1_set_title(
        h_ptr->handle_ptr, NULL != title_ptr ? title_ptr : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(
        h_ptr->handle_ptr, NULL != app_id_ptr ? app_id_ptr : "");
    wlr_foreign_toplevel_handle_v1_set_minimized(
        h_ptr->handle_ptr, wlmtk_window_is_minimized(w));
    wlr_foreign_toplevel_handle_v1_set_maximized(
        h_ptr->handle_ptr, wlmtk_window_is_maximized(w));
    wlr_foreign_toplevel_handle_v1_set_fullscreen(
        h_ptr->handle_ptr, wlmtk_window_is_fullscreen(w));
    wlr_foreign_toplevel_handle_v1_set_activated(
        h_ptr->handle_ptr, wlmtk_window_is_activated(w));
}

/* ------------------------------------------------------------------------- */
/** A window got mapped: Publish client windows (menus have no client). */
void _handle_window_mapped(struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_foreign_toplevel_t *ft_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_t, window_mapped_listener);
    wlmtk_window_t *window_ptr = data_ptr;

    const wlmtk_util_client_t *client_ptr = wlmtk_window_get_client_ptr(
        window_ptr);
    if (NULL == client_ptr || 0 == client_ptr->pid) return;
    if (NULL != _wlmaker_foreign_toplevel_find(ft_ptr, window_ptr)) return;
    // Not in the window list: Neither in the taskbar.
    if (wlmtk_window_get_attributes(window_ptr) &
        WLMTK_WINDOW_ATTR_SKIP_WINDOW_LIST) return;

    wlmaker_foreign_toplevel_handle_t *h_ptr = logged_calloc(
        1, sizeof(wlmaker_foreign_toplevel_handle_t));
    if (NULL == h_ptr) return;
    h_ptr->foreign_toplevel_ptr = ft_ptr;
    h_ptr->window_ptr = window_ptr;
    h_ptr->handle_ptr = wlr_foreign_toplevel_handle_v1_create(
        ft_ptr->manager_ptr);
    if (NULL == h_ptr->handle_ptr) {
        free(h_ptr);
        return;
    }
    bs_dllist_push_back(&ft_ptr->handles, &h_ptr->dlnode);

    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(window_ptr)->state_changed,
        &h_ptr->state_changed_listener, _handle_state_changed);
    wlmtk_util_connect_listener_signal(
        &wlmtk_window_events(window_ptr)->set_activated,
        &h_ptr->set_activated_listener, _handle_set_activated);
    wlmtk_util_connect_listener_signal(
        &h_ptr->handle_ptr->events.request_activate,
        &h_ptr->request_activate_listener, _handle_request_activate);
    wlmtk_util_connect_listener_signal(
        &h_ptr->handle_ptr->events.request_minimize,
        &h_ptr->request_minimize_listener, _handle_request_minimize);
    wlmtk_util_connect_listener_signal(
        &h_ptr->handle_ptr->events.request_maximize,
        &h_ptr->request_maximize_listener, _handle_request_maximize);
    wlmtk_util_connect_listener_signal(
        &h_ptr->handle_ptr->events.request_fullscreen,
        &h_ptr->request_fullscreen_listener, _handle_request_fullscreen);
    wlmtk_util_connect_listener_signal(
        &h_ptr->handle_ptr->events.request_close,
        &h_ptr->request_close_listener, _handle_request_close);

    _wlmaker_foreign_toplevel_handle_update(h_ptr);
}

/* ------------------------------------------------------------------------- */
/** A window got unmapped (closed, or moved to another workspace). */
void _handle_window_unmapped(struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_foreign_toplevel_t *ft_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_t, window_unmapped_listener);
    wlmaker_foreign_toplevel_handle_t *h_ptr = _wlmaker_foreign_toplevel_find(
        ft_ptr, data_ptr);
    if (NULL != h_ptr) _wlmaker_foreign_toplevel_handle_destroy(h_ptr);
}

/* ------------------------------------------------------------------------- */
/** Minimized or restored. */
void _handle_window_minimize_changed(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_foreign_toplevel_t *ft_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_t,
        window_minimize_changed_listener);
    wlmaker_foreign_toplevel_handle_t *h_ptr = _wlmaker_foreign_toplevel_find(
        ft_ptr, data_ptr);
    if (NULL != h_ptr) _wlmaker_foreign_toplevel_handle_update(h_ptr);
}

/* ------------------------------------------------------------------------- */
/** Title, app ID, maximized, fullscreen, ... changed. */
void _handle_state_changed(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        state_changed_listener);
    _wlmaker_foreign_toplevel_handle_update(h_ptr);
}

/* ------------------------------------------------------------------------- */
/** Activation changed. */
void _handle_set_activated(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        set_activated_listener);
    wlr_foreign_toplevel_handle_v1_set_activated(
        h_ptr->handle_ptr, wlmtk_window_is_activated(h_ptr->window_ptr));
}

/* ------------------------------------------------------------------------- */
/** Taskbar click: Switch to the window's workspace, restore, raise, focus. */
void _handle_request_activate(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        request_activate_listener);
    wlmtk_window_t *w = h_ptr->window_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(w);
    if (NULL == workspace_ptr) return;
    wlmtk_root_switch_to_workspace(
        h_ptr->foreign_toplevel_ptr->server_ptr->root_ptr, workspace_ptr);
    wlmtk_workspace_activate_window(workspace_ptr, w);  // Also restores.
    wlmtk_workspace_raise_window(workspace_ptr, w);
}

/* ------------------------------------------------------------------------- */
/** Minimize or restore. */
void _handle_request_minimize(struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        request_minimize_listener);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event_ptr = data_ptr;
    wlmtk_window_t *w = h_ptr->window_ptr;
    if (event_ptr->minimized) {
        wlmtk_window_request_minimize(w);
    } else if (NULL != wlmtk_window_get_workspace(w)) {
        wlmtk_workspace_restore_window(wlmtk_window_get_workspace(w), w);
    }
}

/* ------------------------------------------------------------------------- */
/** Maximize or unmaximize. */
void _handle_request_maximize(struct wl_listener *listener_ptr, void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        request_maximize_listener);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event_ptr = data_ptr;
    wlmtk_window_request_maximized(h_ptr->window_ptr, event_ptr->maximized);
}

/* ------------------------------------------------------------------------- */
/** Fullscreen on or off. */
void _handle_request_fullscreen(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        request_fullscreen_listener);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event_ptr =
        data_ptr;
    wlmtk_window_request_fullscreen(h_ptr->window_ptr, event_ptr->fullscreen);
}

/* ------------------------------------------------------------------------- */
/** Close. */
void _handle_request_close(
    struct wl_listener *listener_ptr,
    __UNUSED__ void *data_ptr)
{
    wlmaker_foreign_toplevel_handle_t *h_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_foreign_toplevel_handle_t,
        request_close_listener);
    wlmtk_window_request_close(h_ptr->window_ptr);
}

/* == End of foreign_toplevel.c ============================================ */
