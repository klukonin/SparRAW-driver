// SPDX-License-Identifier: ISC
/*
 * FTM (802.11 Fine Timing Measurement) через cfg80211 peer measurement
 * (nl80211 PEER_MEASUREMENT_START, `iw dev X measurement ftm_request`)
 * и калибровка задержек трактов TOF.
 *
 * Прошивка 6.2 (vendor и SparRAW) принимает сессию только с параметрами,
 * равными её возможностям (иначе WMI_TOF_SESSION_END со статусом 2):
 * один адресат, ASAP, 4 FTM в пачке, одна пачка, длительность 250 мкс,
 * initial_token 2. Адресат должен быть соединён с нами (прошивка ищет
 * соединение по MAC). Результат пачки — WMI_TOF_FTM_PER_DEST_RES (t1..t4 в
 * пс, первое FTM пачки без меток не входит), затем WMI_TOF_SESSION_END.
 *
 * Калибровка: WMI_TOF_SET_TX_RX_OFFSET живёт до перезагрузки прошивки, поэтому
 * драйвер шлёт её после каждой загрузки (wil_ftm_fw_ready). Для RTT важна
 * только сумма tx + rx каждого устройства; единица ~11,9 пс RTT на 1
 * (стенд 2026-10-20). -1 — оставить значения прошивки.
 */
#include <linux/moduleparam.h>
#include <linux/math64.h>
#include "wil6210.h"
#include "wmi.h"

static int ftm_tx_offset = -1;
module_param(ftm_tx_offset, int, 0644);
MODULE_PARM_DESC(ftm_tx_offset,
		 " TOF TX delay offset (FW units), sent after every FW load; -1 = FW default");

static int ftm_rx_offset = -1;
module_param(ftm_rx_offset, int, 0644);
MODULE_PARM_DESC(ftm_rx_offset,
		 " TOF RX delay offset (FW units), sent after every FW load; -1 = FW default");

/* Возможности TOF прошивки 6.2 (tof__init_session_defaults). */
#define WIL_FTM_PER_BURST	4
#define WIL_FTM_INITIAL_TOKEN	2

const struct cfg80211_pmsr_capabilities wil_pmsr_capa = {
	.max_peers = 1,
	.ftm = {
		.supported = 1,
		.asap = 1,
		.non_asap = 0,
		.preambles = BIT(NL80211_PREAMBLE_DMG),
		.bandwidths = BIT(NL80211_CHAN_WIDTH_20_NOHT) |
			      BIT(NL80211_CHAN_WIDTH_20),
		.max_bursts_exponent = 0,
		.max_ftms_per_burst = WIL_FTM_PER_BURST,
	},
};

struct wil_tof_session_start_1 {
	struct wmi_tof_session_start_cmd hdr;
	struct wmi_ftm_dest_info dest;
} __packed;

/* Ожидание итога сессии, если в запросе не задан свой срок (req->timeout):
 * одна пачка asap по 250 мкс заканчивается за доли секунды.
 */
#define WIL_FTM_TIMEOUT_MS 2000

/* Прошивка не прислала WMI_TOF_SESSION_END (сессия потерялась, пир
 * пропал): без этого wil->ftm.req висел бы, и все следующие запросы
 * получали бы -EBUSY до abort/сброса.
 */
static void wil_ftm_timeout_work(struct work_struct *work)
{
	struct wil6210_priv *wil = container_of(to_delayed_work(work),
						struct wil6210_priv,
						ftm.timeout_work);
	struct cfg80211_pmsr_result r = {
		.type = NL80211_PMSR_TYPE_FTM,
		.status = NL80211_PMSR_STATUS_TIMEOUT,
		.final = 1,
	};
	struct cfg80211_pmsr_request *req;
	struct wireless_dev *wdev;

	mutex_lock(&wil->ftm.lock);
	req = wil->ftm.req;
	wdev = wil->ftm.wdev;
	/* срок проверяется: опоздавший запуск от прошлой сессии не трогает
	 * новую
	 */
	if (!req || time_before(jiffies, wil->ftm.deadline)) {
		mutex_unlock(&wil->ftm.lock);
		return;
	}
	wil_err(wil, "FTM: no session end from FW, timed out\n");
	if (!wil->ftm.reported) {
		ether_addr_copy(r.addr, req->peers[0].addr);
		r.host_time = ktime_get_boottime_ns();
		cfg80211_pmsr_report(wdev, req, &r, GFP_KERNEL);
	}
	wil->ftm.req = NULL;
	mutex_unlock(&wil->ftm.lock);
	cfg80211_pmsr_complete(wdev, req, GFP_KERNEL);
}

void wil_ftm_init(struct wil6210_priv *wil)
{
	mutex_init(&wil->ftm.lock);
	INIT_DELAYED_WORK(&wil->ftm.timeout_work, wil_ftm_timeout_work);
}

/* Завершить висящий запрос (сброс прошивки, down): событий по нему уже не будет. */
void wil_ftm_reset(struct wil6210_priv *wil)
{
	struct cfg80211_pmsr_request *req;
	struct wireless_dev *wdev;

	mutex_lock(&wil->ftm.lock);
	req = wil->ftm.req;
	wdev = wil->ftm.wdev;
	wil->ftm.req = NULL;
	mutex_unlock(&wil->ftm.lock);
	if (req)
		cfg80211_pmsr_complete(wdev, req, GFP_KERNEL);
}

void wil_ftm_fw_ready(struct wil6210_priv *wil)
{
	struct wmi_tof_set_tx_rx_offset_cmd cmd = {};
	struct {
		struct wmi_cmd_hdr wmi;
		struct wmi_tof_set_tx_rx_offset_event evt;
	} __packed reply = { .evt = { .status = WMI_FW_STATUS_FAILURE } };
	int rc;

	if (ftm_tx_offset < 0 || ftm_rx_offset < 0)
		return;
	if (!test_bit(WMI_FW_CAPABILITY_FTM, wil->fw_capabilities))
		return;
	cmd.tx_offset = cpu_to_le32(ftm_tx_offset);
	cmd.rx_offset = cpu_to_le32(ftm_rx_offset);
	rc = wmi_call(wil, WMI_TOF_SET_TX_RX_OFFSET_CMDID, 0, &cmd, sizeof(cmd),
		      WMI_TOF_SET_TX_RX_OFFSET_EVENTID, &reply, sizeof(reply),
		      100);
	if (rc || reply.evt.status != WMI_FW_STATUS_SUCCESS)
		wil_err(wil, "FTM: set tx/rx offset %d/%d failed, rc %d status %d\n",
			ftm_tx_offset, ftm_rx_offset, rc, reply.evt.status);
	else
		wil_info(wil, "FTM: tx/rx offset %d/%d\n", ftm_tx_offset,
			 ftm_rx_offset);
}

int wil_ftm_start(struct wiphy *wiphy, struct wireless_dev *wdev,
		  struct cfg80211_pmsr_request *req)
{
	struct wil6210_priv *wil = wiphy_to_wil(wiphy);
	struct wil6210_vif *vif = wdev_to_vif(wil, wdev);
	struct cfg80211_pmsr_request_peer *peer;
	struct wil_tof_session_start_1 cmd = {};
	int rc;

	if (!test_bit(WMI_FW_CAPABILITY_FTM, wil->fw_capabilities))
		return -EOPNOTSUPP;
	if (req->n_peers != 1)
		return -EOPNOTSUPP;
	peer = &req->peers[0];
	if (!peer->ftm.requested)
		return -EINVAL;
	/* 0 — «без предпочтений» в nl80211; иное должно совпасть с прошивкой */
	if ((peer->ftm.ftms_per_burst &&
	     peer->ftm.ftms_per_burst != WIL_FTM_PER_BURST) ||
	    peer->ftm.num_bursts_exp != 0 ||
	    (peer->ftm.burst_duration != WMI_TOF_BURST_DURATION_250_USEC &&
	     peer->ftm.burst_duration != WMI_TOF_BURST_DURATION_NO_PREFERENCES) ||
	    peer->ftm.burst_period != 0) {
		wil_err(wil, "FTM: FW supports only asap, 4 FTMs per burst, 1 burst, 250 us\n");
		return -EINVAL;
	}

	mutex_lock(&wil->ftm.lock);
	if (wil->ftm.req) {
		rc = -EBUSY;
		goto out;
	}
	if (wil_find_cid(wil, vif->mid, peer->addr) < 0) {
		wil_err(wil, "FTM: peer %pM is not connected\n", peer->addr);
		rc = -ENOTCONN;
		goto out;
	}
	wil->ftm.session_id++;
	cmd.hdr.session_id = cpu_to_le32(wil->ftm.session_id);
	cmd.hdr.num_of_dest = cpu_to_le16(1);
	cmd.dest.flags = WMI_TOF_SESSION_START_FLAG_ASAP;
	cmd.dest.initial_token = WIL_FTM_INITIAL_TOKEN;
	cmd.dest.num_of_ftm_per_burst = WIL_FTM_PER_BURST;
	cmd.dest.num_of_bursts_exp = 0;
	cmd.dest.burst_duration = WMI_TOF_BURST_DURATION_250_USEC;
	cmd.dest.burst_period = 0;
	ether_addr_copy(cmd.dest.dst_mac, peer->addr);
	rc = wmi_send(wil, WMI_TOF_SESSION_START_CMDID, vif->mid, &cmd,
		      sizeof(cmd));
	if (rc)
		goto out;
	wil->ftm.req = req;
	wil->ftm.wdev = wdev;
	wil->ftm.reported = false;
	{
		unsigned long t = msecs_to_jiffies(req->timeout ?:
						   WIL_FTM_TIMEOUT_MS);

		wil->ftm.deadline = jiffies + t;
		mod_delayed_work(system_wq, &wil->ftm.timeout_work, t);
	}
out:
	mutex_unlock(&wil->ftm.lock);
	return rc;
}

void wil_ftm_abort(struct wiphy *wiphy, struct wireless_dev *wdev,
		   struct cfg80211_pmsr_request *req)
{
	struct wil6210_priv *wil = wiphy_to_wil(wiphy);
	bool mine;

	/* У прошивки нет отмены сессии: её события после этого отбрасываются
	 * (wil->ftm.req == NULL).
	 */
	mutex_lock(&wil->ftm.lock);
	mine = wil->ftm.req == req;
	if (mine)
		wil->ftm.req = NULL;
	mutex_unlock(&wil->ftm.lock);
	if (mine)
		cfg80211_pmsr_complete(wdev, req, GFP_KERNEL);
}

static u64 wil_ftm_ts(const u8 *t)
{
	return (u64)get_unaligned_le32(t) | ((u64)get_unaligned_le16(t + 4) << 32);
}

/* пс RTT -> мм: c/2 = 0,149896229 мм/пс */
static s64 wil_ftm_rtt_to_mm(s64 rtt)
{
	return div_s64(rtt * 299792458LL, 2000000000);
}

/* Дисперсия (n·Σx² − (Σx)²) / n² — без ошибки от усечения среднего. */
static s64 wil_ftm_var(u64 sq, s64 sum, int n)
{
	return div64_u64((u64)n * sq - (u64)(sum * sum), (u64)n * n);
}

static void wil_ftm_fill_stats(struct cfg80211_pmsr_ftm_result *f,
			       const struct wmi_responder_ftm_res *res, int n)
{
	s64 rtt_sum = 0, rtt_min = S64_MAX, rtt_max = S64_MIN;
	s64 d_sum = 0, d_min = S64_MAX, d_max = S64_MIN;
	u64 rtt_sq = 0, d_sq = 0;
	int i;

	for (i = 0; i < n; i++) {
		s64 t1 = wil_ftm_ts(res[i].t1), t2 = wil_ftm_ts(res[i].t2);
		s64 t3 = wil_ftm_ts(res[i].t3), t4 = wil_ftm_ts(res[i].t4);
		s64 rtt = (t4 - t1) - (t3 - t2);
		s64 d = wil_ftm_rtt_to_mm(rtt);

		rtt_sum += rtt;
		rtt_sq += rtt * rtt;
		rtt_min = min(rtt_min, rtt);
		rtt_max = max(rtt_max, rtt);
		d_sum += d;
		d_sq += d * d;
		d_min = min(d_min, d);
		d_max = max(d_max, d);
	}
	f->rtt_avg = div_s64(rtt_sum, n);
	f->rtt_variance = wil_ftm_var(rtt_sq, rtt_sum, n);
	f->rtt_spread = rtt_max - rtt_min;
	f->dist_avg = div_s64(d_sum, n);
	f->dist_variance = wil_ftm_var(d_sq, d_sum, n);
	f->dist_spread = d_max - d_min;
	f->rtt_avg_valid = 1;
	f->rtt_variance_valid = 1;
	f->rtt_spread_valid = 1;
	f->dist_avg_valid = 1;
	f->dist_variance_valid = 1;
	f->dist_spread_valid = 1;
}

void wil_ftm_evt_per_dest_res(struct wil6210_vif *vif,
			      const struct wmi_tof_ftm_per_dest_res_event *evt,
			      int len)
{
	struct wil6210_priv *wil = vif_to_wil(vif);
	struct cfg80211_pmsr_result r = {};
	int n;

	if (len < sizeof(*evt)) {
		wil_err(wil, "FTM: short per_dest_res %d\n", len);
		return;
	}
	n = evt->actual_ftm_per_burst;
	if (len < sizeof(*evt) + n * sizeof(evt->responder_ftm_res[0])) {
		wil_err(wil, "FTM: per_dest_res %d < %d measurements\n", len, n);
		return;
	}

	mutex_lock(&wil->ftm.lock);
	if (!wil->ftm.req ||
	    le32_to_cpu(evt->session_id) != wil->ftm.session_id)
		goto out;

	r.type = NL80211_PMSR_TYPE_FTM;
	ether_addr_copy(r.addr, evt->dst_mac);
	r.host_time = ktime_get_boottime_ns();
	r.final = !!(evt->flags & WMI_PER_DEST_RES_REQ_END);
	r.ftm.burst_index = 0;
	r.ftm.num_bursts_exp = evt->responder_num_ftm_bursts_exp;
	r.ftm.burst_duration = evt->responder_burst_duration;
	r.ftm.ftms_per_burst = evt->responder_num_ftm_per_burst;
	r.ftm.burst_period = le16_to_cpu(evt->responder_burst_period);
	if (evt->status != WMI_PER_DEST_RES_NO_ERROR || n == 0) {
		r.status = NL80211_PMSR_STATUS_FAILURE;
		r.ftm.failure_reason =
			evt->status == WMI_PER_DEST_RES_PARAM_DONT_MATCH ?
			NL80211_PMSR_FTM_FAILURE_BAD_CHANGED_PARAMS :
			NL80211_PMSR_FTM_FAILURE_NO_RESPONSE;
		r.final = 1;
	} else {
		r.status = NL80211_PMSR_STATUS_SUCCESS;
		wil_ftm_fill_stats(&r.ftm, evt->responder_ftm_res, n);
	}
	cfg80211_pmsr_report(wil->ftm.wdev, wil->ftm.req, &r, GFP_KERNEL);
	if (r.final)
		wil->ftm.reported = true;
out:
	mutex_unlock(&wil->ftm.lock);
}

void wil_ftm_evt_session_end(struct wil6210_vif *vif,
			     const struct wmi_tof_session_end_event *evt,
			     int len)
{
	struct wil6210_priv *wil = vif_to_wil(vif);
	struct cfg80211_pmsr_request *req;
	struct wireless_dev *wdev;

	if (len < sizeof(*evt))
		return;
	mutex_lock(&wil->ftm.lock);
	req = wil->ftm.req;
	wdev = wil->ftm.wdev;
	if (!req || le32_to_cpu(evt->session_id) != wil->ftm.session_id) {
		mutex_unlock(&wil->ftm.lock);
		return;
	}
	if (!wil->ftm.reported) {
		struct cfg80211_pmsr_result r = {
			.type = NL80211_PMSR_TYPE_FTM,
			.final = 1,
			.host_time = ktime_get_boottime_ns(),
		};

		ether_addr_copy(r.addr, req->peers[0].addr);
		switch (evt->status) {
		case WMI_TOF_SESSION_END_BUSY:
			r.status = NL80211_PMSR_STATUS_FAILURE;
			r.ftm.failure_reason = NL80211_PMSR_FTM_FAILURE_PEER_BUSY;
			break;
		case WMI_TOF_SESSION_END_PARAMS_ERROR:
			r.status = NL80211_PMSR_STATUS_REFUSED;
			break;
		default:
			r.status = NL80211_PMSR_STATUS_FAILURE;
			r.ftm.failure_reason = NL80211_PMSR_FTM_FAILURE_UNSPECIFIED;
		}
		cfg80211_pmsr_report(wdev, req, &r, GFP_KERNEL);
	}
	wil->ftm.req = NULL;
	mutex_unlock(&wil->ftm.lock);
	cfg80211_pmsr_complete(wdev, req, GFP_KERNEL);
}
