#include "app.h"
#include "app_session.h"
#include "streaming.controller.h"
#include "stream/video/session_video.h"
#include "stream/session_priv.h"
#include "ui/root.h"
#include "ui/common/progress_dialog.h"
#include "lvgl/lv_ext_utils.h"

#include "util/user_event.h"
#include "util/i18n.h"
#include "logging.h"

static void exit_streaming(lv_event_t *event);

static void suspend_streaming(lv_event_t *event);

static void open_keyboard(lv_event_t *event);

static void toggle_vmouse(lv_event_t *event);

static void hide_overlay(lv_event_t *event);

static void overlay_clicked(lv_event_t *event);

static bool show_overlay(streaming_controller_t *controller);

static void on_view_created(lv_fragment_t *self, lv_obj_t *view);

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view);

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view);

static bool on_event(lv_fragment_t *self, int code, void *userdata);

static void constructor(lv_fragment_t *self, void *args);

static void controller_dtor(lv_fragment_t *self);

static void overlay_key_cb(lv_event_t *e);

static void update_buttons_layout(streaming_controller_t *controller);
static void stream_audio_changed(lv_event_t *event);
static void stream_audio_toggle(lv_event_t *event);

static void pin_toggle(lv_event_t *e);

const lv_fragment_class_t streaming_controller_class = {
        .constructor_cb = constructor,
        .destructor_cb = controller_dtor,
        .create_obj_cb = streaming_scene_create,
        .obj_created_cb = on_view_created,
        .obj_will_delete_cb = on_delete_obj,
        .obj_deleted_cb = on_obj_deleted,
        .event_cb = on_event,
        .instance_size = sizeof(streaming_controller_t),
};

static bool overlay_showing = false, overlay_pinned = false;
static streaming_controller_t *current_controller = NULL;

bool streaming_overlay_shown() {
    return overlay_showing;
}

bool streaming_stats_shown() {
    return overlay_showing || overlay_pinned;
}

bool streaming_refresh_stats() {
    streaming_controller_t *controller = current_controller;
    if (!controller || !streaming_stats_shown()) { return false; }
    app_t *app = controller->global;
    if (!app->session) { return false; }
    VIDEO_STATS snapshot;
    VIDEO_INFO video;
    AUDIO_INFO audio;
    session_stats_read(&app->session->stats, &snapshot, &video, &audio);
    const VIDEO_STATS *dst = &snapshot;
    const char *format = video.format ? video.format : "Unknown";
    if (video.width > 0 && video.height > 0) {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%s, %d\u00d7%d (%s)", format, video.width,
                              video.height, SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    } else {
        lv_label_set_text_fmt(controller->stats_items.decoder, "%s (%s)", format,
                              SS4S_ModuleInfoGetId(app->ss4s.selection.video_module));
    }
    lv_label_set_text_fmt(controller->stats_items.audio, "%s, %s (%s)",
                          audio.format ? audio.format : "Unknown", audio.channels ? audio.channels : "Unknown",
                          SS4S_ModuleInfoGetId(app->ss4s.selection.audio_module));
    lv_label_set_text_fmt(controller->stats_items.rtt, "%d ms (var. %d ms)", dst->rtt, dst->rttVariance);
    lv_label_set_text_fmt(controller->stats_items.net_fps, "%.2f FPS", dst->receivedFps);
    if (dst->totalFrames) {
        lv_label_set_text_fmt(controller->stats_items.drop_rate, "%.2f%%",
                              100.0f * dst->networkDroppedFrames / dst->totalFrames);
    } else {
        lv_label_set_text(controller->stats_items.drop_rate, "-");
    }
    if (dst->hostLatencyFrames) {
        lv_label_set_text_fmt(controller->stats_items.host_latency, "avg %.2f ms",
                              (double) dst->totalCaptureLatency / dst->hostLatencyFrames / 10.0);
    } else {
        lv_label_set_text(controller->stats_items.host_latency, "not available");
    }
    if (dst->submittedFrames) {
        lv_label_set_text_fmt(controller->stats_items.submit_latency, "avg %.2f ms",
                              (double) dst->totalSubmitTimeUs / dst->submittedFrames / 1000.0);
        lv_label_set_text_fmt(controller->stats_items.feed_latency, "avg %.2f ms",
                              (double) dst->totalFeedTimeUs / dst->submittedFrames / 1000.0);
    } else {
        lv_label_set_text(controller->stats_items.submit_latency, "-");
        lv_label_set_text(controller->stats_items.feed_latency, "-");
    }
    if (dst->receivedFrames) {
        lv_label_set_text_fmt(controller->stats_items.assembly_latency, "avg %.2f ms",
                              (double) dst->totalReassemblyTimeUs / dst->receivedFrames / 1000.0);
    } else {
        lv_label_set_text(controller->stats_items.assembly_latency, "-");
    }
    const SS4S_VideoDiagnostics *diagnostics = &dst->diagnostics;
    if (diagnostics->kind == SS4S_VIDEO_DIAGNOSTICS_COMPLETION && diagnostics->completionLatencyUs >= 0) {
        /* Completion already includes feed time. Never add the overlapping submission span. */
        lv_label_set_text_fmt(controller->stats_items.vdec_latency, "avg %.2f ms",
                              diagnostics->completionLatencyUs / 1000.0);
    } else {
        lv_label_set_text(controller->stats_items.vdec_latency, "not measured");
    }
    if (diagnostics->kind == SS4S_VIDEO_DIAGNOSTICS_QUEUE_ESTIMATE && diagnostics->renderQueueFrames >= 0) {
        if (diagnostics->estimatedQueueDelayUs >= 0) {
            lv_label_set_text_fmt(controller->stats_items.queue_latency, "%d frames (~%.2f ms)",
                                  diagnostics->renderQueueFrames, diagnostics->estimatedQueueDelayUs / 1000.0);
        } else {
            lv_label_set_text_fmt(controller->stats_items.queue_latency, "%d frames", diagnostics->renderQueueFrames);
        }
    } else {
        lv_label_set_text(controller->stats_items.queue_latency, "not available");
    }
    return true;
}

void streaming_notice_show(const char *message) {
    streaming_controller_t *controller = current_controller;
    if (!controller) { return; }
    lv_label_set_text(controller->notice_label, message);
    if (message && message[0]) {
        lv_obj_clear_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(controller->notice, LV_OBJ_FLAG_HIDDEN);
    }
}

static void constructor(lv_fragment_t *self, void *args) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    current_controller = controller;

    overlay_showing = false;

    streaming_styles_init(controller);

    const streaming_scene_arg_t *arg = (streaming_scene_arg_t *) args;
    controller->global = arg->global;
    controller->split = arg->split;
    app_session_begin(arg->global, &arg->uuid, &arg->app, arg->split ? &arg->split_uuid : NULL,
                      arg->split ? &arg->split_app : NULL);
}

static void controller_dtor(lv_fragment_t *self) {
    streaming_controller_t *fragment = (streaming_controller_t *) self;
    streaming_styles_reset(fragment);
    if (current_controller == fragment) {
        current_controller = NULL;
    }
}

static bool on_event(lv_fragment_t *self, int code, void *userdata) {
    LV_UNUSED(userdata);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    switch (code) {
        case USER_STREAM_CONNECTING: {
            controller->progress = progress_dialog_create(locstr("Connecting..."));
            if (lv_obj_check_type(controller->progress->parent, &lv_msgbox_backdrop_class)) {
                lv_obj_set_style_bg_opa(controller->progress->parent, LV_OPA_TRANSP, 0);
            }
            lv_obj_set_width(controller->progress, LV_DPX(300));
            lv_obj_set_height(controller->progress, LV_DPX(100));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        case USER_STREAM_OPEN: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case USER_STREAM_CLOSE: {
            controller->progress = progress_dialog_create(locstr("Disconnecting..."));
            lv_obj_add_flag(controller->overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->stats, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(controller->hint, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case USER_STREAM_FINISHED: {
            if (controller->progress) {
                lv_msgbox_close(controller->progress);
                controller->progress = NULL;
            }
            lv_async_call((lv_async_cb_t) lv_fragment_del, controller);
            break;
        }
        case USER_OPEN_OVERLAY: {
            show_overlay(controller);
            return true;
        }
        case USER_SIZE_CHANGED: {
            update_buttons_layout(controller);
            streaming_overlay_resized(controller);
            return false;
        }
        default: {
            break;
        }
    }
    return false;
}

static void on_view_created(lv_fragment_t *self, lv_obj_t *view) {
    streaming_controller_t *controller = (streaming_controller_t *) self;
    for (int player = 0; player < 2 && controller->split; ++player) {
        lv_obj_add_event_cb(controller->stream_audio[player].gain, stream_audio_changed,
                            LV_EVENT_VALUE_CHANGED, controller);
        lv_obj_add_event_cb(controller->stream_audio[player].gain, stream_audio_changed,
                            LV_EVENT_RELEASED, controller);
        lv_obj_add_event_cb(controller->stream_audio[player].gain, stream_audio_changed,
                            LV_EVENT_DEFOCUSED, controller);
        lv_obj_add_event_cb(controller->stream_audio[player].muted, stream_audio_changed,
                            LV_EVENT_VALUE_CHANGED, controller);
        lv_obj_add_event_cb(controller->stream_audio[player].muted, stream_audio_toggle,
                            LV_EVENT_CLICKED, controller);
    }
    app_input_set_group(&controller->global->ui.input, controller->group);
    lv_obj_add_event_cb(controller->quit_btn, exit_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->suspend_btn, suspend_streaming, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->kbd_btn, open_keyboard, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->vmouse_btn, toggle_vmouse, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->base.obj, overlay_clicked, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(controller->overlay, overlay_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(controller->stats, overlay_key_cb, LV_EVENT_KEY, controller);
    lv_obj_add_event_cb(controller->stats, hide_overlay, LV_EVENT_CANCEL, controller);
    lv_obj_add_event_cb(controller->base.obj, hide_overlay, LV_EVENT_CANCEL, controller);

    lv_obj_t *notice = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(notice, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(notice, LV_ALIGN_TOP_RIGHT, -LV_DPX(20), LV_DPX(20));
    lv_obj_set_style_radius(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_hor(notice, LV_DPX(5), 0);
    lv_obj_set_style_pad_ver(notice, LV_DPX(3), 0);
    lv_obj_set_style_border_opa(notice, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(notice, lv_color_black(), 0);
    lv_obj_t *notice_label = lv_label_create(notice);
    lv_obj_set_size(notice_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(notice_label, lv_theme_get_font_small(view), 0);
    lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN);

    controller->notice = notice;
    controller->notice_label = notice_label;

    lv_obj_add_event_cb(controller->stats_pin, pin_toggle, LV_EVENT_CLICKED, controller->stats);

#if !defined(TARGET_WEBOS)
    const app_settings_t *settings = &controller->global->settings;
    if (settings->syskey_capture) {
        SDL_SetWindowGrab(controller->global->ui.window, SDL_TRUE);
    }
#endif
}

static void stream_audio_toggle(lv_event_t *event) {
    lv_obj_t *target = lv_event_get_current_target(event);
    if (lv_obj_has_state(target, LV_STATE_CHECKED)) lv_obj_clear_state(target, LV_STATE_CHECKED);
    else lv_obj_add_state(target, LV_STATE_CHECKED);
    lv_event_send(target, LV_EVENT_VALUE_CHANGED, NULL);
}

static void stream_audio_changed(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        settings_save(app_configuration);
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    lv_indev_t *indev = lv_indev_get_act();
    if (lv_obj_has_class(target, &lv_slider_class) && indev &&
        lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD &&
        (lv_indev_get_key(indev) == LV_KEY_UP || lv_indev_get_key(indev) == LV_KEY_DOWN)) {
        /* LVGL changes sliders before dispatching the navigation key. The key
         * callback restores the value; never send this transient change to audio. */
        return;
    }
    for (int player = 0; player < 2; ++player) {
        if (target != controller->stream_audio[player].gain && target != controller->stream_audio[player].muted) continue;
        int db = lv_slider_get_value(controller->stream_audio[player].gain);
        bool muted = lv_obj_has_state(controller->stream_audio[player].muted, LV_STATE_CHECKED);
        app_configuration->split_audio_gain_db[player] = db;
        app_configuration->split_audio_muted[player] = muted;
        lv_label_set_text_fmt(controller->stream_audio[player].gain_label, "%d dB", db);
        lv_label_set_text(lv_obj_get_child(controller->stream_audio[player].muted, 0),
                           muted ? locstr("Unmute") : locstr("Mute"));
        session_t *session = player == 0 ? controller->global->session : controller->global->split_session;
        if (session != NULL && session->split) {
            stream_audio_control_set(&session->audio_control, db, muted, true);
        }
        if (target == controller->stream_audio[player].muted) settings_save(app_configuration);
        return;
    }
}

static void on_delete_obj(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    if (controller->notice) {
        lv_obj_del(controller->notice);
    }
    if (controller->stats->parent != controller->overlay) {
        lv_obj_del(controller->stats);
    }
    app_input_set_group(&controller->global->ui.input, NULL);
    lv_group_del(controller->group);

#if !defined(TARGET_WEBOS)
    SDL_SetWindowGrab(controller->global->ui.window, SDL_FALSE);
#endif
}

static void on_obj_deleted(lv_fragment_t *self, lv_obj_t *view) {
    LV_UNUSED(view);
    streaming_controller_t *controller = (streaming_controller_t *) self;
    lv_obj_del(controller->detached_root);
}

static void exit_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    app_session_interrupt(self->global, true, STREAMING_INTERRUPT_USER);
}

static void suspend_streaming(lv_event_t *event) {
    streaming_controller_t *self = lv_event_get_user_data(event);
    app_session_interrupt(self->global, false, STREAMING_INTERRUPT_USER);
}

static void open_keyboard(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    app_start_text_input(&app->ui.input, 0, app->ui.height / 2 - 40, app->ui.width, 40);
    session_screen_keyboard_opened(app->session);
}

static void toggle_vmouse(lv_event_t *event) {
    streaming_controller_t *controller = lv_event_get_user_data(event);
    hide_overlay(event);
    app_t *app = controller->global;
    session_toggle_vmouse(app->session);
}

bool show_overlay(streaming_controller_t *controller) {
    if (overlay_showing) {
        return false;
    }
    overlay_showing = true;
    lv_obj_clear_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);

    lv_area_t coords = controller->video->coords;
    streaming_enter_overlay(controller->global->session, coords.x1, coords.y1, lv_area_get_width(&coords),
                            lv_area_get_height(&coords));
    streaming_refresh_stats();

    app_stop_text_input(&controller->global->ui.input);

    update_buttons_layout(controller);
    return true;
}

static void overlay_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) hide_overlay(event);
}

static void hide_overlay(lv_event_t *event) {
    streaming_controller_t *controller = (streaming_controller_t *) lv_event_get_user_data(event);
    app_input_set_button_points(&controller->global->ui.input, NULL);
    lv_obj_add_flag(controller->base.obj, LV_OBJ_FLAG_HIDDEN);
    if (!overlay_showing) {
        return;
    }
    overlay_showing = false;
    app_set_mouse_grab(&global->input, true);
    streaming_enter_fullscreen(controller->global->session);
}

static void overlay_key_cb(lv_event_t *e) {
    streaming_controller_t *controller = lv_event_get_user_data(e);
    lv_group_t *group = controller->group;
    lv_obj_t *target = lv_event_get_target(e);
    uint32_t key = lv_event_get_key(e);
    if (lv_obj_has_class(target, &lv_slider_class)) {
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) return;
        if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            for (int player = 0; player < 2; ++player) {
                if (target != controller->stream_audio[player].gain) continue;
                _lv_bar_anim_t *animation = &((lv_slider_t *) target)->bar.cur_value_anim;
                animation->anim_state = -1;
                lv_anim_del(animation, NULL);
                lv_slider_set_value(target, app_configuration->split_audio_gain_db[player], LV_ANIM_OFF);
                break;
            }
        }
    }
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP:
        case LV_KEY_LEFT:
            lv_group_focus_prev(group);
            lv_event_stop_bubbling(e);
            break;
        case LV_KEY_DOWN:
        case LV_KEY_RIGHT:
            lv_group_focus_next(group);
            lv_event_stop_bubbling(e);
            break;
        default:
            break;
    }
}

static void update_buttons_layout(streaming_controller_t *controller) {
    lv_area_t coords;
    lv_obj_get_coords(controller->quit_btn, &coords);
    lv_area_center(&coords, &controller->button_points[1]);
    lv_obj_get_coords(controller->suspend_btn, &coords);
    lv_area_center(&coords, &controller->button_points[3]);
    lv_obj_get_coords(controller->kbd_btn, &coords);
    lv_area_center(&coords, &controller->button_points[4]);
    app_input_set_button_points(&controller->global->ui.input, controller->button_points);
}

static void pin_toggle(lv_event_t *e) {
    lv_obj_t *toggle_view = lv_event_get_user_data(e);
    lv_fragment_t *fragment = lv_obj_get_user_data(toggle_view);
    lv_obj_t *button = lv_event_get_current_target(e);
    bool checked = !lv_obj_has_state(button, LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(button, LV_STATE_CHECKED);
    else lv_obj_clear_state(button, LV_STATE_CHECKED);
    bool pinned = toggle_view->parent != fragment->obj;
    overlay_pinned = checked;
    if (checked == pinned) {
        return;
    }
    if (checked) {
        lv_obj_set_parent(toggle_view, lv_layer_top());
        lv_obj_add_state(toggle_view, LV_STATE_USER_1);
    } else {
        lv_obj_set_parent(toggle_view, fragment->obj);
        lv_obj_clear_state(toggle_view, LV_STATE_USER_1);
    }
}
