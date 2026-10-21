/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/modem.h>
#include <ofono/gprs-provision.h>

#include "mbpi.h"
#include "apns-conf.h"

#ifndef APNS_CONF_DATABASE
#  define APNS_CONF_DATABASE "/system/etc/apns-conf.xml"
#endif

const char *apns_conf_database = APNS_CONF_DATABASE;

static const enum ofono_gprs_proto apns_conf_default_xcap_proto =
						OFONO_GPRS_PROTO_IPV4V6;

struct apns_conf_type {
	const char *name;
	enum ofono_gprs_context_type type;
	const enum ofono_gprs_proto *default_proto;
};

static const struct apns_conf_type apns_conf_types[] = {
	{ "default", OFONO_GPRS_CONTEXT_TYPE_INTERNET,
					&mbpi_default_internet_proto },
	{ "mms", OFONO_GPRS_CONTEXT_TYPE_MMS, &mbpi_default_mms_proto },
	{ "ims", OFONO_GPRS_CONTEXT_TYPE_IMS, &mbpi_default_ims_proto },
	{ "xcap", OFONO_GPRS_CONTEXT_TYPE_XCAP,
					&apns_conf_default_xcap_proto }
};

struct apns_conf_data {
	const char *match_mcc;
	const char *match_mnc;
	GSList *apns;
};

static const char *apns_conf_attr(const gchar **names, const gchar **values,
							const char *name)
{
	int i;

	for (i = 0; names[i]; i++)
		if (g_str_equal(names[i], name))
			return values[i];

	return NULL;
}

static const struct apns_conf_type *apns_conf_find_type(const char *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(apns_conf_types); i++)
		if (g_str_equal(apns_conf_types[i].name, name))
			return apns_conf_types + i;

	return NULL;
}

static enum ofono_gprs_proto apns_conf_proto(const char *str,
					enum ofono_gprs_proto default_proto)
{
	if (!g_strcmp0(str, "IP"))
		return OFONO_GPRS_PROTO_IP;
	else if (!g_strcmp0(str, "IPV6"))
		return OFONO_GPRS_PROTO_IPV6;
	else if (!g_strcmp0(str, "IPV4V6"))
		return OFONO_GPRS_PROTO_IPV4V6;

	return default_proto;
}

static enum ofono_gprs_auth_method apns_conf_auth_method(const char *str,
				const char *username, const char *password)
{
	switch (str ? atoi(str) : -1) {
	case 0:
		return OFONO_GPRS_AUTH_METHOD_NONE;
	case 1:
		return OFONO_GPRS_AUTH_METHOD_PAP;
	case 2:
		return OFONO_GPRS_AUTH_METHOD_CHAP;
	case 3:
		return OFONO_GPRS_AUTH_METHOD_ANY;
	}

	return ((username && username[0]) || (password && password[0])) ?
		mbpi_default_auth_method : OFONO_GPRS_AUTH_METHOD_NONE;
}

static char *apns_conf_message_proxy(const char *proxy, const char *port)
{
	if (!proxy || !proxy[0])
		return NULL;

	return (port && port[0]) ? g_strconcat(proxy, ":", port, NULL) :
							g_strdup(proxy);
}

static struct ofono_gprs_provision_data *apns_conf_new_ap(
				const struct apns_conf_type *t,
				const gchar **names, const gchar **values)
{
	struct ofono_gprs_provision_data *ap =
		g_new0(struct ofono_gprs_provision_data, 1);

	ap->type = t->type;
	ap->name = g_strdup(apns_conf_attr(names, values, "carrier"));
	ap->apn = g_strdup(apns_conf_attr(names, values, "apn"));
	ap->username = g_strdup(apns_conf_attr(names, values, "user"));
	ap->password = g_strdup(apns_conf_attr(names, values, "password"));
	ap->auth_method = apns_conf_auth_method(apns_conf_attr(names, values,
				"authtype"), ap->username, ap->password);

	if (t->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		/* gprs.c assumes that the MMS proxy address is IPv4 */
		ap->proto = *t->default_proto;
		ap->message_center = g_strdup(apns_conf_attr(names, values,
								"mmsc"));
		ap->message_proxy = apns_conf_message_proxy(
				apns_conf_attr(names, values, "mmsproxy"),
				apns_conf_attr(names, values, "mmsport"));
	} else {
		ap->proto = apns_conf_proto(apns_conf_attr(names, values,
					"protocol"), *t->default_proto);
	}

	return ap;
}

static gboolean apns_conf_match(const struct apns_conf_data *data,
				const gchar **names, const gchar **values)
{
	const char *apn = apns_conf_attr(names, values, "apn");
	const char *mvno_type = apns_conf_attr(names, values, "mvno_type");

	/* Matching MVNO entries needs SIM data (SPN, GID, IMSI) */
	return apn && apn[0] && !(mvno_type && mvno_type[0]) &&
		!g_strcmp0(apns_conf_attr(names, values, "mcc"),
							data->match_mcc) &&
		!g_strcmp0(apns_conf_attr(names, values, "mnc"),
							data->match_mnc) &&
		g_strcmp0(apns_conf_attr(names, values, "carrier_enabled"),
								"false");
}

static void apns_conf_start(GMarkupParseContext *context,
				const gchar *element_name,
				const gchar **names, const gchar **values,
				gpointer userdata, GError **error)
{
	struct apns_conf_data *data = userdata;
	const char *types;
	gchar **list;
	int i;

	if (!g_str_equal(element_name, "apn") ||
				!apns_conf_match(data, names, values))
		return;

	types = apns_conf_attr(names, values, "type");
	if (!types)
		return;

	/* One Android APN may serve several types, e.g. "default,supl" */
	list = g_strsplit(types, ",", -1);
	for (i = 0; list[i]; i++) {
		const struct apns_conf_type *t =
			apns_conf_find_type(g_strstrip(list[i]));

		if (t)
			data->apns = g_slist_append(data->apns,
					apns_conf_new_ap(t, names, values));
	}
	g_strfreev(list);
}

GSList *apns_conf_lookup_apn(const char *mcc, const char *mnc,
							GError **error)
{
	static const GMarkupParser parser = {
		apns_conf_start,
		NULL,
		NULL,
		NULL,
		NULL,
	};

	struct apns_conf_data data;

	memset(&data, 0, sizeof(data));
	data.match_mcc = mcc;
	data.match_mnc = mnc;

	if (!mbpi_parse_file(apns_conf_database, &parser, &data, error)) {
		g_slist_free_full(data.apns, (GDestroyNotify) mbpi_ap_free);
		return NULL;
	}

	return data.apns;
}
