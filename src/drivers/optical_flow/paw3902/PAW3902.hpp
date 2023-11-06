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
 * @file PAW3902.hpp
 *
 * Driver for the PAW3902JF-TXQT: Optical Motion Tracking Chip
 */

#pragma once

#include "PixArt_PAW3902_Registers.hpp"

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/getopt.h>
#include <px4_platform_common/i2c_spi_buses.h>
#include <drivers/device/spi.h>
#include <conversion/rotation.h>
#include <lib/perf/perf_counter.h>
#include <drivers/drv_hrt.h>
#include <uORB/PublicationMulti.hpp>
#include <uORB/topics/sensor_optical_flow.h>

using namespace time_literals;
using namespace PixArt_PAW3902;

#define DIR_WRITE(a) ((a) | Bit7)
#define DIR_READ(a) ((a) & 0x7F)

class PAW3902 : public device::SPI, public I2CSPIDriver<PAW3902>
{
public:
	PAW3902(const I2CSPIDriverConfig &config);
	virtual ~PAW3902();

	static void print_usage();

	int init() override;

	void print_status() override;

	void RunImpl();

private:
	void exit_and_cleanup() override;

	int probe() override;

	void Reset();

	static int DataReadyInterruptCallback(int irq, void *context, void *arg);
	void DataReady();
	bool DataReadyInterruptConfigure();
	bool DataReadyInterruptDisable();

	uint8_t RegisterRead(uint8_t reg);
	void RegisterWrite(uint8_t reg, uint8_t data);

	void Configure();

	bool ChangeMode(Mode newMode, bool force = false);

	void ConfigureModeBright();
	void ConfigureModeLowLight();
	void ConfigureModeSuperLowLight();

	void EnableLed();

	uORB::PublicationMulti<sensor_optical_flow_s> _sensor_optical_flow_pub{ORB_ID(sensor_optical_flow)};

	perf_counter_t _cycle_perf{perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")};
	perf_counter_t _interval_perf{perf_alloc(PC_INTERVAL, MODULE_NAME": interval")};
	perf_counter_t _reset_perf{perf_alloc(PC_COUNT, MODULE_NAME": reset")};
	perf_counter_t _false_motion_perf{perf_alloc(PC_COUNT, MODULE_NAME": false motion report")};
	perf_counter_t _mode_change_bright_perf{perf_alloc(PC_COUNT, MODULE_NAME": mode change bright (0)")};
	perf_counter_t _mode_change_low_light_perf{perf_alloc(PC_COUNT, MODULE_NAME": mode change low light (1)")};
	perf_counter_t _mode_change_super_low_light_perf{perf_alloc(PC_COUNT, MODULE_NAME": mode change super low light (2)")};
	perf_counter_t _no_motion_interrupt_perf{nullptr};

	const spi_drdy_gpio_t _drdy_gpio;

	matrix::Dcmf _rotation;

	int _discard_reading{3};

	Mode _mode{Mode::LowLight};

	uint32_t _scheduled_interval_us{SAMPLE_INTERVAL_MODE_0};

	int _bright_to_low_counter{0};
	int _low_to_superlow_counter{0};
	int _low_to_bright_counter{0};
	int _superlow_to_low_counter{0};

	px4::atomic<hrt_abstime> _drdy_timestamp_sample{0};
	bool _data_ready_interrupt_enabled{false};

	hrt_abstime _last_write_time{0};
	hrt_abstime _last_read_time{0};

	// force reset if there hasn't been valid data for an extended period (sensor could be in a bad state)
	static constexpr hrt_abstime RESET_TIMEOUT_US = 3_s;

	hrt_abstime _last_good_data{0};
	hrt_abstime _last_reset{0};
};
