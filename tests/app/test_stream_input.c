#include "unity.h"
#include <SDL.h>
#include "input/app_input.h"
#include "stream/session_priv.h"
#include "stream/input/session_virt_mouse.h"

static session_t session;
static app_input_t devices;
static mlc_api_t connection;
static int moves, presses, releases, reports, arrivals;
static SDL_GameController *virtual_controller;
static int virtual_index = -1;
static short last_x, last_y;

static int send_move(short x, short y) {
    moves++;
    last_x = x;
    last_y = y;
    return 0;
}

static int send_button(char action, int button) {
    (void) button;
    if (action == BUTTON_ACTION_PRESS) presses++;
    else releases++;
    return 0;
}

static int send_controller(short id, short mask, int buttons, unsigned char l, unsigned char r,
                           short lx, short ly, short rx, short ry) {
    (void) id; (void) mask; (void) buttons; (void) l; (void) r; (void) rx; (void) ry;
    reports++;
    last_x = lx;
    last_y = ly;
    return 0;
}

static int send_arrival(unsigned char id, unsigned short mask, unsigned char type, unsigned int buttons, unsigned short capabilities) {
    (void) id; (void) mask; (void) type; (void) buttons; (void) capabilities;
    arrivals++;
    return 0;
}

void setUp(void) {
    SDL_memset(&session, 0, sizeof(session));
    SDL_memset(&devices, 0, sizeof(devices));
    SDL_memset(&connection, 0, sizeof(connection));
    connection.LiSendMouseMoveEvent = send_move;
    connection.LiSendMouseButtonEvent = send_button;
    connection.LiSendMultiControllerEvent = send_controller;
    connection.LiSendControllerArrivalEvent = send_arrival;
    session.mlc = &connection;
    session.input.session = &session;
    session.input.input = &devices;
    session.input.vmouse.session = &session;
    devices.max_num_gamepads = 1;
    devices.gamepads[0].instance_id = 42;
    devices.gamepads[0].gs_id = 0;
    devices.activeGamepadMask = 1;
    moves = presses = releases = reports = arrivals = 0;
    last_x = last_y = 0;
}

void tearDown(void) {
    session_input_deinit(&session.input);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (virtual_controller != NULL) {
        SDL_GameControllerClose(virtual_controller);
        virtual_controller = NULL;
    }
    if (virtual_index >= 0) {
        SDL_JoystickDetachVirtual(virtual_index);
        virtual_index = -1;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
}

static void axis(Uint8 id, Sint16 value) {
    SDL_ControllerAxisEvent event = {.which = 42, .axis = id, .value = value};
    stream_input_handle_caxis(&session.input, &event);
}

static void test_radial_deadzone_preserves_other_raw_axis(void) {
    session.input.stick_deadzone = 10;
    axis(SDL_CONTROLLER_AXIS_LEFTX, 2500);
    TEST_ASSERT_EQUAL_INT(0, last_x);
    axis(SDL_CONTROLLER_AXIS_LEFTY, 2500);
    TEST_ASSERT_EQUAL_INT(2500, last_x);
    TEST_ASSERT_EQUAL_INT(-2500, last_y);
    TEST_ASSERT_EQUAL_INT(2500, devices.gamepads[0].leftStickX);
}

static void test_vmouse_neutral_stops_and_releases_on_own_connection(void) {
    vmouse_set_running(&session.input.vmouse, true);
    session_input_set_vmouse_active(&session.input.vmouse, true);
    axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767);
    vmouse_update(&session.input.vmouse);
    TEST_ASSERT_EQUAL_INT(1, moves);
    TEST_ASSERT_TRUE(last_x > 0);
    TEST_ASSERT_EQUAL_INT(0, last_y);
    axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    TEST_ASSERT_EQUAL_INT(1, presses);
    axis(SDL_CONTROLLER_AXIS_RIGHTX, 0);
    axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0);
    TEST_ASSERT_EQUAL_INT(1, releases);
    session.input.vmouse.next_tick = SDL_GetTicks();
    vmouse_update(&session.input.vmouse);
    TEST_ASSERT_EQUAL_INT(1, moves);
}

static void test_stop_clears_motion_preserving_selected_mode(void) {
    vmouse_set_running(&session.input.vmouse, true);
    session_input_set_vmouse_active(&session.input.vmouse, true);
    axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767);
    axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    session_input_stopped(&session.input);
    TEST_ASSERT_EQUAL_INT(1, releases);
    TEST_ASSERT_TRUE(session_input_is_vmouse_active(&session.input.vmouse));
    vmouse_update(&session.input.vmouse);
    TEST_ASSERT_EQUAL_INT(0, moves);
    vmouse_set_running(&session.input.vmouse, true);
    vmouse_update(&session.input.vmouse);
    TEST_ASSERT_EQUAL_INT(0, moves);
    session_input_deinit(&session.input);
    vmouse_update(&session.input.vmouse);
    TEST_ASSERT_EQUAL_INT(0, moves);
}

#if SDL_VERSION_ATLEAST(2, 0, 14)
static void test_reconnect_uses_device_index_for_arrival(void) {
    TEST_ASSERT_EQUAL_INT(0, SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER));
    int old = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    TEST_ASSERT_TRUE(old >= 0);
    SDL_JoystickID old_id = SDL_JoystickGetDeviceInstanceID(old);
    TEST_ASSERT_EQUAL_INT(0, SDL_JoystickDetachVirtual(old));
    virtual_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1);
    TEST_ASSERT_TRUE(virtual_index >= 0);
    virtual_controller = SDL_GameControllerOpen(virtual_index);
    TEST_ASSERT_NOT_NULL(virtual_controller);
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(virtual_controller));
    TEST_ASSERT_TRUE(id != old_id);
    TEST_ASSERT_TRUE(id != virtual_index);
    devices.gamepads[0].instance_id = id;
    devices.gamepads[0].controller = virtual_controller;
    SDL_ControllerDeviceEvent event = {.type = SDL_CONTROLLERDEVICEADDED, .which = virtual_index};
    stream_input_handle_cdevice(&session.input, &event);
    TEST_ASSERT_EQUAL_INT(1, arrivals);
}
#endif

static void test_split_gamepad_masks_cover_all_protocol_bits(void) {
    session.split = true;
    for (unsigned mask = 0; mask < 65536; ++mask) {
        unsigned expected[2] = {0, 0};
        for (unsigned bit = 0; bit < 16; ++bit) {
            if (mask & (1U << bit)) expected[bit % 2] |= 1U << (bit / 2);
        }
        for (session.slot = 0; session.slot < 2; ++session.slot) {
            TEST_ASSERT_EQUAL_UINT16(expected[session.slot],
                    session_host_gamepad_mask(&session, (short) mask));
        }
    }
    session.split = false;
    TEST_ASSERT_EQUAL_HEX16(0x8000, session_host_gamepad_mask(&session, (short) 0x8000));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_radial_deadzone_preserves_other_raw_axis);
    RUN_TEST(test_split_gamepad_masks_cover_all_protocol_bits);
    RUN_TEST(test_vmouse_neutral_stops_and_releases_on_own_connection);
    RUN_TEST(test_stop_clears_motion_preserving_selected_mode);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    RUN_TEST(test_reconnect_uses_device_index_for_arrival);
#endif
    return UNITY_END();
}
