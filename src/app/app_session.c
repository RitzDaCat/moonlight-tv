#include "app_session.h"
#include "app.h"
#include "stream/session.h"
#include "logging.h"

bool app_session_split_supported(const app_t *app) {
    const unsigned required = SS4S_VIDEO_CAP_TRANSFORM_AREA_DEST | SS4S_VIDEO_CAP_TRANSFORM_MULTI_INSTANCE;
    return (app->ss4s.video_cap.transform & required) == required;
}

int app_session_begin(app_t *app, const uuidstr_t *uuid, const APP_LIST *gs_app, const uuidstr_t *split_uuid,
                      const APP_LIST *split_app) {
    if (uuid == NULL || gs_app == NULL ||
        (split_uuid != NULL && (split_app == NULL || uuidstr_t_equals_t(uuid, split_uuid)))) {
        commons_log_error("App", "Split screen requires two distinct computers and a game for each");
        return -1;
    }
    if (split_uuid != NULL && !app_session_split_supported(app)) {
        commons_log_error("App", "Selected decoder does not support simultaneous split-screen streams");
        return -1;
    }
    if (app->session != NULL) {
        commons_log_error("App", "Session already exists");
        return -1;
    }
    const pclist_t *node = pcmanager_node(pcmanager, uuid);
    if (node == NULL) {
        commons_log_error("App", "Failed to find node %s", (const char *) uuid);
        return -1;
    }
    const pclist_t *split_node = NULL;
    if (split_uuid != NULL) {
        split_node = pcmanager_node(pcmanager, split_uuid);
        if (split_node == NULL) {
            commons_log_error("App", "Failed to find split screen node %s", (const char *) split_uuid);
            return -1;
        }
    }
    app->session = session_create(app, app_configuration, node->server, gs_app, 0, split_node != NULL);
    if (split_node != NULL) {
        app->split_session = session_create(app, app_configuration, split_node->server, split_app, 1, true);
    }
    return 0;
}

void app_session_interrupt(app_t *app, bool quitapp, streaming_interrupt_reason_t reason) {
    session_interrupt(app->session, quitapp, reason);
    session_interrupt(app->split_session, quitapp, reason);
}

void app_session_destroy(app_t *app) {
    app_session_destroy_split(app);
    if (app->session == NULL) {
        return;
    }
    session_destroy(app->session);
    app->session = NULL;
}

void app_session_destroy_split(app_t *app) {
    if (app->split_session == NULL) {
        return;
    }
    session_destroy(app->split_session);
    app->split_session = NULL;
}
