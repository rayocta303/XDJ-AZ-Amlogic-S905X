#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int main(void) {
    Display *dpy = XOpenDisplay(":0");
    if (!dpy) {
        fprintf(stderr, "Cannot open display :0\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes attr;
    attr.override_redirect = True;
    /* Bounds matching Row 3, 4, 5 of Beat FX panel: x=1090, y=175, w=180, h=145 */
    Window win = XCreateWindow(dpy, root, 1090, 175, 180, 145, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect, &attr);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    XMapRaised(dpy, win);
    XSync(dpy, False);

    /* 1. Background fill: pure Pioneer black #000000 */
    XSetForeground(dpy, gc, 0x000000);
    XFillRectangle(dpy, win, gc, 0, 0, 180, 145);

    /* 2. Top & Bottom borders of effect name cell: #2C2C2C */
    XSetForeground(dpy, gc, 0x2C2C2C);
    XDrawLine(dpy, win, gc, 0, 0, 179, 0);
    XDrawLine(dpy, win, gc, 0, 42, 179, 42);
    XDrawLine(dpy, win, gc, 0, 87, 179, 87);

    /* 3. Row 1: Effect Name "DELAY" centered at y=26 */
    XFontStruct *font = XLoadQueryFont(dpy, "-*-helvetica-bold-r-normal--18-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    XSetForeground(dpy, gc, 0xFFFFFF);
    const char *fx = "DELAY";
    int text_w = font ? XTextWidth(font, fx, (int)strlen(fx)) : 50;
    int fx_x = (180 - text_w) / 2;
    XDrawString(dpy, win, gc, fx_x, 27, fx, (int)strlen(fx));

    /* 4. Row 2: Beat divisions: 3/4 | [ 1 ] | 2 */
    /* Left division: "3/4" */
    const char *d_left = "3/4";
    int d_left_w = font ? XTextWidth(font, d_left, (int)strlen(d_left)) : 25;
    XDrawString(dpy, win, gc, (60 - d_left_w) / 2, 70, d_left, (int)strlen(d_left));

    /* Active center button: [ 1 ] at x=61, y=44, w=58, h=42 */
    /* Pioneer dark grey fill: #777777 */
    XSetForeground(dpy, gc, 0x777777);
    XFillRectangle(dpy, win, gc, 61, 44, 58, 42);

    /* 1px dark border: #333333 */
    XSetForeground(dpy, gc, 0x333333);
    XDrawRectangle(dpy, win, gc, 61, 44, 57, 41);

    /* Crisp white "1" glyph centered in box */
    XSetForeground(dpy, gc, 0xFFFFFF);
    const char *d_mid = "1";
    int d_mid_w = font ? XTextWidth(font, d_mid, (int)strlen(d_mid)) : 10;
    XDrawString(dpy, win, gc, 61 + (58 - d_mid_w) / 2, 70, d_mid, (int)strlen(d_mid));

    /* Right division: "2" */
    const char *d_right = "2";
    int d_right_w = font ? XTextWidth(font, d_right, (int)strlen(d_right)) : 10;
    XDrawString(dpy, win, gc, 120 + (60 - d_right_w) / 2, 70, d_right, (int)strlen(d_right));

    /* 5. Row 3: TIME 500 ms */
    /* Label "TIME" in dim white/grey #999999 */
    XFontStruct *small_font = XLoadQueryFont(dpy, "-*-helvetica-medium-r-normal--12-*-*-*-*-*-*-*");
    if (!small_font) small_font = font;
    if (small_font) XSetFont(dpy, gc, small_font->fid);

    XSetForeground(dpy, gc, 0x999999);
    XDrawString(dpy, win, gc, 20, 118, "TIME", 4);

    /* Value "500 ms" in crisp white #FFFFFF */
    if (font) XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, 0xFFFFFF);
    XDrawString(dpy, win, gc, 75, 120, "500 ms", 6);

    XFlush(dpy);
    printf("Beat FX overlay window rendered successfully.\n");
    sleep(10); /* Hold for 10 seconds for screenshot */

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

