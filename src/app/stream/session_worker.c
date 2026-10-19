#include "session_worker.h"
#include "session_priv.h"
#include "app.h"
#include "util/bus.h"
#include "logging.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"

/* The streaming screen follows the main session; the split screen session runs without it */
static void session_push_event(const session_t *session, int code) {
    if (session->slot == 0) {
        bus_pushevent(code, NULL, NULL);
    }
}

static void session_start_split_input(session_t *session) {
    session_start_input(session);
}

int session_worker(session_t *session) {
    app_t *app = session->app;
    session_set_state(session, STREAMING_CONNECTING);
    session_push_event(session, USER_STREAM_CONNECTING);
    streaming_error(session, GS_OK, "");
    PSERVER_DATA server = session->server;
    int appId = session->app_id;
    session->player = NULL;

#if FEATURE_INPUT_EVMOUSE
    if (!session->config.view_only && session->config.hardware_mouse) {
        session_evmouse_wait_ready(&session->input.evmouse);
    }
#endif

    commons_log_info("Session", "Launch app %d...", appId);
    GS_CLIENT client = app_gs_client_new(app);
    const char *surround_params = NULL;
#if TARGET_WEBOS
    if (session->config.stream.audioConfiguration == AUDIO_CONFIGURATION_51_SURROUND) {
        // 6 channels, 4 streams, 2 coupled streams, FL, FR, SL, SR, FC, LFE
        surround_params = "642014523";
    }
#endif
    int ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                           session->config.local_audio,
                           session_host_gamepad_mask(session, app_input_gamepads_mask(&app->input)), surround_params);
    if (ret != GS_OK) {
        session_set_state(session, STREAMING_ERROR);
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        if (gs_error) {
            streaming_error(session, ret, "Failed to launch session: %s (code %d)", gs_error, ret);
        } else {
            streaming_error(session, ret, "Failed to launch session: gamestream returned %d", ret);
        }
        commons_log_error("Session", "Failed to launch session: gamestream returned %d, gs_error=%s", ret, gs_error);
        goto thread_cleanup;
    }

    commons_log_info("Session", "Video %d x %d, %d net_fps, %d kbps", session->config.stream.width,
                     session->config.stream.height, session->config.stream.fps, session->config.stream.bitrate);
    commons_log_info("Session", "Audio %d channels",
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(session->config.stream.audioConfiguration));

    session->player = SS4S_PlayerOpen();
    if (session->player == NULL) {
        session_set_state(session, STREAMING_ERROR);
        streaming_error(session, GS_WRONG_STATE, "Failed to create decoder player");
        goto thread_cleanup;
    }
    SS4S_PlayerSetWaitAudioVideoReady(session->player, true);
    SS4S_PlayerSetViewportSize(session->player, app->ui.width, app->ui.height);
    SS4S_PlayerSetUserdata(session->player, app);

    const mlc_api_t *mlc = session->mlc;
    int startResult = mlc->LiStartConnection(&server->serverInfo, &session->config.stream,
                                             mlc->session_connection_callbacks_prepare(session),
                                             mlc->ss4s_dec_callbacks, mlc->ss4s_aud_callbacks, session, 0, session, 0);
    if (startResult != 0) {
        session_set_state(session, STREAMING_ERROR);
        switch (startResult) {
            case CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported video codec.");
                break;
            case CALLBACKS_SESSION_ERROR_VDEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open video decoder.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported audio codec.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open audio backend.");
                break;
            default: {
                if (!streaming_errno) {
                    streaming_error(session, GS_WRONG_STATE, "Failed to start connection: Limelight returned %d (%s)",
                                    startResult, strerror(startResult));
                }
                break;
            }
        }
        commons_log_error("Session", "Failed to start connection: Limelight returned %d", startResult);
        goto thread_cleanup;
    }
    session_set_state(session, STREAMING_STREAMING);
    session_push_event(session, USER_STREAM_OPEN);
    if (session->slot != 0) {
        // The streaming screen starts input for the main session only
        app_bus_post(app, (bus_actionfunc) session_start_split_input, session);
    }
    SDL_LockMutex(session->mutex);
    while (!session->interrupted) {
        // Wait until interrupted
        SDL_CondWait(session->cond, session->mutex);
    }
    SDL_UnlockMutex(session->mutex);
    session_push_event(session, USER_STREAM_CLOSE);

    session_set_state(session, STREAMING_DISCONNECTING);
    mlc->LiStopConnection();

    if (session->quitapp) {
        commons_log_info("Session", "Sending app quit request ...");
        gs_quit_app(client, server);
    }
    worker_context_t update_ctx = {
            .app = app,
            .manager = pcmanager,
    };
    uuidstr_fromstr(&update_ctx.uuid, server->uuid);
    pcmanager_update_by_host(&update_ctx, server->serverInfo.address, server->extPort, true);

    // Don't always reset status as error state should be kept
    session_set_state(session, STREAMING_NONE);
    thread_cleanup:
    session->mlc->session_connection_callbacks_reset(session);
    if (session->player != NULL) {
        SS4S_PlayerClose(session->player);
    }
    gs_destroy(client);
    session_push_event(session, USER_STREAM_FINISHED);
    app_bus_post(app, (bus_actionfunc) (session->slot == 0 ? app_session_destroy : app_session_destroy_split), app);
    return 0;
}