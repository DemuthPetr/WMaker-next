/* ========================================================================= */
/**
 * @file action.c
 *
 * @copyright
 * Copyright (c) 2026 Philipp Kaeser (kaeser@gubbe.ch)
 * Copyright 2024 Google LLC
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

#include "action.h"

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/edges.h>
#undef WLR_USE_UNSTABLE

#include "backend/backend.h"
#include "background.h"
#include "config.h"
#include "default_configuration.h"
#include "idle.h"
#include "input/keyboard.h"
#include "input/manager.h"
#include "root_menu.h"
#include "server.h"
#include "session.h"
#include "subprocess_monitor.h"
#include "toolkit/toolkit.h"

/* == Declarations ========================================================= */

/** State of the bound actions. */
struct _wlmaker_action_handle_t {
    /** Bindings, linked by @ref _wlmaker_action_binding_t::qnode. */
    bs_dequeue_t              bindings;
    /** Back-link to server state. */
    wlmaker_server_t          *server_ptr;
    /** Whether to add 'Logo' to the bindings. */
    bool                      add_logo;
};

/** Key binding for a standard action. */
typedef struct {
    /** Node of @ref wlmaker_action_handle_t::bindings. */
    bs_dequeue_node_t         qnode;
    /** The key binding. */
    struct wlmim_keybinding_combo combo;
    /** The associated action. */
    wlmaker_action_t          action;
    /** Optional argument for the action (eg. command to execute, NULL when unset). */
    char                      *action_arg_ptr;
    /** The key binding it to this node. */
    wlmim_keybinding_t        *keybinding_ptr;
    /** State of the bound actions. */
    wlmaker_action_handle_t   *handle_ptr;
} _wlmaker_action_binding_t;

static bool _wlmaker_keybindings_parse(
    const char *string_ptr,
    uint32_t *modifiers_ptr,
    xkb_keysym_t *keysym_ptr);

static bool _wlmaker_keybindings_bind_item(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr);

static bool _wlmaker_action_bound_callback(
    const struct wlmim_keybinding_combo *binding_ptr);

static bool _wlmaker_action_same_application(
    wlmtk_window_t *window1_ptr,
    wlmtk_window_t *window2_ptr);
static void _wlmaker_action_hide(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *app_window_ptr,
    bool others);
static void _wlmaker_action_show_desktop(wlmaker_server_t *server_ptr);
static void _wlmaker_action_window(
    wlmtk_workspace_t *workspace_ptr,
    wlmtk_window_t *window_ptr,
    wlmaker_action_t action);

/* == Data ================================================================= */

/** Key to lookup the dict from the config dictionary. */
const char *wlmaker_action_config_dict_key = "KeyBindings";

/** The actions that can be bound. */
const bspl_enum_desc_t wlmaker_action_desc[] = {
    BSPL_ENUM("None", WLMAKER_ACTION_NONE),
    BSPL_ENUM("Quit", WLMAKER_ACTION_QUIT),
    BSPL_ENUM("LockScreen", WLMAKER_ACTION_LOCK_SCREEN),
    BSPL_ENUM("InhibitLockBegin", WLMAKER_ACTION_LOCK_INHIBIT_BEGIN),
    BSPL_ENUM("InhibitLockEnd", WLMAKER_ACTION_LOCK_INHIBIT_END),
    BSPL_ENUM("LaunchTerminal", WLMAKER_ACTION_LAUNCH_TERMINAL),
    BSPL_ENUM("ShellExecute", WLMAKER_ACTION_SHELL_EXECUTE),
    BSPL_ENUM("Execute", WLMAKER_ACTION_EXECUTE),

    BSPL_ENUM("WorkspacePrevious", WLMAKER_ACTION_WORKSPACE_TO_PREVIOUS),
    BSPL_ENUM("WorkspaceNext", WLMAKER_ACTION_WORKSPACE_TO_NEXT),
    BSPL_ENUM("WorkspaceAdd", WLMAKER_ACTION_WORKSPACE_ADD),
    BSPL_ENUM("Workspace1", WLMAKER_ACTION_WORKSPACE_1),
    BSPL_ENUM("Workspace2", WLMAKER_ACTION_WORKSPACE_2),
    BSPL_ENUM("Workspace3", WLMAKER_ACTION_WORKSPACE_3),
    BSPL_ENUM("Workspace4", WLMAKER_ACTION_WORKSPACE_4),
    BSPL_ENUM("Workspace5", WLMAKER_ACTION_WORKSPACE_5),
    BSPL_ENUM("Workspace6", WLMAKER_ACTION_WORKSPACE_6),
    BSPL_ENUM("Workspace7", WLMAKER_ACTION_WORKSPACE_7),
    BSPL_ENUM("Workspace8", WLMAKER_ACTION_WORKSPACE_8),
    BSPL_ENUM("Workspace9", WLMAKER_ACTION_WORKSPACE_9),
    BSPL_ENUM("Workspace10", WLMAKER_ACTION_WORKSPACE_10),
    BSPL_ENUM("WindowToWorkspace1", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_1),
    BSPL_ENUM("WindowToWorkspace2", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_2),
    BSPL_ENUM("WindowToWorkspace3", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_3),
    BSPL_ENUM("WindowToWorkspace4", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_4),
    BSPL_ENUM("WindowToWorkspace5", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_5),
    BSPL_ENUM("WindowToWorkspace6", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_6),
    BSPL_ENUM("WindowToWorkspace7", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_7),
    BSPL_ENUM("WindowToWorkspace8", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_8),
    BSPL_ENUM("WindowToWorkspace9", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_9),
    BSPL_ENUM("WindowToWorkspace10", WLMAKER_ACTION_WINDOW_TO_WORKSPACE_10),
    BSPL_ENUM("WorkspaceDestroyLast", WLMAKER_ACTION_WORKSPACE_DESTROY_LAST),

    BSPL_ENUM("TaskPrevious", WLMAKER_ACTION_TASK_TO_PREVIOUS),
    BSPL_ENUM("TaskNext", WLMAKER_ACTION_TASK_TO_NEXT),

    BSPL_ENUM("WindowRaise", WLMAKER_ACTION_WINDOW_RAISE),
    BSPL_ENUM("WindowLower", WLMAKER_ACTION_WINDOW_LOWER),
    BSPL_ENUM("WindowToggleFullscreen", WLMAKER_ACTION_WINDOW_TOGGLE_FULLSCREEN),
    BSPL_ENUM("WindowToggleMaximized", WLMAKER_ACTION_WINDOW_TOGGLE_MAXIMIZED),

    BSPL_ENUM("WindowMaximize", WLMAKER_ACTION_WINDOW_MAXIMIZE),
    BSPL_ENUM("WindowUnmaximize", WLMAKER_ACTION_WINDOW_UNMAXIMIZE),
    BSPL_ENUM("WindowFullscreen", WLMAKER_ACTION_WINDOW_FULLSCREEN),
    BSPL_ENUM("WindowMinimize", WLMAKER_ACTION_WINDOW_MINIMIZE),
    BSPL_ENUM("HideApplication", WLMAKER_ACTION_HIDE_APPLICATION),
    BSPL_ENUM("HideOthers", WLMAKER_ACTION_HIDE_OTHERS),
    BSPL_ENUM("ShowDesktop", WLMAKER_ACTION_SHOW_DESKTOP),
    BSPL_ENUM("WindowShade", WLMAKER_ACTION_WINDOW_SHADE),
    BSPL_ENUM("WindowUnshade", WLMAKER_ACTION_WINDOW_UNSHADE),
    BSPL_ENUM("WindowClose", WLMAKER_ACTION_WINDOW_CLOSE),
    BSPL_ENUM("WindowKill", WLMAKER_ACTION_WINDOW_KILL),
    BSPL_ENUM("WindowAttributes", WLMAKER_ACTION_WINDOW_ATTRIBUTES),
    BSPL_ENUM("WindowAddToDock", WLMAKER_ACTION_WINDOW_ADD_TO_DOCK),
    BSPL_ENUM("SessionSave", WLMAKER_ACTION_SESSION_SAVE),
    BSPL_ENUM("SessionClear", WLMAKER_ACTION_SESSION_CLEAR),
    BSPL_ENUM("WindowTileLeft", WLMAKER_ACTION_WINDOW_TILE_LEFT),
    BSPL_ENUM("WindowTileRight", WLMAKER_ACTION_WINDOW_TILE_RIGHT),
    BSPL_ENUM("WindowTileTop", WLMAKER_ACTION_WINDOW_TILE_TOP),
    BSPL_ENUM("WindowTileBottom", WLMAKER_ACTION_WINDOW_TILE_BOTTOM),
    BSPL_ENUM("WindowTileTopLeft", WLMAKER_ACTION_WINDOW_TILE_TOP_LEFT),
    BSPL_ENUM("WindowTileTopRight", WLMAKER_ACTION_WINDOW_TILE_TOP_RIGHT),
    BSPL_ENUM("WindowTileBottomLeft", WLMAKER_ACTION_WINDOW_TILE_BOTTOM_LEFT),
    BSPL_ENUM("WindowTileBottomRight", WLMAKER_ACTION_WINDOW_TILE_BOTTOM_RIGHT),
    BSPL_ENUM("WindowUntile", WLMAKER_ACTION_WINDOW_UNTILE),
    BSPL_ENUM("WindowCenter", WLMAKER_ACTION_WINDOW_CENTER),
    BSPL_ENUM("WindowToggleOnTop", WLMAKER_ACTION_WINDOW_TOGGLE_ON_TOP),
    BSPL_ENUM("WindowToggleAtBottom", WLMAKER_ACTION_WINDOW_TOGGLE_AT_BOTTOM),
    BSPL_ENUM("WindowToggleOmnipresent", WLMAKER_ACTION_WINDOW_TOGGLE_OMNIPRESENT),
    BSPL_ENUM("WindowToNextWorkspace", WLMAKER_ACTION_WINDOW_TO_NEXT_WORKSPACE),
    BSPL_ENUM("WindowToPreviousWorkspace", WLMAKER_ACTION_WINDOW_TO_PREVIOUS_WORKSPACE),

    BSPL_ENUM("RootMenu", WLMAKER_ACTION_ROOT_MENU),
    BSPL_ENUM("WindowList", WLMAKER_ACTION_WINDOW_LIST),

    BSPL_ENUM("ThemeLoadFromFile", WLMAKER_ACTION_THEME_LOAD_FROM_FILE),

    BSPL_ENUM("OutputMagnify", WLMAKER_ACTION_OUTPUT_MAGNIFY),
    BSPL_ENUM("OutputReduce", WLMAKER_ACTION_OUTPUT_REDUCE),
    BSPL_ENUM("OutputSaveState", WLMAKER_ACTION_OUTPUT_SAVE_STATE),

    BSPL_ENUM("SwitchToVT1", WLMAKER_ACTION_SWITCH_TO_VT1),
    BSPL_ENUM("SwitchToVT2", WLMAKER_ACTION_SWITCH_TO_VT2),
    BSPL_ENUM("SwitchToVT3", WLMAKER_ACTION_SWITCH_TO_VT3),
    BSPL_ENUM("SwitchToVT4", WLMAKER_ACTION_SWITCH_TO_VT4),
    BSPL_ENUM("SwitchToVT5", WLMAKER_ACTION_SWITCH_TO_VT5),
    BSPL_ENUM("SwitchToVT6", WLMAKER_ACTION_SWITCH_TO_VT6),
    BSPL_ENUM("SwitchToVT7", WLMAKER_ACTION_SWITCH_TO_VT7),
    BSPL_ENUM("SwitchToVT8", WLMAKER_ACTION_SWITCH_TO_VT8),
    BSPL_ENUM("SwitchToVT9", WLMAKER_ACTION_SWITCH_TO_VT9),
    BSPL_ENUM("SwitchToVT10", WLMAKER_ACTION_SWITCH_TO_VT10),
    BSPL_ENUM("SwitchToVT11", WLMAKER_ACTION_SWITCH_TO_VT11),
    BSPL_ENUM("SwitchToVT12", WLMAKER_ACTION_SWITCH_TO_VT12),

    // A duplicate to ShellExecute, permits `wmmenugen` compatibility.
    BSPL_ENUM("SHEXEC", WLMAKER_ACTION_SHELL_EXECUTE),
    // A duplicate to Execute, permits compatibility with Window Maker.
    BSPL_ENUM("EXEC", WLMAKER_ACTION_EXECUTE),

    BSPL_ENUM_SENTINEL(),
};

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
wlmaker_action_handle_t *wlmaker_action_bind_keys(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *keybindings_dict_ptr,
    bool add_logo)
{
    wlmaker_action_handle_t *handle_ptr = logged_calloc(
        1, sizeof(wlmaker_action_handle_t));
    if (NULL == handle_ptr) return NULL;
    handle_ptr->server_ptr = server_ptr;
    handle_ptr->add_logo = add_logo;

    if (bspl_dict_foreach(
            keybindings_dict_ptr,
            _wlmaker_keybindings_bind_item,
            handle_ptr)) {
        return handle_ptr;
    }

    wlmaker_action_unbind_keys(handle_ptr);
    return NULL;
}

/* ------------------------------------------------------------------------- */
void wlmaker_action_unbind_keys(wlmaker_action_handle_t *handle_ptr)
{
    bs_dequeue_node_t *qnode_ptr = handle_ptr->bindings.head_ptr;
    while (NULL != qnode_ptr) {
        _wlmaker_action_binding_t *binding_ptr = BS_CONTAINER_OF(
            qnode_ptr, _wlmaker_action_binding_t, qnode);
        qnode_ptr = qnode_ptr->next_ptr;
        wlmim_unbind_key(
            handle_ptr->server_ptr->input_manager_ptr,
            binding_ptr->keybinding_ptr);
        free(binding_ptr->action_arg_ptr);
        free(binding_ptr);
    }

    free(handle_ptr);
}

/* ------------------------------------------------------------------------- */
void wlmaker_action_execute(wlmaker_server_t *server_ptr,
                            wlmaker_action_t action,
                            void *arg_ptr)
{
    wlmtk_workspace_t *workspace_ptr, *next_workspace_ptr;
    wlmtk_window_t *window_ptr;
    const char **argv;

    switch (action) {
    case WLMAKER_ACTION_NONE:
        break;

    case WLMAKER_ACTION_QUIT:
        if (NULL != server_ptr->session_ptr &&
            wlmaker_session_save_on_exit(server_ptr->session_ptr)) {
            wlmaker_session_save(server_ptr->session_ptr);
        }
        wl_display_terminate(server_ptr->wl_display_ptr);
        break;

    case WLMAKER_ACTION_LOCK_SCREEN:
        if (NULL != server_ptr->idle_monitor_ptr) {
            wlmaker_idle_monitor_lock(server_ptr->idle_monitor_ptr);
        }
        break;

    case WLMAKER_ACTION_LOCK_INHIBIT_BEGIN:
        wlmaker_idle_monitor_inhibit(server_ptr->idle_monitor_ptr);
        break;

    case WLMAKER_ACTION_LOCK_INHIBIT_END:
        wlmaker_idle_monitor_uninhibit(server_ptr->idle_monitor_ptr);
        break;

    case WLMAKER_ACTION_LAUNCH_TERMINAL:
        argv = (const char*[]){ "/bin/sh", "-c", "/usr/bin/foot", NULL };
        wlmaker_subprocess_monitor_run(
            server_ptr->monitor_ptr,
            bs_subprocess_create(argv[0], argv, NULL));
        break;

    case WLMAKER_ACTION_SHELL_EXECUTE:
        argv = (const char*[]){ "/bin/sh", "-c", arg_ptr, NULL };
        wlmaker_subprocess_monitor_run(
            server_ptr->monitor_ptr,
            bs_subprocess_create(argv[0], argv, NULL));
        break;

    case WLMAKER_ACTION_EXECUTE:
        wlmaker_subprocess_monitor_run(
            server_ptr->monitor_ptr,
            bs_subprocess_create_cmdline(arg_ptr));
        break;

    case WLMAKER_ACTION_SESSION_SAVE:
        if (NULL != server_ptr->session_ptr) {
            wlmaker_session_save(server_ptr->session_ptr);
        }
        break;

    case WLMAKER_ACTION_SESSION_CLEAR:
        if (NULL != server_ptr->session_ptr) {
            wlmaker_session_clear(server_ptr->session_ptr);
        }
        break;

    case WLMAKER_ACTION_WINDOW_ADD_TO_DOCK: {
        // The settings tool finds the application's desktop entry, adds a
        // launcher for it and reloads (Config.plist SettingsCommand).
        workspace_ptr = wlmtk_root_get_current_workspace(server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL == window_ptr) break;
        const char *cmd_ptr = bspl_dict_get_string_value(
            server_ptr->config_dict_ptr, "SettingsCommand");
        const char *app_id_ptr = wlmtk_window_get_app_id(window_ptr);
        if (NULL == app_id_ptr || 0 == *app_id_ptr) break;
        argv = (const char*[]){
            "/bin/sh", "-c", "exec \"$0\" --dock-add \"$1\"",
            NULL != cmd_ptr ? cmd_ptr : "wmaker-next-settings",
            app_id_ptr, NULL };
        wlmaker_subprocess_monitor_run(
            server_ptr->monitor_ptr,
            bs_subprocess_create(argv[0], argv, NULL));
        break;
    }

    case WLMAKER_ACTION_WINDOW_ATTRIBUTES: {
        // Opens the attributes inspector (Config.plist AttributesCommand,
        // wmaker-next-settings by default) for the window's application.
        workspace_ptr = wlmtk_root_get_current_workspace(server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL == window_ptr) break;
        const char *cmd_ptr = bspl_dict_get_string_value(
            server_ptr->config_dict_ptr, "AttributesCommand");
        const char *app_id_ptr = wlmtk_window_get_app_id(window_ptr);
        const char *title_ptr = wlmtk_window_get_title(window_ptr);
        // App ID & title come from the client: Arguments, never shell code.
        argv = (const char*[]){
            "/bin/sh", "-c", "exec \"$0\" --attributes \"$1\" \"$2\"",
            NULL != cmd_ptr ? cmd_ptr : "wmaker-next-settings",
            NULL != app_id_ptr ? app_id_ptr : "",
            NULL != title_ptr ? title_ptr : "",
            NULL };
        wlmaker_subprocess_monitor_run(
            server_ptr->monitor_ptr,
            bs_subprocess_create(argv[0], argv, NULL));
        break;
    }

    case WLMAKER_ACTION_WORKSPACE_TO_PREVIOUS:
        wlmtk_root_switch_to_previous_workspace(server_ptr->root_ptr);
        break;

    case WLMAKER_ACTION_WORKSPACE_TO_NEXT:
        wlmtk_root_switch_to_next_workspace(server_ptr->root_ptr);
        break;

    case WLMAKER_ACTION_WORKSPACE_ADD:
        workspace_ptr = wlmtk_workspace_create(
            server_ptr->wlr_output_layout_ptr,
            "New",
            &server_ptr->style_ptr->tile);
        if (NULL != workspace_ptr) {
            bs_dllist_node_t *bg_ptr = wlmaker_background_create(
                workspace_ptr,
                server_ptr->wlr_output_layout_ptr,
                server_ptr->style_ptr->background_color);
            if (NULL != bg_ptr) {
                bs_dllist_push_back(&server_ptr->backgrounds, bg_ptr);
                wlmtk_root_add_workspace(server_ptr->root_ptr, workspace_ptr);
            } else {
                wlmtk_workspace_destroy(workspace_ptr);
            }
        }
        break;

    case WLMAKER_ACTION_WORKSPACE_DESTROY_LAST:
        // TODO(kaeser@gubbe.ch): This leaks the corresponding background. The
        // workspace dtor will remove the associated element via the
        // panel dtor, but it remains in @ref wlmaker_server_t::backgrounds.
        wlmtk_root_destroy_last_workspace(server_ptr->root_ptr);
        break;

    // The switcher shows the windows in a stable order. Its first press goes
    // to the window used before (Alt+Tab, release: back and forth); further
    // presses, while held, step through the tiles in their order.
    case WLMAKER_ACTION_TASK_TO_PREVIOUS:
        if (server_ptr->task_list_enabled) {
            wlmtk_workspace_activate_window_in_order(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr), false);
        } else {
            wlmtk_workspace_activate_previous_window(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr));
        }
        wlmaker_server_activate_task_list(server_ptr);
        break;

    case WLMAKER_ACTION_TASK_TO_NEXT:
        if (server_ptr->task_list_enabled) {
            wlmtk_workspace_activate_window_in_order(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr), true);
        } else {
            wlmtk_workspace_activate_next_window(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr));
        }
        wlmaker_server_activate_task_list(server_ptr);
        break;

    case WLMAKER_ACTION_WINDOW_RAISE:
    case WLMAKER_ACTION_WINDOW_LOWER:
    case WLMAKER_ACTION_WINDOW_KILL:
    case WLMAKER_ACTION_WINDOW_TILE_LEFT:
    case WLMAKER_ACTION_WINDOW_TILE_RIGHT:
    case WLMAKER_ACTION_WINDOW_TILE_TOP:
    case WLMAKER_ACTION_WINDOW_TILE_BOTTOM:
    case WLMAKER_ACTION_WINDOW_TILE_TOP_LEFT:
    case WLMAKER_ACTION_WINDOW_TILE_TOP_RIGHT:
    case WLMAKER_ACTION_WINDOW_TILE_BOTTOM_LEFT:
    case WLMAKER_ACTION_WINDOW_TILE_BOTTOM_RIGHT:
    case WLMAKER_ACTION_WINDOW_UNTILE:
    case WLMAKER_ACTION_WINDOW_CENTER:
    case WLMAKER_ACTION_WINDOW_TOGGLE_ON_TOP:
    case WLMAKER_ACTION_WINDOW_TOGGLE_AT_BOTTOM:
    case WLMAKER_ACTION_WINDOW_TOGGLE_OMNIPRESENT:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            _wlmaker_action_window(workspace_ptr, window_ptr, action);
        }
        break;

    case WLMAKER_ACTION_WINDOW_TOGGLE_FULLSCREEN:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_fullscreen(
                window_ptr, !wlmtk_window_is_fullscreen(window_ptr));
        }
        break;

    case WLMAKER_ACTION_WINDOW_TOGGLE_MAXIMIZED:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_maximized(
                window_ptr, !wlmtk_window_is_maximized(window_ptr));
        }
        break;

    case WLMAKER_ACTION_WINDOW_MAXIMIZE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_maximized(window_ptr, true);
        }
        break;

    case WLMAKER_ACTION_WINDOW_UNMAXIMIZE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_maximized(window_ptr, false);
        }
        break;

    case WLMAKER_ACTION_WINDOW_FULLSCREEN:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_fullscreen(window_ptr, true);
        }
        break;

    case WLMAKER_ACTION_WINDOW_MINIMIZE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_minimize(window_ptr);
        }
        break;

    case WLMAKER_ACTION_SHOW_DESKTOP:
        _wlmaker_action_show_desktop(server_ptr);
        break;

    case WLMAKER_ACTION_HIDE_APPLICATION:
    case WLMAKER_ACTION_HIDE_OTHERS:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            _wlmaker_action_hide(server_ptr, window_ptr,
                                 WLMAKER_ACTION_HIDE_OTHERS == action);
        }
        break;

    case WLMAKER_ACTION_WINDOW_SHADE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_shaded(window_ptr, true);
        }
        break;

    case WLMAKER_ACTION_WINDOW_UNSHADE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_shaded(window_ptr, false);
        }
        break;

    case WLMAKER_ACTION_WORKSPACE_1:
    case WLMAKER_ACTION_WORKSPACE_2:
    case WLMAKER_ACTION_WORKSPACE_3:
    case WLMAKER_ACTION_WORKSPACE_4:
    case WLMAKER_ACTION_WORKSPACE_5:
    case WLMAKER_ACTION_WORKSPACE_6:
    case WLMAKER_ACTION_WORKSPACE_7:
    case WLMAKER_ACTION_WORKSPACE_8:
    case WLMAKER_ACTION_WORKSPACE_9:
    case WLMAKER_ACTION_WORKSPACE_10:
        wlmtk_root_switch_to_workspace(
            server_ptr->root_ptr,
            wlmtk_root_get_workspace(
                server_ptr->root_ptr,
                action - WLMAKER_ACTION_WORKSPACE_1));
        break;

    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_1:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_2:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_3:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_4:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_5:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_6:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_7:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_8:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_9:
    case WLMAKER_ACTION_WINDOW_TO_WORKSPACE_10:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        next_workspace_ptr = wlmtk_root_get_workspace(
            server_ptr->root_ptr,
            action - WLMAKER_ACTION_WINDOW_TO_WORKSPACE_1);
        if (NULL != window_ptr &&
            NULL != next_workspace_ptr &&
            workspace_ptr != next_workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
            wlmtk_workspace_map_window(next_workspace_ptr, window_ptr);
        }
        break;

    case WLMAKER_ACTION_WINDOW_TO_NEXT_WORKSPACE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        next_workspace_ptr = wlmtk_workspace_from_dlnode(
            wlmtk_dlnode_from_workspace(workspace_ptr)->next_ptr);
        if (NULL != window_ptr &&
            NULL != next_workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
            wlmtk_workspace_map_window(next_workspace_ptr, window_ptr);
        }
        break;

    case WLMAKER_ACTION_WINDOW_TO_PREVIOUS_WORKSPACE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        next_workspace_ptr = wlmtk_workspace_from_dlnode(
            wlmtk_dlnode_from_workspace(workspace_ptr)->prev_ptr);
        if (NULL != window_ptr &&
            NULL != next_workspace_ptr) {
            wlmtk_workspace_unmap_window(workspace_ptr, window_ptr);
            wlmtk_workspace_map_window(next_workspace_ptr, window_ptr);
        }
        break;

    case WLMAKER_ACTION_WINDOW_CLOSE:
        workspace_ptr = wlmtk_root_get_current_workspace(
            server_ptr->root_ptr);
        window_ptr = wlmtk_workspace_get_activated_window(workspace_ptr);
        if (NULL != window_ptr) {
            wlmtk_window_request_close(window_ptr);
        }
        break;

    case WLMAKER_ACTION_WINDOW_LIST:
        if (NULL != server_ptr->window_list_ptr) {
            wlmaker_window_list_open(server_ptr->window_list_ptr);
        }
        break;

    case WLMAKER_ACTION_ROOT_MENU:
        // TODO(kaeser@gubbe.ch): Clean up.
        if (NULL != server_ptr->root_menu_ptr &&
            NULL == wlmtk_window_get_workspace(
                wlmaker_root_menu_window(server_ptr->root_menu_ptr))) {
            wlmtk_workspace_map_window(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr),
                wlmaker_root_menu_window(server_ptr->root_menu_ptr));
            wlmtk_workspace_set_window_position(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr),
                wlmaker_root_menu_window(server_ptr->root_menu_ptr),
                wlmim_wlr_cursor(server_ptr->input_manager_ptr)->x,
                wlmim_wlr_cursor(server_ptr->input_manager_ptr)->y);
            wlmtk_workspace_confine_within(
                wlmtk_root_get_current_workspace(server_ptr->root_ptr),
                wlmaker_root_menu_window(server_ptr->root_menu_ptr));
            wlmtk_menu_set_mode(
                wlmaker_root_menu_menu(server_ptr->root_menu_ptr),
                WLMTK_MENU_MODE_NORMAL);
            wlmtk_menu_set_open(
                wlmaker_root_menu_menu(server_ptr->root_menu_ptr),
                true);
        }
        break;

    case WLMAKER_ACTION_THEME_LOAD_FROM_FILE:
        wlmaker_action_theme_load(server_ptr, arg_ptr);
        break;

    case WLMAKER_ACTION_OUTPUT_MAGNIFY:
        wlmbe_backend_magnify(server_ptr->backend_ptr);
        break;
    case WLMAKER_ACTION_OUTPUT_REDUCE:
        wlmbe_backend_reduce(server_ptr->backend_ptr);
        break;

    case WLMAKER_ACTION_OUTPUT_SAVE_STATE:
        wlmbe_backend_save_ephemeral_output_configs(server_ptr->backend_ptr);
        break;

    case WLMAKER_ACTION_SWITCH_TO_VT1:
    case WLMAKER_ACTION_SWITCH_TO_VT2:
    case WLMAKER_ACTION_SWITCH_TO_VT3:
    case WLMAKER_ACTION_SWITCH_TO_VT4:
    case WLMAKER_ACTION_SWITCH_TO_VT5:
    case WLMAKER_ACTION_SWITCH_TO_VT6:
    case WLMAKER_ACTION_SWITCH_TO_VT7:
    case WLMAKER_ACTION_SWITCH_TO_VT8:
    case WLMAKER_ACTION_SWITCH_TO_VT9:
    case WLMAKER_ACTION_SWITCH_TO_VT10:
    case WLMAKER_ACTION_SWITCH_TO_VT11:
    case WLMAKER_ACTION_SWITCH_TO_VT12:
        // Enums are required to be defined consecutively, so we can compute
        // the VT number from the action code.
        wlmbe_backend_switch_to_vt(
            server_ptr->backend_ptr,
            action - WLMAKER_ACTION_SWITCH_TO_VT1 + 1);
        break;

    default:
        bs_log(BS_WARNING, "Unhandled action %d.", action);
        break;
    }
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/**
 * @return whether both windows belong to the same application: Same app ID
 *     if both have one, else the same client process.
 */
bool _wlmaker_action_same_application(
    wlmtk_window_t *window1_ptr,
    wlmtk_window_t *window2_ptr)
{
    if (window1_ptr == window2_ptr) return true;
    const char *app_id1_ptr = wlmtk_window_get_app_id(window1_ptr);
    const char *app_id2_ptr = wlmtk_window_get_app_id(window2_ptr);
    if (NULL != app_id1_ptr && NULL != app_id2_ptr) {
        return 0 == strcmp(app_id1_ptr, app_id2_ptr);
    }
    pid_t pid1 = wlmtk_window_get_client_ptr(window1_ptr)->pid;
    return 0 != pid1 && pid1 == wlmtk_window_get_client_ptr(window2_ptr)->pid;
}

/* ------------------------------------------------------------------------- */
/**
 * Toggles "Show Desktop" on the current workspace, as in Plasma: Minimizes
 * all windows; or, if none is left visible, restores those it minimized.
 *
 * @param server_ptr
 */
void _wlmaker_action_show_desktop(wlmaker_server_t *server_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_root_get_current_workspace(
        server_ptr->root_ptr);
    wlmtk_window_t *root_menu_window_ptr = NULL;
    if (NULL != server_ptr->root_menu_ptr) {
        root_menu_window_ptr = wlmaker_root_menu_window(
            server_ptr->root_menu_ptr);
    }
    bs_dllist_t *windows_ptr = wlmtk_workspace_get_windows_dllist(
        workspace_ptr);

    bool any_visible = false;
    for (bs_dllist_node_t *dlnode_ptr = windows_ptr->head_ptr;
         NULL != dlnode_ptr;
         dlnode_ptr = dlnode_ptr->next_ptr) {
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
        if (window_ptr == root_menu_window_ptr) continue;
        any_visible |= !wlmtk_window_is_minimized(window_ptr);
    }

    if (any_visible) {
        for (bs_dllist_node_t *dlnode_ptr = windows_ptr->head_ptr;
             NULL != dlnode_ptr;
             dlnode_ptr = dlnode_ptr->next_ptr) {
            wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
            if (window_ptr == root_menu_window_ptr ||
                wlmtk_window_is_minimized(window_ptr)) continue;
            wlmtk_window_request_minimize(window_ptr);
            wlmtk_window_set_minimized_by_show_desktop(window_ptr, true);
        }
        return;
    }

    // Restore in reverse stacking order, so the topmost ends up on top.
    for (bs_dllist_node_t *dlnode_ptr = windows_ptr->tail_ptr;
         NULL != dlnode_ptr; ) {
        // Restoring raises the window: It moves to the list's head.
        bs_dllist_node_t *prev_ptr = dlnode_ptr->prev_ptr;
        wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
        if (wlmtk_window_is_minimized_by_show_desktop(window_ptr)) {
            wlmtk_workspace_restore_window(workspace_ptr, window_ptr);
        }
        dlnode_ptr = prev_ptr;
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Window Maker's "Hide" and "Hide Others": Minimizes all windows of the
 * application of `app_window_ptr`, on all workspaces. Or, with `others`,
 * all windows of other applications on the current workspace.
 *
 * @param server_ptr
 * @param app_window_ptr
 * @param others
 */
void _wlmaker_action_hide(
    wlmaker_server_t *server_ptr,
    wlmtk_window_t *app_window_ptr,
    bool others)
{
    wlmtk_workspace_t *current_ptr = wlmtk_root_get_current_workspace(
        server_ptr->root_ptr);
    wlmtk_window_t *root_menu_window_ptr = NULL;
    if (NULL != server_ptr->root_menu_ptr) {
        root_menu_window_ptr = wlmaker_root_menu_window(
            server_ptr->root_menu_ptr);
    }

    for (size_t i = 0; ; ++i) {
        wlmtk_workspace_t *workspace_ptr = wlmtk_root_get_workspace(
            server_ptr->root_ptr, i);
        if (NULL == workspace_ptr) break;
        if (others && workspace_ptr != current_ptr) continue;

        // Minimizing keeps windows mapped, so the list stays intact.
        for (bs_dllist_node_t *dlnode_ptr =
                 wlmtk_workspace_get_windows_dllist(workspace_ptr)->head_ptr;
             NULL != dlnode_ptr;
             dlnode_ptr = dlnode_ptr->next_ptr) {
            wlmtk_window_t *window_ptr = wlmtk_window_from_dlnode(dlnode_ptr);
            if (window_ptr == root_menu_window_ptr) continue;
            if (others == _wlmaker_action_same_application(
                    window_ptr, app_window_ptr)) continue;
            wlmtk_window_request_minimize(window_ptr);
        }
    }
}

/* ------------------------------------------------------------------------- */
/**
 * Binds an action for one item of the 'KeyBindings' dict.
 *
 * Supports two configuration formats:
 * - Array format: `"Key" = (Action, "command");` for actions with arguments.
 * - String format: `"Key" = Action;` for simple actions without arguments.
 *
 * @param key_ptr             The key binding string (eg. "Logo+Return").
 * @param object_ptr          Configuration value: a string with the action
 *                            name, or an array with action and argument.
 * @param userdata_ptr        Points to @ref wlmaker_action_handle_t.
 *
 * @return true on success.
 */
bool _wlmaker_keybindings_bind_item(
    const char *key_ptr,
    bspl_object_t *object_ptr,
    void *userdata_ptr)
{
    wlmaker_action_handle_t *handle_ptr = userdata_ptr;
    const char *action_name_ptr = NULL;
    char *action_arg_ptr = NULL;

    // Extract action name and optional argument from the configuration value.
    bspl_array_t *array_ptr = bspl_array_from_object(object_ptr);
    if (NULL != array_ptr) {
        // Array format: (Action, "command"). The command argument is optional.
        action_name_ptr = bspl_array_string_value_at(array_ptr, 0);
        if (NULL == action_name_ptr) {
            bs_log(BS_WARNING,
                   "Key binding \"%s\": first element must be a string.",
                   key_ptr);
            return false;
        }
        // Second element is the command argument, may be absent.
        const char *arg_ptr = bspl_array_string_value_at(array_ptr, 1);
        if (NULL != arg_ptr) {
            action_arg_ptr = logged_strdup(arg_ptr);
            if (NULL == action_arg_ptr) return false;
        }
    } else {
        // String format: Action name only, no argument.
        bspl_string_t *string_ptr = bspl_string_from_object(object_ptr);
        if (NULL == string_ptr) {
            bs_log(BS_WARNING,
                   "Key binding \"%s\": must be a string or array.",
                   key_ptr);
            return false;
        }
        action_name_ptr = bspl_string_value(string_ptr);
    }

    // Parse the key binding string into modifiers and keysym.
    uint32_t modifiers;
    xkb_keysym_t keysym;
    if (!_wlmaker_keybindings_parse(key_ptr, &modifiers, &keysym)) {
        bs_log(BS_WARNING,
               "Failed to parse binding '%s' for keybinding action '%s'",
               key_ptr, action_name_ptr);
        free(action_arg_ptr);
        return false;
    }
    if (handle_ptr->add_logo) modifiers |= WLR_MODIFIER_LOGO;

    // Lookup the action enum value from the action name.
    int action;
    if (!bspl_enum_name_to_value(
            wlmaker_action_desc, action_name_ptr, &action)) {
        bs_log(BS_WARNING, "Not a valid keybinding action: '%s'",
               action_name_ptr);
        free(action_arg_ptr);
        return false;
    }

    // Create and populate the binding structure.
    _wlmaker_action_binding_t *action_binding_ptr = logged_calloc(
        1, sizeof(_wlmaker_action_binding_t));
    if (NULL == action_binding_ptr) {
        free(action_arg_ptr);
        return false;
    }
    action_binding_ptr->handle_ptr = handle_ptr;
    action_binding_ptr->action = action;
    action_binding_ptr->action_arg_ptr = action_arg_ptr;
    action_binding_ptr->combo.keysym = keysym;
    action_binding_ptr->combo.ignore_case = true;
    action_binding_ptr->combo.modifiers = modifiers;
    action_binding_ptr->combo.modifiers_mask = wlmim_modifiers_default_mask;

    // Register the key binding with the server.
    action_binding_ptr->keybinding_ptr = wlmim_bind_key(
        handle_ptr->server_ptr->input_manager_ptr,
        &action_binding_ptr->combo,
        _wlmaker_action_bound_callback);
    if (NULL == action_binding_ptr->keybinding_ptr) {
        free(action_binding_ptr->action_arg_ptr);
        free(action_binding_ptr);
        return false;
    }

    bs_dequeue_push_back(&handle_ptr->bindings, &action_binding_ptr->qnode);
    return true;
}

/* ------------------------------------------------------------------------- */
/**
 * Parses a keybinding string: Tokenizes into modifiers and keysym.
 *
 * @param string_ptr
 * @param modifiers_ptr
 * @param keysym_ptr
 *
 * @return true on success.
 */
bool _wlmaker_keybindings_parse(
    const char *string_ptr,
    uint32_t *modifiers_ptr,
    xkb_keysym_t *keysym_ptr)
{
    *keysym_ptr = XKB_KEY_NoSymbol;
    *modifiers_ptr = 0;
    bool rv = true;

    // Tokenize along '+', then lookup each of the keys.
    for (const char *start_ptr = string_ptr; *start_ptr != '\0'; ++start_ptr) {

        const char *end_ptr = start_ptr;
        while (*end_ptr != '\0' && *end_ptr != '+') ++end_ptr;

        size_t len = end_ptr - start_ptr;
        char *token_ptr = malloc(len + 1);
        memcpy(token_ptr, start_ptr, len);
        token_ptr[len] = '\0';

        start_ptr = end_ptr;

        int new_modifier;
        if (bspl_enum_name_to_value(
                wlmim_keyboard_modifiers, token_ptr, &new_modifier)) {
            *modifiers_ptr |= new_modifier;
        } else if (*keysym_ptr == XKB_KEY_NoSymbol) {
            *keysym_ptr = xkb_keysym_to_upper(
                xkb_keysym_from_name(token_ptr, XKB_KEYSYM_CASE_INSENSITIVE));
        } else {
            rv = false;
        }

        free(token_ptr);
        if (!*start_ptr) break;
    }

    return rv && (XKB_KEY_NoSymbol != *keysym_ptr);
}

/* ------------------------------------------------------------------------- */
/**
 * Callback for when the bound key is triggered: Executes the corresponding
 * action.
 *
 * @param combo_ptr
 *
 * @return true always.
 */
bool _wlmaker_action_bound_callback(
    const struct wlmim_keybinding_combo *combo_ptr)
{
    _wlmaker_action_binding_t *action_binding_ptr = BS_CONTAINER_OF(
        combo_ptr, _wlmaker_action_binding_t, combo);

    wlmaker_action_execute(
        action_binding_ptr->handle_ptr->server_ptr,
        action_binding_ptr->action,
        action_binding_ptr->action_arg_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Window actions: Stacking, tiling, states, kill -- on `window_ptr`. */
void _wlmaker_action_window(
    wlmtk_workspace_t *workspace_ptr,
    wlmtk_window_t *window_ptr,
    wlmaker_action_t action)
{
    static const struct { wlmaker_action_t action; uint32_t edges; } tiles[] = {
        { WLMAKER_ACTION_WINDOW_TILE_LEFT, WLR_EDGE_LEFT },
        { WLMAKER_ACTION_WINDOW_TILE_RIGHT, WLR_EDGE_RIGHT },
        { WLMAKER_ACTION_WINDOW_TILE_TOP, WLR_EDGE_TOP },
        { WLMAKER_ACTION_WINDOW_TILE_BOTTOM, WLR_EDGE_BOTTOM },
        { WLMAKER_ACTION_WINDOW_TILE_TOP_LEFT, WLR_EDGE_TOP | WLR_EDGE_LEFT },
        { WLMAKER_ACTION_WINDOW_TILE_TOP_RIGHT, WLR_EDGE_TOP | WLR_EDGE_RIGHT },
        { WLMAKER_ACTION_WINDOW_TILE_BOTTOM_LEFT,
          WLR_EDGE_BOTTOM | WLR_EDGE_LEFT },
        { WLMAKER_ACTION_WINDOW_TILE_BOTTOM_RIGHT,
          WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT },
        { WLMAKER_ACTION_WINDOW_UNTILE, 0 },
    };
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); ++i) {
        if (tiles[i].action != action) continue;
        if (!wlmtk_window_is_resizable(window_ptr)) return;
        // The same tile again restores the window, as Window Maker does.
        uint32_t edges = tiles[i].edges;
        if (edges == wlmtk_window_get_tiled_edges(window_ptr)) edges = 0;
        wlmtk_window_request_tiled(window_ptr, edges);
        return;
    }

    switch (action) {
    case WLMAKER_ACTION_WINDOW_RAISE:
        wlmtk_workspace_raise_window(workspace_ptr, window_ptr);
        break;
    case WLMAKER_ACTION_WINDOW_LOWER:
        wlmtk_workspace_lower_window(workspace_ptr, window_ptr);
        break;
    case WLMAKER_ACTION_WINDOW_CENTER:
        wlmtk_workspace_center_window(workspace_ptr, window_ptr);
        break;
    case WLMAKER_ACTION_WINDOW_TOGGLE_ON_TOP:
        wlmtk_window_set_keep_on_top(
            window_ptr, !wlmtk_window_is_keep_on_top(window_ptr));
        break;
    case WLMAKER_ACTION_WINDOW_TOGGLE_AT_BOTTOM:
        wlmtk_window_set_keep_at_bottom(
            window_ptr, !wlmtk_window_is_keep_at_bottom(window_ptr));
        break;
    case WLMAKER_ACTION_WINDOW_TOGGLE_OMNIPRESENT:
        wlmtk_window_set_omnipresent(
            window_ptr, !wlmtk_window_is_omnipresent(window_ptr));
        break;
    case WLMAKER_ACTION_WINDOW_KILL: {
        // Unlike Close, doesn't ask the application: Ends its process.
        const wlmtk_util_client_t *client_ptr =
            wlmtk_window_get_client_ptr(window_ptr);
        if (NULL == client_ptr || 0 >= client_ptr->pid ||
            getpid() == client_ptr->pid) {
            bs_log(BS_WARNING, "Kill: No process known for window %p",
                   window_ptr);
            break;
        }
        bs_log(BS_INFO, "Kill: Sending SIGKILL to process %d",
               (int)client_ptr->pid);
        kill(client_ptr->pid, SIGKILL);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
bool wlmaker_action_theme_load(
    wlmaker_server_t *server_ptr,
    const char *fname_ptr)
{
    if (!wlmaker_theme_load(
            server_ptr->files_ptr,
            fname_ptr,
            server_ptr->style_ptr)) return false;

    // TODO(kaeser@gubbe.ch): Als update style for ...
    // - Dock
    // - Clip
    // - Cursor
    // - BackgroundColor
    // - Tasklist

    bool rv = true;
    rv &= wlmtk_root_set_style(
        server_ptr->root_ptr,
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));

    if (NULL != server_ptr->root_menu_ptr) {
        wlmtk_menu_set_style(
            wlmaker_root_menu_menu(server_ptr->root_menu_ptr),
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
        wlmtk_window_set_style(
            wlmaker_root_menu_window(server_ptr->root_menu_ptr),
            wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    }

    bs_dllist_for_each(
        &server_ptr->backgrounds,
        wlmaker_background_dlnode_set_color,
        &server_ptr->style_ptr->background_color);

    rv &= wlmim_set_style(
        server_ptr->input_manager_ptr,
        &server_ptr->style_ptr->cursor);

    wl_signal_emit(&server_ptr->theme_changed_event, server_ptr->style_ptr);

    return rv;
}

/* == Unit tests =========================================================== */

static void test_keybindings_parse(bs_test_t *test_ptr);
static void test_default_keybindings(bs_test_t *test_ptr);
static void test_keybindings_formats(bs_test_t *test_ptr);

/** Test cases for key bindings. */
static const bs_test_case_t   wlmaker_action_test_cases[] = {
    { true, "parse", test_keybindings_parse },
    { true, "default_keybindings", test_default_keybindings },
    { true, "formats", test_keybindings_formats },
    BS_TEST_CASE_SENTINEL()
};

const bs_test_set_t wlmaker_action_test_set = BS_TEST_SET(
    true, "action", wlmaker_action_test_cases);

/* ------------------------------------------------------------------------- */
/** Tests @ref _wlmaker_keybindings_parse. */
void test_keybindings_parse(bs_test_t *test_ptr)
{
    uint32_t m;
    xkb_keysym_t ks;

    // Lower- and upper case.
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmaker_keybindings_parse("A", &m, &ks));
    BS_TEST_VERIFY_EQ(test_ptr, 0, m);
    BS_TEST_VERIFY_EQ(test_ptr, XKB_KEY_A, ks);
    BS_TEST_VERIFY_TRUE(test_ptr, _wlmaker_keybindings_parse("a", &m, &ks));
    BS_TEST_VERIFY_EQ(test_ptr, 0, m);
    BS_TEST_VERIFY_EQ(test_ptr, XKB_KEY_A, ks);

    // Modifier.
    BS_TEST_VERIFY_TRUE(
        test_ptr, _wlmaker_keybindings_parse("Ctrl+Logo+Q", &m, &ks));
    BS_TEST_VERIFY_EQ(test_ptr, WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO, m);
    BS_TEST_VERIFY_EQ(test_ptr, XKB_KEY_Q, ks);

    // Test some fancier keys.
    BS_TEST_VERIFY_TRUE(
        test_ptr, _wlmaker_keybindings_parse("Escape", &m, &ks));
    BS_TEST_VERIFY_EQ(test_ptr, XKB_KEY_Escape, ks);
    BS_TEST_VERIFY_TRUE(
        test_ptr, _wlmaker_keybindings_parse("XF86AudioLowerVolume", &m, &ks));
    BS_TEST_VERIFY_EQ(test_ptr, XKB_KEY_XF86AudioLowerVolume, ks);

    // Not peritted: Empty, just modifiers, more than one keysym.
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_keybindings_parse("", &m, &ks));
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_keybindings_parse("A+B", &m, &ks));
    BS_TEST_VERIFY_FALSE(
        test_ptr, _wlmaker_keybindings_parse("Shift+Ctrl", &m, &ks));
}

/* ------------------------------------------------------------------------- */
/** Tests the default configuration's 'KeyBindings' section. */
void test_default_keybindings(bs_test_t *test_ptr)
{
    bspl_dict_t *d = bspl_dict_create();
    wlmaker_server_t server = {
        .input_manager_ptr = wlmim_input_manager_create(
            NULL, NULL, NULL, d, NULL, NULL)
    };
    bspl_object_t *obj_ptr = bspl_create_object_from_plist_data(
        embedded_binary_default_configuration_data,
        embedded_binary_default_configuration_size);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, bspl_dict_from_object(obj_ptr));

    bspl_dict_t *dict_ptr = bspl_dict_get_dict(
        bspl_dict_from_object(obj_ptr), wlmaker_action_config_dict_key);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);

    wlmaker_action_handle_t *handle_ptr = wlmaker_action_bind_keys(
        &server, dict_ptr, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, handle_ptr);
    bspl_object_unref(obj_ptr);
    wlmaker_action_unbind_keys(handle_ptr);

    bspl_dict_unref(d);
    wlmim_input_manager_destroy(server.input_manager_ptr);
}

/* ------------------------------------------------------------------------- */
/** Tests string and array format keybindings. */
void test_keybindings_formats(bs_test_t *test_ptr)
{
    bspl_dict_t *d = bspl_dict_create();
    wlmaker_server_t server = {
        .input_manager_ptr = wlmim_input_manager_create(
            NULL, NULL, NULL, d, NULL, NULL)
    };
    bspl_object_t *obj_ptr;
    bspl_dict_t *dict_ptr;
    wlmaker_action_handle_t *handle_ptr;

    // Test: String format (original format).
    const char *string_format =
        "{ \"Logo+q\" = Quit; \"Logo+l\" = LockScreen; }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)string_format, strlen(string_format));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, handle_ptr);
    wlmaker_action_unbind_keys(handle_ptr);
    bspl_object_unref(obj_ptr);

    // Test: Array format with command argument.
    const char *array_format =
        "{ \"Logo+Return\" = (Execute, \"/usr/bin/foot\"); }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)array_format, strlen(array_format));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, handle_ptr);
    wlmaker_action_unbind_keys(handle_ptr);
    bspl_object_unref(obj_ptr);

    // Test: Array format without command argument.
    const char *array_no_arg = "{ \"Logo+q\" = (Quit); }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)array_no_arg, strlen(array_no_arg));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, handle_ptr);
    wlmaker_action_unbind_keys(handle_ptr);
    bspl_object_unref(obj_ptr);

    // Test: Mixed formats in one dict.
    const char *mixed_format =
        "{ \"Logo+q\" = Quit; \"Logo+Return\" = (Execute, \"/bin/sh\"); }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)mixed_format, strlen(mixed_format));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, handle_ptr);
    wlmaker_action_unbind_keys(handle_ptr);
    bspl_object_unref(obj_ptr);

    // Test: Empty array should fail.
    const char *empty_array = "{ \"Logo+q\" = (); }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)empty_array, strlen(empty_array));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, handle_ptr);
    bspl_object_unref(obj_ptr);

    // Test: Invalid action name should fail.
    const char *invalid_action = "{ \"Logo+q\" = NotAnAction; }";
    obj_ptr = bspl_create_object_from_plist_data(
        (const uint8_t*)invalid_action, strlen(invalid_action));
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, obj_ptr);
    dict_ptr = bspl_dict_from_object(obj_ptr);
    BS_TEST_VERIFY_NEQ(test_ptr, NULL, dict_ptr);
    handle_ptr = wlmaker_action_bind_keys(&server, dict_ptr, false);
    BS_TEST_VERIFY_EQ(test_ptr, NULL, handle_ptr);
    bspl_object_unref(obj_ptr);

    bspl_dict_unref(d);
    wlmim_input_manager_destroy(server.input_manager_ptr);
}

/* == End of action.c ====================================================== */
