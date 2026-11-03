/*
 * flx6_bridge.h -- RX3-compatible interface and key definitions for XDJ-AZ
 * Target: Amlogic S905X + AlphaTheta XDJ-AZ EP147 + DDJ-FLX6
 */
#ifndef XDJ_AZ_FLX6_BRIDGE_H
#define XDJ_AZ_FLX6_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Internal Keycodes (matching rbp_abi.h) */
#define K_SELECTOR      0x420c
#define K_BROWSE        0x0202
#define K_SOURCE        0x0201
#define K_TAGLIST       0x0203
#define K_MENU          0x0206
#define K_INFO          0x020b
#define K_PLAYLIST      0x0204
#define K_SEARCH        0x0205
#define K_SHORTCUT      0x0210
#define K_TRACKFILTER   0x420f
#define K_BACK          0x420d
#define K_LOAD          0x4311
#define K_PLAY          0x4101
#define K_CUE           0x4102
#define K_SYNC          0x4112
#define K_VINYL         0x4104
#define K_JOG_TOUCH     0x4306
#define K_JOG_ROT       0x4305

/* Pioneer XDJ-AZ Native Jog Protocol Constants */
#define JOG_TOUCH_FWD     0x0F
#define JOG_TOUCH_BACK    0x0B
#define JOG_TOUCH_RELEASE 0x00
#define JOG_STOPPED       0xFFFF
#define K_TEMPO_RANGE   0x4107
#define K_MT            0x4108
#define K_TEMPO_SLIDER  0x4109
#define K_QUANTIZE      0x410b
#define K_ALOOP         0x4114
#define K_HOTCUE        0x4113
#define K_SLIPLOOP      0x4115
#define K_BEATJUMP      0x4116
#define K_TRFWD         0x4214
#define K_TRREV         0x4215
#define K_SRFWD         0x411f
#define K_SRREV         0x4120
#define K_CALL_NEXT     0x4322
#define K_CALL_PREV     0x4323
#define K_MEM_CUE       0x4125
#define K_MEM_DEL       0x4124
#define K_PAD1          0x4117
#define K_LOOPIN        0x410c
#define K_LOOPOUT       0x410d
#define K_RELOOP        0x410e
#define K_REV           0x410f
#define K_SLIP          0x4110
#define K_MASTER        0x4111
#define K_MASTERCUE     0x4407
#define K_TRIM          0x5019
#define K_EQH           0x501a
#define K_EQM           0x501b
#define K_EQL           0x501c
#define K_FADER         0x501e
#define K_XFADER        0x6017
#define K_HPMIX         0x4405
#define K_HPLEVEL       0x4406
#define K_MASTERLVL     0x4403
#define K_COLOR         0x509d
#define K_FILTER        0x50a6
#define K_DUBECHO       0x50a2
#define K_SWEEP         0x50a3
#define K_NOISE         0x50a4
#define K_SPACE         0x50a5
#define K_CRUSH         0x50a1
#define K_BFXTYPE       0x448b
#define K_BFXCH         0x448c
#define K_BFX           0x448d
#define K_TIME          0x448e
#define K_DEPTH         0x448f
#define K_BEATPREV      0x4490
#define K_BEATNEXT      0x4491
#define K_TAP           0x4492

/* Operation codes */
#define OP_PRESS        0
#define OP_REPEAT       1
#define OP_RELEASE      2
#define OP_ROTATE       4
#define OP_VALUE        5

#define CH_GLOBAL       1
#define BFX_CH_MASTER   5
#define BROWSE_MODE_UTILITY 1

/* Compatibility stubs / defines */
extern uint32_t g_dummy_mixer_route[4];
#define ADDR_MIXER_ROUTE_PLAYER0 (&g_dummy_mixer_route[0])
#define ADDR_MIXER_ROUTE_PLAYER1 (&g_dummy_mixer_route[1])
#define MIXER_ROUTE_INPUT0 0
#define MIXER_ROUTE_INPUT1 1
#define UIBROWSE_GLOBAL    0
#define UIBROWSE_OFF_MODE  0

static inline int is_rbp_process(void) { return 1; }

/* Bridge function declarations */
void send_rx_key(int key, int op, int sch, long val);
void send_rx_key_f(int key, int op, int sch, long val, float fval);
void send_rx_key_fl(int key, int op, int sch, long val, float fval, long lval);

void me_set_channel_fader(int input, float f);
float me_get_channel_fader(int input);
void me_set_crossfader(float f);
float me_get_crossfader(void);
void me_set_crossfader_assign(int input, int assign);
int me_get_crossfader_assign(int input);
void me_set_crossfader_curve(int curve);
int me_get_crossfader_curve(void);

void me_set_cue(int idx, int on);
int me_get_cue(int idx);
void me_set_master_cue(int on);
int me_get_master_cue(void);

int midi_note(int midi_ch, int note, int val);
int midi_cc(int midi_ch, int cc, int val);

int cc_to_10bit(int v);
unsigned long long shim_now_ms(void);
void klog(const char *fmt, ...);

int env_num(const char *name, int def);
double env_dnum(const char *name, double def);
int env_on(const char *name, int def);

int rbp_is_playing(int deck);
int rbp_is_loaded(int deck);
long rbp_get_playing_pos(int deck);
int rbp_has_arrived_at_end(int deck);
int rbp_beatfx_numer(void);
int rbp_beatfx_denom(void);
int rbp_beatfx_on(void);
int rbp_beatfx_type(void);
int rbp_beatfx_channel(void);
int hotcue_delete(int deck, int pad);

void *mixer_engine(void);

/* Bridge lifecycle */
void az_bridge_init(void);
void az_bridge_set_midi_fd(int fd);

/* Deck selection */
extern int active_left_deck;
extern int active_right_deck;
void flx6_switch_deck(int deck);
int flx6_get_active_deck(int side);

#endif /* XDJ_AZ_FLX6_BRIDGE_H */

