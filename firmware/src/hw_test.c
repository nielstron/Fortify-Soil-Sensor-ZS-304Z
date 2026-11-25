/*
 * hw_test.c - Arteco ZS-304Z bench test (build with tools/build.py --hw-test).
 *
 * Never starts Zigbee. Prints one line per second via GPIO printf on the SWS pad:
 * LED blink (PD2), button level (PB6), probe ADC through the normal read path
 * (PD3 power + PWM + settle), light ADC (PB5), battery and SHTC3.
 * Holding the button keeps the probe supply (PD3) and the LED on for a multimeter check.
 * Each loop also sweeps the probe settle time (PD3 off 300 ms between points, as from
 * cold) and prints "#n settle ms:probe:ref ..." to find the shortest stable settle time.
 */
#include "tl_common.h"
#if defined(ARTECO_HW_TEST)
#include "battery.h"
#include "sensor_th.h"
#include "sensor_rh.h"
#include "app_main.h"

static u16 read_light_mv(void) {
	gpio_write(GPIO_PROBE_PWR, PROBE_PWR_ON);
	gpio_set_output_en(GPIO_LIGHT, 0);
	gpio_set_input_en(GPIO_LIGHT, 1);
	gpio_set_func(GPIO_LIGHT, AS_ADC);
	sleep_us(10000); // stock waits 10 ms after switching to the light channel
	adc_channel_init(CHNL_LIGHT);
	u16 mv = get_adc_mv(0);
	gpio_set_func(GPIO_LIGHT, AS_GPIO);
	gpio_write(GPIO_PROBE_PWR, !PROBE_PWR_ON);
	return mv;
}

static const u16 sweep_ms[] = {1, 5, 20, 50, 100, 300};
#define SWEEP_N (sizeof(sweep_ms) / sizeof(sweep_ms[0]))

/* Probe at each settle time, from cold; leaves sensor_rh at the 300 ms reading. */
static void settle_sweep(u32 n) {
	u16 probe[SWEEP_N], ref[SWEEP_N];
	for (unsigned i = 0; i < SWEEP_N; i++) {
		sleep_us(300000); // PD3 off since the previous reading
		probe[i] = probe_measure_raw(sweep_ms[i]);
		ref[i] = sensor_rh.adc_d;
	}
	sws_printf("#%d settle", n);
	for (unsigned i = 0; i < SWEEP_N; i++)
		sws_printf(" %d:%d:%d", sweep_ms[i], probe[i], ref[i]);
	sws_printf("\n");
}

void hw_test_run(void) {
	sws_init();
	sws_printf("\n\nZS-304Z hw-test: LED=PD2 BTN=PB6 PWR=PD3 PWM=PC4 ADC=PB4 LIGHT=PB5\n");
	init_i2c();
	init_sensor();
	sws_printf("TH sensor: i2c_addr=0x%02x type=%d id=0x%08x\n",
			sensor_ht.i2c_addr, sensor_ht.sensor_type, sensor_ht.id);
	u32 n = 0;
	while (1) {
		u32 t0 = clock_time();
		int btn = gpio_read(BUTTON1) ? 1 : 0;
		if (btn == BUTTON1_ON) {
			gpio_write(GPIO_PROBE_PWR, PROBE_PWR_ON);
			gpio_write(GPIO_LED, LED_ON);
			sws_printf("#%d button held: PD3 and LED on\n", n++);
			while (clock_time() - t0 < 1000 * CLOCK_16M_SYS_TIMER_CLK_1MS);
			continue;
		}
		gpio_write(GPIO_PROBE_PWR, !PROBE_PWR_ON);
		gpio_write(GPIO_LED, (n & 1) ? LED_ON : LED_OFF);

		read_sensor(); // battery + start TH measurement (probe is rate-limited there)
		if (sensor_ht.read_callback) {
			sleep_us(sensor_ht.measure_timeout_us);
			sensor_ht.read_callback();
		}
		settle_sweep(n); // ends with the 300 ms probe reading used below
		u16 light = read_light_mv();
		sws_printf("#%d btn=%d probe_adc=%d ref_adc=%d light_mv=%d vbat_mv=%d T=%d RH=%d\n",
				n++, btn, sensor_rh.adc_rh, sensor_rh.adc_d, light,
				measured_battery.mv, sensor_ht.temp, sensor_ht.humi);
		while (clock_time() - t0 < 1000 * CLOCK_16M_SYS_TIMER_CLK_1MS);
	}
}
#endif
