/* ========================================================================= */
/**
 * @file behavior.c
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

#include "behavior.h"

#include <libbase/libbase.h>
#include <wayland-server-core.h>

#include "window.h"
#include "workspace.h"

/* == Data ================================================================= */

/** Current behavior. */
static wlmtk_behavior_t _wlmtk_behavior = {
    .focus_mode = WLMTK_FOCUS_CLICK,
    .raise_delay_ms = -1,
    .placement = WLMTK_PLACEMENT_SMART,
    .snapping = true,
    .snap_distance = 8,
    .snap_top_maximizes = true,
    .show_geometry = true,
};
/** Event loop, for the auto-raise timer. */
static struct wl_event_loop *_wlmtk_behavior_loop_ptr;
/** Auto-raise timer. Created on first use. */
static struct wl_event_source *_wlmtk_behavior_timer_ptr;
/** Workspace whose activated window the timer raises. */
static wlmtk_workspace_t *_wlmtk_behavior_raise_workspace_ptr;

static int _wlmtk_behavior_handle_timer(void *data_ptr);

/* == Exported methods ===================================================== */

/* ------------------------------------------------------------------------- */
void wlmtk_behavior_set(
    const wlmtk_behavior_t *behavior_ptr,
    struct wl_event_loop *wl_event_loop_ptr)
{
    _wlmtk_behavior = *behavior_ptr;
    _wlmtk_behavior.snap_distance = BS_MIN(BS_MAX(
        _wlmtk_behavior.snap_distance, 1), 200);
    if (NULL != wl_event_loop_ptr) _wlmtk_behavior_loop_ptr = wl_event_loop_ptr;
}

/* ------------------------------------------------------------------------- */
const wlmtk_behavior_t *wlmtk_behavior_get(void)
{
    return &_wlmtk_behavior;
}

/* ------------------------------------------------------------------------- */
void wlmtk_behavior_schedule_raise(wlmtk_workspace_t *workspace_ptr)
{
    if (0 > _wlmtk_behavior.raise_delay_ms) return;
    if (0 == _wlmtk_behavior.raise_delay_ms ||
        NULL == _wlmtk_behavior_loop_ptr) {
        _wlmtk_behavior_raise_workspace_ptr = workspace_ptr;
        _wlmtk_behavior_handle_timer(NULL);
        return;
    }
    if (NULL == _wlmtk_behavior_timer_ptr) {
        _wlmtk_behavior_timer_ptr = wl_event_loop_add_timer(
            _wlmtk_behavior_loop_ptr, _wlmtk_behavior_handle_timer, NULL);
        if (NULL == _wlmtk_behavior_timer_ptr) return;
    }
    _wlmtk_behavior_raise_workspace_ptr = workspace_ptr;
    wl_event_source_timer_update(_wlmtk_behavior_timer_ptr,
                                 _wlmtk_behavior.raise_delay_ms);
}

/* ------------------------------------------------------------------------- */
void wlmtk_behavior_cancel_raise(wlmtk_workspace_t *workspace_ptr)
{
    if (_wlmtk_behavior_raise_workspace_ptr != workspace_ptr) return;
    _wlmtk_behavior_raise_workspace_ptr = NULL;
    if (NULL != _wlmtk_behavior_timer_ptr) {
        wl_event_source_timer_update(_wlmtk_behavior_timer_ptr, 0);
    }
}

/* == Local (static) methods =============================================== */

/* ------------------------------------------------------------------------- */
/** Timer: Raises the activated window of the workspace. */
int _wlmtk_behavior_handle_timer(__UNUSED__ void *data_ptr)
{
    wlmtk_workspace_t *workspace_ptr = _wlmtk_behavior_raise_workspace_ptr;
    _wlmtk_behavior_raise_workspace_ptr = NULL;
    if (NULL == workspace_ptr) return 0;
    wlmtk_window_t *window_ptr = wlmtk_workspace_get_activated_window(
        workspace_ptr);
    if (NULL != window_ptr && !wlmtk_window_is_fullscreen(window_ptr)) {
        wlmtk_workspace_raise_window(workspace_ptr, window_ptr);
    }
    return 0;
}

/* == End of behavior.c ==================================================== */
