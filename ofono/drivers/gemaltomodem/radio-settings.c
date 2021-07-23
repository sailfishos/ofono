/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017 Intel Corporation. All rights reserved.
 *  Copyright (C) 2021 Sergey Matyukevich. All rights reserved.
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "gemaltomodem.h"

static const char *none_prefix[] = { NULL };
static const char *sxrat_prefix[] = { "^SXRAT:", NULL };

struct radio_settings_data {
	GAtChat *chat;
};

static void sxrat_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	unsigned int mode;
	struct ofono_error error;
	int value, pref1, pref2;
	GAtResultIter iter;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SXRAT:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &value))
		goto error;

	g_at_result_iter_next_number_default(&iter, -1, &pref1);
	g_at_result_iter_next_number_default(&iter, -1, &pref2);

	DBG("mode %d pref1 %d pref2 %d", value, pref1, pref2);

	switch (value) {
	case 0:
		mode = OFONO_RADIO_ACCESS_MODE_GSM;
		break;
	case 1:
		mode = OFONO_RADIO_ACCESS_MODE_GSM |
			OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case 2:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		break;
	case 3:
		mode = OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 4:
		mode = OFONO_RADIO_ACCESS_MODE_UMTS |
			OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 5:
		mode = OFONO_RADIO_ACCESS_MODE_GSM |
			OFONO_RADIO_ACCESS_MODE_LTE;
		break;
	case 6:
		mode = OFONO_RADIO_ACCESS_MODE_ANY;
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, mode, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void gemalto_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("");

	if (g_at_chat_send(rsd->chat, "AT^SXRAT?", sxrat_prefix,
				sxrat_query_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		g_free(cbd);
	}
}

static void sxrat_modify_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void gemalto_set_rat_mode(struct ofono_radio_settings *rs,
				unsigned int m,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	int val= 6, p1 = 3, p2 = 2;
	char buf[20];

	DBG("mode %d", m);

	switch (m) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		val = 6;
		p1 = 3;
		p2 = 2;
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		val = 0;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		val = 2;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		val = 3;
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS|OFONO_RADIO_ACCESS_MODE_GSM:
		val = 1;
		p1 = 2;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE|OFONO_RADIO_ACCESS_MODE_UMTS:
		val = 4;
		p1 = 3;
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE|OFONO_RADIO_ACCESS_MODE_GSM:
		val = 5;
		p1 = 3;
		break;
	}

	if (val == 6)
		snprintf(buf, sizeof(buf), "AT^SXRAT=%u,%u,%u", val, p1, p2);
	else if (val == 1 || val == 4 || val == 5)
		snprintf(buf, sizeof(buf), "AT^SXRAT=%u,%u", val, p1);
	else
		snprintf(buf, sizeof(buf), "AT^SXRAT=%u", val);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
				sxrat_modify_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void sxrat_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_radio_settings *rs = user_data;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_radio_settings_remove(rs);
		return;
	}

	ofono_radio_settings_register(rs);
}

static int gemalto_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct radio_settings_data *rsd;

	DBG("");

	rsd = g_new0(struct radio_settings_data, 1);

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);

	g_at_chat_send(rsd->chat, "AT^SXRAT=?", sxrat_prefix,
			sxrat_support_cb, rs, NULL);

	return 0;
}

static void gemalto_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);

	DBG("");

	ofono_radio_settings_set_data(rs, NULL);
	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static const struct ofono_radio_settings_driver driver = {
	.name			= "gemaltomodem",
	.probe			= gemalto_radio_settings_probe,
	.remove			= gemalto_radio_settings_remove,
	.query_rat_mode		= gemalto_query_rat_mode,
	.set_rat_mode		= gemalto_set_rat_mode
};

void gemalto_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void gemalto_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
