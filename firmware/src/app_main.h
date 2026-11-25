#ifndef _DEVICE_H_
#define _DEVICE_H_

#include "zcl_include.h"

/**********************************************************************
 * CONSTANT
 */
#define SENSOR_DEVICE_ENDPOINT  0x01
#define SENSOR_DEVICE_ENDPOINT2  0x02
#define SENSOR_DEVICE_ENDPOINT3  0x03
#define SENSOR_DEVICE_ENDPOINT4  0x04

/**********************************************************************
 * TYPEDEFS
 */
typedef struct{
	u8 keyType; /* ERTIFICATION_KEY or MASTER_KEY key for touch-link or distribute network
	 	 	 	 SS_UNIQUE_LINK_KEY or SS_GLOBAL_LINK_KEY for distribute network */
	u8 key[16];	/* the key used */
}app_linkKey_info_t;

typedef struct{
	u16 reportUpSec; // report add (sec)

	u8  keyPressed;	// gpio keyPressed
	u8  key_on_flag; // new keyPressed and flags (task_keys() used)

	u32 secTimeTik;	// used for calculate measure seconds
	u32 readSensorTime; // read sensor timer (tick)
	u32 keyPressedTime; // key pressed time (tick)
	u32 measureInterval; // measure step (tick)

	u16 ledOnTime;	// used blink
	u16 ledOffTime; // used blink

	u8 	oriSta;		// original state before blink
	u8 	sta;		// current state in blink

	u8 	times;		// blink times

	u8  reportFlg;

#if	!USE_BLE && USE_DISPLAY
	ev_timer_event_t *timerTaskEvt;
#endif
	ev_timer_event_t *timerLedEvt;
	ev_timer_event_t *timerKeyEvt;
	ev_timer_event_t *timerSteerEvt;
	ev_timer_event_t *timerRejoinBackoffEvt;
#ifdef ZCL_IDENTIFY
	ev_timer_event_t *timerIdentifyEvt;
#endif
	u16	rejoin_cnt; // startup counter
	app_linkKey_info_t tcLinkKey;
} app_ctx_t;

// g_sensorAppCtx.reportFlg:
#define FLG_CHECK_REPORT		0x01	// check report table
#define FLG_REPEAT_REPORT		0x02	// repeat report


/**
 *  @brief Defined for basic cluster attributes
 */
typedef struct{
	u8 	zclVersion;
	u8	appVersion;
	u8	stackVersion;
	u8	hwVersion;
	u8	manuName[ZCL_BASIC_MAX_LENGTH];
	u8	modelId[ZCL_BASIC_MAX_LENGTH];
	u8	swBuildId[ZCL_BASIC_MAX_LENGTH];
 	u8  dateCode[ZCL_BASIC_MAX_LENGTH];
	u8	powerSource;
	u8	deviceEnable;
}zcl_basicAttr_t;

/**
 *  @brief Defined for identify cluster attributes
 */
typedef struct{
	u16	identifyTime;
}zcl_identifyAttr_t;

/**
 *  @brief Defined for power configuration cluster attributes
 */
typedef struct{
#ifdef POWER_MAINS
	u16 mainsVoltage;
	u8  mainsFrequency;
#endif
	u8  batteryVoltage;      //0x20
	u8  batteryPercentage;   //0x21
}zcl_powerAttr_t;

/**
 *  @brief Defined for temperature cluster attributes
 */
typedef struct {
	s16 measuredValue;
	s16 minValue;
	s16 maxValue;
	u16 tolerance;
}zcl_temperatureAttr_t;

/**
 *  @brief Defined for relative humidity cluster attributes
 */
typedef struct {
	u16 measuredValue;
	u16 minValue;
	u16 maxValue;
	u16 tolerance;
}zcl_relHumidityAttr_t;

typedef struct {
	u16 measuredValue;
	u16 minValue;
	u16 maxValue;
	u16 tolerance;
	u16 raw_adc;    // raw soil-moisture ADC (x4), pre-curve
	u16 adc_diode;  // diode/Vbat reference ADC (x4) for battery-compensated calibration
}zcl_MoistureAttr_t;

/**
 *  @brief Defined for thermostat UI config cluster attributes
 */
typedef struct {
	s16 temp_offset;	// in 0.01 C
	s16 humi_offset;	// in 0.01 %
#if	USE_DISPLAY
	s16 temp_comfort_min; // in 0.01 C
	s16 temp_comfort_max; // in 0.01 C
	u16 humi_comfort_min; // in 0.01 %
	u16 humi_comfort_max; // in 0.01 %
	u8 TemperatureDisplayMode;
	u8 showSmiley;
	u8 display_off;
#endif
	u8 measureInterval;
	/* Adaptive TX power control (TPC) config — persisted via NV with the UI cfg */
	u8 txpwr_mode;       // 0 = fixed, 1 = adaptive
	u8 txpwr_min_dbm;    // adaptive lower bound (dBm)
	u8 txpwr_max_dbm;    // adaptive upper bound (dBm)
	u8 txpwr_fixed_dbm;  // level used when mode = fixed (dBm)
	/* Soil probe timing (attrs 0x0126/0x0127) — persisted via NV with the UI cfg */
	u16 probe_interval_s; // probe measured at most this often (s); T/RH keep measureInterval
	u16 probe_settle_ms;  // probe power + PWM on -> ADC read (ms)
	u16 probe_report_change; // raw 0xF000/0xF001 reportable change (ADC counts x4)
	u8  probe_samples;    // ADC reads averaged per probe phase (1..16)
	u8  probe_heartbeat_n; // unchanged probe values re-sent every N probe intervals
	u16 probe_pwm_khz;    // probe excitation frequency (kHz)
	u8  probe_pwm_duty;   // probe excitation duty (%)
	u16 probe_cal_dry;    // raw ADC (x4) at 0 % moisture; 0 = not calibrated
	u16 probe_cal_wet;    // raw ADC (x4) at 100 % moisture; 0 = not calibrated
}zcl_thermostatUICfgAttr_t;

/* Adaptive TX power control (implemented in app_main.c) */
extern u8 g_txpwr_step;
extern s8 g_txpwr_cur_dbm;
void tpc_apply(void);
void tpc_note_tx(u8 status);
void tpc_boost_max(void);
void sensorDevice_dataCnfCb(void *arg);

/* Debug mode (UI cfg attr 0x0125, boolean, RAM only): while on, measure the probe
 * every DEBUG_MODE_PERIOD_MS and report raw 0xF000/0xF001 to the coordinator;
 * switches itself off after DEBUG_MODE_SEC and reports the attribute as off. */
#define DEBUG_MODE_ATTRID		0x0125
#define DEBUG_MODE_PERIOD_MS	500
#define DEBUG_MODE_SEC			600
extern u8 g_debug_on;
void debug_mode_update(void);
status_t app_report_to_coordinator(u8 endpoint, u16 cluster_id, u16 attr_id);
int read_rh_sensor_now(void);	// sensor_rh.c: measure the probe regardless of probe_interval_s
u16 probe_measure_raw(u32 settle_ms);	// sensor_rh.c: one reading at an explicit settle time

/* Probe sweep: write 1 to UI cfg attr 0x0130 -> the probe is measured once at
 * each settle time below, from cold (PD3 off PROBE_SWEEP_GAP_MS before each),
 * and each raw result is reported on EP2 attr 0xF010 + i. */
#define PROBE_SWEEP_ATTRID		0x0130
#define PROBE_SWEEP_RESULT_ATTRID	0xF010
#define PROBE_SWEEP_N			7
#define PROBE_SWEEP_GAP_MS		1000
extern u8 g_probe_sweep_cmd;
extern u16 g_probe_sweep[PROBE_SWEEP_N];

#define PROBE_INTERVAL_ATTRID	0x0126
#define PROBE_SETTLE_ATTRID		0x0127
#define PROBE_REPORT_CHANGE_ATTRID	0x0128
#define PROBE_SAMPLES_ATTRID	0x0129
#define PROBE_HEARTBEAT_ATTRID	0x012A
#define PROBE_PWM_KHZ_ATTRID	0x012B
#define PROBE_PWM_DUTY_ATTRID	0x012C
#define PROBE_CAL_DRY_ATTRID	0x012D
#define PROBE_CAL_WET_ATTRID	0x012E
#define PROBE_CAL_CAPTURE_ATTRID	0x012F	// write 1 = dry, 2 = wet: store the current reading
#define PROBE_INTERVAL_MIN_S	10
#define PROBE_SETTLE_MAX_MS		1000
#define PROBE_SAMPLES_MAX		16
#define PROBE_HEARTBEAT_N_MAX	100
#define PROBE_PWM_KHZ_MIN		10
#define PROBE_PWM_KHZ_MAX		2000
extern u8 g_probe_cal_capture;
void probe_apply_reporting(void);	// app_main.c: threshold 0x0128 + heartbeat 0x012A into the reporting table
void probe_cfg_written(u16 attrID);	// app_main.c: side effects of a written probe config attribute
u16 probe_percent(u16 raw);			// sensor_rh.c: moisture in 0.01 % from the dry/wet points, 0xffff if uncalibrated

/* Parents hold frames for a sleepy child only ~7.68 s (Zigbee default), so the
 * device polls at least this often regardless of the measurement interval. */
#define LONG_POLL_MAX_SEC		7


/**
 *  @brief Defined for Occupancy cluster attributes
 */
typedef struct {
#if !USE_SENSOR_XBR818
	ev_timer_event_t *timerEvt;
#endif
	u16 delay;
	u8  occupancy;
	u8  sensor_type;
	u8  state;
	u8  thres;
	//	u16 oc2un_delay;
} zcl_occupancyAttr_t;

#if USE_SENSOR_XBR818
typedef struct {
	u16 delay;
	u8  thres;
} zcl_occupancy_save_t;
#endif

extern zcl_occupancyAttr_t zcl_occupAttr;

/**
 *  @brief  Defined for poll control cluster attributes
 */
typedef struct{
	u32	chkInInterval;
	u32	longPollInterval;
	u32	chkInIntervalMin;
	u32	longPollIntervalMin;
	u16	shortPollInterval;
	u16	fastPollTimeout;
	u16	fastPollTimeoutMax;
}zcl_pollCtrlAttr_t;

typedef struct _comfort_t {
	s16 t[2];
	u16 h[2];
} scomfort_t;


/**********************************************************************
 * GLOBAL VARIABLES
 */
extern app_ctx_t g_sensorAppCtx;

extern bdb_appCb_t g_zbDemoBdbCb;

extern bdb_commissionSetting_t g_bdbCommissionSetting;

extern const u8 SENSOR_DEVICE_CB_CLUSTER_NUM;
extern const zcl_specClusterInfo_t g_sensorDeviceClusterList[];
extern const af_simple_descriptor_t sensorDevice_simpleDesc;

/* Attributes */
extern zcl_basicAttr_t g_zcl_basicAttrs;
extern zcl_identifyAttr_t g_zcl_identifyAttrs;
extern zcl_powerAttr_t g_zcl_powerAttrs;
#ifdef ZCL_THERMOSTAT_UI_CFG
extern zcl_thermostatUICfgAttr_t g_zcl_thermostatUICfgAttrs;
extern const zcl_thermostatUICfgAttr_t g_zcl_thermostatUICfgDefault;
#endif
#ifdef ZCL_TEMPERATURE_MEASUREMENT
extern zcl_temperatureAttr_t g_zcl_temperatureAttrs;
#endif
#ifdef ZCL_RELATIVE_HUMIDITY_MEASUREMENT
extern zcl_relHumidityAttr_t g_zcl_relHumidityAttrs;
#endif
#if (DEV_SERVICES & SERVICE_PLM)
extern const u8 SENSOR_DEVICE_CB_CLUSTER_NUM2;
extern const zcl_specClusterInfo_t g_sensorDeviceClusterList2[];
extern const af_simple_descriptor_t sensorDevice_simpleDesc2;
extern zcl_MoistureAttr_t g_zcl_MoistureAttrs;
#endif

// extern zcl_iasZoneAttr_t g_zcl_iasZoneAttrs;
extern zcl_pollCtrlAttr_t g_zcl_pollCtrlAttrs;

#define zcl_iasZoneAttrGet()	&g_zcl_iasZoneAttrs
#define zcl_pollCtrlAttrGet()	&g_zcl_pollCtrlAttrs
#define zcl_onoffAttrGet()      &g_zcl_onOffAttrs

extern scomfort_t cmf;

/**********************************************************************
 * FUNCTIONS
 */
#if !USE_BLE
#define pm_wait_ms(t) cpu_stall_wakeup_by_timer0(t*(CLOCK_SYS_CLOCK_HZ/1000));
#define pm_wait_us(t) cpu_stall_wakeup_by_timer0(t*(CLOCK_SYS_CLOCK_HZ/1000000));
#else
#define pm_wait_ms(t) sleep_us((t)*1000);
#define pm_wait_us(t) sleep_us(t);
#endif

void init_nv_app(void);

void sensorDevice_zclProcessIncomingMsg(zclIncoming_t *pInHdlrMsg);

status_t sensorDevice_basicCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
status_t sensorDevice_identifyCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
status_t sensorDevice_iasZoneCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
status_t sensorDevice_powerCfgCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
status_t sensorDevice_pollCtrlCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
status_t sensorDevice_groupCb(zclIncomingAddrInfo_t *pAddrInfo, u8 cmdId, void *cmdPayload);
void sensorDevice_zclCheckInStart(void);
void set_PollRate(void);
status_t test_set_measure_longpoll_interval(u32 measureInterval);

void sensorDevice_leaveCnfHandler(nlme_leave_cnf_t *pLeaveCnf);
void sensorDevice_leaveIndHandler(nlme_leave_ind_t *pLeaveInd);
#if !USE_BLE
void sensorDevice_nwkStatusIndHandler(zdo_nwk_status_ind_t *pNwkStatusInd);
#endif
void sensorDevice_otaProcessMsgHandler(u8 evt, u8 status);

#if USE_BLE
void user_ble_init(bool isRetention);
extern u8  mac_public[6];
extern u8  mac_random_static[6];

void blc_initMacAddress(u32 flash_addr, u8 *mac_public, u8 *mac_random_static);
void blc_newMacAddress(u32 flash_addr, u8 *mac_pub, u8 *mac_rand);

void app_pm_task(void);

#if USE_CHG_NAME
void read_dev_name(void);
void save_dev_name(u16 attrID);
#endif

#endif // USE_BLE

void user_init(bool isRetention);

#ifdef ZCL_POLL_CTRL
typedef struct {
	ev_timer_event_t *zclFastPollTimeoutTimerEvt;
	ev_timer_event_t *zclCheckInTimerEvt;
	bool isFastPollMode;
} poll_ctrl_wrk_t;
extern poll_ctrl_wrk_t poll_ctrl_wrk;
#endif

void scan_task(void);
s32 sensors_task(void *arg);
void read_sensor_and_show(void);
#if	USE_DISPLAY
void show_th(void);
#endif

#endif /* _DEVICE_H_ */
