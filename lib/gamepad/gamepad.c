#include "gamepad.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wpa_ctrl.h>

#include "audio.h"
#include "command.h"
#include "input.h"
#include "video.h"

#include "os/os.h"
#include "status.h"
#include "util.h"

static const uint32_t STOP_CODE = 0xCAFEBABE;

unsigned int reverse_bits(unsigned int b, int bit_count)
{
    unsigned int result = 0;

    for (int i = 0; i < bit_count; i++) {
        result |= ((b >> i) & 1) << (bit_count - 1 -i );
    }

    return result;
}

uint32_t get_ip_address(uint8_t octet1, uint8_t octet2, uint8_t octet3, uint8_t octet4)
{
    uint32_t addr = 0;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    addr = (octet4 << 24) | (octet3 << 16) | (octet2 << 8) | octet1;
#else
    addr = (octet1 << 24) | (octet2 << 16) | (octet3 << 8) | octet4;
#endif
    return addr;
}

void send_stop_code(void *from_socket, uint16_t port)
{
    os_write_to_udp_socket(from_socket, get_ip_address(127,0,0,1), port, &STOP_CODE, sizeof(STOP_CODE));
}

void send_to_console(void *socket, const void *data, size_t length, int port)
{
    os_write_to_udp_socket(socket, get_ip_address(127,0,0,1), port - 100, data, length);
}

int connect_as_gamepad_internal(vanilla_event_handler_t event_handler, void *context)
{
    struct gamepad_thread_context info;
    info.event_handler = event_handler;
    info.context = context;

    int ret = VANILLA_ERROR;

    // Open all required sockets
    if (!os_open_udp_socket(&info.socket_vid, PORT_VID)) goto exit;
    if (!os_open_udp_socket(&info.socket_msg, PORT_MSG)) goto exit_vid;
    if (!os_open_udp_socket(&info.socket_hid, PORT_HID)) goto exit_msg;
    if (!os_open_udp_socket(&info.socket_aud, PORT_AUD)) goto exit_hid;
    if (!os_open_udp_socket(&info.socket_cmd, PORT_CMD)) goto exit_aud;

    pthread_t video_thread, audio_thread, input_thread, msg_thread, cmd_thread;

    pthread_create(&video_thread, NULL, listen_video, &info);
    pthread_create(&audio_thread, NULL, listen_audio, &info);
    pthread_create(&input_thread, NULL, listen_input, &info);
    pthread_create(&cmd_thread, NULL, listen_command, &info);

    while (1) {
        usleep(250 * 1000);
        if (is_interrupted()) {
            // Wake up any threads that might be blocked on `recv`
            send_stop_code(info.socket_msg, PORT_VID);
            send_stop_code(info.socket_msg, PORT_AUD);
            send_stop_code(info.socket_msg, PORT_CMD);
            break;
        }
    }

    pthread_join(video_thread, NULL);
    pthread_join(audio_thread, NULL);
    pthread_join(input_thread, NULL);
    pthread_join(cmd_thread, NULL);

    ret = VANILLA_SUCCESS;

exit_cmd:
    os_close_udp_socket(info.socket_cmd);

exit_aud:
    os_close_udp_socket(info.socket_aud);

exit_hid:
    os_close_udp_socket(info.socket_hid);

exit_msg:
    os_close_udp_socket(info.socket_msg);

exit_vid:
    os_close_udp_socket(info.socket_vid);

exit:
    return ret;
}

int is_stop_code(const char *data, size_t data_length)
{
    return (data_length == sizeof(STOP_CODE) && !memcmp(data, &STOP_CODE, sizeof(STOP_CODE)));
}