/*
 * ctrl_map.c -- Binding tables and registration for control surface maps.
 * Ported from XDJ-RX3 reference architecture for AlphaTheta XDJ-AZ / S905X.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrl_map.h"

struct note_ctrl note_map[CTRL_NKEYS];
int note_map_n = 0;

struct abs_ctrl abs_map[CTRL_NABS];
int abs_map_n = 0;

void ctrl_bindings_reset(void)
{
    note_map_n = 0;
    abs_map_n = 0;
}

void add_note(int rch, int note, int key, int sch)
{
    if (note_map_n >= CTRL_NKEYS)
        return;
    note_map[note_map_n].rch = rch;
    note_map[note_map_n].note = note;
    note_map[note_map_n].key = key;
    note_map[note_map_n].sch = sch;
    note_map[note_map_n].pressed = 0;
    note_map[note_map_n].repeat = 0;
    note_map_n++;
}

void add_note_repeat(int rch, int note, int key, int sch)
{
    if (note_map_n >= CTRL_NKEYS)
        return;
    add_note(rch, note, key, sch);
    note_map[note_map_n - 1].repeat = 1;
}

void add_abs(int rch, int cc, int key, int sch)
{
    if (abs_map_n >= CTRL_NABS)
        return;
    abs_map[abs_map_n].rch = rch;
    abs_map[abs_map_n].cc = cc;
    abs_map[abs_map_n].key = key;
    abs_map[abs_map_n].sch = sch;
    abs_map[abs_map_n].last = -1;
    abs_map_n++;
}

void ctrl_abs_invalidate(void)
{
    for (int i = 0; i < abs_map_n; i++)
        abs_map[i].last = -1;
}

