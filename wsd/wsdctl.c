/* wsdctl: test-only host tool for wsd. Sends synthetic input (WSD_DEBUG_INPUT) to the largest visible window.
 * Usage: wsdctl SOCKET key KEYCODE down|up | tap KEYCODE | click X Y | hover X Y | move COUNT HZ
 *        | window move X Y | window size W H | window minimize|restore|zoom|close|quit  (screen points) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "wsd_protocol.h"
static int sendmsg_wsd(int fd, uint32_t type, const void *p, uint32_t len)
{
    struct wsd_hdr h = {type, len};
    if (write(fd, &h, sizeof(h)) != sizeof(h)) return -1;
    return write(fd, p, len) == (ssize_t)len ? 0 : -1;
}
int main(int argc, char **argv)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    struct wsd_hello hello = {WSD_PROTOCOL_VERSION, (uint32_t)getpid()};
    struct wsd_debug_input in = {0};
    int fd;
    if (argc < 4) {
        fprintf(stderr, "usage: wsdctl SOCKET key CODE down|up | tap CODE | click X Y | hover X Y | move COUNT HZ | "
                        "window move X Y | window size W H | window minimize|restore|zoom|close|quit\n");
        return 2;
    }
    strncpy(a.sun_path, argv[1], sizeof(a.sun_path) - 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 1; }
    sendmsg_wsd(fd, WSD_HELLO, &hello, sizeof(hello));
    if (!strcmp(argv[2], "key") && argc >= 5) {
        in.kind = 1; in.a = atoi(argv[3]); in.b = !strcmp(argv[4], "down");
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
    } else if (!strcmp(argv[2], "tap")) {
        in.kind = 1; in.a = atoi(argv[3]); in.b = 1;
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
        usleep(80000);
        in.b = 0;
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
    } else if (!strcmp(argv[2], "move") && argc >= 5) {
        /* move COUNT HZ: COUNT raw motions of +-6 px at HZ per second (relative mouse test) */
        int count = atoi(argv[3]), hz = atoi(argv[4]);
        for (int i = 0; i < count; i++) {
            in.kind = 3; in.a = 10000 + ((i & 1) ? 6 : -6); in.b = 10000 + ((i & 2) ? 3 : -3);
            sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
            usleep(1000000 / (hz > 0 ? hz : 1000));
        }
    } else if (!strcmp(argv[2], "window")) {
        static const char *const actions[] = {"", "move", "size", "minimize", "restore", "zoom", "close", "quit"};
        in.kind = 4;
        for (uint32_t i = 1; i < sizeof(actions) / sizeof(*actions); i++) if (!strcmp(argv[3], actions[i])) in.a = i;
        if (argc >= 6) { in.b = atoi(argv[4]); in.reserved = atoi(argv[5]); }
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
    } else if (!strcmp(argv[2], "hover") && argc >= 5) {
        in.kind = 5; in.a = atoi(argv[3]); in.b = atoi(argv[4]);
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
    } else if (!strcmp(argv[2], "click") && argc >= 5) {
        in.kind = 2; in.a = atoi(argv[3]); in.b = atoi(argv[4]);
        sendmsg_wsd(fd, WSD_DEBUG_INPUT, &in, sizeof(in));
    }
    usleep(100000);
    close(fd);
    return 0;
}
