/* ========================================================================= */
/**
 * @file wlmaker.c
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

/// setenv() is a POSIX extension.
#define _POSIX_C_SOURCE 200112L

#include <libbase/libbase.h>
#include <libbase/plist.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
/// Use non-stable features of wlroots.
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#undef WLR_USE_UNSTABLE

#include "../share/theme.h"  // IWYU pragma: keep
#include "action.h"
#include "backend/backend.h"
#include "background.h"
#include "wlmbacktrace.h"
#include "clip.h"
#include "config.h"
#include "dock.h"
#include "foreign_toplevel.h"
#include "icon_area.h"
#include "files.h"
#include "root_menu.h"
#include "server.h"
#include "task_list.h"
#include "window_rules.h"
#include "popup_menu.h"
#include "tooltip.h"
#include "toolkit/toolkit.h"

/** Will hold the value of --config_file. */
static char *wlmaker_arg_config_file_ptr = NULL;
/** Will hold the value of --state_file. */
static char *wlmaker_arg_state_file_ptr = NULL;
/** Will hold the value of --theme_file. */
static char *wlmaker_arg_theme_file_ptr = NULL;
/** Will hold the value of --root_menu_file. */
static char *wlmaker_arg_root_menu_file_ptr = NULL;

#if !defined(WLMAKER_VERSION_MAJOR) || !defined(WLMAKER_VERSION_MINOR) || !defined(WLMAKER_VERSION_FULL)
#eror "WLMAKER_VERSION_... not defined!"
#else
static const char *wlmaker_version_major = WLMAKER_VERSION_MAJOR;
static const char *wlmaker_version_minor = WLMAKER_VERSION_MINOR;
static const char *wlmaker_version_full = WLMAKER_VERSION_FULL;
#endif

/** Startup options for the server. */
static wlmaker_server_options_t wlmaker_server_options = {
    .start_xwayland = false,
    .width = 0,
    .height = 0,
    .bind_with_logo = false,
};

/** Log levels. */
static const bs_arg_enum_table_t wlmaker_log_levels[] = {
    { .name_ptr = "DEBUG", BS_DEBUG },
    { .name_ptr = "INFO", BS_INFO },
    { .name_ptr = "WARNING", BS_WARNING },
    { .name_ptr = "ERROR", BS_ERROR },
    { .name_ptr = NULL },
};

/** Definition of commandline arguments. */
static const bs_arg_t wlmaker_args[] = {
#if defined(WLMAKER_HAVE_XWAYLAND)
    BS_ARG_BOOL(
        "start_xwayland",
        "Optional: Whether to start XWayland. Disabled by default.",
        false,
        &wlmaker_server_options.start_xwayland),
#endif  // defined(WLMAKER_HAVE_XWAYLAND)
    BS_ARG_STRING(
        "config_file",
        "Optional: Path to a configuration file. If not provided, wlmaker "
        "will scan default paths for a configuration file, or fall back to "
        "a built-in configuration.",
        NULL,
        &wlmaker_arg_config_file_ptr),
    BS_ARG_STRING(
        "state_file",
        "Optional: Path to a state file, with state of workspaces, dock and "
        "clips configured. If not provided, wlmaker will scan default paths "
        "for a state file, or fall back to a built-in default.",
        NULL,
        &wlmaker_arg_state_file_ptr),
    BS_ARG_STRING(
        "theme_file",
        "Optional: Path to a \"theme\" file, configuring the visual style for "
        "elements. If not provided, wlmaker will use a built-in default theme.",
        NULL,
        &wlmaker_arg_theme_file_ptr),
    BS_ARG_STRING(
        "root_menu_file",
        "Optional: Path to a file describing the root menu. If not provided, "
        "wlmaker will use a built-in definition for the root menu.",
        NULL,
        &wlmaker_arg_root_menu_file_ptr),
    BS_ARG_ENUM(
        "log_level",
        "Log level to apply. One of DEBUG, INFO, WARNING, ERROR.",
        "INFO",
        &wlmaker_log_levels[0],
        (int*)&bs_log_severity),
    BS_ARG_BOOL(
        "bind_with_logo",
        "Optional: Whether to add 'Logo' as modifier to each key binding. "
        "Useful to test key bindings in wlmaker when running as a Wayland "
        "or X11 client. Disabled by default.",
        false,
        &wlmaker_server_options.bind_with_logo),
    BS_ARG_UINT32(
        "height",
        "Desired output height. Applies when running in windowed mode, and "
        "only if --width is set, too. Set to 0 for using the output's "
        "preferred dimensions.",
        0, 0, UINT32_MAX,
        &wlmaker_server_options.height),
    BS_ARG_UINT32(
        "width",
        "Desired output width. Applies when running in windowed mode, and "
        "only if --height is set, too. Set to 0 for using the output's "
        "preferred dimensions.",
        0, 0, UINT32_MAX,
        &wlmaker_server_options.width),
    BS_ARG_SENTINEL()
};

/** References auto-started subprocesses. */
static bs_ptr_stack_t         wlmaker_subprocess_stack;

/** Compiled regular expression for extracting file & line no. from wlr_log. */
static regex_t                wlmaker_wlr_log_regex;

/** File lookup, kept for reloading. */
static wlmaker_files_t        *wlmaker_files_ptr = NULL;
/** Current configuration. Key bindings refer to it. */
static bspl_dict_t            *wlmaker_config_dict_ptr = NULL;
/** Current state. */
static bspl_dict_t            *wlmaker_state_dict_ptr = NULL;
/** The theme file currently applied, or NULL for the default theme. */
static char                   *wlmaker_theme_file_ptr = NULL;
/** Key bindings, from @ref wlmaker_config_dict_ptr. */
static wlmaker_action_handle_t *wlmaker_action_handle_ptr = NULL;
/** The dock. */
static wlmaker_dock_t         *wlmaker_dock_ptr = NULL;
/** The clip. */
static wlmaker_clip_t         *wlmaker_clip_ptr = NULL;
/** The icon area, holding mini-windows. */
static wlmaker_icon_area_t    *wlmaker_icon_area_ptr = NULL;
/** Per-application window rules. */
static wlmaker_window_rules_t *wlmaker_window_rules_ptr = NULL;
/** Regular expression string for extracting file & line no. from wlr_log. */
static const char             *wlmaker_wlr_log_regex_string =
    "^\\[([^\\:]+)\\:([0-9]+)\\]\\ ";

/** Contents of the workspace style. */
typedef struct {
    /** Workspace name. */
    char            name[32];
    /** Background color. */
    uint32_t        color;
    /** Wallpaper image (PNG), or empty. */
    char            image[1024];
    /** How the wallpaper is fit to the output. */
    int             image_mode;
} wlmaker_workspace_style_t;

/** Style descriptor for the "Workspace" dict of wlmaker-state.plist. */
static const bspl_desc_t wlmaker_workspace_style_desc[] = {
    BSPL_DESC_CHARBUF(
        "Name", true, wlmaker_workspace_style_t, name, name, 32, NULL),
    BSPL_DESC_ARGB32(
        "Color", false, wlmaker_workspace_style_t, color, color, 0),
    BSPL_DESC_CHARBUF(
        "Image", false, wlmaker_workspace_style_t, image, image, 1024, ""),
    BSPL_DESC_ENUM(
        "ImageMode", false, wlmaker_workspace_style_t, image_mode, image_mode,
        WLMAKER_BACKGROUND_FILL, wlmaker_background_mode_desc),
    BSPL_DESC_SENTINEL()
};

/* ------------------------------------------------------------------------- */
/**
 * Wraps the wlr_log calls on bs_log.
 *
 * @param importance
 * @param fmt
 * @param args
 */
static void wlr_to_bs_log(
    enum wlr_log_importance importance,
    const char *fmt,
    va_list args)
{
    bs_log_severity_t severity = BS_DEBUG;

    switch (importance) {
    case WLR_SILENT:  // Fall-through to DEBUG severity.
    case WLR_DEBUG: severity = BS_DEBUG; break;
    case WLR_INFO: severity = BS_INFO; break;
    case WLR_ERROR: severity = BS_ERROR; break;
    default: severity = BS_INFO; break;
    }

    if (!bs_will_log(severity)) return;

    // Log to buffer. Ignores overflows.
    char buf[BS_LOG_MAX_BUF_SIZE];
    vsnprintf(buf, sizeof(buf), fmt, args);

    regmatch_t matches[4];
    if (0 != regexec(&wlmaker_wlr_log_regex, buf, 4, &matches[0], 0) ||
        matches[0].rm_so != 0 ||
        !(matches[0].rm_eo >= 6) ||  // Minimum "[x:1] ".
        matches[1].rm_so != 1 ||
        !(matches[2].rm_so > 2) ||
        matches[3].rm_so != -1) {
        bs_log(severity, "%s (wlr_log unexpected format!)", buf);
        return;
    }

    buf[matches[1].rm_eo] = '\0';
    buf[matches[2].rm_eo] = '\0';
    uint64_t line_no = 0;
    bs_strconvert_uint64(&buf[matches[2].rm_so], &line_no, 10);
    line_no = BS_MIN((uint64_t)INT_MAX, line_no);

    bs_log_write(severity, &buf[matches[1].rm_so], (int)line_no, "%s",
        &buf[matches[0].rm_eo]);
}

/* ------------------------------------------------------------------------- */
/** Launches a sub-process, and keeps it on the subprocess stack. */
bool start_subprocess(const char *cmdline_ptr)
{
    bs_subprocess_t *sp_ptr = bs_subprocess_create_cmdline(cmdline_ptr);
    if (NULL == sp_ptr) {
        bs_log(BS_ERROR, "Failed bs_subprocess_create_cmdline(\"%s\")",
               cmdline_ptr);
        return false;
    }
    if (!bs_subprocess_start(sp_ptr)) {
        bs_log(BS_ERROR, "Failed bs_subprocess_start for \"%s\"",
               cmdline_ptr);
        return false;
    }
    // Push to stack. Ignore errors: We'll keep it running untracked.
    bs_ptr_stack_push(&wlmaker_subprocess_stack, sp_ptr);
    return true;
}

/** The "Effects" configuration, as decoded. See @ref wlmtk_effects_t. */
typedef struct {
    /** Opacity of the activated window. */
    double                    active_opacity;
    /** Opacity of other windows. */
    double                    inactive_opacity;
    /** Animate minimize and restore. */
    bool                      animate_minimize;
    /** Fade in new windows. */
    bool                      animate_map;
    /** Animation duration, milliseconds. */
    int64_t                   duration_ms;
} _wlmaker_effects_config_t;

/** Decoder for the "Effects" dict of the configuration. */
static const bspl_desc_t _wlmaker_effects_desc[] = {
    BSPL_DESC_DOUBLE("ActiveOpacity", false, _wlmaker_effects_config_t,
                     active_opacity, active_opacity, 1.0),
    BSPL_DESC_DOUBLE("InactiveOpacity", false, _wlmaker_effects_config_t,
                     inactive_opacity, inactive_opacity, 1.0),
    BSPL_DESC_BOOL("AnimateMinimize", false, _wlmaker_effects_config_t,
                   animate_minimize, animate_minimize, false),
    BSPL_DESC_BOOL("AnimateMap", false, _wlmaker_effects_config_t,
                   animate_map, animate_map, false),
    BSPL_DESC_INT64("DurationMs", false, _wlmaker_effects_config_t,
                    duration_ms, duration_ms, 180),
    BSPL_DESC_SENTINEL()
};

/** Enum descriptor for "FocusMode". */
static const bspl_enum_desc_t _wlmaker_focus_modes[] = {
    BSPL_ENUM("Click", WLMTK_FOCUS_CLICK),
    BSPL_ENUM("Sloppy", WLMTK_FOCUS_SLOPPY),
    BSPL_ENUM("FollowsMouse", WLMTK_FOCUS_FOLLOWS_MOUSE),
    BSPL_ENUM_SENTINEL(),
};

/** Enum descriptor for "Placement". */
static const bspl_enum_desc_t _wlmaker_placements[] = {
    BSPL_ENUM("Smart", WLMTK_PLACEMENT_SMART),
    BSPL_ENUM("Cascade", WLMTK_PLACEMENT_CASCADE),
    BSPL_ENUM("Center", WLMTK_PLACEMENT_CENTER),
    BSPL_ENUM("UnderPointer", WLMTK_PLACEMENT_UNDER_POINTER),
    BSPL_ENUM("Random", WLMTK_PLACEMENT_RANDOM),
    BSPL_ENUM_SENTINEL(),
};

/** The "Windows" configuration, as decoded. See @ref wlmtk_behavior_t. */
typedef struct {
    /** Focus mode. */
    int                       focus_mode;
    /** Auto-raise delay, ms; negative: never. */
    int64_t                   raise_delay_ms;
    /** Placement. */
    int                       placement;
    /** Snapping to edges & corners. */
    bool                      snapping;
    /** Snap distance, pixels. */
    int64_t                   snap_distance;
    /** Snapping to the top edge maximizes. */
    bool                      snap_top_maximizes;
    /** Show position & size while moving or resizing. */
    bool                      show_geometry;
} _wlmaker_windows_config_t;

/** Decoder for the "Windows" dict of the configuration. */
static const bspl_desc_t _wlmaker_windows_desc[] = {
    BSPL_DESC_ENUM("FocusMode", false, _wlmaker_windows_config_t,
                   focus_mode, focus_mode, WLMTK_FOCUS_CLICK,
                   _wlmaker_focus_modes),
    BSPL_DESC_INT64("RaiseDelay", false, _wlmaker_windows_config_t,
                    raise_delay_ms, raise_delay_ms, -1),
    BSPL_DESC_ENUM("Placement", false, _wlmaker_windows_config_t,
                   placement, placement, WLMTK_PLACEMENT_SMART,
                   _wlmaker_placements),
    BSPL_DESC_BOOL("Snapping", false, _wlmaker_windows_config_t,
                   snapping, snapping, true),
    BSPL_DESC_INT64("SnapDistance", false, _wlmaker_windows_config_t,
                    snap_distance, snap_distance, 8),
    BSPL_DESC_BOOL("SnapTopMaximizes", false, _wlmaker_windows_config_t,
                   snap_top_maximizes, snap_top_maximizes, true),
    BSPL_DESC_BOOL("ShowGeometry", false, _wlmaker_windows_config_t,
                   show_geometry, show_geometry, true),
    BSPL_DESC_SENTINEL()
};

/* ------------------------------------------------------------------------- */
/** Applies window behavior from the config's optional "Windows" dict. */
static void _wlmaker_apply_windows(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    _wlmaker_windows_config_t wc = {
        .focus_mode = WLMTK_FOCUS_CLICK, .raise_delay_ms = -1,
        .placement = WLMTK_PLACEMENT_SMART, .snapping = true,
        .snap_distance = 8, .snap_top_maximizes = true,
        .show_geometry = true };
    bspl_dict_t *dict_ptr = bspl_dict_get_dict(config_dict_ptr, "Windows");
    if (NULL != dict_ptr &&
        !bspl_decode_dict(dict_ptr, _wlmaker_windows_desc, &wc)) {
        bs_log(BS_WARNING, "Failed to decode 'Windows', using defaults.");
        wc = (_wlmaker_windows_config_t){
            .focus_mode = WLMTK_FOCUS_CLICK, .raise_delay_ms = -1,
            .placement = WLMTK_PLACEMENT_SMART, .snapping = true,
            .snap_distance = 8, .snap_top_maximizes = true,
        .show_geometry = true };
    }
    wlmtk_behavior_t behavior = {
        .focus_mode = (wlmtk_focus_mode_t)wc.focus_mode,
        .raise_delay_ms = (int)BS_MIN(wc.raise_delay_ms, 10000),
        .placement = (wlmtk_placement_t)wc.placement,
        .snapping = wc.snapping,
        .snap_distance = (int)wc.snap_distance,
        .snap_top_maximizes = wc.snap_top_maximizes,
        .show_geometry = wc.show_geometry,
    };
    wlmtk_behavior_set(&behavior,
                       wl_display_get_event_loop(server_ptr->wl_display_ptr));
}

/* ------------------------------------------------------------------------- */
/** Applies look & feel effects from the config's optional "Effects" dict. */
static void _wlmaker_apply_effects(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    _wlmaker_effects_config_t ec = { .active_opacity = 1.0,
                                     .inactive_opacity = 1.0,
                                     .duration_ms = 180 };
    bspl_dict_t *effects_dict_ptr = bspl_dict_get_dict(
        config_dict_ptr, "Effects");
    if (NULL != effects_dict_ptr &&
        !bspl_decode_dict(effects_dict_ptr, _wlmaker_effects_desc, &ec)) {
        bs_log(BS_WARNING, "Failed to decode 'Effects', using defaults.");
    }
    wlmtk_effects_t effects = {
        .active_opacity = ec.active_opacity,
        .inactive_opacity = ec.inactive_opacity,
        .animate_minimize = ec.animate_minimize,
        .animate_map = ec.animate_map,
        .duration_ms = (int)ec.duration_ms,
    };
    wlmtk_effects_set(&effects,
                      wl_display_get_event_loop(server_ptr->wl_display_ptr));
}

/* ------------------------------------------------------------------------- */
/** @return the theme file configured: --theme_file, or config's ThemeFile. */
static const char *_wlmaker_theme_file(bspl_dict_t *config_dict_ptr)
{
    if (NULL != wlmaker_arg_theme_file_ptr) return wlmaker_arg_theme_file_ptr;
    return bspl_dict_get_string_value(config_dict_ptr, "ThemeFile");
}

/* ------------------------------------------------------------------------- */
/** @return whether the dicts' `key` entries are the same (or both absent). */
static bool _wlmaker_same_entry(
    bspl_dict_t *a_ptr,
    bspl_dict_t *b_ptr,
    const char *key_ptr)
{
    bspl_object_t *a = bspl_dict_get(a_ptr, key_ptr);
    bspl_object_t *b = bspl_dict_get(b_ptr, key_ptr);
    if (NULL == a || NULL == b) return a == b;
    bs_dynbuf_t da = {}, db = {};
    bool same = false;
    if (bs_dynbuf_init(&da, 1024, SIZE_MAX) &&
        bs_dynbuf_init(&db, 1024, SIZE_MAX) &&
        bspl_object_write(a, &da) && bspl_object_write(b, &db)) {
        same = da.length == db.length &&
            0 == memcmp(da.data_ptr, db.data_ptr, da.length);
    }
    bs_dynbuf_fini(&da);
    bs_dynbuf_fini(&db);
    return same;
}

/* ------------------------------------------------------------------------- */
/**
 * Applies the server's sections of a re-read configuration, each only if it
 * changed: Input (Keyboard, Touchpad, Mouse, Cursor), HotCorner, ScreenLock
 * and Outputs. Re-applying unchanged ones would reset eg. the keyboard
 * layout group, or flicker the outputs.
 */
static void _wlmaker_reload_server_config(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr)
{
    bspl_dict_t *old_ptr = server_ptr->config_dict_ptr;

    bool keyboard_changed = !_wlmaker_same_entry(
        old_ptr, config_dict_ptr, "Keyboard");
    if (keyboard_changed ||
        !_wlmaker_same_entry(old_ptr, config_dict_ptr, "Touchpad") ||
        !_wlmaker_same_entry(old_ptr, config_dict_ptr, "Mouse") ||
        !_wlmaker_same_entry(old_ptr, config_dict_ptr, "Cursor")) {
        wlmim_reconfigure(server_ptr->input_manager_ptr, config_dict_ptr,
                          keyboard_changed);
    }
    if (!_wlmaker_same_entry(old_ptr, config_dict_ptr, "HotCorner")) {
        wlmaker_corner_t *corner_ptr = wlmaker_corner_create(
            bspl_dict_get_dict(config_dict_ptr, "HotCorner"),
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            server_ptr->wlr_output_layout_ptr,
            wlmim_wlr_cursor(server_ptr->input_manager_ptr),
            &wlmim_events(server_ptr->input_manager_ptr)->cursor_position_updated,
            server_ptr);
        if (NULL != corner_ptr) {
            wlmaker_corner_destroy(server_ptr->corner_ptr);
            server_ptr->corner_ptr = corner_ptr;
        } else {
            bs_log(BS_WARNING, "Invalid 'HotCorner' configuration, keeping it.");
        }
    }
    if (!_wlmaker_same_entry(old_ptr, config_dict_ptr, "ScreenLock")) {
        wlmaker_idle_monitor_reconfigure(
            server_ptr->idle_monitor_ptr,
            bspl_dict_get_dict(config_dict_ptr, "ScreenLock"));
    }
    if (!_wlmaker_same_entry(old_ptr, config_dict_ptr, "Outputs")) {
        wlmbe_backend_reconfigure(server_ptr->backend_ptr, config_dict_ptr);
    }

    server_ptr->config_dict_ptr = bspl_dict_ref(config_dict_ptr);
    bspl_dict_unref(old_ptr);
}

/* ------------------------------------------------------------------------- */
/** Re-reads the configuration: Theme, effects, key bindings, root menu. */
static void _wlmaker_reload_config(wlmaker_server_t *server_ptr)
{
    bspl_dict_t *config_dict_ptr = wlmaker_config_load(
        wlmaker_files_ptr, wlmaker_arg_config_file_ptr);
    if (NULL == config_dict_ptr) {
        bs_log(BS_WARNING, "Failed to reload configuration, keeping it.");
        return;
    }

    _wlmaker_reload_server_config(server_ptr, config_dict_ptr);
    if (NULL != wlmaker_window_rules_ptr) {
        wlmaker_window_rules_set_config(wlmaker_window_rules_ptr, config_dict_ptr);
    }

    const char *theme_file_ptr = _wlmaker_theme_file(config_dict_ptr);
    bool theme_changed = (NULL == theme_file_ptr) !=
        (NULL == wlmaker_theme_file_ptr) ||
        (NULL != theme_file_ptr &&
         0 != strcmp(theme_file_ptr, wlmaker_theme_file_ptr));
    if (theme_changed) {
        if (wlmaker_action_theme_load(server_ptr, theme_file_ptr)) {
            free(wlmaker_theme_file_ptr);
            wlmaker_theme_file_ptr = NULL;
            if (NULL != theme_file_ptr) {
                wlmaker_theme_file_ptr = logged_strdup(theme_file_ptr);
            }
            bs_log(BS_INFO, "Applied theme %s",
                   NULL != theme_file_ptr ? theme_file_ptr : "(default)");
        } else {
            bs_log(BS_WARNING, "Failed to load theme %s", theme_file_ptr);
        }
    }

    _wlmaker_apply_effects(server_ptr, config_dict_ptr);
    _wlmaker_apply_windows(server_ptr, config_dict_ptr);

    // Key bindings: Replace. If the new ones fail, restore the previous.
    wlmaker_action_unbind_keys(wlmaker_action_handle_ptr);
    wlmaker_action_handle_ptr = wlmaker_action_bind_keys(
        server_ptr,
        bspl_dict_get_dict(config_dict_ptr, wlmaker_action_config_dict_key),
        wlmaker_server_options.bind_with_logo);
    if (NULL == wlmaker_action_handle_ptr) {
        bs_log(BS_WARNING, "Failed to bind new keys, restoring previous.");
        wlmaker_action_handle_ptr = wlmaker_action_bind_keys(
            server_ptr,
            bspl_dict_get_dict(wlmaker_config_dict_ptr,
                               wlmaker_action_config_dict_key),
            wlmaker_server_options.bind_with_logo);
        bspl_dict_unref(config_dict_ptr);
    } else {
        bspl_dict_unref(wlmaker_config_dict_ptr);
        wlmaker_config_dict_ptr = config_dict_ptr;
    }

    // Root menu: Re-create from its file, unless it's open right now.
    if (NULL != server_ptr->root_menu_ptr &&
        NULL == wlmtk_window_get_workspace(
            wlmaker_root_menu_window(server_ptr->root_menu_ptr))) {
        wlmaker_root_menu_destroy(server_ptr->root_menu_ptr);
        server_ptr->root_menu_ptr = wlmaker_root_menu_create(
            server_ptr,
            wlmaker_arg_root_menu_file_ptr,
            wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
            wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
        if (NULL != server_ptr->root_menu_ptr) {
            wlmtk_menu_set_open(
                wlmaker_root_menu_menu(server_ptr->root_menu_ptr), false);
        } else {
            bs_log(BS_WARNING, "Failed to re-create the root menu.");
        }
    }
}

static void _wlmaker_reload_workspaces(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr);

/* ------------------------------------------------------------------------- */
/** Re-reads the state: Re-creates the dock, moves the clip and icon area. */
static void _wlmaker_reload_state(wlmaker_server_t *server_ptr)
{
    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        wlmaker_files_ptr, wlmaker_arg_state_file_ptr);
    if (NULL == state_dict_ptr) {
        bs_log(BS_WARNING, "Failed to reload state, keeping it.");
        return;
    }

    wlmaker_dock_t *dock_ptr = wlmaker_dock_create(
        server_ptr, state_dict_ptr, server_ptr->style_ptr);
    if (NULL != dock_ptr) {
        if (NULL != wlmaker_dock_ptr) wlmaker_dock_destroy(wlmaker_dock_ptr);
        wlmaker_dock_ptr = dock_ptr;
    } else {
        bs_log(BS_WARNING, "Failed to re-create the dock, keeping it.");
    }
    if (NULL != wlmaker_clip_ptr) {
        wlmaker_clip_set_state(wlmaker_clip_ptr, state_dict_ptr);
    }
    if (NULL != wlmaker_icon_area_ptr) {
        wlmaker_icon_area_set_state(wlmaker_icon_area_ptr, state_dict_ptr);
    }
    _wlmaker_reload_workspaces(server_ptr, state_dict_ptr);

    bspl_dict_unref(wlmaker_state_dict_ptr);
    wlmaker_state_dict_ptr = state_dict_ptr;
}

/* ------------------------------------------------------------------------- */
/**
 * Handles SIGHUP: Re-reads configuration and state, and applies them. Sent
 * by wmaker-next-settings, so changes show without restarting the session.
 */
static int _wlmaker_handle_sighup(
    __UNUSED__ int signal_number,
    void *data_ptr)
{
    wlmaker_server_t *server_ptr = data_ptr;
    bs_log(BS_INFO, "SIGHUP: Reloading configuration and state.");
    _wlmaker_reload_config(server_ptr);
    _wlmaker_reload_state(server_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/** Applies the workspace's wallpaper. "~/" in the path is expanded. */
static void _wlmaker_set_wallpaper(
    bs_dllist_node_t *bg_dlnode_ptr,
    const wlmaker_workspace_style_t *s_ptr)
{
    char *path_ptr = NULL;
    if (0 < strlen(s_ptr->image)) {
        path_ptr = bs_file_resolve_path(s_ptr->image, NULL);
        if (NULL == path_ptr) {
            bs_log(BS_WARNING, "Wallpaper \"%s\" not found.", s_ptr->image);
        }
    }
    wlmaker_background_dlnode_set_image(
        bg_dlnode_ptr, path_ptr, s_ptr->image_mode);
    free(path_ptr);
}

/* ------------------------------------------------------------------------- */
/** Adds a workspace with the given name & color (0: the theme's). */
static bool _wlmaker_add_workspace(
    wlmaker_server_t *server_ptr,
    wlmaker_workspace_style_t *s_ptr)
{
    wlmtk_workspace_t *workspace_ptr = wlmtk_workspace_create(
        server_ptr->wlr_output_layout_ptr,
        s_ptr->name,
        &server_ptr->style_ptr->tile);
    if (NULL == workspace_ptr) {
        bs_log(BS_ERROR, "Failed wlmtk_workspace_create(\"%s\")", s_ptr->name);
        return false;
    }

    if (s_ptr->color == 0) {
        s_ptr->color = server_ptr->style_ptr->background_color;
    }
    bs_dllist_node_t *bg_dlnode_ptr = wlmaker_background_create(
        workspace_ptr,
        server_ptr->wlr_output_layout_ptr,
        s_ptr->color);
    if (NULL == bg_dlnode_ptr) {
        bs_log(BS_ERROR, "Failed wlmaker_background()");
        wlmtk_workspace_destroy(workspace_ptr);
        return false;
    }
    bs_dllist_push_back(&server_ptr->backgrounds, bg_dlnode_ptr);
    _wlmaker_set_wallpaper(bg_dlnode_ptr, s_ptr);
    wlmtk_root_add_workspace(server_ptr->root_ptr, workspace_ptr);
    return true;
}

/* ------------------------------------------------------------------------- */
/** Creates workspaces as configured in the state dict. */
bool create_workspaces(
    bspl_dict_t *state_dict_ptr,
    wlmaker_server_t *server_ptr)
{
    bspl_array_t *array_ptr = bspl_dict_get_array(
        state_dict_ptr, "Workspaces");
    if (NULL == array_ptr) return false;

    bool rv = true;
    for (size_t i = 0; i < bspl_array_size(array_ptr); ++i) {
        bspl_dict_t *dict_ptr = bspl_dict_from_object(
            bspl_array_at(array_ptr, i));
        if (NULL == dict_ptr) {
            bs_log(BS_ERROR, "Array element in \"Workspaces\" is not a dict");
            rv = false;
            break;
        }

        wlmaker_workspace_style_t s;
        if (!bspl_decode_dict(dict_ptr, wlmaker_workspace_style_desc, &s)) {
            bs_log(BS_ERROR,
                   "Failed to decode dict element %zu in \"Workspace\"",
                   i);
            rv = false;
            break;
        }

        if (!_wlmaker_add_workspace(server_ptr, &s)) {
            rv = false;
            break;
        }
    }

    return rv;
}

/* ------------------------------------------------------------------------- */
/**
 * Applies the state's `Workspaces`: Renames and recolors the existing ones,
 * adds missing ones, and removes surplus ones from the end -- unless one is
 * the current workspace, or holds windows: Those stay.
 */
static void _wlmaker_reload_workspaces(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *state_dict_ptr)
{
    bspl_array_t *array_ptr = bspl_dict_get_array(state_dict_ptr, "Workspaces");
    if (NULL == array_ptr || 0 == bspl_array_size(array_ptr)) return;

    bs_dllist_t *workspaces_ptr = wlmtk_root_get_workspaces(server_ptr->root_ptr);
    bs_dllist_node_t *ws_dlnode_ptr = workspaces_ptr->head_ptr;
    bs_dllist_node_t *bg_dlnode_ptr = server_ptr->backgrounds.head_ptr;
    size_t n = bspl_array_size(array_ptr);
    for (size_t i = 0; i < n; ++i) {
        wlmaker_workspace_style_t s;
        bspl_dict_t *dict_ptr = bspl_dict_from_object(bspl_array_at(array_ptr, i));
        if (NULL == dict_ptr ||
            !bspl_decode_dict(dict_ptr, wlmaker_workspace_style_desc, &s)) {
            bs_log(BS_WARNING, "Invalid workspace %zu in state, skipping.", i);
            if (NULL != ws_dlnode_ptr) ws_dlnode_ptr = ws_dlnode_ptr->next_ptr;
            if (NULL != bg_dlnode_ptr) bg_dlnode_ptr = bg_dlnode_ptr->next_ptr;
            continue;
        }
        if (NULL == ws_dlnode_ptr) {
            _wlmaker_add_workspace(server_ptr, &s);
            continue;
        }
        wlmtk_workspace_set_name(wlmtk_workspace_from_dlnode(ws_dlnode_ptr),
                                 s.name);
        if (0 == s.color) s.color = server_ptr->style_ptr->background_color;
        if (NULL != bg_dlnode_ptr) {
            wlmaker_background_dlnode_set_color(bg_dlnode_ptr, &s.color);
            _wlmaker_set_wallpaper(bg_dlnode_ptr, &s);
            bg_dlnode_ptr = bg_dlnode_ptr->next_ptr;
        }
        ws_dlnode_ptr = ws_dlnode_ptr->next_ptr;
    }

    // Surplus workspaces: Remove from the end, as far as permitted.
    while (bs_dllist_size(workspaces_ptr) > n) {
        size_t before = bs_dllist_size(workspaces_ptr);
        wlmtk_root_destroy_last_workspace(server_ptr->root_ptr);
        if (bs_dllist_size(workspaces_ptr) == before) {
            bs_log(BS_INFO, "Keeping workspace %zu: current, or has windows.",
                   before);
            break;
        }
        bs_dllist_node_t *bg_ptr = bs_dllist_pop_back(&server_ptr->backgrounds);
        if (NULL != bg_ptr) wlmaker_background_dlnode_destroy(bg_ptr, NULL);
    }

    if (NULL != wlmaker_clip_ptr) wlmaker_clip_refresh(wlmaker_clip_ptr);
}

/* == Main program ========================================================= */

/* ------------------------------------------------------------------------- */
/** State of scripted test actions, see @ref _wlmaker_test_actions_start. */
typedef struct {
    /** The server. */
    wlmaker_server_t          *server_ptr;
    /** Remaining script: "<delay ms>:<Action>,..." */
    char                      *script_ptr;
    /** Timer. */
    struct wl_event_source    *timer_ptr;
    /** Action due when the timer fires. */
    int                       action;
} _wlmaker_test_actions_t;

static int _wlmaker_test_actions_timer(void *data_ptr);

/* ------------------------------------------------------------------------- */
/** Schedules the next scripted action, if any. */
static void _wlmaker_test_actions_next(_wlmaker_test_actions_t *ta_ptr)
{
    char *item_ptr = ta_ptr->script_ptr;
    if (NULL == item_ptr || 0 == *item_ptr) return;
    char *rest_ptr = strchr(item_ptr, ',');
    if (NULL != rest_ptr) *rest_ptr++ = '\0';
    ta_ptr->script_ptr = rest_ptr;

    char *name_ptr = strchr(item_ptr, ':');
    if (NULL == name_ptr) return;
    *name_ptr++ = '\0';
    for (const bspl_enum_desc_t *d = wlmaker_action_desc; NULL != d->name_ptr;
         ++d) {
        if (0 != strcmp(d->name_ptr, name_ptr)) continue;
        ta_ptr->action = d->value;
        wl_event_source_timer_update(ta_ptr->timer_ptr, atoi(item_ptr));
        return;
    }
    bs_log(BS_ERROR, "WLMAKER_TEST_ACTIONS: Unknown action '%s'", name_ptr);
}

/* ------------------------------------------------------------------------- */
/** Timer callback: Executes the due action, schedules the next. */
int _wlmaker_test_actions_timer(void *data_ptr)
{
    _wlmaker_test_actions_t *ta_ptr = data_ptr;
    bs_log(BS_INFO, "WLMAKER_TEST_ACTIONS: Executing action %d", ta_ptr->action);
    wlmaker_action_execute(ta_ptr->server_ptr, ta_ptr->action, NULL);
    _wlmaker_test_actions_next(ta_ptr);
    return 0;
}

/* ------------------------------------------------------------------------- */
/**
 * For automated tests without input devices: Runs the actions listed in
 * `WLMAKER_TEST_ACTIONS` ("<delay ms>:<Action>,...", eg. "500:TaskNext"),
 * each after its delay. Inactive when not set.
 */
static void _wlmaker_test_actions_start(wlmaker_server_t *server_ptr)
{
    const char *env_ptr = getenv("WLMAKER_TEST_ACTIONS");
    if (NULL == env_ptr || 0 == *env_ptr) return;
    // Lives until exit.
    static _wlmaker_test_actions_t ta;
    ta.server_ptr = server_ptr;
    ta.script_ptr = logged_strdup(env_ptr);
    ta.timer_ptr = wl_event_loop_add_timer(
        wl_display_get_event_loop(server_ptr->wl_display_ptr),
        _wlmaker_test_actions_timer, &ta);
    _wlmaker_test_actions_next(&ta);
}

/* ------------------------------------------------------------------------- */
/**
 * Runs in each child right after fork(), before it execs: Gives it a clean
 * signal state. wlmaker blocks SIGCHLD for its event loop (signalfd); the
 * mask survives exec, so launched apps couldn't wait for their own children
 * (Konsole failed to start its shell, after a long timeout).
 */
static void _wlmaker_child_reset_signals(void)
{
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    // Handlers would be reset by exec; ignored signals (SIGPIPE) are not.
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/** The main program. */
int main(__UNUSED__ int argc, __UNUSED__ const char **argv)
{
    // Every process we start (apps, menus, Xwayland) starts unblocked.
    pthread_atfork(NULL, NULL, _wlmaker_child_reset_signals);

    wlmaker_foreign_toplevel_t *foreign_toplevel_ptr = NULL;
    wlmaker_task_list_t       *task_list_ptr = NULL;
    int                       rv = EXIT_SUCCESS;

    if (!wlmaker_backtrace_setup(argv[0])) return EXIT_FAILURE;

    rv = regcomp(
        &wlmaker_wlr_log_regex,
        wlmaker_wlr_log_regex_string,
        REG_EXTENDED);
    if (0 != rv) {
        char err_buf[512];
        regerror(rv, &wlmaker_wlr_log_regex, err_buf, sizeof(err_buf));
        bs_log(BS_ERROR, "Failed compiling regular expression: %s", err_buf);
        return EXIT_FAILURE;
    }

    wlr_log_init(WLR_DEBUG, wlr_to_bs_log);
    bs_log_severity = BS_INFO;  // Will be overwritten in bs_arg_parse().
    BS_ASSERT(bs_ptr_stack_init(&wlmaker_subprocess_stack));

    if (!bs_arg_parse(wlmaker_args, BS_ARG_MODE_EXTRA_ARGS, &argc, argv)) {
        fprintf(stderr, "Failed to parse commandline arguments.\n");
        bs_arg_print_usage(stderr, wlmaker_args);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--help")) {
            bs_arg_print_usage(stdout, wlmaker_args);
            return EXIT_SUCCESS;
        } else if (0 == strcmp(argv[i], "--version")) {
            fprintf(stdout, "wlmaker version %s.%s (%s)\n",
                    wlmaker_version_major,
                    wlmaker_version_minor,
                    wlmaker_version_full);
            return EXIT_SUCCESS;
        } else {
            bs_log(BS_ERROR, "Unhandled extra argument \"%s\"", argv[i]);
            return EXIT_FAILURE;
        }
    }
    bs_log(BS_INFO, "Starting wlmaker %s.%s (%s)",
           wlmaker_version_major,
           wlmaker_version_minor,
           wlmaker_version_full);

    wlmaker_files_t *files_ptr = wlmaker_files_create("wlmaker");
    wlmaker_files_ptr = files_ptr;
    if (NULL == files_ptr) {
        bs_log(BS_ERROR, "Failed wlmaker_files_create(\"wlmaker\")");
        return EXIT_FAILURE;
    }

    bspl_dict_t *config_dict_ptr = wlmaker_config_load(
        files_ptr, wlmaker_arg_config_file_ptr);
    wlmaker_config_dict_ptr = config_dict_ptr;
    if (NULL == config_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize configuration.\n");
        return EXIT_FAILURE;
    }

    bspl_dict_t *state_dict_ptr = wlmaker_state_load(
        files_ptr, wlmaker_arg_state_file_ptr);
    wlmaker_state_dict_ptr = state_dict_ptr;
    if (NULL == state_dict_ptr) {
        fprintf(stderr, "Failed to load & initialize state.\n");
        return EXIT_FAILURE;
    }

    const char *theme_file_ptr = _wlmaker_theme_file(config_dict_ptr);
    if (NULL != theme_file_ptr) {
        wlmaker_theme_file_ptr = logged_strdup(theme_file_ptr);
    }
    wlmaker_config_style_t style = {};
    if (!wlmaker_theme_load(files_ptr, theme_file_ptr, &style)) {
        fprintf(stderr, "Failed to load & initialize theme.\n");
        return EXIT_FAILURE;
    }

    wlmaker_server_t *server_ptr = wlmaker_server_create(
        config_dict_ptr, files_ptr, &style, &wlmaker_server_options);
    if (NULL == server_ptr) return EXIT_FAILURE;
    // Before the taskbar & others listen to mapping: They see the attributes.
    wlmaker_window_rules_ptr = wlmaker_window_rules_create(
        server_ptr, config_dict_ptr);

    _wlmaker_apply_effects(server_ptr, config_dict_ptr);
    _wlmaker_apply_windows(server_ptr, config_dict_ptr);
    struct wl_event_source *sighup_source_ptr = wl_event_loop_add_signal(
        wl_display_get_event_loop(server_ptr->wl_display_ptr),
        SIGHUP, _wlmaker_handle_sighup, server_ptr);

    // TODO(kaeser@gubbe.ch): Uh, that's ugly...
    server_ptr->root_menu_ptr = wlmaker_root_menu_create(
        server_ptr,
        wlmaker_arg_root_menu_file_ptr,
        wlmtk_window_style_to_ref(server_ptr->style_ptr->window_style_ptr),
        wlmtk_menu_style_to_ref(server_ptr->style_ptr->menu_style_ptr));
    if (NULL == server_ptr->root_menu_ptr) {
        return EXIT_FAILURE;
    }
    wlmtk_menu_set_open(
        wlmaker_root_menu_menu(server_ptr->root_menu_ptr),
        false);
    server_ptr->window_list_ptr = wlmaker_window_list_create(server_ptr);
    if (NULL == server_ptr->window_list_ptr) {
        return EXIT_FAILURE;
    }
    server_ptr->popup_menu_ptr = wlmaker_popup_menu_create(server_ptr);
    if (NULL == server_ptr->popup_menu_ptr) {
        return EXIT_FAILURE;
    }
    if (!wlmaker_tooltip_init(
            wl_display_get_event_loop(server_ptr->wl_display_ptr),
            server_ptr->wlr_output_layout_ptr,
            &server_ptr->wlr_scene_ptr->tree)) {
        return EXIT_FAILURE;
    }

    wlmaker_action_handle_ptr = wlmaker_action_bind_keys(
        server_ptr,
        bspl_dict_get_dict(config_dict_ptr, wlmaker_action_config_dict_key),
        wlmaker_server_options.bind_with_logo);
    if (NULL == wlmaker_action_handle_ptr) {
        bs_log(BS_ERROR, "Failed to bind keys.");
        return EXIT_FAILURE;
    }

    if (!create_workspaces(state_dict_ptr, server_ptr)) {
        return EXIT_FAILURE;
    }

    rv = EXIT_SUCCESS;
    if (wlr_backend_start(wlmbe_backend_wlr(server_ptr->backend_ptr))) {

        if (0 >= wlmbe_num_outputs(server_ptr->wlr_output_layout_ptr)) {
            bs_log(BS_ERROR, "No outputs available!");
            return EXIT_FAILURE;
        }

        bs_log(BS_INFO, "Starting Wayland compositor for server %p at %s ...",
               server_ptr, server_ptr->wl_socket_name_ptr);

        setenv("WAYLAND_DISPLAY", server_ptr->wl_socket_name_ptr, true);

        bspl_array_t *autostarted_ptr = bspl_dict_get_array(
            config_dict_ptr, "Autostart");
        if (NULL != autostarted_ptr) {
            for (size_t i = 0; i < bspl_array_size(autostarted_ptr); ++i) {
                const char *cmd_ptr = bspl_array_string_value_at(
                    autostarted_ptr, i);
                if (!start_subprocess(cmd_ptr)) return EXIT_FAILURE;
            }
        }

        wlmaker_dock_ptr = wlmaker_dock_create(
            server_ptr, state_dict_ptr, &style);
        wlmaker_clip_ptr = wlmaker_clip_create(
            server_ptr, state_dict_ptr, &style);
        wlmaker_icon_area_ptr = wlmaker_icon_area_create(server_ptr, &style);
        if (NULL != wlmaker_icon_area_ptr) {
            wlmaker_icon_area_set_state(wlmaker_icon_area_ptr, state_dict_ptr);
        }
        foreign_toplevel_ptr = wlmaker_foreign_toplevel_create(server_ptr);
        if (NULL == foreign_toplevel_ptr) {
            bs_log(BS_WARNING, "Failed to create foreign toplevel manager.");
        }
        task_list_ptr = wlmaker_task_list_create(
            server_ptr, &style.task_list);
        if (NULL == wlmaker_dock_ptr || NULL == wlmaker_clip_ptr ||
            NULL == wlmaker_icon_area_ptr ||
            NULL == task_list_ptr) {
            bs_log(BS_ERROR,
                   "Failed to create dock, clip, icon area or task list.");
        } else {
            _wlmaker_test_actions_start(server_ptr);
            wl_display_run(server_ptr->wl_display_ptr);
        }

    } else {
        bs_log(BS_ERROR, "Failed wlmaker_server_start()");
        rv = EXIT_FAILURE;
    }

    if (NULL != task_list_ptr) wlmaker_task_list_destroy(task_list_ptr);
    if (NULL != foreign_toplevel_ptr) {
        wlmaker_foreign_toplevel_destroy(foreign_toplevel_ptr);
    }
    if (NULL != sighup_source_ptr) wl_event_source_remove(sighup_source_ptr);
    if (NULL != wlmaker_window_rules_ptr) {
        wlmaker_window_rules_destroy(wlmaker_window_rules_ptr);
    }
    wlmaker_tooltip_fini();
    if (NULL != server_ptr->popup_menu_ptr) {
        wlmaker_popup_menu_destroy(server_ptr->popup_menu_ptr);
        server_ptr->popup_menu_ptr = NULL;
    }
    if (NULL != wlmaker_icon_area_ptr) {
        wlmaker_icon_area_destroy(wlmaker_icon_area_ptr);
    }
    if (NULL != wlmaker_clip_ptr) wlmaker_clip_destroy(wlmaker_clip_ptr);
    if (NULL != wlmaker_dock_ptr) wlmaker_dock_destroy(wlmaker_dock_ptr);
    wlmaker_action_unbind_keys(wlmaker_action_handle_ptr);
    bspl_array_unref(server_ptr->root_menu_array_ptr);
    wlmaker_server_destroy(server_ptr);

    bs_subprocess_t *sp_ptr;
    while (NULL != (sp_ptr = bs_ptr_stack_pop(&wlmaker_subprocess_stack))) {
        bs_subprocess_destroy(sp_ptr);
    }
    bs_ptr_stack_fini(&wlmaker_subprocess_stack);

    bspl_decoded_destroy(wlmaker_config_style_desc, &style);
    bspl_dict_unref(wlmaker_config_dict_ptr);
    bspl_dict_unref(wlmaker_state_dict_ptr);
    free(wlmaker_theme_file_ptr);
    free(wlmaker_arg_config_file_ptr);
    free(wlmaker_arg_state_file_ptr);
    regfree(&wlmaker_wlr_log_regex);
    return rv;
}

/* == End of wlmaker.c ===================================================== */
