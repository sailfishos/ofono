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

#define CARRIER_NAME_FILE	"carrier-name.conf"
#define CARRIER_GROUP		"Carrier"
#define CARRIER_NAME_KEY	"Name"
#define CARRIER_RANGES_GROUP	"Ranges"
#define CARRIER_RANGE_KEY	"Range"

struct carrier_range {
	char *first;
	char *last;
};

static char *carrier_spn;
static char *carrier_network;
static GSList *carrier_ranges;
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

static char *carrier_read_name(GKeyFile *conf, const char *key)
{
	char *name = ofono_conf_get_string(conf, CARRIER_GROUP, key);

	if (name == NULL)
		name = ofono_conf_get_string(conf, CARRIER_GROUP, CARRIER_NAME_KEY);

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
	char **ranges = NULL;
	char **value;

	carrier_loaded = TRUE;

	/*
	 * Same list separator the merger uses. A missing file is left
	 * empty, which means no renaming.
	 */
	g_key_file_set_list_separator(conf, ',');
	ofono_conf_merge_files(conf, path);

	carrier_spn = carrier_read_name(conf, "ServiceProviderName");
	carrier_network = carrier_read_name(conf, "NetworkName");
	if (!carrier_spn && !carrier_network)
		goto done;

	ranges = ofono_conf_get_strings(conf, CARRIER_RANGES_GROUP,
					CARRIER_RANGE_KEY, ',');
	if (ranges == NULL)
		ofono_warn("No carrier ranges in %s", path);

	for (value = ranges; value != NULL && *value != NULL; value++) {
		struct carrier_range *range = carrier_range_parse(
						CARRIER_RANGE_KEY, *value);

		if (range != NULL)
			carrier_ranges = g_slist_append(carrier_ranges, range);
	}

done:
	g_strfreev(ranges);
	g_key_file_free(conf);
	g_free(path);
}

const char *__ofono_carrier_name(const char *iccid, gboolean network)
{
	GSList *l;
	const char *name;

	if (!carrier_loaded)
		carrier_load();

	name = network ? carrier_network : carrier_spn;
	if (name == NULL || !carrier_digits(iccid))
		return NULL;

	for (l = carrier_ranges; l != NULL; l = l->next) {
		if (carrier_in_range(iccid, l->data))
			return name;
	}

	return NULL;
}

void __ofono_carrier_name_cleanup(void)
{
	GSList *l;

	for (l = carrier_ranges; l; l = l->next) {
		struct carrier_range *range = l->data;

		g_free(range->first);
		g_free(range->last);
		g_free(range);
	}
	g_slist_free(carrier_ranges);
	carrier_ranges = NULL;
	g_free(carrier_spn);
	carrier_spn = NULL;
	g_free(carrier_network);
	carrier_network = NULL;
	carrier_loaded = FALSE;
}
