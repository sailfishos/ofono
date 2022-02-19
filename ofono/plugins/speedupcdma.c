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

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/cdma-netreg.h>
#include <ofono/cdma-connman.h>
#include <ofono/log.h>

#include "drivers/atmodem/atutil.h"
#include "drivers/atmodem/vendor.h"

struct speedupcdma_data {
	GAtChat *modem;
	GAtChat *aux;
};

static void speedupcdma_debug(const char *str, void *data)
{
	const char *prefix = data;

	ofono_info("%s%s", prefix, str);
}

static int speedupcdma_probe(struct ofono_modem *modem)
{
	struct speedupcdma_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct speedupcdma_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void speedupcdma_remove(struct ofono_modem *modem)
{
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	/* Cleanup after hot-unplug */
	g_at_chat_unref(data->aux);

	g_free(data);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;

		g_at_chat_unref(data->aux);
		data->aux = NULL;
	}

	ofono_modem_set_powered(modem, ok);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	return at_util_open_device(modem, key, speedupcdma_debug, debug, NULL);
}

static int speedupcdma_enable(struct ofono_modem *modem)
{
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	data->modem = open_device(modem, "Modem", "Modem: ");
	if (data->modem == NULL)
		return -EINVAL;

	data->aux = open_device(modem, "Aux", "Aux: ");
	if (data->aux == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_set_slave(data->modem, data->aux);

	g_at_chat_send(data->modem, "ATE0 &C0 +CMEE=1", NULL, NULL, NULL, NULL);
	g_at_chat_send(data->aux, "ATE0 &C0 +CMEE=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(data->aux, "AT+CFUN=1", NULL,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int speedupcdma_disable(struct ofono_modem *modem)
{
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_cancel_all(data->modem);
	g_at_chat_unregister_all(data->modem);

	g_at_chat_unref(data->modem);
	data->modem = NULL;

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);

	g_at_chat_send(data->aux, "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void speedupcdma_pre_sim(struct ofono_modem *modem)
{
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "cdmamodem", data->aux);
}

static void speedupcdma_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void speedupcdma_post_online(struct ofono_modem *modem)
{
	struct speedupcdma_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_cdma_netreg_create(modem, 0, "huaweicdmamodem", data->aux);

	ofono_cdma_connman_create(modem, OFONO_VENDOR_HUAWEI, "cdmamodem",
					data->modem);
}

static struct ofono_modem_driver speedupcdma_driver = {
	.name		= "speedupcdma",
	.probe		= speedupcdma_probe,
	.remove		= speedupcdma_remove,
	.enable		= speedupcdma_enable,
	.disable	= speedupcdma_disable,
	.pre_sim	= speedupcdma_pre_sim,
	.post_sim	= speedupcdma_post_sim,
	.post_online	= speedupcdma_post_online,
};

static int speedupcdma_init(void)
{
	return ofono_modem_driver_register(&speedupcdma_driver);
}

static void speedupcdma_exit(void)
{
	ofono_modem_driver_unregister(&speedupcdma_driver);
}

OFONO_PLUGIN_DEFINE(speedupcdma, "Speed Up CDMA modem driver", VERSION,
				OFONO_PLUGIN_PRIORITY_DEFAULT,
				speedupcdma_init, speedupcdma_exit)
