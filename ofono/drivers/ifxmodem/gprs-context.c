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
#include "gatrawip.h"

#include "ifxmodem.h"

#define TUN_DEV "/dev/net/tun"

#define STATIC_IP_NETMASK "255.255.255.255"
#define IPV6_DEFAULT_PREFIX_LEN 8

static const char *none_prefix[] = { NULL };
static const char *xdns_prefix[] = { "+XDNS:", NULL };
static const char *cgpaddr_prefix[] = { "+CGPADDR:", NULL };
static const char *cgcontrdp_prefix[] = { "+CGCONTRDP:", NULL };

enum state {
	STATE_IDLE,
	STATE_ENABLING,
	STATE_DISABLING,
	STATE_ACTIVE,
};

struct gprs_context_data {
	GAtChat *chat;
	unsigned int vendor;
	unsigned int active_context;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	GAtRawIP *rawip;
	enum state state;
	enum ofono_gprs_proto proto;
	char address[64];
	char gateway[64];
	char netmask[64];
	char dns1[64];
	char dns2[64];
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

static void rawip_debug(const char *str, void *data)
{
	ofono_info("%s: %s", (const char *) data, str);
}

static const char *setup_rawip(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtIO *io;

	DBG("");

	io = g_at_chat_get_io(gcd->chat);

	g_at_chat_suspend(gcd->chat);

	gcd->rawip = g_at_rawip_new_from_io(io);

	if (gcd->rawip == NULL) {
		g_at_chat_resume(gcd->chat);
		return NULL;
	}

	if (getenv("OFONO_IP_DEBUG"))
		g_at_rawip_set_debug(gcd->rawip, rawip_debug, "IP");

	g_at_rawip_open(gcd->rawip);

	return g_at_rawip_get_interface(gcd->rawip);
}

static void failed_setup(struct ofono_gprs_context *gc,
				GAtResult *result, gboolean deactivate)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ofono_error error;
	char buf[64];

	DBG("deactivate %d", deactivate);

	if (deactivate == TRUE) {
		sprintf(buf, "AT+CGACT=0,%u", gcd->active_context);
		g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);
	}

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	if (result == NULL) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	decode_at_error(&error, g_at_result_final_response(result));
	gcd->cb(&error, gcd->cb_data);
}

static void session_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	const char *interface;
	const char *dns[3];

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Failed to establish session");
		failed_setup(gc, result, TRUE);
		return;
	}

	gcd->state = STATE_ACTIVE;

	dns[0] = gcd->dns1;
	dns[1] = gcd->dns2;
	dns[2] = 0;

	interface = setup_rawip(gc);
	if (interface == NULL)
		interface = "invalid";

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, gcd->address, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, STATIC_IP_NETMASK);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);

	gcd->cb = NULL;
	gcd->cb_data = NULL;
}

static void dns_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];
	int cid;
	const char *dns1, *dns2;
	GAtResultIter iter;
	gboolean found = FALSE;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to get DNS details");
		failed_setup(gc, result, TRUE);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+XDNS:")) {
		if (!g_at_result_iter_next_number(&iter, &cid))
			goto error;

		if (!g_at_result_iter_next_string(&iter, &dns1))
			goto error;

		if (!g_at_result_iter_next_string(&iter, &dns2))
			goto error;

		if ((unsigned int) cid == gcd->active_context) {
			found = TRUE;
			strncpy(gcd->dns1, dns1, sizeof(gcd->dns1));
			strncpy(gcd->dns2, dns2, sizeof(gcd->dns2));
		}
	}

	if (found == FALSE)
		goto error;

	ofono_info("IP: %s", gcd->address);
	ofono_info("DNS: %s, %s", gcd->dns1, gcd->dns2);

	sprintf(buf, "AT+CGDATA=\"M-RAW_IP\",%d", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, none_prefix,
					session_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, TRUE);
}

static void address_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	int cid;
	const char *address;
	GAtResultIter iter;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to get context address");
		failed_setup(gc, result, TRUE);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGPADDR:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &cid))
		goto error;

	if ((unsigned int) cid != gcd->active_context)
		goto error;

	if (!g_at_result_iter_next_string(&iter, &address))
		goto error;

	strncpy(gcd->address, address, sizeof(gcd->address));

	if (g_at_chat_send(gcd->chat, "AT+XDNS?", xdns_prefix,
					dns_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, TRUE);
}

static void cgdata_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Failed to establish session");
		failed_setup(gc, result, TRUE);
		return;
	}

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static const char *get_datapath(struct ofono_modem *modem,
						const char *interface)
{
	static char datapath[256];
	char n;

	if (!strcmp(interface,
			ofono_modem_get_string(modem, "NetworkInterface")))
		n = '0';
	else if (!strcmp(interface,
			ofono_modem_get_string(modem, "NetworkInterface2")))
		n = '1';
	else if (!strcmp(interface,
			ofono_modem_get_string(modem, "NetworkInterface3")))
		n = '2';
	else
		return NULL;

	snprintf(datapath, sizeof(datapath), "%s%c",
			ofono_modem_get_string(modem, "DataPath"), n);
	return datapath;
}

static void cgcontrdp_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct ofono_modem *modem = ofono_gprs_context_get_modem(gc);
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;

	const char *laddrnetmask = NULL;
	const char *gw = NULL;
	const char *dns[3];
	const char *ctrlpath;
	const char *datapath;
	char buf[512];
	const char *interface;

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);

		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGCONTRDP:")) {
		/* skip cid, bearer_id, apn */
		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_skip_next(&iter);
		g_at_result_iter_skip_next(&iter);

		if (!g_at_result_iter_next_string(&iter, &laddrnetmask))
			break;

		if (!g_at_result_iter_next_string(&iter, &gw))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[0]))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[1]))
			break;
	}

	strncpy(gcd->dns1, dns[0], sizeof(gcd->dns1));
	strncpy(gcd->dns2, dns[1], sizeof(gcd->dns2));
	dns[2] = 0;

	DBG("DNS: %s, %s\n", gcd->dns1, gcd->dns2);

	if (gw)
		strncpy(gcd->gateway, gw, sizeof(gcd->gateway));

	if (gcd->proto == OFONO_GPRS_PROTO_IP) {
		if (!laddrnetmask ||
			at_util_get_ipv4_address_and_netmask(laddrnetmask,
					gcd->address, gcd->netmask) < 0) {
			failed_setup(gc, NULL, TRUE);
			return;
		}

		ofono_gprs_context_set_ipv4_address(gc, gcd->address, TRUE);

		if (gcd->netmask[0])
			ofono_gprs_context_set_ipv4_netmask(gc, gcd->netmask);

		if (gcd->gateway[0])
			ofono_gprs_context_set_ipv4_gateway(gc, gcd->gateway);

		ofono_gprs_context_set_ipv4_dns_servers(gc, dns);
	}

	if (gcd->proto == OFONO_GPRS_PROTO_IPV6) {
		if (!laddrnetmask ||
			at_util_get_ipv6_address_and_netmask(laddrnetmask,
					gcd->address, gcd->netmask) < 0) {
			failed_setup(gc, NULL, TRUE);
			return;
		}

		ofono_gprs_context_set_ipv6_address(gc, gcd->address);

		if (gcd->gateway[0])
			ofono_gprs_context_set_ipv6_gateway(gc, gcd->gateway);

		ofono_gprs_context_set_ipv6_dns_servers(gc, dns);
		ofono_gprs_context_set_ipv6_prefix_length(gc,
						IPV6_DEFAULT_PREFIX_LEN);
	}

	gcd->state = STATE_ACTIVE;

	DBG("address: %s\n", gcd->address);
	DBG("netmask: %s\n", gcd->netmask);
	DBG("DNS1: %s\n", gcd->dns1);
	DBG("DNS2: %s\n", gcd->dns2);
	DBG("Gateway: %s\n", gcd->gateway);

	ctrlpath = ofono_modem_get_string(modem, "CtrlPath");
	interface = ofono_gprs_context_get_interface(gc);
	datapath = get_datapath(modem, interface);

	snprintf(buf, sizeof(buf), "AT+XDATACHANNEL=1,1,\"%s\",\"%s\",0,%u",
			ctrlpath, datapath, gcd->active_context);
	g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL);
	snprintf(buf, sizeof(buf), "AT+CGDATA=\"M-RAW_IP\",%u",
			gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, cgdata_cb,
						gc, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void ifx_read_settings(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	gcd->address[0] = '\0';
	gcd->gateway[0] = '\0';
	gcd->netmask[0] = '\0';
	gcd->dns1[0] = '\0';
	gcd->dns2[0] = '\0';

	/* read IP configuration info */
	if(gcd->vendor == OFONO_VENDOR_XMM) {
		snprintf(buf, sizeof(buf), "AT+CGCONTRDP=%u",
							gcd->active_context);

		if (g_at_chat_send(gcd->chat, buf, cgcontrdp_prefix,
					cgcontrdp_cb, gc, NULL) > 0)
			return;
	} else {
		sprintf(buf, "AT+CGPADDR=%u", gcd->active_context);

		if (g_at_chat_send(gcd->chat, buf, cgpaddr_prefix,
						address_cb, gc, NULL) > 0)
			return;
	}

	failed_setup(gc, NULL, TRUE);
}

static void ifx_gprs_read_settings(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	DBG("cid %u", cid);

	gcd->active_context = cid;
	gcd->cb = cb;
	gcd->cb_data = data;

	ifx_read_settings(gc);
}

static void activate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Unable to activate context");
		failed_setup(gc, result, FALSE);
		return;
	}

	ifx_read_settings(gc);
}

static void setup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[384];

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("Failed to setup context");
		failed_setup(gc, result, FALSE);
		return;
	}

	if (gcd->username[0] && gcd->password[0])
		sprintf(buf, "AT+XGAUTH=%u,1,\"%s\",\"%s\"",
			gcd->active_context, gcd->username, gcd->password);
	else
		sprintf(buf, "AT+XGAUTH=%u,0,\"\",\"\"", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL) == 0)
		goto error;

	g_at_chat_send(gcd->chat, "AT+XDNS=?", none_prefix, NULL, NULL, NULL);

	switch (gcd->proto) {
	case OFONO_GPRS_PROTO_IP:
		sprintf(buf, "AT+XDNS=%u,1", gcd->active_context);
		break;
	case OFONO_GPRS_PROTO_IPV6:
		sprintf(buf, "AT+XDNS=%u,2", gcd->active_context);
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		sprintf(buf, "AT+XDNS=%u,3", gcd->active_context);
		break;
	}

	if (g_at_chat_send(gcd->chat, buf, none_prefix, NULL, NULL, NULL) == 0)
		goto error;

	sprintf(buf, "AT+CGACT=1,%u", gcd->active_context);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				activate_cb, gc, NULL) > 0)
		return;

error:
	failed_setup(gc, NULL, FALSE);
}

static void ifx_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len = 0;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	gcd->cb = cb;
	gcd->cb_data = data;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	gcd->state = STATE_ENABLING;
	gcd->proto = ctx->proto;

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IP:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"",
								ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV6\"",
								ctx->cid);
		break;
	case OFONO_GPRS_PROTO_IPV4V6:
		len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IPV4V6\"",
								ctx->cid);
		break;
	}

	snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"", ctx->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				setup_cb, gc, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
}

static void deactivate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	g_at_rawip_unref(gcd->rawip);
	gcd->rawip = NULL;

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;

	if (gcd->vendor != OFONO_VENDOR_XMM)
		g_at_chat_resume(gcd->chat);

	if (!gcd->cb)
		return;

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void ifx_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtChat *chat = g_at_chat_get_slave(gcd->chat);
	char buf[64];

	DBG("cid %u", cid);

	gcd->state = STATE_DISABLING;
	gcd->cb = cb;
	gcd->cb_data = data;

	g_at_rawip_shutdown(gcd->rawip);

	sprintf(buf, "AT+CGACT=0,%u", gcd->active_context);

	if (gcd->vendor == OFONO_VENDOR_XMM) {
		if (g_at_chat_send(gcd->chat, buf, none_prefix,
				deactivate_cb, gc, NULL) > 0)
			return;
	} else {
		if (g_at_chat_send(chat, buf, none_prefix,
				deactivate_cb, gc, NULL) > 0)
			return;
	}

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ifx_gprs_detach_shutdown(struct ofono_gprs_context *gc,
						unsigned int cid)
{
	DBG("");
	ifx_gprs_deactivate_primary(gc, cid, NULL, NULL);
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

	if (gcd->state != STATE_IDLE && gcd->rawip) {
		g_at_rawip_shutdown(gcd->rawip);

		g_at_rawip_unref(gcd->rawip);
		gcd->rawip = NULL;
		g_at_chat_resume(gcd->chat);
	}

	ofono_gprs_context_deactivated(gc, gcd->active_context);

	gcd->active_context = 0;
	gcd->state = STATE_IDLE;
}

static int ifx_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;
	struct stat st;

	DBG("");

	if (vendor != OFONO_VENDOR_XMM) {
		if (stat(TUN_DEV, &st) < 0) {
			ofono_error("Missing support for TUN/TAP devices");
			return -ENODEV;
		}
	}

	if (vendor != OFONO_VENDOR_XMM) {
		if (g_at_chat_get_slave(chat) == NULL)
			return -EINVAL;
	}

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->vendor = vendor;
	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	if (vendor != OFONO_VENDOR_XMM)
		chat = g_at_chat_get_slave(gcd->chat);

	g_at_chat_register(chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	return 0;
}

static void ifx_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	if (gcd->state != STATE_IDLE && gcd->rawip) {
		g_at_rawip_unref(gcd->rawip);
		g_at_chat_resume(gcd->chat);
	}

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);
	g_free(gcd);
}

static const struct ofono_gprs_context_driver driver = {
	.name			= "ifxmodem",
	.probe			= ifx_gprs_context_probe,
	.remove			= ifx_gprs_context_remove,
	.activate_primary	= ifx_gprs_activate_primary,
	.deactivate_primary	= ifx_gprs_deactivate_primary,
	.read_settings		= ifx_gprs_read_settings,
	.detach_shutdown	= ifx_gprs_detach_shutdown
};

void ifx_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void ifx_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
