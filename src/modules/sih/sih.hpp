/****************************************************************************
*
*   Copyright (c) 2019-2022 PX4 Development Team. All rights reserved.
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

/**
 * @file sih.hpp
 * Simulator in Hardware
 *
 * @author Romain Chiappinelli      <romain.chiap@gmail.com>
 *
 * Coriolis g Corporation - January 2019
 */

// The sensor signals reconstruction and noise levels are from [1]
// [1] Bulka E, and Nahon M, "Autonomous fixed-wing aerobatics: from theory to flight."
//     In 2018 IEEE International Conference on Robotics and Automation (ICRA), pp. 6573-6580. IEEE, 2018.
// The aerodynamic model is from [2]
// [2] Khan W, supervised by Nahon M, "Dynamics modeling of agile fixed-wing unmanned aerial vehicles."
//     McGill University (Canada), PhD thesis, 2016.
// The quaternion integration are from [3]
// [3] Sveier A, Sjøberg AM, Egeland O. "Applied Runge–Kutta–Munthe-Kaas Integration for the Quaternion Kinematics."
//     Journal of Guidance, Control, and Dynamics. 2019 Dec;42(12):2747-54.
// The tailsitter model is from [4]
// [4] Chiappinelli R, supervised by Nahon M, "Modeling and control of a flying wing tailsitter unmanned aerial vehicle."
//     McGill University (Canada), Masters Thesis, 2018.

#pragma once

#include <px4_platform_common/module.h>
#include <px4_platform_common/module_params.h>
#include <px4_platform_common/posix.h>

#include <matrix/matrix/math.hpp>   // matrix, vectors, dcm, quaterions
#include <conversion/rotation.h>    // math::radians,
#include <lib/geo/geo.h>        // to get the physical constants
#include <drivers/drv_hrt.h>        // to get the real time
#include <lib/drivers/accelerometer/PX4Accelerometer.hpp>
#include <lib/drivers/gyroscope/PX4Gyroscope.hpp>
#include <lib/drivers/magnetometer/PX4Magnetometer.hpp>
#include <perf/perf_counter.h>
#include <uORB/Publication.hpp>
#include <uORB/Subscription.hpp>
#include <uORB/SubscriptionInterval.hpp>
#include <uORB/topics/parameter_update.h>
#include <uORB/topics/actuator_outputs.h>
#include <uORB/topics/sensor_gps.h>
#include <uORB/topics/sensor_baro.h>
#include <uORB/topics/vehicle_angular_velocity.h>   // to publish groundtruth
#include <uORB/topics/vehicle_attitude.h>           // to publish groundtruth
#include <uORB/topics/vehicle_global_position.h>    // to publish groundtruth
#include <uORB/topics/distance_sensor.h>
#include <uORB/topics/airspeed.h>

#if defined(ENABLE_LOCKSTEP_SCHEDULER)
#include <sys/time.h>
#endif

using namespace time_literals;

extern "C" __EXPORT int sih_main(int argc, char *argv[]);

class Sih : public ModuleBase<Sih>, public ModuleParams
{
public:
	Sih();

	virtual ~Sih();

	/** @see ModuleBase */
	static int task_spawn(int argc, char *argv[]);

	/** @see ModuleBase */
	static Sih *instantiate(int argc, char *argv[]);

	/** @see ModuleBase */
	static int custom_command(int argc, char *argv[]);

	/** @see ModuleBase::print_status() */
	int print_status() override;

	/** @see ModuleBase */
	static int print_usage(const char *reason = nullptr);

	/** @see ModuleBase::run() */
	void run() override;

	static float generate_wgn();    // generate white Gaussian noise sample

	// generate white Gaussian noise sample as a 3D vector with specified std
	static matrix::Vector3f noiseGauss3f(float stdx, float stdy, float stdz);

	// timer called periodically to post the semaphore
	static void timer_callback(void *sem);

private:
	void parameters_updated();

	// simulated sensor instances
	PX4Accelerometer _px4_accel{1310988}; // 1310988: DRV_IMU_DEVTYPE_SIM, BUS: 1, ADDR: 1, TYPE: SIMULATION
	PX4Gyroscope     _px4_gyro{1310988};  // 1310988: DRV_IMU_DEVTYPE_SIM, BUS: 1, ADDR: 1, TYPE: SIMULATION
	PX4Magnetometer  _px4_mag{197388};    //  197388: DRV_MAG_DEVTYPE_MAGSIM, BUS: 3, ADDR: 1, TYPE: SIMULATION

	uORB::PublicationMulti<sensor_baro_s> _sensor_baro_pub{ORB_ID(sensor_baro)};

	// to publish the gps position
	sensor_gps_s			_sensor_gps{};
	uORB::Publication<sensor_gps_s>	_sensor_gps_pub{ORB_ID(sensor_gps)};

	// to publish the distance sensor
	distance_sensor_s                    _distance_snsr{};
	uORB::Publication<distance_sensor_s> _distance_snsr_pub{ORB_ID(distance_sensor)};

	// angular velocity groundtruth
	vehicle_angular_velocity_s			_vehicle_angular_velocity_gt{};
	uORB::Publication<vehicle_angular_velocity_s>	_vehicle_angular_velocity_gt_pub{ORB_ID(vehicle_angular_velocity_groundtruth)};

	// attitude groundtruth
	vehicle_attitude_s				_att_gt{};
	uORB::Publication<vehicle_attitude_s>		_att_gt_pub{ORB_ID(vehicle_attitude_groundtruth)};

	// global position groundtruth
	vehicle_global_position_s			_gpos_gt{};
	uORB::Publication<vehicle_global_position_s>	_gpos_gt_pub{ORB_ID(vehicle_global_position_groundtruth)};

	// airspeed
	uORB::Publication<airspeed_s>				_airspeed_pub{ORB_ID(airspeed)};

	uORB::SubscriptionInterval _parameter_update_sub{ORB_ID(parameter_update), 1_s};
	uORB::Subscription _actuator_out_sub{ORB_ID(actuator_outputs)};

	// hard constants
	static constexpr uint16_t NB_MOTORS = 6;
	static constexpr float T1_C = 15.0f;                        // ground temperature in celcius
	static constexpr float T1_K = T1_C - CONSTANTS_ABSOLUTE_NULL_CELSIUS;   // ground temperature in Kelvin
	static constexpr float TEMP_GRADIENT  = -6.5f / 1000.0f;    // temperature gradient in degrees per metre
	// Aerodynamic coefficients
	static constexpr float RHO = 1.225f; 		// air density at sea level [kg/m^3]
	static constexpr float SPAN = 0.86f; 	// wing span [m]
	static constexpr float MAC = 0.21f; 	// wing mean aerodynamic chord [m]
	static constexpr float RP = 0.1f; 	// radius of the propeller [m]
	static constexpr float FLAP_MAX = M_PI_F / 12.0f; // 15 deg, maximum control surface deflection

	void init_variables();
	void gps_fix();
	void gps_no_fix();
	void read_motors();
	void generate_force_and_torques();
	void equations_of_motion();
	void reconstruct_sensors_signals();
	void send_gps();
	void send_airspeed();
	void send_dist_snsr();
	void publish_sih();
	void generate_fw_aerodynamics();
	void generate_ts_aerodynamics();
	void sensor_step();

#if defined(ENABLE_LOCKSTEP_SCHEDULER)
	void lockstep_loop();
	uint64_t _current_simulation_time_us{0};
	float _achieved_speedup{0.f};
#endif

	void realtime_loop();
	px4_sem_t       _data_semaphore;
	hrt_call 	_timer_call;

	perf_counter_t  _loop_perf{perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")};
	perf_counter_t  _loop_interval_perf{perf_alloc(PC_INTERVAL, MODULE_NAME": cycle interval")};

	hrt_abstime _last_run{0};
	hrt_abstime _last_actuator_output_time{0};
	hrt_abstime _baro_time{0};
	hrt_abstime _gps_time{0};
	hrt_abstime _airspeed_time{0};
	hrt_abstime _mag_time{0};
	hrt_abstime _gt_time{0};
	hrt_abstime _dist_snsr_time{0};
	hrt_abstime _now{0};
	float       _dt{0};         // sampling time [s]
	bool        _grounded{true};// whether the vehicle is on the ground

	matrix::Vector3f    _T_B;           // thrust force in body frame [N]
	matrix::Vector3f    _Fa_I;          // aerodynamic force in inertial frame [N]
	matrix::Vector3f    _Mt_B;          // thruster moments in the body frame [Nm]
	matrix::Vector3f    _Ma_B;          // aerodynamic moments in the body frame [Nm]
	matrix::Vector3f    _p_I;           // inertial position [m]
	matrix::Vector3f    _v_I;           // inertial velocity [m/s]
	matrix::Vector3f    _v_B;           // body frame velocity [m/s]
	matrix::Vector3f    _p_I_dot;       // inertial position differential
	matrix::Vector3f    _v_I_dot;       // inertial velocity differential
	matrix::Quatf       _q;             // quaternion attitude
	matrix::Dcmf        _C_IB;          // body to inertial transformation
	matrix::Vector3f    _w_B;           // body rates in body frame [rad/s]
	matrix::Quatf       _dq;            // quaternion differential
	matrix::Vector3f    _w_B_dot;       // body rates differential
	float       _u[NB_MOTORS];          // thruster signals

	enum class VehicleType {MC, FW, TS};
	VehicleType _vehicle = VehicleType::MC;

	// aerodynamic segments for the fixedwing
	AeroSeg _wing_l = AeroSeg(SPAN / 2.0f, MAC, -4.0f, matrix::Vector3f(0.0f, -SPAN / 4.0f, 0.0f), 3.0f,
				  SPAN / MAC, MAC / 3.0f);
	AeroSeg _wing_r = AeroSeg(SPAN / 2.0f, MAC, -4.0f, matrix::Vector3f(0.0f, SPAN / 4.0f, 0.0f), -3.0f,
				  SPAN / MAC, MAC / 3.0f);
	AeroSeg _tailplane = AeroSeg(0.3f, 0.1f, 0.0f, matrix::Vector3f(-0.4f, 0.0f, 0.0f), 0.0f, -1.0f, 0.05f, RP);
	AeroSeg _fin = AeroSeg(0.25, 0.18, 0.0f, matrix::Vector3f(-0.45f, 0.0f, -0.1f), -90.0f, -1.0f, 0.12f, RP);
	AeroSeg _fuselage = AeroSeg(0.2, 0.8, 0.0f, matrix::Vector3f(0.0f, 0.0f, 0.0f), -90.0f);

	// aerodynamic segments for the tailsitter
	static constexpr const int NB_TS_SEG = 11;
	static constexpr const float TS_AR = 3.13f;
	static constexpr const float TS_CM = 0.115f;	// longitudinal position of the CM from trailing edge
	static constexpr const float TS_RP = 0.0625f;	// propeller radius [m]
	static constexpr const float TS_DEF_MAX = math::radians(39.0f); 	// max deflection
	matrix::Dcmf _C_BS = matrix::Dcmf(matrix::Eulerf(0.0f, math::radians(90.0f), 0.0f)); // segment to body 90 deg pitch
	AeroSeg _ts[NB_TS_SEG] = {
		AeroSeg(0.0225f, 0.110f, 0.0f, matrix::Vector3f(0.083f - TS_CM, -0.239f, 0.0f), 0.0f, TS_AR),
		AeroSeg(0.0383f, 0.125f, 0.0f, matrix::Vector3f(0.094f - TS_CM, -0.208f, 0.0f), 0.0f, TS_AR, 0.063f),
		// AeroSeg(0.0884f, 0.148f, 0.0f, matrix::Vector3f(0.111f-TS_CM, -0.143f, 0.0f), 0.0f, TS_AR, 0.063f, TS_RP),
		AeroSeg(0.0884f, 0.085f, 0.0f, matrix::Vector3f(0.158f - TS_CM, -0.143f, 0.0f), 0.0f, TS_AR),
		AeroSeg(0.0884f, 0.063f, 0.0f, matrix::Vector3f(0.047f - TS_CM, -0.143f, 0.0f), 0.0f, TS_AR, 0.063f, TS_RP),
		AeroSeg(0.0633f, 0.176f, 0.0f, matrix::Vector3f(0.132f - TS_CM, -0.068f, 0.0f), 0.0f, TS_AR, 0.063f),
		AeroSeg(0.0750f, 0.231f, 0.0f, matrix::Vector3f(0.173f - TS_CM,  0.000f, 0.0f), 0.0f, TS_AR),
		AeroSeg(0.0633f, 0.176f, 0.0f, matrix::Vector3f(0.132f - TS_CM,  0.068f, 0.0f), 0.0f, TS_AR, 0.063f),
		// AeroSeg(0.0884f, 0.148f, 0.0f, matrix::Vector3f(0.111f-TS_CM,  0.143f, 0.0f), 0.0f, TS_AR, 0.063f, TS_RP),
		AeroSeg(0.0884f, 0.085f, 0.0f, matrix::Vector3f(0.158f - TS_CM,  0.143f, 0.0f), 0.0f, TS_AR),
		AeroSeg(0.0884f, 0.063f, 0.0f, matrix::Vector3f(0.047f - TS_CM,  0.143f, 0.0f), 0.0f, TS_AR, 0.063f, TS_RP),
		AeroSeg(0.0383f, 0.125f, 0.0f, matrix::Vector3f(0.094f - TS_CM,  0.208f, 0.0f), 0.0f, TS_AR, 0.063f),
		AeroSeg(0.0225f, 0.110f, 0.0f, matrix::Vector3f(0.083f - TS_CM,  0.239f, 0.0f), 0.0f, TS_AR)
	};

	// AeroSeg _ts[NB_TS_SEG] = {
	// 	AeroSeg(0.0225f, 0.110f, -90.0f, matrix::Vector3f(0.0f, -0.239f, TS_CM-0.083f), 0.0f, TS_AR),
	// 	AeroSeg(0.0383f, 0.125f, -90.0f, matrix::Vector3f(0.0f, -0.208f, TS_CM-0.094f), 0.0f, TS_AR, 0.063f),
	// 	AeroSeg(0.0884f, 0.148f, -90.0f, matrix::Vector3f(0.0f, -0.143f, TS_CM-0.111f), 0.0f, TS_AR, 0.063f, TS_RP),
	// 	AeroSeg(0.0633f, 0.176f, -90.0f, matrix::Vector3f(0.0f, -0.068f, TS_CM-0.132f), 0.0f, TS_AR, 0.063f),
	// 	AeroSeg(0.0750f, 0.231f, -90.0f, matrix::Vector3f(0.0f,  0.000f, TS_CM-0.173f), 0.0f, TS_AR),
	// 	AeroSeg(0.0633f, 0.176f, -90.0f, matrix::Vector3f(0.0f,  0.068f, TS_CM-0.132f), 0.0f, TS_AR, 0.063f),
	// 	AeroSeg(0.0884f, 0.148f, -90.0f, matrix::Vector3f(0.0f,  0.143f, TS_CM-0.111f), 0.0f, TS_AR, 0.063f, TS_RP),
	// 	AeroSeg(0.0383f, 0.125f, -90.0f, matrix::Vector3f(0.0f,  0.208f, TS_CM-0.094f), 0.0f, TS_AR, 0.063f),
	// 	AeroSeg(0.0225f, 0.110f, -90.0f, matrix::Vector3f(0.0f,  0.239f, TS_CM-0.083f), 0.0f, TS_AR)
	// 	};

	// sensors reconstruction
	matrix::Vector3f    _acc;
	matrix::Vector3f    _mag;
	matrix::Vector3f    _gyro;
	matrix::Vector3f    _gps_vel;
	double      _gps_lat, _gps_lat_noiseless;
	double      _gps_lon, _gps_lon_noiseless;
	float       _gps_alt, _gps_alt_noiseless;
	float       _baro_p_mBar;   // reconstructed (simulated) pressure in mBar
	float       _baro_temp_c;   // reconstructed (simulated) barometer temperature in celcius

	// parameters
	float _MASS, _T_MAX, _Q_MAX, _L_ROLL, _L_PITCH, _KDV, _KDW, _H0, _T_TAU;
	double _LAT0, _LON0, _COS_LAT0;
	matrix::Vector3f _W_I;  // weight of the vehicle in inertial frame [N]
	matrix::Matrix3f _I;    // vehicle inertia matrix
	matrix::Matrix3f _Im1;  // inverse of the intertia matrix
	matrix::Vector3f _mu_I; // NED magnetic field in inertial frame [G]

	int _gps_used;
	float _baro_offset_m, _mag_offset_x, _mag_offset_y, _mag_offset_z;
	float _distance_snsr_min, _distance_snsr_max, _distance_snsr_override;

	// parameters defined in sih_params.c
	DEFINE_PARAMETERS(
		(ParamInt<px4::params::IMU_GYRO_RATEMAX>) _imu_gyro_ratemax,
		(ParamInt<px4::params::IMU_INTEG_RATE>) _imu_integration_rate,
		(ParamFloat<px4::params::SIH_MASS>) _sih_mass,
		(ParamFloat<px4::params::SIH_IXX>) _sih_ixx,
		(ParamFloat<px4::params::SIH_IYY>) _sih_iyy,
		(ParamFloat<px4::params::SIH_IZZ>) _sih_izz,
		(ParamFloat<px4::params::SIH_IXY>) _sih_ixy,
		(ParamFloat<px4::params::SIH_IXZ>) _sih_ixz,
		(ParamFloat<px4::params::SIH_IYZ>) _sih_iyz,
		(ParamFloat<px4::params::SIH_T_MAX>) _sih_t_max,
		(ParamFloat<px4::params::SIH_Q_MAX>) _sih_q_max,
		(ParamFloat<px4::params::SIH_L_ROLL>) _sih_l_roll,
		(ParamFloat<px4::params::SIH_L_PITCH>) _sih_l_pitch,
		(ParamFloat<px4::params::SIH_KDV>) _sih_kdv,
		(ParamFloat<px4::params::SIH_KDW>) _sih_kdw,
		(ParamInt<px4::params::SIH_LOC_LAT0>) _sih_lat0,
		(ParamInt<px4::params::SIH_LOC_LON0>) _sih_lon0,
		(ParamFloat<px4::params::SIH_LOC_H0>) _sih_h0,
		(ParamFloat<px4::params::SIH_LOC_MU_X>) _sih_mu_x,
		(ParamFloat<px4::params::SIH_LOC_MU_Y>) _sih_mu_y,
		(ParamFloat<px4::params::SIH_LOC_MU_Z>) _sih_mu_z,
		(ParamInt<px4::params::SIH_GPS_USED>) _sih_gps_used,
		(ParamFloat<px4::params::SIH_BARO_OFFSET>) _sih_baro_offset,
		(ParamFloat<px4::params::SIH_MAG_OFFSET_X>) _sih_mag_offset_x,
		(ParamFloat<px4::params::SIH_MAG_OFFSET_Y>) _sih_mag_offset_y,
		(ParamFloat<px4::params::SIH_MAG_OFFSET_Z>) _sih_mag_offset_z,
		(ParamFloat<px4::params::SIH_DISTSNSR_MIN>) _sih_distance_snsr_min,
		(ParamFloat<px4::params::SIH_DISTSNSR_MAX>) _sih_distance_snsr_max,
		(ParamFloat<px4::params::SIH_DISTSNSR_OVR>) _sih_distance_snsr_override,
		(ParamFloat<px4::params::SIH_T_TAU>) _sih_thrust_tau,
		(ParamInt<px4::params::SIH_VEHICLE_TYPE>) _sih_vtype,
		(ParamBool<px4::params::SYS_CTRL_ALLOC>) _sys_ctrl_alloc
	)
};
