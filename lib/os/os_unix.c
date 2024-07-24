#include "os.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wpa_ctrl.h>

#include "status.h"
#include "util.h"
#include "vanilla.h"

const char *wpa_ctrl_interface = "/var/run/wpa_supplicant_drc";

typedef int (*ready_callback_t)(struct wpa_ctrl *, void *);

void wpa_msg(char *msg, size_t len)
{
    print_info("%.*s", len, msg);
}

void wpa_ctrl_command(struct wpa_ctrl *ctrl, const char *cmd, char *buf, size_t *buf_len)
{
    wpa_ctrl_request(ctrl, cmd, strlen(cmd), buf, buf_len, NULL /*wpa_msg*/);
}

int get_binary_in_working_directory(const char *bin_name, char *buf, size_t buf_size)
{
    size_t path_size = get_max_path_length();
    char *path_buf = malloc(path_size);
    if (!path_buf) {
        // Failed to allocate buffer, terminate
        return -1;
    }

    // Get current working directory
    // TODO: This is Linux only and will require changes on other platforms
    ssize_t link_len = readlink("/proc/self/exe", path_buf, path_size);
    if (link_len < 0) {
        print_info("READLINK ERROR: %i", errno);
        return -1;
    }

    // Merge current working directory with wpa_supplicant name
    path_buf[link_len] = 0;
    dirname(path_buf);
    int r = snprintf(buf, path_size, "%s/%s", path_buf, bin_name);
    free(path_buf);

    return r;
}

ssize_t read_line_from_pipe(int pipe, char *buf, size_t buf_len)
{
    int attempts = 0;
    const static int max_attempts = 5;
    ssize_t read_count = 0;
    while (read_count < buf_len) {
        ssize_t this_read = read(pipe, buf + read_count, 1);
        if (this_read == 0) {
            attempts++;
            if (is_interrupted() || attempts == max_attempts) {
                return -1;
            }
            sleep(1); // Wait for more output
            continue;
        }

        attempts = 0;

        if (buf[read_count] == '\n') {
            buf[read_count] = 0;
            break;
        }

        read_count++;
    }
    return read_count;
}

int wait_for_output(int pipe, const char *expected_output)
{
    static const int max_attempts = 5;
    int nbytes, attempts = 0, success = 0;
    const int expected_len = strlen(expected_output);
    char buf[100];
    int read_count = 0;
    int ret = 0;
    do {
        // Read line from child process
        read_line_from_pipe(pipe, buf, sizeof(buf));

        print_info("SUBPROCESS %s", buf);

        // We got success message!
        if (!memcmp(buf, expected_output, expected_len)) {
            ret = 1;
            break;
        }

        // Haven't gotten success message (yet), wait and try again
    } while (attempts < max_attempts && !is_interrupted());
    
    return ret;
}

int start_wpa_supplicant(const char *wireless_interface, const char *config_file, pid_t *pid)
{
    // TODO: drc-sim has `rfkill unblock wlan`, should we do that too?

    // Kill any potentially orphaned wpa_supplicant_drcs
    const char *wpa_supplicant_drc = "wpa_supplicant_drc";
    const char *kill_argv[] = {"killall", "-9", wpa_supplicant_drc, NULL};
    pid_t kill_pid;
    int kill_pipe;
    int r = start_process(kill_argv, &kill_pid, &kill_pipe, NULL);
    int status;
    waitpid(kill_pid, &status, 0);

    size_t path_size = get_max_path_length();
    char *wpa_buf = malloc(path_size);

    get_binary_in_working_directory(wpa_supplicant_drc, wpa_buf, path_size);

    const char *argv[] = {wpa_buf, "-Dnl80211", "-i", wireless_interface, "-c", config_file, NULL};
    int pipe;

    r = start_process(argv, pid, &pipe, NULL);
    free(wpa_buf);

    if (r != VANILLA_SUCCESS) {
        return r;
    }

    // Wait for WPA supplicant to start
    if (wait_for_output(pipe, "Successfully initialized wpa_supplicant")) {
        // I'm not sure why, but closing this pipe breaks wpa_supplicant in subtle ways, so just leave it.
        //close(pipe);

        // WPA initialized correctly! Continue with action...
        return VANILLA_SUCCESS;
    } else {
        // Give up
give_up:
        kill((*pid), SIGTERM);
        return VANILLA_ERROR;
    }
}

static const char *nmcli = "nmcli";
int is_networkmanager_managing_device(const char *wireless_interface, int *is_managed)
{
    pid_t nmcli_pid;
    int pipe;

    const char *argv[] = {nmcli, "device", "show", wireless_interface, NULL};

    int r = start_process(argv, &nmcli_pid, &pipe, NULL);
    if (r != VANILLA_SUCCESS) {
        // Assume nmcli is not installed so the host is not using NetworkManager
        print_info("FAILED TO LAUNCH NMCLI, RESULTS MAY BE UNPREDICTABLE");
        *is_managed = 0;
        return VANILLA_SUCCESS;
    }

    int status;
    waitpid(nmcli_pid, &status, 0);

    if (!WIFEXITED(status)) {
        // Something went wrong
        print_info("NMCLI DID NOT EXIT NORMALLY");
        return VANILLA_ERROR;
    }

    char buf[100];
    int ret = VANILLA_ERROR;
    while (read_line_from_fd(pipe, buf, sizeof(buf))) {
        if (memcmp(buf, "GENERAL.STATE", 13) == 0) {
            *is_managed = !strstr(buf, "unmanaged");
            ret = VANILLA_SUCCESS;
            goto exit;
        }
    }

exit:
    close(pipe);
    return ret;
}

int set_networkmanager_on_device(const char *wireless_interface, int on)
{
    const char *argv[] = {nmcli, "device", "set", wireless_interface, "managed", on ? "on" : "off", NULL};

    pid_t nmcli_pid;
    int r = start_process(argv, &nmcli_pid, NULL, NULL);
    if (r != VANILLA_SUCCESS) {
        return r;
    }

    int status;
    waitpid(nmcli_pid, &status, 0);
    if (WIFEXITED(status)) {
        return VANILLA_SUCCESS;
    } else {
        return VANILLA_ERROR;
    }
}

int disable_networkmanager_on_device(const char *wireless_interface)
{
    return set_networkmanager_on_device(wireless_interface, 0);
}

int enable_networkmanager_on_device(const char *wireless_interface)
{
    return set_networkmanager_on_device(wireless_interface, 1);
}

int wpa_setup_environment(const char *wireless_interface, const char *wireless_conf_file, ready_callback_t callback, void *callback_data)
{
    int ret = VANILLA_ERROR;

    clear_interrupt();
    //install_interrupt_handler();

    // Check status of interface with NetworkManager
    int is_managed = 0;
    if (is_networkmanager_managing_device(wireless_interface, &is_managed) != VANILLA_SUCCESS) {
        print_info("FAILED TO DETERMINE MANAGED STATE OF WIRELESS INTERFACE");
        //goto die;
    }

    // If NetworkManager is managing this device, temporarily stop it from doing so
    if (is_managed) {
        if (disable_networkmanager_on_device(wireless_interface) != VANILLA_SUCCESS) {
            print_info("FAILED TO SET %s TO UNMANAGED, RESULTS MAY BE UNPREDICTABLE");
        } else {
            print_info("TEMPORARILY SET %s TO UNMANAGED", wireless_interface);
        }
    }

    // Start modified WPA supplicant
    pid_t pid;
    int err = start_wpa_supplicant(wireless_interface, wireless_conf_file, &pid);
    if (err != VANILLA_SUCCESS || is_interrupted()) {
        print_info("FAILED TO START WPA SUPPLICANT");
        goto die_and_reenable_managed;
    }

    // Get control interface
    const size_t buf_len = 1048576;
    char *buf = malloc(buf_len);
    snprintf(buf, buf_len, "%s/%s", wpa_ctrl_interface, wireless_interface);
    struct wpa_ctrl *ctrl;
    while (!(ctrl = wpa_ctrl_open(buf))) {
        if (is_interrupted()) goto die_and_kill;
        print_info("WAITING FOR CTRL INTERFACE");
        sleep(1);
    }

    if (is_interrupted() || wpa_ctrl_attach(ctrl) < 0) {
        print_info("FAILED TO ATTACH TO WPA");
        goto die_and_close;
    }

    ret = callback(ctrl, callback_data);

die_and_detach:
    wpa_ctrl_detach(ctrl);

die_and_close:
    wpa_ctrl_close(ctrl);

die_and_kill:
    kill(pid, SIGTERM);

    free(buf);

die_and_reenable_managed:
    if (is_managed) {
        print_info("SETTING %s BACK TO MANAGED", wireless_interface);
        enable_networkmanager_on_device(wireless_interface);
    }

die:
    // Remove our custom sigint signal handler
    //uninstall_interrupt_handler();

    return ret;
}

int call_dhcp(const char *network_interface, pid_t *dhclient_pid)
{
    const char *argv[] = {"dhclient", "-d", "--no-pid", network_interface, NULL, NULL, NULL};

    size_t buf_size = get_max_path_length();
    char *dhclient_buf = malloc(buf_size);
    char *dhclient_script = NULL;
    get_binary_in_working_directory("dhclient", dhclient_buf, buf_size);

    if (access(dhclient_buf, F_OK) == 0) {
        // HACK: Assume we're working in our deployed environment
        // TODO: Should probably just incorporate dhclient (or something like it) directly as a library
        argv[0] = dhclient_buf;
        argv[4] = "-sf";

        dhclient_script = malloc(buf_size);
        get_binary_in_working_directory("../sbin/dhclient-script", dhclient_script, buf_size);
        argv[5] = dhclient_script;
        
        print_info("Using custom dhclient at: %s", argv[0]);
    } else {
        print_info("Using system dhclient");
    }

    int dhclient_pipe;
    int r = start_process(argv, dhclient_pid, NULL, &dhclient_pipe);
    if (r != VANILLA_SUCCESS) {
        print_info("FAILED TO CALL DHCLIENT");
        return r;
    }

    free(dhclient_buf);
    if (dhclient_script) free(dhclient_script);

    if (wait_for_output(dhclient_pipe, "bound to")) {
        return VANILLA_SUCCESS;
    } else {
        print_info("FAILED TO ESTABLISH DHCP");
        kill(*dhclient_pid, SIGTERM);

        return VANILLA_ERROR;
    }
}

int create_connect_config(const char *input_config, const char *bssid)
{
    FILE *in_file = fopen(input_config, "r");
    if (!in_file) {
        print_info("FAILED TO OPEN INPUT CONFIG FILE");
        return VANILLA_ERROR;
    }
    
    FILE *out_file = fopen(get_wireless_connect_config_filename(), "w");
    if (!out_file) {
        print_info("FAILED TO OPEN OUTPUT CONFIG FILE");
        return VANILLA_ERROR;
    }

    int len;
    char buf[150];
    while (len = read_line_from_file(in_file, buf, sizeof(buf))) {
        if (memcmp("\tssid=", buf, 6) == 0) {
            fprintf(out_file, "\tscan_ssid=1\n\tbssid=%s\n", bssid);
        }

        fwrite(buf, len, 1, out_file);

        if (memcmp(buf, "update_config=1", 15) == 0) {
            static const char *ap_scan_line = "ap_scan=1\n";
            fwrite(ap_scan_line, strlen(ap_scan_line), 1, out_file);
        }
    }

    fclose(in_file);
    fclose(out_file);

    return VANILLA_SUCCESS;
}

int sync_with_console_internal(struct wpa_ctrl *ctrl, uint16_t code)
{
    char buf[16384];
    const size_t buf_len = sizeof(buf);

    int found_console = 0;
    char bssid[18];
    do {
        size_t actual_buf_len;

        if (is_interrupted()) goto exit_loop;

        // Request scan from hardware
        while (1) {
            if (is_interrupted()) goto exit_loop;

            // print_info("SCANNING");
            actual_buf_len = buf_len;
            wpa_ctrl_command(ctrl, "SCAN", buf, &actual_buf_len);

            if (!memcmp(buf, "FAIL-BUSY", 9)) {
                //print_info("DEVICE BUSY, RETRYING");
                sleep(5);
            } else if (!memcmp(buf, "OK", 2)) {
                break;
            } else {
                print_info("UNKNOWN SCAN RESPONSE: %.*s (RETRYING)", actual_buf_len, buf);
                sleep(5);
            }
        }

        //print_info("WAITING FOR SCAN RESULTS");
        actual_buf_len = buf_len;
        wpa_ctrl_command(ctrl, "SCAN_RESULTS", buf, &actual_buf_len);
        print_info("RECEIVED SCAN RESULTS");

        const char *line = strtok(buf, "\n");
        while (line) {
            if (is_interrupted()) goto exit_loop;

            if (strstr(line, "WiiU")) {
                print_info("FOUND WII U, TESTING WPS PIN");

                // Make copy of bssid for later
                strncpy(bssid, line, sizeof(bssid));
                bssid[17] = '\0';

                char wps_buf[100];
                snprintf(wps_buf, sizeof(wps_buf), "WPS_PIN %.*s %04d5678", 17, bssid, code);

                size_t actual_buf_len = buf_len;
                wpa_ctrl_command(ctrl, wps_buf, buf, &actual_buf_len);

                static const int max_wait = 20;
                int wait_count = 0;
                int cred_received = 0;

                while (!is_interrupted()) {
                    while (wait_count < max_wait && !wpa_ctrl_pending(ctrl)) {
                        if (is_interrupted()) goto exit_loop;
                        sleep(1);
                        wait_count++;
                    }

                    if (wait_count == max_wait) {
                        print_info("GIVING UP, RETURNING TO SCANNING");
                        break;
                    }

                    actual_buf_len = buf_len;
                    wpa_ctrl_recv(ctrl, buf, &actual_buf_len);
                    print_info("CRED RECV: %.*s", buf_len, buf);

                    if (!memcmp("<3>WPS-CRED-RECEIVED", buf, 20)) {
                        print_info("RECEIVED AUTHENTICATION FROM CONSOLE");
                        cred_received = 1;
                        break;
                    }
                }

                if (cred_received) {
                    // Tell wpa_supplicant to save config
                    actual_buf_len = buf_len;
                    print_info("SAVING CONFIG", actual_buf_len, buf);
                    wpa_ctrl_command(ctrl, "SAVE_CONFIG", buf, &actual_buf_len);

                    // Create connect config which needs a couple more parameters
                    create_connect_config(get_wireless_authenticate_config_filename(), bssid);

                    found_console = 1;
                }
            }
            line = strtok(NULL, "\n");
        }
    } while (!found_console);

exit_loop:
    return found_console ? VANILLA_SUCCESS : VANILLA_ERROR;
}

struct sync_args {
    uint16_t code;
};

int thunk_to_sync(struct wpa_ctrl *ctrl, void *data)
{
    struct sync_args *args = (struct sync_args *) data;
    return sync_with_console_internal(ctrl, args->code);
}

int os_sync_with_console(const char *wireless_interface, uint16_t code)
{
    const char *wireless_conf_file;

    FILE *config;
    wireless_conf_file = get_wireless_authenticate_config_filename();
    config = fopen(wireless_conf_file, "w");
    if (!config) {
        print_info("FAILED TO WRITE TEMP CONFIG: %s", wireless_conf_file);
        return VANILLA_ERROR;
    }

    fprintf(config, "ctrl_interface=%s\nupdate_config=1\n", wpa_ctrl_interface);
    fclose(config);

    struct sync_args args;
    args.code = code;

    return wpa_setup_environment(wireless_interface, wireless_conf_file, thunk_to_sync, &args);
}

struct connect_args {
    const char *wireless_interface;
    int (*callback)(void *);
    void *context;
};

int os_thunk_to_connect(struct wpa_ctrl *ctrl, void *data)
{
    struct connect_args *args = (struct connect_args *) data;

    while (1) {
        while (!wpa_ctrl_pending(ctrl)) {
            sleep(2);
            print_info("WAITING FOR CONNECTION");

            if (is_interrupted()) return VANILLA_ERROR;
        }

        char buf[1024];
        size_t actual_buf_len = sizeof(buf);
        wpa_ctrl_recv(ctrl, buf, &actual_buf_len);

        if (memcmp(buf, "<3>CTRL-EVENT-CONNECTED", 23) == 0) {
            break;
        }

        if (is_interrupted()) return VANILLA_ERROR;
    }

    print_info("CONNECTED TO CONSOLE");

    // Use DHCP on interface
    pid_t dhclient_pid;
    int r = call_dhcp(args->wireless_interface, &dhclient_pid);
    if (r != VANILLA_SUCCESS) {
        print_info("FAILED TO RUN DHCP ON %s", args->wireless_interface);
        return r;
    } else {
        print_info("DHCP ESTABLISHED");
    }

    {
        // Destroy default route that dhclient will have created
        pid_t ip_pid;
        const char *ip_args[] = {"ip", "route", "del", "default", "via", "192.168.1.1", "dev", args->wireless_interface, NULL};
        r = start_process(ip_args, &ip_pid, NULL, NULL);
        if (r != VANILLA_SUCCESS) {
            print_info("FAILED TO REMOVE CONSOLE ROUTE FROM SYSTEM");
        }

        int ip_status;
        waitpid(ip_pid, &ip_status, 0);

        if (!WIFEXITED(ip_status)) {
            print_info("FAILED TO REMOVE CONSOLE ROUTE FROM SYSTEM");
        }
    }

    r = args->callback(args->context);

    int kill_ret = kill(dhclient_pid, SIGTERM);
    print_info("killing dhclient %i: %i", dhclient_pid, kill_ret);

    return r;
}

int os_connect_to_console(const char *wireless_interface, int (*callback)(void *), void *context)
{
    struct connect_args args;
    args.wireless_interface = wireless_interface;
    args.callback = callback;
    args.context = context;
    return wpa_setup_environment(wireless_interface, get_wireless_connect_config_filename(), os_thunk_to_connect, &args);
}

int os_open_udp_socket(void **socket_out, uint16_t port)
{
    // TODO: Limit these sockets to one interface?

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    int skt = socket(AF_INET, SOCK_DGRAM, 0);
    (*socket_out) = (void *) skt;
    
    //setsockopt((*socket_out), SOL_SOCKET, SO_RCVTIMEO)
    
    if (bind(skt, (const struct sockaddr *) &address, sizeof(address)) == -1) {
        print_info("FAILED TO BIND PORT %u: %i", port, errno);
        return 0;
    }

    return 1;
}

ssize_t os_read_from_udp_socket(const void *socket, void *data, size_t max)
{
    return recv((int) socket, data, max, 0);
}

int os_write_to_udp_socket(const void *from_socket, uint32_t to_address, uint16_t to_port, const void *data, size_t length)
{
    int fd = (int) socket;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = to_address;
    address.sin_port = htons(to_port);
    ssize_t sent = sendto(fd, data, length, 0, (const struct sockaddr *) &address, sizeof(address));
    if (sent == -1) {
        print_info("Failed to send to Wii U socket: fd - %d; port - %d", fd, to_port);
        return 0;
    } else {
        return 1;
    }
}

int os_close_udp_socket(void *socket)
{
    close((int) socket);
}