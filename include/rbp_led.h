/*
 * rbp_led.h -- DDJ-FLX6 / XDJ-AZ Hardware LED and Illumination Bridge
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 */
#ifndef XDJ_AZ_RBP_LED_H
#define XDJ_AZ_RBP_LED_H

#include <stdint.h>
#include <stdbool.h>

/* Deck LED slots */
enum {
    L_SYNC, L_MASTER, L_CUE, L_PLAY, L_KEYLOCK, L_VINYL, L_SLIP,
    L_LOOPIN, L_LOOPOUT, L_RELOOP, L_CALL_PREV, L_CALL_NEXT,
    L_MODE0, L_MODE1, L_MODE2, L_MODE3,
    L_SLOT_COUNT
};

/* Armed latch per deck for Loop-in */
extern int led_loop_armed[4];

/* Trigger track load animation (ONE-SHOT) */
void led_trigger_load_animation(int deck);

/* LED stream thread */
void *led_thread(void *arg);

#endif /* XDJ_AZ_RBP_LED_H */

