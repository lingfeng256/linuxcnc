/** This file, 'siggen.c', is a HAL component that generates square,
    triangle, and sine waves.  I expect that it will mostly be used
    for testing.  It is a realtime component.

    It supports any number of signal generators, as set by the
    insmod parameter 'num_chan'.

    Each generator has a number of pins and parameters, whose
    names begin with 'siggen.x.', where 'x' is the generator number.

    Each generator is controlled by three parameters.  'frequency'
    sets the frequency in Hertz.  'amplitude' sets the peak amplitude,
    and 'offset' sets the DC offset.  For example, if 'amplitude'
    is 1.0 and 'offset' is 0.0, the outputs will swing from -1.0
    to +1.0.  If 'amplitude' is 2.5 and 'offset' is 10.0, then
    the outputs will swing from 7.5 to 12.5.

    There are four output pins: 'square', 'triangle', 'sine' and
    'cosine'.  All four run at the same frequency, amplitude, and
    offset.

    This component exports one function per signal generator,
    called 'siggen.x.update'.  It is a floating point function.

    If the insmod parameter 'fp_period' is specified, the component
    will create a floating point thread called 'siggen.thread'
    with the specified period.

*/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>
*/

/** This program is free software; you can redistribute it and/or
    modify it under the terms of version 2.1 of the GNU General
    Public License as published by the Free Software Foundation.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA

    THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the EMC HAL project.  For more
    information, go to www.linuxcnc.org.
*/

#ifndef RTAPI
#error This is a realtime component only!
#endif

#include "rtapi.h"		/* RTAPI realtime OS API */
#include "rtapi_app.h"		/* RTAPI realtime module decls */
#include "hal.h"		/* HAL public API decls */

#ifdef MODULE
/* module information */
MODULE_AUTHOR("John Kasunich");
MODULE_DESCRIPTION("Signal Generator Component for EMC HAL");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif /* MODULE_LICENSE */
static int num_chan = 1;	/* number of channels - default = 1 */
MODULE_PARM(num_chan, "i");
MODULE_PARM_DESC(chan, "number of channels");
static long fp_period = 0;	/* float pt thread period, default = none */
MODULE_PARM(fp_period, "l");
MODULE_PARM_DESC(fp_period, "floating point thread period (nsecs)");
#endif /* MODULE */

/***********************************************************************
*                STRUCTURES AND GLOBAL VARIABLES                       *
************************************************************************/

/** This structure contains the runtime data for a single siggen.
*/

typedef struct {
    hal_float_t *square;	/* pin: output */
    hal_float_t *triangle;	/* pin: output */
    hal_float_t *sine;		/* pin: output */
    hal_float_t *cosine;	/* pin: output */
    hal_float_t frequency;	/* param: frequency */
    hal_float_t amplitude;	/* param: amplitude */
    hal_float_t offset;		/* param: offset */
    float index;		/* position within output cycle */
} hal_siggen_t;

/* pointer to array of siggen_t structs in shared memory, 1 per gen */
static hal_siggen_t *siggen_array;

/* other globals */
static int comp_id;		/* component ID */

/***********************************************************************
*                  LOCAL FUNCTION DECLARATIONS                         *
************************************************************************/

static int export_siggen(int num, hal_siggen_t * addr);
static void calc_siggen(void *arg, long period);

#ifndef _MATH_H
/* FIXME - these are here because rtapi doesn't yet handle linking
   to the math library */

#define PI 3.1415927

double sin(double x)
{
    int flip;
    double retval, top;

    /* reduce x to 0-2pi */
    while (x < 0.0) {
	x += PI * 2.0;
    }
    while (x > (PI * 2.0)) {
	x -= PI * 2.0;
    }
    /* reduce x to 0-PI */
    flip = 0;
    if (x > PI) {
	x -= PI;
	flip = 1;
    }
    /* reduce x to 0-PI/2 */
    if (x > PI * 0.5) {
	x = PI - x;
    }
    /* if x > pi/4, use cos taylor series */
    if (x > PI * 0.25) {
	x = (PI * 0.5) - x;
	/* cosine taylor series */
	retval = 1;
	top = x * x;
	retval -= top * (1.0 / 2.0);
	top *= x * x;
	retval += top * (1.0 / 24.0);
	top *= x * x;
	retval -= top * (1.0 / 720.0);
	top *= x * x;
	retval += top * (1.0 / 40320.0);
	top *= x * x;
	retval -= top * (1.0 / 3628800.0);
    } else {
	/* sine taylor series */
	retval = x;
	top = x * x * x;
	retval -= top * (1.0 / 6.0);
	top *= x * x;
	retval += top * (1.0 / 120.0);
	top *= x * x;
	retval -= top * (1.0 / 5040.0);
	top *= x * x;
	retval += top * (1.0 / 362880.0);
	top *= x * x;
	retval -= top * (1.0 / 39916800.0);
    }
    if (flip) {
	retval *= -1.0;
    }
    return retval;
}

double cos(double x)
{
    return sin(x + (PI / 2.0));
}

double floor(double x)
{
    /* NOTE: this implementation of floor only works for positive numbers -
       that's good enough here */
    int tmp;

    tmp = x;
    return tmp;
}

#endif

/***********************************************************************
*                       INIT AND EXIT CODE                             *
************************************************************************/

#define MAX_CHAN 16

int rtapi_app_main(void)
{
    int n, retval;

    /* test for number of channels */
    if ((num_chan <= 0) || (num_chan > MAX_CHAN)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIGGEN: ERROR: invalid num_chan: %d\n", num_chan);
	return -1;
    }
    /* have good config info, connect to the HAL */
    comp_id = hal_init("siggen");
    if (comp_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "SIGGEN: ERROR: hal_init() failed\n");
	return -1;
    }
    /* allocate shared memory for siggen data */
    siggen_array = hal_malloc(num_chan * sizeof(hal_siggen_t));
    if (siggen_array == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIGGEN: ERROR: hal_malloc() failed\n");
	hal_exit(comp_id);
	return -1;
    }
    /* export variables and functions for each siggen */
    for (n = 0; n < num_chan; n++) {
	/* export everything for this loop */
	retval = export_siggen(n + 1, &(siggen_array[n]));
	if (retval != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"SIGGEN: ERROR: siggen %d var export failed\n", n + 1);
	    hal_exit(comp_id);
	    return -1;
	}
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"SIGGEN: installed %d signal generators\n", num_chan);
    if (fp_period > 0) {
	/* create a floating point thread */
	retval = hal_create_thread("siggen.thread", fp_period, 1, comp_id);
	if (retval < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"SIGGEN: ERROR: could not create FP thread\n");
	    hal_exit(comp_id);
	    return -1;
	} else {
	    rtapi_print_msg(RTAPI_MSG_INFO,
		"SIGGEN: created %d uS FP thread\n", fp_period / 1000);
	}
    }
    return 0;
}

void rtapi_app_exit(void)
{
    hal_exit(comp_id);
}

/***********************************************************************
*                       REALTIME LOOP CALCULATIONS                     *
************************************************************************/

static void calc_siggen(void *arg, long period)
{
    hal_siggen_t *siggen;
    float tmp1, tmp2;

    /* point to the data for this signal generator */
    siggen = arg;
    /* calculate the time since last execution */
    tmp1 = period * 0.000000001;

    /* index ramps from 0.0 to 0.99999 for each output cycle */
    siggen->index += (siggen->frequency * tmp1);
    /* wrap index if it is >= 1.0 */
    siggen->index -= floor(siggen->index);

    /* generate the square wave output */
    /* tmp1 steps from -1.0 to +1.0 when index passes 0.5 */
    tmp1 = (2.0 * floor(siggen->index * 2.0)) - 1.0;
    /* apply scaling and offset, and write to output */
    *(siggen->square) = (tmp1 * siggen->amplitude) + siggen->offset;

    /* generate the triangle wave output */
    /* tmp2 ramps from -2.0 to +2.0 as index goes from 0 to 1 */
    tmp2 = (siggen->index * 4.0) - 2.0;
    /* flip first half of ramp, now goes from +1 to -1 to +1 */
    tmp2 = (tmp2 * tmp1) - 1.0;
    /* apply scaling and offset, and write to output */
    *(siggen->triangle) = (tmp2 * siggen->amplitude) + siggen->offset;

    /* generate the sine wave output */
    /* tmp1 is angle in radians */
    tmp1 = siggen->index * (2.0 * 3.1415927);
    /* get sine, apply scaling and offset, and write to output */
    *(siggen->sine) = (sin(tmp1) * siggen->amplitude) + siggen->offset;

    /* generate the cosine wave output */
    /* get cosine, apply scaling and offset, and write to output */
    *(siggen->cosine) = (cos(tmp1) * siggen->amplitude) + siggen->offset;
    /* done */
}

/***********************************************************************
*                   LOCAL FUNCTION DEFINITIONS                         *
************************************************************************/

static int export_siggen(int num, hal_siggen_t * addr)
{
    int retval;
    char buf[HAL_NAME_LEN + 2];

    /* export pins */
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.square", num);
    retval = hal_pin_float_new(buf, HAL_WR, &(addr->square), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.triangle", num);
    retval = hal_pin_float_new(buf, HAL_WR, &(addr->triangle), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.sine", num);
    retval = hal_pin_float_new(buf, HAL_WR, &(addr->sine), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.cosine", num);
    retval = hal_pin_float_new(buf, HAL_WR, &(addr->cosine), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export parameters */
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.frequency", num);
    retval = hal_param_float_new(buf, &(addr->frequency), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.amplitude", num);
    retval = hal_param_float_new(buf, &(addr->amplitude), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.offset", num);
    retval = hal_param_float_new(buf, &(addr->offset), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* init all structure members */
    *(addr->square) = 0.0;
    *(addr->triangle) = 0.0;
    *(addr->sine) = 0.0;
    *(addr->cosine) = 0.0;
    addr->frequency = 1.0;
    addr->amplitude = 1.0;
    addr->offset = 0.0;
    /* export function for this loop */
    rtapi_snprintf(buf, HAL_NAME_LEN, "siggen.%d.update", num);
    retval =
	hal_export_funct(buf, calc_siggen, &(siggen_array[num - 1]), 1, 0,
	comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIGGEN: ERROR: update funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    return 0;
}
