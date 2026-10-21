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

/* Android's APN database, see apns-conf.xml in AOSP */
extern const char *apns_conf_database;

GSList *apns_conf_lookup_apn(const char *mcc, const char *mnc,
							GError **error);
