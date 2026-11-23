/* opaco: the Allwinner display engine's UI layer (channel 1, layer 0) opaque or not, on the TrimUI Smart Pro /
 * Brick Pro (kernel 4.9, include/video/sunxi_display2.h's structures).
 *   opaco on    global alpha 255: everything the game draws is seen
 *   opaco off   per-pixel alpha (the firmware's own, as at the start)
 * The PowerVR GE8300's EGL picks a surface with an alpha channel and the display engine, with per-pixel alpha, then
 * shows the picture transparent: a black screen with sound. The same fix as spruceOS's ("setalpha 0"). Written for
 * the Mosaik firmware by this port's contributor, and contributed here under this project's MIT licence.
 * Built by inside.sh; launch.sh runs "opaco on" before the game and "opaco off" after it. */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct disp_rect { int x, y; unsigned int width, height; };
struct disp_rectsz { unsigned int width, height; };
struct disp_rect64 { long long x, y, width, height; };
struct disp_fb_info {
    unsigned long long addr[3];
    struct disp_rectsz size[3];
    unsigned int align[3];
    int format;
    int color_space;
    unsigned int trd_right_addr[3];
    bool pre_multiply;
    struct disp_rect64 crop;
    int flags;
    int scan;
};
struct disp_layer_info {
    int mode;
    unsigned char zorder, alpha_mode, alpha_value;
    struct disp_rect screen_win;
    bool b_trd_out;
    int out_trd_mode;
    union { unsigned int color; struct disp_fb_info fb; };
    unsigned int id;
};
struct disp_layer_config { struct disp_layer_info info; bool enable; unsigned int channel, layer_id; };
#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

int main(int argc, char **argv) {
    bool on = argc == 2 && !strcmp(argv[1], "on");
    if (argc != 2 || (!on && strcmp(argv[1], "off"))) {
        fprintf(stderr, "usage: opaco on|off\n");
        return 2;
    }
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) { perror("opaco: /dev/disp"); return 1; }
    struct disp_layer_config c;
    memset(&c, 0, sizeof c);
    c.channel = 1;
    c.layer_id = 0;
    unsigned long arg[4] = { 0, (unsigned long)&c, 1, 0 };
    if (ioctl(fd, DISP_LAYER_GET_CONFIG, arg) < 0) { perror("opaco: reading the layer"); close(fd); return 1; }
    unsigned char before = c.info.alpha_mode;
    c.info.alpha_mode = on ? 1 : 0;   /* 1 global, 0 per pixel */
    if (on) c.info.alpha_value = 255;
    if (ioctl(fd, DISP_LAYER_SET_CONFIG, arg) < 0) { perror("opaco: writing the layer"); close(fd); return 1; }
    close(fd);
    printf("opaco: layer 1.0 alpha %s (was mode %d)\n", on ? "global 255" : "per pixel", before);
    return 0;
}
