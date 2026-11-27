/* ========================================================================= */
/**
 * @file session.h
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
#ifndef __SESSION_H__
#define __SESSION_H__

#include <stdbool.h>

/** Forward declaration: Session state. */
typedef struct _wlmaker_session_t wlmaker_session_t;

#include "server.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * Session saving, as Window Maker's "save session on exit": Saves the
 * application windows (app ID, workspace, position, size, state) to
 * `Session.plist` in wlmaker's config dir. At the next start, windows of the
 * saved applications are put back where they were, once each, as they map.
 * Relaunching the applications is up to the session (wmaker-next-settings
 * --restore-session, from the autostart).
 *
 * @param server_ptr
 *
 * @return The session handle, or NULL on error.
 */
wlmaker_session_t *wlmaker_session_create(wlmaker_server_t *server_ptr);

/** Destroys the session handle. */
void wlmaker_session_destroy(wlmaker_session_t *session_ptr);

/** Saves the current windows. @return true on success. */
bool wlmaker_session_save(wlmaker_session_t *session_ptr);

/** Removes the saved session. */
void wlmaker_session_clear(wlmaker_session_t *session_ptr);

/** Whether the configuration asks to save the session on exit. */
bool wlmaker_session_save_on_exit(wlmaker_session_t *session_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif /* __SESSION_H__ */
/* == End of session.h ===================================================== */
