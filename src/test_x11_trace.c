#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>
#include <X11/Xlib.h>

int XPutImage(Display *d, Drawable dr, GC gc, XImage *img, int sx, int sy, int dx, int dy, unsigned w, unsigned h) {
    static int (*real)(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned, unsigned) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "XPutImage");
    static int cnt = 0;
    if (cnt < 20) {
        fprintf(stderr, "[XPutImage #%d] dr=%lu dx=%d dy=%d w=%u h=%u bpp=%d depth=%d bpl=%d\n",
                cnt++, (unsigned long)dr, dx, dy, w, h, img->bits_per_pixel, img->depth, img->bytes_per_line);
    }
    return real(d, dr, gc, img, sx, sy, dx, dy, w, h);
}

int XCopyArea(Display *d, Drawable src, Drawable dst, GC gc, int sx, int sy, unsigned w, unsigned h, int dx, int dy) {
    static int (*real)(Display*, Drawable, Drawable, GC, int, int, unsigned, unsigned, int, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "XCopyArea");
    static int cnt = 0;
    if (cnt < 20) {
        fprintf(stderr, "[XCopyArea #%d] src=%lu dst=%lu sx=%d sy=%d w=%u h=%u dx=%d dy=%d\n",
                cnt++, (unsigned long)src, (unsigned long)dst, sx, sy, w, h, dx, dy);
    }
    return real(d, src, dst, gc, sx, sy, w, h, dx, dy);
}

