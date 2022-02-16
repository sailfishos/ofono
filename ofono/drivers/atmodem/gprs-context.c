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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"
#include "gatppp.h"

#include "atmodem.h"
#include "vendor.h"

#define TUN_DEV "/dev/net/tun"

#define STATIC_IP_NETMASK "255.255.255.255"

static const char *cgdata_prefix[] = { "+CGDATA:", NULL };
static const char *none_prefix[] = { NULL };

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	GAtPPPAuthMethod auth_method;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	GAtPPP *ppp;
	enum state state;
	ofono_gprs_context_cb_t cb;
	void *cb_data;                                  /* Callback data */
	unsigned int vendor;
	gboolean use_atd99;
};

static void ppp_debug(const char *str, void *data)
{
	ofono_info("%s: %s", (const char *) data, str);
}

static void ppp_connect(const char *interface, const char *local,
			const char *remote,
			const char *dns1, const char *dns2,
			gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *dns[3];

	DBG("");

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	ofono_info("IP: %s", local);
	ofono_info("DNS: %s, %s", dns1, dns2);

	gcd->state = STATE_ACTIVE;
	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, local, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, STATIC_IP_NETMASK);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("Reason: %d", reason);

	g_at_ppp_unref(gcd->ppp);
	gcd->ppp = NULL;

	switch (gcd->state) {
	case STATE_ENABLING:
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		break;
	case STATE_DISABLING:
		CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
		break;
	default:
		ofono_gprs_context_deactivated(gc, gcd->active_context);
		break;
	}

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;
	/*
	 * If the channel of gcd->chat is NULL, it might cause
	 * gprs_context_remove get called and the gprs context will be
	 * removed.
	 */
	g_at_chat_resume(gcd->chat);
}

static gboolean setup_ppp(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtIO *io;

	DBG("");

	io = g_at_chat_get_io(gcd->chat);

	g_at_chat_suspend(gcd->chat);

	/* open ppp */
	gcd->ppp = g_at_ppp_new();

	if (gcd->ppp == NULL) {
		g_at_chat_resume(gcd->chat);
		return FALSE;
	}

	if (getenv("OFONO_PPP_DEBUG"))
		g_at_ppp_set_debug(gcd->ppp, ppp_debug, "PPP");

	g_at_ppp_set_auth_method(gcd->ppp, gcd->auth_method);
	g_at_ppp_set_credentials(gcd->ppp, gcd->username, gcd->password);

	/* set connect and disconnect callbacks */
	g_at_ppp_set_connect_function(gcd->ppp, ppp_connect, gc);
	g_at_ppp_set_disconnect_function(gcd->ppp, ppp_disconnect, gc);

	/* open the ppp connection */
	g_at_ppp_open(gcd->ppp, io);

	return TRUE;
}

static void at_cgdata_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		ofono_info("Unable to enter data state");

		gcd->active_context = 0;
		gcd->state = STATE_IDLE;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}

	setup_ppp(gc);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;
		gcd->state = STATE_IDLE;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}

	if (gcd->use_atd99)
		sprintf(buf, "ATD*99***%u#", gcd->active_context);
	else
		sprintf(buf, "AT+CGDATA=\"PPP\",%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdata_cb, gc, NULL) > 0)
		return;

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void at_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len;

	/* IPv6 support not implemented */
	if (ctx->proto != OFONO_GPRS_PROTO_IP)
		goto error;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	gcd->cb = cb;
	gcd->cb_data = data;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	/* We only support CHAP and PAP */
	switch (ctx->auth_method) {
	case OFONO_GPRS_AUTH_METHOD_ANY:
	case OFONO_GPRS_AUTH_METHOD_NONE:
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		gcd->auth_method = G_AT_PPP_AUTH_METHOD_CHAP;
		break;
	case OFONO_GPRS_AUTH_METHOD_PAP:
		gcd->auth_method = G_AT_PPP_AUTH_METHOD_PAP;
		break;
	default:
		goto error;
	}

	gcd->state = STATE_ENABLING;

	if (gcd->vendor == OFONO_VENDOR_ZTE) {
		GAtChat *chat = g_at_chat_get_slave(gcd->chat);

		/*
		 * The modem port of ZTE devices with certain firmware
		 * versions ends up getting suspended. It will no longer
		 * signal POLLOUT and becomes pretty unresponsive.
		 *
		 * To wake up the modem port, the only reliable method
		 * found so far is AT+ZOPRT power mode command. It is
		 * enough to ask for the current mode and the modem
		 * port wakes up and accepts commands again.
		 *
		 * And since the modem port is suspended, this command
		 * needs to be send on the control port of course.
		 *
		 */
		g_at_chat_send(chat, "AT+ZOPRT?", none_prefix,
						NULL, NULL, NULL);
	}

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"", ctx->cid);

	switch (gcd->vendor) {
	case OFONO_VENDOR_UBLOX:
		/*
		 * U-blox modems require a magic prefix to the APN to
		 * specify the authentication method to use in the
		 * network.  See UBX-13002752 - R21.
		 *
		 * As the response of the read command omits this magic
		 * prefix, this is the least invasive place to set it.
		 */
		switch (ctx->auth_method) {
		case OFONO_GPRS_AUTH_METHOD_ANY:
		case OFONO_GPRS_AUTH_METHOD_CHAP:
			snprintf(buf + len, sizeof(buf) - len - 3,
					",\"CHAP:%s\"", ctx->apn);
			break;
		case OFONO_GPRS_AUTH_METHOD_PAP:
			snprintf(buf + len, sizeof(buf) - len - 3,
					",\"PAP:%s\"", ctx->apn);
			break;
		case OFONO_GPRS_AUTH_METHOD_NONE:
			snprintf(buf + len, sizeof(buf) - len - 3,
					",\"%s\"", ctx->apn);
			break;
		}
		break;
	default:
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
				ctx->apn);
		break;
	}

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				at_cgdcont_cb, gc, NULL) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("cid %u", cid);

	gcd->state = STATE_DISABLING;
	gcd->cb = cb;
	gcd->cb_data = data;

	g_at_ppp_shutdown(gcd->ppp);
}

static void at_gprs_detach_shutdown(struct ofono_gprs_context *gc,
					unsigned int cid)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("cid %u", cid);

	g_at_ppp_shutdown(gcd->ppp);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *event;
	int cid;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "NW DEACT") == FALSE)
		return;

	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_next_number(&iter, &cid))
		return;

	DBG("cid %d", cid);

	if ((unsigned int) cid != gcd->active_context)
		return;

	if (gcd->state != STATE_IDLE && gcd->ppp)
		g_at_ppp_shutdown(gcd->ppp);
}

static void at_cgdata_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *data_type;
	gboolean found = FALSE;

	gcd->use_atd99 = TRUE;

	if (!ok) {
		DBG("not ok");
		goto error;
	}

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CGDATA:")) {
		DBG("no +CGDATA line");
		goto error;
	}

	if (gcd->vendor != OFONO_VENDOR_QUECTEL_SERIAL) {
		if (!g_at_result_iter_open_list(&iter)) {
			DBG("no list found");
			goto error;
		}
	}

	while (!found && g_at_result_iter_next_string(&iter, &data_type)) {
		if (g_str_equal(data_type, "PPP")) {
			found = TRUE;
			gcd->use_atd99 = FALSE;
		}
	}
error:
	DBG("use_atd99:%d", gcd->use_atd99);
}

static int at_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;
	struct stat st;

	DBG("");

	if (stat(TUN_DEV, &st) < 0) {
		ofono_error("Missing support for TUN/TAP devices");
		return -ENODEV;
	}

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->chat = g_at_chat_clone(chat);
	gcd->vendor = vendor;

	ofono_gprs_context_set_data(gc, gcd);

	chat = g_at_chat_get_slave(gcd->chat);
	if (chat == NULL)
		return 0;

	switch (vendor) {
	case OFONO_VENDOR_SIMCOM_SIM900:
		gcd->use_atd99 = FALSE;
		break;
	default:
		g_at_chat_send(chat, "AT+CGDATA=?", cgdata_prefix,
						at_cgdata_test_cb, gc, NULL);
	}

	g_at_chat_register(chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	return 0;
}

static void at_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtIO *io;

	DBG("");

	if (gcd->state != STATE_IDLE && gcd->ppp) {
		if ((gcd->vendor == OFONO_VENDOR_HUAWEI) && gcd->chat) {
			/* immediately send escape sequence */
			io = g_at_chat_get_io(gcd->chat);

			if (io)
				g_at_io_write(io, "+++", 3);
		}

		g_at_ppp_unref(gcd->ppp);
		g_at_chat_resume(gcd->chat);
	}

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static const struct ofono_gprs_context_driver driver = {
	.name			= "atmodem",
	.probe			= at_gprs_context_probe,
	.remove			= at_gprs_context_remove,
	.activate_primary	= at_gprs_activate_primary,
	.deactivate_primary	= at_gprs_deactivate_primary,
	.detach_shutdown	= at_gprs_detach_shutdown,
};

void at_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void at_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
