// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
/*
    c352.c - Namco C352 custom PCM chip emulation
    v2.0
    By R. Belmont
    Rewritten and improved by superctr
    Additional code by cync and the hoot development team

    Thanks to Cap of VivaNonno for info and The_Author for preliminary reverse-engineering

    Chip specs:
    32 voices
    Supports 8-bit linear and 8-bit muLaw samples
    Output: digital, 16 bit, 4 channels
    Output sample rate is the input clock / (288 * 2).
 */

//#include "emu.h"
//#include "streams.h"
#include <math.h>
#include <stdlib.h>
#include <string.h> // for memset
#include <stddef.h> // for NULL
#include "mamedef.h"
#include "c352.h"

#define C352_VOICES 32
enum {
    C352_FLG_BUSY       = 0x8000,   // channel is busy
    C352_FLG_KEYON      = 0x4000,   // Keyon
    C352_FLG_KEYOFF     = 0x2000,   // Keyoff
    C352_FLG_LOOPTRG    = 0x1000,   // Loop Trigger
    C352_FLG_LOOPHIST   = 0x0800,   // Loop History
    C352_FLG_FM         = 0x0400,   // Frequency Modulation
    C352_FLG_PHASERL    = 0x0200,   // Rear Left invert phase 180 degrees
    C352_FLG_PHASEFL    = 0x0100,   // Front Left invert phase 180 degrees
    C352_FLG_PHASEFR    = 0x0080,   // invert phase 180 degrees (e.g. flip sign of sample)
    C352_FLG_LDIR       = 0x0040,   // loop direction
    C352_FLG_LINK       = 0x0020,   // "long-format" sample (can't loop, not sure what else it means)
    C352_FLG_NOISE      = 0x0010,   // play noise instead of sample
    C352_FLG_MULAW      = 0x0008,   // sample is mulaw instead of linear 8-bit PCM
    C352_FLG_FILTER     = 0x0004,   // don't apply filter
    C352_FLG_REVLOOP    = 0x0003,   // loop backwards
    C352_FLG_LOOP       = 0x0002,   // loop forward
    C352_FLG_REVERSE    = 0x0001    // play sample backwards
};

typedef struct {

    UINT32 pos;
    UINT32 counter;

    INT16 sample;
    INT16 last_sample;

    UINT16 vol_f;
    UINT16 vol_r;
    UINT8 curr_vol[4];
    UINT16 freq;
    UINT16 flags;

    UINT16 wave_bank;
    UINT16 wave_start;
    UINT16 wave_end;
    UINT16 wave_loop;

    UINT8 mute;

} C352_Voice;

typedef struct {

    UINT32 sample_rate_base;
    UINT16 divider;

    C352_Voice v[C352_VOICES];

    UINT16 random;
    UINT16 control; // control flags, purpose unknown.

    UINT8* wave;
    UINT32 wavesize;
    UINT32 wave_mask;

    UINT8 muteRear;     // flag from VGM header
    //UINT8 optMuteRear;  // option

} C352;


#define MAX_CHIPS   0x02
static C352 C352Data[MAX_CHIPS];

static UINT8 MuteAllRear = 0x00;


static void C352_fetch_sample(C352 *c, C352_Voice *v)
{
    v->last_sample = v->sample;
    
    if(v->flags & C352_FLG_NOISE)
    {
        c->random = (c->random>>1) ^ ((-(c->random&1)) & 0xfff6);
        v->sample = c->random;
    }
    else
    {
        INT8 s, s2;
        UINT16 pos;

        s = (INT8)c->wave[v->pos & c->wave_mask];

        v->sample = s<<8;
        if(v->flags & C352_FLG_MULAW)
        {
            s2 = (s&0x7f)>>4;

            v->sample = ((s2*s2)<<4) - (~(s2<<1)) * (s&0x0f);
            v->sample = (s&0x80) ? (~v->sample)<<5 : v->sample<<5;
        }
        
        pos = v->pos&0xffff;
        
        if((v->flags & C352_FLG_LOOP) && v->flags & C352_FLG_REVERSE)
        {
            // backwards>forwards
            if((v->flags & C352_FLG_LDIR) && pos == v->wave_loop)
                v->flags &= ~C352_FLG_LDIR;
            // forwards>backwards
            else if(!(v->flags & C352_FLG_LDIR) && pos == v->wave_end)
                v->flags |= C352_FLG_LDIR;
            
            v->pos += (v->flags&C352_FLG_LDIR) ? -1 : 1;
        }
        else if(pos == v->wave_end)
        {
            if((v->flags & C352_FLG_LINK) && (v->flags & C352_FLG_LOOP))
            {
                v->pos = (v->wave_start<<16) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            }
            else if(v->flags & C352_FLG_LOOP)
            {
                v->pos = (v->pos&0xff0000) | v->wave_loop;
                v->flags |= C352_FLG_LOOPHIST;
            }
            else
            {
                v->flags |= C352_FLG_KEYOFF;
                v->flags &= ~C352_FLG_BUSY;
                v->sample=0;
            }
        }
        else
        {
            v->pos += (v->flags&C352_FLG_REVERSE) ? -1 : 1;
        }
    }
}

static void c352_ramp_volume(C352_Voice* v,int ch,UINT8 val)
{
    INT16 vol_delta = v->curr_vol[ch] - val;
    if(vol_delta != 0)
        v->curr_vol[ch] += (vol_delta>0) ? -1 : 1;
}

void c352_update(UINT8 ChipID, stream_sample_t **outputs, int samples)
{
    C352 *c = &C352Data[ChipID];
    int i, j;
    INT16 s;
    INT32 next_counter;
    C352_Voice* v;

    stream_sample_t out[4];

    memset(outputs[0], 0, samples * sizeof(stream_sample_t));
    memset(outputs[1], 0, samples * sizeof(stream_sample_t));

    for(i=0;i<samples;i++)
    {
        out[0]=out[1]=out[2]=out[3]=0;

        for(j=0;j<C352_VOICES;j++)
        {

            v = &c->v[j];
            s = 0;

            if(v->flags & C352_FLG_BUSY)
            {
                next_counter = v->counter+v->freq;

                if(next_counter & 0x10000)
                {
                    C352_fetch_sample(c,v);
                }

                if((next_counter^v->counter) & 0x18000)
                {
                    c352_ramp_volume(v,0,v->vol_f>>8);
                    c352_ramp_volume(v,1,v->vol_f&0xff);
                    c352_ramp_volume(v,2,v->vol_r>>8);
                    c352_ramp_volume(v,3,v->vol_r&0xff);
                }

                v->counter = next_counter&0xffff;

                s = v->sample;

                // Interpolate samples
                if((v->flags & C352_FLG_FILTER) == 0)
                    s = v->last_sample + (v->counter*(v->sample-v->last_sample)>>16);
            }

            if(!c->v[j].mute)
            {
                // Left
                out[0] += (((v->flags & C352_FLG_PHASEFL) ? -s : s) * v->curr_vol[0])>>8;
                out[2] += (((v->flags & C352_FLG_PHASEFR) ? -s : s) * v->curr_vol[2])>>8;

                // Right
                out[1] += (((v->flags & C352_FLG_PHASERL) ? -s : s) * v->curr_vol[1])>>8;
                out[3] += (((v->flags & C352_FLG_PHASERL) ? -s : s) * v->curr_vol[3])>>8;
            }
        }

        outputs[0][i] += out[0];
        outputs[1][i] += out[1];
        if (!c->muteRear && !MuteAllRear)
        {
            outputs[0][i] += out[2];
            outputs[1][i] += out[3];
        }
    }
}

int device_start_c352(UINT8 ChipID, int clock, int clkdiv)
{
    C352 *c;

    if (ChipID >= MAX_CHIPS)
        return 0;

    c = &C352Data[ChipID];

    c->wave = NULL;
    c->wavesize = 0x00;

    c->divider = clkdiv ? clkdiv : 288;
    c->sample_rate_base = (clock&0x7FFFFFFF) / c->divider;
    c->muteRear = (clock&0x80000000)>>31;

    memset(c->v,0,sizeof(C352_Voice)*C352_VOICES);

    c352_set_mute_mask(ChipID, 0x00000000);

    return c->sample_rate_base;
}

void device_stop_c352(UINT8 ChipID)
{
    C352 *c = &C352Data[ChipID];
    
    free(c->wave);
    c->wave = NULL;
    
    return;
}

void device_reset_c352(UINT8 ChipID)
{
    C352 *c = &C352Data[ChipID];
    UINT32 muteMask;
    
    muteMask = c352_get_mute_mask(ChipID);
    
    // clear all channels states
    memset(c->v,0,sizeof(C352_Voice)*C352_VOICES);
    
    // init noise generator
    c->random = 0x1234;
    c->control = 0;
    
    c352_set_mute_mask(ChipID, muteMask);
    
    return;
}

static UINT16 C352RegMap[8] = {
    offsetof(C352_Voice,vol_f) / sizeof(UINT16),
    offsetof(C352_Voice,vol_r) / sizeof(UINT16),
    offsetof(C352_Voice,freq) / sizeof(UINT16),
    offsetof(C352_Voice,flags) / sizeof(UINT16),
    offsetof(C352_Voice,wave_bank) / sizeof(UINT16),
    offsetof(C352_Voice,wave_start) / sizeof(UINT16),
    offsetof(C352_Voice,wave_end) / sizeof(UINT16),
    offsetof(C352_Voice,wave_loop) / sizeof(UINT16),
};

UINT16 c352_r(UINT8 ChipID, offs_t address)
{
    C352 *c = &C352Data[ChipID];

    if(address < 0x100)
        return *((UINT16*)&c->v[address/8]+C352RegMap[address%8]);
    else if(address == 0x200)
        return c->control;
    else
        return 0;
}

void c352_w(UINT8 ChipID, offs_t address, UINT16 val)
{
    C352 *c = &C352Data[ChipID];
    
    int i;
    
    if(address < 0x100) // Channel registers, see map above.
    {
        *((UINT16*)&c->v[address/8]+C352RegMap[address%8]) = val;
    }
    else if(address == 0x200)
    {
        c->control = val;
        //logerror("C352 control register write: %04x\n",val);
    }
    else if(address == 0x202) // execute keyons/keyoffs
    {
        for(i=0;i<C352_VOICES;i++)
        {
            if((c->v[i].flags & C352_FLG_KEYON))
            {
                c->v[i].pos = (c->v[i].wave_bank<<16) | c->v[i].wave_start;

                c->v[i].sample = 0;
                c->v[i].last_sample = 0;
                c->v[i].counter = 0xffff;

                c->v[i].flags |= C352_FLG_BUSY;
                c->v[i].flags &= ~(C352_FLG_KEYON|C352_FLG_LOOPHIST);

                c->v[i].curr_vol[0] = c->v[i].curr_vol[1] = 0;
                c->v[i].curr_vol[2] = c->v[i].curr_vol[3] = 0;
            }
            else if(c->v[i].flags & C352_FLG_KEYOFF)
            {
                c->v[i].flags &= ~(C352_FLG_BUSY|C352_FLG_KEYOFF);
                c->v[i].counter = 0xffff;
            }
        }
    }
}


void c352_write_rom(UINT8 ChipID, offs_t ROMSize, offs_t DataStart, offs_t DataLength,
                    const UINT8* ROMData)
{
    C352 *c = &C352Data[ChipID];
    
    if (c->wavesize != ROMSize)
    {
        c->wave = (UINT8*)realloc(c->wave, ROMSize);
        c->wavesize = ROMSize;
		for (c->wave_mask = 1; c->wave_mask < c->wavesize; c->wave_mask <<= 1)
			;
		c->wave_mask --;
        memset(c->wave, 0xFF, ROMSize);
    }
    if (DataStart > ROMSize)
        return;
    if (DataStart + DataLength > ROMSize)
        DataLength = ROMSize - DataStart;
    
    memcpy(c->wave + DataStart, ROMData, DataLength);
    
    return;
}

void c352_set_mute_mask(UINT8 ChipID, UINT32 MuteMask)
{
    C352 *c = &C352Data[ChipID];
    UINT8 CurChn;
    
    for (CurChn = 0; CurChn < C352_VOICES; CurChn ++)
        c->v[CurChn].mute = (MuteMask >> CurChn) & 0x01;
    
    return;
}

UINT32 c352_get_mute_mask(UINT8 ChipID)
{
    C352 *c = &C352Data[ChipID];
    UINT32 muteMask;
    UINT8 CurChn;
    
    muteMask = 0x00000000;
    for (CurChn = 0; CurChn < C352_VOICES; CurChn ++)
        muteMask |= (c->v[CurChn].mute << CurChn);
    
    return muteMask;
}

void c352_set_options(UINT8 Flags)
{
    MuteAllRear = (Flags & 0x01) >> 0;
    
    return;
}
