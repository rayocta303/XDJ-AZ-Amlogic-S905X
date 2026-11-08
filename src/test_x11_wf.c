#include <stdio.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

bool is_waveform_screen(Display *dpy, Window root) {
    XImage *chk = XGetImage(dpy, root, 80, 35, 65, 1, AllPlanes, ZPixmap);
    if (!chk) return false;
    unsigned long pix_f = XGetPixel(chk, 0, 0);     /* (80, 35) */
    unsigned long pix_m = XGetPixel(chk, 60, 0);    /* (140, 35) */
    XDestroyImage(chk);

    int r_f = (pix_f >> 16) & 0xFF;
    int g_f = (pix_f >> 8) & 0xFF;
    int b_f = pix_f & 0xFF;

    int r_m = (pix_m >> 16) & 0xFF;
    int g_m = (pix_m >> 8) & 0xFF;
    int b_m = pix_m & 0xFF;

    printf("F(80,35): RGB(%d,%d,%d), M(140,35): RGB(%d,%d,%d)\n", r_f, g_f, b_f, r_m, g_m, b_m);

    bool f_white = (r_f >= 200 && g_f >= 200 && b_f >= 200);
    bool m_white = (r_m >= 200 && g_m >= 200 && b_m >= 200);

    return f_white && m_white;
}

int main(void) {
    Display *dpy = XOpenDisplay(":0");
    if (!dpy) { printf("Cannot open display\n"); return 1; }
    Window root = RootWindow(dpy, DefaultScreen(dpy));
    bool wf = is_waveform_screen(dpy, root);
    printf("is_waveform_screen returned: %s\n", wf ? "TRUE" : "FALSE");
    XCloseDisplay(dpy);
    return 0;
}
