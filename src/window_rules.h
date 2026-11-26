/* ========================================================================= */
/**
 * @file window_rules.h
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
#ifndef __WINDOW_RULES_H__
#define __WINDOW_RULES_H__

#include <libbase/plist.h>

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/** Per-application window rules: Config.plist's WindowAttributes. */
typedef struct _wlmaker_window_rules_t wlmaker_window_rules_t;

/**
 * Creates the rules, applied to windows as they are mapped. Create it before
 * other listeners of window mapping (eg. the taskbar's), so they see the
 * window's attributes.
 *
 * @param server_ptr
 * @param config_dict_ptr     The configuration; reads its WindowAttributes.
 *
 * @return The rules handle, or NULL on error.
 */
wlmaker_window_rules_t *wlmaker_window_rules_create(
    wlmaker_server_t *server_ptr,
    bspl_dict_t *config_dict_ptr);

/**
 * Takes new rules (eg. after a configuration reload), and applies their
 * lasting attributes to all windows. Start attributes (StartMaximized, ...)
 * only apply to newly mapped windows.
 *
 * @param rules_ptr
 * @param config_dict_ptr
 */
void wlmaker_window_rules_set_config(
    wlmaker_window_rules_t *rules_ptr,
    bspl_dict_t *config_dict_ptr);

/** Destroys the rules. */
void wlmaker_window_rules_destroy(wlmaker_window_rules_t *rules_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __WINDOW_RULES_H__ */
/* == End of window_rules.h ================================================ */
