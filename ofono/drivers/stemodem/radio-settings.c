/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
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

#include "stemodem.h"

static const char *none_prefix[] = { NULL };
static const char *cfun_prefix[] = { "+CFUN:", NULL };

struct radio_settings_data {
	GAtChat *chat;
};

enum ste_radio_mode {
	STE_RADIO_OFF =	0,
	STE_RADIO_ON =		1,
	STE_RADIO_FLIGHT_MODE = 4,
	STE_RADIO_GSM_ONLY =	5,
	STE_RADIO_WCDMA_ONLY =	6
};

static gboolean ste_mode_to_ofono_mode(enum ste_radio_mode stemode,
				unsigned int *mode)
{
	switch (stemode) {
	case STE_RADIO_ON:
		*mode = OFONO_RADIO_ACCESS_MODE_ANY;
		return TRUE;
	case STE_RADIO_GSM_ONLY:
		*mode = OFONO_RADIO_ACCESS_MODE_GSM;
		return TRUE;
	case STE_RADIO_WCDMA_ONLY:
		*mode = OFONO_RADIO_ACCESS_MODE_UMTS;
		return TRUE;
	case STE_RADIO_OFF:
	case STE_RADIO_FLIGHT_MODE:
		break;
	}

	return FALSE;
}

static gboolean ofono_mode_to_ste_mode(unsigned int mode,
				enum ste_radio_mode *stemode)
{
	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		*stemode = STE_RADIO_ON;
		return TRUE;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		*stemode = STE_RADIO_GSM_ONLY;
		return TRUE;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		*stemode = STE_RADIO_WCDMA_ONLY;
		return TRUE;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		break;
	}

	return FALSE;
}

static void rat_query_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_query_cb_t cb = cbd->cb;
	unsigned int mode;
	struct ofono_error error;
	GAtResultIter iter;
	int value;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CFUN:"))
		goto err;

	if (!g_at_result_iter_next_number(&iter, &value))
		goto err;

	if (!ste_mode_to_ofono_mode(value, &mode))
		goto err;

	CALLBACK_WITH_SUCCESS(cb, mode, cbd->data);

	return;

err:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void ste_query_rat_mode(struct ofono_radio_settings *rs,
				ofono_radio_settings_rat_mode_query_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(rsd->chat, "AT+CFUN?", cfun_prefix,
					rat_query_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		g_free(cbd);
	}
}

static void rat_modify_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_radio_settings_rat_mode_set_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void ste_set_rat_mode(struct ofono_radio_settings *rs, unsigned int mode,
				ofono_radio_settings_rat_mode_set_cb_t cb,
				void *data)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[20];
	enum ste_radio_mode value;

	if (!ofono_mode_to_ste_mode(mode, &value)) {
		CALLBACK_WITH_FAILURE(cb, data);
		g_free(cbd);
		return;
	}

	snprintf(buf, sizeof(buf), "AT+CFUN=%u", value);

	if (g_at_chat_send(rsd->chat, buf, none_prefix,
					rat_modify_cb, cbd, g_free) == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		g_free(cbd);
	}
}

static gboolean ste_radio_settings_register(gpointer user)
{
	struct ofono_radio_settings *rs = user;

	ofono_radio_settings_register(rs);

	return FALSE;
}

static int ste_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct radio_settings_data *rsd;

	rsd = g_try_new0(struct radio_settings_data, 1);
	if (rsd == NULL)
		return -ENOMEM;

	rsd->chat = g_at_chat_clone(chat);

	ofono_radio_settings_set_data(rs, rsd);
	g_idle_add(ste_radio_settings_register, rs);

	return 0;
}

static void ste_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct radio_settings_data *rsd = ofono_radio_settings_get_data(rs);
	ofono_radio_settings_set_data(rs, NULL);

	g_at_chat_unref(rsd->chat);
	g_free(rsd);
}

static const struct ofono_radio_settings_driver driver = {
	.name		= "stemodem",
	.probe		= ste_radio_settings_probe,
	.remove		= ste_radio_settings_remove,
	.query_rat_mode	= ste_query_rat_mode,
	.set_rat_mode	= ste_set_rat_mode
};

void ste_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void ste_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
