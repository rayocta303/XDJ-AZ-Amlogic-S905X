/*
 * menu_paint.h -- Framebuffer painting structures and Pioneer palette definitions
 * for AlphaTheta XDJ-AZ / XDJ-RX3 shims.
 */
#ifndef XDJ_AZ_MENU_PAINT_H
#define XDJ_AZ_MENU_PAINT_H

#include "menu_zone.h"

/* The target drawing surface/view */
struct menu_view {
    void *pix;          /* Base pointer to pixel buffer (e.g. mmapped /dev/fb0) */
    int   pitch;        /* Pitch in pixels (1280) */
    int   bpp;          /* Bits per pixel (32 for XRGB8888, 16 for RGB565) */
    int   bx, by;       /* Origin X, Y in framebuffer coordinates */
    int   dw, dh;       /* Display width and height in pixels */
    int   fb_w, fb_h;   /* Underlying framebuffer width and height */
};

/* Palette class enumeration */
enum {
    MENU_NONE = 0,
    MENU_FILL,          /* Solid black #000000 */
    MENU_BORDER,        /* Dark grey border #333333 */
    MENU_DIV,           /* Divider #222222 */
    MENU_BTN,           /* Inactive button face */
    MENU_BTN_PRESSED,   /* Pressed button face */
    MENU_LABEL,         /* Active label white #FFFFFF */
    MENU_LABEL_PRESSED, /* Pressed label white #FFFFFF */
    MENU_FX_PLATE,      /* Pioneer panel plate grey #202020 (32, 32, 32) */
    MENU_FX_SEL,        /* Pioneer active selection blue #007DDE (0, 125, 222) */
    MENU_CUE,           /* Pioneer CUE orange #FF8C00 */
    MENU_PLAY           /* Pioneer PLAY green #28D246 */
};

/**
 * @brief Resolves color value for specified depth and palette class.
 */
static inline unsigned int menu_pixel(int bpp, int cls) {
    if (bpp == 32) {
        switch (cls) {
        case MENU_FILL:          return 0x00000000u;
        case MENU_BORDER:        return 0x00333333u;
        case MENU_DIV:           return 0x00222222u;
        case MENU_BTN:           return 0x00181818u;
        case MENU_BTN_PRESSED:   return 0x00007DDEu;
        case MENU_LABEL:         return 0x00FFFFFFu;
        case MENU_LABEL_PRESSED: return 0x00FFFFFFu;
        case MENU_FX_PLATE:      return 0x00202020u; /* RGB(32, 32, 32) */
        case MENU_FX_SEL:        return 0x00007DDEu; /* Pioneer Blue (0, 125, 222) */
        case MENU_CUE:           return 0x00FF8C00u;
        case MENU_PLAY:          return 0x0028D246u;
        default:                 return 0x00000000u;
        }
    } else {
        /* 16-bit RGB565 */
        switch (cls) {
        case MENU_FILL:          return 0x0000u;
        case MENU_BORDER:        return 0x3186u;
        case MENU_DIV:           return 0x2104u;
        case MENU_BTN:           return 0x18C3u;
        case MENU_BTN_PRESSED:   return 0x03FBu;
        case MENU_LABEL:         return 0xFFFFu;
        case MENU_LABEL_PRESSED: return 0xFFFFu;
        case MENU_FX_PLATE:      return 0x2104u;
        case MENU_FX_SEL:        return 0x03FBu;
        case MENU_CUE:           return 0xFC60u;
        case MENU_PLAY:          return 0x2E88u;
        default:                 return 0x0000u;
        }
    }
}

#endif /* XDJ_AZ_MENU_PAINT_H */

