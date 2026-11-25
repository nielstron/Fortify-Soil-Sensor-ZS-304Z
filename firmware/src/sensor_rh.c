/*
 * rh.c
 *
 *  Created on: 05.04.2022
 *      Author: pvvx
 */
#include "tl_common.h"
#if (DEV_SERVICES & SERVICE_PLM)
#include "battery.h"
#include "sensor_th.h"
#include "sensor_rh.h"
#include "app_main.h"

#define DEF_ADC_DIODE	8810 // ADC value GPIO PWM = "1", Ubat: 2995 mV
#define DEF_ADC_RH_Z	820  // ADC value at 100%
#define DEF_RH_K		3100000 // (ADC value at 0% - DEF_ADC_HZ) * 1000

#ifndef PWM_CYCLE
#define PWM_CYCLE		18   // F=24000/PWM_CYCLE, 12->2MHz, 18->1.33MHz 48->500 кГц
#endif
#ifndef PWM_PULSE
#define PWM_PULSE		3	 // 8 MHz
#endif
#ifndef PROBE_SETTLE_DEF_MS
#define PROBE_SETTLE_US_DEF	128		// inherited ZG-303Z timing
#else
#define PROBE_SETTLE_US_DEF	(PROBE_SETTLE_DEF_MS * 1000)
#endif
#ifndef PROBE_SAMPLES_DEF
#define PROBE_SAMPLES_DEF	1
#endif

/* Wait between powering/exciting the probe and the ADC read, and ADC reads
 * averaged per phase. Set from the probe_settle_ms / probe_samples config
 * attributes before each measurement. */
u32 probe_settle_us = PROBE_SETTLE_US_DEF;
static u8 probe_samples = PROBE_SAMPLES_DEF;
/* PWM period/pulse in PWM clock ticks (= system clock); set from probe_pwm_khz/_duty */
static u16 pwm_cycle = PWM_CYCLE, pwm_pulse = PWM_PULSE;

#ifdef PROBE_PWM_KHZ_DEF
static void probe_pwm_from_cfg(void) {
	u32 khz = g_zcl_thermostatUICfgAttrs.probe_pwm_khz;
	u32 duty = g_zcl_thermostatUICfgAttrs.probe_pwm_duty;
	if(khz < PROBE_PWM_KHZ_MIN || khz > PROBE_PWM_KHZ_MAX)
		khz = PROBE_PWM_KHZ_DEF;
	if(duty < 1 || duty > 99)
		duty = PROBE_PWM_DUTY_DEF;
	u32 cycle = CLOCK_SYS_CLOCK_HZ / 1000 / khz;	// 24 MHz: 100 kHz -> 240
	u32 pulse = cycle * duty / 100;
	if(pulse < 1) pulse = 1;
	if(pulse >= cycle) pulse = cycle - 1;
	pwm_cycle = (u16)cycle;
	pwm_pulse = (u16)pulse;
}

/* Linear moisture between the two calibration points, in 0.01 %. Works for
 * either direction (stock Arteco: wetter = lower ADC). */
u16 probe_percent(u16 raw) {
	s32 dry = g_zcl_thermostatUICfgAttrs.probe_cal_dry;
	s32 wet = g_zcl_thermostatUICfgAttrs.probe_cal_wet;
	if(!dry || !wet || dry == wet)
		return 0xffff;	// ZCL "unknown": not calibrated
	s32 v = (dry - (s32)raw) * 10000 / (dry - wet);
	if(v < 0) v = 0;
	if(v > 10000) v = 10000;
	return (u16)v;
}
#endif

static u16 probe_adc(void) {
	u32 sum = 0;
	adc_channel_init(CHNL_RHI);
	for(int i = 0; i < probe_samples; i++)
		sum += get_adc_mv(1); // adc value x4
	return (u16)(sum / probe_samples);
}

const sensor_rh_coef_t rh_coef_def = {
		.k = DEF_RH_K,
		.z = DEF_ADC_RH_Z,
		.d = DEF_ADC_DIODE
};
sensor_rh_t sensor_rh;

static void start_pwm(void) {
	reg_clk_en0 |= FLD_CLK0_PWM_EN;
	reg_pwm_clk = 0; // = CLOCK_SYS_CLOCK_HZ 24Mhz
	pwm_set_mode(PWM_ID, PWM_NORMAL_MODE);
	pwm_set_phase(PWM_ID, 0);   //no phase at pwm beginning
	pwm_set_cycle_and_duty(PWM_ID, pwm_cycle, pwm_pulse);
	pwm_start(PWM_ID);
	gpio_set_output_en(GPIO_RHI, 0);
	gpio_set_func(GPIO_RHI, AS_GPIO);
	gpio_setup_up_down_resistor(GPIO_RHI, PM_PIN_UP_DOWN_FLOAT);
	gpio_set_func(PWM_PIN, AS_PWMx);
}


static void	discharge_c_rh(void) {
	gpio_setup_up_down_resistor(GPIO_RHI, PM_PIN_PULLDOWN_100K);
	gpio_write(PWM_PIN, 0);
	gpio_set_func(PWM_PIN, AS_GPIO);
	gpio_set_output_en(GPIO_RHI, 1);
}


static u32 get_rh(void) {

#ifdef GPIO_PROBE_PWR
	gpio_write(GPIO_PROBE_PWR, PROBE_PWR_ON); // power the probe front-end
#endif
	// discharge cap RHI
	discharge_c_rh();

	sleep_us(64);

	start_pwm();

	sleep_us(probe_settle_us);
	sensor_rh.adc_rh = probe_adc(); // measure rh, adc value x4

	pwm_stop(PWM_ID); // PMM Off

	// set pin PWM "1"
	gpio_set_func(PWM_PIN, AS_GPIO);
	gpio_write(PWM_PIN, 1);
	gpio_set_output_en(PWM_PIN, 1);

	sleep_us(128);
	// get ADC value RHI (+Vbat - Udiode)
	sensor_rh.adc_d = probe_adc();  // measure rh, adc value x4

	// set PWM OUT "0"
	gpio_write(PWM_PIN, 0);
	discharge_c_rh();
#ifdef GPIO_PROBE_PWR
	gpio_write(GPIO_PROBE_PWR, !PROBE_PWR_ON); // front-end off between samples
#endif

#if BOARD == BOARD_ARTECO_ZS304Z
    // Two-point calibration from config (0x012D/0x012E) on the raw reading;
    // 0xffff (ZCL unknown) until both points are set. Raw values are published
    // independently. No supply-voltage compensation (not characterised yet).
    return probe_percent(sensor_rh.adc_rh);
#else
    u32 k, z, val;
	k = sensor_rh.coef.d << 16;
	k /= sensor_rh.adc_d;
	z = sensor_rh.coef.z << 16;
	z /= k;
	val = sensor_rh.adc_rh * k;
	val >>= 16;
	val -= z;
	val = sensor_rh.coef.k / val;
	if(val >= 11000)
		return 10000; // 100.00%
	if(val <= 1000)
		return 0; // 0.00%
	return val - 1000;
#endif
}


#ifdef USE_AVERAGE_RH_SHL
void clr_rh_summ(void) {
	sensor_rh.summ_rh = 0;
	sensor_rh.cnt_summ = 0;
}
#else
#define clr_rh_summ()
#endif

int calibrate_rh_0(void) {
	// @TODO
	clr_rh_summ();
	get_rh(); // save final RH ADC measurement
	return 1;
}

int calibrate_rh_100(void) {
	// @TODO
	clr_rh_summ();
	get_rh(); // save final RH ADC measurement
	return 1;
}

void init_rh_sensor(void) {
	memcpy(&sensor_rh.coef, &rh_coef_def, sizeof(sensor_rh.coef));
	clr_rh_summ();
}

#ifdef PROBE_INTERVAL_DEF_S
extern u32 rtcSeconds;
static u32 probe_last_sec;
static u8 probe_measured;

/* Called on every sensor wake (measurement interval); the probe itself only
 * runs every probe_interval_s, since each reading keeps the chip awake for
 * the settle time. */
int read_rh_sensor(void) {
	u32 iv = g_zcl_thermostatUICfgAttrs.probe_interval_s;
	if(iv < PROBE_INTERVAL_MIN_S)
		iv = PROBE_INTERVAL_DEF_S;
	if(probe_measured && rtcSeconds - probe_last_sec < iv)
		return 1;
	return read_rh_sensor_now();
}

/* ADC samples and PWM from config (everything except the settle time). */
static void probe_cfg_load(void) {
	u8 n = g_zcl_thermostatUICfgAttrs.probe_samples;
	probe_samples = (n < 1 || n > PROBE_SAMPLES_MAX) ? PROBE_SAMPLES_DEF : n;
	probe_pwm_from_cfg();
}

/* One probe measurement with an explicit settle time (sweeps). Leaves
 * sensor_rh.adc_rh/adc_d set; does not touch the reported attributes. */
u16 probe_measure_raw(u32 settle_ms) {
	probe_cfg_load();
	probe_settle_us = settle_ms * 1000;
	get_rh();
	return sensor_rh.adc_rh;
}

int	read_rh_sensor_now(void) {
	u32 settle_ms = g_zcl_thermostatUICfgAttrs.probe_settle_ms;
	if(settle_ms < 1 || settle_ms > PROBE_SETTLE_MAX_MS)
		settle_ms = PROBE_SETTLE_DEF_MS;
	probe_settle_us = settle_ms * 1000;
	probe_cfg_load();
	probe_last_sec = rtcSeconds;
	probe_measured = 1;
#else
int	read_rh_sensor(void) {
#endif
	u32 adc_uint = get_rh();
#ifdef USE_AVERAGE_RH_SHL
	// Calculate the average value from (1<<USE_AVERAGE_TH_SHL) measurements
	sensor_rh.summ_rh += adc_uint;
	if(++sensor_rh.cnt_summ >= (1<<USE_AVERAGE_RH_SHL)) {
		adc_uint = sensor_rh.summ_rh >> USE_AVERAGE_RH_SHL;
		sensor_rh.summ_rh -= adc_uint;
		sensor_rh.cnt_summ--;
	} else {
		adc_uint = sensor_rh.summ_rh / sensor_rh.cnt_summ;
	}
#endif
	sensor_rh.rh = (u16)adc_uint;
	g_zcl_MoistureAttrs.measuredValue = sensor_rh.rh;
	g_zcl_MoistureAttrs.raw_adc = sensor_rh.adc_rh;   // raw soil ADC (x4)
	g_zcl_MoistureAttrs.adc_diode = sensor_rh.adc_d;  // diode/Vbat reference (x4)
	return 1;
}

#endif // (DEV_SERVICES & SERVICE_PLM)
