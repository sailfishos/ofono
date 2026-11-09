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

#include <string.h>

#include <glib.h>

#include <ofono/conf.h>

#include "ofono.h"

#define CARRIER_NAME_FILE	"carrier_name.conf"
#define CARRIER_RANGE_KEY	"Ranges"

struct carrier_range {
	char *first;
	char *last;
};

struct carrier_override {
	char *spn;
	char *network;
	GSList *ranges;
};

static GSList *carrier_overrides;
static gboolean carrier_loaded;

static gboolean carrier_digits(const char *s)
{
	if (s == NULL || s[0] == '\0')
		return FALSE;

	while (*s != '\0') {
		if (*s < '0' || *s > '9')
			return FALSE;

		s++;
	}

	return TRUE;
}

static struct carrier_range *carrier_range_parse(const char *key,
							const char *value)
{
	char *copy;
	char *colon;
	struct carrier_range *range;

	if (value == NULL) {
		ofono_warn("Empty carrier range %s", key);
		return NULL;
	}

	copy = g_strdup(value);
	colon = strchr(copy, ':');
	if (colon == NULL || colon == copy || colon[1] == '\0' ||
			strchr(colon + 1, ':') != NULL) {
		ofono_warn("Bad carrier range %s=%s", key, value);
		g_free(copy);
		return NULL;
	}

	*colon++ = '\0';
	g_strstrip(copy);
	g_strstrip(colon);
	if (!carrier_digits(copy) || !carrier_digits(colon) ||
			strlen(copy) != strlen(colon) ||
			strcmp(copy, colon) > 0) {
		ofono_warn("Bad carrier range %s=%s", key, value);
		g_free(copy);
		return NULL;
	}

	range = g_new0(struct carrier_range, 1);
	range->first = g_strdup(copy);
	range->last = g_strdup(colon);
	g_free(copy);

	return range;
}

static gboolean carrier_in_range(const char *iccid,
					const struct carrier_range *range)
{
	size_t len = strlen(iccid);

	if (len != strlen(range->first) || len != strlen(range->last))
		return FALSE;

	return strcmp(iccid, range->first) >= 0 &&
					strcmp(iccid, range->last) <= 0;
}

static char *carrier_read_name(GKeyFile *conf, const char *group,
						const char *key)
{
	char *name = g_key_file_has_key(conf, group, key, NULL) ?
		g_key_file_get_string(conf, group, key, NULL) : g_strdup(group);

	if (name && (!name[0] || !g_utf8_validate(name, -1, NULL))) {
		g_free(name);
		name = NULL;
	}
	return name;
}

static void carrier_load(void)
{
	GKeyFile *conf = g_key_file_new();
	char *path = g_build_filename(ofono_config_dir(),
						CARRIER_NAME_FILE, NULL);
	char **groups;
	char **group;

	carrier_loaded = TRUE;

	/* Use the same list separator as the configuration merger. */
	g_key_file_set_list_separator(conf, ',');
	ofono_conf_merge_files(conf, path);
	groups = g_key_file_get_groups(conf, NULL);

	for (group = groups; group && *group; group++) {
		char **ranges = g_key_file_get_string_list(conf, *group,
						CARRIER_RANGE_KEY, NULL, NULL);
		char **value;
		struct carrier_override *carrier;

		if (ranges == NULL)
			continue;

		carrier = g_new0(struct carrier_override, 1);
		carrier->spn = carrier_read_name(conf, *group,
						"ServiceProviderName");
		carrier->network = carrier_read_name(conf, *group, "NetworkName");

		for (value = ranges; *value; value++) {
			struct carrier_range *range = carrier_range_parse(
						*group, g_strstrip(*value));

			if (range != NULL)
				carrier->ranges = g_slist_append(carrier->ranges,
								range);
		}
		carrier_overrides = g_slist_append(carrier_overrides, carrier);
		g_strfreev(ranges);
	}

	g_strfreev(groups);
	g_key_file_free(conf);
	g_free(path);
}

const char *__ofono_carrier_name(const char *iccid, gboolean network)
{
	GSList *l;

	if (!carrier_loaded)
		carrier_load();

	if (!carrier_digits(iccid))
		return NULL;

	for (l = carrier_overrides; l; l = l->next) {
		struct carrier_override *carrier = l->data;
		GSList *r;

		for (r = carrier->ranges; r; r = r->next) {
			if (carrier_in_range(iccid, r->data))
				return network ? carrier->network : carrier->spn;
		}
	}

	return NULL;
}

void __ofono_carrier_name_cleanup(void)
{
	GSList *l;

	for (l = carrier_overrides; l; l = l->next) {
		struct carrier_override *carrier = l->data;
		GSList *r;

		for (r = carrier->ranges; r; r = r->next) {
			struct carrier_range *range = r->data;

			g_free(range->first);
			g_free(range->last);
			g_free(range);
		}
		g_slist_free(carrier->ranges);
		g_free(carrier->spn);
		g_free(carrier->network);
		g_free(carrier);
	}
	g_slist_free(carrier_overrides);
	carrier_overrides = NULL;
	carrier_loaded = FALSE;
}
