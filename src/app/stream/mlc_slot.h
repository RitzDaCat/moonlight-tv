#pragma once

/*
 * moonlight-common-c keeps its connection state in globals, so one copy can drive one stream.
 * Split screen links a second copy (with the session callbacks that hold per-stream state) whose
 * symbols carry an mlc2_ prefix; see cmake/MlcSecondCopy.cmake. Slot 0 is the normal copy.
 * Calls that must reach a specific stream go through session->mlc.
 */

#include <Limelight.h>

#include "stream/session.h"
#include "stream/connection/session_connection.h"

#define MLC_FUNCTIONS(X) \
    X(LiStartConnection) \
    X(LiStopConnection) \
    X(LiGetHdrMetadata) \
    X(LiGetHostFeatureFlags) \
    X(LiSendMouseMoveEvent) \
    X(LiSendMousePositionEvent) \
    X(LiSendMouseButtonEvent) \
    X(LiSendScrollEvent) \
    X(LiSendHScrollEvent) \
    X(LiSendHighResScrollEvent) \
    X(LiSendHighResHScrollEvent) \
    X(LiSendTouchEvent) \
    X(LiSendKeyboardEvent) \
    X(LiSendUtf8TextEvent) \
    X(LiSendMultiControllerEvent) \
    X(LiSendControllerArrivalEvent) \
    X(LiSendControllerMotionEvent) \
    X(LiSendControllerTouchEvent) \
    X(session_connection_callbacks_prepare) \
    X(session_connection_callbacks_reset)

#define MLC_DATA(X) \
    X(DECODER_RENDERER_CALLBACKS, ss4s_dec_callbacks) \
    X(AUDIO_RENDERER_CALLBACKS, ss4s_aud_callbacks) \
    X(VIDEO_STATS, vdec_summary_stats) \
    X(VIDEO_INFO, vdec_stream_info) \
    X(AUDIO_INFO, audio_stream_info)

#if FEATURE_SPLIT_SCREEN
#define MLC_SLOTS 2
#else
#define MLC_SLOTS 1
#endif

typedef struct mlc_api_t {
#define MLC_FN_FIELD(name) __typeof__(name) *name;
#define MLC_DATA_FIELD(type, name) type *name;
    MLC_FUNCTIONS(MLC_FN_FIELD)
    MLC_DATA(MLC_DATA_FIELD)
#undef MLC_FN_FIELD
#undef MLC_DATA_FIELD
} mlc_api_t;

extern const mlc_api_t mlc_slots[MLC_SLOTS];
