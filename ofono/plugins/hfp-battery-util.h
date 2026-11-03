/* HFP battery AT argument parsers. */
#ifndef HFP_BATTERY_UTIL_H
#define HFP_BATTERY_UTIL_H

#include <glib.h>

gboolean hfp_battery_parse_xapl(const char *args, unsigned int *features);
gboolean hfp_battery_parse_iphoneaccev(const char *args, int *percentage);
gboolean hfp_battery_parse_xevent(const char *args, int *percentage);

#endif
