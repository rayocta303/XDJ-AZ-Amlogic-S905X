#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static const char *fx_names[14] = {
    "DELAY", "ECHO", "PING PONG", "SPIRAL", "HELIX",
    "REVERB", "FLANGER", "PHASER", "FILTER", "TRANS",
    "ROLL", "SLIP ROLL", "PITCH", "VINYL BRAKE"
};

int main() {
    Display *d = XOpenDisplay(":0");
    if (!d) {
        fprintf(stderr, "Failed to open X11 display :0\n");
        return 1;
    }
    int screen = DefaultScreen(d);
    XSetWindowAttributes attr;
    attr.override_redirect = True;

    int win_x = 1090;
    int win_y = 85;
    int win_w = 180;
    int win_h = 406;

    Window win = XCreateWindow(d, RootWindow(d, screen),
                               win_x, win_y, win_w, win_h, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect, &attr);
    XMapRaised(d, win);

    GC gc = XCreateGC(d, win, 0, NULL);

    /* Background: #202020 */
    XSetForeground(d, gc, 0x202020);
    XFillRectangle(d, win, gc, 0, 0, win_w, win_h);

    /* 14 rows */
    for (int i = 0; i < 14; i++) {
        int ry = i * 29;
        /* Row box: dark grey with highlighted first item */
        XSetForeground(d, gc, (i == 0) ? 0x0070B0 : 0x181818);
        XFillRectangle(d, win, gc, 2, ry + 2, win_w - 4, 26);

        XSetForeground(d, gc, 0x333333);
        XDrawRectangle(d, win, gc, 2, ry + 2, win_w - 5, 25);

        /* Text: White */
        XSetForeground(d, gc, 0xFFFFFF);
        XDrawString(d, win, gc, 16, ry + 19, fx_names[i], strlen(fx_names[i]));
    }

    XFlush(d);
    printf("X11 Picker displayed on :0!\n");
    sleep(3);

    XUnmapWindow(d, win);
    XDestroyWindow(d, win);
    XCloseDisplay(d);
    return 0;
}

