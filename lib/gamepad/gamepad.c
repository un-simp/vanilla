#include "gamepad.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio.h"
#include "command.h"
#include "input.h"
#include "video.h"

#include "../pipe/linux/def.h"
#include "status.h"
#include "util.h"

static const uint32_t STOP_CODE = 0xCAFEBABE;
static uint32_t SERVER_ADDRESS = 0;

uint16_t PORT_MSG;
uint16_t PORT_VID;
uint16_t PORT_AUD;
uint16_t PORT_HID;
uint16_t PORT_CMD;

unsigned int reverse_bits(unsigned int b, int bit_count)
{
    unsigned int result = 0;

    for (int i = 0; i < bit_count; i++) {
        result |= ((b >> i) & 1) << (bit_count - 1 -i );
    }

    return result;
}

void send_to_console(int fd, const void *data, size_t data_size, int port)
{
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = SERVER_ADDRESS;
    address.sin_port = htons((uint16_t) (port - 100));

    char ip[20];
    inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));

    ssize_t sent = sendto(fd, data, data_size, 0, (const struct sockaddr *) &address, sizeof(address));
    if (sent == -1) {
        print_info("Failed to send to Wii U socket: fd - %d; port - %d", fd, port);
    }
}

int create_socket(int *socket_out, uint16_t port)
{
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    (*socket_out) = socket(AF_INET, SOCK_DGRAM, 0);
    
    if (bind((*socket_out), (const struct sockaddr *) &address, sizeof(address)) == -1) {
        print_info("FAILED TO BIND PORT %u: %i", port, errno);
        return 0;
    }

    return 1;
}

void send_stop_code(int from_socket, in_port_t port)
{
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");

    address.sin_port = htons(port);
    sendto(from_socket, &STOP_CODE, sizeof(STOP_CODE), 0, (struct sockaddr *)&address, sizeof(address));
}

int send_pipe_cc(int skt, uint32_t cc, int wait_for_reply)
{
    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = SERVER_ADDRESS;
    addr.sin_port = htons(VANILLA_PIPE_CMD_SERVER_PORT);

    ssize_t read_size;
    uint32_t send_cc = htonl(cc);
    uint32_t recv_cc;

    do {
        sendto(skt, &send_cc, sizeof(send_cc), 0, (struct sockaddr *) &addr, sizeof(addr));

        if (wait_for_reply) {
            read_size = recv(skt, &recv_cc, sizeof(recv_cc), 0);
            if (read_size == sizeof(recv_cc) && ntohl(recv_cc) == VANILLA_PIPE_CC_BIND_ACK) {
                return 1;
            }
        }
    } while (!is_interrupted());
    
    return 0;
}

int connect_as_gamepad_internal(vanilla_event_handler_t event_handler, void *context, uint32_t server_address)
{
    clear_interrupt();

    PORT_MSG = 50110;
    PORT_VID = 50120;
    PORT_AUD = 50121;
    PORT_HID = 50122;
    PORT_CMD = 50123;

    if (server_address == 0) {
        SERVER_ADDRESS = inet_addr("192.168.1.10");
    } else {
        SERVER_ADDRESS = htonl(server_address);
        PORT_MSG += 200;
        PORT_VID += 200;
        PORT_AUD += 200;
        PORT_HID += 200;
        PORT_CMD += 200;
    }

    struct gamepad_thread_context info;
    info.event_handler = event_handler;
    info.context = context;

    int ret = VANILLA_ERROR;

    // Try to bind with backend
    int pipe_cc_skt;
    if (!create_socket(&pipe_cc_skt, VANILLA_PIPE_CMD_CLIENT_PORT)) goto exit;

    struct timeval tv = {0};
    tv.tv_sec = 2;
    setsockopt(pipe_cc_skt, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!send_pipe_cc(pipe_cc_skt, VANILLA_PIPE_CC_BIND, 1)) {
        print_info("FAILED TO BIND TO PIPE");
        goto exit_pipe;
    }

    // Open all required sockets
    if (!create_socket(&info.socket_vid, PORT_VID)) goto exit_pipe;
    if (!create_socket(&info.socket_msg, PORT_MSG)) goto exit_vid;
    if (!create_socket(&info.socket_hid, PORT_HID)) goto exit_msg;
    if (!create_socket(&info.socket_aud, PORT_AUD)) goto exit_hid;
    if (!create_socket(&info.socket_cmd, PORT_CMD)) goto exit_aud;

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

    send_pipe_cc(pipe_cc_skt, VANILLA_PIPE_CC_UNBIND, 0);

    ret = VANILLA_SUCCESS;

exit_cmd:
    close(info.socket_cmd);

exit_aud:
    close(info.socket_aud);

exit_hid:
    close(info.socket_hid);

exit_msg:
    close(info.socket_msg);

exit_vid:
    close(info.socket_vid);

exit_pipe:
    close(pipe_cc_skt);

exit:
    return ret;
}

int is_stop_code(const char *data, size_t data_length)
{
    return (data_length == sizeof(STOP_CODE) && !memcmp(data, &STOP_CODE, sizeof(STOP_CODE)));
}