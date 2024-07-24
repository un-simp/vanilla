#include "vanilla.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "gamepad/command.h"
#include "gamepad/gamepad.h"
#include "gamepad/input.h"
#include "gamepad/video.h"
#include "os/os.h"
#include "status.h"
#include "util.h"
#include "vanilla.h"

int vanilla_sync_with_console(const char *wireless_interface, uint16_t code)
{
    return os_sync_with_console(wireless_interface, code);
}

struct connect_args
{
    vanilla_event_handler_t event_handler;
    void *context;
};

int thunk_to_connect(void *context)
{
    struct connect_args *args = (struct connect_args *) context;
    return connect_as_gamepad_internal(args->event_handler, args->context);
}

int vanilla_connect_to_console(const char *wireless_interface, vanilla_event_handler_t event_handler, void *context)
{
    struct connect_args args;
    args.event_handler = event_handler;
    args.context = context;
    return os_connect_to_console(wireless_interface, thunk_to_connect, &args);
}

int vanilla_has_config()
{
    return (access(get_wireless_connect_config_filename(), F_OK) == 0);
}

void vanilla_stop()
{
    force_interrupt();
}

void vanilla_set_button(int button, int32_t value)
{
    set_button_state(button, value);
}

void vanilla_set_touch(int x, int y)
{
    set_touch_state(x, y);
}

void default_logger(const char *format, va_list args)
{
    vprintf(format, args);
}

void (*custom_logger)(const char *, va_list) = default_logger;
void vanilla_log(const char *format, ...)
{
    va_list va;
    va_start(va, format);

    if (custom_logger) {
        custom_logger(format, va);
        custom_logger("\n", va);
    }

    va_end(va);
}

void vanilla_log_no_newline(const char *format, ...)
{
    va_list va;
    va_start(va, format);

    vanilla_log_no_newline_va(format, va);

    va_end(va);
}

void vanilla_log_no_newline_va(const char *format, va_list args)
{
    if (custom_logger) {
        custom_logger(format, args);
    }
}

void vanilla_install_logger(void (*logger)(const char *, va_list))
{
    custom_logger = logger;
}

void vanilla_request_idr()
{
    request_idr();
}

void vanilla_retrieve_sps_pps_data(void *data, size_t *size)
{
    if (data != NULL) {
        memcpy(data, sps_pps_params, MIN(*size, sizeof(sps_pps_params)));
    }
    *size = sizeof(sps_pps_params);
}

void vanilla_set_region(int region)
{
    set_region(region);
}

void vanilla_set_battery_status(int battery_status)
{
    set_battery_status(battery_status);
}