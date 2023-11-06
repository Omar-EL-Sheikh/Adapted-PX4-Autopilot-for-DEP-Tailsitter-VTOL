/****************************************************************************
 *
 *   Copyright (c) 2013-2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "FixedwingPositionControl.hpp"

#include <px4_platform_common/events.h>

using math::constrain;
using math::max;
using math::min;
using math::radians;

using matrix::Dcmf;
using matrix::Eulerf;
using matrix::Quatf;
using matrix::Vector2f;
using matrix::Vector2d;
using matrix::Vector3f;
using matrix::wrap_pi;

FixedwingPositionControl::FixedwingPositionControl(bool vtol) :
	ModuleParams(nullptr),
	WorkItem(MODULE_NAME, px4::wq_configurations::nav_and_controllers),
	_attitude_sp_pub(vtol ? ORB_ID(fw_virtual_attitude_setpoint) : ORB_ID(vehicle_attitude_setpoint)),
	_loop_perf(perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")),
	_launchDetector(this),
	_runway_takeoff(this)
{
	if (vtol) {
		_param_handle_airspeed_trans = param_find("VT_ARSP_TRANS");
	}

	// limit to 50 Hz
	_local_pos_sub.set_interval_ms(20);

	_pos_ctrl_status_pub.advertise();
	_pos_ctrl_landing_status_pub.advertise();
	_tecs_status_pub.advertise();

	_airspeed_slew_rate_controller.setSlewRate(ASPD_SP_SLEW_RATE);

	/* fetch initial parameter values */
	parameters_update();
}

FixedwingPositionControl::~FixedwingPositionControl()
{
	perf_free(_loop_perf);
}

bool
FixedwingPositionControl::init()
{
	if (!_local_pos_sub.registerCallback()) {
		PX4_ERR("callback registration failed");
		return false;
	}

	return true;
}

int
FixedwingPositionControl::parameters_update()
{
	updateParams();

	// VTOL parameter VT_ARSP_TRANS
	if (_param_handle_airspeed_trans != PARAM_INVALID) {
		param_get(_param_handle_airspeed_trans, &_param_airspeed_trans);
	}

	// L1 control parameters
	_l1_control.set_l1_damping(_param_fw_l1_damping.get());
	_l1_control.set_l1_period(_param_fw_l1_period.get());
	_l1_control.set_l1_roll_limit(radians(_param_fw_r_lim.get()));
	_l1_control.set_roll_slew_rate(radians(_param_fw_l1_r_slew_max.get()));

	// NPFG parameters
	_npfg.setPeriod(_param_npfg_period.get());
	_npfg.setDamping(_param_npfg_damping.get());
	_npfg.enablePeriodLB(_param_npfg_en_period_lb.get());
	_npfg.enablePeriodUB(_param_npfg_en_period_ub.get());
	_npfg.enableMinGroundSpeed(_param_npfg_en_min_gsp.get());
	_npfg.enableTrackKeeping(_param_npfg_en_track_keeping.get());
	_npfg.enableWindExcessRegulation(_param_npfg_en_wind_reg.get());
	_npfg.setMinGroundSpeed(_param_fw_gnd_spd_min.get());
	_npfg.setMaxTrackKeepingMinGroundSpeed(_param_npfg_track_keeping_gsp_max.get());
	_npfg.setRollTimeConst(_param_npfg_roll_time_const.get());
	_npfg.setSwitchDistanceMultiplier(_param_npfg_switch_distance_multiplier.get());
	_npfg.setRollLimit(radians(_param_fw_r_lim.get()));
	_npfg.setRollSlewRate(radians(_param_fw_l1_r_slew_max.get()));
	_npfg.setPeriodSafetyFactor(_param_npfg_period_safety_factor.get());

	// TECS parameters
	_tecs.set_max_climb_rate(_param_fw_t_clmb_max.get());
	_tecs.set_max_sink_rate(_param_fw_t_sink_max.get());
	_tecs.set_speed_weight(_param_fw_t_spdweight.get());
	_tecs.set_equivalent_airspeed_trim(_param_fw_airspd_trim.get());
	_tecs.set_equivalent_airspeed_min(_param_fw_airspd_min.get());
	_tecs.set_equivalent_airspeed_max(_param_fw_airspd_max.get());
	_tecs.set_min_sink_rate(_param_fw_t_sink_min.get());
	_tecs.set_throttle_damp(_param_fw_t_thr_damp.get());
	_tecs.set_integrator_gain_throttle(_param_fw_t_I_gain_thr.get());
	_tecs.set_integrator_gain_pitch(_param_fw_t_I_gain_pit.get());
	_tecs.set_throttle_slewrate(_param_fw_thr_slew_max.get());
	_tecs.set_vertical_accel_limit(_param_fw_t_vert_acc.get());
	_tecs.set_speed_comp_filter_omega(_param_fw_t_spd_omega.get());
	_tecs.set_roll_throttle_compensation(_param_fw_t_rll2thr.get());
	_tecs.set_pitch_damping(_param_fw_t_ptch_damp.get());
	_tecs.set_height_error_time_constant(_param_fw_t_h_error_tc.get());
	_tecs.set_heightrate_ff(_param_fw_t_hrate_ff.get());
	_tecs.set_airspeed_error_time_constant(_param_fw_t_tas_error_tc.get());
	_tecs.set_ste_rate_time_const(_param_ste_rate_time_const.get());
	_tecs.set_speed_derivative_time_constant(_param_tas_rate_time_const.get());
	_tecs.set_seb_rate_ff_gain(_param_seb_rate_ff.get());

	// Landing slope
	/* check if negative value for 2/3 of flare altitude is set for throttle cut */
	float land_thrust_lim_alt_relative = _param_fw_lnd_tlalt.get();

	if (land_thrust_lim_alt_relative < 0.0f) {
		land_thrust_lim_alt_relative = 0.66f * _param_fw_lnd_flalt.get();
	}

	_landingslope.update(radians(_param_fw_lnd_ang.get()), _param_fw_lnd_flalt.get(), land_thrust_lim_alt_relative,
			     _param_fw_lnd_hvirt.get());

	landing_status_publish();

	int check_ret = PX4_OK;

	// sanity check parameters
	if (_param_fw_airspd_max.get() < _param_fw_airspd_min.get()) {
		mavlink_log_critical(&_mavlink_log_pub, "Config invalid: Airspeed max smaller than min\t");
		/* EVENT
		 * @description
		 * - <param>FW_AIRSPD_MAX</param>: {1:.1}
		 * - <param>FW_AIRSPD_MIN</param>: {2:.1}
		 */
		events::send<float, float>(events::ID("fixedwing_position_control_conf_invalid_airspeed"), events::Log::Error,
					   "Invalid configuration: Airspeed max smaller than min",
					   _param_fw_airspd_max.get(), _param_fw_airspd_min.get());
		check_ret = PX4_ERROR;
	}

	if (_param_fw_airspd_max.get() < 5.0f || _param_fw_airspd_min.get() > 100.0f) {
		mavlink_log_critical(&_mavlink_log_pub, "Config invalid: Airspeed max < 5 m/s or min > 100 m/s\t");
		/* EVENT
		 * @description
		 * - <param>FW_AIRSPD_MAX</param>: {1:.1}
		 * - <param>FW_AIRSPD_MIN</param>: {2:.1}
		 */
		events::send<float, float>(events::ID("fixedwing_position_control_conf_invalid_airspeed_bounds"), events::Log::Error,
					   "Invalid configuration: Airspeed max \\< 5 m/s or min \\> 100 m/s",
					   _param_fw_airspd_max.get(), _param_fw_airspd_min.get());
		check_ret = PX4_ERROR;
	}

	if (_param_fw_airspd_trim.get() < _param_fw_airspd_min.get() ||
	    _param_fw_airspd_trim.get() > _param_fw_airspd_max.get()) {
		mavlink_log_critical(&_mavlink_log_pub, "Config invalid: Airspeed trim out of min or max bounds\t");
		/* EVENT
		 * @description
		 * - <param>FW_AIRSPD_MAX</param>: {1:.1}
		 * - <param>FW_AIRSPD_MIN</param>: {2:.1}
		 * - <param>FW_AIRSPD_TRIM</param>: {3:.1}
		 */
		events::send<float, float, float>(events::ID("fixedwing_position_control_conf_invalid_trim_bounds"),
						  events::Log::Error,
						  "Invalid configuration: Airspeed trim out of min or max bounds",
						  _param_fw_airspd_max.get(), _param_fw_airspd_min.get(), _param_fw_airspd_trim.get());
		check_ret = PX4_ERROR;
	}

	if (_param_fw_airspd_stall.get() > _param_fw_airspd_min.get() * 0.9f) {
		mavlink_log_critical(&_mavlink_log_pub, "Config invalid: Stall airspeed higher than 0.9 of min\t");
		/* EVENT
		 * @description
		 * - <param>FW_AIRSPD_MIN</param>: {1:.1}
		 * - <param>FW_AIRSPD_STALL</param>: {2:.1}
		 */
		events::send<float, float>(events::ID("fixedwing_position_control_conf_invalid_stall"), events::Log::Error,
					   "Invalid configuration: Stall airspeed higher than 90% of minimum airspeed",
					   _param_fw_airspd_min.get(), _param_fw_airspd_stall.get());
		check_ret = PX4_ERROR;
	}

	return check_ret;
}

void
FixedwingPositionControl::vehicle_control_mode_poll()
{
	if (_control_mode_sub.updated()) {
		const bool was_armed = _control_mode.flag_armed;

		if (_control_mode_sub.copy(&_control_mode)) {

			// reset state when arming
			if (!was_armed && _control_mode.flag_armed) {
				reset_takeoff_state(true);
				reset_landing_state();
			}
		}
	}
}

void
FixedwingPositionControl::vehicle_command_poll()
{
	vehicle_command_s vehicle_command;

	while (_vehicle_command_sub.update(&vehicle_command)) {
		if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_GO_AROUND) {
			// only abort landing before point of no return (horizontal and vertical)
			if (_control_mode.flag_control_auto_enabled &&
			    _pos_sp_triplet.current.valid &&
			    (_pos_sp_triplet.current.type == position_setpoint_s::SETPOINT_TYPE_LAND)) {

				abort_landing(true);
			}

		} else if (vehicle_command.command == vehicle_command_s::VEHICLE_CMD_DO_CHANGE_SPEED) {

			if ((static_cast<uint8_t>(vehicle_command.param1 + .5f) == vehicle_command_s::SPEED_TYPE_AIRSPEED)) {
				if (vehicle_command.param2 > FLT_EPSILON) {	// param2 is an equivalent airspeed setpoint
					if (_control_mode_current == FW_POSCTRL_MODE_AUTO) {
						_pos_sp_triplet.current.cruising_speed = vehicle_command.param2;

					} else if (_control_mode_current == FW_POSCTRL_MODE_MANUAL_ALTITUDE
						   || _control_mode_current == FW_POSCTRL_MODE_MANUAL_POSITION) {
						_commanded_manual_airspeed_setpoint = vehicle_command.param2;
					}

				}
			}

		}
	}
}

void
FixedwingPositionControl::airspeed_poll()
{
	bool airspeed_valid = _airspeed_valid;
	airspeed_validated_s airspeed_validated;

	if ((_param_fw_arsp_mode.get() == 0) && _airspeed_validated_sub.update(&airspeed_validated)) {

		_eas2tas = 1.0f; //this is the default value, taken in case of invalid airspeed

		if (PX4_ISFINITE(airspeed_validated.calibrated_airspeed_m_s)
		    && PX4_ISFINITE(airspeed_validated.true_airspeed_m_s)
		    && (airspeed_validated.calibrated_airspeed_m_s > 0.0f)) {

			airspeed_valid = true;

			_time_airspeed_last_valid = airspeed_validated.timestamp;
			_airspeed = airspeed_validated.calibrated_airspeed_m_s;

			_eas2tas = constrain(airspeed_validated.true_airspeed_m_s / airspeed_validated.calibrated_airspeed_m_s, 0.9f, 2.0f);
		}

	} else {
		// no airspeed updates for one second
		if (airspeed_valid && (hrt_elapsed_time(&_time_airspeed_last_valid) > 1_s)) {
			airspeed_valid = false;
		}
	}

	// update TECS if validity changed
	if (airspeed_valid != _airspeed_valid) {
		_tecs.enable_airspeed(airspeed_valid);
		_airspeed_valid = airspeed_valid;
	}
}

void
FixedwingPositionControl::wind_poll()
{
	if (_wind_sub.updated()) {
		wind_s wind;
		_wind_sub.update(&wind);

		// assumes wind is valid if finite
		_wind_valid = PX4_ISFINITE(wind.windspeed_north)
			      && PX4_ISFINITE(wind.windspeed_east);

		_time_wind_last_received = hrt_absolute_time();

		_wind_vel(0) = wind.windspeed_north;
		_wind_vel(1) = wind.windspeed_east;

	} else {
		// invalidate wind estimate usage (and correspondingly NPFG, if enabled) after subscription timeout
		_wind_valid = _wind_valid && (hrt_absolute_time() - _time_wind_last_received) < WIND_EST_TIMEOUT;
	}
}

void
FixedwingPositionControl::manual_control_setpoint_poll()
{
	_manual_control_setpoint_sub.update(&_manual_control_setpoint);

	_manual_control_setpoint_for_height_rate = _manual_control_setpoint.x;
	_manual_control_setpoint_for_airspeed = math::constrain(_manual_control_setpoint.z, 0.0f, 1.0f);

	if (_param_fw_pos_stk_conf.get() & STICK_CONFIG_SWAP_STICKS_BIT) {
		/* Alternate stick allocation (similar concept as for multirotor systems:
		 * demanding up/down with the throttle stick, and move faster/break with the pitch one.
		 */
		_manual_control_setpoint_for_height_rate = -(math::constrain(_manual_control_setpoint.z, 0.0f, 1.0f) * 2.f - 1.f);
		_manual_control_setpoint_for_airspeed = math::constrain(_manual_control_setpoint.x, -1.0f, 1.0f) / 2.f + 0.5f;
	}

	// send neutral setpoints if no update for 1 s
	if (hrt_elapsed_time(&_manual_control_setpoint.timestamp) > 1_s) {
		_manual_control_setpoint_for_height_rate = 0.f;
		_manual_control_setpoint_for_airspeed = 0.5f;
	}
}


void
FixedwingPositionControl::vehicle_attitude_poll()
{
	vehicle_attitude_s att;

	if (_vehicle_attitude_sub.update(&att)) {
		vehicle_angular_velocity_s angular_velocity{};
		_vehicle_angular_velocity_sub.copy(&angular_velocity);
		const Vector3f rates{angular_velocity.xyz};

		Dcmf R{Quatf(att.q)};

		// if the vehicle is a tailsitter we have to rotate the attitude by the pitch offset
		// between multirotor and fixed wing flight
		if (_vehicle_status.is_vtol_tailsitter) {
			const Dcmf R_offset{Eulerf{0.f, M_PI_2_F, 0.f}};
			R = R * R_offset;

			_yawrate = rates(0);

		} else {
			_yawrate = rates(2);
		}

		const Eulerf euler_angles(R);
		_pitch = euler_angles(1);
		_yaw = euler_angles(2);

		_body_acceleration = R.transpose() * Vector3f{_local_pos.ax, _local_pos.ay, _local_pos.az};
		_body_velocity = R.transpose() * Vector3f{_local_pos.vx, _local_pos.vy, _local_pos.vz};

		// load factor due to banking
		const float load_factor = 1.f / cosf(euler_angles(0));
		_tecs.set_load_factor(load_factor);
	}
}

float
FixedwingPositionControl::get_manual_airspeed_setpoint()
{
	float altctrl_airspeed = _param_fw_airspd_trim.get();

	if (_param_fw_pos_stk_conf.get() & STICK_CONFIG_ENABLE_AIRSPEED_SP_MANUAL_BIT) {
		// neutral throttle corresponds to trim airspeed
		if (_manual_control_setpoint_for_airspeed < 0.5f) {
			// lower half of throttle is min to trim airspeed
			altctrl_airspeed = _param_fw_airspd_min.get() +
					   (_param_fw_airspd_trim.get() - _param_fw_airspd_min.get()) *
					   _manual_control_setpoint_for_airspeed * 2;

		} else {
			// upper half of throttle is trim to max airspeed
			altctrl_airspeed = _param_fw_airspd_trim.get() +
					   (_param_fw_airspd_max.get() - _param_fw_airspd_trim.get()) *
					   (_manual_control_setpoint_for_airspeed * 2 - 1);
		}

	} else if (PX4_ISFINITE(_commanded_manual_airspeed_setpoint)) {
		altctrl_airspeed = _commanded_manual_airspeed_setpoint;
	}

	return altctrl_airspeed;
}

float
FixedwingPositionControl::get_auto_airspeed_setpoint(const float control_interval, const float pos_sp_cruise_airspeed,
		const Vector2f &ground_speed)
{
	// overwrite internal setpoint (e.g. set prior through MAV_CMD_DO_CHANGE_SPEED) in case
	// the current position_setpoint contains a valid airspeed setpoint
	float airspeed_setpoint = _param_fw_airspd_trim.get();

	if (PX4_ISFINITE(pos_sp_cruise_airspeed) && pos_sp_cruise_airspeed > FLT_EPSILON) {
		airspeed_setpoint = pos_sp_cruise_airspeed;
	}

	// Adapt cruise airspeed when otherwise the min groundspeed couldn't be maintained
	if (!_l1_control.circle_mode() && !using_npfg_with_wind_estimate()) {
		/*
		 * This error value ensures that a plane (as long as its throttle capability is
		 * not exceeded) travels towards a waypoint (and is not pushed more and more away
		 * by wind). Not countering this would lead to a fly-away. Only non-zero in presence
		 * of sufficient wind. "minimum ground speed undershoot".
		 */
		const float ground_speed_body = _body_velocity(0);

		if (ground_speed_body < _param_fw_gnd_spd_min.get()) {
			airspeed_setpoint += _param_fw_gnd_spd_min.get() - ground_speed_body;
		}
	}

	float airspeed_min_adjusted = _param_fw_airspd_min.get();

	float load_factor = 1.0f;

	if (PX4_ISFINITE(_att_sp.roll_body)) {
		load_factor = 1.0f / sqrtf(cosf(_att_sp.roll_body));
	}

	float weight_ratio = 1.0f;

	if (_param_weight_base.get() > FLT_EPSILON && _param_weight_gross.get() > FLT_EPSILON) {
		weight_ratio = math::constrain(_param_weight_gross.get() / _param_weight_base.get(), MIN_WEIGHT_RATIO,
					       MAX_WEIGHT_RATIO);
	}

	// Stall speed increases with the square root of the load factor times the weight ratio
	// Vs ~ sqrt(load_factor * weight_ratio)
	airspeed_min_adjusted = constrain(_param_fw_airspd_stall.get() * sqrtf(load_factor * weight_ratio),
					  airspeed_min_adjusted, _param_fw_airspd_max.get());

	// constrain setpoint
	airspeed_setpoint = constrain(airspeed_setpoint, airspeed_min_adjusted, _param_fw_airspd_max.get());

	// initialize to current airspeed setpoint, also if previous setpoint is out of bounds to not apply slew rate in that case
	const bool outside_of_limits = _airspeed_slew_rate_controller.getState() < airspeed_min_adjusted
				       || _airspeed_slew_rate_controller.getState() > _param_fw_airspd_max.get();

	if (!PX4_ISFINITE(_airspeed_slew_rate_controller.getState()) || outside_of_limits) {
		_airspeed_slew_rate_controller.setForcedValue(airspeed_setpoint);

	} else if (control_interval > FLT_EPSILON) {
		// constrain airspeed setpoint changes with slew rate of ASPD_SP_SLEW_RATE m/s/s
		airspeed_setpoint = _airspeed_slew_rate_controller.update(airspeed_setpoint, control_interval);
	}

	return airspeed_setpoint;
}

void
FixedwingPositionControl::tecs_status_publish()
{
	tecs_status_s t{};

	switch (_tecs.tecs_mode()) {
	case TECS::ECL_TECS_MODE_NORMAL:
		t.mode = tecs_status_s::TECS_MODE_NORMAL;
		break;

	case TECS::ECL_TECS_MODE_UNDERSPEED:
		t.mode = tecs_status_s::TECS_MODE_UNDERSPEED;
		break;

	case TECS::ECL_TECS_MODE_BAD_DESCENT:
		t.mode = tecs_status_s::TECS_MODE_BAD_DESCENT;
		break;

	case TECS::ECL_TECS_MODE_CLIMBOUT:
		t.mode = tecs_status_s::TECS_MODE_CLIMBOUT;
		break;
	}

	t.altitude_sp = _tecs.hgt_setpoint();
	t.altitude_filtered = _tecs.vert_pos_state();

	t.true_airspeed_sp = _tecs.TAS_setpoint_adj();
	t.true_airspeed_filtered = _tecs.tas_state();

	t.height_rate_setpoint = _tecs.hgt_rate_setpoint();
	t.height_rate = _tecs.vert_vel_state();

	t.equivalent_airspeed_sp = _tecs.get_EAS_setpoint();
	t.true_airspeed_derivative_sp = _tecs.TAS_rate_setpoint();
	t.true_airspeed_derivative = _tecs.speed_derivative();
	t.true_airspeed_derivative_raw = _tecs.speed_derivative_raw();
	t.true_airspeed_innovation = _tecs.getTASInnovation();

	t.total_energy_error = _tecs.STE_error();
	t.total_energy_rate_error = _tecs.STE_rate_error();

	t.energy_distribution_error = _tecs.SEB_error();
	t.energy_distribution_rate_error = _tecs.SEB_rate_error();

	t.total_energy = _tecs.STE();
	t.total_energy_rate = _tecs.STE_rate();
	t.total_energy_balance = _tecs.SEB();
	t.total_energy_balance_rate = _tecs.SEB_rate();

	t.total_energy_sp = _tecs.STE_setpoint();
	t.total_energy_rate_sp = _tecs.STE_rate_setpoint();
	t.total_energy_balance_sp = _tecs.SEB_setpoint();
	t.total_energy_balance_rate_sp = _tecs.SEB_rate_setpoint();

	t.throttle_integ = _tecs.throttle_integ_state();
	t.pitch_integ = _tecs.pitch_integ_state();

	t.throttle_sp = _tecs.get_throttle_setpoint();
	t.pitch_sp_rad = _tecs.get_pitch_setpoint();
	t.throttle_trim = _tecs.get_throttle_trim();

	t.timestamp = hrt_absolute_time();

	_tecs_status_pub.publish(t);
}

void
FixedwingPositionControl::status_publish()
{
	position_controller_status_s pos_ctrl_status = {};

	pos_ctrl_status.nav_roll = _att_sp.roll_body;
	pos_ctrl_status.nav_pitch = _att_sp.pitch_body;

	if (_l1_control.has_guidance_updated()) {
		pos_ctrl_status.nav_bearing = _l1_control.nav_bearing();

		pos_ctrl_status.target_bearing = _l1_control.target_bearing();
		pos_ctrl_status.xtrack_error = _l1_control.crosstrack_error();

	} else {
		pos_ctrl_status.nav_bearing = NAN;
		pos_ctrl_status.target_bearing = NAN;
		pos_ctrl_status.xtrack_error = NAN;

	}

	if (_param_fw_use_npfg.get()) {
		npfg_status_s npfg_status = {};

		npfg_status.wind_est_valid = _wind_valid;

		const float bearing = _npfg.getBearing(); // dont repeat atan2 calc

		pos_ctrl_status.nav_bearing = bearing;
		pos_ctrl_status.target_bearing = _npfg.targetBearing();
		pos_ctrl_status.xtrack_error = _npfg.getTrackError();
		pos_ctrl_status.acceptance_radius = _npfg.switchDistance(500.0f);

		npfg_status.lat_accel = _npfg.getLateralAccel();
		npfg_status.lat_accel_ff = _npfg.getLateralAccelFF();
		npfg_status.heading_ref = _npfg.getHeadingRef();
		npfg_status.bearing = bearing;
		npfg_status.bearing_feas = _npfg.getBearingFeas();
		npfg_status.bearing_feas_on_track = _npfg.getOnTrackBearingFeas();
		npfg_status.signed_track_error = _npfg.getTrackError();
		npfg_status.track_error_bound = _npfg.getTrackErrorBound();
		npfg_status.airspeed_ref = _npfg.getAirspeedRef();
		npfg_status.min_ground_speed_ref = _npfg.getMinGroundSpeedRef();
		npfg_status.adapted_period = _npfg.getAdaptedPeriod();
		npfg_status.p_gain = _npfg.getPGain();
		npfg_status.time_const = _npfg.getTimeConst();
		npfg_status.timestamp = hrt_absolute_time();

		_npfg_status_pub.publish(npfg_status);

	} else {
		pos_ctrl_status.acceptance_radius = _l1_control.switch_distance(500.0f);
	}

	pos_ctrl_status.wp_dist = get_distance_to_next_waypoint(_current_latitude, _current_longitude,
				  _pos_sp_triplet.current.lat, _pos_sp_triplet.current.lon);

	pos_ctrl_status.yaw_acceptance = NAN;

	pos_ctrl_status.timestamp = hrt_absolute_time();

	pos_ctrl_status.type = _position_sp_type;

	_pos_ctrl_status_pub.publish(pos_ctrl_status);
}

void
FixedwingPositionControl::landing_status_publish()
{
	position_controller_landing_status_s pos_ctrl_landing_status = {};

	pos_ctrl_landing_status.slope_angle_rad = _landingslope.landing_slope_angle_rad();
	pos_ctrl_landing_status.horizontal_slope_displacement = _landingslope.horizontal_slope_displacement();
	pos_ctrl_landing_status.flare_length = _landingslope.flare_length();

	pos_ctrl_landing_status.abort_landing = _land_abort;

	pos_ctrl_landing_status.timestamp = hrt_absolute_time();

	_pos_ctrl_landing_status_pub.publish(pos_ctrl_landing_status);
}

void
FixedwingPositionControl::abort_landing(bool abort)
{
	// only announce changes
	if (abort && !_land_abort) {
		mavlink_log_critical(&_mavlink_log_pub, "Landing aborted\t");
		// TODO: add reason
		events::send(events::ID("fixedwing_position_control_land_aborted"), events::Log::Critical, "Landing aborted");
	}

	_land_abort = abort;
	landing_status_publish();
}

void
FixedwingPositionControl::get_waypoint_heading_distance(float heading, position_setpoint_s &waypoint_prev,
		position_setpoint_s &waypoint_next, bool flag_init)
{
	position_setpoint_s temp_prev = waypoint_prev;
	position_setpoint_s temp_next = waypoint_next;

	if (flag_init) {
		// previous waypoint: HDG_HOLD_SET_BACK_DIST meters behind us
		waypoint_from_heading_and_distance(_current_latitude, _current_longitude, heading + radians(180.0f),
						   HDG_HOLD_SET_BACK_DIST, &temp_prev.lat, &temp_prev.lon);

		// next waypoint: HDG_HOLD_DIST_NEXT meters in front of us
		waypoint_from_heading_and_distance(_current_latitude, _current_longitude, heading,
						   HDG_HOLD_DIST_NEXT, &temp_next.lat, &temp_next.lon);

	} else {
		// use the existing flight path from prev to next

		// previous waypoint: shifted HDG_HOLD_REACHED_DIST + HDG_HOLD_SET_BACK_DIST
		create_waypoint_from_line_and_dist(waypoint_next.lat, waypoint_next.lon, waypoint_prev.lat, waypoint_prev.lon,
						   HDG_HOLD_REACHED_DIST + HDG_HOLD_SET_BACK_DIST, &temp_prev.lat, &temp_prev.lon);

		// next waypoint: shifted -(HDG_HOLD_DIST_NEXT + HDG_HOLD_REACHED_DIST)
		create_waypoint_from_line_and_dist(waypoint_next.lat, waypoint_next.lon, waypoint_prev.lat, waypoint_prev.lon,
						   -(HDG_HOLD_REACHED_DIST + HDG_HOLD_DIST_NEXT), &temp_next.lat, &temp_next.lon);
	}

	waypoint_prev = temp_prev;
	waypoint_prev.alt = _current_altitude;
	waypoint_prev.valid = true;

	waypoint_next = temp_next;
	waypoint_next.alt = _current_altitude;
	waypoint_next.valid = true;
}

float
FixedwingPositionControl::get_terrain_altitude_takeoff(float takeoff_alt)
{
	float terrain_alt = _local_pos.ref_alt - (_local_pos.dist_bottom + _local_pos.z);

	if (PX4_ISFINITE(terrain_alt) && _local_pos.dist_bottom_valid) {
		return terrain_alt;
	}

	return takeoff_alt;
}

float
FixedwingPositionControl::getManualHeightRateSetpoint()
{
	/*
	 * The complete range is -1..+1, so this is 6%
	 * of the up or down range or 3% of the total range.
	 */
	const float deadBand = 0.06f;

	/*
	 * The correct scaling of the complete range needs
	 * to account for the missing part of the slope
	 * due to the deadband
	 */
	const float factor = 1.0f - deadBand;

	float height_rate_setpoint = 0.0f;

	/*
	 * Manual control has as convention the rotation around
	 * an axis. Positive X means to rotate positively around
	 * the X axis in NED frame, which is pitching down
	 */
	if (_manual_control_setpoint_for_height_rate > deadBand) {
		/* pitching down */
		float pitch = -(_manual_control_setpoint_for_height_rate - deadBand) / factor;
		height_rate_setpoint = pitch * _param_sinkrate_target.get();

	} else if (_manual_control_setpoint_for_height_rate < - deadBand) {
		/* pitching up */
		float pitch = -(_manual_control_setpoint_for_height_rate + deadBand) / factor;
		const float climb_rate_target = _param_climbrate_target.get();

		height_rate_setpoint = pitch * climb_rate_target;

	}

	return height_rate_setpoint;
}

bool
FixedwingPositionControl::in_takeoff_situation()
{
	// a VTOL does not need special takeoff handling
	if (_vehicle_status.is_vtol) {
		return false;
	}

	// in air for < 10s
	return (hrt_elapsed_time(&_time_went_in_air) < 10_s)
	       && (_current_altitude <= _takeoff_ground_alt + _param_fw_clmbout_diff.get());
}

void
FixedwingPositionControl::set_control_mode_current(const hrt_abstime &now, bool pos_sp_curr_valid)
{
	/* only run position controller in fixed-wing mode and during transitions for VTOL */
	if (_vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING && !_vehicle_status.in_transition_mode) {
		_control_mode_current = FW_POSCTRL_MODE_OTHER;
		return; // do not publish the setpoint
	}

	if (((_control_mode.flag_control_auto_enabled && _control_mode.flag_control_position_enabled) ||
	     _control_mode.flag_control_offboard_enabled) && pos_sp_curr_valid) {

		if (_pos_sp_triplet.current.type == position_setpoint_s::SETPOINT_TYPE_TAKEOFF) {

			if (_vehicle_status.is_vtol && _vehicle_status.in_transition_mode) {
				_control_mode_current = FW_POSCTRL_MODE_AUTO;

				// in this case we want the waypoint handled as a position setpoint -- a submode in control_auto()
				_pos_sp_triplet.current.type = position_setpoint_s::SETPOINT_TYPE_POSITION;

			} else {
				_control_mode_current = FW_POSCTRL_MODE_AUTO_TAKEOFF;
			}

		} else if (_pos_sp_triplet.current.type == position_setpoint_s::SETPOINT_TYPE_LAND) {

			if (!_vehicle_status.in_transition_mode) {
				_control_mode_current = FW_POSCTRL_MODE_AUTO_LANDING;

			} else {
				// in this case we want the waypoint handled as a position setpoint -- a submode in control_auto()
				_pos_sp_triplet.current.type = position_setpoint_s::SETPOINT_TYPE_POSITION;
			}

		} else {
			_control_mode_current = FW_POSCTRL_MODE_AUTO;
		}

	} else if (_control_mode.flag_control_auto_enabled && _control_mode.flag_control_climb_rate_enabled
		   && pos_sp_curr_valid) {

		// reset timer the first time we switch into this mode
		if (_control_mode_current != FW_POSCTRL_MODE_AUTO_ALTITUDE && _control_mode_current != FW_POSCTRL_MODE_AUTO_CLIMBRATE) {
			_time_in_fixed_bank_loiter = now;
		}

		if (hrt_elapsed_time(&_time_in_fixed_bank_loiter) < (_param_nav_gpsf_lt.get() * 1_s)
		    && !_vehicle_status.in_transition_mode) {
			if (_control_mode_current != FW_POSCTRL_MODE_AUTO_ALTITUDE) {
				// Need to init because last loop iteration was in a different mode
				mavlink_log_critical(&_mavlink_log_pub, "Start loiter with fixed bank angle.\t");
				events::send(events::ID("fixedwing_position_control_fb_loiter"), events::Log::Critical,
					     "Start loiter with fixed bank angle");
			}

			_control_mode_current = FW_POSCTRL_MODE_AUTO_ALTITUDE;

		} else {
			if (_control_mode_current != FW_POSCTRL_MODE_AUTO_CLIMBRATE && !_vehicle_status.in_transition_mode) {
				mavlink_log_critical(&_mavlink_log_pub, "Start descending.\t");
				events::send(events::ID("fixedwing_position_control_descend"), events::Log::Critical, "Start descending");
			}

			_control_mode_current = FW_POSCTRL_MODE_AUTO_CLIMBRATE;
		}


	} else if (_control_mode.flag_control_manual_enabled && _control_mode.flag_control_position_enabled) {
		if (_control_mode_current != FW_POSCTRL_MODE_MANUAL_POSITION) {
			/* Need to init because last loop iteration was in a different mode */
			_hdg_hold_yaw = _yaw; // yaw is not controlled, so set setpoint to current yaw
			_hdg_hold_enabled = false; // this makes sure the waypoints are reset below
			_yaw_lock_engaged = false;

			/* reset setpoints from other modes (auto) otherwise we won't
			 * level out without new manual input */
			_att_sp.roll_body = _manual_control_setpoint.y * radians(_param_fw_r_lim.get());
			_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw
		}

		_control_mode_current = FW_POSCTRL_MODE_MANUAL_POSITION;

	} else if (_control_mode.flag_control_manual_enabled && _control_mode.flag_control_altitude_enabled) {

		_control_mode_current = FW_POSCTRL_MODE_MANUAL_ALTITUDE;

	} else {
		_control_mode_current = FW_POSCTRL_MODE_OTHER;
	}
}

void
FixedwingPositionControl::update_in_air_states(const hrt_abstime now)
{
	/* save time when airplane is in air */
	if (!_was_in_air && !_landed) {
		_was_in_air = true;
		_time_went_in_air = now;
		_takeoff_ground_alt = _current_altitude; // XXX: is this really a good idea?
	}

	/* reset flag when airplane landed */
	if (_landed) {
		_was_in_air = false;
	}
}

void
FixedwingPositionControl::move_position_setpoint_for_vtol_transition(position_setpoint_s &current_sp)
{
	// TODO: velocity, altitude, or just a heading hold position mode should be used for this, not position
	// shifting hacks

	if (_vehicle_status.in_transition_to_fw) {

		if (!PX4_ISFINITE(_transition_waypoint(0))) {
			double lat_transition, lon_transition;
			// create a virtual waypoint HDG_HOLD_DIST_NEXT meters in front of the vehicle which the L1 controller can track
			// during the transition
			waypoint_from_heading_and_distance(_current_latitude, _current_longitude, _yaw, HDG_HOLD_DIST_NEXT, &lat_transition,
							   &lon_transition);

			_transition_waypoint(0) = lat_transition;
			_transition_waypoint(1) = lon_transition;
		}


		current_sp.lat = _transition_waypoint(0);
		current_sp.lon = _transition_waypoint(1);

	} else {
		/* reset transition waypoint, will be set upon entering front transition */
		_transition_waypoint(0) = static_cast<double>(NAN);
		_transition_waypoint(1) = static_cast<double>(NAN);
	}
}

void
FixedwingPositionControl::control_auto(const float control_interval, const Vector2d &curr_pos,
				       const Vector2f &ground_speed, const position_setpoint_s &pos_sp_prev, const position_setpoint_s &pos_sp_curr,
				       const position_setpoint_s &pos_sp_next)
{
	position_setpoint_s current_sp = pos_sp_curr;
	move_position_setpoint_for_vtol_transition(current_sp);

	const uint8_t position_sp_type = handle_setpoint_type(current_sp);

	_position_sp_type = position_sp_type;

	if (position_sp_type == position_setpoint_s::SETPOINT_TYPE_LOITER
	    || current_sp.type == position_setpoint_s::SETPOINT_TYPE_LOITER) {
		publishOrbitStatus(current_sp);
	}

	const bool was_circle_mode = (_param_fw_use_npfg.get()) ? _npfg.circleMode() : _l1_control.circle_mode();

	switch (position_sp_type) {
	case position_setpoint_s::SETPOINT_TYPE_IDLE:
		_att_sp.thrust_body[0] = 0.0f;
		_att_sp.roll_body = 0.0f;
		_att_sp.pitch_body = radians(_param_fw_psp_off.get());
		_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
		_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;

		break;

	case position_setpoint_s::SETPOINT_TYPE_POSITION:
		control_auto_position(control_interval, curr_pos, ground_speed, pos_sp_prev, current_sp);
		break;

	case position_setpoint_s::SETPOINT_TYPE_VELOCITY:
		control_auto_velocity(control_interval, curr_pos, ground_speed, current_sp);
		break;

	case position_setpoint_s::SETPOINT_TYPE_LOITER:
		control_auto_loiter(control_interval, curr_pos, ground_speed, pos_sp_prev, current_sp, pos_sp_next);
		break;
	}

	if (was_circle_mode && !_l1_control.circle_mode()) {
		/* just kicked out of loiter, reset roll integrals */
		_att_sp.roll_reset_integral = true;
	}

	/* Copy thrust output for publication, handle special cases */
	if (position_sp_type == position_setpoint_s::SETPOINT_TYPE_IDLE) {

		_att_sp.thrust_body[0] = 0.0f;

	} else {
		// when we are landed state we want the motor to spin at idle speed
		_att_sp.thrust_body[0] = (_landed) ? min(_param_fw_thr_idle.get(), 1.f) : get_tecs_thrust();
	}

	/* Copy thrust and pitch values from tecs */
	_att_sp.pitch_body = get_tecs_pitch();

	if (!_vehicle_status.in_transition_to_fw) {
		publishLocalPositionSetpoint(current_sp);
	}
}

void
FixedwingPositionControl::control_auto_fixed_bank_alt_hold(const float control_interval)
{
	// only control altitude and airspeed ("fixed-bank loiter")

	tecs_update_pitch_throttle(control_interval,
				   _current_altitude,
				   _param_fw_airspd_trim.get(),
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   _param_fw_thr_min.get(),
				   _param_fw_thr_max.get(),
				   false,
				   _param_fw_p_lim_min.get());

	_att_sp.roll_body = math::radians(_param_nav_gpsf_r.get()); // open loop loiter bank angle
	_att_sp.yaw_body = 0.f;

	if (_landed) {
		_att_sp.thrust_body[0] = _param_fw_thr_min.get();

	} else {
		_att_sp.thrust_body[0] = min(get_tecs_thrust(), _param_fw_thr_max.get());
	}

	_att_sp.pitch_body = get_tecs_pitch();

	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
}

void
FixedwingPositionControl::control_auto_descend(const float control_interval)
{
	// Hard-code descend rate to 0.5m/s. This is a compromise to give the system to recover,
	// but not letting it drift too far away.
	const float descend_rate = -0.5f;

	tecs_update_pitch_throttle(control_interval,
				   _current_altitude,
				   _param_fw_airspd_trim.get(),
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   _param_fw_thr_min.get(),
				   _param_fw_thr_max.get(),
				   false,
				   _param_fw_p_lim_min.get(),
				   false,
				   descend_rate);

	_att_sp.roll_body = math::radians(_param_nav_gpsf_r.get()); // open loop loiter bank angle
	_att_sp.yaw_body = 0.f;

	_att_sp.thrust_body[0] = (_landed) ? _param_fw_thr_min.get() : min(get_tecs_thrust(), _param_fw_thr_max.get());
	_att_sp.pitch_body = get_tecs_pitch();

	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
}

uint8_t
FixedwingPositionControl::handle_setpoint_type(const position_setpoint_s &pos_sp_curr)
{
	uint8_t position_sp_type = pos_sp_curr.type;

	if (!_control_mode.flag_control_position_enabled && _control_mode.flag_control_velocity_enabled) {
		return position_setpoint_s::SETPOINT_TYPE_VELOCITY;
	}

	Vector2d curr_wp{0, 0};

	/* current waypoint (the one currently heading for) */
	curr_wp = Vector2d(pos_sp_curr.lat, pos_sp_curr.lon);

	const float acc_rad = (_param_fw_use_npfg.get()) ? _npfg.switchDistance(500.0f) : _l1_control.switch_distance(500.0f);

	if (pos_sp_curr.type == position_setpoint_s::SETPOINT_TYPE_POSITION
	    || pos_sp_curr.type == position_setpoint_s::SETPOINT_TYPE_LOITER) {

		float dist_xy = -1.f;
		float dist_z = -1.f;

		const float dist = get_distance_to_point_global_wgs84(
					   (double)curr_wp(0), (double)curr_wp(1), pos_sp_curr.alt,
					   _current_latitude, _current_longitude, _current_altitude,
					   &dist_xy, &dist_z);

		float loiter_radius_abs = fabsf(_param_nav_loiter_rad.get());

		if (fabsf(pos_sp_curr.loiter_radius) > FLT_EPSILON) {
			loiter_radius_abs = fabsf(pos_sp_curr.loiter_radius);
		}

		if (pos_sp_curr.type == position_setpoint_s::SETPOINT_TYPE_POSITION) {
			// POSITION: achieve position setpoint altitude via loiter
			// close to waypoint, but altitude error greater than twice acceptance
			if ((!_vehicle_status.in_transition_mode) && (dist >= 0.f)
			    && (dist_z > _param_nav_fw_alt_rad.get())
			    && (dist_xy < 2.f * math::max(acc_rad, loiter_radius_abs))) {
				// SETPOINT_TYPE_POSITION -> SETPOINT_TYPE_LOITER
				position_sp_type = position_setpoint_s::SETPOINT_TYPE_LOITER;
			}

		} else if (pos_sp_curr.type == position_setpoint_s::SETPOINT_TYPE_LOITER) {
			// LOITER: use SETPOINT_TYPE_POSITION to get to SETPOINT_TYPE_LOITER (NPFG handles this internally)
			if ((dist >= 0.f)
			    && (dist_xy > 2.f * math::max(acc_rad, loiter_radius_abs))
			    && !_param_fw_use_npfg.get()) {
				// SETPOINT_TYPE_LOITER -> SETPOINT_TYPE_POSITION
				position_sp_type = position_setpoint_s::SETPOINT_TYPE_POSITION;
			}
		}
	}

	return position_sp_type;
}

void
FixedwingPositionControl::control_auto_position(const float control_interval, const Vector2d &curr_pos,
		const Vector2f &ground_speed, const position_setpoint_s &pos_sp_prev, const position_setpoint_s &pos_sp_curr)
{
	const float acc_rad = (_param_fw_use_npfg.get()) ? _npfg.switchDistance(500.0f) : _l1_control.switch_distance(500.0f);
	Vector2d curr_wp{0, 0};
	Vector2d prev_wp{0, 0};

	/* current waypoint (the one currently heading for) */
	curr_wp = Vector2d(pos_sp_curr.lat, pos_sp_curr.lon);

	if (pos_sp_prev.valid) {
		prev_wp(0) = pos_sp_prev.lat;
		prev_wp(1) = pos_sp_prev.lon;

	} else {
		/*
			* No valid previous waypoint, go for the current wp.
			* This is automatically handled by the L1 library.
			*/
		prev_wp(0) = pos_sp_curr.lat;
		prev_wp(1) = pos_sp_curr.lon;
	}

	float tecs_fw_thr_min;
	float tecs_fw_thr_max;

	if (pos_sp_curr.gliding_enabled) {
		/* enable gliding with this waypoint */
		_tecs.set_speed_weight(2.0f);
		tecs_fw_thr_min = 0.0;
		tecs_fw_thr_max = 0.0;

	} else {
		tecs_fw_thr_min = _param_fw_thr_min.get();
		tecs_fw_thr_max = _param_fw_thr_max.get();
	}

	// waypoint is a plain navigation waypoint
	float position_sp_alt = pos_sp_curr.alt;

	// Altitude first order hold (FOH)
	if (pos_sp_prev.valid && PX4_ISFINITE(pos_sp_prev.alt) &&
	    ((pos_sp_prev.type == position_setpoint_s::SETPOINT_TYPE_POSITION) ||
	     (pos_sp_prev.type == position_setpoint_s::SETPOINT_TYPE_LOITER))
	   ) {
		const float d_curr_prev = get_distance_to_next_waypoint((double)curr_wp(0), (double)curr_wp(1),
					  pos_sp_prev.lat, pos_sp_prev.lon);

		// Do not try to find a solution if the last waypoint is inside the acceptance radius of the current one
		if (d_curr_prev > math::max(acc_rad, fabsf(pos_sp_curr.loiter_radius))) {
			// Calculate distance to current waypoint
			const float d_curr = get_distance_to_next_waypoint((double)curr_wp(0), (double)curr_wp(1),
					     _current_latitude, _current_longitude);

			// Save distance to waypoint if it is the smallest ever achieved, however make sure that
			// _min_current_sp_distance_xy is never larger than the distance between the current and the previous wp
			_min_current_sp_distance_xy = math::min(d_curr, _min_current_sp_distance_xy, d_curr_prev);

			// if the minimal distance is smaller than the acceptance radius, we should be at waypoint alt
			// navigator will soon switch to the next waypoint item (if there is one) as soon as we reach this altitude
			if (_min_current_sp_distance_xy > math::max(acc_rad, fabsf(pos_sp_curr.loiter_radius))) {
				// The setpoint is set linearly and such that the system reaches the current altitude at the acceptance
				// radius around the current waypoint
				const float delta_alt = (pos_sp_curr.alt - pos_sp_prev.alt);
				const float grad = -delta_alt / (d_curr_prev - math::max(acc_rad, fabsf(pos_sp_curr.loiter_radius)));
				const float a = pos_sp_prev.alt - grad * d_curr_prev;

				position_sp_alt = a + grad * _min_current_sp_distance_xy;
			}
		}
	}

	float target_airspeed = get_auto_airspeed_setpoint(control_interval, pos_sp_curr.cruising_speed, ground_speed);

	Vector2f curr_pos_local{_local_pos.x, _local_pos.y};
	Vector2f curr_wp_local = _global_local_proj_ref.project(curr_wp(0), curr_wp(1));
	Vector2f prev_wp_local = _global_local_proj_ref.project(prev_wp(0), prev_wp(1));

	if (_param_fw_use_npfg.get()) {
		_npfg.setAirspeedNom(target_airspeed * _eas2tas);
		_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);

		if (_control_mode.flag_control_offboard_enabled && PX4_ISFINITE(pos_sp_curr.vx) && PX4_ISFINITE(pos_sp_curr.vy)) {
			// Navigate directly on position setpoint and path tangent
			matrix::Vector2f velocity_2d(pos_sp_curr.vx, pos_sp_curr.vy);
			float curvature = PX4_ISFINITE(_pos_sp_triplet.current.loiter_radius) ? 1 / _pos_sp_triplet.current.loiter_radius :
					  0.0f;
			_npfg.navigatePathTangent(curr_pos_local, curr_wp_local, velocity_2d.normalized(), get_nav_speed_2d(ground_speed),
						  _wind_vel, curvature);

		} else {
			_npfg.navigateWaypoints(prev_wp_local, curr_wp_local, curr_pos_local, get_nav_speed_2d(ground_speed), _wind_vel);
		}

		_att_sp.roll_body = _npfg.getRollSetpoint();
		target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

	} else {
		_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, get_nav_speed_2d(ground_speed));
		_att_sp.roll_body = _l1_control.get_roll_setpoint();
	}

	_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;

	tecs_update_pitch_throttle(control_interval,
				   position_sp_alt,
				   target_airspeed,
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   tecs_fw_thr_min,
				   tecs_fw_thr_max,
				   false,
				   radians(_param_fw_p_lim_min.get()));
}

void
FixedwingPositionControl::control_auto_velocity(const float control_interval, const Vector2d &curr_pos,
		const Vector2f &ground_speed, const position_setpoint_s &pos_sp_curr)
{
	float tecs_fw_thr_min;
	float tecs_fw_thr_max;

	if (pos_sp_curr.gliding_enabled) {
		/* enable gliding with this waypoint */
		_tecs.set_speed_weight(2.0f);
		tecs_fw_thr_min = 0.0;
		tecs_fw_thr_max = 0.0;

	} else {
		tecs_fw_thr_min = _param_fw_thr_min.get();
		tecs_fw_thr_max = _param_fw_thr_max.get();
	}

	// waypoint is a plain navigation waypoint
	float position_sp_alt = pos_sp_curr.alt;


	//Offboard velocity control
	Vector2f target_velocity{pos_sp_curr.vx, pos_sp_curr.vy};
	_target_bearing = wrap_pi(atan2f(target_velocity(1), target_velocity(0)));

	float target_airspeed = get_auto_airspeed_setpoint(control_interval, pos_sp_curr.cruising_speed, ground_speed);

	if (_param_fw_use_npfg.get()) {
		_npfg.setAirspeedNom(target_airspeed * _eas2tas);
		_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
		_npfg.navigateBearing(_target_bearing, ground_speed, _wind_vel);
		_att_sp.roll_body = _npfg.getRollSetpoint();
		target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

	} else {
		_l1_control.navigate_heading(_target_bearing, _yaw, ground_speed);
		_att_sp.roll_body = _l1_control.get_roll_setpoint();
	}

	_att_sp.yaw_body = _yaw;

	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;

	tecs_update_pitch_throttle(control_interval,
				   position_sp_alt,
				   target_airspeed,
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   tecs_fw_thr_min,
				   tecs_fw_thr_max,
				   false,
				   radians(_param_fw_p_lim_min.get()),
				   tecs_status_s::TECS_MODE_NORMAL,
				   pos_sp_curr.vz);
}

void
FixedwingPositionControl::control_auto_loiter(const float control_interval, const Vector2d &curr_pos,
		const Vector2f &ground_speed, const position_setpoint_s &pos_sp_prev, const position_setpoint_s &pos_sp_curr,
		const position_setpoint_s &pos_sp_next)
{
	Vector2d curr_wp{0, 0};
	Vector2d prev_wp{0, 0};

	/* current waypoint (the one currently heading for) */
	curr_wp = Vector2d(pos_sp_curr.lat, pos_sp_curr.lon);

	if (pos_sp_prev.valid) {
		prev_wp(0) = pos_sp_prev.lat;
		prev_wp(1) = pos_sp_prev.lon;

	} else {
		/*
			* No valid previous waypoint, go for the current wp.
			* This is automatically handled by the L1 library.
			*/
		prev_wp(0) = pos_sp_curr.lat;
		prev_wp(1) = pos_sp_curr.lon;
	}

	float airspeed_sp = -1.f;

	if (PX4_ISFINITE(pos_sp_curr.cruising_speed) &&
	    pos_sp_curr.cruising_speed > FLT_EPSILON) {

		airspeed_sp = pos_sp_curr.cruising_speed;
	}

	float tecs_fw_thr_min;
	float tecs_fw_thr_max;

	if (pos_sp_curr.gliding_enabled) {
		/* enable gliding with this waypoint */
		_tecs.set_speed_weight(2.0f);
		tecs_fw_thr_min = 0.0;
		tecs_fw_thr_max = 0.0;

	} else {
		tecs_fw_thr_min = _param_fw_thr_min.get();
		tecs_fw_thr_max = _param_fw_thr_max.get();
	}

	/* waypoint is a loiter waypoint */
	float loiter_radius = pos_sp_curr.loiter_radius;
	uint8_t loiter_direction = pos_sp_curr.loiter_direction;

	if (fabsf(pos_sp_curr.loiter_radius) < FLT_EPSILON) {
		loiter_radius = _param_nav_loiter_rad.get();
		loiter_direction = signNoZero(loiter_radius);
	}

	const bool in_circle_mode = (_param_fw_use_npfg.get()) ? _npfg.circleMode() : _l1_control.circle_mode();

	if (pos_sp_next.type == position_setpoint_s::SETPOINT_TYPE_LAND && pos_sp_next.valid
	    && in_circle_mode && _param_fw_lnd_earlycfg.get()) {
		// We're in a loiter directly before a landing WP. Enable our landing configuration (flaps,
		// landing airspeed and potentially tighter altitude control) already such that we don't
		// have to do this switch (which can cause significant altitude errors) close to the ground.
		_tecs.set_height_error_time_constant(_param_fw_thrtc_sc.get() * _param_fw_t_h_error_tc.get());
		airspeed_sp = _param_fw_lnd_airspd_sc.get() * _param_fw_airspd_min.get();
		_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_LAND;
		_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_LAND;

	} else {
		_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
		_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
	}

	float target_airspeed = get_auto_airspeed_setpoint(control_interval, airspeed_sp, ground_speed);

	Vector2f curr_pos_local{_local_pos.x, _local_pos.y};
	Vector2f curr_wp_local = _global_local_proj_ref.project(curr_wp(0), curr_wp(1));

	if (_param_fw_use_npfg.get()) {
		_npfg.setAirspeedNom(target_airspeed * _eas2tas);
		_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
		_npfg.navigateLoiter(curr_wp_local, curr_pos_local, loiter_radius, loiter_direction, get_nav_speed_2d(ground_speed),
				     _wind_vel);
		_att_sp.roll_body = _npfg.getRollSetpoint();
		target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

	} else {
		_l1_control.navigate_loiter(curr_wp_local, curr_pos_local, loiter_radius, loiter_direction,
					    get_nav_speed_2d(ground_speed));
		_att_sp.roll_body = _l1_control.get_roll_setpoint();
	}

	_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

	float alt_sp = pos_sp_curr.alt;

	if (in_takeoff_situation()) {
		alt_sp = max(alt_sp, _takeoff_ground_alt + _param_fw_clmbout_diff.get());
		_att_sp.roll_body = constrain(_att_sp.roll_body, radians(-5.0f), radians(5.0f));
	}

	if (_land_abort) {
		if (pos_sp_curr.alt - _current_altitude  < _param_fw_clmbout_diff.get()) {
			// aborted landing complete, normal loiter over landing point
			abort_landing(false);

		} else {
			// continue straight until vehicle has sufficient altitude
			_att_sp.roll_body = 0.0f;
		}

		_tecs.set_height_error_time_constant(_param_fw_thrtc_sc.get() * _param_fw_t_h_error_tc.get());
	}

	tecs_update_pitch_throttle(control_interval,
				   alt_sp,
				   target_airspeed,
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   tecs_fw_thr_min,
				   tecs_fw_thr_max,
				   false,
				   radians(_param_fw_p_lim_min.get()));
}

void
FixedwingPositionControl::control_auto_takeoff(const hrt_abstime &now, const float control_interval,
		const Vector2d &curr_pos, const Vector2f &ground_speed, const position_setpoint_s &pos_sp_prev,
		const position_setpoint_s &pos_sp_curr)
{
	/* current waypoint (the one currently heading for) */
	Vector2d curr_wp(pos_sp_curr.lat, pos_sp_curr.lon);
	Vector2d prev_wp{0, 0}; /* previous waypoint */
	Vector2f curr_pos_local{_local_pos.x, _local_pos.y};
	Vector2f curr_wp_local = _global_local_proj_ref.project(curr_wp(0), curr_wp(1));

	if (pos_sp_prev.valid) {
		prev_wp(0) = pos_sp_prev.lat;
		prev_wp(1) = pos_sp_prev.lon;

	} else {
		/*
		 * No valid previous waypoint, go for the current wp.
		 * This is automatically handled by the L1 library.
		 */
		prev_wp(0) = pos_sp_curr.lat;
		prev_wp(1) = pos_sp_curr.lon;
	}

	// continuously reset launch detection and runway takeoff until armed
	if (!_control_mode.flag_armed) {
		_launchDetector.reset();
		_launch_detection_state = LAUNCHDETECTION_RES_NONE;
		_launch_detection_notify = 0;
	}

	if (_runway_takeoff.runwayTakeoffEnabled()) {
		if (!_runway_takeoff.isInitialized()) {
			_runway_takeoff.init(now, _yaw, _current_latitude, _current_longitude);

			/* need this already before takeoff is detected
			 * doesn't matter if it gets reset when takeoff is detected eventually */
			_takeoff_ground_alt = _current_altitude;

			mavlink_log_info(&_mavlink_log_pub, "Takeoff on runway\t");
			events::send(events::ID("fixedwing_position_control_takeoff"), events::Log::Info, "Takeoff on runway");
		}

		float terrain_alt = get_terrain_altitude_takeoff(_takeoff_ground_alt);

		// update runway takeoff helper
		_runway_takeoff.update(now, _airspeed, _current_altitude - terrain_alt,
				       _current_latitude, _current_longitude, &_mavlink_log_pub);

		float target_airspeed = get_auto_airspeed_setpoint(control_interval,
					_runway_takeoff.getMinAirspeedScaling() * _param_fw_airspd_min.get(), ground_speed);

		/*
		 * Update navigation: _runway_takeoff returns the start WP according to mode and phase.
		 * If we use the navigator heading or not is decided later.
		 */
		Vector2f prev_wp_local = _global_local_proj_ref.project(_runway_takeoff.getStartWP()(0),
					 _runway_takeoff.getStartWP()(1));

		if (_param_fw_use_npfg.get()) {
			_npfg.setAirspeedNom(target_airspeed * _eas2tas);
			_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
			// NOTE: current waypoint is passed twice to trigger the "point following" logic -- TODO: create
			// point following navigation interface instead of this hack.
			_npfg.navigateWaypoints(curr_wp_local, curr_wp_local, curr_pos_local, ground_speed, _wind_vel);
			_att_sp.roll_body = _runway_takeoff.getRoll(_npfg.getRollSetpoint());
			target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

		} else {
			_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed);
			_att_sp.roll_body = _runway_takeoff.getRoll(_l1_control.get_roll_setpoint());
		}

		_att_sp.yaw_body = _runway_takeoff.getYaw(_yaw);


		// update tecs
		const float takeoff_pitch_max_deg = _runway_takeoff.getMaxPitch(_param_fw_p_lim_max.get());

		tecs_update_pitch_throttle(control_interval,
					   pos_sp_curr.alt,
					   target_airspeed,
					   radians(_param_fw_p_lim_min.get()),
					   radians(takeoff_pitch_max_deg),
					   _param_fw_thr_min.get(),
					   _param_fw_thr_max.get(), // XXX should we also set runway_takeoff_throttle here?
					   _runway_takeoff.climbout(),
					   radians(_runway_takeoff.getMinPitch(_takeoff_pitch_min.get(), _param_fw_p_lim_min.get())));

		// assign values
		_att_sp.fw_control_yaw = _runway_takeoff.controlYaw();
		_att_sp.pitch_body = _runway_takeoff.getPitch(get_tecs_pitch());

		// reset integrals except yaw (which also counts for the wheel controller)
		_att_sp.roll_reset_integral = _runway_takeoff.resetIntegrators();
		_att_sp.pitch_reset_integral = _runway_takeoff.resetIntegrators();

		// apply flaps for takeoff according to the corresponding scale factor set via FW_FLAPS_TO_SCL
		_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_TAKEOFF;
		_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;

	} else {
		/* Perform launch detection */
		if (_launchDetector.launchDetectionEnabled() &&
		    _launch_detection_state != LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS) {

			if (_control_mode.flag_armed) {
				/* Perform launch detection */

				/* Inform user that launchdetection is running every 4s */
				if ((now - _launch_detection_notify) > 4_s) {
					mavlink_log_critical(&_mavlink_log_pub, "Launch detection running\t");
					events::send(events::ID("fixedwing_position_control_launch_detection"), events::Log::Info, "Launch detection running");
					_launch_detection_notify = now;
				}

				/* Detect launch using body X (forward) acceleration */
				_launchDetector.update(control_interval, _body_acceleration(0));

				/* update our copy of the launch detection state */
				_launch_detection_state = _launchDetector.getLaunchDetected();
			}

		} else	{
			/* no takeoff detection --> fly */
			_launch_detection_state = LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS;
		}

		/* Set control values depending on the detection state */
		if (_launch_detection_state != LAUNCHDETECTION_RES_NONE) {
			/* Launch has been detected, hence we have to control the plane. */

			float target_airspeed = get_auto_airspeed_setpoint(control_interval, _param_fw_airspd_trim.get(), ground_speed);

			Vector2f prev_wp_local = _global_local_proj_ref.project(prev_wp(0), prev_wp(1));

			if (_param_fw_use_npfg.get()) {
				_npfg.setAirspeedNom(target_airspeed * _eas2tas);
				_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
				_npfg.navigateWaypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed, _wind_vel);
				_att_sp.roll_body = _npfg.getRollSetpoint();
				target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

			} else {
				_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed);
				_att_sp.roll_body = _l1_control.get_roll_setpoint();
			}

			_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

			/* Select throttle: only in LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS we want to use
			 * full throttle, otherwise we use idle throttle */
			float takeoff_throttle = _param_fw_thr_max.get();

			if (_launch_detection_state != LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS) {
				takeoff_throttle = _param_fw_thr_idle.get();
			}

			/* select maximum pitch: the launchdetector may impose another limit for the pitch
			 * depending on the state of the launch */
			const float takeoff_pitch_max_deg = _launchDetector.getPitchMax(_param_fw_p_lim_max.get());
			const float altitude_error = pos_sp_curr.alt - _current_altitude;

			/* apply minimum pitch and limit roll if target altitude is not within climbout_diff meters */
			if (_param_fw_clmbout_diff.get() > 0.0f && altitude_error > _param_fw_clmbout_diff.get()) {
				/* enforce a minimum of 10 degrees pitch up on takeoff, or take parameter */
				tecs_update_pitch_throttle(control_interval,
							   pos_sp_curr.alt,
							   target_airspeed,
							   radians(_param_fw_p_lim_min.get()),
							   radians(takeoff_pitch_max_deg),
							   _param_fw_thr_min.get(),
							   takeoff_throttle,
							   true,
							   radians(_takeoff_pitch_min.get()));

				/* limit roll motion to ensure enough lift */
				_att_sp.roll_body = constrain(_att_sp.roll_body, radians(-15.0f), radians(15.0f));

			} else {
				tecs_update_pitch_throttle(control_interval,
							   pos_sp_curr.alt,
							   target_airspeed,
							   radians(_param_fw_p_lim_min.get()),
							   radians(_param_fw_p_lim_max.get()),
							   _param_fw_thr_min.get(),
							   takeoff_throttle,
							   false,
							   radians(_param_fw_p_lim_min.get()));
			}

		} else {
			/* Tell the attitude controller to stop integrating while we are waiting
			 * for the launch */
			_att_sp.roll_reset_integral = true;
			_att_sp.pitch_reset_integral = true;
			_att_sp.yaw_reset_integral = true;

			/* Set default roll and pitch setpoints during detection phase */
			_att_sp.roll_body = 0.0f;
			_att_sp.pitch_body = radians(_takeoff_pitch_min.get());
		}

		_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
		_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
	}

	if (_launch_detection_state != LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS && !_runway_takeoff.runwayTakeoffEnabled()) {

		/* making sure again that the correct thrust is used,
		 * without depending on library calls for safety reasons.
		   the pre-takeoff throttle and the idle throttle normally map to the same parameter. */
		_att_sp.thrust_body[0] = _param_fw_thr_idle.get();

	} else if (_runway_takeoff.runwayTakeoffEnabled()) {

		_att_sp.thrust_body[0] = _runway_takeoff.getThrottle(now, get_tecs_thrust());

	} else {
		/* Copy thrust and pitch values from tecs */
		// when we are landed state we want the motor to spin at idle speed
		_att_sp.thrust_body[0] = (_landed) ? min(_param_fw_thr_idle.get(), 1.f) : get_tecs_thrust();
	}

	// only use TECS pitch setpoint if launch has not been detected and runway takeoff is not enabled
	if (!(_launch_detection_state == LAUNCHDETECTION_RES_NONE || _runway_takeoff.runwayTakeoffEnabled())) {
		_att_sp.pitch_body = get_tecs_pitch();
	}

	if (!_vehicle_status.in_transition_to_fw) {
		publishLocalPositionSetpoint(pos_sp_curr);
	}
}

void
FixedwingPositionControl::control_auto_landing(const hrt_abstime &now, const float control_interval,
		const Vector2d &curr_pos, const Vector2f &ground_speed, const position_setpoint_s &pos_sp_prev,
		const position_setpoint_s &pos_sp_curr)
{
	// Enable tighter altitude control for landings
	_tecs.set_height_error_time_constant(_param_fw_thrtc_sc.get() * _param_fw_t_h_error_tc.get());

	/* current waypoint (the one currently heading for) */
	Vector2d curr_wp(pos_sp_curr.lat, pos_sp_curr.lon);
	Vector2d prev_wp{0, 0}; /* previous waypoint */

	if (pos_sp_prev.valid) {
		prev_wp(0) = pos_sp_prev.lat;
		prev_wp(1) = pos_sp_prev.lon;

	} else {
		/*
		 * No valid previous waypoint, go for the current wp.
		 * This is automatically handled by the L1 library.
		 */
		prev_wp(0) = pos_sp_curr.lat;
		prev_wp(1) = pos_sp_curr.lon;
	}

	// save time at which we started landing and reset abort_landing
	if (_time_started_landing == 0) {
		reset_landing_state();
		_time_started_landing = now;
	}

	const float bearing_airplane_currwp = get_bearing_to_next_waypoint((double)curr_pos(0), (double)curr_pos(1),
					      (double)curr_wp(0), (double)curr_wp(1));

	float bearing_lastwp_currwp = bearing_airplane_currwp;

	if (pos_sp_prev.valid) {
		bearing_lastwp_currwp = get_bearing_to_next_waypoint((double)prev_wp(0), (double)prev_wp(1), (double)curr_wp(0),
					(double)curr_wp(1));
	}

	/* Horizontal landing control */
	/* switch to heading hold for the last meters, continue heading hold after */
	float wp_distance = get_distance_to_next_waypoint((double)curr_pos(0), (double)curr_pos(1), (double)curr_wp(0),
			    (double)curr_wp(1));

	/* calculate a waypoint distance value which is 0 when the aircraft is behind the waypoint */
	float wp_distance_save = wp_distance;

	if (fabsf(wrap_pi(bearing_airplane_currwp - bearing_lastwp_currwp)) >= radians(90.0f)) {
		wp_distance_save = 0.0f;
	}

	// create virtual waypoint which is on the desired flight path but
	// some distance behind landing waypoint. This will make sure that the plane
	// will always follow the desired flight path even if we get close or past
	// the landing waypoint
	if (pos_sp_prev.valid) {
		double lat = pos_sp_curr.lat;
		double lon = pos_sp_curr.lon;

		create_waypoint_from_line_and_dist(pos_sp_curr.lat, pos_sp_curr.lon,
						   pos_sp_prev.lat, pos_sp_prev.lon, -1000.0f, &lat, &lon);

		curr_wp(0) = lat;
		curr_wp(1) = lon;
	}

	Vector2f curr_pos_local{_local_pos.x, _local_pos.y};
	Vector2f curr_wp_local = _global_local_proj_ref.project(curr_wp(0), curr_wp(1));
	Vector2f prev_wp_local = _global_local_proj_ref.project(prev_wp(0),
				 prev_wp(1));

	// we want the plane to keep tracking the desired flight path until we start flaring
	// if we go into heading hold mode earlier then we risk to be pushed away from the runway by cross winds
	if ((_param_fw_lnd_hhdist.get() > 0.0f) && !_land_noreturn_horizontal &&
	    ((wp_distance < _param_fw_lnd_hhdist.get()) || _land_noreturn_vertical)) {

		if (pos_sp_prev.valid) {
			/* heading hold, along the line connecting this and the last waypoint */
			_target_bearing = bearing_lastwp_currwp;

		} else {
			_target_bearing = _yaw;
		}

		_land_noreturn_horizontal = true;
		mavlink_log_info(&_mavlink_log_pub, "Landing, heading hold\t");
		events::send(events::ID("fixedwing_position_control_landing"), events::Log::Info, "Landing, heading hold");
	}

	/* Vertical landing control */
	/* apply minimum pitch (flare) and limit roll if close to touch down, altitude error is negative (going down) */

	// default to no terrain estimation, just use landing waypoint altitude
	float terrain_alt = pos_sp_curr.alt;

	if (_param_fw_lnd_useter.get() == 1) {
		if (_local_pos.dist_bottom_valid) {
			// all good, have valid terrain altitude
			float terrain_vpos = _local_pos.dist_bottom + _local_pos.z;
			terrain_alt = (_local_pos.ref_alt - terrain_vpos);
			_last_valid_terrain_alt_estimate = terrain_alt;
			_last_time_terrain_alt_was_valid = now;

		} else if (_last_time_terrain_alt_was_valid == 0) {
			// we have started landing phase but don't have valid terrain
			// wait for some time, maybe we will soon get a valid estimate
			// until then just use the altitude of the landing waypoint
			if ((now - _time_started_landing) < 10_s) {
				terrain_alt = pos_sp_curr.alt;

			} else {
				// still no valid terrain, abort landing
				terrain_alt = pos_sp_curr.alt;
				abort_landing(true);
			}

		} else if ((!_local_pos.dist_bottom_valid && (now - _last_time_terrain_alt_was_valid) < TERRAIN_ALT_TIMEOUT)
			   || _land_noreturn_vertical) {
			// use previous terrain estimate for some time and hope to recover
			// if we are already flaring (land_noreturn_vertical) then just
			//  go with the old estimate
			terrain_alt = _last_valid_terrain_alt_estimate;

		} else {
			// terrain alt was not valid for long time, abort landing
			terrain_alt = _last_valid_terrain_alt_estimate;
			abort_landing(true);
		}
	}

	/* Check if we should start flaring with a vertical and a
	 * horizontal limit (with some tolerance)
	 * The horizontal limit is only applied when we are in front of the wp
	 */
	if ((_current_altitude < terrain_alt + _landingslope.flare_relative_alt()) ||
	    _land_noreturn_vertical) {  //checking for land_noreturn to avoid unwanted climb out

		/* land with minimal speed */

		/* force TECS to only control speed with pitch, altitude is only implicitly controlled now */
		// _tecs.set_speed_weight(2.0f);

		/* kill the throttle if param requests it */
		float throttle_max = _param_fw_thr_max.get();

		if (((_current_altitude < terrain_alt + _landingslope.motor_lim_relative_alt()) &&
		     (wp_distance_save < _landingslope.flare_length() + 5.0f)) || // Only kill throttle when close to WP
		    _land_motor_lim) {
			throttle_max = min(throttle_max, _param_fw_thr_lnd_max.get());

			if (!_land_motor_lim) {
				_land_motor_lim  = true;
				mavlink_log_info(&_mavlink_log_pub, "Landing, limiting throttle\t");
				events::send(events::ID("fixedwing_position_control_landing_limit_throttle"), events::Log::Info,
					     "Landing, limiting throttle");
			}
		}

		float flare_curve_alt_rel = _landingslope.getFlareCurveRelativeAltitudeSave(wp_distance, bearing_lastwp_currwp,
					    bearing_airplane_currwp);

		/* avoid climbout */
		if ((_flare_curve_alt_rel_last < flare_curve_alt_rel && _land_noreturn_vertical) || _land_stayonground) {
			flare_curve_alt_rel = 0.0f; // stay on ground
			_land_stayonground = true;
		}

		const float airspeed_land = _param_fw_lnd_airspd_sc.get() * _param_fw_airspd_min.get();
		float target_airspeed = get_auto_airspeed_setpoint(control_interval, airspeed_land, ground_speed);

		/* lateral guidance */
		if (_param_fw_use_npfg.get()) {
			_npfg.setAirspeedNom(target_airspeed * _eas2tas);
			_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);

			if (_land_noreturn_horizontal) {
				// heading hold
				_npfg.navigateHeading(_target_bearing, ground_speed, _wind_vel);

			} else {
				// normal navigation
				_npfg.navigateWaypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed, _wind_vel);
			}

			target_airspeed = _npfg.getAirspeedRef() /  _eas2tas;
			_att_sp.roll_body = _npfg.getRollSetpoint();

		} else {
			if (_land_noreturn_horizontal) {
				// heading hold
				_l1_control.navigate_heading(_target_bearing, _yaw, ground_speed);

			} else {
				// normal navigation
				_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed);
			}

			_att_sp.roll_body = _l1_control.get_roll_setpoint();
		}

		if (_land_noreturn_horizontal) {
			/* limit roll motion to prevent wings from touching the ground first */
			_att_sp.roll_body = constrain(_att_sp.roll_body, radians(-10.0f), radians(10.0f));
		}

		/* enable direct yaw control using rudder/wheel */
		if (_land_noreturn_horizontal) {
			_att_sp.yaw_body = _target_bearing;
			_att_sp.fw_control_yaw = true;

		} else {
			_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw
		}

		tecs_update_pitch_throttle(control_interval,
					   terrain_alt + flare_curve_alt_rel,
					   target_airspeed,
					   radians(_param_fw_lnd_fl_pmin.get()),
					   radians(_param_fw_lnd_fl_pmax.get()),
					   0.0f,
					   throttle_max,
					   false,
					   _land_motor_lim ? radians(_param_fw_lnd_fl_pmin.get()) : radians(_param_fw_p_lim_min.get()),
					   true);

		if (!_land_noreturn_vertical) {
			// just started with the flaring phase
			_flare_pitch_sp = radians(_param_fw_psp_off.get());
			_flare_height = _current_altitude - terrain_alt;
			mavlink_log_info(&_mavlink_log_pub, "Landing, flaring\t");
			events::send(events::ID("fixedwing_position_control_landing_flaring"), events::Log::Info, "Landing, flaring");
			_land_noreturn_vertical = true;

		} else {
			if (_local_pos.vz > 0.1f) {
				_flare_pitch_sp = radians(_param_fw_lnd_fl_pmin.get()) *
						  constrain((_flare_height - (_current_altitude - terrain_alt)) / _flare_height, 0.0f, 1.0f);
			}

			// otherwise continue using previous _flare_pitch_sp
		}

		_att_sp.pitch_body = _flare_pitch_sp;
		_flare_curve_alt_rel_last = flare_curve_alt_rel;

	} else {

		/* intersect glide slope:
		 * minimize speed to approach speed
		 * if current position is higher than the slope follow the glide slope (sink to the
		 * glide slope)
		 * also if the system captures the slope it should stay
		 * on the slope (bool land_onslope)
		 * if current position is below the slope continue at previous wp altitude
		 * until the intersection with slope
		 * */

		float altitude_desired = terrain_alt;

		const float landing_slope_alt_rel_desired = _landingslope.getLandingSlopeRelativeAltitudeSave(wp_distance,
				bearing_lastwp_currwp, bearing_airplane_currwp);

		if (_current_altitude > terrain_alt + landing_slope_alt_rel_desired || _land_onslope) {
			/* stay on slope */
			altitude_desired = terrain_alt + landing_slope_alt_rel_desired;

			if (!_land_onslope) {
				mavlink_log_info(&_mavlink_log_pub, "Landing, on slope\t");
				events::send(events::ID("fixedwing_position_control_landing_on_slope"), events::Log::Info, "Landing, on slope");
				_land_onslope = true;
			}

		} else {
			/* continue horizontally */
			if (pos_sp_prev.valid) {
				altitude_desired = pos_sp_prev.alt;

			} else {
				altitude_desired = _current_altitude;
			}
		}

		const float airspeed_approach = _param_fw_lnd_airspd_sc.get() * _param_fw_airspd_min.get();
		float target_airspeed = get_auto_airspeed_setpoint(control_interval, airspeed_approach, ground_speed);

		/* lateral guidance */
		if (_param_fw_use_npfg.get()) {
			_npfg.setAirspeedNom(target_airspeed * _eas2tas);
			_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);

			if (_land_noreturn_horizontal) {
				// heading hold
				_npfg.navigateHeading(_target_bearing, ground_speed, _wind_vel);

			} else {
				// normal navigation
				_npfg.navigateWaypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed, _wind_vel);
			}

			target_airspeed = _npfg.getAirspeedRef() / _eas2tas;
			_att_sp.roll_body = _npfg.getRollSetpoint();

		} else {
			if (_land_noreturn_horizontal) {
				// heading hold
				_l1_control.navigate_heading(_target_bearing, _yaw, ground_speed);

			} else {
				// normal navigation
				_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed);
			}

			_att_sp.roll_body = _l1_control.get_roll_setpoint();
		}

		_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

		tecs_update_pitch_throttle(control_interval,
					   altitude_desired,
					   target_airspeed,
					   radians(_param_fw_p_lim_min.get()),
					   radians(_param_fw_p_lim_max.get()),
					   _param_fw_thr_min.get(),
					   _param_fw_thr_max.get(),
					   false,
					   radians(_param_fw_p_lim_min.get()));
		_att_sp.pitch_body = get_tecs_pitch();
	}

	/* Copy thrust and pitch values from tecs */
	// when we are landed state we want the motor to spin at idle speed
	_att_sp.thrust_body[0] = (_landed) ? min(_param_fw_thr_idle.get(), 1.f) : get_tecs_thrust();

	// Apply flaps and spoilers for landing. Amount of deflection is handled in the FW attitdue controller
	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_LAND;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_LAND;

	if (!_vehicle_status.in_transition_to_fw) {
		publishLocalPositionSetpoint(pos_sp_curr);
	}
}

void
FixedwingPositionControl::control_manual_altitude(const float control_interval, const Vector2d &curr_pos,
		const Vector2f &ground_speed)
{
	/* ALTITUDE CONTROL: pitch stick moves altitude setpoint, throttle stick sets airspeed */

	/* Get demanded airspeed */
	float altctrl_airspeed = get_manual_airspeed_setpoint();

	// if we assume that user is taking off then help by demanding altitude setpoint well above ground
	// and set limit to pitch angle to prevent steering into ground
	// this will only affect planes and not VTOL
	float pitch_limit_min = _param_fw_p_lim_min.get();
	float height_rate_sp = NAN;
	float altitude_sp_amsl = _current_altitude;

	if (in_takeoff_situation()) {
		// if we assume that user is taking off then help by demanding altitude setpoint well above ground
		// and set limit to pitch angle to prevent steering into ground
		// this will only affect planes and not VTOL
		altitude_sp_amsl = _takeoff_ground_alt + _param_fw_clmbout_diff.get();
		pitch_limit_min = radians(10.0f);

	} else {
		height_rate_sp = getManualHeightRateSetpoint();
	}

	/* throttle limiting */
	float throttle_max = _param_fw_thr_max.get();

	if (_landed && (fabsf(_manual_control_setpoint_for_airspeed) < THROTTLE_THRESH)) {
		throttle_max = 0.0f;
	}

	tecs_update_pitch_throttle(control_interval,
				   altitude_sp_amsl,
				   altctrl_airspeed,
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   _param_fw_thr_min.get(),
				   throttle_max,
				   false,
				   pitch_limit_min,
				   false,
				   height_rate_sp);

	_att_sp.roll_body = _manual_control_setpoint.y * radians(_param_fw_r_lim.get());
	_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

	/* Copy thrust and pitch values from tecs */
	if (_landed) {
		// when we are landed state we want the motor to spin at idle speed
		_att_sp.thrust_body[0] = min(_param_fw_thr_idle.get(), throttle_max);

	} else {
		_att_sp.thrust_body[0] = min(get_tecs_thrust(), throttle_max);
	}

	_att_sp.pitch_body = get_tecs_pitch();

	// In Manual modes flaps and spoilers are directly controlled in FW Attitude controller and not passed
	// through attitdue setpoints
	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
}

void
FixedwingPositionControl::control_manual_position(const float control_interval, const Vector2d &curr_pos,
		const Vector2f &ground_speed)
{
	// if we assume that user is taking off then help by demanding altitude setpoint well above ground
	// and set limit to pitch angle to prevent steering into ground
	// this will only affect planes and not VTOL
	float pitch_limit_min = _param_fw_p_lim_min.get();
	float height_rate_sp = NAN;
	float altitude_sp_amsl = _current_altitude;

	if (in_takeoff_situation()) {
		// if we assume that user is taking off then help by demanding altitude setpoint well above ground
		// and set limit to pitch angle to prevent steering into ground
		// this will only affect planes and not VTOL
		altitude_sp_amsl = _takeoff_ground_alt + _param_fw_clmbout_diff.get();
		pitch_limit_min = radians(10.0f);

	} else {
		height_rate_sp = getManualHeightRateSetpoint();
	}

	/* throttle limiting */
	float throttle_max = _param_fw_thr_max.get();

	if (_landed && (fabsf(_manual_control_setpoint_for_airspeed) < THROTTLE_THRESH)) {
		throttle_max = 0.0f;
	}

	float target_airspeed = get_manual_airspeed_setpoint();

	/* heading control */
	if (fabsf(_manual_control_setpoint.y) < HDG_HOLD_MAN_INPUT_THRESH &&
	    fabsf(_manual_control_setpoint.r) < HDG_HOLD_MAN_INPUT_THRESH) {

		/* heading / roll is zero, lock onto current heading */
		if (fabsf(_yawrate) < HDG_HOLD_YAWRATE_THRESH && !_yaw_lock_engaged) {
			// little yaw movement, lock to current heading
			_yaw_lock_engaged = true;
		}

		/* user tries to do a takeoff in heading hold mode, reset the yaw setpoint on every iteration
			to make sure the plane does not start rolling
		*/
		if (in_takeoff_situation()) {
			_hdg_hold_enabled = false;
			_yaw_lock_engaged = true;
		}

		if (_yaw_lock_engaged) {

			/* just switched back from non heading-hold to heading hold */
			if (!_hdg_hold_enabled) {
				_hdg_hold_enabled = true;
				_hdg_hold_yaw = _yaw;

				get_waypoint_heading_distance(_hdg_hold_yaw, _hdg_hold_prev_wp, _hdg_hold_curr_wp, true);
			}

			/* we have a valid heading hold position, are we too close? */
			const float dist = get_distance_to_next_waypoint(_current_latitude, _current_longitude, _hdg_hold_curr_wp.lat,
					   _hdg_hold_curr_wp.lon);

			if (dist < HDG_HOLD_REACHED_DIST) {
				get_waypoint_heading_distance(_hdg_hold_yaw, _hdg_hold_prev_wp, _hdg_hold_curr_wp, false);
			}

			Vector2d prev_wp{_hdg_hold_prev_wp.lat, _hdg_hold_prev_wp.lon};
			Vector2d curr_wp{_hdg_hold_curr_wp.lat, _hdg_hold_curr_wp.lon};

			Vector2f curr_pos_local{_local_pos.x, _local_pos.y};
			Vector2f curr_wp_local = _global_local_proj_ref.project(curr_wp(0), curr_wp(1));
			Vector2f prev_wp_local = _global_local_proj_ref.project(prev_wp(0),
						 prev_wp(1));

			if (_param_fw_use_npfg.get()) {
				_npfg.setAirspeedNom(target_airspeed * _eas2tas);
				_npfg.setAirspeedMax(_param_fw_airspd_max.get() * _eas2tas);
				_npfg.navigateWaypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed, _wind_vel);
				_att_sp.roll_body = _npfg.getRollSetpoint();
				target_airspeed = _npfg.getAirspeedRef() / _eas2tas;

			} else {
				/* populate l1 control setpoint */
				_l1_control.navigate_waypoints(prev_wp_local, curr_wp_local, curr_pos_local, ground_speed);
				_att_sp.roll_body = _l1_control.get_roll_setpoint();
			}

			_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw

			if (in_takeoff_situation()) {
				/* limit roll motion to ensure enough lift */
				_att_sp.roll_body = constrain(_att_sp.roll_body, radians(-15.0f), radians(15.0f));
			}
		}
	}

	tecs_update_pitch_throttle(control_interval,
				   altitude_sp_amsl,
				   target_airspeed,
				   radians(_param_fw_p_lim_min.get()),
				   radians(_param_fw_p_lim_max.get()),
				   _param_fw_thr_min.get(),
				   throttle_max,
				   false,
				   pitch_limit_min,
				   false,
				   height_rate_sp);

	if (!_yaw_lock_engaged || fabsf(_manual_control_setpoint.y) >= HDG_HOLD_MAN_INPUT_THRESH ||
	    fabsf(_manual_control_setpoint.r) >= HDG_HOLD_MAN_INPUT_THRESH) {

		_hdg_hold_enabled = false;
		_yaw_lock_engaged = false;

		// do slew rate limiting on roll if enabled
		float roll_sp_new = _manual_control_setpoint.y * radians(_param_fw_r_lim.get());
		const float roll_rate_slew_rad = radians(_param_fw_l1_r_slew_max.get());

		if (control_interval > 0.f && roll_rate_slew_rad > 0.f) {
			roll_sp_new = constrain(roll_sp_new, _att_sp.roll_body - roll_rate_slew_rad * control_interval,
						_att_sp.roll_body + roll_rate_slew_rad * control_interval);
		}

		_att_sp.roll_body = roll_sp_new;
		_att_sp.yaw_body = _yaw; // yaw is not controlled, so set setpoint to current yaw
	}

	/* Copy thrust and pitch values from tecs */
	if (_landed) {
		// when we are landed state we want the motor to spin at idle speed
		_att_sp.thrust_body[0] = min(_param_fw_thr_idle.get(), throttle_max);

	} else {
		_att_sp.thrust_body[0] = min(get_tecs_thrust(), throttle_max);
	}

	_att_sp.pitch_body = get_tecs_pitch();

	// In Manual modes flaps and spoilers are directly controlled in FW Attitude controller and not passed
	// through attitdue setpoints
	_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
	_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;
}

float
FixedwingPositionControl::get_tecs_pitch()
{
	if (_is_tecs_running) {
		return _tecs.get_pitch_setpoint() + radians(_param_fw_psp_off.get());
	}

	// return level flight pitch offset to prevent stale tecs state when it's not running
	return radians(_param_fw_psp_off.get());
}

float
FixedwingPositionControl::get_tecs_thrust()
{
	if (_is_tecs_running) {
		return min(_tecs.get_throttle_setpoint(), 1.f);
	}

	// return 0 to prevent stale tecs state when it's not running
	return 0.0f;
}

void
FixedwingPositionControl::Run()
{
	if (should_exit()) {
		_local_pos_sub.unregisterCallback();
		exit_and_cleanup();
		return;
	}

	perf_begin(_loop_perf);

	/* only run controller if position changed */

	if (_local_pos_sub.update(&_local_pos)) {

		const float control_interval = math::constrain((_local_pos.timestamp - _last_time_position_control_called) * 1e-6f,
					       MIN_AUTO_TIMESTEP, MAX_AUTO_TIMESTEP);
		_last_time_position_control_called = _local_pos.timestamp;

		// check for parameter updates
		if (_parameter_update_sub.updated()) {
			// clear update
			parameter_update_s pupdate;
			_parameter_update_sub.copy(&pupdate);

			// update parameters from storage
			parameters_update();
		}

		vehicle_global_position_s gpos;

		if (_global_pos_sub.update(&gpos)) {
			_current_latitude = gpos.lat;
			_current_longitude = gpos.lon;
		}

		_current_altitude = -_local_pos.z + _local_pos.ref_alt; // Altitude AMSL in meters

		// handle estimator reset events. we only adjust setpoins for manual modes
		if (_control_mode.flag_control_manual_enabled) {
			if (_control_mode.flag_control_altitude_enabled && _local_pos.vz_reset_counter != _alt_reset_counter) {
				// make TECS accept step in altitude and demanded altitude
				_tecs.handle_alt_step(-_local_pos.delta_z, _current_altitude);
			}

			// adjust navigation waypoints in position control mode
			if (_control_mode.flag_control_altitude_enabled && _control_mode.flag_control_velocity_enabled
			    && _local_pos.vxy_reset_counter != _pos_reset_counter) {

				// reset heading hold flag, which will re-initialise position control
				_hdg_hold_enabled = false;
			}
		}

		// update the reset counters in any case
		_alt_reset_counter = _local_pos.vz_reset_counter;
		_pos_reset_counter = _local_pos.vxy_reset_counter;

		// Convert Local setpoints to global setpoints
		if (!_global_local_proj_ref.isInitialized()
		    || (_global_local_proj_ref.getProjectionReferenceTimestamp() != _local_pos.ref_timestamp)
		    || (_local_pos.vxy_reset_counter != _pos_reset_counter)) {

			_global_local_proj_ref.initReference(_local_pos.ref_lat, _local_pos.ref_lon,
							     _local_pos.ref_timestamp);
			_global_local_alt0 = _local_pos.ref_alt;
		}

		if (_control_mode.flag_control_offboard_enabled) {
			vehicle_local_position_setpoint_s trajectory_setpoint;

			if (_trajectory_setpoint_sub.update(&trajectory_setpoint)) {
				bool valid_setpoint = false;
				_pos_sp_triplet = {}; // clear any existing
				_pos_sp_triplet.timestamp = trajectory_setpoint.timestamp;
				_pos_sp_triplet.current.timestamp = trajectory_setpoint.timestamp;
				_pos_sp_triplet.current.cruising_speed = NAN; // ignored
				_pos_sp_triplet.current.cruising_throttle = NAN; // ignored
				_pos_sp_triplet.current.vx = NAN;
				_pos_sp_triplet.current.vy = NAN;
				_pos_sp_triplet.current.vz = NAN;
				_pos_sp_triplet.current.lat = static_cast<double>(NAN);
				_pos_sp_triplet.current.lon = static_cast<double>(NAN);
				_pos_sp_triplet.current.alt = NAN;

				if (PX4_ISFINITE(trajectory_setpoint.x) && PX4_ISFINITE(trajectory_setpoint.y) && PX4_ISFINITE(trajectory_setpoint.z)) {
					if (_global_local_proj_ref.isInitialized()) {
						double lat;
						double lon;
						_global_local_proj_ref.reproject(trajectory_setpoint.x, trajectory_setpoint.y, lat, lon);
						valid_setpoint = true;
						_pos_sp_triplet.current.type = position_setpoint_s::SETPOINT_TYPE_POSITION;
						_pos_sp_triplet.current.lat = lat;
						_pos_sp_triplet.current.lon = lon;
						_pos_sp_triplet.current.alt = _global_local_alt0 - trajectory_setpoint.z;
					}

				}

				if (PX4_ISFINITE(trajectory_setpoint.vx) && PX4_ISFINITE(trajectory_setpoint.vx)
				    && PX4_ISFINITE(trajectory_setpoint.vz)) {
					valid_setpoint = true;
					_pos_sp_triplet.current.type = position_setpoint_s::SETPOINT_TYPE_POSITION;
					_pos_sp_triplet.current.vx = trajectory_setpoint.vx;
					_pos_sp_triplet.current.vy = trajectory_setpoint.vy;
					_pos_sp_triplet.current.vz = trajectory_setpoint.vz;

					if (PX4_ISFINITE(trajectory_setpoint.acceleration[0]) && PX4_ISFINITE(trajectory_setpoint.acceleration[1])
					    && PX4_ISFINITE(trajectory_setpoint.acceleration[2])) {
						Vector2f velocity_sp_2d(trajectory_setpoint.vx, trajectory_setpoint.vy);
						Vector2f acceleration_sp_2d(trajectory_setpoint.acceleration[0], trajectory_setpoint.acceleration[1]);
						Vector2f acceleration_normal = acceleration_sp_2d - acceleration_sp_2d.dot(velocity_sp_2d) *
									       velocity_sp_2d.normalized();
						_pos_sp_triplet.current.loiter_radius = velocity_sp_2d.norm() * velocity_sp_2d.norm() / acceleration_normal.norm();

					} else {
						_pos_sp_triplet.current.loiter_radius = NAN;
					}
				}

				if (!valid_setpoint) {
					mavlink_log_critical(&_mavlink_log_pub, "Invalid offboard setpoint\t");
					events::send(events::ID("fixedwing_position_control_invalid_offboard_sp"), events::Log::Error,
						     "Invalid offboard setpoint");

				} else {
					_pos_sp_triplet.current.valid = valid_setpoint;
				}
			}

		} else {
			if (_pos_sp_triplet_sub.update(&_pos_sp_triplet)) {
				// reset the altitude foh (first order hold) logic
				_min_current_sp_distance_xy = FLT_MAX;
			}
		}

		airspeed_poll();
		manual_control_setpoint_poll();
		vehicle_attitude_poll();
		vehicle_command_poll();
		vehicle_control_mode_poll();
		wind_poll();

		vehicle_air_data_s air_data;

		if (_vehicle_air_data_sub.update(&air_data)) {
			_air_density = PX4_ISFINITE(air_data.rho) ? air_data.rho : _air_density;
		}

		if (_vehicle_land_detected_sub.updated()) {
			vehicle_land_detected_s vehicle_land_detected;

			if (_vehicle_land_detected_sub.update(&vehicle_land_detected)) {
				_landed = vehicle_land_detected.landed;
			}
		}

		_vehicle_status_sub.update(&_vehicle_status);

		Vector2d curr_pos(_current_latitude, _current_longitude);
		Vector2f ground_speed(_local_pos.vx, _local_pos.vy);

		set_control_mode_current(_local_pos.timestamp, _pos_sp_triplet.current.valid);

		update_in_air_states(_local_pos.timestamp);

		// update lateral guidance timesteps for slewrates
		if (_param_fw_use_npfg.get()) {
			_npfg.setDt(control_interval);

		} else {
			_l1_control.set_dt(control_interval);
		}

		// restore nominal TECS parameters in case changed intermittently (e.g. in landing handling)
		_tecs.set_speed_weight(_param_fw_t_spdweight.get());
		_tecs.set_height_error_time_constant(_param_fw_t_h_error_tc.get());

		_att_sp.roll_reset_integral = false;
		_att_sp.pitch_reset_integral = false;
		_att_sp.yaw_reset_integral = false;

		// by default we don't want yaw to be contoller directly with rudder
		_att_sp.fw_control_yaw = false;

		switch (_control_mode_current) {
		case FW_POSCTRL_MODE_AUTO: {
				control_auto(control_interval, curr_pos, ground_speed, _pos_sp_triplet.previous, _pos_sp_triplet.current,
					     _pos_sp_triplet.next);
				break;
			}

		case FW_POSCTRL_MODE_AUTO_ALTITUDE: {
				control_auto_fixed_bank_alt_hold(control_interval);
				break;
			}

		case FW_POSCTRL_MODE_AUTO_CLIMBRATE: {
				control_auto_descend(control_interval);
				break;
			}

		case FW_POSCTRL_MODE_AUTO_LANDING: {
				control_auto_landing(_local_pos.timestamp, control_interval, curr_pos, ground_speed, _pos_sp_triplet.previous,
						     _pos_sp_triplet.current);
				break;
			}

		case FW_POSCTRL_MODE_AUTO_TAKEOFF: {
				control_auto_takeoff(_local_pos.timestamp, control_interval, curr_pos, ground_speed, _pos_sp_triplet.previous,
						     _pos_sp_triplet.current);
				break;
			}

		case FW_POSCTRL_MODE_MANUAL_POSITION: {
				control_manual_position(control_interval, curr_pos, ground_speed);
				break;
			}

		case FW_POSCTRL_MODE_MANUAL_ALTITUDE: {
				control_manual_altitude(control_interval, curr_pos, ground_speed);
				break;
			}

		case FW_POSCTRL_MODE_OTHER: {
				/* reset landing and takeoff state */
				if (!_last_manual) {
					reset_landing_state();
					reset_takeoff_state();
				}

				_att_sp.thrust_body[0] = min(_att_sp.thrust_body[0], _param_fw_thr_max.get());

				_att_sp.apply_flaps = vehicle_attitude_setpoint_s::FLAPS_OFF;
				_att_sp.apply_spoilers = vehicle_attitude_setpoint_s::SPOILERS_OFF;

				_tecs.reset_state();

				break;
			}

		}

		if (_control_mode_current != FW_POSCTRL_MODE_AUTO_LANDING) {
			reset_landing_state();
		}

		if (_control_mode_current != FW_POSCTRL_MODE_AUTO_TAKEOFF) {
			reset_takeoff_state();
		}

		if (_control_mode_current != FW_POSCTRL_MODE_OTHER) {

			if (_control_mode.flag_control_manual_enabled) {
				_att_sp.roll_body = constrain(_att_sp.roll_body, -radians(_param_fw_r_lim.get()),
							      radians(_param_fw_r_lim.get()));
				_att_sp.pitch_body = constrain(_att_sp.pitch_body, radians(_param_fw_p_lim_min.get()),
							       radians(_param_fw_p_lim_max.get()));
			}

			if (_control_mode.flag_control_position_enabled ||
			    _control_mode.flag_control_velocity_enabled ||
			    _control_mode.flag_control_acceleration_enabled ||
			    _control_mode.flag_control_altitude_enabled ||
			    _control_mode.flag_control_climb_rate_enabled) {

				const Quatf q(Eulerf(_att_sp.roll_body, _att_sp.pitch_body, _att_sp.yaw_body));
				q.copyTo(_att_sp.q_d);

				_att_sp.timestamp = hrt_absolute_time();
				_attitude_sp_pub.publish(_att_sp);

				// only publish status in full FW mode
				if (_vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_FIXED_WING
				    || _vehicle_status.in_transition_mode) {
					status_publish();

				}
			}

			_l1_control.reset_has_guidance_updated();
			_last_manual = !_control_mode.flag_control_position_enabled;
		}

		perf_end(_loop_perf);
	}
}

void
FixedwingPositionControl::reset_takeoff_state(bool force)
{
	// only reset takeoff if !armed or just landed
	if (!_control_mode.flag_armed || (_was_in_air && _landed) || force) {

		_runway_takeoff.reset();

		_launchDetector.reset();
		_launch_detection_state = LAUNCHDETECTION_RES_NONE;
		_launch_detection_notify = 0;

	} else {
		_launch_detection_state = LAUNCHDETECTION_RES_DETECTED_ENABLEMOTORS;
	}
}

void
FixedwingPositionControl::reset_landing_state()
{
	_time_started_landing = 0;

	// reset terrain estimation relevant values
	_last_time_terrain_alt_was_valid = 0;

	_land_noreturn_horizontal = false;
	_land_noreturn_vertical = false;
	_land_stayonground = false;
	_land_motor_lim = false;
	_land_onslope = false;

	// reset abort land, unless loitering after an abort
	if (_land_abort && (_pos_sp_triplet.current.type != position_setpoint_s::SETPOINT_TYPE_LOITER)) {

		abort_landing(false);
	}
}

bool FixedwingPositionControl::using_npfg_with_wind_estimate() const
{
	// high wind mitigation is handled by NPFG if the wind estimate is valid
	return _param_fw_use_npfg.get() && _wind_valid;
}

Vector2f
FixedwingPositionControl::get_nav_speed_2d(const Vector2f &ground_speed)
{

	Vector2f nav_speed_2d{ground_speed};

	if (_airspeed_valid && !using_npfg_with_wind_estimate()) {
		// l1 navigation logic breaks down when wind speed exceeds max airspeed
		// compute 2D groundspeed from airspeed-heading projection
		const Vector2f air_speed_2d{_airspeed * cosf(_yaw), _airspeed * sinf(_yaw)};

		// angle between air_speed_2d and ground_speed
		const float air_gnd_angle = acosf((air_speed_2d * ground_speed) / (air_speed_2d.length() * ground_speed.length()));

		// if angle > 90 degrees or groundspeed is less than threshold, replace groundspeed with airspeed projection
		if ((fabsf(air_gnd_angle) > M_PI_2_F) || (ground_speed.length() < 3.0f)) {
			nav_speed_2d = air_speed_2d;
		}
	}

	return nav_speed_2d;
}

float FixedwingPositionControl::compensateTrimThrottleForDensityAndWeight(float throttle_trim, float throttle_min,
		float throttle_max)
{
	float weight_ratio = 1.0f;

	if (_param_weight_base.get() > FLT_EPSILON && _param_weight_gross.get() > FLT_EPSILON) {
		weight_ratio = math::constrain(_param_weight_gross.get() / _param_weight_base.get(), MIN_WEIGHT_RATIO,
					       MAX_WEIGHT_RATIO);
	}

	float air_density_throttle_scale = 1.0f;

	if (PX4_ISFINITE(_air_density)) {
		// scale throttle as a function of sqrt(rho0/rho)
		const float eas2tas = sqrtf(CONSTANTS_AIR_DENSITY_SEA_LEVEL_15C / _air_density);
		const float eas2tas_at_5000m_amsl = sqrtf(CONSTANTS_AIR_DENSITY_SEA_LEVEL_15C / AIR_DENSITY_STANDARD_ATMOS_5000_AMSL);
		air_density_throttle_scale = constrain(eas2tas, 1.f, eas2tas_at_5000m_amsl);
	}

	// compensate trim throttle for both weight and air density
	return math::constrain(throttle_trim * sqrtf(weight_ratio) * air_density_throttle_scale, throttle_min, throttle_max);
}

void
FixedwingPositionControl::tecs_update_pitch_throttle(const float control_interval, float alt_sp, float airspeed_sp,
		float pitch_min_rad, float pitch_max_rad,
		float throttle_min, float throttle_max,
		bool climbout_mode, float climbout_pitch_min_rad,
		bool disable_underspeed_detection, float hgt_rate_sp)
{
	// do not run TECS if we are not in air
	bool run_tecs = !_landed;

	// do not run TECS if vehicle is a VTOL and we are in rotary wing mode or in transition
	// (it should also not run during VTOL blending because airspeed is too low still)
	if (_vehicle_status.is_vtol) {
		if (_vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING || _vehicle_status.in_transition_mode) {
			run_tecs = false;
		}

		if (_vehicle_status.in_transition_mode) {
			// we're in transition
			_was_in_transition = true;

			// set this to transition airspeed to init tecs correctly
			if (_param_fw_arsp_mode.get() == 1 && PX4_ISFINITE(_param_airspeed_trans)) {
				// some vtols fly without airspeed sensor
				_airspeed_after_transition = _param_airspeed_trans;

			} else {
				_airspeed_after_transition = _airspeed;
			}

			_airspeed_after_transition = constrain(_airspeed_after_transition, _param_fw_airspd_min.get(),
							       _param_fw_airspd_max.get());

		} else if (_was_in_transition) {
			// after transition we ramp up desired airspeed from the speed we had coming out of the transition
			_airspeed_after_transition += control_interval * 2.0f; // increase 2m/s

			if (_airspeed_after_transition < airspeed_sp && _airspeed < airspeed_sp) {
				airspeed_sp = max(_airspeed_after_transition, _airspeed);

			} else {
				_was_in_transition = false;
				_airspeed_after_transition = 0.0f;
			}
		}
	}

	_is_tecs_running = run_tecs;

	if (!run_tecs) {
		// next time we run TECS we should reinitialize states
		_reinitialize_tecs = true;
		return;
	}

	if (_reinitialize_tecs) {
		_tecs.reset_state();
		_reinitialize_tecs = false;
	}

	/* No underspeed protection in landing mode */
	_tecs.set_detect_underspeed_enabled(!disable_underspeed_detection);

	/* tell TECS to update its state, but let it know when it cannot actually control the plane */
	bool in_air_alt_control = (!_landed &&
				   (_control_mode.flag_control_auto_enabled ||
				    _control_mode.flag_control_velocity_enabled ||
				    _control_mode.flag_control_altitude_enabled ||
				    _control_mode.flag_control_climb_rate_enabled));

	/* update TECS vehicle state estimates */
	_tecs.update_vehicle_state_estimates(_airspeed, _body_acceleration(0), (_local_pos.timestamp > 0), in_air_alt_control,
					     _current_altitude, _local_pos.vz);

	const float throttle_trim_comp = compensateTrimThrottleForDensityAndWeight(_param_fw_thr_trim.get(), throttle_min,
					 throttle_max);

	_tecs.update_pitch_throttle(_pitch - radians(_param_fw_psp_off.get()),
				    _current_altitude,
				    alt_sp,
				    airspeed_sp,
				    _airspeed,
				    _eas2tas,
				    climbout_mode,
				    climbout_pitch_min_rad - radians(_param_fw_psp_off.get()),
				    throttle_min,
				    throttle_max,
				    throttle_trim_comp,
				    pitch_min_rad - radians(_param_fw_psp_off.get()),
				    pitch_max_rad - radians(_param_fw_psp_off.get()),
				    _param_climbrate_target.get(),
				    _param_sinkrate_target.get(),
				    hgt_rate_sp);

	tecs_status_publish();
}

void FixedwingPositionControl::publishLocalPositionSetpoint(const position_setpoint_s &current_waypoint)
{
	if (_global_local_proj_ref.isInitialized()) {
		Vector2f current_setpoint = _global_local_proj_ref.project(current_waypoint.lat, current_waypoint.lon);
		vehicle_local_position_setpoint_s local_position_setpoint{};
		local_position_setpoint.timestamp = hrt_absolute_time();
		local_position_setpoint.x = current_setpoint(0);
		local_position_setpoint.y = current_setpoint(1);
		local_position_setpoint.z = _global_local_alt0 - current_waypoint.alt;
		local_position_setpoint.yaw = NAN;
		local_position_setpoint.yawspeed = NAN;
		local_position_setpoint.vx = NAN;
		local_position_setpoint.vy = NAN;
		local_position_setpoint.vz = NAN;
		local_position_setpoint.acceleration[0] = NAN;
		local_position_setpoint.acceleration[1] = NAN;
		local_position_setpoint.acceleration[2] = NAN;
		local_position_setpoint.thrust[0] = _att_sp.thrust_body[0];
		local_position_setpoint.thrust[1] = _att_sp.thrust_body[1];
		local_position_setpoint.thrust[2] = _att_sp.thrust_body[2];
		_local_pos_sp_pub.publish(local_position_setpoint);
	}
}

void FixedwingPositionControl::publishOrbitStatus(const position_setpoint_s pos_sp)
{
	orbit_status_s orbit_status{};
	orbit_status.timestamp = hrt_absolute_time();
	float loiter_radius = pos_sp.loiter_radius;
	uint8_t loiter_direction = pos_sp.loiter_direction;

	if (fabsf(loiter_radius) < FLT_EPSILON) {
		loiter_radius = _param_nav_loiter_rad.get();
		loiter_direction = signNoZero(loiter_radius);
	}

	orbit_status.radius = static_cast<float>(loiter_direction) * loiter_radius;
	orbit_status.frame = 0; // MAV_FRAME::MAV_FRAME_GLOBAL
	orbit_status.x = static_cast<double>(pos_sp.lat);
	orbit_status.y = static_cast<double>(pos_sp.lon);
	orbit_status.z = pos_sp.alt;
	orbit_status.yaw_behaviour = orbit_status_s::ORBIT_YAW_BEHAVIOUR_HOLD_FRONT_TANGENT_TO_CIRCLE;
	_orbit_status_pub.publish(orbit_status);
}

int FixedwingPositionControl::task_spawn(int argc, char *argv[])
{
	bool vtol = false;

	if (argc > 1) {
		if (strcmp(argv[1], "vtol") == 0) {
			vtol = true;
		}
	}

	FixedwingPositionControl *instance = new FixedwingPositionControl(vtol);

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int FixedwingPositionControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int FixedwingPositionControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
fw_pos_control_l1 is the fixed wing position controller.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("fw_pos_control_l1", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_ARG("vtol", "VTOL mode", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

extern "C" __EXPORT int fw_pos_control_l1_main(int argc, char *argv[])
{
	return FixedwingPositionControl::main(argc, argv);
}
