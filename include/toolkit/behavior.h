/* ========================================================================= */
/**
 * @file behavior.h
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
#ifndef __WLMTK_BEHAVIOR_H__
#define __WLMTK_BEHAVIOR_H__

#include <stdbool.h>

struct wl_event_loop;

/** Forward declaration: Workspace. */
typedef struct _wlmtk_workspace_t wlmtk_workspace_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** How windows get the keyboard focus. */
typedef enum {
    /** Clicking a window focuses (and raises) it. */
    WLMTK_FOCUS_CLICK = 0,
    /** The window under the pointer gets focus; the desktop keeps it. */
    WLMTK_FOCUS_SLOPPY = 1,
    /** The window under the pointer gets focus; the desktop takes it away. */
    WLMTK_FOCUS_FOLLOWS_MOUSE = 2,
} wlmtk_focus_mode_t;

/** Where new windows are placed. */
typedef enum {
    /** First free space without overlapping others; else cascading. */
    WLMTK_PLACEMENT_SMART = 0,
    /** Cascading from the top left. */
    WLMTK_PLACEMENT_CASCADE = 1,
    /** Centered on the output. */
    WLMTK_PLACEMENT_CENTER = 2,
    /** Centered at the pointer. */
    WLMTK_PLACEMENT_UNDER_POINTER = 3,
    /** Somewhere random. */
    WLMTK_PLACEMENT_RANDOM = 4,
} wlmtk_placement_t;

/** Window behavior. Defaults: Click-to-focus, smart placement, snapping. */
typedef struct {
    /** Focus mode. */
    wlmtk_focus_mode_t        focus_mode;
    /**
     * With a focus-follows-pointer mode: Raise the focused window after this
     * many milliseconds. Negative: Never.
     */
    int                       raise_delay_ms;
    /** Placement of new windows. */
    wlmtk_placement_t         placement;
    /** Whether dragging a window to a screen edge or corner tiles it. */
    bool                      snapping;
    /** Distance from the edge, in pixels, that triggers snapping. */
    int                       snap_distance;
    /** Whether snapping to the top edge maximizes (else: top half). */
    bool                      snap_top_maximizes;
} wlmtk_behavior_t;

/**
 * Sets the window behavior. Auto-raise needs the event loop, for its timer.
 *
 * @param behavior_ptr
 * @param wl_event_loop_ptr
 */
void wlmtk_behavior_set(
    const wlmtk_behavior_t *behavior_ptr,
    struct wl_event_loop *wl_event_loop_ptr);

/** @return The current window behavior. */
const wlmtk_behavior_t *wlmtk_behavior_get(void);

/**
 * Raises the workspace's activated window after the raise delay, unless
 * the focus moves on before. See @ref wlmtk_behavior_t::raise_delay_ms.
 *
 * @param workspace_ptr
 */
void wlmtk_behavior_schedule_raise(wlmtk_workspace_t *workspace_ptr);

/**
 * Cancels a scheduled raise for the workspace (eg. it is being destroyed).
 *
 * @param workspace_ptr
 */
void wlmtk_behavior_cancel_raise(wlmtk_workspace_t *workspace_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WLMTK_BEHAVIOR_H__ */
/* == End of behavior.h ==================================================== */
