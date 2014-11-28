/*
 * Copyright (c) 2007 - 2014 Joseph Gaeddert
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// Generic channel
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// portable structured channel object
struct CHANNEL(_s) {
    // additive white Gauss noise
    int   enabled_awgn;     // AWGN enabled?
    T     gamma;            // channel gain
    T     nstd;             // noise standard deviation
    float noise_floor_dB;
    float SNRdB;

    // carrier offset
    int   enabled_carrier;  // carrier offset enabled?
    float dphi;             // channel gain
    float phi;              // noise standard deviation
    NCO() nco;              // oscillator

    // multi-path channel
    int enabled_multipath;          // enable multi-path channel filter?
    FIRFILT()    channel_filter;    // multi-path channel filter object
    TC *         h;                 // multi-path channel filter coefficients
    unsigned int h_len;             // multi-path channel filter length
};

// create structured channel object
//  _h      :   coefficients array [size: 1 x _n]
//  _n      :   channel length
CHANNEL() CHANNEL(_create)(TC *         _h,
                           unsigned int _n)
{
    CHANNEL() q = (CHANNEL()) malloc(sizeof(struct CHANNEL(_s)));

    // initialize all options as off
    q->enabled_awgn      = 0;
    q->enabled_carrier   = 0;
    q->enabled_multipath = 0;

    // create internal objects
    q->nco = NCO(_create)(LIQUID_VCO);
    q->channel_filter = NULL;
    q->h_len          = 1;
    q->h              = (TC*) malloc(q->h_len*sizeof(TC));
    q->h[0]           = 1.0f;

    // return object
    return q;
}

// destroy channel object
void CHANNEL(_destroy)(CHANNEL() _q)
{
    // destroy internal objects
    NCO(_destroy)(_q->nco);
    free(_q->h);
    if (_q->channel_filter != NULL)
        FIRFILT(_destroy)(_q->channel_filter);

    // free main object memory
    free(_q);
}

// print channel object
void CHANNEL(_print)(CHANNEL() _q)
{
    printf("channel\n");
    if (_q->enabled_awgn) {
        printf("  AWGN:\n");
    }
}

// apply additive white Gausss noise impairment
//  _q              : channel object
//  _noise_floor_dB : noise floor power spectral density
//  _SNR_dB         : signal-to-noise ratio [dB]
void CHANNEL(_add_awgn)(CHANNEL() _q,
                        float     _noise_floor_dB,
                        float     _SNRdB)
{
    // enable module
    _q->enabled_awgn = 1;

    //
    _q->noise_floor_dB = _noise_floor_dB;
    _q->SNRdB          = _SNRdB;

    // set values appropriately
    _q->nstd  = powf(10.0f, _noise_floor_dB/20.0f);
    _q->gamma = powf(10.0f, (_q->SNRdB+_q->noise_floor_dB)/20.0f);
}

// apply carrier offset impairment
//  _q          : channel object
//  _frequency  : carrier frequency offse [radians/sample
//  _phase      : carrier phase offset    [radians]
void CHANNEL(_add_carrier_offset)(CHANNEL() _q,
                                  float     _frequency,
                                  float     _phase)
{
    // enable module
    _q->enabled_carrier = 1;

    // carrier frequency/phase offsets
    _q->dphi = _frequency;
    _q->phi  = _phase;

    // set values appropriately
    NCO(_set_frequency)(_q->nco, _q->dphi);
    NCO(_set_phase)(    _q->nco, _q->phi);
}

// apply mulit-path channel impairment
//  _q          : channel object
//  _h          : channel coefficients (NULL for random)
//  _h_len      : number of channel coefficients
void CHANNEL(_add_multipath)(CHANNEL()    _q,
                             TC *         _h,
                             unsigned int _h_len)
{
    if (_h_len == 0) {
        fprintf(stderr,"warning: channel_%s_add_multipath(), filter length is zero (ignoring)\n", EXTENSION_FULL);
        return;
    } else if (_h_len > 1000) {
        fprintf(stderr,"warning: channel_%s_add_multipath(), filter length exceeds maximum\n", EXTENSION_FULL);
        exit(1);
    }

    // enable module
    _q->enabled_multipath = 1;

    // set values appropriately
    // TODO: test for types other than float complex
    if (_q->h_len != _h_len)
        _q->h = (TC*) realloc(_q->h, _h_len*sizeof(TC));

    // update length
    _q->h_len = _h_len;
    
    if (_h == NULL) {
        // generate random coefficients
        // TODO: support types other than float
        _q->h[0] = 1.0f;
        unsigned int i;
        for (i=1; i<_q->h_len; i++)
            _q->h[i] = 0.05f * ( randnf() * _Complex_I*randnf() );
    } else {
        // copy coefficients internally
        memmove(_q->h, _h, _q->h_len*sizeof(TC));
    }

    // destroy multi-path channel object if it already exists
    // TODO: recreate object
    if (_q->channel_filter != NULL)
        FIRFILT(_destroy)(_q->channel_filter);

    // create new filter
    _q->channel_filter = FIRFILT(_create)(_q->h, _q->h_len);
}

// apply channel impairments on input array
//  _q      : channel object
//  _x      : input array [size: _nx x 1]
//  _nx     : input array length
//  _y      : output array
//  _ny     : output array length
void CHANNEL(_execute)(CHANNEL()      _q,
                       TI *           _x,
                       unsigned int   _nx,
                       TO *           _y,
                       unsigned int * _ny)
{
    unsigned int i;

    for (i=0; i<_nx; i++) {

        // apply filter
        if (_q->enabled_multipath) {
            FIRFILT(_push)(   _q->channel_filter,  _x[i]);
            FIRFILT(_execute)(_q->channel_filter, &_y[i]);
        } else
            _y[i] = _x[i];

        // apply carrier if enabled
        if (_q->enabled_carrier)
            NCO(_mix_up)(_q->nco, _y[i], &_y[i]);

        // apply AWGN if enabled
        if (_q->enabled_awgn) {
            _y[i] *= _q->gamma;
            _y[i] += _q->nstd * ( randnf() + _Complex_I*randnf() ) * M_SQRT1_2;
        }
    }

    *_ny = _nx;
}
