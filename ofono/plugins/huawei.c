/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/audio-settings.h>
#include <ofono/radio-settings.h>
#include <ofono/voicecall.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/phonebook.h>
#include <ofono/message-waiting.h>
#include <ofono/cdma-netreg.h>
#include <ofono/cdma-connman.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *gcap_prefix[] = { "+GCAP:", NULL };
static const char *rfswitch_prefix[] = { "^RFSWITCH:", NULL };
static const char *sysinfo_prefix[] = { "^SYSINFO:", NULL };
static const char *ussdmode_prefix[] = { "^USSDMODE:", NULL };
static const char *dialmode_prefix[] = { "^DIALMODE:", NULL };
static const char *cvoice_prefix[] = { "^CVOICE:", NULL };

enum {
	SIM_STATE_INVALID_OR_LOCKED =	0,
	SIM_STATE_VALID =		1,
	SIM_STATE_INVALID_CS =		2,
	SIM_STATE_INVALID_PS =		3,
	SIM_STATE_INVALID_PS_AND_CS =	4,
	SIM_STATE_ROMSIM =		240,
	SIM_STATE_NOT_EXISTENT =	255,
};

struct huawei_data {
	GAtChat *modem;
	GAtChat *pcui;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	gboolean have_sim;
	int sim_state;
	guint sysinfo_poll_source;
	guint sysinfo_poll_count;
	struct cb_data *online_cbd;
	const char *offline_command;
	gboolean have_voice;
	gboolean have_gsm;
	gboolean have_cdma;
	gboolean have_ndis;
	gboolean have_ussdmode;
};

static int huawei_probe(struct ofono_modem *modem)
{
	struct huawei_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct huawei_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void huawei_remove(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after potential enable polling */
	if (data->sysinfo_poll_source > 0)
		g_source_remove(data->sysinfo_poll_source);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->pcui);

	g_free(data);
}

static void huawei_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void ussdmode_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;
	gint ussdmode;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^USSDMODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ussdmode))
		return;

	if (ussdmode == 1)
		data->have_ussdmode = TRUE;
}

static void ussdmode_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^USSDMODE:"))
		return;

	/* Query current USSD mode */
	g_at_chat_send(data->pcui, "AT^USSDMODE?", ussdmode_prefix,
					ussdmode_query_cb, data, NULL);
}

static void dialmode_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;
	gint dialmode, cdc_spec;
	const char *str = "unknown";

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^DIALMODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &dialmode))
		return;

	if (g_at_result_iter_next_number(&iter, &cdc_spec)) {
		switch (cdc_spec) {
		case 0:
			str = "none";
			break;
		case 1:
			str = "Modem port";
			break;
		case 2:
			str = "NDIS port";
			break;
		case 3:
			str = "Modem and NDIS port";
			break;
		}
	}

	switch (dialmode) {
	case 0:
		ofono_info("Modem support (CDC support: %s)", str);
		data->have_ndis = FALSE;
		break;
	case 1:
		ofono_info("NDIS support (CDC support: %s)", str);
		data->have_ndis = TRUE;
		break;
	case 2:
		ofono_info("Modem and NDIS support (CDC support: %s)", str);
		data->have_ndis = TRUE;
		break;
	}
}

static void dialmode_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^DIALMODE:"))
		return;

	/* Query current NDIS mode */
	g_at_chat_send(data->pcui, "AT^DIALMODE?", dialmode_prefix,
					dialmode_query_cb, data, NULL);
}

static void cvoice_query_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	gint mode, rate, bits, period;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CVOICE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &rate))
		return;

	if (!g_at_result_iter_next_number(&iter, &bits))
		return;

	if (!g_at_result_iter_next_number(&iter, &period))
		return;

	data->have_voice = TRUE;

	ofono_info("Voice channel: %d Hz, %d bits, %dms period",
						rate, bits, period);

	/* Check available voice ports */
	g_at_chat_send(data->pcui, "AT^DDSETEX=?", none_prefix,
						NULL, NULL, NULL);
}

static void cvoice_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^CVOICE:"))
		return;

	/* Query current voice setting */
	g_at_chat_send(data->pcui, "AT^CVOICE?", cvoice_prefix,
					cvoice_query_cb, modem, NULL);
}

static void simst_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int sim_state;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SIMST:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &sim_state))
		return;

	DBG("%d -> %d", data->sim_state, sim_state);

	data->sim_state = sim_state;
}

static gboolean parse_sysinfo_result(GAtResult *result, int *srv_status,
					int *srv_domain, int *sim_state)
{
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SYSINFO:"))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, srv_status))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, srv_domain))
		return FALSE;

	if (!g_at_result_iter_skip_next(&iter))
		return FALSE;

	if (!g_at_result_iter_skip_next(&iter))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, sim_state))
		return FALSE;

	return TRUE;
}

static void shutdown_device(struct huawei_data *data)
{
	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->pcui);
	g_at_chat_unregister_all(data->pcui);

	g_at_chat_unref(data->pcui);
	data->pcui = NULL;
}

static void cfun_offline(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		shutdown_device(data);
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static gboolean sysinfo_enable_check(gpointer user_data);

static void sysinfo_enable_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	int srv_status, srv_domain, sim_state;

	if (!ok)
		goto failure;

	if (parse_sysinfo_result(result, &srv_status, &srv_domain,
						&sim_state) == FALSE)
		goto failure;

	DBG("%d -> %d", data->sim_state, sim_state);

	data->sim_state = sim_state;

	if (sim_state == SIM_STATE_NOT_EXISTENT) {
		data->sysinfo_poll_count++;

		if (data->sysinfo_poll_count > 5)
			goto failure;

		data->sysinfo_poll_source = g_timeout_add_seconds(1,
						sysinfo_enable_check, modem);
		return;
	}

	data->have_sim = TRUE;

	/* Switch data carrier detect signal off */
	g_at_chat_send(data->modem, "AT&C0", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->pcui, "AT&C0", NULL, NULL, NULL, NULL);

	/*
	 * Ensure that the modem is using GSM character set and not IRA,
	 * otherwise weirdness with umlauts and other non-ASCII characters
	 * can result
	 */
	g_at_chat_send(data->modem, "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);
	g_at_chat_send(data->pcui, "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);

	/* Query current device settings */
	g_at_chat_send(data->pcui, "AT^U2DIAG?", none_prefix,
						NULL, NULL, NULL);

	/* Query current port settings */
	g_at_chat_send(data->pcui, "AT^GETPORTMODE", none_prefix,
						NULL, NULL, NULL);

	/* Check USSD mode support */
	g_at_chat_send(data->pcui, "AT^USSDMODE=?", ussdmode_prefix,
					ussdmode_support_cb, data, NULL);

	/* Check NDIS mode support */
	g_at_chat_send(data->pcui, "AT^DIALMODE=?", dialmode_prefix,
					dialmode_support_cb, data, NULL);

	/* Check for voice support */
	g_at_chat_send(data->pcui, "AT^CVOICE=?", cvoice_prefix,
					cvoice_support_cb, modem, NULL);

	/* For CDMA we use AlwaysOnline so we leave the modem online. */
	if (data->have_gsm == FALSE && data->have_cdma == TRUE) {
		ofono_modem_set_boolean(modem, "AlwaysOnline", TRUE);
		ofono_modem_set_powered(modem, TRUE);
		return;
	}

	if (g_at_chat_send(data->pcui, data->offline_command, none_prefix,
					cfun_offline, modem, NULL) > 0)
		return;

failure:
	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);
}

static gboolean sysinfo_enable_check(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	data->sysinfo_poll_source = 0;

	g_at_chat_send(data->pcui, "AT^SYSINFO", sysinfo_prefix,
					sysinfo_enable_cb, modem, NULL);

	return FALSE;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		shutdown_device(data);
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	/* Follow sim state changes */
	g_at_chat_register(data->pcui, "^SIMST:", simst_notify,
						FALSE, modem, NULL);

	data->sysinfo_poll_count = 0;

	sysinfo_enable_check(modem);
}

static void rfswitch_support(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	if (data->have_gsm == FALSE && data->have_cdma == TRUE) {
		data->offline_command = "AT+CFUN=5";
		goto done;
	}

	if (!ok)
		data->offline_command = "AT+CFUN=5";
	else
		data->offline_command = "AT+CFUN=7";

done:
	g_at_chat_send(data->pcui, "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);
}

static void gcap_support(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	const char *gcap;

	if (!ok)
		goto done;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+GCAP:"))
		goto done;

	while (g_at_result_iter_next_unquoted_string(&iter, &gcap)) {
		if (*gcap == '\0')
			break;

		if (!strcmp(gcap, "+CGSM"))
			data->have_gsm = TRUE;
		else if (!strcmp(gcap, "+CIS707-A"))
			data->have_cdma = TRUE;
	}

done:
	g_at_chat_send(data->pcui, "AT^RFSWITCH=?", rfswitch_prefix,
					rfswitch_support, modem, NULL);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	GAtChat *chat = at_util_open_device(modem, key, huawei_debug, debug,
						NULL);

	if (!chat)
		return NULL;

	g_at_chat_add_terminator(chat, "COMMAND NOT SUPPORT", -1, FALSE);
	g_at_chat_add_terminator(chat, "TOO MANY PARAMETERS", -1, FALSE);

	return chat;
}

static void modem_disconnect(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	if (data == NULL) {
		DBG("Modem has already been removed");
		return;
	}

	ofono_warn("Modem channel disconnected");

	/* clean and close modem device */
	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);
	g_at_chat_unref(data->modem);
	data->modem = NULL;

	/* close gprs context driver */
	ofono_gprs_context_remove(data->gc);

	/* reopen modem channel */
	data->modem = open_device(modem, "Modem", "Modem: ");

	if (data->modem == NULL) {
		DBG("Can't reopen device");
		return;
	}

	/* configure modem channel */
	g_at_chat_set_disconnect_function(data->modem, modem_disconnect, modem);
	g_at_chat_set_slave(data->modem, data->pcui);
	g_at_chat_send(data->modem, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);

	/* reopen gprs context driver */
	data->gc = ofono_gprs_context_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->modem);

	if (data->gprs && data->gc)
		ofono_gprs_add_context(data->gprs, data->gc);
}

static int huawei_enable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	g_at_chat_set_disconnect_function(data->modem, modem_disconnect, modem);

	data->pcui = open_device(modem, "Pcui", "PCUI: ");
	if (data->pcui == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_set_slave(data->modem, data->pcui);

	g_at_chat_send(data->modem, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->pcui, "ATE0 +CMEE=1", NULL, NULL, NULL, NULL);

	data->sim_state = SIM_STATE_NOT_EXISTENT;

	/* Check for GSM capabilities */
	g_at_chat_send(data->pcui, "ATI", gcap_prefix,
					gcap_support, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->pcui);
	data->pcui = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int huawei_disable(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->pcui);
	g_at_chat_unregister_all(data->pcui);

	/* Cleanup potential online enable polling */
	if (data->sysinfo_poll_source > 0) {
		g_source_remove(data->sysinfo_poll_source);
		data->sysinfo_poll_source = 0;

		g_free(data->online_cbd);
		data->online_cbd = NULL;
	}

	g_at_chat_send(data->pcui, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static gboolean sysinfo_online_check(gpointer user_data);

static void sysinfo_online_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct huawei_data *data = user_data;
	ofono_modem_online_cb_t cb = data->online_cbd->cb;
	int srv_status, srv_domain, sim_state;

	if (!ok)
		goto failure;

	if (parse_sysinfo_result(result, &srv_status, &srv_domain,
							&sim_state) == FALSE)
		goto failure;

	DBG("%d -> %d", data->sim_state, sim_state);

	data->sim_state = sim_state;

	/* Valid service status and at minimum PS domain */
	if (srv_status > 0 && srv_domain > 1) {
		CALLBACK_WITH_SUCCESS(cb, data->online_cbd->data);
		goto done;
	}

	switch (sim_state) {
	case SIM_STATE_VALID:
	case SIM_STATE_INVALID_CS:
	case SIM_STATE_INVALID_PS:
	case SIM_STATE_INVALID_PS_AND_CS:
	case SIM_STATE_ROMSIM:
		CALLBACK_WITH_SUCCESS(cb, data->online_cbd->data);
		goto done;
	}

	data->sysinfo_poll_count++;

	if (data->sysinfo_poll_count > 15)
		goto failure;

	data->sysinfo_poll_source = g_timeout_add_seconds(2,
						sysinfo_online_check, data);
	return;

failure:
	CALLBACK_WITH_FAILURE(cb, data->online_cbd->data);

done:
	g_free(data->online_cbd);
	data->online_cbd = NULL;
}

static gboolean sysinfo_online_check(gpointer user_data)
{
	struct huawei_data *data = user_data;

	data->sysinfo_poll_source = 0;

	g_at_chat_send(data->pcui, "AT^SYSINFO", sysinfo_prefix,
					sysinfo_online_cb, data, NULL);

	return FALSE;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct huawei_data *data = ofono_modem_get_data(modem);

	if (!ok) {
		ofono_modem_online_cb_t cb = data->online_cbd->cb;

		CALLBACK_WITH_FAILURE(cb, data->online_cbd->data);

		g_free(data->online_cbd);
		data->online_cbd = NULL;
		return;
	}

	data->sysinfo_poll_count = 0;

	sysinfo_online_check(data);
}

static void set_offline_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void huawei_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (online == TRUE) {
		data->online_cbd = cb_data_new(cb, user_data);

		if (g_at_chat_send(data->pcui, "AT+CFUN=1", none_prefix,
					set_online_cb, modem, NULL) > 0)
			return;

		g_free(data->online_cbd);
		data->online_cbd = NULL;
	} else {
		struct cb_data *cbd = cb_data_new(cb, user_data);

		if (g_at_chat_send(data->pcui, data->offline_command,
				none_prefix, set_offline_cb, cbd, g_free) > 0)
			return;

		g_free(cbd);
	}

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static void huawei_pre_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim = NULL;

	DBG("%p", modem);

	if (data->have_gsm == TRUE) {
		ofono_devinfo_create(modem, 0, "atmodem", data->pcui);
		sim = ofono_sim_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->pcui);
	} else if (data->have_cdma == TRUE) {
		ofono_devinfo_create(modem, 0, "cdmamodem", data->pcui);

		/* Create SIM atom only if SIM is not embedded */
		if (data->sim_state != SIM_STATE_ROMSIM)
			sim = ofono_sim_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem-noef", data->pcui);
	}

	if (sim && data->have_sim == TRUE)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void huawei_post_sim(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->have_voice == TRUE) {
		ofono_voicecall_create(modem, 0, "huaweimodem", data->pcui);
		ofono_audio_settings_create(modem, 0,
						"huaweimodem", data->pcui);
	}

	if (data->have_gsm == TRUE) {
		ofono_phonebook_create(modem, 0, "atmodem", data->pcui);
		ofono_radio_settings_create(modem, 0,
						"huaweimodem", data->pcui);

		ofono_sms_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->pcui);

		data->gprs = ofono_gprs_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->pcui);
		data->gc = ofono_gprs_context_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->modem);

		if (data->gprs && data->gc)
			ofono_gprs_add_context(data->gprs, data->gc);
	}
}

static void huawei_post_online(struct ofono_modem *modem)
{
	struct huawei_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->have_gsm == TRUE) {
		ofono_netreg_create(modem, OFONO_VENDOR_HUAWEI,
						"atmodem", data->pcui);

		ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
						"atmodem", data->pcui);
		if (data->have_ussdmode == TRUE)
			ofono_ussd_create(modem, 0, "huaweimodem", data->pcui);
		else
			ofono_ussd_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
							"atmodem", data->pcui);
	} else if (data->have_cdma == TRUE) {
		ofono_cdma_netreg_create(modem, 0, "huaweimodem", data->pcui);

		ofono_cdma_connman_create(modem, OFONO_VENDOR_HUAWEI,
						"cdmamodem", data->modem);
	}

	if (data->have_voice == TRUE) {
		struct ofono_message_waiting *mw;

		ofono_call_forwarding_create(modem, 0, "atmodem", data->pcui);
		ofono_call_settings_create(modem, 0, "atmodem", data->pcui);
		ofono_call_barring_create(modem, 0, "atmodem", data->pcui);

		mw = ofono_message_waiting_create(modem);
		if (mw)
			ofono_message_waiting_register(mw);
	}
}

static struct ofono_modem_driver huawei_driver = {
	.name		= "huawei",
	.probe		= huawei_probe,
	.remove		= huawei_remove,
	.enable		= huawei_enable,
	.disable	= huawei_disable,
	.set_online	= huawei_set_online,
	.pre_sim	= huawei_pre_sim,
	.post_sim	= huawei_post_sim,
	.post_online	= huawei_post_online,
};

static int huawei_init(void)
{
	return ofono_modem_driver_register(&huawei_driver);
}

static void huawei_exit(void)
{
	ofono_modem_driver_unregister(&huawei_driver);
}

OFONO_PLUGIN_DEFINE(huawei, "HUAWEI Mobile modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, huawei_init, huawei_exit)
