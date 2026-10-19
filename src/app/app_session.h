#pragma once

#include "uuidstr.h"
#include "client.h"
#include "stream/session.h"

typedef struct app_t app_t;

bool app_session_split_supported(const app_t *app);

/* split_uuid and split_app start a second host on the right half; NULL for a normal stream */
int app_session_begin(app_t *app, const uuidstr_t *uuid, const APP_LIST *gs_app, const uuidstr_t *split_uuid,
                      const APP_LIST *split_app);

/* Interrupts every running session */
void app_session_interrupt(app_t *app, bool quitapp, streaming_interrupt_reason_t reason);

void app_session_destroy(app_t *app);

void app_session_destroy_split(app_t *app);
