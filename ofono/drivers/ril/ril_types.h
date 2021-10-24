/*
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2015-2020 Jolla Ltd.
 *  Copyright (C) 2019-2020 Open Mobile Platform LLC.
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

#ifndef RIL_TYPES_H
#define RIL_TYPES_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <grilio_types.h>
#include <gutil_macros.h>

struct ofono_watch;
struct ofono_modem;
struct ofono_sim;

#include <ofono/types.h>
#include <ofono/radio-settings.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ril_constants.h"

#define RIL_RETRY_SECS (2)
#define RIL_RETRY_MS   (RIL_RETRY_SECS*1000)

struct ril_data;
struct ril_data_call;
struct ril_modem;
struct ril_radio;
struct ril_network;
struct ril_sim_card;
struct ril_vendor;

enum ril_data_role {
	RIL_DATA_ROLE_NONE,    /* Mobile data not required */
	RIL_DATA_ROLE_MMS,     /* Data is needed at any speed */
	RIL_DATA_ROLE_INTERNET /* Data is needed at full speed */
};

struct ril_slot_config {
	guint slot;
	enum ofono_radio_access_mode techs;
	enum ril_pref_net_type lte_network_mode;
	enum ril_pref_net_type umts_network_mode;
	int network_mode_timeout;
	int network_selection_timeout;
	int signal_strength_dbm_weak;
	int signal_strength_dbm_strong;
	gboolean query_available_band_mode;
	gboolean empty_pin_query;
	gboolean radio_power_cycle;
	gboolean confirm_radio_power_on;
	gboolean enable_voicecall;
	gboolean enable_cbs;
	gboolean enable_stk;
	gboolean replace_strange_oper;
	gboolean network_selection_manual_0;
	gboolean force_gsm_when_radio_off;
	gboolean use_data_profiles;
	guint mms_data_profile_id;
	GUtilInts *local_hangup_reasons;
	GUtilInts *remote_hangup_reasons;
	int cell_info_interval_short_ms;
	int cell_info_interval_long_ms;
	int ril_request_on_set_udub;
};

#endif /* RIL_TYPES_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
