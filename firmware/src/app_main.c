/**********************************************************************
 * INCLUDES
 */
#include "tl_common.h"
#include "app_main.h"
#include "app_utils.h"
#include "zb_api.h"
#include "zcl_include.h"
#include "bdb.h"
#include "ota.h"
#include "app_main.h"
#if ZBHCI_EN
#include "zbhci.h"
#endif

#include "app_ui.h"
#include "app_onoff.h"
#include "zcl_relative_humidity.h"
#include "zcl_illuminance_level_sensing.h"
#ifdef ZCL_DIHUMIDIFICATION_CONTROL
#include "zcl_dehumidification_control.h"
#endif
#include "chip_8258/timer.h"
#include "sensor_th.h"
#include "battery.h"
#include "lcd.h"
#include "reporting.h"
#include "ext_ota.h"
#include "zcl_thermostat_ui_cfg.h"
#if USE_BLE
#include "ble_cfg.h"
#include "zigbee_ble_switch.h"
#endif
#if USE_TRIGGER
#include "trigger.h"
#endif
#if (DEV_SERVICES & SERVICE_PLM)
#include "sensor_rh.h"
#endif
#if USE_SENSOR_LX
#include "sensor_lx.h"
#endif
#if (DEV_SERVICES & SERVICE_PIR)
#include "sensor_pir.h"
#endif
#if USE_SENSOR_XBR818
#include "sensor_xbr818.h"
#endif
#if USE_SENSOR_RC2417
#include "sensor_rc2417.h"
#endif
#if (BOARD == BOARD_ZG223Z)
#include "sensors_zg223z.h"
#endif

/**********************************************************************
 * LOCAL CONSTANTS
 */


/**********************************************************************
 * TYPEDEFS
 */


/**********************************************************************
 * GLOBAL VARIABLES
 */

/**********************************************************************
 * Adaptive TX power control (TPC) — boost-on-failure, then force-rejoin-when-stuck
 *
 * Output: g_zb_txPowerSet is re-applied by the MAC on every radio power-up
 *         (rf_reset -> rf_setTxPower), so a change survives sleep/wake.
 * Input:  per-report APS data-confirm status (MAC_STA_NO_ACK = the parent did
 *         not hear us) and parent-loss / rejoin events.
 * Config: g_zcl_thermostatUICfgAttrs.txpwr_* (persisted to NV on HA write).
 */
extern u8 g_zb_txPowerSet;
extern void rf_setTxPower(u8 power);

static const u8 tpc_ladder[] = {
	RF_POWER_INDEX_P3p01dBm,  // ~+3 dBm (pvvx default)
	RF_POWER_INDEX_P4p57dBm,  // ~+5
	RF_POWER_INDEX_P6p14dBm,  // ~+6
	RF_POWER_INDEX_P7p79dBm,  // ~+8
	RF_POWER_INDEX_P8p97dBm,  // ~+9
	RF_POWER_INDEX_P9p81dBm,  // ~+10 (near chip max)
};
static const u8 tpc_dbm[] = { 3, 5, 6, 8, 9, 10 };
#define TPC_N (sizeof(tpc_ladder) / sizeof(tpc_ladder[0]))

u8 g_txpwr_step    = 0;
s8 g_txpwr_cur_dbm = 3;
extern u32 rtcSeconds;
static u8  tpc_badscore      = 0;  // rolling bad-parent-link score (NO_ACK +2, ok -1)

/* Binary search for the lowest rung at which the parent acknowledges TPC_PROBE_N
 * polls in a row. A poll is MAC-acknowledged by the parent, so its ACK proves our
 * uplink at that power, and a missed poll loses no data. hi = a rung known to work,
 * lo = lowest rung not yet ruled out. Runs after boot and every TPC_SEARCH_PERIOD_S;
 * in between, misses still climb one rung at a time (tpc_note_tx). */
static u8  tpc_srch;               // search running
static u8  tpc_srch_done;          // a search has run since boot
static u8  tpc_lo, tpc_hi, tpc_probe_ok;
static u32 tpc_srch_last;          // rtcSeconds when the last search started
/* Diagnostics, attribute 0x0139 (read-only): byte 0 probe polls of the last search
 * (0 = none ran), byte 1 rung of its last miss (0xFF = none), byte 2 MAC status of
 * that miss, byte 3 the rung it settled on. */
u32 g_tpc_trace;


static u8 tpc_step_for_dbm(u8 d){
	u8 s = 0;
	for(u8 i = 0; i < TPC_N; i++){
		if(tpc_dbm[i] <= d) s = i;   // highest rung whose dBm <= requested
	}
	return s;
}

static void tpc_set_step(u8 step){
	if(step >= TPC_N) step = TPC_N - 1;
	g_txpwr_step    = step;
	g_txpwr_cur_dbm = (s8)tpc_dbm[step];
	g_zb_txPowerSet = tpc_ladder[step];   // re-applied by MAC across sleep/wake
	rf_setTxPower(tpc_ladder[step]);      // apply immediately
}

/* Probe the middle of [lo, hi], or settle on hi once the interval is one rung. */
static void tpc_search_next(void){
	if(tpc_lo >= tpc_hi){
		tpc_srch = 0;
		g_tpc_trace = (g_tpc_trace & 0x00ffffff) | ((u32)tpc_hi << 24);
		tpc_set_step(tpc_hi);
		tpc_badscore = 0;
		return;
	}
	tpc_probe_ok = 0;
	tpc_set_step((tpc_lo + tpc_hi) / 2);
}

static void tpc_search_note(bool ok, u8 status){
	if((g_tpc_trace & 0xff) < 0xff)
		g_tpc_trace++;
	if(!ok) {
		g_tpc_trace = (g_tpc_trace & 0xff0000ff) | ((u32)status << 16) | ((u32)g_txpwr_step << 8);
		tpc_lo = g_txpwr_step + 1;     // too weak: everything up to here is ruled out
	}
	else if(++tpc_probe_ok >= TPC_PROBE_N)
		tpc_hi = g_txpwr_step;         // passed: new known-good rung
	else
		return;
	tpc_search_next();
}

static void tpc_search_start(void){
	tpc_srch_done = 1;
	tpc_srch_last = rtcSeconds;
	tpc_lo = tpc_step_for_dbm(g_zcl_thermostatUICfgAttrs.txpwr_min_dbm);
	tpc_hi = g_txpwr_step;            // the current rung works
	if(tpc_hi < tpc_lo)
		tpc_hi = tpc_lo;
	tpc_srch = 1;
	g_tpc_trace = 0x0000ff00;         // no probe yet, no miss yet
	tpc_search_next();
}

/* The parent acknowledged our data poll: with data for us (SUCCESS), with nothing
 * queued (NO_DATA) or with more queued (FRAME_PENDING, 0x20). Counting only the
 * first two made every poll with pending data look like a lost link. */
static bool poll_acked(u8 status){
	return status == SUCCESS || status == MAC_STA_NO_DATA || status == MAC_STA_FRAME_PENDING;
}

/* Next probe poll of a search burst, right after the previous confirm. */
static void tpc_probe_poll(void *arg){
	(void) arg;
	if(tpc_srch)
		zb_endDeviceSyncReq();
}

/* Poll again right away (after a miss, now at max power). */
static void tpc_repoll(void *arg){
	(void) arg;
	if(zb_isDeviceJoinedNwk())
		zb_endDeviceSyncReq();
}

/* Poll result: feeds the search, or starts one when due on a healthy link. The
 * search runs as one burst of back-to-back polls (<= 3 rungs x TPC_PROBE_N, well
 * under a second), so it never skips a regular poll: the parent keeps queued
 * messages for a sleeping child only ~7.7 s (macTransactionPersistenceTime). */
static void tpc_note_poll(u8 status){
	bool ok = poll_acked(status);
	if(!g_zcl_thermostatUICfgAttrs.txpwr_mode)
		return;                       // fixed power
	if(tpc_srch) {
		tpc_search_note(ok, status);
	} else {
		tpc_note_tx(ok ? SUCCESS : MAC_STA_NO_ACK);
		if(ok && !tpc_badscore && zb_isDeviceJoinedNwk() && !g_sensorAppCtx.linkDown
		 && (!tpc_srch_done || rtcSeconds - tpc_srch_last >= TPC_SEARCH_PERIOD_S))
			tpc_search_start();
	}
	if(tpc_srch)
		TL_SCHEDULE_TASK(tpc_probe_poll, NULL);
}

void tpc_apply(void){
	zcl_thermostatUICfgAttr_t *c = &g_zcl_thermostatUICfgAttrs;
	if(tpc_srch)
		g_txpwr_step = tpc_hi;            // interrupted burst: back to the known-good rung
	tpc_srch = 0;                         // (re)join or new limits: search again
	tpc_srch_done = 0;
	u8 mn = tpc_step_for_dbm(c->txpwr_min_dbm);
	u8 mx = tpc_step_for_dbm(c->txpwr_max_dbm);
	if(mx < mn) mx = mn;
	if(c->txpwr_mode == 0){               // fixed
		tpc_set_step(tpc_step_for_dbm(c->txpwr_fixed_dbm));
	} else {                              // adaptive: clamp current to [min,max]
		u8 s = g_txpwr_step;
		if(s < mn) s = mn;
		if(s > mx) s = mx;
		tpc_set_step(s);
	}
	tpc_badscore = 0;
}

void tpc_boost_max(void){
	zcl_thermostatUICfgAttr_t *c = &g_zcl_thermostatUICfgAttrs;
	if(c->txpwr_mode){                    // adaptive only
		tpc_srch = 0;
		tpc_srch_done = 0;                // search down again once the link is back
		tpc_set_step(tpc_step_for_dbm(c->txpwr_max_dbm));
		tpc_badscore = 0;
	}
}

/* ---- Parent link monitoring / re-homing ---------------------------------- */
static u16 link_hist;			// bit i = 1: event i (0 = newest) failed
static u32 rehome_last_sec;		// rtcSeconds of the last proactive re-home
static u8  rehome_done;			// a proactive re-home happened since boot
u8 g_rehome_cmd = 0;			// attribute 0x0137 (command: 1 = re-home now)

static int link_rehome_cb(void *arg) {
	(void) arg;
	if(zb_isDeviceJoinedNwk()) {
		sws_printf("re-home: rejoin via best parent\n");
		tpc_boost_max();			// scan for parents at full TX power
		zb_rejoinSecModeSet(REJOIN_SECURITY);
		zb_rejoinReq(zb_apsChannelMaskGet(), g_bdbAttrs.scanDuration);
	}
	return -1;
}

/* Rejoin through the best parent in range. Proactive (not forced) re-homes are
 * capped to one per LINK_REHOME_MIN_S; returns whether one was scheduled. */
static bool link_rehome(bool forced, u32 delay_ms) {
	if(!forced && rehome_done && rtcSeconds - rehome_last_sec < LINK_REHOME_MIN_S)
		return false;
	if(!forced) {
		rehome_done = 1;
		rehome_last_sec = rtcSeconds;
	}
	link_hist = 0;
	TL_ZB_TIMER_SCHEDULE(link_rehome_cb, NULL, delay_ms);
	return true;
}

/* Record one link event; too many failures in the window -> proactive re-home. */
void link_note(bool ok) {
	link_hist = ((link_hist << 1) | (ok ? 0 : 1)) & ((1 << LINK_WINDOW) - 1);
	u8 fails = 0;
	for(u16 h = link_hist; h; h >>= 1)
		fails += h & 1;
	if(fails >= LINK_FAIL_THRESHOLD && zb_isDeviceJoinedNwk())
		link_rehome(false, 10);
}

/* Data poll to the parent: answered (with or without pending data) = ok. */
void sensorDevice_nlmeSyncCnf(void *arg) {
	nlme_sync_cnf_t *cnf = (nlme_sync_cnf_t *)arg;
	bool ok = poll_acked(cnf->status);
	/* Fortify: a parent that answers our polls means the link is up, even if no
	 * BDB success callback reported it; this also ends a pending rejoin backoff. */
	if(ok && g_sensorAppCtx.linkDown && zb_isDeviceJoinedNwk())
		g_sensorAppCtx.linkDown = 0;
	link_note(ok);
	/* TX power feedback. During a search burst any other failure also counts as a
	 * miss, so the burst always continues (or settles) instead of stalling low. */
	if(ok || cnf->status == MAC_STA_NO_ACK || tpc_srch)
		tpc_note_poll(cnf->status);
}

static int rehome_cmd_cb(void *arg) {
	(void) arg;
	g_rehome_cmd = 0;		// switch feedback: report "off" before leaving the parent
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG, REHOME_ATTRID);
	link_rehome(true, 500);	// manual: not capped; give the report time to go out
	return -1;
}

/* Write to REHOME_ATTRID. Deferred so the write response precedes the "off" report. */
void rehome_cmd_written(void) {
	if(g_rehome_cmd)
		TL_ZB_TIMER_SCHEDULE(rehome_cmd_cb, NULL, 250);
}

void tpc_note_tx(u8 status){
	zcl_thermostatUICfgAttr_t *c = &g_zcl_thermostatUICfgAttrs;
	if(!c->txpwr_mode) return;            // adaptive only
	u8 mx = tpc_step_for_dbm(c->txpwr_max_dbm);

	bool miss = status == APS_STATUS_NO_ACK || status == MAC_STA_NO_ACK;
	if(tpc_srch){                                   // a report failed at a probed rung
		if(miss)
			tpc_search_note(false, status);
		return;
	}
	if(!miss){
		if(tpc_badscore) tpc_badscore--;            // a good tx slowly heals
		return;
	}

	/* A miss below max: jump to max and poll again at once, so whatever the parent
	 * queued for us (kept only ~7.7 s) is fetched; that poll's success then starts
	 * a new search burst. No immediate re-poll at max, so a lost parent cannot
	 * cause a poll storm. */
	if(g_txpwr_step < mx){
		tpc_set_step(mx);
		tpc_badscore = 0;
		tpc_srch_done = 0;
		TL_SCHEDULE_TASK(tpc_repoll, NULL);
		return;
	}
	/* Bad even at max TX power: force a rejoin so the stack re-picks a parent.
	 * The NWK LQI threshold (NWK_NEIGHBORTBL_ADD_LQITHRESHOLD) then excludes a
	 * too-weak coordinator, which lets us land on a nearer router. */
	if(tpc_badscore < 200) tpc_badscore += 2;
	if(tpc_badscore >= 12){
		tpc_badscore = 0;
		if(zb_isDeviceJoinedNwk())
			link_rehome(false, 10);                 // shares the daily re-home cap
	}
}

/* EP1 data-confirm: feed TPC and link monitoring, then chain the existing on/off retry handler. */
void sensorDevice_dataCnfCb(void *arg){
	apsdeDataConf_t *cnf = (apsdeDataConf_t *)arg;
	tpc_note_tx(cnf->status);
	link_note(cnf->status == SUCCESS);
#if defined(ZCL_ON_OFF) && USE_RETRY_ONOFF
	afTestOnOffCb(arg);
#endif
}

/* Report one attribute straight to the coordinator (nwk 0x0000, endpoint 1),
 * independent of the binding table. */
status_t app_report_to_coordinator(u8 endpoint, u16 cluster_id, u16 attr_id) {
	if(!zb_isDeviceJoinedNwk())
		return ZCL_STA_FAILURE;
	zclAttrInfo_t *pAttrEntry = zcl_findAttribute(endpoint, cluster_id, attr_id);
	if(!pAttrEntry)
		return ZCL_STA_FAILURE;
	epInfo_t dstEpInfo;
	TL_SETSTRUCTCONTENT(dstEpInfo, 0);
	dstEpInfo.profileId = HA_PROFILE_ID;
	dstEpInfo.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
	dstEpInfo.dstAddr.shortAddr = 0x0000;
	dstEpInfo.dstEp = 1;
	return zcl_sendReportCmd(endpoint, &dstEpInfo, TRUE, ZCL_FRAME_SERVER_CLIENT_DIR,
			cluster_id, pAttrEntry->id, pAttrEntry->type, pAttrEntry->data);
}

/* Report several attributes of one cluster straight to the coordinator, in
 * frames of up to 8 records (no APS fragmentation needed). */
static void app_report_attrs_to_coordinator(u8 endpoint, u16 cluster_id, const u16 *ids, u8 n) {
	if(!zb_isDeviceJoinedNwk())
		return;
	u8 buf[sizeof(zclReportCmd_t) + 8 * sizeof(zclReport_t)];
	zclReportCmd_t *cmd = (zclReportCmd_t *)buf;
	epInfo_t dstEpInfo;
	TL_SETSTRUCTCONTENT(dstEpInfo, 0);
	dstEpInfo.profileId = HA_PROFILE_ID;
	dstEpInfo.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
	dstEpInfo.dstAddr.shortAddr = 0x0000;
	dstEpInfo.dstEp = 1;
	cmd->numAttr = 0;
	for(u8 i = 0; i < n; i++) {
		zclAttrInfo_t *a = zcl_findAttribute(endpoint, cluster_id, ids[i]);
		if(a) {
			cmd->attrList[cmd->numAttr].attrID = a->id;
			cmd->attrList[cmd->numAttr].dataType = a->type;
			cmd->attrList[cmd->numAttr].attrData = a->data;
			cmd->numAttr++;
		}
		if(cmd->numAttr == 8 || (i == n - 1 && cmd->numAttr)) {
			zcl_sendReportAttrsCmd(endpoint, &dstEpInfo, TRUE, ZCL_FRAME_SERVER_CLIENT_DIR, cluster_id, cmd);
			cmd->numAttr = 0;
		}
	}
}

/* All UI cfg settings, so HA's entities match the device after a (re)join or reset. */
void app_report_config(void) {
	static const u16 ids[] = {
		0x0100, 0x0101, 0x0107,		// temperature/humidity offset, reporting (measurement) interval
		0x0120, 0x0121, 0x0122, 0x0123, 0x0124,
#if (DEV_SERVICES & SERVICE_PLM)
		DEBUG_MODE_ATTRID, PROBE_INTERVAL_ATTRID, PROBE_SETTLE_ATTRID, PROBE_REPORT_CHANGE_ATTRID,
		PROBE_SAMPLES_ATTRID, PROBE_HEARTBEAT_ATTRID, PROBE_PWM_KHZ_ATTRID, PROBE_PWM_DUTY_ATTRID,
		PROBE_CAL_DRY_ATTRID, PROBE_CAL_WET_ATTRID, PROBE_CAL_DRY_MV_ATTRID, PROBE_CAL_WET_MV_ATTRID,
		PROBE_COMP_SLOPE_DRY_ATTRID, PROBE_COMP_SLOPE_WET_ATTRID,
		PROBE_CAL_CAPTURE_ATTRID, PROBE_SWEEP_ATTRID, PROBE_RESET_ATTRID,
#endif
		REHOME_ATTRID,
	};
	app_report_attrs_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
			ids, sizeof(ids) / sizeof(ids[0]));
}

static int app_report_config_cb(void *arg) {
	(void) arg;
	app_report_config();
	return -1;
}

void app_report_config_later(void) {
	TL_ZB_TIMER_SCHEDULE(app_report_config_cb, NULL, 5000);
}

#if (DEV_SERVICES & SERVICE_PLM)
u8 g_probe_reset_cmd = 0;			// attribute 0x0138 (command: 1 = reset probe settings)

static int probe_reset_cb(void *arg) {
	(void) arg;
	zcl_thermostatUICfgAttr_t *c = &g_zcl_thermostatUICfgAttrs;
	const zcl_thermostatUICfgAttr_t *d = &g_zcl_thermostatUICfgDefault;
	c->probe_interval_s = d->probe_interval_s;
	c->probe_settle_ms = d->probe_settle_ms;
	c->probe_report_change = d->probe_report_change;
	c->probe_samples = d->probe_samples;
	c->probe_heartbeat_n = d->probe_heartbeat_n;
	c->probe_pwm_khz = d->probe_pwm_khz;
	c->probe_pwm_duty = d->probe_pwm_duty;
	c->probe_comp_slope_dry = d->probe_comp_slope_dry;
	c->probe_comp_slope_wet = d->probe_comp_slope_wet;
	g_probe_reset_cmd = 0;		// switch feedback
	zcl_thermostatConfig_save(0);
	probe_apply_reporting();
	g_zcl_MoistureAttrs.measuredValue = probe_percent(g_zcl_MoistureAttrs.raw_adc);
	app_report_config();
	sws_printf("probe settings reset to defaults\n");
	return -1;
}

/* Write to PROBE_RESET_ATTRID; deferred so the write response precedes the reports. */
void probe_reset_written(void) {
	if(g_probe_reset_cmd)
		TL_ZB_TIMER_SCHEDULE(probe_reset_cb, NULL, 250);
}

/* Reporting of the soil values on EP2 from config: threshold (0x0128) for raw
 * 0xF000/0xF001; heartbeat = N (0x012A) x probe interval (0x0126) for those
 * and the moisture % (0x0000), capped below 0xFFFF ("no reporting"). */
void probe_apply_reporting(void) {
	u16 change = g_zcl_thermostatUICfgAttrs.probe_report_change;
	u32 n = g_zcl_thermostatUICfgAttrs.probe_heartbeat_n;
	u32 iv = g_zcl_thermostatUICfgAttrs.probe_interval_s;
	if(n < 1 || n > PROBE_HEARTBEAT_N_MAX)
		n = PROBE_HEARTBEAT_N_DEF;
	if(iv < PROBE_INTERVAL_MIN_S)
		iv = PROBE_INTERVAL_DEF_S;
	u32 hb = n * iv;
	u16 heartbeat = hb > 0xFFFE ? 0xFFFE : (u16)hb;
	static const u16 attrs[] = {0xF000, 0xF001, ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE};
	for(int i = 0; i < 3; i++) {
		reportCfgInfo_t *pEntry = zcl_reportCfgInfoEntryFind(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, attrs[i]);
		if(!pEntry)
			continue;
		if(attrs[i] != ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE) {
			pEntry->reportableChange[0] = change & 0xff;
			pEntry->reportableChange[1] = change >> 8;
		}
		pEntry->maxInterval = heartbeat;
		if(pEntry->maxIntCnt > heartbeat)
			pEntry->maxIntCnt = heartbeat;
	}
}

u8 g_probe_cal_capture = 0;	// attribute 0x012F (command: 1 = dry, 2 = wet)

/* Store the current probe reading as the dry or wet calibration point. Runs from
 * a timer after the write, so the write response precedes the "off" report. */
static int probe_cal_capture_cb(void *arg) {
	(void) arg;
	u8 which = g_probe_cal_capture;
	g_probe_cal_capture = 0;
	if(which != 1 && which != 2)
		return -1;
	read_rh_sensor_now();	// fresh reading with the current probe settings
	u16 raw = g_zcl_MoistureAttrs.raw_adc;
	u16 vbat = measured_battery.average_mv;	// for battery compensation of this point
	if(which == 1) {
		g_zcl_thermostatUICfgAttrs.probe_cal_dry = raw;
		g_zcl_thermostatUICfgAttrs.probe_cal_dry_mv = vbat;
	} else {
		g_zcl_thermostatUICfgAttrs.probe_cal_wet = raw;
		g_zcl_thermostatUICfgAttrs.probe_cal_wet_mv = vbat;
	}
	g_zcl_MoistureAttrs.measuredValue = probe_percent(raw);
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
			which == 1 ? PROBE_CAL_DRY_ATTRID : PROBE_CAL_WET_ATTRID);
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
			which == 1 ? PROBE_CAL_DRY_MV_ATTRID : PROBE_CAL_WET_MV_ATTRID);
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, 0xF000);
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE);
	zcl_thermostatConfig_save(0);	// the write handler saved before this ran
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
			PROBE_CAL_CAPTURE_ATTRID);	// switch feedback: back to off
	sws_printf("probe cal %s = %d\n", which == 1 ? "dry" : "wet", raw);
	return -1;
}

/* Probe sweep: one reading per settle time, each from cold, reported as it is taken. */
static const u16 probe_sweep_ms[PROBE_SWEEP_N] = {1, 5, 20, 50, 100, 300, 1000};
u16 g_probe_sweep[PROBE_SWEEP_N];	// EP2 attrs 0xF010.. raw ADC (x4); 0 = not measured yet
u8 g_probe_sweep_cmd = 0;			// attribute 0x0130 (command: 1 = run)
static u8 sweep_step;
static ev_timer_event_t *sweepTimerEvt = NULL;

static int probe_sweep_cb(void *arg) {
	(void) arg;
	g_probe_sweep[sweep_step] = probe_measure_raw(probe_sweep_ms[sweep_step]); // ends with PD3 off
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
			PROBE_SWEEP_RESULT_ATTRID + sweep_step);
	sws_printf("sweep %d ms: %d\n", probe_sweep_ms[sweep_step], g_probe_sweep[sweep_step]);
	if(++sweep_step < PROBE_SWEEP_N)
		return 0;			// next point after another cold gap
	sweepTimerEvt = NULL;
	g_probe_sweep_cmd = 0;	// switch feedback: back to off when done
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG, PROBE_SWEEP_ATTRID);
	return -1;
}

/* Command stays 1 (switch on) while the sweep runs. */
static void probe_sweep_start(void) {
	if(sweepTimerEvt)
		return;			// already running
	sweep_step = 0;
	sweepTimerEvt = TL_ZB_TIMER_SCHEDULE(probe_sweep_cb, NULL, PROBE_SWEEP_GAP_MS);
}

/* Side effects of a written probe config attribute (NV save happens in the caller). */
void probe_cfg_written(u16 attrID) {
	switch(attrID) {
	case PROBE_SWEEP_ATTRID:
		if(g_probe_sweep_cmd)
			probe_sweep_start();
		break;
	case PROBE_REPORT_CHANGE_ATTRID:
	case PROBE_HEARTBEAT_ATTRID:
	case PROBE_INTERVAL_ATTRID:		// heartbeat scales with the probe interval
		probe_apply_reporting();
		break;
	case PROBE_CAL_CAPTURE_ATTRID:
		if(g_probe_cal_capture)
			TL_ZB_TIMER_SCHEDULE(probe_cal_capture_cb, NULL, 250);
		break;
	case REHOME_ATTRID:
		rehome_cmd_written();
		break;
	case PROBE_RESET_ATTRID:
		probe_reset_written();
		break;
	case PROBE_CAL_DRY_ATTRID:
	case PROBE_CAL_WET_ATTRID:
	case PROBE_CAL_DRY_MV_ATTRID:
	case PROBE_CAL_WET_MV_ATTRID:
	case PROBE_COMP_SLOPE_DRY_ATTRID:
	case PROBE_COMP_SLOPE_WET_ATTRID:
		g_zcl_MoistureAttrs.measuredValue = probe_percent(g_zcl_MoistureAttrs.raw_adc);
		break;
	}
}

u8 g_debug_on = 0;			// attribute 0x0125
static u16 debug_secs_left;
static ev_timer_event_t *debugTimerEvt = NULL;
static u8 debug_ticks;

static int debug_timer_cb(void *arg) {
	(void) arg;
	if(!g_debug_on || !debug_secs_left || !zb_isDeviceJoinedNwk()) {
		g_debug_on = 0;
		debug_secs_left = 0;
		debugTimerEvt = NULL;
		set_PollRate();		// back to the normal long poll
		app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG, DEBUG_MODE_ATTRID);
		sws_printf("debug mode off\n");
		return -1;			// cancel timer
	}
	read_rh_sensor_now();	// PD3 + PWM + settle + ADC -> raw_adc / adc_diode
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, 0xF000);
	app_report_to_coordinator(SENSOR_DEVICE_ENDPOINT2, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, 0xF001);
	if(++debug_ticks >= 1000 / DEBUG_MODE_PERIOD_MS) {
		debug_ticks = 0;
		debug_secs_left--;
	}
	return 0;				// keep the period
}

/* Called after a write to DEBUG_MODE_ATTRID. On (re)starts the 10-min window;
 * off stops at the next tick. */
void debug_mode_update(void) {
	if(!g_debug_on)
		return;
	g_debug_on = 1;
	debug_secs_left = DEBUG_MODE_SEC;
	if(!debugTimerEvt) {
		debug_ticks = 0;
		zb_setPollRate(1000);	// poll the parent every second so "off" arrives quickly
		debugTimerEvt = TL_ZB_TIMER_SCHEDULE(debug_timer_cb, NULL, DEBUG_MODE_PERIOD_MS);
		sws_printf("debug mode on: %d s\n", debug_secs_left);
	}
}
#endif

app_ctx_t g_sensorAppCtx;

#if ZCL_OTA_SUPPORT
extern ota_callBack_t sensorDevice_otaCb;

//running code firmware information
ota_preamble_t sensorDevice_otaInfo = {
	.fileVer 			= FILE_VERSION,
	.imageType 			= IMAGE_TYPE,
	.manufacturerCode 	= MANUFACTURER_CODE_TELINK,
};
#endif


//Must declare the application call back function which used by ZDO layer
const zdo_appIndCb_t appCbLst = {
	bdb_zdoStartDevCnf,//start device cnf cb
	NULL,//reset cnf cb
	NULL,//device announce indication cb
	sensorDevice_leaveIndHandler,//leave ind cb
	sensorDevice_leaveCnfHandler,//leave cnf cb
	NULL,//nwk update ind cb
	NULL,//permit join ind cb
	(zdo_nlmeSyncCnfCb_t)sensorDevice_nlmeSyncCnf,//nlme sync cnf cb: data poll results -> link monitoring
	NULL,//tc join ind cb
	NULL,//tc detects that the frame counter is near limit
#if !USE_BLE
	sensorDevice_nwkStatusIndHandler //nwk status ind cb
#endif
};


/**
 *  @brief Definition for bdb commissioning setting
 */
bdb_commissionSetting_t g_bdbCommissionSetting = {
	.linkKey.tcLinkKey.keyType = SS_GLOBAL_LINK_KEY,
	.linkKey.tcLinkKey.key = (u8 *)tcLinkKeyCentralDefault,       		//can use unique link key stored in NV

	.linkKey.distributeLinkKey.keyType = MASTER_KEY,
	.linkKey.distributeLinkKey.key = (u8 *)linkKeyDistributedMaster,  	//use linkKeyDistributedCertification before testing

	.linkKey.touchLinkKey.keyType = MASTER_KEY,
	.linkKey.touchLinkKey.key = (u8 *)touchLinkKeyMaster,   			//use touchLinkKeyCertification before testing

#if TOUCHLINK_SUPPORT
	.touchlinkEnable = 1,												/* enable touch-link */
#else
	.touchlinkEnable = 0,												/* disable touch-link */
#endif
	.touchlinkChannel = DEFAULT_CHANNEL, 								/* touch-link default operation channel for target */
	.touchlinkLqiThreshold = 0xA0,			   							/* threshold for touch-link scan req/resp command */
};

/**********************************************************************
 * LOCAL VARIABLES
 */

/**********************************************************************
 * FUNCTIONS
 */

void populate_date_code(void) {
	u8 month;
	if (__DATE__[0] == 'J' && __DATE__[1] == 'a' && __DATE__[2] == 'n') month = 1;
	else if (__DATE__[0] == 'F') month = 2;
	else if (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'r') month = 3;
	else if (__DATE__[0] == 'A' && __DATE__[1] == 'p') month = 4;
	else if (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'y') month = 5;
	else if (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'n') month = 6;
	else if (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'l') month = 7;
	else if (__DATE__[0] == 'A' && __DATE__[1] == 'u') month = 8;
	else if (__DATE__[0] == 'S') month = 9;
	else if (__DATE__[0] == 'O') month = 10;
	else if (__DATE__[0] == 'N') month = 11;
	else if (__DATE__[0] == 'D') month = 12;

	g_zcl_basicAttrs.dateCode[1] = __DATE__[7];
	g_zcl_basicAttrs.dateCode[2] = __DATE__[8];
	g_zcl_basicAttrs.dateCode[3] = __DATE__[9];
	g_zcl_basicAttrs.dateCode[4] = __DATE__[10];
	g_zcl_basicAttrs.dateCode[5] = '0' + month / 10;
	g_zcl_basicAttrs.dateCode[6] = '0' + month % 10;
	g_zcl_basicAttrs.dateCode[7] = __DATE__[4] >= '0' ? (__DATE__[4]) : '0';
	g_zcl_basicAttrs.dateCode[8] = __DATE__[5];
}


void populate_hw_version(void) {
#if BOARD == BOARD_LYWSD03MMC
/*
 HW  | LCD I2C addr | SHTxxx I2C addr | Note
-----|--------------|-----------------|---------
B1.4 | 0x3C         | 0x70   (SHTC3)  |
B1.5 | UART!        | 0x70   (SHTC3)  |
B1.6 | UART!        | 0x44   (SHT4x)  |
B1.7 | 0x3C         | 0x44   (SHT4x)  | Test   original string HW
B1.9 | 0x3E         | 0x44   (SHT4x)  |
B2.0 | 0x3C         | 0x44   (SHT4x)  | Test   original string HW
*/
#if USE_BLE
    ble_attr.my_HardStr[0] = 'B';
    ble_attr.my_HardStr[1] = '1';
    ble_attr.my_HardStr[2] = '.';
#endif
    if (scr.i2c_address == B14_I2C_ADDR << 1) {
        if (sensor_ht.sensor_type == TH_SENSOR_SHTC3) { // sensor_version == 0)
            g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_B14; // B1.4
#if USE_BLE
            ble_attr.my_HardStr[3] = '4';
#endif
        } else {
            g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_B20; // B2.0 or B1.7
#if USE_BLE
            ble_attr.my_HardStr[1] = '2';
            ble_attr.my_HardStr[3] = '0';
#endif
        }
    } else if (scr.i2c_address == (B16_I2C_ADDR << 1)) {
        if (sensor_ht.sensor_type == TH_SENSOR_SHTC3) { // (sensor_version == 0)
            g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_B15; // B1.5
#if USE_BLE
            ble_attr.my_HardStr[3] = '5';
#endif
        } else if (scr.i2c_address == N16_I2C_ADDR) {
            g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_N16; // N1.6
#if USE_BLE
            ble_attr.my_HardStr[3] = '1';
#endif
        } else {
            g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_B16; // B1.6
#if USE_BLE
            ble_attr.my_HardStr[3] = '6';
#endif
        }
    } else if (scr.i2c_address == (B19_I2C_ADDR << 1)) {
        g_zcl_basicAttrs.hwVersion = BOARD_LYWSD03MMC_B19; // B1.9
#if USE_BLE
        ble_attr.my_HardStr[3] = '9';
#endif
    }
#else // BOARD != BOARD_LYWSD03MMC
    g_zcl_basicAttrs.hwVersion = BOARD;
#if USE_BLE
    ble_attr.my_HardStr[0] = 'V';
    ble_attr.my_HardStr[1] = '0';
    ble_attr.my_HardStr[2] = '0' + BOARD / 10;
    ble_attr.my_HardStr[3] = '0' + BOARD % 10;
#endif
#endif
}

#if SHOW_SMILEY

u8 is_comfort(s16 t, u16 h) {
	u8 ret = 0;
	if (t >= g_zcl_thermostatUICfgAttrs.temp_comfort_min
		&& t <= g_zcl_thermostatUICfgAttrs.temp_comfort_max
		&& h >= g_zcl_thermostatUICfgAttrs.humi_comfort_min
		&& h <= g_zcl_thermostatUICfgAttrs.humi_comfort_max)
		ret = 1;
	return ret;
}
#endif

#if	USE_DISPLAY
void show_th(void) {
#ifdef ZCL_THERMOSTAT_UI_CFG
    		if (g_zcl_thermostatUICfgAttrs.TemperatureDisplayMode) {
    			// (°F) = (Temperature in degrees Celsius (°C) * 9/5) + 32.
    			show_big_number_x10(((sensor_ht.temp * 9 + 25) / 50) + 320, TEMP_SYMBOL_F); // convert C to F
    		} else {
    			show_big_number_x10((sensor_ht.temp + 5) / 10, TEMP_SYMBOL_C);
    		}
#else // !ZCL_THERMOSTAT_UI_CFG
    		show_big_number_x10((sensor_ht.temp  + 5) / 10, TEMP_SYMBOL_C);
#endif // ZCL_THERMOSTAT_UI_CFG

#ifdef USE_DISPLAY_SMALL_NUMBER_X10
    		show_small_number_x10((sensor_ht.humi + 5)/ 10, 1);
#else
    		show_small_number((sensor_ht.humi + 50) / 100, 1);
#endif
#ifdef USE_DISPLAY_BATTERY_LEVEL
    		show_battery_symbol(true, g_zcl_powerAttrs.batteryPercentage);
#else
    		show_battery_symbol(g_zcl_powerAttrs.batteryPercentage < 25);
#endif
#if SHOW_SMILEY
#ifdef ZCL_THERMOSTAT_UI_CFG
    		if(g_zcl_thermostatUICfgAttrs.showSmiley == 0)
    			show_smiley(is_comfort(sensor_ht.temp, sensor_ht.humi) ? SMILE_HAPPY : SMILE_SAD);
    		else
    			show_smiley(SMILE_NONE);
#else
    		show_smiley(is_comfort(sensor_ht.temp, sensor_ht.humi) ? SMILE_HAPPY : SMILE_SAD);
#endif // ZCL_THERMOSTAT_UI_CFG
#endif // SHOW_SMILEY
}
#endif // USE_DISPLAY


void read_sensor_and_show(void) {
#if (USE_SENSOR_LX == 1) || ((USE_SENSOR_LX == 2) && !USE_SENSOR_TH) || (USE_SENSOR_RND)
#if (USE_SENSOR_RND)
	read_sensors();
#else
	read_illumi_sensor();
#endif
#ifdef ZCL_POWER_CFG
	g_zcl_powerAttrs.batteryVoltage = (u8)((measured_battery.average_mv + 50) / 100);
	g_zcl_powerAttrs.batteryPercentage = (u8)measured_battery.level;
#endif
#elif USE_SENSOR_TH
	if(sensor_ht.read_callback)
		sensor_ht.read_callback();
	else read_sensor();
#else
	battery_detect(0);
#ifdef ZCL_POWER_CFG
	g_zcl_powerAttrs.batteryVoltage = (u8)((measured_battery.average_mv + 50) / 100);
	g_zcl_powerAttrs.batteryPercentage = (u8)measured_battery.level;
#endif
#endif
#if USE_SENSOR_TH && USE_TRIGGER
	if(sensor_ht.flag & FLG_MEASURE_HT_TRG) {
		sensor_ht.flag &= ~FLG_MEASURE_HT_TRG;
		set_trigger_out();
	}
#endif
#if	USE_DISPLAY
#ifdef ZCL_THERMOSTAT_UI_CFG
    if(!g_zcl_thermostatUICfgAttrs.display_off
    	// no redraw over the reset screen while the button is held
    	&& !(g_sensorAppCtx.keyPressed && clock_time_exceed(g_sensorAppCtx.keyPressedTime, BUTTON_ZIGBEE_RESET_MS * 1000))) {
#endif // ZCL_THERMOSTAT_UI_CFG
    	if(sensor_ht.flag & FLG_MEASURE_HT_LCD) {
    		sensor_ht.flag &= ~FLG_MEASURE_HT_LCD;
    		show_th();
    	} else {
    		show_err_sensors();
    	}
#if USE_BLE
extern u8 blt_state;
		show_ble_symbol(blt_state);
#endif
		update_lcd();
#ifdef ZCL_THERMOSTAT_UI_CFG
    }
#endif
#endif // USE_DISPLAY
	while(clock_time() - g_sensorAppCtx.secTimeTik >= CLOCK_16M_SYS_TIMER_CLK_1S) {
		g_sensorAppCtx.secTimeTik += CLOCK_16M_SYS_TIMER_CLK_1S;
		g_sensorAppCtx.reportUpSec++; // + 1 sec
	}
}


/**********************************************************************
 * FUNCTIONS
 */
s32 sensors_task(void *arg) {
	(void) arg;
#if USE_SENSOR_TH
	if(sensor_ht.read_callback)
		sensor_ht.read_callback();
	else
#endif
	{
		u32 tt = clock_time();
		if(tt - g_sensorAppCtx.readSensorTime >= g_sensorAppCtx.measureInterval) {
			g_sensorAppCtx.readSensorTime = tt;
			read_sensor_and_show();
#if (USE_SENSOR_LX == 1) || ((USE_SENSOR_LX == 2) && !USE_SENSOR_TH)
			g_sensorAppCtx.reportFlg = FLG_CHECK_REPORT; // check report table
#endif
		}
	}
#ifdef USE_EPD
	task_lcd();
#endif
	return 0;
}

#if !USE_BLE
#if USE_DISPLAY
#define SENSORS_TIMER_MS	(g_zcl_pollCtrlAttrs.longPollInterval * POLL_RATE_QUARTERSECONDS)
#else
#define SENSORS_TIMER_MS	(600 * 1000) // only keeps a wake-up timer
#endif

/* Clears the handle before the timer is deleted: the timer slot can be reused */
s32 sensors_timer_cb(void *arg) {
	sensors_task(arg);
	if(zb_isDeviceJoinedNwk() && !g_sensorAppCtx.linkDown) {
		g_sensorAppCtx.timerTaskEvt = NULL;
		return -1; // delete timer
	}
	return 0;
}
#endif

/**********************************************************************
 * FUNCTIONS
 */
void app_task(void)
{
	u16 rep_uptime_sec;
#if !USE_BLE
	sensors_task(NULL);
	task_keys();
#endif
#if (DEV_SERVICES & SERVICE_PIR)
	task_pir();
#endif
	if(bdb_isIdle()) {
		// report handler
		if(zb_isDeviceJoinedNwk()) {
#if	USE_DISPLAY
			if(!g_zcl_thermostatUICfgAttrs.display_off) {
				if(!g_sensorAppCtx.timerLedEvt) {
					show_connected_symbol(true);
					update_lcd();
				}
			}
#ifdef USE_BLINK_LED
			if(!g_sensorAppCtx.timerLedEvt)
				light_off();
#endif // USE_BLINK_LED

#else // !USE_DISPLAY
			if(!g_sensorAppCtx.timerLedEvt)
				light_off();
#endif // USE_DISPLAY
#if USE_RETRY_ONOFF
			taskRetryOnOff();
#endif
			rep_uptime_sec = g_sensorAppCtx.reportUpSec;
			if(rep_uptime_sec || g_sensorAppCtx.reportFlg) {
				g_sensorAppCtx.reportUpSec = 0;
				if(app_chk_report(rep_uptime_sec) == ZCL_STA_SUCCESS)
					g_sensorAppCtx.reportFlg = 0;
				else
					g_sensorAppCtx.reportFlg = FLG_REPEAT_REPORT;
#if USE_TRIGGER
				send_onoff();
#endif
			}
		} else { // Device not Joined
#if	USE_DISPLAY
			if(!g_zcl_thermostatUICfgAttrs.display_off) {
				if(!g_sensorAppCtx.timerLedEvt) {
					show_connected_symbol(false);
					update_lcd();
				}
			}
#ifdef USE_BLINK_LED
			if(!g_sensorAppCtx.timerLedEvt)
				light_blink_start(1, 50, 10000);
#endif // USE_BLINK_LED
#else // !USE_DISPLAY
			if(!g_sensorAppCtx.timerLedEvt)
				light_blink_start(1, 50, 10000);
			//gpio_write(GPIO_LED, LED_ON); // - не включать PIN_PULLUP/PULLDOWN !
#endif // USE_DISPLAY
		}
#if PM_ENABLE
#ifdef USE_EPD
		task_lcd();
#endif
#if !USE_BLE
#if	USE_SENSOR_TH
		if(sensor_ht.read_callback) {
			sensor_ht.read_callback();
		} else
#endif // USE_SENSOR_TH
		{
			// no timer: the device would sleep until a button press
			if(!g_sensorAppCtx.timerTaskEvt) {
				ev_timer_event_t *n = ev_timer_nearestGet();
				if(!n || n->timeout > SENSORS_TIMER_MS + 1000)
					g_sensorAppCtx.timerTaskEvt = TL_ZB_TIMER_SCHEDULE(sensors_timer_cb, NULL, SENSORS_TIMER_MS);
			}
			sws_buffer_flush();
			drv_pm_lowPowerEnter();
		}
#endif // !USE_BLE
#endif // PM_ENABLE
	}
}

#if USE_PRINTF || DEBUG_ENABLE
void sensorDeviceSysException(void)
{
	extern volatile u16 T_evtExcept[4];
	sws_printf("SysException: %d, %04x\n", T_evtExcept[0], T_evtExcept[1]);
 #if USE_DISPLAY
	show_big_number_x10(T_evtExcept[0], 0);
  #ifdef USE_DISPLAY_SMALL_NUMBER_X10
	show_small_number_x10(T_evtExcept[1], TEMP_SYMBOL_E);
  #else
	show_small_number(T_evtExcept[1], TEMP_SYMBOL_E);
  #endif
	update_lcd();
  #ifdef USE_EPD
	while(task_lcd()) sleep_us(USE_EPD*1000);
  #endif
 #endif
	sws_buffer_flush();
	// zb_resetDevice();
	drv_pm_longSleep(PM_SLEEP_MODE_DEEPSLEEP, PM_WAKEUP_SRC_TIMER, 20*1000);
}
#endif

#if !USE_BLE
/**
 *  @brief Definition for BDB finding and binding cluster
 */
u16 bdb_findBindClusterList[] =
{
#ifdef ZCL_ON_OFF
	ZCL_CLUSTER_GEN_ON_OFF,
#endif
};

/**
 *  @brief Definition for BDB finding and binding cluster number
 */
#define FIND_AND_BIND_CLUSTER_NUM		(sizeof(bdb_findBindClusterList)/sizeof(bdb_findBindClusterList[0]))
#endif

/*********************************************************************
 * @fn      user_app_init
 *
 * @brief   This function initialize the application
 *
 * @param   None
 *
 * @return  None
 */
void user_app_init(void)
{
	u32 reportableChange;

	sws_init();
	sws_puts("Init..\n");

	/* Populate properties with compiled-in values */
	// populate_sw_build();
	populate_date_code();

#if USE_BLE
	blc_initMacAddress(CFG_MAC_ADDRESS, mac_public, mac_random_static);
#endif

#if 1 // (ZCL_THERMOSTAT_UI_CFG_SUPPORT || USE_CHG_NAME)
	init_nv_app();
#endif

	//TODO: set bool AUTO_QUICK_DATA_POLL_ENABLE = FALSE ? -> zb_config.c
#if QUICK_POLL_DISABLE
	AUTO_QUICK_DATA_POLL_ENABLE = FALSE; // look zb_config.c
#endif
	/* Initialize ZB stack */
	zb_init();
	/* Register stack CB */
	zb_zdoCbRegister((zdo_appIndCb_t *)&appCbLst);

#if	USE_DISPLAY
	LCD_INIT_DELAY();
#endif

#if USE_SENSOR_XBR818
	xbr818_init();
#endif
#if USE_SENSOR_RC2417
	rc2417_init();
#endif
	init_sensor();

#if	USE_DISPLAY
	init_lcd();
	show_connected_symbol(false);
#endif
#if USE_PRINTF || DEBUG_ENABLE
	/* Register except handler for test */
	sys_exceptHandlerRegister(sensorDeviceSysException);
#endif


	populate_hw_version();


#if ZCL_POLL_CTRL_SUPPORT
	af_powerDescPowerModeUpdate(POWER_MODE_RECEIVER_COMES_PERIODICALLY);
#else
	af_powerDescPowerModeUpdate(POWER_MODE_RECEIVER_COMES_WHEN_STIMULATED);
#endif
    /* Initialize ZCL layer */
	/* Register Incoming ZCL Foundation command/response messages */
	zcl_init(sensorDevice_zclProcessIncomingMsg);

	/* Register endPoint */
	af_endpointRegister(SENSOR_DEVICE_ENDPOINT, (af_simple_descriptor_t *)&sensorDevice_simpleDesc, zcl_rx_handler, sensorDevice_dataCnfCb);
#if (DEV_SERVICES & SERVICE_PLM)
	af_endpointRegister(SENSOR_DEVICE_ENDPOINT2, (af_simple_descriptor_t *)&sensorDevice_simpleDesc2, zcl_rx_handler, NULL);
#endif
	zcl_reportingTabInit();

	/* Register ZCL specific cluster information */
	zcl_register(SENSOR_DEVICE_ENDPOINT, SENSOR_DEVICE_CB_CLUSTER_NUM, (zcl_specClusterInfo_t *)g_sensorDeviceClusterList);
#if (DEV_SERVICES & SERVICE_PLM)
	zcl_register(SENSOR_DEVICE_ENDPOINT2, SENSOR_DEVICE_CB_CLUSTER_NUM2, (zcl_specClusterInfo_t *)g_sensorDeviceClusterList2);
#endif

#if ZCL_OTA_SUPPORT
    ota_init(OTA_TYPE_CLIENT, (af_simple_descriptor_t *)&sensorDevice_simpleDesc, &sensorDevice_otaInfo, &sensorDevice_otaCb);
#endif

    // read sensors
	read_sensor_and_show();

    light_off();

	// keys init
	keys_init();

	/* User's Task */
#if ZBHCI_EN
	ev_on_poll(EV_POLL_HCI, zbhciTask);
#endif
	ev_on_poll(EV_POLL_IDLE, app_task);

	/* Load the pre-install code from flash */
	if(bdb_preInstallCodeLoad(&g_sensorAppCtx.tcLinkKey.keyType, g_sensorAppCtx.tcLinkKey.key) == RET_OK){
		g_bdbCommissionSetting.linkKey.tcLinkKey.keyType = g_sensorAppCtx.tcLinkKey.keyType;
		g_bdbCommissionSetting.linkKey.tcLinkKey.key = g_sensorAppCtx.tcLinkKey.key;
	}
	/* Set default reporting configuration */
	reportableChange = 0;
#ifdef ZCL_POWER_CFG
    bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_GEN_POWER_CFG,
		ZCL_ATTRID_BATTERY_VOLTAGE,
		360,
		3600,
		(u8 *)&reportableChange
	);
    bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_GEN_POWER_CFG,
		ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING,
		360,
		3600,
		(u8 *)&reportableChange
	);
#endif
#ifdef ZCL_ON_OFF
#if !NOT_INPYT_ONOFF
    reportableChange = 0;
    bdb_defaultReportingCfg(
    	SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_GEN_ON_OFF,
		ZCL_ATTRID_ONOFF,
        0, 3600,
		(u8 *)&reportableChange);
#endif
#endif
#ifdef ZCL_ILLUMINANCE_MEASUREMENT
        reportableChange = 10;
		bdb_defaultReportingCfg(
			SENSOR_DEVICE_ENDPOINT,
			HA_PROFILE_ID,
			ZCL_CLUSTER_MS_ILLUMINANCE_MEASUREMENT,
			ZCL_ATTRID_MEASURED_VALUE,
			REPORTABLE_SENSOR_TIMER_MIN_SEC,
			REPORTABLE_SENSOR_TIMER_MAX_SEC,
			(u8 *)&reportableChange
		);
#ifdef ZCL_ILLUMINANCE_LEVEL_SENSING
    	reportableChange = 0;
		bdb_defaultReportingCfg(
			SENSOR_DEVICE_ENDPOINT,
			HA_PROFILE_ID,
			ZCL_CLUSTER_MS_ILLUMINANCE_LEVEL_SENSING_CONFIG,
			ZCL_ATTRID_ILSC_LEVEL_STATUS,
			0,
			3600,
			(u8 *)&reportableChange
		);
#endif // ZCL_ILLUMINANCE_LEVEL_SENSING
#endif // ZCL_ILLUMINANCE_MEASUREMENT
#ifdef ZCL_TEMPERATURE_MEASUREMENT
    reportableChange = 10;
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
		ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		REPORTABLE_SENSOR_TIMER_MAX_SEC,
		(u8 *)&reportableChange
	);
#endif
#if (DEV_SERVICES & SERVICE_PLM)
    reportableChange = 50;
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT2,
		HA_PROFILE_ID,
		ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
		ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		REPORTABLE_SENSOR_TIMER_MAX_SEC,
		(u8 *)&reportableChange
	);
    reportableChange = g_zcl_thermostatUICfgAttrs.probe_report_change; // raw soil ADC counts (x4)
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT2,
		HA_PROFILE_ID,
		ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
		0xF000,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		600, // 10-min heartbeat even if unchanged
		(u8 *)&reportableChange
	);
    reportableChange = g_zcl_thermostatUICfgAttrs.probe_report_change; // diode reference ADC counts (x4)
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT2,
		HA_PROFILE_ID,
		ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
		0xF001,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		600, // 10-min heartbeat
		(u8 *)&reportableChange
	);
#endif
#ifdef ZCL_THERMOSTAT_UI_CFG
    reportableChange = 1; // TX power current (dBm): report on any change
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
		0x0124,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		10800, // 3-hour heartbeat even if unchanged
		(u8 *)&reportableChange
	);
#endif
#ifdef ZCL_RELATIVE_HUMIDITY_MEASUREMENT
    reportableChange = 50;
	bdb_defaultReportingCfg(
		SENSOR_DEVICE_ENDPOINT,
		HA_PROFILE_ID,
		ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
		ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE,
		REPORTABLE_SENSOR_TIMER_MIN_SEC,
		REPORTABLE_SENSOR_TIMER_MAX_SEC,
		(u8 *)&reportableChange
	);
#endif
#ifdef ZCL_OCCUPANCY_SENSING
    	reportableChange = 0;
		bdb_defaultReportingCfg(
			SENSOR_DEVICE_ENDPOINT,
			HA_PROFILE_ID,
			ZCL_CLUSTER_MS_OCCUPANCY_SENSING,
			ZCL_ATTRID_OCCUPANCY,
			0,
			3600,
			(u8 *)&reportableChange
		);
#endif
#ifdef ZCL_DIHUMIDIFICATION_CONTROL
    	reportableChange = 0;
		bdb_defaultReportingCfg(
			SENSOR_DEVICE_ENDPOINT,
			HA_PROFILE_ID,
			ZCL_CLUSTER_HAVC_DIHUMIDIFICATION_CONTROL,
			ZCL_ATTRID_DHUM_COOLING,
			0,
			3600,
			(u8 *)&reportableChange
		);
#endif

//#if !USE_BLE
//	bdb_findBindMatchClusterSet(FIND_AND_BIND_CLUSTER_NUM, bdb_findBindClusterList);
//#endif
	/* Initialize BDB */
	bdb_init((af_simple_descriptor_t *)&sensorDevice_simpleDesc,
			&g_bdbCommissionSetting, &g_zbDemoBdbCb, drv_pm_deepSleep_flag_get() ? 0 : 1);

#if USE_BLE && defined(STARTUP_IN_BLE) && (!STARTUP_IN_BLE) // Startup in Zigbee ?
	u32 r = drv_disable_irq();
	switch_to_zb_context();
	drv_restore_irq(r);
#endif
#if (DEV_SERVICES & SERVICE_PLM)
	probe_apply_reporting(); // entries restored from NV keep their old settings otherwise
#endif
}

/*********************************************************************
 * @fn      user_init
 *
 * @brief   User level initialization code.
 *
 * @param   isRetention - if it is waking up with ram retention.
 *
 * @return  None
 */
#if defined(ARTECO_HW_TEST)
void hw_test_run(void);
#endif

void user_init(bool isRetention)
{
#if defined(ARTECO_HW_TEST)
	hw_test_run(); // bench test, never returns
#endif

#if ZBHCI_EN
	zbhciInit();
#endif

	if(!isRetention){

		/* Initialize user application */
		user_app_init();

	} else {
		/* Re-config phy when system recovery from deep sleep with retention */
		mac_phyReconfig();
	}
}
