/* ========================================================================= */
/**
 * @file window_rules.c
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

#define _GNU_SOURCE  // FNM_CASEFOLD
#include "window_rules.h"

#include <fnmatch.h>
#include <libbase/libbase.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-core.h>

#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the rules. */
struct _wlmaker_window_rules_t {
    /** Back-link to the server. */
    wlmaker_server_t          *server_ptr;
    /** The WindowAttributes dict: Application pattern -> attributes. */
    bspl_dict_t               *rules_dict_ptr;
    /** Listener for @ref wlmtk_root_events_t::window_mapped. */
    struct wl_listener        window_mapped_listener;
};

/** Value of a boolean attribute: Unset, or set to false or true. */
typedef enum { _UNSET = -1, _NO = 0, _YES = 1 } _tristate_t;

/** The attributes for one window, merged from all rules that match it. */
typedef struct {
    /** Bits of @ref wlmtk_window_attribute_t that are set. */
    uint32_t                  attributes;
    /** Bits of @ref wlmtk_window_attribute_t that rules mention. */
    uint32_t                  attributes_set;
    /** KeepOnTop. */
    _tristate_t               keep_on_top;
    /** KeepAtBottom. */
    _tristate_t               keep_at_bottom;
    /** Omnipresent: On all workspaces. */
    _tristate_t               omnipresent;
    /** StartMaximized. */
    _tristate_t               start_maximized;
    /** StartMiniaturized. */
    _tristate_t               start_miniaturized;
    /** StartCentered. */
    _tristate_t               start_centered;
    /** StartWorkspace, 1-based; 0 for none. */
    int                       start_workspace;
} _wlmaker_window_attrs_t;

/** Attribute names, and their @ref wlmtk_window_attribute_t bit. */
static const struct { const char *name_ptr; uint32_t bit; } _attribute_bits[] = {
    { "NoTitleBar", WLMTK_WINDOW_ATTR_NO_TITLEBAR },
    { "NoResizeBar", WLMTK_WINDOW_ATTR_NO_RESIZEBAR },
    { "NoBorder", WLMTK_WINDOW_ATTR_NO_BORDER },
    { "NoCloseButton", WLMTK_WINDOW_ATTR_NO_CLOSE_BUTTON },
    { "NoMiniaturizeButton", WLMTK_WINDOW_ATTR_NO_MINIATURIZE_BUTTON },
    { "SkipWindowList", WLMTK_WINDOW_ATTR_SKIP_WINDOW_LIST },
    { "SkipSwitchPanel", WLMTK_WINDOW_ATTR_SKIP_SWITCHER },
    { "Unfocusable", WLMTK_WINDOW_ATTR_NO_FOCUS },
};

static void _wlmaker_window_rules_handle_window_mapped(
    struct wl_listener *listener_ptr,
    void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_window_rules_t *wlmaker_window_rules_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    wlmaker_window_rules_t *rules_ptr = logged_calloc(1, sizeof(*rules_ptr));
    if (NULL == rules_ptr) return NULL;
    rules_ptr->server_ptr = server_ptr;
    rules_ptr->rules_dict_ptr = bspl_dict_ref(
        bspl_dict_get_dict(config_dict_ptr, "WindowAttributes"));
    wlmtk_util_connect_listener_signal(
        &wlmtk_root_events(server_ptr->root_ptr)->window_mapped,
        &rules_ptr->window_mapped_listener,
        _wlmaker_window_rules_handle_window_mapped);
    return rules_ptr;
}

/* ------------------------------------------------------------------------- */
void wlmaker_window_rules_destroy(wlmaker_window_rules_t *rules_ptr)
{
    wlmtk_util_disconnect_listener(&rules_ptr->window_mapped_listener);
    if (NULL != rules_ptr->rules_dict_ptr) {
        bspl_dict_unref(rules_ptr->rules_dict_ptr);
    }
    free(rules_ptr);
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** @return the boolean value of a dict entry: Unset if absent. */
static _tristate_t _bool_value(bspl_dict_t *dict_ptr, const char *key_ptr)
{
    const char *v = bspl_dict_get_string_value(dict_ptr, key_ptr);
    if (NULL == v) return _UNSET;
    return (0 == strcasecmp(v, "true") || 0 == strcasecmp(v, "yes") ||
            0 == strcasecmp(v, "on") || 0 == strcasecmp(v, "enabled") ||
            0 == strcmp(v, "1")) ? _YES : _NO;
}

/* ------------------------------------------------------------------------- */
/** Applies the attributes of one rule over `attrs_ptr`. */
static void _merge_rule(bspl_dict_t *dict_ptr, _wlmaker_window_attrs_t *attrs_ptr)
{
    for (size_t i = 0; i < sizeof(_attribute_bits) / sizeof(_attribute_bits[0]);
         ++i) {
        _tristate_t v = _bool_value(dict_ptr, _attribute_bits[i].name_ptr);
        if (_UNSET == v) continue;
        attrs_ptr->attributes_set |= _attribute_bits[i].bit;
        if (_YES == v) {
            attrs_ptr->attributes |= _attribute_bits[i].bit;
        } else {
            attrs_ptr->attributes &= ~_attribute_bits[i].bit;
        }
    }
    struct { const char *name_ptr; _tristate_t *value_ptr; } bools[] = {
        { "KeepOnTop", &attrs_ptr->keep_on_top },
        { "KeepAtBottom", &attrs_ptr->keep_at_bottom },
        { "Omnipresent", &attrs_ptr->omnipresent },
        { "StartMaximized", &attrs_ptr->start_maximized },
        { "StartMiniaturized", &attrs_ptr->start_miniaturized },
        { "StartCentered", &attrs_ptr->start_centered },
    };
    for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); ++i) {
        _tristate_t v = _bool_value(dict_ptr, bools[i].name_ptr);
        if (_UNSET != v) *bools[i].value_ptr = v;
    }
    const char *ws_ptr = bspl_dict_get_string_value(dict_ptr, "StartWorkspace");
    if (NULL != ws_ptr) attrs_ptr->start_workspace = atoi(ws_ptr);
}

/** Argument for @ref _match_iterator. */
typedef struct {
    /** Application ID to match. */
    const char                *app_id_ptr;
    /** Pass: 0 for "*", 1 for patterns, 2 for exact names. */
    int                       pass;
    /** Merged attributes. */
    _wlmaker_window_attrs_t   *attrs_ptr;
} _match_arg_t;

/* ------------------------------------------------------------------------- */
/** Merges the rule `key_ptr` if it matches, in this pass. */
static bool _match_iterator(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *ud_ptr)
{
    _match_arg_t *arg_ptr = ud_ptr;
    bspl_dict_t *dict_ptr = bspl_dict_from_object(object_ptr);
    if (NULL == dict_ptr) return true;
    bool is_default = 0 == strcmp(key_ptr, "*");
    bool is_pattern = !is_default && NULL != strpbrk(key_ptr, "*?[");
    bool match = false;
    switch (arg_ptr->pass) {
    case 0: match = is_default; break;
    case 1:
        match = is_pattern &&
            0 == fnmatch(key_ptr, arg_ptr->app_id_ptr, FNM_CASEFOLD);
        break;
    default:
        match = !is_default && !is_pattern &&
            0 == strcasecmp(key_ptr, arg_ptr->app_id_ptr);
        break;
    }
    if (match) _merge_rule(dict_ptr, arg_ptr->attrs_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** @return the window's merged attributes: "*", then patterns, then exact. */
static _wlmaker_window_attrs_t _attrs_for(
    wlmaker_window_rules_t *rules_ptr,
    wlmtk_window_t *window_ptr)
{
    _wlmaker_window_attrs_t attrs = {
        .keep_on_top = _UNSET, .keep_at_bottom = _UNSET,
        .omnipresent = _UNSET, .start_maximized = _UNSET,
        .start_miniaturized = _UNSET, .start_centered = _UNSET };
    if (NULL == rules_ptr->rules_dict_ptr) return attrs;
    const char *app_id_ptr = wlmtk_window_get_app_id(window_ptr);
    _match_arg_t arg = {
        .app_id_ptr = NULL != app_id_ptr ? app_id_ptr : "",
        .attrs_ptr = &attrs };
    for (arg.pass = 0; arg.pass < 3; ++arg.pass) {
        bspl_dict_foreach(rules_ptr->rules_dict_ptr, _match_iterator, &arg);
    }
    return attrs;
}

/* ------------------------------------------------------------------------- */
/** Applies the lasting attributes: Decoration, lists, focus, layers. */
static void _apply_lasting(
    wlmtk_window_t *window_ptr,
    const _wlmaker_window_attrs_t *attrs_ptr)
{
    wlmtk_window_set_attributes(window_ptr, attrs_ptr->attributes);
    if (_UNSET != attrs_ptr->keep_on_top) {
        wlmtk_window_set_keep_on_top(window_ptr, _YES == attrs_ptr->keep_on_top);
    }
    if (_UNSET != attrs_ptr->keep_at_bottom) {
        wlmtk_window_set_keep_at_bottom(
            window_ptr, _YES == attrs_ptr->keep_at_bottom);
    }
    if (_UNSET != attrs_ptr->omnipresent) {
        wlmtk_window_set_omnipresent(window_ptr, _YES == attrs_ptr->omnipresent);
    }
}

/* ------------------------------------------------------------------------- */
/** Applies the rules to a window being mapped. */
void _wlmaker_window_rules_handle_window_mapped(
    struct wl_listener *listener_ptr,
    void *data_ptr)
{
    wlmaker_window_rules_t *rules_ptr = BS_CONTAINER_OF(
        listener_ptr, wlmaker_window_rules_t, window_mapped_listener);
    wlmtk_window_t *window_ptr = data_ptr;
    // Application windows only: Not menus & other internal windows.
    if (!wlmtk_window_is_iconifiable(window_ptr)) return;

    _wlmaker_window_attrs_t attrs = _attrs_for(rules_ptr, window_ptr);
    _apply_lasting(window_ptr, &attrs);

    // Start attributes: Once per window (not when moved to a workspace).
    if (wlmtk_window_rules_applied(window_ptr)) return;
    wlmtk_window_set_rules_applied(window_ptr, true);

    wlmtk_workspace_t *workspace_ptr = wlmtk_window_get_workspace(window_ptr);
    if (0 < attrs.start_workspace) {
        wlmtk_workspace_t *target_ptr = wlmtk_root_get_workspace(
            rules_ptr->server_ptr->root_ptr, attrs.start_workspace - 1);
        if (NULL != target_ptr && target_ptr != workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
            wlmtk_workspace_map_window(target_ptr, window_ptr);
            workspace_ptr = target_ptr;
        }
    }
    if (_YES == attrs.start_centered) {
        wlmtk_workspace_center_window(workspace_ptr, window_ptr);
    }
    if (_YES == attrs.start_maximized) {
        wlmtk_window_request_maximized(window_ptr, true);
    }
    if (_YES == attrs.start_miniaturized) {
        wlmtk_window_request_minimize(window_ptr);
    }
}

/* ------------------------------------------------------------------------- */
/** Re-applies lasting attributes to a window of a workspace. */
static void _reapply_window(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
    if (!wlmtk_window_is_iconifiable(window_ptr)) return;
    _wlmaker_window_attrs_t attrs = _attrs_for(ud_ptr, window_ptr);
    _apply_lasting(window_ptr, &attrs);
}

/* ------------------------------------------------------------------------- */
/** Re-applies lasting attributes to the windows of a workspace. */
static void _reapply_workspace(bs_dllist_node_t *dlnode_ptr, void *ud_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_workspace_from_dlnode(dlnode_ptr);
    bs_dllist_for_each(wlmtk_workspace_get_windows_dllist(workspace_ptr),
                       _reapply_window, ud_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_window_rules_set_config(
    wlmaker_window_rules_t *rules_ptr,
    bspl_dict_t *config_dict_ptr)
{
    bspl_dict_t *new_ptr = bspl_dict_ref(
        bspl_dict_get_dict(config_dict_ptr, "WindowAttributes"));
    if (NULL != rules_ptr->rules_dict_ptr) {
        bspl_dict_unref(rules_ptr->rules_dict_ptr);
    }
    rules_ptr->rules_dict_ptr = new_ptr;
    wlmtk_root_for_each_workspace(rules_ptr->server_ptr->root_ptr,
                                  _reapply_workspace, rules_ptr);
}

/* == End of window_rules.c ================================================ */
