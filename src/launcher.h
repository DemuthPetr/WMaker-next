/* ========================================================================= */
/**
 * @file launcher.h
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
#ifndef __LAUNCHER_H__
#define __LAUNCHER_H__

#include <libbase/libbase.h>
#include <libbase/plist.h>

#include "files.h"
#include "subprocess_monitor.h"
#include "toolkit/toolkit.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Forward declaration: Launcher handle. */
typedef struct _wlmaker_launcher_t wlmaker_launcher_t;

/**
 * Creates an application launcher, configured from a plist dict.
 *
 * @param style_ptr
 * @param dict_ptr
 * @param monitor_ptr
 * @param files_ptr
 *
 * @return Pointer to the launcher handle or NULL on error.
 */
wlmaker_launcher_t *wlmaker_launcher_create_from_plist(
    const struct wlmtk_tile_style *style_ptr,
    bspl_dict_t *dict_ptr,
    wlmaker_subprocess_monitor_t *monitor_ptr,
    wlmaker_files_t *files_ptr);

/**
 * Destroys the application launcher.
 *
 * @param launcher_ptr
 */
void wlmaker_launcher_destroy(wlmaker_launcher_t *launcher_ptr);

/**
 * Sets the server, needed for launchers executing an `Action`.
 *
 * @param launcher_ptr
 * @param server_ptr
 */
void wlmaker_launcher_set_server(
    wlmaker_launcher_t *launcher_ptr,
    wlmaker_server_t *server_ptr);

/**
 * Enables drag & drop: `drop_fn` is called when the launcher is dragged and
 * released, with the pointer's layout coordinates. Dragging won't launch.
 *
 * @param launcher_ptr
 * @param drop_fn
 * @param ud_ptr
 */
void wlmaker_launcher_set_drop_handler(
    wlmaker_launcher_t *launcher_ptr,
    void (*drop_fn)(void *ud_ptr, wlmaker_launcher_t *launcher_ptr,
                    double x, double y),
    void *ud_ptr);

/** @return The launcher's plist definition. */
bspl_dict_t *wlmaker_launcher_dict(wlmaker_launcher_t *launcher_ptr);

/** Sets the handler of right clicks: Opens the dock's menu for it. */
void wlmaker_launcher_set_menu_handler(
    wlmaker_launcher_t *launcher_ptr,
    void (*menu_fn)(void *ud_ptr, wlmaker_launcher_t *launcher_ptr),
    void *ud_ptr);

/** Launches the application (or runs the action), as a left click does. */
void wlmaker_launcher_launch(wlmaker_launcher_t *launcher_ptr);

/** @return whether windows of applications it launched are mapped. */
bool wlmaker_launcher_is_running(wlmaker_launcher_t *launcher_ptr);

/** @return whether it has a command (not just an action). */
bool wlmaker_launcher_has_command(wlmaker_launcher_t *launcher_ptr);

/** @return whether it is protected from removal ('Lock'). */
bool wlmaker_launcher_is_locked(wlmaker_launcher_t *launcher_ptr);

/** @return whether it launches as the session starts ('AutoLaunch'). */
bool wlmaker_launcher_auto_launches(wlmaker_launcher_t *launcher_ptr);

/** @return whether any window it launched is shown (not minimized). */
bool wlmaker_launcher_any_shown(wlmaker_launcher_t *launcher_ptr);

/**
 * Hides (minimizes) the windows it launched, if any is shown; else shows
 * them again. @return true if it hid them.
 */
bool wlmaker_launcher_toggle_hidden(wlmaker_launcher_t *launcher_ptr);

/** @return A pointer to the @ref wlmtk_tile_t superclass of `launcher_ptr`. */
wlmtk_tile_t *wlmaker_launcher_tile(wlmaker_launcher_t *launcher_ptr);

/** Unit test set. */
extern const bs_test_set_t wlmaker_launcher_test_set;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __LAUNCHER_H__ */
/* == End of launcher.h ==================================================== */
