/*
 * canonical_machine.c - rs274/ngc canonical machine.
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2013 Alden S Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * This code is a loose implementation of Kramer, Proctor and Messina's
 * canonical machining functions as described in the NIST RS274/NGC v3
 *
 * The canonical machine is the layer between the Gcode parser and the
 * motion control code for a specific robot. It keeps state and executes
 * commands - passing the stateless commands to the motion control layer. 
 */
/* --- System state contexts and canonical machine command execution ---
 *
 *	Useful reference for doing C callbacks http://www.newty.de/fpt/fpt.html
 *
 *	There are 3 temporal contexts for system state:
 *	  - The Gcode model in the canonical machine (the "model" context, held in gm)
 *	  - The machine model used by the planner for planning ("planner" context, held in mm)
 *	  - The "runtime" context used for move execution (held in mr)
 *
 *	Functions in the canonical machine may apply to one or more contexts. Commands that
 *	apply to the Gcode model and/or planner are executed immediately (i.e. when called)
 *
 *	Commands that affect the runtime need to be synchronized with movement and are 
 *	therefore queued into the planner queue and execute from the queue - Synchronous commands
 *
 *	There are a few commands that affect all 3 contexts and are therefore executed
 *	to the gm amd mm structs and are also queued to execute their runtine part.  
 *
 *	The applicable context is in the function name as "model", "planner" or "runtime"
 *
 *	Synchronous commands work like this:
 *
 *	  - Call the cm_xxx_xxx() function which will do any input validation and 
 *		return an error if it detects one.
 *
 *	  - The cm_ function calls mp_queue_command(). Arguments are a callback to
 *		the _exec_...() function, which is the runtime execution routine, and
 *		any arguments that rae needed by the runtime. See typedef for *exec in
 *		planner.h for details
 *
 *	  - mp_queue_command() stores the callback and the args in a planner buffer.
 *
 *	  - When planner execution reaches the buffer is tectures the callback w/ the 
 *		args.  Take careful note that the callback executes under an interrupt, 
 *		so beware of variables that may need to be Volatile.
 *
 *	Notes:
 *	  - The synchronous command execution mechanism uses 2 vectors in the bf buffer
 *		to store and return values for the callback. It's obvious, but impractical
 *		to pass the entire bf buffer to the callback as some of these commands are 
 *		actually executed locally and have no buffer.
 *
 *	  - Commands that are used to set the gm model state for interpretation of the
 *		current Gcode block. For example, cm_set_feed_rate(). This sets the model
 *		so the move time is properly calculated for the current (and subsequent) 
 *		blocks, so it's effected immediately. Note that the "feed rate" (actually 
 *		move time) is carried forward into the planner - planned moves are not 
 *		affected by upstream changes to the gm model. Many other vars also fall into
 *		this category.
 *
 */

#include "tinyg.h"
#include "util.h"
#include "config.h"
#include "canonical_machine.h"
#include "plan_arc.h"
#include "planner.h"
#include "stepper.h"
#include "spindle.h"
#include "report.h"
#include "gpio.h"
#include "system.h"
#include "xio/xio.h"			// for serial queue flush

/***********************************************************************************
 **** STRUCTURE ALLOCATIONS ********************************************************
 ***********************************************************************************/

cmSingleton_t cm;		// canonical machine controller singleton
GCodeState_t  gm;		// core gcode model state
GCodeStateX_t gmx;		// extended gcode model state
GCodeInput_t  gn;		// gcode input values - transient
GCodeInput_t  gf;		// gcode input flags - transient

/***********************************************************************************
 **** GENERIC STATIC FUNCTIONS AND VARIABLES ***************************************
 ***********************************************************************************/

static float _get_move_times(float *min_time);

// command execution callbacks from planner queue
static void _exec_offset(float *value, float *flag);
static void _exec_change_tool(float *value, float *flag);
static void _exec_select_tool(float *value, float *flag);
static void _exec_mist_coolant_control(float *value, float *flag);
static void _exec_flood_coolant_control(float *value, float *flag);
static void _exec_absolute_origin(float *value, float *flag);
static void _exec_program_finalize(float *value, float *flag);

#define _to_millimeters(a) ((gm.units_mode == INCHES) ? (a * MM_PER_INCH) : a)


/***********************************************************************************
 **** CODE *************************************************************************
 ***********************************************************************************/

/*************************************************************************
 *
 * HELPERS AND UTILITY FUNCTIONS
 *
 *	These functions are not part of the NIST defined functions
 *
 ************************************************************************/

/* Runtime State functions
 *
 * cm_get_combined_state() - combines raw states into something a user might want to see
 * cm_get_machine_state()
 * cm_get_motion_state() 
 * cm_get_cycle_state() 
 * cm_get_hold_state() 
 * cm_get_homing_state()
 * cm_get_runtime_motion_mode()
 * cm_get_runtime_busy()
 */
uint8_t cm_get_combined_state() 
{
	if (cm.machine_state == MACHINE_CYCLE) {
		if (cm.motion_state == MOTION_RUN) cm.combined_state = COMBINED_RUN;
		if (cm.motion_state == MOTION_HOLD) cm.combined_state = COMBINED_HOLD;
		if (cm.cycle_state == CYCLE_HOMING) cm.combined_state = COMBINED_HOMING;
		if (cm.cycle_state == CYCLE_PROBE) cm.combined_state = COMBINED_PROBE;
		if (cm.cycle_state == CYCLE_JOG) cm.combined_state = COMBINED_JOG;
	} else {
		cm.combined_state = cm.machine_state;
	}
	return cm.combined_state;
}
uint8_t cm_get_machine_state() { return cm.machine_state;}
uint8_t cm_get_cycle_state() { return cm.cycle_state;}
uint8_t cm_get_motion_state() { return cm.motion_state;}
uint8_t cm_get_hold_state() { return cm.hold_state;}
uint8_t cm_get_homing_state() { return cm.homing_state;}

uint8_t cm_get_runtime_motion_mode() { return mp_get_runtime_motion_mode();}
uint8_t cm_get_runtime_busy() { return (mp_get_runtime_busy());}


/* 
 * Model state Getters and Setters
 */
// set parameters in gm struct
void cm_set_motion_mode(uint8_t motion_mode) {gm.motion_mode = motion_mode;} 
void cm_set_absolute_override(uint8_t absolute_override) { gm.absolute_override = absolute_override;}
void cm_set_spindle_mode(uint8_t spindle_mode) { gm.spindle_mode = spindle_mode;} 
void cm_set_spindle_speed_parameter(float speed) { gm.spindle_speed = speed;}
void cm_set_tool_number(uint8_t tool) { gm.tool = tool;}

// get parameter from gm struct
uint8_t cm_get_model_motion_mode() { return gm.motion_mode;}
uint8_t cm_get_model_coord_system() { return gm.coord_system;}
uint8_t cm_get_model_units_mode() { return gm.units_mode;}
uint8_t cm_get_model_select_plane() { return gm.select_plane;}
uint8_t cm_get_model_path_control() { return gm.path_control;}
uint8_t cm_get_model_distance_mode() { return gm.distance_mode;}
uint8_t cm_get_model_inverse_feed_rate_mode() { return gm.inverse_feed_rate_mode;}
uint8_t cm_get_model_spindle_mode() { return gm.spindle_mode;} 
uint32_t cm_get_model_linenum() { return gm.linenum;}
uint8_t	cm_get_block_delete_switch() { return gmx.block_delete_switch;}

/* Position and Offset getters - operates on model and runtime contexts
 *
 * cm_get_model_coord_offset() - return the currently active coordinate offset for an axis
 * cm_get_model_coord_offset_vector() - return currently active coordinate offsets as a vector
 * cm_get_model_work_position() - return position from the gm struct into gn struct form (external form)
 * cm_get_model_work_position_vector() - return model position vector in externalized form
 * cm_get_model_canonical_target() - return model target in internal canonical form
 * cm_get_model_canonical_position_vector() - return model position vector in internal canonical form
 *
 * cm_get_runtime_machine_position() - return current machine position in external form 
 * cm_get_runtime work_position() - return current work coordinate position in external form 
 * cm_get_runtime work_offset() - return current work offset
 * cm_get_runtime work_scaling() - return current work scaling factor
 */

float cm_get_model_coord_offset(uint8_t axis)
{
	if (gm.absolute_override == true) {
		return (0);						// no work offset if in abs override mode
	}
	if (gmx.origin_offset_enable == 1) {
		return (cfg.offset[gm.coord_system][axis] + gmx.origin_offset[axis]); // includes G5x and G92 compoenents
	} else {
		return (cfg.offset[gm.coord_system][axis]);		// just the g5x coordinate system components
	}
}

float *cm_get_model_coord_offset_vector(float vector[])
{
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		vector[axis] = cm_get_model_coord_offset(axis);
	}
//	for (uint8_t i=0; i<AXES; i++) {
//		vector[i] = cm_get_model_coord_offset(i);
//	}
	return (vector);
}

float cm_get_model_work_position(uint8_t axis) 
{
	if (gm.units_mode == INCHES) {
		return ((gmx.position[axis] - cm_get_model_coord_offset(axis)) / MM_PER_INCH);
	} else {
		return (gmx.position[axis] - cm_get_model_coord_offset(axis));
	}
}
/*
float *cm_get_model_work_position_vector(float position[]) 
{
	for (uint8_t axis = AXIS_X; axis < AXIS_MAX; axis++) {
		position[axis] = cm_get_model_work_position(axis);
	}
	return (position);
}
*/
float cm_get_model_canonical_target(uint8_t axis) 
{
	return (gm.target[axis]);
}

float *cm_get_model_canonical_position_vector(float position[])
{
	copy_axis_vector(position, gmx.position);	
	return (position);
}

/* NOTE: machine position is always returned in mm mode. No units conversion is performed */
float cm_get_runtime_machine_position(uint8_t axis) 
{
	return (mp_get_runtime_machine_position(axis));
}
//	deprecated behavior - left in for reference
//	if (gm.units_mode == INCHES) {
//		return (mp_get_runtime_machine_position(axis) / MM_PER_INCH);
//	} else {
//		return (mp_get_runtime_machine_position(axis));
//	}

float cm_get_runtime_work_position(uint8_t axis) 
{
	if (gm.units_mode == INCHES) {
		return (mp_get_runtime_work_position(axis) / MM_PER_INCH);
	} else {
		return (mp_get_runtime_work_position(axis));
	}
}

float cm_get_runtime_work_offset(uint8_t axis) 
{
	return (mp_get_runtime_work_offset(axis));
}

/*
 * Model initializers - these inhale gn values into the gm struct
 *
 *	Input coordinates are in native block formats (gn form);
 *	i.e. they are not unit adjusted or otherwise pre-processed.
 *	The setters take care of coordinate system, units, and 
 *	distance mode conversions and normalizations.
 *
 * cm_set_model_arc_offset()  - set all IJK offsets
 * cm_set_model_radius()	  - set radius value
 * cm_set_model_linenum() 	  - set line number in the model
 */

void cm_set_model_arc_offset(float i, float j, float k)
{ 
	gmx.arc_offset[0] = _to_millimeters(i);
	gmx.arc_offset[1] = _to_millimeters(j);
	gmx.arc_offset[2] = _to_millimeters(k);
}

void cm_set_model_arc_radius(float r) 
{ 
	gmx.arc_radius = _to_millimeters(r);
}

void cm_set_model_linenum(uint32_t linenum)
{
	gm.linenum = linenum;	// you must first set the model line number,
	cmd_add_object("n");		// then add the line number to the cmd list
//++++ The above is not the same as the G2 version	
}

/* 
 * cm_set_model_target() - set target vector in GM model
 *
 * This is a core routine. It handles:
 *	- conversion of linear units to internal canonical form (mm)
 *	- conversion of relative mode to absolute (internal canonical form)
 *	- translation of work coordinates to machine coordinates (internal canonical form)
 *	- computation and application of axis modes as so:
 *
 *	DISABLED  - Incoming value is ignored. Target value is not changed
 *	ENABLED	  - Convert axis values to canonical format and store as target
 *	INHIBITED - Same processing as ENABLED, but axis will not actually be run
 * 	RADIUS	  - ABC axis value is provided in Gcode block in linear units
 *			  - Target is set to degrees based on axis' Radius value
 *			  - Radius mode is only processed for ABC axes. Application to XYZ is ignored.
 *
 *	Target coordinates are provided in target[]
 *	Axes that need processing are signaled in flag[]
 */
static float _calc_ABC(uint8_t i, float target[], float flag[]);

void cm_set_model_target(float target[], float flag[])
{ 
	uint8_t i;
	float tmp = 0;

	// process XYZABC for lower modes
	for (i=AXIS_X; i<=AXIS_Z; i++) {
		if ((fp_FALSE(flag[i])) || (cfg.a[i].axis_mode == AXIS_DISABLED)) {
			continue;
		} else if ((cfg.a[i].axis_mode == AXIS_STANDARD) || (cfg.a[i].axis_mode == AXIS_INHIBITED)) {
			if (gm.distance_mode == ABSOLUTE_MODE) {
				gm.target[i] = cm_get_model_coord_offset(i) + _to_millimeters(target[i]);
			} else {
				gm.target[i] += _to_millimeters(target[i]);
			}
		}
	}
	// FYI: The ABC loop below relies on the XYZ loop having been run first
	for (i=AXIS_A; i<=AXIS_C; i++) {
		// skip axis if not flagged for update or its disabled
		if ((fp_FALSE(flag[i])) || (cfg.a[i].axis_mode == AXIS_DISABLED)) {
			continue;
		} else tmp = _calc_ABC(i, target, flag);		
		
		if (gm.distance_mode == ABSOLUTE_MODE) {
			gm.target[i] = tmp + cm_get_model_coord_offset(i); // sacidu93's fix to Issue #22
		} else {
			gm.target[i] += tmp;
		}
	}
}

// ESTEE: fix to workaround a gcc compiler bug wherein it runs out of spill registers
// we moved this block into its own function so that we get a fresh stack push
// ALDEN: This shows up in avr-gcc 4.7.0 and avr-libc 1.8.0
static float _calc_ABC(uint8_t i, float target[], float flag[])
{
	float tmp = 0;
	
	if ((cfg.a[i].axis_mode == AXIS_STANDARD) || (cfg.a[i].axis_mode == AXIS_INHIBITED)) {
		tmp = target[i];	// no mm conversion - it's in degrees

	} else if ((cfg.a[i].axis_mode == AXIS_RADIUS) && (fp_TRUE(flag[i]))) {
		tmp = _to_millimeters(target[i]) * 360 / (2 * M_PI * cfg.a[i].radius);

/* DEPRECATED CODE FOR SLAVE MODES - LEFT IN FOR EXAMPLE
	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_X) && (fp_TRUE(flag[X]))) {
		tmp = (target[X] - gmx.position[X]) * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_Y) && (fp_TRUE(flag[Y]))) {
		tmp = (target[Y] - gmx.position[Y]) * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_Z) && (fp_TRUE(flag[Z]))) {
		tmp = (target[Z] - gmx.position[Z]) * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_XY) && ((fp_TRUE(flag[X])) || (fp_TRUE(flag[Y])))) {
		float length = sqrt(square(target[X] - gmx.position[X]) + square(target[Y] - gmx.position[Y]));
		tmp = length * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_XZ) && ((fp_TRUE(flag[X])) || (fp_TRUE(flag[Z])))) {
		float length = sqrt(square(target[X] - gmx.position[X]) + square(target[Z] - gmx.position[Z]));
		tmp = length * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_YZ) && ((fp_TRUE(flag[Y])) || (fp_TRUE(flag[Z])))) {
		float length = sqrt(square(target[Y] - gmx.position[Y]) + square(target[Z] - gmx.position[Z]));
		tmp = length * 360 / (2 * M_PI * cfg.a[i].radius);

	} else if ((cfg.a[i].axis_mode == AXIS_SLAVE_XYZ) && ((fp_TRUE(flag[X])) || (fp_TRUE(flag[Y])) || (fp_TRUE(flag[Z])))) {
		float length = sqrt(square(target[X] - gmx.position[X]) + square(target[Y] - gmx.position[Y]) + square(target[Z] - gmx.position[Z]));
		tmp = length * 360 / (2 * M_PI * cfg.a[i].radius);
*/
	}
	return tmp;
}

/* 
 * cm_set_model_endpoint_position() - uses internal canonical coordinates only
 *
 * 	This routine sets the endpoint position in the gccode model if the move was
 *	successfully completed (no errors). Leaving the endpoint position alone for 
 *	errors allows too-short-lines to accumulate into longer lines (line interpolation).
 *
 * 	Note: As far as the canonical machine is concerned the final position is achieved 
 *	as soon at the move is executed and the position is now the target. In reality 
 *	the planner(s) and steppers will still be processing the action and the real tool 
 *	position is still close to the starting point. 
 */

void cm_set_model_endpoint_position(stat_t status) 
{
	if (status == STAT_OK) copy_axis_vector(gmx.position, gm.target);
}

/* 
 * _get_move_times() - get minimum and optimal move times
 *
 *	The minimum time is the fastest the move can be performed given the velocity 
 *	constraints on each participating axis - regardless of the feed rate requested. 
 *	The minimum time is the time limited by the rate-limiting axis. The minimum 
 *	time is needed to compute the optimal time and is recorded for possible 
 *	feed override computation..
 *
 *	The optimal time is either the time resulting from the requested feed rate or 
 *	the minimum time if the requested feed rate is not achievable. Optimal times for 
 *	traverses are always the minimum time.
 *
 *	Axis modes are taken into account by having cm_set_target() load the targets 
 *	before calling this function.
 *
 *	The following times are compared and the longest is returned:
 *	  -	G93 inverse time (if G93 is active)
 *	  -	time for coordinated move at requested feed rate
 *	  -	time that the slowest axis would require for the move
 *
 *	Returns:
 *	  - Optimal time returned as the function return
 *	  - Minimum time is set via the function argument
 */
/* --- NIST RS274NGC_v3 Guidance ---
 *
 * The following is verbatim text from NIST RS274NGC_v3. As I interpret A for 
 * moves that combine both linear and rotational movement, the feed rate should
 * apply to the XYZ movement, with the rotational axis (or axes) timed to start
 * and end at the same time the linear move is performed. It is possible under 
 * this case for the rotational move to rate-limit the linear move.
 *
 * 	2.1.2.5 Feed Rate
 *
 *	The rate at which the controlled point or the axes move is nominally a steady 
 *	rate which may be set by the user. In the Interpreter, the interpretation of 
 *	the feed rate is as follows unless inverse time feed rate mode is being used 
 *	in the RS274/NGC view (see Section 3.5.19). The canonical machining functions 
 *	view of feed rate, as described in Section 4.3.5.1, has conditions under which 
 *	the set feed rate is applied differently, but none of these is used in the 
 *	Interpreter.
 *
 *	A. 	For motion involving one or more of the X, Y, and Z axes (with or without 
 *		simultaneous rotational axis motion), the feed rate means length units 
 *		per minute along the programmed XYZ path, as if the rotational axes were 
 *		not moving.
 *
 *	B.	For motion of one rotational axis with X, Y, and Z axes not moving, the 
 *		feed rate means degrees per minute rotation of the rotational axis.
 *
 *	C.	For motion of two or three rotational axes with X, Y, and Z axes not moving, 
 *		the rate is applied as follows. Let dA, dB, and dC be the angles in degrees 
 *		through which the A, B, and C axes, respectively, must move. 
 *		Let D = sqrt(dA^2 + dB^2 + dC^2). Conceptually, D is a measure of total 
 *		angular motion, using the usual Euclidean metric. Let T be the amount of 
 *		time required to move through D degrees at the current feed rate in degrees 
 *		per minute. The rotational axes should be moved in coordinated linear motion 
 *		so that the elapsed time from the start to the end of the motion is T plus 
 *		any time required for acceleration or deceleration.
 */

static float _get_move_times(float *min_time)
{
	float inv_time=0;	// inverse time if doing a feed in G93 mode
	float xyz_time=0;	// coordinated move linear part at req feed rate
	float abc_time=0;	// coordinated move rotary part at req feed rate
	float max_time=0;	// time required for the rate-limiting axis
	float tmp_time=0;	// used in computation
	*min_time = 1234567;// arbitrarily large number

	// compute times for feed motion
	if (gm.motion_mode == MOTION_MODE_STRAIGHT_FEED) {
		if (gm.inverse_feed_rate_mode == true) {
			inv_time = gmx.inverse_feed_rate;
		} else {
			xyz_time = sqrt(square(gm.target[AXIS_X] - gmx.position[AXIS_X]) + // in mm
							square(gm.target[AXIS_Y] - gmx.position[AXIS_Y]) +
							square(gm.target[AXIS_Z] - gmx.position[AXIS_Z])) / gm.feed_rate; // in linear units
			if (xyz_time ==0) {
				abc_time = sqrt(square(gm.target[AXIS_A] - gmx.position[AXIS_A]) + // in deg
								square(gm.target[AXIS_B] - gmx.position[AXIS_B]) +
								square(gm.target[AXIS_C] - gmx.position[AXIS_C])) / gm.feed_rate; // in degree units
			}
		}
	}
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (gm.motion_mode == MOTION_MODE_STRAIGHT_FEED) {
			tmp_time = fabs(gm.target[axis] - gmx.position[axis]) / cfg.a[axis].feedrate_max;
		} else { // gm.motion_mode == MOTION_MODE_STRAIGHT_TRAVERSE
			tmp_time = fabs(gm.target[axis] - gmx.position[axis]) / cfg.a[axis].velocity_max;
		}
		max_time = max(max_time, tmp_time);
		*min_time = min(*min_time, tmp_time);
	}
	return (max4(inv_time, max_time, xyz_time, abc_time));
}

/* 
 * _test_soft_limits() - return error code if soft limit is exceeded
 *
 *	Must be called with target properly set in GM struct. Best done after cm_set_model_target() 
 */
stat_t _test_soft_limits()
{
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if ((gm.target[axis] < 0) || (gm.target[axis] > cfg.a[axis].travel_max)) {
			return (STAT_SOFT_LIMIT_EXCEEDED);
		}
	}
	return (STAT_OK);
}

 

/*************************************************************************
 *
 * CANONICAL MACHINING FUNCTIONS
 *
 *	Values are passed in pre-unit_converted state (from gn structure)
 *	All operations occur on gm (current model state)
 *
 ************************************************************************/

/* 
 * Initialization and Termination (4.3.2)
 *
 * canonical_machine_init() 
 * canonical_machine_alarm() 
 *
 *	Config init cfg_init() must have been run beforehand. Many parameters 
 *	used by the canonical machine are actually set during cfg_init().
 */

void canonical_machine_init()
{
// If you can assume all memory has been zeroed by a hard reset you don't need this code:
	memset(&cm, 0, sizeof(cm));		// reset canonicalMachineSingleton
	memset(&gn, 0, sizeof(gn));		// clear all values, pointers and status
	memset(&gf, 0, sizeof(gf));
	memset(&gm, 0, sizeof(gm));

	// setup magic numbers
	cm.magic_start = MAGICNUM;
	cm.magic_end = MAGICNUM;
	gmx.magic_start = MAGICNUM;
	gmx.magic_end = MAGICNUM;

	// set gcode defaults
	cm_set_units_mode(cfg.units_mode);
	cm_set_coord_system(cfg.coord_system);
	cm_select_plane(cfg.select_plane);
	cm_set_path_control(cfg.path_control);
	cm_set_distance_mode(cfg.distance_mode);

	gmx.block_delete_switch = true;

	// never start a machine in a motion mode	
	gm.motion_mode = MOTION_MODE_CANCEL_MOTION_MODE;

	// reset request flags
	cm.feedhold_requested = false;
	cm.queue_flush_requested = false;
	cm.cycle_start_requested = false;

	// signal that the machine is ready for action
	cm.machine_state = MACHINE_READY;	
	cm.combined_state = COMBINED_READY;
}

/*
 * canonical_machine_alarm() - alarm state; shut down machine
 */
void canonical_machine_alarm(uint8_t value)
{
	// stop the steppers and the spindle
	st_deenergize_motors();
	cm_spindle_control(SPINDLE_OFF);

	// disable all MCode functions
//	gpio_set_bit_off(SPINDLE_BIT);			//###### this current stuff is temporary
//	gpio_set_bit_off(SPINDLE_DIR);
//	gpio_set_bit_off(SPINDLE_PWM);
//	gpio_set_bit_off(MIST_COOLANT_BIT);		//###### replace with exec function
//	gpio_set_bit_off(FLOOD_COOLANT_BIT);	//###### replace with exec function

	rpt_exception(STAT_ALARMED,value);		// send shutdown message
	cm.machine_state = MACHINE_ALARM;
}

/*
 * Representation (4.3.3)
 *
 * Functions that affect the Gcode model only:
 * cm_select_plane()			- G17,G18,G19 select axis plane
 * cm_set_units_mode()			- G20, G21
 * cm_set_distance_mode()		- G90, G91
 * cm_set_coord_offsets()		- G10 (delayed persistence)
 *
 * Functions that affect gcode model and are queued to planner
 * cm_set_coord_system()		- G54-G59
 * cm_set_absolute_origin()		- G28.3 - model, planner and queue to runtime
 * cm_set_axis_origin()			- set the origin of a single axis - model and planner
 * cm_set_origin_offsets()		- G92
 * cm_reset_origin_offsets()	- G92.1
 * cm_suspend_origin_offsets()	- G92.2
 * cm_resume_origin_offsets()	- G92.3
 */

/*
 * cm_select_plane() - G17,G18,G19 select axis plane (AFFECTS MODEL ONLY)
 */
stat_t cm_select_plane(uint8_t plane) 
{
	gm.select_plane = plane;
	if (plane == CANON_PLANE_YZ) {
		gmx.plane_axis_0 = AXIS_Y;
		gmx.plane_axis_1 = AXIS_Z;
		gmx.plane_axis_2 = AXIS_X;
	} else if (plane == CANON_PLANE_XZ) {
		gmx.plane_axis_0 = AXIS_X;
		gmx.plane_axis_1 = AXIS_Z;
		gmx.plane_axis_2 = AXIS_Y;
	} else {
		gmx.plane_axis_0 = AXIS_X;
		gmx.plane_axis_1 = AXIS_Y;
		gmx.plane_axis_2 = AXIS_Z;
	}
	return (STAT_OK);
}

/*
 * cm_set_units_mode() - G20, G21 (AFFECTS MODEL ONLY)
 */
stat_t cm_set_units_mode(uint8_t mode)
{
	gm.units_mode = mode;		// 0 = inches, 1 = mm.
	return(STAT_OK);
}

/*
 * cm_set_distance_mode() - G90, G91 (AFFECTS MODEL ONLY)
 */
stat_t cm_set_distance_mode(uint8_t mode)
{
	gm.distance_mode = mode;		// 0 = absolute mode, 1 = incremental
	return (STAT_OK);
}

/*
 * cm_set_coord_offsets() - G10 L2 Pn (AFFECTS MODEL ONLY)
 *
 *	This function applies the offset to the GM model but does not persist the 
 *	offsets during the Gcode cycle. The persist flag is used to persist offsets
 *	once the cycle has ended. You can also use $g54x - $g59c config functions 
 *	to change offsets.
 */
stat_t cm_set_coord_offsets(uint8_t coord_system, float offset[], float flag[])
{
	if ((coord_system < G54) || (coord_system > COORD_SYSTEM_MAX)) { // you can't set G53
		return (STAT_INTERNAL_RANGE_ERROR);
	}
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (fp_TRUE(flag[axis])) {
			cfg.offset[coord_system][axis] = offset[axis];
			cm.g10_persist_flag = true;		// this will persist offsets to NVM once move has stopped
		}
	}
	return (STAT_OK);
}

/*
 * cm_set_coord_system() - G54-G59 
 * _exec_offset() - callback from planner
 */
stat_t cm_set_coord_system(uint8_t coord_system)
{
	gm.coord_system = coord_system;

	float value[AXES] = { (float)coord_system,0,0,0,0,0 };	// pass coordinate system in value[0] element
	mp_queue_command(_exec_offset, value, value);			// second vector (flags) is not used, so fake it
	return (STAT_OK);
}
static void _exec_offset(float *value, float *flag)
{
	uint8_t coord_system = ((uint8_t)value[0]);				// coordinate system is passed in value[0] element
	float offsets[AXES];
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		offsets[axis] = cfg.offset[coord_system][axis] + (gmx.origin_offset[axis] * gmx.origin_offset_enable);
	}
	mp_set_runtime_work_offset(offsets);
}

/*
 * cm_set_absolute_origin() - G28.3 - model, planner and queue to runtime
 * _exec_absolute_origin()  - callback from planner
 * cm_set_axis_origin()		- set the origin of a single axis - model and planner
 *
 *	cm_set_absolute_origin() takes a vector of origins (presumably 0's, but not 
 *	necessarily) and applies them to all axes where the corresponding position 
 *	in the flag vector is true (1).
 *
 *	This is a 2 step process. The model and planner contexts are set immediately,
 *	the runtime command is queued and synchronized woth the planner queue.
 *
 *	This is an "unofficial gcode" command to allow arbitrarily setting an axis 
 *	to an absolute position. This is needed to support the Otherlab infinite 
 *	Y axis. USE: With the axis(or axes) where you want it, issue g92.4 y0 
 *	(for example). The Y axis will now be set to 0 (or whatever value provided)
 */

stat_t cm_set_absolute_origin(float origin[], float flag[])
{
	float value[AXES];

	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (fp_TRUE(flag[axis])) {
			value[axis] = cfg.offset[gm.coord_system][axis] + _to_millimeters(origin[axis]);
			cm_set_axis_origin(axis, value[axis]);
		}
	}
	mp_queue_command(_exec_absolute_origin, value, flag);
	return (STAT_OK);
}

static void _exec_absolute_origin(float *value, float *flag)
{
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (fp_TRUE(flag[axis])) {
			mp_set_runtime_position(axis, value[axis]);
			cm.homed[axis] = true;				// it's not considered homed until you get to the runtime
		}
	}
}

void cm_set_axis_origin(uint8_t axis, const float position)
{
	gmx.position[axis] = position;
	gm.target[axis] = position;
	mp_set_planner_position(axis, position);
}

/* 
 * cm_set_origin_offsets() 		- G92
 * cm_reset_origin_offsets() 	- G92.1
 * cm_suspend_origin_offsets() 	- G92.2
 * cm_resume_origin_offsets() 	- G92.3
 *
 * G92's behave according to NIST 3.5.18 & LinuxCNC G92
 * http://linuxcnc.org/docs/html/gcode/gcode.html#sec:G92-G92.1-G92.2-G92.3
 */
stat_t cm_set_origin_offsets(float offset[], float flag[])
{
	// set offsets in the Gcode model context
	gmx.origin_offset_enable = 1;
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		if (fp_TRUE(flag[axis])) {
			gmx.origin_offset[axis] = gmx.position[axis] - 
									 cfg.offset[gm.coord_system][axis] - _to_millimeters(offset[axis]);
		}
	}

	// now pass the offset to the callback - setting the coordinate system also applies the offsets
	float value[AXES] = { (float)gm.coord_system,0,0,0,0,0 }; // pass coordinate system in value[0] element
	mp_queue_command(_exec_offset, value, value);				  // second vector is not used
	return (STAT_OK);
}

stat_t cm_reset_origin_offsets()
{
	gmx.origin_offset_enable = 0;
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		gmx.origin_offset[axis] = 0;
	}
	float value[AXES] = { (float)gm.coord_system,0,0,0,0,0 };
	mp_queue_command(_exec_offset, value, value);
	return (STAT_OK);
}

stat_t cm_suspend_origin_offsets()
{
	gmx.origin_offset_enable = 0;
	float value[AXES] = { (float)gm.coord_system,0,0,0,0,0 };
	mp_queue_command(_exec_offset, value, value);
	return (STAT_OK);
}

stat_t cm_resume_origin_offsets()
{
	gmx.origin_offset_enable = 1;
	float value[AXES] = { (float)gm.coord_system,0,0,0,0,0 };
	mp_queue_command(_exec_offset, value, value);
	return (STAT_OK);
}

/* 
 * Free Space Motion (4.3.4)
 *
 * cm_straight_traverse() - G0 linear rapid
 */

stat_t cm_straight_traverse(float target[], float flags[])
{
	gm.motion_mode = MOTION_MODE_STRAIGHT_TRAVERSE;
	cm_set_model_target(target,flags);
	if (vector_equal(gm.target, gmx.position)) { return (STAT_OK); }
//	ritorno(_test_soft_limits());

	cm_get_model_coord_offset_vector(gm.work_offset); // copy the fully resolved offsets to the state
	gm.move_time = _get_move_times(&gm.minimum_time); // set move time and minimum time in the state
	cm_cycle_start();								  // required for homing & other cycles
	stat_t status = mp_aline(&gm);					  // run the move
	cm_set_model_endpoint_position(status);           // update position if the move was successful
	return (status);
}

/*
 * cm_set_g28_position()  - G28.1
 * cm_goto_g28_position() - G28
 * cm_set_g30_position()  - G30.1
 * cm_goto_g30_position() - G30
 */

stat_t cm_set_g28_position(void)
{
	copy_axis_vector(gmx.g28_position, gmx.position);
	return (STAT_OK);
}

stat_t cm_goto_g28_position(float target[], float flags[])
{
	cm_set_absolute_override(true);
	cm_straight_traverse(target, flags);
	while (mp_get_planner_buffers_available() == 0); 	// make sure you have an available buffer
	float f[] = {1,1,1,1,1,1};
	return(cm_straight_traverse(gmx.g28_position, f));
}

stat_t cm_set_g30_position(void)
{
	copy_axis_vector(gmx.g30_position, gmx.position);
	return (STAT_OK);
}

stat_t cm_goto_g30_position(float target[], float flags[])
{
	cm_set_absolute_override(true);
	cm_straight_traverse(target, flags);
	while (mp_get_planner_buffers_available() == 0); 	// make sure you have an available buffer
	float f[] = {1,1,1,1,1,1};
	return(cm_straight_traverse(gmx.g30_position, f));
}

/* 
 * Machining Attributes (4.3.5)
 */ 

/*
 * cm_set_feed_rate() - F parameter
 *
 * Sets feed rate; or sets inverse feed rate if it's active.
 * Converts all values to internal format (mm's)
 * Errs out of feed rate exceeds maximum, but doesn't compute maximum for 
 * inverse feed rate as this would require knowing the move length in advance.
 */

stat_t cm_set_feed_rate(float feed_rate)
{
	if (gm.inverse_feed_rate_mode == true) {
		gmx.inverse_feed_rate = feed_rate;	// minutes per motion for this block only
	} else {
		gm.feed_rate = _to_millimeters(feed_rate);
	}
	return (STAT_OK);
}

/*
 * cm_set_inverse_feed_rate() - G93, G94
 *
 *	TRUE = inverse time feed rate in effect - for this block only
 *	FALSE = units per minute feed rate in effect
 */

stat_t cm_set_inverse_feed_rate_mode(uint8_t mode)
{
	gm.inverse_feed_rate_mode = mode;
	return (STAT_OK);
}

/*
 * cm_set_path_control() - G61, G61.1, G64
 */

stat_t cm_set_path_control(uint8_t mode)
{
	gm.path_control = mode;
	return (STAT_OK);
}

/* 
 * Machining Functions (4.3.6)
 *
 * cm_arc_feed() - see plan_arc.c
 * cm_dwell() - G4, P parameter (seconds)
 * cm_straight_feed() - G1
 */ 

stat_t cm_dwell(float seconds)
{
	gm.parameter = seconds;
	(void)mp_dwell(seconds);
	return (STAT_OK);

}

stat_t cm_straight_feed(float target[], float flags[])
{
	gm.motion_mode = MOTION_MODE_STRAIGHT_FEED;

	// trap zero feed rate condition
	if ((gm.inverse_feed_rate_mode == false) && (fp_ZERO(gm.feed_rate))) {
		return (STAT_GCODE_FEEDRATE_ERROR);
	}

	// Introduce a short delay if the machine is not busy to enable the planning
	// queue to begin to fill (avoids first block having to plan down to zero)
//	if (st_isbusy() == false) {
//		cm_dwell(PLANNER_STARTUP_DELAY_SECONDS);
//	}

	cm_set_model_target(target, flags);
	if (vector_equal(gm.target, gmx.position)) { return (STAT_OK); }

	cm_get_model_coord_offset_vector(gm.work_offset); // copy the fully resolved offsets to the state
	gm.move_time = _get_move_times(&gm.minimum_time); // set move time and minimum time in the state
	cm_cycle_start();								  // required for homing & other cycles
	stat_t status = mp_aline(&gm);					  // run the move
	cm_set_model_endpoint_position(status);           // update position if the move was successful
	return (status);
}

/* 
 * Spindle Functions (4.3.7)
 */
// see spindle.c, spindle.h

/*
 * Tool Functions (4.3.8)
 *
 * cm_change_tool() - M6 (This might become a complete tool change cycle)
 * cm_select_tool() - T parameter
 *
 * These functions are stubbed out for now and don't actually do anything
 */

stat_t cm_change_tool(uint8_t tool)
{
	float value[AXES] = { (float)tool,0,0,0,0,0 };
	mp_queue_command(_exec_change_tool, value, value);
	return (STAT_OK);
}

static void _exec_change_tool(float *value, float *flag)
{
	gm.tool = (uint8_t)value[0];
}

stat_t cm_select_tool(uint8_t tool)
{
	float value[AXES] = { (float)tool,0,0,0,0,0 };
	mp_queue_command(_exec_select_tool, value, value);
	return (STAT_OK);
}
static void _exec_select_tool(float *value, float *flag)
{
	gm.tool = (uint8_t)value[0];
}

//void cm_sync_tool_number(uint8_t tool) { mp_sync_command(SYNC_TOOL_NUMBER, (float)tool);}

/* 
 * Miscellaneous Functions (4.3.9)
 *
 * cm_mist_coolant_control() - M7
 * cm_flood_coolant_control() - M8, M9
 */

stat_t cm_mist_coolant_control(uint8_t mist_coolant)
{
	float value[AXES] = { (float)mist_coolant,0,0,0,0,0 };
	mp_queue_command(_exec_mist_coolant_control, value, value);
	return (STAT_OK);
}
static void _exec_mist_coolant_control(float *value, float *flag)
{
	gm.mist_coolant = (uint8_t)value[0];
	if (gm.mist_coolant == true) {
		gpio_set_bit_on(MIST_COOLANT_BIT);
//+++++	coolant_enable_pin.set();
	} else {
		gpio_set_bit_off(MIST_COOLANT_BIT);
//+++++	coolant_enable_pin.clear();
	}
}

stat_t cm_flood_coolant_control(uint8_t flood_coolant)
{
	float value[AXES] = { (float)flood_coolant,0,0,0,0,0 };
	mp_queue_command(_exec_flood_coolant_control, value, value);
	return (STAT_OK);
}
static void _exec_flood_coolant_control(float *value, float *flag)
{
	gm.flood_coolant = (uint8_t)value[0];
	if (gm.flood_coolant == true) {
		gpio_set_bit_on(FLOOD_COOLANT_BIT);
//		coolant_enable_pin.set();
	} else {
		gpio_set_bit_off(FLOOD_COOLANT_BIT);
//		coolant_enable_pin.clear();
		float val2[AXES] = { 0,0,0,0,0,0 };
		_exec_mist_coolant_control(val2, val2);		// M9 special function
	}
}

/*
 * cm_override_enables() - M48, M49
 * cm_feed_rate_override_enable() - M50
 * cm_feed_rate_override_factor() - M50.1
 * cm_traverse_override_enable() - M50.2
 * cm_traverse_override_factor() - M50.3
 * cm_spindle_override_enable() - M51
 * cm_spindle_override_factor() - M51.1
 *
 *	Override enables are kind of a mess in Gcode. This is an attempt to sort them out.
 *	See http://www.linuxcnc.org/docs/2.4/html/gcode_main.html#sec:M50:-Feed-Override
 */

stat_t cm_override_enables(uint8_t flag)			// M48, M49
{
	gmx.feed_rate_override_enable = flag;
	gmx.traverse_override_enable = flag;
	gmx.spindle_override_enable = flag;
	return (STAT_OK);
}

stat_t cm_feed_rate_override_enable(uint8_t flag)	// M50
{
	if ((gf.parameter == true) && (gn.parameter == 0)) {
		gmx.feed_rate_override_enable = false;
	} else {
		gmx.feed_rate_override_enable = true;
	}
	return (STAT_OK);
}

stat_t cm_feed_rate_override_factor(uint8_t flag)	// M50.1
{
	gmx.feed_rate_override_enable = flag;
	gmx.feed_rate_override_factor = gn.parameter;
//	mp_feed_rate_override(flag, gn.parameter);		// replan the queue for new feed rate
	return (STAT_OK);
}

stat_t cm_traverse_override_enable(uint8_t flag)	// M50.2
{
	if ((gf.parameter == true) && (gn.parameter == 0)) {
		gmx.traverse_override_enable = false;
	} else {
		gmx.traverse_override_enable = true;
	}
	return (STAT_OK);
}

stat_t cm_traverse_override_factor(uint8_t flag)	// M51
{
	gmx.traverse_override_enable = flag;
	gmx.traverse_override_factor = gn.parameter;
//	mp_feed_rate_override(flag, gn.parameter);		// replan the queue for new feed rate
	return (STAT_OK);
}

stat_t cm_spindle_override_enable(uint8_t flag)	// M51.1
{
	if ((gf.parameter == true) && (gn.parameter == 0)) {
		gmx.spindle_override_enable = false;
	} else {
		gmx.spindle_override_enable = true;
	}
	return (STAT_OK);
}

stat_t cm_spindle_override_factor(uint8_t flag)	// M50.1
{
	gmx.spindle_override_enable = flag;
	gmx.spindle_override_factor = gn.parameter;
//	change spindle speed
	return (STAT_OK);
}

/*
 * cm_message() - queue a message to the response string (unconditionally)
 */

void cm_message(char *message)
{
	cmd_add_string("msg",message);
//	cmd_add_conditional_message(message);	// conditional version
}

/*
 * Program Functions (4.3.10)
 *
 * This group implements stop, start, end, and hold. 
 * It is extended beyond the NIST spec to handle various situations.
 *
 *	cm_cycle_start()			(no Gcode)  Do a cycle start right now
 *	cm_cycle_end()				(no Gcode)	Do a cycle end right now
 *	cm_feedhold()				(no Gcode)	Initiate a feedhold right now	
 *	cm_program_stop()			(M0, M60)	Queue a program stop
 *	cm_optional_program_stop()	(M1)
 *	cm_program_end()			(M2, M30)
 *	_exec_program_finalize()
 *
 * cm_program_stop and cm_optional_program_stop are synchronous Gcode 
 * commands that are received through the interpreter. They cause all motion
 * to stop at the end of the current command, including spindle motion. 
 *
 * Note that the stop occurs at the end of the immediately preceding command
 * (i.e. the stop is queued behind the last command).
 *
 * cm_program_end is a stop that also resets the machine to initial state
 */

/*
 * cm_request_feedhold()
 * cm_request_queue_flush()
 * cm_request_cycle_start()
 * cm_feedhold_sequencing_callback() - process feedholds, cycle starts & queue flushes
 * cm_flush_planner() - Flush planner queue and correct model positions
 *
 * Feedholds, queue flushes and cycles starts are all related. The request functions set
 *	flags for these. The sequencing callback interprets the flags according to the 
 *	following rules:
 *
 *	A feedhold request received during motion should be honored
 *	A feedhold request received during a feedhold should be ignored and reset
 *	A feedhold request received during a motion stop should be ignored and reset
 *
 *	A queue flush request received during motion should be ignored but not reset
 *	A queue flush request received during a feedhold should be deferred until 
 *		the feedhold enters a HOLD state (i.e. until deceleration is complete)
 *	A queue flush request received during a motion stop should be honored
 *
 *	A cycle start request received during motion should be ignored and reset
 *	A cycle start request received during a feedhold should be deferred until 
 *		the feedhold enters a HOLD state (i.e. until deceleration is complete)
 *		If a queue flush request is also present the queue flush should be done first
 *	A cycle start request received during a motion stop should be honored and 
 *		should start to run anything in the planner queue
 */

void cm_request_feedhold(void) { cm.feedhold_requested = true; }
void cm_request_queue_flush(void) { cm.queue_flush_requested = true; }
void cm_request_cycle_start(void) { cm.cycle_start_requested = true; }

stat_t cm_feedhold_sequencing_callback()
{
	if (cm.feedhold_requested == true) {
		if ((cm.motion_state == MOTION_RUN) && (cm.hold_state == FEEDHOLD_OFF)) {
			cm.motion_state = MOTION_HOLD;
			cm.hold_state = FEEDHOLD_SYNC;	// invokes hold from aline execution
		}
		cm.feedhold_requested = false;
	}
	if (cm.queue_flush_requested == true) {
		if ((cm.motion_state == MOTION_STOP) ||
			((cm.motion_state == MOTION_HOLD) && (cm.hold_state == FEEDHOLD_HOLD))) {
			cm.queue_flush_requested = false;
			cm_queue_flush();
		}
	}
	if ((cm.cycle_start_requested == true) && (cm.queue_flush_requested == false)) {
		cm.cycle_start_requested = false;
		cm.hold_state = FEEDHOLD_END_HOLD;
		cm_cycle_start();
		mp_end_hold();
	}
	return (STAT_OK);
}

stat_t cm_queue_flush()
{
	xio_reset_usb_rx_buffers();		// flush serial queues
	mp_flush_planner();				// flush planner queue

	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
		mp_set_planner_position(axis, mp_get_runtime_machine_position(axis)); // set mm from mr
		gmx.position[axis] = mp_get_runtime_machine_position(axis);
		gm.target[axis] = gmx.position[axis];
	}
	float value[AXES] = { (float)MACHINE_PROGRAM_STOP, 0,0,0,0,0 };
	_exec_program_finalize(value, value);			// finalize now, not later

// DEPRECATED
//	cm.hold_state = FEEDHOLD_OFF;					// end feedhold (if in feed hold)
//	cm.motion_state = MOTION_STOP;
////	rpt_request_status_report(SR_IMMEDIATE_REQUEST);// request a final status report
//	rpt_request_queue_report();
	return (STAT_OK);
}

/*
 * Program and cycle state functions
 *
 * _exec_program_finalize() 	- helper
 * cm_cycle_start()
 * cm_cycle_end()
 * cm_program_stop()			- M0
 * cm_optional_program_stop()	- M1	
 * cm_program_end()				- M2, M30
 *
 * cm_program_end() implements M2 and M30
 * The END behaviors are defined by NIST 3.6.1 are:
 *	1. Axis offsets are set to zero (like G92.2) and origin offsets are set to the default (like G54)
 *	2. Selected plane is set to CANON_PLANE_XY (like G17)
 *	3. Distance mode is set to MODE_ABSOLUTE (like G90)
 *	4. Feed rate mode is set to UNITS_PER_MINUTE (like G94)
 *	5. Feed and speed overrides are set to ON (like M48)
 *	6. Cutter compensation is turned off (like G40)
 *	7. The spindle is stopped (like M5)
 *	8. The current motion mode is set to G_1 (like G1)
 *	9. Coolant is turned off (like M9)
 *
 * cm_program_end() implments things slightly differently:
 *	1. Axis offsets are set to G92.1 CANCEL offsets (instead of using G92.2 SUSPEND Offsets)
 *	   Set default coordinate system (uses $gco, not G54)
 *	2. Selected plane is set to default plane ($gpl) (instead of setting it to G54)
 *	3. Distance mode is set to MODE_ABSOLUTE (like G90)
 *	4. Feed rate mode is set to UNITS_PER_MINUTE (like G94)
 * 	5. Not implemented
 *	6. Not implemented 
 *	7. The spindle is stopped (like M5)
 *	8. Motion mode is canceled like G80 (not set to G1) 
 *	9. Coolant is turned off (like M9)
 *	+  Default INCHES or MM units mode is restored ($gun) 
 */

static void _exec_program_finalize(float *value, float *flag)
{
	cm.machine_state = (uint8_t)value[0];;
	cm.motion_state = MOTION_STOP;
	if (cm.cycle_state == CYCLE_MACHINING) {
		cm.cycle_state = CYCLE_OFF;					// don't end cycle if homing, probing, etc.
	}
	cm.hold_state = FEEDHOLD_OFF;					// end feedhold (if in feed hold)
	cm.cycle_start_requested = false;				// cancel any pending cycle start request
	mp_zero_segment_velocity();						// for reporting purposes

	// execute program END resets
	if (cm.machine_state == MACHINE_PROGRAM_END) {
		cm_reset_origin_offsets();					// G92.1 - we do G91.1 instead of G92.2
	//	cm_suspend_origin_offsets();				// G92.2 - as per Kramer
		cm_set_coord_system(cfg.coord_system);		// reset to default coordinate system
		cm_select_plane(cfg.select_plane);			// reset to default arc plane
		cm_set_distance_mode(cfg.distance_mode);
		cm_set_units_mode(cfg.units_mode);			// reset to default units mode
		cm_spindle_control(SPINDLE_OFF);			// M5
		cm_flood_coolant_control(false);			// M9
		cm_set_inverse_feed_rate_mode(false);
	//	cm_set_motion_mode(MOTION_MODE_STRAIGHT_FEED);	// NIST specifies G1
		cm_set_motion_mode(MOTION_MODE_CANCEL_MOTION_MODE);	
	}

	rpt_request_status_report(SR_IMMEDIATE_REQUEST);// request a final status report (not unfiltered)
	cmd_persist_offsets(cm.g10_persist_flag);		// persist offsets if any changes made
}

void cm_cycle_start()
{
	cm.machine_state = MACHINE_CYCLE;
	if (cm.cycle_state == CYCLE_OFF) {
		cm.cycle_state = CYCLE_MACHINING;			// don't change homing, probe or other cycles
		rpt_clear_queue_report();					// clear queue reporting buffer counts
	}
}

void cm_cycle_end() 
{
	if (cm.cycle_state != CYCLE_OFF) {
		float value[AXES] = { (float)MACHINE_PROGRAM_STOP, 0,0,0,0,0 };
		_exec_program_finalize(value,value);
	}
}

void cm_program_stop() 
{ 
	float value[AXES] = { (float)MACHINE_PROGRAM_STOP, 0,0,0,0,0 };
	mp_queue_command(_exec_program_finalize, value, value);
}

void cm_optional_program_stop()	
{ 
	float value[AXES] = { (float)MACHINE_PROGRAM_STOP, 0,0,0,0,0 };
	mp_queue_command(_exec_program_finalize, value, value);
}

void cm_program_end()
{
	float value[AXES] = { (float)MACHINE_PROGRAM_END, 0,0,0,0,0 };
	mp_queue_command(_exec_program_finalize, value, value);
}
