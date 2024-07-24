#ifndef VANILLA_GAMEPAD_H
#define VANILLA_GAMEPAD_H

#include "vanilla.h"

struct wpa_ctrl;

static const uint16_t PORT_MSG = 50110;
static const uint16_t PORT_VID = 50120;
static const uint16_t PORT_AUD = 50121;
static const uint16_t PORT_HID = 50122;
static const uint16_t PORT_CMD = 50123;

struct gamepad_thread_context
{
    vanilla_event_handler_t event_handler;
    void *context;

    void *socket_vid;
    void *socket_aud;
    void *socket_hid;
    void *socket_msg;
    void *socket_cmd;
};

int connect_as_gamepad_internal(vanilla_event_handler_t event_handler, void *context);
unsigned int reverse_bits(unsigned int b, int bit_count);
void send_to_console(void *socket, const void *data, size_t length, int port);
int is_stop_code(const char *data, size_t data_length);

#endif // VANILLA_GAMEPAD_H