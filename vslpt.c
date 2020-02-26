/*
  Vienna Symphonic Library Performance Tool LV2 Plugin
  Copyright 2020 Reese Wang <thuwrx10@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/core/lv2.h"
#include "lv2/core/lv2_util.h"
#include "lv2/patch/patch.h"
#include "lv2/state/state.h"
#include "lv2/log/log.h"
#include "lv2/log/logger.h"
#include "lv2/midi/midi.h"
#include "lv2/urid/urid.h"

#define VSLPT_URI "https://github.com/ReeseWang/vslpt-lv2"
#define VSL_FIRSTNOTE_KEYSW 0x0D
#define VSL_RELEASE_KEYSW   0x0E
#define VSL_REPEAT_KEYSW    0x0F
#define VSL_INTERVAL_KEYSW_BASE 0x00

typedef struct {
    LV2_URID atom_Path;
    LV2_URID atom_Resource;
    LV2_URID atom_Sequence;
    LV2_URID atom_URID;
    LV2_URID atom_eventTransfer;
    LV2_URID midi_Event;
    LV2_URID patch_Set;
    LV2_URID patch_property;
    LV2_URID patch_value;
} VSLPTURIs;

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    MIDI_IN  = 0,
    MIDI_OUT = 1
};

// Struct for a 3 byte MIDI event, used for writing notes
typedef struct {
    LV2_Atom_Event event;
    uint8_t        msg[3];
} MIDIEvent;

#define NOTE_STACK_SIZE 32
#define MIDI_CHANNELS 16
typedef struct {
    // Features
    LV2_URID_Map*  map;
    LV2_Log_Logger logger;

    // Ports
    const LV2_Atom_Sequence* in_port;
    LV2_Atom_Sequence*       out_port;

    // URIs
    VSLPTURIs uris;

    // Status
    uint8_t note_stack[MIDI_CHANNELS][NOTE_STACK_SIZE][2];
    uint8_t note_stack_top[MIDI_CHANNELS];
    uint8_t active_note[MIDI_CHANNELS];
    uint8_t control_note[MIDI_CHANNELS];
    uint32_t out_capacity;
    MIDIEvent evbuf;
} VSLPT;

static inline void
map_uris(LV2_URID_Map* map, VSLPTURIs* uris)
{
    uris->atom_Path          = map->map(map->handle, LV2_ATOM__Path);
    uris->atom_Resource      = map->map(map->handle, LV2_ATOM__Resource);
    uris->atom_Sequence      = map->map(map->handle, LV2_ATOM__Sequence);
    uris->atom_URID          = map->map(map->handle, LV2_ATOM__URID);
    uris->atom_eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    uris->midi_Event         = map->map(map->handle, LV2_MIDI__MidiEvent);
    uris->patch_Set          = map->map(map->handle, LV2_PATCH__Set);
    uris->patch_property     = map->map(map->handle, LV2_PATCH__property);
    uris->patch_value        = map->map(map->handle, LV2_PATCH__value);
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
    VSLPT* self = (VSLPT*)instance;
    switch (port) {
    case MIDI_IN:
        self->in_port = (const LV2_Atom_Sequence*)data;
        break;
    case MIDI_OUT:
        self->out_port = (LV2_Atom_Sequence*)data;
        break;
    default:
        break;
    }
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               path,
            const LV2_Feature* const* features)
{
    // Allocate and initialise instance structure.
    VSLPT* self = (VSLPT*)calloc(1, sizeof(VSLPT));
    if (!self) {
        return NULL;
    }

    // Scan host features for URID map
    const char*  missing = lv2_features_query(
        features,
        LV2_LOG__log,  &self->logger.log, false,
        LV2_URID__map, &self->map,        true,
        NULL);
    lv2_log_logger_set_map(&self->logger, self->map);
    if (missing) {
        lv2_log_error(&self->logger, "Missing feature <%s>\n", missing);
        free(self);
        return NULL;
    }

    map_uris(self->map, &self->uris);
    memset(self->note_stack_top, 0, MIDI_CHANNELS);
    memset(self->active_note, 0xFF, MIDI_CHANNELS);
    memset(self->control_note, 0xFF, MIDI_CHANNELS);

    return (LV2_Handle)self;
}

static void
cleanup(LV2_Handle instance)
{
    free(instance);
}

static void
add_to_note_stack(VSLPT *self,
        const uint8_t* const msg)
{
    uint8_t ch = msg[0] & 0x0F;
    if (self->note_stack_top[ch] == 0)
    {
        self->note_stack_top[ch] = 1;
        self->note_stack[ch][0][0] = msg[1];
        self->note_stack[ch][0][1] = msg[2];
    }
    else
    {
        for (uint8_t i=0; i<self->note_stack_top[ch]; i++)
            if (self->note_stack[ch][i][0] == msg[1])
            {
                // If has dup notes, update velocity and return
                self->note_stack[ch][i][1] = msg[2];
                return;
            }
        // If no duplicated notes, push to stack
        self->note_stack[ch][self->note_stack_top[ch]][0] = msg[1];
        self->note_stack[ch][self->note_stack_top[ch]][1] = msg[2];
        self->note_stack_top[ch]++;
    }
}

static void
remove_from_note_stack(VSLPT *self,
        const uint8_t* const msg)
{
    uint8_t ch = msg[0] & 0x0F;
    if (self->note_stack_top[ch] != 0)
    {
        for (uint8_t i=0; i<self->note_stack_top[ch]; i++)
            if (self->note_stack[ch][i][0] == msg[1])
            {
                // Remove from stack
                self->note_stack_top[ch]--;
                for (uint8_t j=i; j<self->note_stack_top[ch]; j++)
                {
                    self->note_stack[ch][j][0] = self->note_stack[ch][j+1][0];
                    self->note_stack[ch][j][1] = self->note_stack[ch][j+1][1];
                }
            }
    }
}

static inline void
send_note_on(VSLPT *self, uint8_t ch, uint8_t note, uint8_t vel)
{
    self->evbuf.msg[0] = LV2_MIDI_MSG_NOTE_ON | (0x0F & ch);
    self->evbuf.msg[1] = note;
    self->evbuf.msg[2] = vel;
    lv2_atom_sequence_append_event(
            self->out_port, self->out_capacity, &(self->evbuf.event));
}

static inline void
send_note_off(VSLPT *self, uint8_t ch, uint8_t note)
{
    self->evbuf.msg[0] = LV2_MIDI_MSG_NOTE_OFF | (0x0F & ch);
    self->evbuf.msg[1] = note;
    self->evbuf.msg[2] = 64;
    lv2_atom_sequence_append_event(
            self->out_port, self->out_capacity, &(self->evbuf.event));
}

static void
generate_note_events(VSLPT *self,
        const uint8_t* const msg)
{
    uint8_t ch = msg[0] & 0x0F;
    if (self->active_note[ch] == 0xFF && self->note_stack_top[ch] > 0)
    {
        send_note_on(self, ch, VSL_FIRSTNOTE_KEYSW, 100);
        self->control_note[ch] = VSL_FIRSTNOTE_KEYSW;
        send_note_on(self, 
                ch, 
                self->note_stack[ch][self->note_stack_top[ch] - 1][0] - 24,
                self->note_stack[ch][self->note_stack_top[ch] - 1][1]);
        self->active_note[ch] = self->note_stack[ch][self->note_stack_top[ch] - 1][0] - 24;
        lv2_log_note(&self->logger, "Note on %d", self->note_stack_top[ch]);
    }
    else if (self->active_note[ch] != 0xFF && self->note_stack_top[ch] == 0)
    {
        send_note_off(self, ch, self->active_note[ch]);
        self->active_note[ch] = 0xFF;
        send_note_off(self, ch, self->control_note[ch]);
        self->control_note[ch] = 0xFF;
    }
    else if (self->active_note[ch] != 0xFF && self->note_stack_top[ch] > 0)
    {
        uint8_t real_last_note;
        uint8_t next_note = self->note_stack[ch][self->note_stack_top[ch] - 1][0];
        if (self->active_note[ch] > 72)
            real_last_note = self->active_note[ch] - 24;
        else
            real_last_note = self->active_note[ch] + 24;
        if (next_note < real_last_note)
        {
            send_note_off(self, ch, self->active_note[ch]);
            send_note_off(self, ch, self->control_note[ch]);
            send_note_on(self, ch, real_last_note - next_note, 100);
            self->control_note[ch] = real_last_note - next_note;
            send_note_on(self, 
                    ch, 
                    self->note_stack[ch][self->note_stack_top[ch] - 1][0] + 24,
                    self->note_stack[ch][self->note_stack_top[ch] - 1][1]);
            self->active_note[ch] = self->note_stack[ch][self->note_stack_top[ch] - 1][0] + 24;
        }
        else if (next_note > real_last_note)
        {
            send_note_off(self, ch, self->active_note[ch]);
            send_note_off(self, ch, self->control_note[ch]);
            send_note_on(self, ch, next_note - real_last_note, 100);
            self->control_note[ch] = next_note - real_last_note;
            send_note_on(self, 
                    ch, 
                    self->note_stack[ch][self->note_stack_top[ch] - 1][0] - 24,
                    self->note_stack[ch][self->note_stack_top[ch] - 1][1]);
            self->active_note[ch] = self->note_stack[ch][self->note_stack_top[ch] - 1][0] - 24;
        }
    }
}

static void
run(LV2_Handle instance,
    uint32_t   sample_count)
{
    VSLPT*     self = (VSLPT*)instance;
    VSLPTURIs* uris = &self->uris;

    // Initially self->out_port contains a Chunk with size set to capacity

    // Get the capacity
    self->out_capacity = self->out_port->atom.size;

    // Write an empty Sequence header to the output
    lv2_atom_sequence_clear(self->out_port);
    self->out_port->atom.type = self->in_port->atom.type;

    // Read incoming events
    LV2_ATOM_SEQUENCE_FOREACH(self->in_port, ev) {
        if (ev->body.type == uris->midi_Event) {
            const uint8_t* const msg = (const uint8_t*)(ev + 1);
            switch (lv2_midi_message_type(msg)) {
            case LV2_MIDI_MSG_NOTE_ON:
                if (msg[2] == 0) // vel=0 is a Note Off
                    goto noteoff;
                add_to_note_stack(self, msg);
                goto gen_event;
            case LV2_MIDI_MSG_NOTE_OFF:
noteoff:
                remove_from_note_stack(self, msg);
gen_event:
                // Output note events share the same header
                self->evbuf.event = *ev; 
                generate_note_events(self, msg);
                break;
            case LV2_MIDI_MSG_CHANNEL_PRESSURE:
                // Convert channel pressure to modulation CC
                self->evbuf.event.time.frames = ev->time.frames; 
                self->evbuf.event.body.type = ev->body.type;
                self->evbuf.event.body.size = 3;
                self->evbuf.msg[0] = LV2_MIDI_MSG_CONTROLLER | (msg[0] & 0x0F);
                self->evbuf.msg[1] = 0x01;
                self->evbuf.msg[2] = msg[1];
                lv2_atom_sequence_append_event(
                        self->out_port, self->out_capacity, &(self->evbuf.event));
                break;
            default:
                // Forward all other MIDI events directly
                lv2_atom_sequence_append_event(
                    self->out_port, self->out_capacity, ev);
                break;
            }
        }
    }
}

static const void*
extension_data(const char* uri)
{
    return NULL;
}

static const LV2_Descriptor descriptor = {
    VSLPT_URI,
    instantiate,
    connect_port,
    NULL,  // activate,
    run,
    NULL,  // deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    switch (index) {
    case 0:
        return &descriptor;
    default:
        return NULL;
    }
}

// vim: set et ts=4 sw=4:
