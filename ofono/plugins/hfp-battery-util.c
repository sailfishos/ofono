/*
 *  oFono HFP battery argument parsing
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hfp-battery-util.h"

static gboolean next_decimal(const char **cursor, unsigned long *value,
					unsigned long maximum)
{
	const char *p = *cursor;
	char *end;

	if (*p < '0' || *p > '9')
		return FALSE;
	errno = 0;
	*value = strtoul(p, &end, 10);
	if (errno == ERANGE || end == p || *value > maximum)
		return FALSE;
	*cursor = end;
	return TRUE;
}

static gboolean next_hex_group(const char **cursor)
{
	const char *p = *cursor;
	unsigned int n = 0;

	while (n < 4 && ((*p >= '0' && *p <= '9') ||
			(*p >= 'a' && *p <= 'f') ||
			(*p >= 'A' && *p <= 'F'))) {
		p++;
		n++;
	}
	if (n != 4)
		return FALSE;
	*cursor = p;
	return TRUE;
}

gboolean hfp_battery_parse_xapl(const char *args, unsigned int *features)
{
	const char *p = args;
	unsigned long value;
	unsigned int result;

	if (args == NULL || features == NULL)
		return FALSE;
	if (!next_hex_group(&p) || *p++ != '-' ||
			!next_hex_group(&p) || *p++ != '-' ||
			!next_hex_group(&p) || *p++ != ',')
		return FALSE;
	if (!next_decimal(&p, &value, UINT_MAX) || *p != '\0')
		return FALSE;
	result = (unsigned int) value;
	*features = result;
	return TRUE;
}

gboolean hfp_battery_parse_iphoneaccev(const char *args, int *percentage)
{
	const char *p = args;
	unsigned long count;
	unsigned long key;
	unsigned long value;
	unsigned long i;
	int result = -1;
	gboolean battery_seen = FALSE;

	if (args == NULL || percentage == NULL ||
			!next_decimal(&p, &count, UINT_MAX))
		return FALSE;
	for (i = 0; i < count; i++) {
		if (*p != ',')
			return FALSE;
		p++;
		if (!next_decimal(&p, &key, INT_MAX) || *p != ',')
			return FALSE;
		p++;
		if (!next_decimal(&p, &value, INT_MAX))
			return FALSE;
		if (key == 1) {
			if (battery_seen || value > 9)
				return FALSE;
			battery_seen = TRUE;
			result = ((int) value + 1) * 10;
		}
	}
	if (*p != '\0')
		return FALSE;
	*percentage = result;
	return TRUE;
}

gboolean hfp_battery_parse_xevent(const char *args, int *percentage)
{
	const char *p = args;
	unsigned long level, levels, minutes, charging;
	unsigned long long scaled;
	int result;

	if (args == NULL || percentage == NULL)
		return FALSE;
	if (g_str_has_prefix(args, "USER-AGENT,")) {
		p = args + strlen("USER-AGENT,");
		if (*p == '\0')
			return FALSE;
		while (*p) {
			if (!g_ascii_isprint(*p))
				return FALSE;
			p++;
		}
		*percentage = -1;
		return TRUE;
	}
	if (!g_str_has_prefix(args, "BATTERY,")) {
		/* Identification: arbitrary text, one comma, decimal identifier.
		 * Acknowledge without changing the battery. The AT server may
		 * have removed unquoted spaces from the name.
		 */
		p = strchr(args, ',');
		if (p == NULL || !g_ascii_isdigit(*++p))
			return FALSE;
		while (g_ascii_isdigit(*p))
			p++;
		if (*p != '\0')
			return FALSE;
		*percentage = -1;
		return TRUE;
	}
	p = args + strlen("BATTERY,");
	if (!next_decimal(&p, &level, INT_MAX))
		return FALSE;
	if (*p == '\0') {
		if (level > 9)
			return FALSE;
		result = (int) ((level + 1) * 10);
		*percentage = result;
		return TRUE;
	}
	if (*p++ != ',')
		return FALSE;
	if (!next_decimal(&p, &levels, INT_MAX) || *p++ != ',' ||
			!next_decimal(&p, &minutes, INT_MAX) || *p++ != ',' ||
			!next_decimal(&p, &charging, INT_MAX) || *p != '\0')
		return FALSE;
	if (levels <= 1 || level >= levels || charging > 1)
		return FALSE;
	/* Widen before multiplication, including on 32-bit targets. */
	scaled = (unsigned long long) level * 100;
	result = (int) (scaled / (levels - 1));
	*percentage = result;
	return TRUE;
}
