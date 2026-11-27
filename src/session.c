/* ========================================================================= */
/**
 * @file session.c
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

#include "session.h"

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_output_layout.h>
#undef WLR_USE_UNSTABLE

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** A saved window. */
typedef struct {
    /** Node of @ref wlmaker_session_t::pending. */
    bs_dllist_node_t          dlnode;
    /** Application ID. */
    char                      *app_id_ptr;
    /** Title, for reference. */
    char                      *title_ptr;
    /** Workspace, 1-based. */
    int64_t                   workspace;
    /** Position. */
    int64_t                   x, y;
    /** Size. */
    int64_t                   width, height;
    /** States. */
    bool                      maximized, minimized;
} _wlmaker_session_window_t;

/** Session state. */
struct _wlmaker_session_t {
    /** Back-link. */
    wlmaker_server_t          *server_ptr;
    /** Saved windows not put back yet. @ref _wlmaker_session_window_t. */
    bs_dllist_t               pending;
    /** Listener for @ref wlmtk_root_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
    /** Ends the restoring: Windows mapped later are new ones. */
    struct wl_event_source    *expire_timer_ptr;
};

static char *_wlmaker_session_path(void);
static void _wlmaker_session_load(wlmaker_session_t *session_ptr);
static void _wlmaker_session_window_destroy(
    bs_dllist_node_t *dlnode_ptr, void *ud_ptr);
static void _wlmaker_session_handle_window_mapped(
    struct wl_listener *listener_ptr, void *data_ptr);
static int _wlmaker_session_handle_expire(void *data_ptr);

/* == Data ================================================================= */

/** Descriptor of a saved window. */
static const bspl_desc_t _wlmaker_session_window_desc[] = {
    BSPL_DESC_STRING("AppId", true, _wlmaker_session_window_t,
                     app_id_ptr, app_id_ptr, ""),
    BSPL_DESC_STRING("Title", false, _wlmaker_session_window_t,
                     title_ptr, title_ptr, ""),
    BSPL_DESC_INT64("Workspace", false, _wlmaker_session_window_t,
                    workspace, workspace, 0),
    BSPL_DESC_INT64("X", false, _wlmaker_session_window_t, x, x, 0),
    BSPL_DESC_INT64("Y", false, _wlmaker_session_window_t, y, y, 0),
    BSPL_DESC_INT64("Width", false, _wlmaker_session_window_t,
                    width, width, 0),
    BSPL_DESC_INT64("Height", false, _wlmaker_session_window_t,
                    height, height, 0),
    BSPL_DESC_BOOL("Maximized", false, _wlmaker_session_window_t,
                   maximized, maximized, false),
    BSPL_DESC_BOOL("Minimized", false, _wlmaker_session_window_t,
                   minimized, minimized, false),
    BSPL_DESC_SENTINEL()
};

/** Windows mapped this long after the start are new ones, not restored. */
static const int _wlmaker_session_restore_ms = 3 * 60 * 1000;

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_session_t *wlmaker_session_create(wlmaker_server_t *server_ptr)
{
    wlmaker_session_t *session_ptr = logged_calloc(1, sizeof(*session_ptr));
    if (NULL == session_ptr) return NULL;
    session_ptr->server_ptr = server_ptr;

    _wlmaker_session_load(session_ptr);
    if (0 < bs_dllist_size(&session_ptr->pending)) {
        wlmtk_util_connect_listener_signal(
            &wlmtk_root_events(server_ptr->root_ptr)->window_mapped,
            &session_ptr->window_mapped_listener,
            _wlmaker_session_handle_window_mapped);
        session_ptr->expire_timer_ptr = wl_event_loop_add_timer(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            _wlmaker_session_handle_expire, session_ptr);
        if (NULL != session_ptr->expire_timer_ptr) {
            wl_event_source_timer_update(session_ptr->expire_timer_ptr,
                                         _wlmaker_session_restore_ms);
        }
    }
    return session_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_session_destroy(wlmaker_session_t *session_ptr)
{
    if (NULL != session_ptr->expire_timer_ptr) {
        wl_event_source_remove(session_ptr->expire_timer_ptr);
        session_ptr->expire_timer_ptr = NULL;
    }
    wlmtk_util_disconnect_listener(&session_ptr->window_mapped_listener);
    bs_dllist_for_each(&session_ptr->pending,
                       _wlmaker_session_window_destroy, NULL);
    free(session_ptr);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_session_save_on_exit(wlmaker_session_t *session_ptr)
{
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(
        session_ptr->server_ptr->config_dict_ptr, "Session");
    if (NULL == dict_ptr) return false;
    struct { bool save_on_exit; } c = {};
    const bspl_desc_t desc[] = {
        BSPL_DESC_BOOL("SaveOnExit", false, __typeof__(c), save_on_exit,
                       save_on_exit, false),
        BSPL_DESC_SENTINEL()
    };
    return bspl_decode_dict(dict_ptr, desc, &c) && c.save_on_exit;
}

/* ------------------------------------------------------------------------- */
/** Writes a plist string, quoted and escaped. */
static void _wlmaker_session_write_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; NULL != s && '\0' != *s; ++s) {
        if ('"' == *s || '\\' == *s) fputc('\\', f);
        if ('\n' == *s) {
            fputs("\\n", f);
            continue;
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

/* ------------------------------------------------------------------------- */
bool wlmaker_session_save(wlmaker_session_t *session_ptr)
{
    char *path_ptr = _wlmaker_session_path();
    if (NULL == path_ptr) return false;
    char *tmp_ptr = bs_strdupf("%s.tmp", path_ptr);
    FILE *f = (NULL != tmp_ptr) ? fopen(tmp_ptr, "w") : NULL;
    if (NULL == f) {
        bs_log(BS_WARNING | BS_ERRNO, "Failed to write session %s", path_ptr);
        free(tmp_ptr);
        free(path_ptr);
        return false;
    }
    fprintf(f, "// Written by wlmaker: Windows of the last session.\n"
            "{\n  Windows = (\n");

    int n = 0, index = 0;
    for (bs_dllist_node_t *wsdl_ptr = wlmtk_root_get_workspaces(
             session_ptr->server_ptr->root_ptr)->head_ptr;
         NULL != wsdl_ptr;
         wsdl_ptr = wsdl_ptr->next_ptr, ++index) {
        wlmtk_workspace_t *ws_ptr = wlmtk_workspace_from_dlnode(wsdl_ptr);
        for (bs_dllist_node_t *wdl_ptr =
                 wlmtk_workspace_get_windows_dllist(ws_ptr)->head_ptr;
             NULL != wdl_ptr;
             wdl_ptr = wdl_ptr->next_ptr) {
            wlmtk_window_t *w = wlmtk_window_from_dlnode(wdl_ptr);
            const char *app_id_ptr = wlmtk_window_get_app_id(w);
            if (!wlmtk_window_is_iconifiable(w) || NULL == app_id_ptr ||
                '\0' == *app_id_ptr) continue;
            int x, y;
            wlmtk_element_get_position(wlmtk_window_element(w), &x, &y);
            struct wlr_box box = wlmtk_window_get_size(w);
            fprintf(f, "%s    {\n      AppId = ", n ? ",\n" : "");
            _wlmaker_session_write_string(f, app_id_ptr);
            fprintf(f, ";\n      Title = ");
            _wlmaker_session_write_string(f, wlmtk_window_get_title(w));
            fprintf(f, ";\n      Workspace = \"%d\";\n"
                    "      X = \"%d\";\n      Y = \"%d\";\n"
                    "      Width = \"%d\";\n      Height = \"%d\";\n"
                    "      Maximized = %s;\n      Minimized = %s;\n    }",
                    index + 1, x, y, box.width, box.height,
                    wlmtk_window_is_maximized(w) ? "True" : "False",
                    wlmtk_window_is_minimized(w) ? "True" : "False");
            ++n;
        }
    }
    fprintf(f, "%s  );\n}\n", n ? "\n" : "");
    bool rv = (0 == fclose(f)) && (0 == rename(tmp_ptr, path_ptr));
    if (rv) {
        bs_log(BS_INFO, "Saved %d windows to session %s", n, path_ptr);
    } else {
        bs_log(BS_WARNING | BS_ERRNO, "Failed to write session %s", path_ptr);
    }
    free(tmp_ptr);
    free(path_ptr);
    return rv;
}

/* ------------------------------------------------------------------------- */
void wlmaker_session_clear(wlmaker_session_t *session_ptr)
{
    char *path_ptr = _wlmaker_session_path();
    if (NULL != path_ptr) {
        unlink(path_ptr);
        free(path_ptr);
    }
    bs_dllist_for_each(&session_ptr->pending,
                       _wlmaker_session_window_destroy, NULL);
    session_ptr->pending = (bs_dllist_t){};
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Path to `Session.plist`: In $XDG_CONFIG_HOME/wlmaker (~/.config/wlmaker). */
char *_wlmaker_session_path(void)
{
    const char *config_ptr = getenv("XDG_CONFIG_HOME");
    if (NULL != config_ptr && '\0' != *config_ptr) {
        return bs_strdupf("%s/wlmaker/Session.plist", config_ptr);
    }
    const char *home_ptr = getenv("HOME");
    if (NULL == home_ptr) return NULL;
    return bs_strdupf("%s/.config/wlmaker/Session.plist", home_ptr);
}

/* ------------------------------------------------------------------------- */
/** Loads the saved windows into @ref wlmaker_session_t::pending. */
void _wlmaker_session_load(wlmaker_session_t *session_ptr)
{
    char *path_ptr = _wlmaker_session_path();
    if (NULL == path_ptr) return;
    if (0 != access(path_ptr, R_OK)) {
        free(path_ptr);
        return;
    }
    bspl_object_t *obj_ptr = bspl_create_object_from_plist_file(path_ptr);
    free(path_ptr);
    bspl_dict_t *dict_ptr = bspl_dict_from_object(obj_ptr);
    bspl_array_t *array_ptr = (NULL != dict_ptr) ?
        bspl_dict_get_array(dict_ptr, "Windows") : NULL;
    for (size_t i = 0; NULL != array_ptr && i < bspl_array_size(array_ptr); ++i) {
        bspl_dict_t *w_ptr = bspl_dict_from_object(bspl_array_at(array_ptr, i));
        _wlmaker_session_window_t *sw_ptr = logged_calloc(1, sizeof(*sw_ptr));
        if (NULL == sw_ptr) break;
        if (NULL == w_ptr ||
            !bspl_decode_dict(w_ptr, _wlmaker_session_window_desc, sw_ptr)) {
            bs_log(BS_WARNING, "Skipping invalid window %zu of the session", i);
            _wlmaker_session_window_destroy(&sw_ptr->dlnode, NULL);
            continue;
        }
        bs_dllist_push_back(&session_ptr->pending, &sw_ptr->dlnode);
    }
    if (NULL != obj_ptr) bspl_object_unref(obj_ptr);
    if (0 < bs_dllist_size(&session_ptr->pending)) {
        bs_log(BS_INFO, "Session: %zu windows to put back.",
               bs_dllist_size(&session_ptr->pending));
    }
}

/* ------------------------------------------------------------------------- */
/** Dtor for a @ref _wlmaker_session_window_t. */
void _wlmaker_session_window_destroy(
    bs_dllist_node_t *dlnode_ptr,
    __UNUSED__ void *ud_ptr)
{
    _wlmaker_session_window_t *sw_ptr = BS_CONTAINER_OF(
        dlnode_ptr, _wlmaker_session_window_t, dlnode);
    bspl_decoded_destroy(_wlmaker_session_window_desc, sw_ptr);
    free(sw_ptr);
}

/* ------------------------------------------------------------------------- */
/** Matches a saved window by its app ID. */
static bool _wlmaker_session_window_matches(
    bs_dllist_node_t *dlnode_ptr,
    void *ud_ptr)
{
    _wlmaker_session_window_t *sw_ptr = BS_CONTAINER_OF(
        dlnode_ptr, _wlmaker_session_window_t, dlnode);
    return 0 == strcmp(sw_ptr->app_id_ptr, (const char *)ud_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * A window mapped: If it belongs to a saved application, puts it back where
 * the first not-yet-restored window of that application was.
 */
void _wlmaker_session_handle_window_mapped(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_session_t *session_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_session_t, window_mapped_listener);
    wlmtk_window_t *window_ptr = data_ptr;
    const char *app_id_ptr = wlmtk_window_get_app_id(window_ptr);
    if (!wlmtk_window_is_iconifiable(window_ptr) || NULL == app_id_ptr) return;

    bs_dllist_node_t *dlnode_ptr = bs_dllist_find(
        &session_ptr->pending, _wlmaker_session_window_matches,
        (void *)app_id_ptr);
    if (NULL == dlnode_ptr) return;
    bs_dllist_remove(&session_ptr->pending, dlnode_ptr);
    _wlmaker_session_window_t *sw_ptr = BS_CONTAINER_OF(
        dlnode_ptr, _wlmaker_session_window_t, dlnode);

    wlmtk_root_t *root_ptr = session_ptr->server_ptr->root_ptr;
    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    wlmtk_workspace_t *target_ptr = (0 < sw_ptr->workspace) ?
        wlmtk_root_get_workspace(root_ptr, sw_ptr->workspace - 1) : NULL;
    if (NULL != target_ptr && NULL != workspace_ptr &&
        target_ptr != workspace_ptr) {
        wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
        wlmtk_workspace_map_window(target_ptr, window_ptr);
        workspace_ptr = target_ptr;
    }
    // Only if the spot is still on a display.
    if (NULL != workspace_ptr && 0 < sw_ptr->width && 0 < sw_ptr->height &&
        wlr_output_layout_contains_point(
            session_ptr->server_ptr->wlr_output_layout_ptr, NULL,
            sw_ptr->x + sw_ptr->width / 2, sw_ptr->y + 10)) {
        struct wlr_box box = {
            .width = sw_ptr->width, .height = sw_ptr->height };
        wlmtk_window_request_size(window_ptr, &box);
        wlmtk_workspace_set_window_position(
            workspace_ptr, window_ptr, sw_ptr->x, sw_ptr->y);
    }
    if (sw_ptr->maximized) wlmtk_window_request_maximized(window_ptr, true);
    if (sw_ptr->minimized) wlmtk_window_request_minimize(window_ptr);

    _wlmaker_session_window_destroy(dlnode_ptr, NULL);
    if (0 == bs_dllist_size(&session_ptr->pending)) {
        wlmtk_util_disconnect_listener(&session_ptr->window_mapped_listener);
    }
}

/* ------------------------------------------------------------------------- */
/** Restoring is over: Forgets the windows not put back. */
int _wlmaker_session_handle_expire(void *data_ptr)
{
    wlmaker_session_t *session_ptr = data_ptr;
    bs_dllist_for_each(&session_ptr->pending,
                       _wlmaker_session_window_destroy, NULL);
    session_ptr->pending = (bs_dllist_t){};
    wlmtk_util_disconnect_listener(&session_ptr->window_mapped_listener);
    return 0;
}

/* == End of session.c ===================================================== */
