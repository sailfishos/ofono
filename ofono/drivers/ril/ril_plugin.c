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

#include "ril_plugin.h"
#include "ril_config.h"
#include "ril_sim_card.h"
#include "ril_sim_settings.h"
#include "ril_cell_info.h"
#include "ril_network.h"
#include "ril_radio.h"
#include "ril_radio_caps.h"
#include "ril_data.h"
#include "ril_util.h"
#include "ril_vendor.h"
#include "ril_devmon.h"
#include "ril_log.h"
#include "ril-constants.h"

#include "ofono.h"
#include "sailfish_manager.h"

#include <ofono/storage.h>
#include <ofono/watch.h>

#include <grilio_transport.h>

#include <gutil_ints.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <mce_log.h>

#include <linux/capability.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/storage.h>
#include <ofono/ril-transport.h>

#define OFONO_RADIO_ACCESS_MODE_ALL (OFONO_RADIO_ACCESS_MODE_GSM |\
                                     OFONO_RADIO_ACCESS_MODE_UMTS |\
                                     OFONO_RADIO_ACCESS_MODE_LTE)

#define RIL_DEVICE_IDENTITY_RETRIES_LAST 2

#define RIL_SUB_SIZE                4

#define RILMODEM_CONF_FILE          "ril_subscription.conf"
#define RILMODEM_DEFAULT_IDENTITY   "radio:radio"
#define RILMODEM_DEFAULT_SOCK       "/dev/socket/rild"
#define RILMODEM_DEFAULT_SOCK2      "/dev/socket/rild2"
#define RILMODEM_DEFAULT_SUB        "SUB1"
#define RILMODEM_DEFAULT_TECHS      OFONO_RADIO_ACCESS_MODE_ALL
#define RILMODEM_DEFAULT_LTE_MODE   PREF_NET_TYPE_LTE_GSM_WCDMA
#define RILMODEM_DEFAULT_UMTS_MODE  PREF_NET_TYPE_GSM_WCDMA_AUTO
#define RILMODEM_DEFAULT_NETWORK_MODE_TIMEOUT (20*1000) /* ms */
#define RILMODEM_DEFAULT_NETWORK_SELECTION_TIMEOUT (100*1000) /* ms */
#define RILMODEM_DEFAULT_DBM_WEAK   (-100) /* very weak, 0.0000000001 mW */
#define RILMODEM_DEFAULT_DBM_STRONG (-60)  /* strong signal, 0.000001 mW */
#define RILMODEM_DEFAULT_ENABLE_VOICECALL TRUE
#define RILMODEM_DEFAULT_ENABLE_CBS TRUE
#define RILMODEM_DEFAULT_ENABLE_STK TRUE
#define RILMODEM_DEFAULT_SLOT       0xffffffff
#define RILMODEM_DEFAULT_TIMEOUT    0 /* No timeout */
#define RILMODEM_DEFAULT_SIM_FLAGS  RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND
#define RILMODEM_DEFAULT_DATA_OPT   RIL_ALLOW_DATA_AUTO
#define RILMODEM_DEFAULT_DM_FLAGS  (RIL_DATA_MANAGER_3GLTE_HANDOVER | \
                                    RIL_DATA_MANAGER_FORCE_GSM_ON_OTHER_SLOTS)
#define RILMODEM_DEFAULT_START_TIMEOUT 20000 /* ms */
#define RILMODEM_DEFAULT_DATA_CALL_FORMAT RIL_DATA_CALL_FORMAT_AUTO
#define RILMODEM_DEFAULT_DATA_CALL_RETRY_LIMIT 4
#define RILMODEM_DEFAULT_DATA_CALL_RETRY_DELAY 200 /* ms */
#define RILMODEM_DEFAULT_EMPTY_PIN_QUERY TRUE /* optimistic */
#define RILMODEM_DEFAULT_QUERY_AVAILABLE_BAND_MODE TRUE /* Qualcomm */
#define RILMODEM_DEFAULT_LEGACY_IMEI_QUERY FALSE
#define RILMODEM_DEFAULT_RADIO_POWER_CYCLE TRUE
#define RILMODEM_DEFAULT_CONFIRM_RADIO_POWER_ON TRUE
#define RILMODEM_DEFAULT_REPLACE_STRANGE_OPER FALSE
#define RILMODEM_DEFAULT_NETWORK_SELECTION_MANUAL_0 TRUE
#define RILMODEM_DEFAULT_FORCE_GSM_WHEN_RADIO_OFF TRUE
#define RILMODEM_DEFAULT_USE_DATA_PROFILES FALSE
#define RILMODEM_DEFAULT_MMS_DATA_PROFILE_ID RIL_DATA_PROFILE_IMS
#define RILMODEM_DEFAULT_SLOT_FLAGS SAILFISH_SLOT_NO_FLAGS
#define RILMODEM_DEFAULT_CELL_INFO_INTERVAL_SHORT_MS (2000) /* 2 sec */
#define RILMODEM_DEFAULT_CELL_INFO_INTERVAL_LONG_MS  (30000) /* 30 sec */
#define RILMODEM_DEFAULT_RIL_REQUEST_ON_SET_UDUB     RIL_REQUEST_UDUB

/* RIL socket transport name and parameters */
#define RIL_TRANSPORT_MODEM                 "modem"
#define RIL_TRANSPORT_SOCKET                "socket"
#define RIL_TRANSPORT_SOCKET_PATH           "path"
#define RIL_TRANSPORT_SOCKET_SUB            "sub"

/*
 * The convention is that the keys which can only appear in the [Settings]
 * section start with the upper case, those which appear in the [ril_*]
 * modem section (OR in the [Settings] if they apply to all modems) start
 * with lower case.
 */
#define RILCONF_SETTINGS_EMPTY              "EmptyConfig"
#define RILCONF_SETTINGS_IDENTITY           "Identity"
#define RILCONF_SETTINGS_3GHANDOVER         "3GLTEHandover"
#define RILCONF_SETTINGS_GSM_NON_DATA_SLOTS "ForceGsmForNonDataSlots"
#define RILCONF_SETTINGS_SET_RADIO_CAP      "SetRadioCapability"

#define RILCONF_MODEM_PREFIX                "ril_"
#define RILCONF_PATH_PREFIX                 "/" RILCONF_MODEM_PREFIX
#define RILCONF_TRANSPORT                   "transport"
#define RILCONF_NAME                        "name"
#define RILCONF_SOCKET                      "socket"
#define RILCONF_SLOT                        "slot"
#define RILCONF_SUB                         "sub"
#define RILCONF_START_TIMEOUT               "startTimeout"
#define RILCONF_TIMEOUT                     "timeout"
#define RILCONF_4G                          "enable4G" /* Deprecated */
#define RILCONF_ENABLE_VOICECALL            "enableVoicecall"
#define RILCONF_ENABLE_CBS                  "enableCellBroadcast"
#define RILCONF_ENABLE_STK                  "enableSimToolkit"
#define RILCONF_TECHNOLOGIES                "technologies"
#define RILCONF_LTE_MODE                    "lteNetworkMode"
#define RILCONF_UMTS_MODE                   "umtsNetworkMode"
#define RILCONF_NETWORK_MODE_TIMEOUT        "networkModeTimeout"
#define RILCONF_NETWORK_SELECTION_TIMEOUT   "networkSelectionTimeout"
#define RILCONF_SIGNAL_STRENGTH_RANGE       "signalStrengthRange"
#define RILCONF_UICC_WORKAROUND             "uiccWorkaround"
#define RILCONF_ECCLIST_FILE                "ecclistFile"
#define RILCONF_ALLOW_DATA_REQ              "allowDataReq"
#define RILCONF_EMPTY_PIN_QUERY             "emptyPinQuery"
#define RILCONF_DATA_CALL_FORMAT            "dataCallFormat"
#define RILCONF_VENDOR_DRIVER               "vendorDriver"
#define RILCONF_DATA_CALL_RETRY_LIMIT       "dataCallRetryLimit"
#define RILCONF_DATA_CALL_RETRY_DELAY       "dataCallRetryDelay"
#define RILCONF_LOCAL_HANGUP_REASONS        "localHangupReasons"
#define RILCONF_REMOTE_HANGUP_REASONS       "remoteHangupReasons"
#define RILCONF_LEGACY_IMEI_QUERY           "legacyImeiQuery"
#define RILCONF_RADIO_POWER_CYCLE           "radioPowerCycle"
#define RILCONF_CONFIRM_RADIO_POWER_ON      "confirmRadioPowerOn"
#define RILCONF_SINGLE_DATA_CONTEXT         "singleDataContext"
#define RILCONF_REPLACE_STRANGE_OPER        "replaceStrangeOperatorNames"
#define RILCONF_NETWORK_SELECTION_MANUAL_0  "networkSelectionManual0"
#define RILCONF_FORCE_GSM_WHEN_RADIO_OFF    "forceGsmWhenRadioOff"
#define RILCONF_USE_DATA_PROFILES           "useDataProfiles"
#define RILCONF_MMS_DATA_PROFILE_ID         "mmsDataProfileId"
#define RILCONF_DEVMON                      "deviceStateTracking"
#define RILCONF_CELL_INFO_INTERVAL_SHORT_MS "cellInfoIntervalShortMs"
#define RILCONF_CELL_INFO_INTERVAL_LONG_MS  "cellInfoIntervalLongMs"
#define RILCONF_RIL_REQUEST_ON_SET_UDUB     "rilRequestOnSetUdub"

/* Modem error ids */
#define RIL_ERROR_ID_RILD_RESTART           "rild-restart"
#define RIL_ERROR_ID_CAPS_SWITCH_ABORTED    "ril-caps-switch-aborted"

enum ril_plugin_io_events {
	IO_EVENT_CONNECTED,
	IO_EVENT_ERROR,
	IO_EVENT_EOF,
	IO_EVENT_RADIO_STATE_CHANGED,
	IO_EVENT_COUNT
};

enum ril_plugin_watch_events {
	WATCH_EVENT_MODEM,
	WATCH_EVENT_COUNT
};

enum ril_set_radio_cap_opt {
	RIL_SET_RADIO_CAP_AUTO,
	RIL_SET_RADIO_CAP_ENABLED,
	RIL_SET_RADIO_CAP_DISABLED
};

enum ril_devmon_opt {
	RIL_DEVMON_SS = 0x01,
	RIL_DEVMON_DS = 0x02,
	RIL_DEVMON_UR = 0x04
};

struct ril_plugin_identity {
	uid_t uid;
	gid_t gid;
};

struct ril_plugin_settings {
	int dm_flags;
	enum ril_set_radio_cap_opt set_radio_cap;
	struct ril_plugin_identity identity;
};

typedef struct sailfish_slot_manager_impl {
	struct sailfish_slot_manager *handle;
	struct ril_data_manager *data_manager;
	struct ril_radio_caps_manager *caps_manager;
	struct ril_plugin_settings settings;
	gulong caps_manager_event_id;
	guint start_timeout_id;
	GSList *slots;
} ril_plugin;

typedef struct sailfish_slot_impl {
	ril_plugin* plugin;
	struct sailfish_slot *handle;
	struct sailfish_cell_info *cell_info;
	struct ofono_watch *watch;
	gulong watch_event_id[WATCH_EVENT_COUNT];
	char *path;
	char *imei;
	char *imeisv;
	char *name;
	char *transport_name;
	GHashTable *transport_params;
	char *ecclist_file;
	int timeout;            /* RIL timeout, in milliseconds */
	int index;
	int sim_flags;
	struct ril_data_options data_opt;
	struct ril_slot_config config;
	struct ril_modem *modem;
	struct ril_radio *radio;
	struct ril_radio_caps *caps;
	struct ril_radio_caps_request *caps_req;
	struct ril_network *network;
	struct ril_sim_card *sim_card;
	struct ril_sim_settings *sim_settings;
	struct ril_oem_raw *oem_raw;
	const struct ril_vendor_driver *vendor_driver;
	struct ril_vendor *vendor;
	struct ril_data *data;
	gboolean legacy_imei_query;
	enum sailfish_slot_flags slot_flags;
	guint start_timeout;
	guint start_timeout_id;
	struct ril_devmon *devmon;
	struct ril_devmon_io *devmon_io;
	GRilIoChannel *io;
	gulong io_event_id[IO_EVENT_COUNT];
	gulong sim_card_state_event_id;
	gboolean received_sim_status;
	guint serialize_id;
	guint caps_check_id;
	guint imei_req_id;
	guint trace_id;
	guint dump_id;
	guint retry_id;
} ril_slot;

typedef void (*ril_plugin_slot_cb_t)(ril_slot *slot);
typedef void (*ril_plugin_slot_param_cb_t)(ril_slot *slot, void *param);

static void ril_debug_trace_notify(struct ofono_debug_desc *desc);
static void ril_debug_dump_notify(struct ofono_debug_desc *desc);
static void ril_debug_grilio_notify(struct ofono_debug_desc *desc);
static void ril_debug_mce_notify(struct ofono_debug_desc *desc);
static void ril_plugin_debug_notify(struct ofono_debug_desc *desc);
static void ril_plugin_drop_orphan_slots(ril_plugin *plugin);
static void ril_plugin_retry_init_io(ril_slot *slot);
static void ril_plugin_check_modem(ril_slot *slot);

GLOG_MODULE_DEFINE("rilmodem");

static const char ril_debug_trace_name[] = "ril_trace";

static GLogModule ril_debug_trace_module = {
	.name = ril_debug_trace_name,
	.max_level = GLOG_LEVEL_VERBOSE,
	.level = GLOG_LEVEL_VERBOSE,
	.flags = GLOG_FLAG_HIDE_NAME
};

static struct ofono_debug_desc ril_debug_trace OFONO_DEBUG_ATTR = {
	.name = ril_debug_trace_name,
	.flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
	.notify = ril_debug_trace_notify
};

static struct ofono_debug_desc ril_debug_dump OFONO_DEBUG_ATTR = {
	.name = "ril_dump",
	.flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
	.notify = ril_debug_dump_notify
};

static struct ofono_debug_desc grilio_debug OFONO_DEBUG_ATTR = {
	.name = "grilio",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_debug_grilio_notify
};

static struct ofono_debug_desc mce_debug OFONO_DEBUG_ATTR = {
	.name = "mce",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_debug_mce_notify
};

static struct ofono_debug_desc ril_plugin_debug OFONO_DEBUG_ATTR = {
	.name = "ril_plugin",
	.flags = OFONO_DEBUG_FLAG_DEFAULT,
	.notify = ril_plugin_debug_notify
};

static inline const char *ril_slot_debug_prefix(const ril_slot *slot)
{
	/* slot->path always starts with a slash, skip it */
	return slot->path + 1;
}

static gboolean ril_plugin_multisim(ril_plugin *plugin)
{
	return plugin->slots && plugin->slots->next;
}

static void ril_plugin_foreach_slot_param(ril_plugin *plugin,
				ril_plugin_slot_param_cb_t fn, void *param)
{
	GSList *l = plugin->slots;

	while (l) {
		GSList *next = l->next;

		fn((ril_slot *)l->data, param);
		l = next;
	}
}

static void ril_plugin_foreach_slot_proc(gpointer data, gpointer user_data)
{
	((ril_plugin_slot_cb_t)user_data)(data);
}

static void ril_plugin_foreach_slot(ril_plugin *plugin, ril_plugin_slot_cb_t fn)
{
	g_slist_foreach(plugin->slots, ril_plugin_foreach_slot_proc, fn);
}

static void ril_plugin_foreach_slot_manager_proc(ril_plugin *plugin, void *data)
{
	ril_plugin_foreach_slot(plugin, (ril_plugin_slot_cb_t)data);
}

static void ril_plugin_foreach_slot_manager(struct sailfish_slot_driver_reg *r,
						ril_plugin_slot_cb_t fn)
{
	sailfish_manager_foreach_slot_manager(r,
				ril_plugin_foreach_slot_manager_proc, fn);
}

static void ril_plugin_remove_slot_handler(ril_slot *slot, int id)
{
	GASSERT(id >= 0 && id<IO_EVENT_COUNT);
	if (slot->io_event_id[id]) {
		grilio_channel_remove_handler(slot->io, slot->io_event_id[id]);
		slot->io_event_id[id] = 0;
	}
}

static void ril_plugin_shutdown_slot(ril_slot *slot, gboolean kill_io)
{
	if (slot->modem) {
		ril_data_allow(slot->data, RIL_DATA_ROLE_NONE);
		ril_modem_delete(slot->modem);
		/* The above call is expected to result in
		 * ril_plugin_modem_removed getting called
		 * which will set slot->modem to NULL */
		GASSERT(!slot->modem);
	}

	if (kill_io) {
		if (slot->retry_id) {
			g_source_remove(slot->retry_id);
			slot->retry_id = 0;
		}

		if (slot->devmon_io) {
			ril_devmon_io_free(slot->devmon_io);
			slot->devmon_io = NULL;
		}

		if (slot->cell_info) {
			sailfish_manager_set_cell_info(slot->handle, NULL);
			sailfish_cell_info_unref(slot->cell_info);
			slot->cell_info = NULL;
		}

		if (slot->caps) {
			ril_network_set_radio_caps(slot->network, NULL);
			ril_radio_caps_request_free(slot->caps_req);
			ril_radio_caps_drop(slot->caps);
			slot->caps_req = NULL;
			slot->caps = NULL;
		}

		if (slot->data) {
			ril_data_allow(slot->data, RIL_DATA_ROLE_NONE);
			ril_data_unref(slot->data);
			slot->data = NULL;
		}

		if (slot->radio) {
			ril_radio_unref(slot->radio);
			slot->radio = NULL;
		}

		if (slot->network) {
			ril_network_unref(slot->network);
			slot->network = NULL;
		}

		if (slot->sim_card) {
			ril_sim_card_remove_handler(slot->sim_card,
						slot->sim_card_state_event_id);
			ril_sim_card_unref(slot->sim_card);
			slot->sim_card_state_event_id = 0;
			slot->sim_card = NULL;
			slot->received_sim_status = FALSE;
		}

		if (slot->vendor) {
			ril_vendor_unref(slot->vendor);
			slot->vendor = NULL;
		}

		if (slot->io) {
			int i;

			grilio_channel_remove_logger(slot->io, slot->trace_id);
			grilio_channel_remove_logger(slot->io, slot->dump_id);
			slot->trace_id = 0;
			slot->dump_id = 0;

			if (slot->caps_check_id) {
				grilio_channel_cancel_request(slot->io,
						slot->caps_check_id, FALSE);
				slot->caps_check_id = 0;
			}

			if (slot->imei_req_id) {
				grilio_channel_cancel_request(slot->io,
						slot->imei_req_id, FALSE);
				slot->imei_req_id = 0;
			}

			if (slot->serialize_id) {
				grilio_channel_deserialize(slot->io,
							slot->serialize_id);
				slot->serialize_id = 0;
			}

			for (i=0; i<IO_EVENT_COUNT; i++) {
				ril_plugin_remove_slot_handler(slot, i);
			}

			grilio_channel_shutdown(slot->io, FALSE);
			grilio_channel_unref(slot->io);
			slot->io = NULL;
		}
	}
}

static void ril_plugin_check_ready(ril_slot *slot)
{
	if (slot->serialize_id && slot->imei && slot->sim_card &&
						slot->sim_card->status) {
		grilio_channel_deserialize(slot->io, slot->serialize_id);
		slot->serialize_id = 0;
	}
}

static void ril_plugin_get_imeisv_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	ril_slot *slot = user_data;
	char *imeisv = NULL;

	GASSERT(slot->imei_req_id);
	slot->imei_req_id = 0;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		imeisv = grilio_parser_get_utf8(&rilp);
		DBG("%s", imeisv);

		/*
		 * slot->imei should be either NULL (when we get connected
		 * to rild the very first time) or match the already known
		 * IMEI (if rild crashed and we have reconnected)
		 */
		if (slot->imeisv && imeisv && strcmp(slot->imeisv, imeisv)) {
			ofono_warn("IMEISV has changed \"%s\" -> \"%s\"",
							slot->imeisv, imeisv);
		}
	} else {
		ofono_error("Slot %u IMEISV query error: %s",
			slot->config.slot, ril_error_to_string(status));
	}

	if (slot->imeisv) {
		/* We assume that IMEISV never changes */
		g_free(imeisv);
	} else {
		slot->imeisv = (imeisv ? imeisv : g_strdup(""));
		sailfish_manager_imeisv_obtained(slot->handle, slot->imeisv);
	}

	ril_plugin_check_modem(slot);
}

static void ril_plugin_get_imei_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	ril_slot *slot = user_data;
	char *imei = NULL;

	GASSERT(slot->imei_req_id);
	slot->imei_req_id = 0;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;

		grilio_parser_init(&rilp, data, len);
		imei = grilio_parser_get_utf8(&rilp);
		DBG("%s", imei);

		/*
		 * slot->imei should be either NULL (when we get connected
		 * to rild the very first time) or match the already known
		 * IMEI (if rild crashed and we have reconnected)
		 */
		if (slot->imei && imei && strcmp(slot->imei, imei)) {
			ofono_warn("IMEI has changed \"%s\" -> \"%s\"",
							slot->imei, imei);
		}

		if (imei) {
			/* IMEI query was successful, fetch IMEISV too */
			GRilIoRequest *req = grilio_request_new();
			slot->imei_req_id =
				grilio_channel_send_request_full(slot->io,
					req, RIL_REQUEST_GET_IMEISV,
					ril_plugin_get_imeisv_cb, NULL, slot);
			grilio_request_unref(req);
		}
	} else {
		ofono_error("Slot %u IMEI query error: %s", slot->config.slot,
						ril_error_to_string(status));
	}

	if (slot->imei) {
		/* We assume that IMEI never changes */
		g_free(imei);
	} else {
		slot->imei = imei ? imei : g_strdup_printf("%d", slot->index);
		sailfish_manager_imei_obtained(slot->handle, slot->imei);
	}

	ril_plugin_check_modem(slot);
	ril_plugin_check_ready(slot);
}

static void ril_plugin_device_identity_cb(GRilIoChannel *io, int status,
			const void *data, guint len, void *user_data)
{
	ril_slot *slot = user_data;
	char *imei = NULL;
	char *imeisv = NULL;

	GASSERT(slot->imei_req_id);
	slot->imei_req_id = 0;

	if (status == RIL_E_SUCCESS) {
		GRilIoParser rilp;
		guint32 n;

		/*
		 * RIL_REQUEST_DEVICE_IDENTITY
		 *
		 * "response" is const char **
		 * ((const char **)response)[0] is IMEI (for GSM)
		 * ((const char **)response)[1] is IMEISV (for GSM)
		 * ((const char **)response)[2] is ESN (for CDMA)
		 * ((const char **)response)[3] is MEID (for CDMA)
		 */
		grilio_parser_init(&rilp, data, len);
		if (grilio_parser_get_uint32(&rilp, &n) && n >= 2) {
			imei = grilio_parser_get_utf8(&rilp);
			imeisv = grilio_parser_get_utf8(&rilp);
			DBG("%s %s", imei, imeisv);
		} else {
			DBG("parsing failure!");
		}

		/*
		 * slot->imei should be either NULL (when we get connected
		 * to rild the very first time) or match the already known
		 * IMEI (if rild crashed and we have reconnected)
		 */
		if (slot->imei && imei && strcmp(slot->imei, imei)) {
			ofono_warn("IMEI has changed \"%s\" -> \"%s\"",
							slot->imei, imei);
		}
	} else {
		ofono_error("Slot %u IMEI query error: %s", slot->config.slot,
						ril_error_to_string(status));
	}

	if (slot->imei) {
		/* We assume that IMEI never changes */
		g_free(imei);
	} else {
		slot->imei = imei ? imei : g_strdup_printf("%d", slot->index);
		sailfish_manager_imei_obtained(slot->handle, slot->imei);
	}

	if (slot->imeisv) {
		g_free(imeisv);
	} else {
		slot->imeisv = (imeisv ? imeisv : g_strdup(""));
		sailfish_manager_imeisv_obtained(slot->handle, slot->imeisv);
	}

	ril_plugin_check_modem(slot);
	ril_plugin_check_ready(slot);
}

static void ril_plugin_start_imei_query(ril_slot *slot, gboolean blocking,
								int retries)
{
	GRilIoRequest *req = grilio_request_new();

	/* There was a bug in libgrilio which was making request blocking
	 * regardless of what we pass to grilio_request_set_blocking(),
	 * that's why we don't call grilio_request_set_blocking() if
	 * blocking is FALSE */
	if (blocking) grilio_request_set_blocking(req, TRUE);
	grilio_request_set_retry(req, RIL_RETRY_MS, retries);
	grilio_channel_cancel_request(slot->io, slot->imei_req_id, FALSE);
	slot->imei_req_id = (slot->legacy_imei_query ?
		grilio_channel_send_request_full(slot->io, req,
				RIL_REQUEST_GET_IMEI,
				ril_plugin_get_imei_cb, NULL, slot) :
		grilio_channel_send_request_full(slot->io, req,
				RIL_REQUEST_DEVICE_IDENTITY,
				ril_plugin_device_identity_cb, NULL, slot));
	grilio_request_unref(req);
}

static enum sailfish_sim_state ril_plugin_sim_state(ril_slot *slot)
{
	const struct ril_sim_card_status *status = slot->sim_card->status;

	if (status) {
		switch (status->card_state) {
		case RIL_CARDSTATE_PRESENT:
			return SAILFISH_SIM_STATE_PRESENT;
		case RIL_CARDSTATE_ABSENT:
			return SAILFISH_SIM_STATE_ABSENT;
		case RIL_CARDSTATE_ERROR:
			return SAILFISH_SIM_STATE_ERROR;
		default:
			break;
		}
	}

	return SAILFISH_SIM_STATE_UNKNOWN;
}

static void ril_plugin_sim_state_changed(struct ril_sim_card *card, void *data)
{
	ril_slot *slot = data;
	const enum sailfish_sim_state sim_state = ril_plugin_sim_state(slot);

	if (card->status) {
		switch (sim_state) {
		case SAILFISH_SIM_STATE_PRESENT:
			DBG("SIM found in slot %u", slot->config.slot);
			break;
		case SAILFISH_SIM_STATE_ABSENT:
			DBG("No SIM in slot %u", slot->config.slot);
			break;
		default:
			break;
		}
		if (!slot->received_sim_status && slot->imei_req_id) {
			/*
			 * We have received the SIM status but haven't yet
			 * got IMEI from the modem. Some RILs behave this
			 * way if the modem doesn't have IMEI initialized
			 * yet. Cancel the current request (with unlimited
			 * number of retries) and give a few more tries
			 * (this time, limited number).
			 *
			 * Some RILs fail RIL_REQUEST_DEVICE_IDENTITY until
			 * the modem has been properly initialized.
			 */
			DBG("Giving slot %u last chance", slot->config.slot);
			ril_plugin_start_imei_query(slot, FALSE,
					 RIL_DEVICE_IDENTITY_RETRIES_LAST);
		}
		slot->received_sim_status = TRUE;
	}

	sailfish_manager_set_sim_state(slot->handle, sim_state);
	ril_plugin_check_ready(slot);
}

static void ril_plugin_handle_error(ril_slot *slot, const char *message)
{
	ofono_error("%s %s", ril_slot_debug_prefix(slot), message);
	sailfish_manager_slot_error(slot->handle, RIL_ERROR_ID_RILD_RESTART,
								message);
	ril_plugin_shutdown_slot(slot, TRUE);
	ril_plugin_retry_init_io(slot);
}

static void ril_plugin_slot_error(GRilIoChannel *io, const GError *error,
								void *data)
{
	ril_plugin_handle_error((ril_slot *)data, GERRMSG(error));
}

static void ril_plugin_slot_disconnected(GRilIoChannel *io, void *data)
{
	ril_plugin_handle_error((ril_slot *)data, "disconnected");
}

static void ril_plugin_caps_switch_aborted(struct ril_radio_caps_manager *mgr,
								void *data)
{
	ril_plugin *plugin = data;
	DBG("radio caps switch aborted");
	sailfish_manager_error(plugin->handle,
				RIL_ERROR_ID_CAPS_SWITCH_ABORTED,
				"Capability switch transaction aborted");
}

static void ril_plugin_trace(GRilIoChannel *io, GRILIO_PACKET_TYPE type,
	guint id, guint code, const void *data, guint data_len, void *user_data)
{
	ril_slot *slot = user_data;
	struct ril_vendor *vendor = slot->vendor;
	static const GLogModule* log_module = &ril_debug_trace_module;
	const char *prefix = io->name ? io->name : "";
	const char dir = (type == GRILIO_PACKET_REQ) ? '<' : '>';
	const char *scode = NULL;

	switch (type) {
	case GRILIO_PACKET_REQ:
		if (io->ril_version <= 9 &&
				code == RIL_REQUEST_V9_SET_UICC_SUBSCRIPTION) {
			scode = "V9_SET_UICC_SUBSCRIPTION";
		} else {
			scode = ril_vendor_request_to_string(vendor, code);
			if (!scode) {
				/* Not a vendor specific request */
				scode = ril_request_to_string(code);
			}
		}
		gutil_log(log_module, GLOG_LEVEL_VERBOSE, "%s%c [%08x] %s",
				prefix, dir, id, scode);
		break;
	case GRILIO_PACKET_ACK:
		gutil_log(log_module, GLOG_LEVEL_VERBOSE, "%s%c [%08x] ACK",
				prefix, dir, id);
		break;
	case GRILIO_PACKET_RESP:
	case GRILIO_PACKET_RESP_ACK_EXP:
		gutil_log(log_module, GLOG_LEVEL_VERBOSE, "%s%c [%08x] %s",
				prefix, dir, id, ril_error_to_string(code));
		break;
	case GRILIO_PACKET_UNSOL:
	case GRILIO_PACKET_UNSOL_ACK_EXP:
		scode = ril_vendor_event_to_string(vendor, code);
		if (!scode) {
			/* Not a vendor specific event */
			scode = ril_unsol_event_to_string(code);
		}
		gutil_log(log_module, GLOG_LEVEL_VERBOSE, "%s%c %s",
				prefix, dir, scode);
		break;
	}
}

static void ril_debug_dump_update(ril_slot *slot)
{
	if (slot->io) {
		if (ril_debug_dump.flags & OFONO_DEBUG_FLAG_PRINT) {
			if (!slot->dump_id) {
				slot->dump_id =
					grilio_channel_add_default_logger(
						slot->io, GLOG_LEVEL_VERBOSE);
			}
		} else if (slot->dump_id) {
			grilio_channel_remove_logger(slot->io, slot->dump_id);
			slot->dump_id = 0;
		}
	}
}

static void ril_debug_trace_update(ril_slot *slot)
{
	if (slot->io) {
		if (ril_debug_trace.flags & OFONO_DEBUG_FLAG_PRINT) {
			if (!slot->trace_id) {
				slot->trace_id =
					grilio_channel_add_logger(slot->io,
						ril_plugin_trace, slot);
				/*
				 * Loggers are invoked in the order they have
				 * been registered. Make sure that dump logger
				 * is invoked after ril_plugin_trace.
				 */
				if (slot->dump_id) {
					grilio_channel_remove_logger(slot->io,
								slot->dump_id);
					slot->dump_id = 0;
				}
				ril_debug_dump_update(slot);
			}
		} else if (slot->trace_id) {
			grilio_channel_remove_logger(slot->io, slot->trace_id);
			slot->trace_id = 0;
		}
	}
}

static const char *ril_plugin_log_prefix(ril_slot *slot)
{
	return ril_plugin_multisim(slot->plugin) ?
					ril_slot_debug_prefix(slot) : "";
}

static void ril_plugin_create_modem(ril_slot *slot)
{
	struct ril_modem *modem;
	const char *log_prefix = ril_plugin_log_prefix(slot);

	DBG("%s", ril_slot_debug_prefix(slot));
	GASSERT(slot->io && slot->io->connected);
	GASSERT(!slot->modem);

	modem = ril_modem_create(slot->io, log_prefix, slot->path, slot->imei,
		slot->imeisv, slot->ecclist_file, &slot->config, slot->radio,
		slot->network, slot->sim_card, slot->data, slot->sim_settings,
		slot->vendor, slot->cell_info);

	if (modem) {
		slot->modem = modem;
		slot->oem_raw = ril_oem_raw_new(modem, log_prefix);
	} else {
		ril_plugin_shutdown_slot(slot, TRUE);
	}
}

static void ril_plugin_check_modem(ril_slot *slot)
{
	if (!slot->modem && slot->handle->enabled &&
			slot->io && slot->io->connected &&
			!slot->imei_req_id && slot->imei) {
		ril_plugin_create_modem(slot);
	}
}

/*
 * It seems to be necessary to kick (with RIL_REQUEST_RADIO_POWER) the
 * modems with power on after one of the modems has been powered off.
 * Otherwise bad things may happen (like the modem never registering
 * on the network).
 */
static void ril_plugin_power_check(ril_slot *slot)
{
	ril_radio_confirm_power_on(slot->radio);
}

static void ril_plugin_radio_state_changed(GRilIoChannel *io, guint code,
				const void *data, guint len, void *user_data)
{
	if (ril_radio_state_parse(data, len) == RADIO_STATE_OFF) {
		ril_slot *slot = user_data;

		DBG("power off for slot %u", slot->config.slot);
		ril_plugin_foreach_slot(slot->plugin, ril_plugin_power_check);
	}
}

static void ril_plugin_radio_caps_cb(const struct ril_radio_capability *cap,
							void *user_data)
{
	ril_slot *slot = user_data;

	DBG("radio caps %s", cap ? "ok" : "NOT supported");
	GASSERT(slot->caps_check_id);
	slot->caps_check_id = 0;

	if (cap) {
		ril_plugin *plugin = slot->plugin;

		if (!plugin->caps_manager) {
			plugin->caps_manager = ril_radio_caps_manager_new
				(plugin->data_manager);
			plugin->caps_manager_event_id =
				ril_radio_caps_manager_add_tx_aborted_handler(
					plugin->caps_manager,
					ril_plugin_caps_switch_aborted,
					plugin);
		}

		GASSERT(!slot->caps);
		slot->caps = ril_radio_caps_new(plugin->caps_manager,
			ril_plugin_log_prefix(slot), slot->io, slot->watch,
			slot->data, slot->radio, slot->sim_card,
			slot->sim_settings, &slot->config, cap);
		ril_network_set_radio_caps(slot->network, slot->caps);
	}
}

static void ril_plugin_manager_started(ril_plugin *plugin)
{
	ril_plugin_drop_orphan_slots(plugin);
	ril_data_manager_check_data(plugin->data_manager);
	sailfish_slot_manager_started(plugin->handle);
}

static void ril_plugin_all_slots_started_cb(ril_slot *slot, void *param)
{
	if (!slot->handle) {
		(*((gboolean*)param)) = FALSE; /* Not all */
	}
}

static void ril_plugin_check_if_started(ril_plugin* plugin)
{
	if (plugin->start_timeout_id) {
		gboolean all = TRUE;

		ril_plugin_foreach_slot_param(plugin,
				ril_plugin_all_slots_started_cb, &all);
		if (all) {
			DBG("Startup done!");
			g_source_remove(plugin->start_timeout_id);
			/* id is zeroed by ril_plugin_manager_start_done */
			GASSERT(!plugin->start_timeout_id);
			ril_plugin_manager_started(plugin);
		}
	}
}

static void ril_plugin_slot_connected(ril_slot *slot)
{
	ril_plugin *plugin = slot->plugin;
	const struct ril_plugin_settings *ps = &plugin->settings;
	const char *log_prefix = ril_plugin_log_prefix(slot);

	ofono_debug("%s version %u", (slot->name && slot->name[0]) ?
				slot->name : "RIL", slot->io->ril_version);

	GASSERT(slot->io->connected);
	GASSERT(!slot->io_event_id[IO_EVENT_CONNECTED]);

	/*
	 * Modem will be registered after RIL_REQUEST_DEVICE_IDENTITY
	 * successfully completes. By the time ofono starts, rild may
	 * not be completely functional. Waiting until it responds to
	 * RIL_REQUEST_DEVICE_IDENTITY (or RIL_REQUEST_GET_IMEI/SV)
	 * and retrying the request on failure, (hopefully) gives rild
	 * enough time to finish whatever it's doing during initialization.
	 */
	ril_plugin_start_imei_query(slot, TRUE, -1);

	GASSERT(!slot->radio);
	slot->radio = ril_radio_new(slot->io);

	GASSERT(!slot->io_event_id[IO_EVENT_RADIO_STATE_CHANGED]);
	if (slot->config.confirm_radio_power_on) {
		slot->io_event_id[IO_EVENT_RADIO_STATE_CHANGED] =
			grilio_channel_add_unsol_event_handler(slot->io,
				ril_plugin_radio_state_changed,
				RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, slot);
	}

	GASSERT(!slot->sim_card);
	slot->sim_card = ril_sim_card_new(slot->io, slot->config.slot,
							slot->sim_flags);
	slot->sim_card_state_event_id = ril_sim_card_add_state_changed_handler(
			slot->sim_card, ril_plugin_sim_state_changed, slot);
	/* ril_sim_card is expected to perform RIL_REQUEST_GET_SIM_STATUS
	 * asynchronously and report back when request has completed: */
	GASSERT(!slot->sim_card->status);
	GASSERT(!slot->received_sim_status);

	GASSERT(!slot->vendor);
	slot->vendor = ril_vendor_create(slot->vendor_driver, slot->io,
				slot->path, &slot->config);

	GASSERT(!slot->network);
	slot->network = ril_network_new(slot->path, slot->io, log_prefix,
			slot->radio, slot->sim_card, slot->sim_settings, 
			&slot->config, slot->vendor);

	GASSERT(!slot->data);
	slot->data = ril_data_new(plugin->data_manager, log_prefix,
		slot->radio, slot->network, slot->io, &slot->data_opt,
		&slot->config, slot->vendor);

	GASSERT(!slot->cell_info);
	if (slot->io->ril_version >= 9) {
		slot->cell_info = ril_cell_info_new(slot->io, log_prefix,
				slot->radio, slot->sim_card);
	}

	GASSERT(!slot->caps);
	GASSERT(!slot->caps_check_id);
	if (ril_plugin_multisim(plugin) &&
		(ps->set_radio_cap == RIL_SET_RADIO_CAP_ENABLED ||
		(ps->set_radio_cap == RIL_SET_RADIO_CAP_AUTO &&
					slot->io->ril_version >= 11))) {
		/* Check if RIL really supports radio capability management */
		slot->caps_check_id = ril_radio_caps_check(slot->io,
					ril_plugin_radio_caps_cb, slot);
	}

	GASSERT(!slot->devmon_io);
	if (slot->devmon) {
		slot->devmon_io = ril_devmon_start_io(slot->devmon,
						slot->io, slot->cell_info);
	}

	if (!slot->handle) {
		GASSERT(plugin->start_timeout_id);
		GASSERT(slot->start_timeout_id);

		/* We have made it before the timeout expired */
		g_source_remove(slot->start_timeout_id);
		slot->start_timeout_id = 0;

		/* Register this slot with the sailfish manager plugin */
		slot->handle = sailfish_manager_slot_add2(plugin->handle, slot,
				slot->path, slot->config.techs, slot->imei,
				slot->imeisv, ril_plugin_sim_state(slot),
				slot->slot_flags);
		grilio_channel_set_enabled(slot->io, slot->handle->enabled);

		/* Check if this was the last slot we were waiting for */
		ril_plugin_check_if_started(plugin);
	}

	sailfish_manager_set_cell_info(slot->handle, slot->cell_info);
	ril_plugin_check_modem(slot);
	ril_plugin_check_ready(slot);
}

static void ril_plugin_slot_connected_cb(GRilIoChannel *io, void *user_data)
{
	ril_slot *slot = user_data;

	ril_plugin_remove_slot_handler(slot, IO_EVENT_CONNECTED);
	ril_plugin_slot_connected(slot);
}

static void ril_plugin_init_io(ril_slot *slot)
{
	if (!slot->io) {
		struct grilio_transport *transport =
			ofono_ril_transport_connect(slot->transport_name,
						slot->transport_params);

		slot->io = grilio_channel_new(transport);
		if (slot->io) {
			ril_debug_trace_update(slot);
			ril_debug_dump_update(slot);

			if (slot->name) {
				grilio_channel_set_name(slot->io, slot->name);
			}

			grilio_channel_set_timeout(slot->io, slot->timeout);
			slot->io_event_id[IO_EVENT_ERROR] =
				grilio_channel_add_error_handler(slot->io,
					ril_plugin_slot_error, slot);
			slot->io_event_id[IO_EVENT_EOF] =
				grilio_channel_add_disconnected_handler(
						slot->io,
						ril_plugin_slot_disconnected,
						slot);

			/* Serialize requests at startup */
			slot->serialize_id = grilio_channel_serialize(slot->io);

			if (slot->io->connected) {
				ril_plugin_slot_connected(slot);
			} else {
				slot->io_event_id[IO_EVENT_CONNECTED] =
					grilio_channel_add_connected_handler(
						slot->io,
						ril_plugin_slot_connected_cb,
						slot);
			}
		}
		grilio_transport_unref(transport);
	}

	if (!slot->io) {
		ril_plugin_retry_init_io(slot);
	}
}

static gboolean ril_plugin_retry_init_io_cb(gpointer data)
{
	ril_slot *slot = data;

	GASSERT(slot->retry_id);
	slot->retry_id = 0;
	ril_plugin_init_io(slot);

	return G_SOURCE_REMOVE;
}

static void ril_plugin_retry_init_io(ril_slot *slot)
{
	if (slot->retry_id) {
		g_source_remove(slot->retry_id);
	}

	DBG("%s", slot->path);
	slot->retry_id = g_timeout_add_seconds(RIL_RETRY_SECS,
					ril_plugin_retry_init_io_cb, slot);
}

static void ril_plugin_slot_modem_changed(struct ofono_watch *w,
							void *user_data)
{
	ril_slot *slot = user_data;

	DBG("%s", slot->path);
	if (!w->modem) {
		GASSERT(slot->modem);

		if (slot->oem_raw) {
			ril_oem_raw_free(slot->oem_raw);
			slot->oem_raw = NULL;
		}

		slot->modem = NULL;
		ril_data_allow(slot->data, RIL_DATA_ROLE_NONE);
		ril_radio_caps_request_free(slot->caps_req);
		slot->caps_req = NULL;
	}
}

static void ril_slot_free(ril_slot *slot)
{
	ril_plugin* plugin = slot->plugin;

	DBG("%s", slot->path);
	ril_plugin_shutdown_slot(slot, TRUE);
	plugin->slots = g_slist_remove(plugin->slots, slot);
	ofono_watch_remove_all_handlers(slot->watch, slot->watch_event_id);
	ofono_watch_unref(slot->watch);
	ril_devmon_free(slot->devmon);
	ril_sim_settings_unref(slot->sim_settings);
	gutil_ints_unref(slot->config.local_hangup_reasons);
	gutil_ints_unref(slot->config.remote_hangup_reasons);
	g_free(slot->path);
	g_free(slot->imei);
	g_free(slot->imeisv);
	g_free(slot->name);
	g_free(slot->transport_name);
	g_hash_table_destroy(slot->transport_params);
	g_free(slot->ecclist_file);
	g_free(slot);
}

static gboolean ril_plugin_slot_start_timeout(gpointer user_data)
{
	ril_slot *slot = user_data;
	ril_plugin* plugin = slot->plugin;

	DBG("%s", slot->path);
	plugin->slots = g_slist_remove(plugin->slots, slot);
	slot->start_timeout_id = 0;
	ril_slot_free(slot);
	ril_plugin_check_if_started(plugin);
	return G_SOURCE_REMOVE;
}

static ril_slot *ril_plugin_slot_new_take(char *transport,
			GHashTable *transport_params, char *dbus_path,
			char *name, guint slot_index)
{
	ril_slot *slot = g_new0(ril_slot, 1);
	struct ril_slot_config *config = &slot->config;

	slot->transport_name = transport;
	slot->transport_params = transport_params;
	slot->path = dbus_path;
	slot->name = name;
	config->slot = slot_index;
	config->techs = RILMODEM_DEFAULT_TECHS;
	config->lte_network_mode = RILMODEM_DEFAULT_LTE_MODE;
	config->umts_network_mode = RILMODEM_DEFAULT_UMTS_MODE;
	config->network_mode_timeout = RILMODEM_DEFAULT_NETWORK_MODE_TIMEOUT;
	config->network_selection_timeout =
		RILMODEM_DEFAULT_NETWORK_SELECTION_TIMEOUT;
	config->signal_strength_dbm_weak = RILMODEM_DEFAULT_DBM_WEAK;
	config->signal_strength_dbm_strong = RILMODEM_DEFAULT_DBM_STRONG;
	config->empty_pin_query = RILMODEM_DEFAULT_EMPTY_PIN_QUERY;
	config->radio_power_cycle = RILMODEM_DEFAULT_RADIO_POWER_CYCLE;
	config->confirm_radio_power_on =
		RILMODEM_DEFAULT_CONFIRM_RADIO_POWER_ON;
	config->enable_voicecall = RILMODEM_DEFAULT_ENABLE_VOICECALL;
	config->enable_cbs = RILMODEM_DEFAULT_ENABLE_CBS;
	config->enable_stk = RILMODEM_DEFAULT_ENABLE_STK;
	config->query_available_band_mode =
		RILMODEM_DEFAULT_QUERY_AVAILABLE_BAND_MODE;
	config->replace_strange_oper = RILMODEM_DEFAULT_REPLACE_STRANGE_OPER;
	config->network_selection_manual_0 =
		RILMODEM_DEFAULT_NETWORK_SELECTION_MANUAL_0;
	config->force_gsm_when_radio_off =
		RILMODEM_DEFAULT_FORCE_GSM_WHEN_RADIO_OFF;
	config->use_data_profiles = RILMODEM_DEFAULT_USE_DATA_PROFILES;
	config->mms_data_profile_id = RILMODEM_DEFAULT_MMS_DATA_PROFILE_ID;
	config->cell_info_interval_short_ms =
				RILMODEM_DEFAULT_CELL_INFO_INTERVAL_SHORT_MS;
	config->cell_info_interval_long_ms =
				RILMODEM_DEFAULT_CELL_INFO_INTERVAL_LONG_MS;
	config->ril_request_on_set_udub =
				RILMODEM_DEFAULT_RIL_REQUEST_ON_SET_UDUB;
	slot->timeout = RILMODEM_DEFAULT_TIMEOUT;
	slot->sim_flags = RILMODEM_DEFAULT_SIM_FLAGS;
	slot->slot_flags = RILMODEM_DEFAULT_SLOT_FLAGS;
	slot->legacy_imei_query = RILMODEM_DEFAULT_LEGACY_IMEI_QUERY;
	slot->start_timeout = RILMODEM_DEFAULT_START_TIMEOUT;
	slot->data_opt.allow_data = RILMODEM_DEFAULT_DATA_OPT;
	slot->data_opt.data_call_format = RILMODEM_DEFAULT_DATA_CALL_FORMAT;
	slot->data_opt.data_call_retry_limit =
		RILMODEM_DEFAULT_DATA_CALL_RETRY_LIMIT;
	slot->data_opt.data_call_retry_delay_ms =
		RILMODEM_DEFAULT_DATA_CALL_RETRY_DELAY;
	slot->devmon = ril_devmon_auto_new(config);
	slot->watch = ofono_watch_new(dbus_path);
	slot->watch_event_id[WATCH_EVENT_MODEM] =
		ofono_watch_add_modem_changed_handler(slot->watch,
			ril_plugin_slot_modem_changed, slot);
	return slot;
}

static void ril_plugin_slot_apply_vendor_defaults(ril_slot *slot)
{
	if (slot->vendor_driver) {
		struct ril_slot_config *config = &slot->config;
		struct ril_vendor_defaults defaults;

		/* Let the vendor extension to adjust (some) defaults */
		memset(&defaults, 0, sizeof(defaults));
		defaults.legacy_imei_query = slot->legacy_imei_query;
		defaults.enable_cbs = config->enable_cbs;
		defaults.enable_stk = config->enable_stk;
		defaults.empty_pin_query = config->empty_pin_query;
		defaults.mms_data_profile_id = config->mms_data_profile_id;
		defaults.use_data_profiles = config->use_data_profiles;
		defaults.replace_strange_oper = config->replace_strange_oper;
		defaults.force_gsm_when_radio_off =
			config->force_gsm_when_radio_off;
		defaults.query_available_band_mode =
			config->query_available_band_mode;

		ril_vendor_get_defaults(slot->vendor_driver, &defaults);
		slot->legacy_imei_query = defaults.legacy_imei_query;
		config->enable_cbs = defaults.enable_cbs;
		config->enable_stk = defaults.enable_stk;
		config->empty_pin_query = defaults.empty_pin_query;
		config->use_data_profiles = defaults.use_data_profiles;
		config->mms_data_profile_id = defaults.mms_data_profile_id;
		config->replace_strange_oper = defaults.replace_strange_oper;
		config->force_gsm_when_radio_off =
			defaults.force_gsm_when_radio_off;
		config->query_available_band_mode =
			defaults.query_available_band_mode;
	}
}

static ril_slot *ril_plugin_slot_new_socket(const char *sockpath,
				const char *sub, const char *dbus_path,
				const char *name, guint slot_index)
{
	/* RIL socket configuration */
	GHashTable *params = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);

	g_hash_table_insert(params, g_strdup(RIL_TRANSPORT_SOCKET_PATH),
							g_strdup(sockpath));
	if (sub) {
		g_hash_table_insert(params, g_strdup(RIL_TRANSPORT_SOCKET_SUB),
							g_strdup(sub));
	}

	return ril_plugin_slot_new_take(g_strdup(RIL_TRANSPORT_SOCKET), params,
			g_strdup(dbus_path), g_strdup(name), slot_index);
}

static GSList *ril_plugin_create_default_config()
{
	GSList *list = NULL;

	if (g_file_test(RILMODEM_DEFAULT_SOCK2, G_FILE_TEST_EXISTS)) {
		DBG("Falling back to default dual SIM config");
		list = g_slist_append(list, ril_plugin_slot_new_socket
				(RILMODEM_DEFAULT_SOCK, NULL,
					RILCONF_PATH_PREFIX "0", "RIL1", 0));
		list = g_slist_append(list, ril_plugin_slot_new_socket
				(RILMODEM_DEFAULT_SOCK2, NULL,
					RILCONF_PATH_PREFIX "1", "RIL2", 1));
	} else {
		DBG("Falling back to default single SIM config");
		list = g_slist_append(list, ril_plugin_slot_new_socket
				(RILMODEM_DEFAULT_SOCK, RILMODEM_DEFAULT_SUB,
					RILCONF_PATH_PREFIX "0", "RIL", 0));
	}

	return list;
}

/*
 * Parse the spec according to the following grammar:
 *
 *   spec: transport | transport ':' parameters
 *   params: param | params ';' param
 *   param: name '=' value
 *   transport: STRING
 *   name: STRING
 *   value: STRING
 *
 * For example, a RIL socket spec may look like this:
 *
 *   socket:path=/dev/socket/rild;sub=SUB1
 */
static char *ril_plugin_parse_transport_spec(const char *spec,
							GHashTable *params)
{
	char *transport = NULL;
	char *sep = strchr(spec, ':');

	if (sep) {
		transport = g_strstrip(g_strndup(spec, sep - spec));
		if (transport[0]) {
			char **list = g_strsplit(sep + 1, ";", 0);
			char **ptr;

			for (ptr = list; *ptr; ptr++) {
				const char *p = *ptr;

				sep = strchr(p, '=');
				if (sep) {
					char *name = g_strndup(p, sep - p);
					char* value = g_strdup(sep + 1);

					g_hash_table_insert(params,
							g_strstrip(name),
							g_strstrip(value));
				}
			}
			g_strfreev(list);
			return transport;
		}
	} else {
		/* Use default transport attributes */
		transport = g_strstrip(g_strdup(spec));
		if (transport[0]) {
			return transport;
		}
	}
	g_free(transport);
	return NULL;
}

static ril_slot *ril_plugin_parse_config_group(GKeyFile *file,
							const char *group)
{
	ril_slot *slot;
	struct ril_slot_config *config;
	gboolean bval;
	int ival;
	char *sval;
	char **strv;
	char *modem;
	GUtilInts *ints;
	GHashTable *transport_params = g_hash_table_new_full(g_str_hash,
						g_str_equal, g_free, g_free);
	char *transport = NULL;
	char *transport_spec = g_key_file_get_string(file, group,
						RILCONF_TRANSPORT, NULL);

	if (transport_spec) {
		transport = ril_plugin_parse_transport_spec(transport_spec,
							transport_params);
		if (transport) {
			DBG("%s: %s:%s", group, transport,
					strchr(transport_spec, ':') + 1);
		}
		g_free(transport_spec);
	} else {
		/* Fall back to socket transport */
		char *sockpath = g_key_file_get_string(file, group,
							RILCONF_SOCKET, NULL);

		if (sockpath) {
			char *sub = g_key_file_get_string(file, group,
							RILCONF_SUB, NULL);

			transport = g_strdup(RIL_TRANSPORT_SOCKET);
			g_hash_table_insert(transport_params,
					g_strdup(RIL_TRANSPORT_SOCKET_PATH),
					sockpath);
			if (sub && strlen(sub) == RIL_SUB_SIZE) {
				DBG("%s: %s:%s", group, sockpath, sub);
				g_hash_table_insert(transport_params,
					g_strdup(RIL_TRANSPORT_SOCKET_SUB),
					sub);
			} else {
				DBG("%s: %s", group, sockpath);
				g_free(sub);
			}
		}
	}

	if (!transport) {
		ofono_warn("No usable RIL transport defined for %s", group);
		g_hash_table_destroy(transport_params);
		return NULL;
	}

	/* ril_plugin_slot_new_take() will take ownership of this memory */
	modem = g_strconcat("/", group, NULL);

	/* Add "modem" entry to point to the actual modem path */
	g_hash_table_replace(transport_params, g_strdup(RIL_TRANSPORT_MODEM),
							g_strdup(modem));

	slot = ril_plugin_slot_new_take(transport, transport_params, modem,
			ril_config_get_string(file, group, RILCONF_NAME),
			RILMODEM_DEFAULT_SLOT);
	config = &slot->config;

	/* slot */
	if (ril_config_get_integer(file, group, RILCONF_SLOT, &ival) &&
								ival >= 0) {
		config->slot = ival;
		DBG("%s: " RILCONF_SLOT " %u", group, config->slot);
	}

	/* vendorDriver */
	sval = ril_config_get_string(file, group, RILCONF_VENDOR_DRIVER);
	if (sval) {
		slot->vendor_driver = ril_vendor_find_driver(sval);
		if (slot->vendor_driver) {
			DBG("%s: " RILCONF_VENDOR_DRIVER " %s", group, sval);
			ril_plugin_slot_apply_vendor_defaults(slot);
		} else {
			ofono_warn("Unknown vendor '%s'", sval);
		}
		g_free(sval);
	}

	/* startTimeout */
	if (ril_config_get_integer(file, group, RILCONF_START_TIMEOUT,
							&ival) && ival >= 0) {
		DBG("%s: " RILCONF_START_TIMEOUT " %d ms", group, ival);
		slot->start_timeout = ival;
	}

	/* timeout */
	if (ril_config_get_integer(file, group, RILCONF_TIMEOUT,
							&slot->timeout)) {
		DBG("%s: " RILCONF_TIMEOUT " %d", group, slot->timeout);
	}

	/* enableVoicecall */
	if (ril_config_get_boolean(file, group, RILCONF_ENABLE_VOICECALL,
					&config->enable_voicecall)) {
		DBG("%s: " RILCONF_ENABLE_VOICECALL " %s", group,
				config->enable_voicecall ? "yes" : "no");
	}

	/* enableCellBroadcast */
	if (ril_config_get_boolean(file, group, RILCONF_ENABLE_CBS,
					&config->enable_cbs)) {
		DBG("%s: " RILCONF_ENABLE_CBS " %s", group,
				config->enable_cbs ? "yes" : "no");
	}

	/* enableSimTookit */
	if (ril_config_get_boolean(file, group, RILCONF_ENABLE_STK,
					&config->enable_stk)) {
		DBG("%s: " RILCONF_ENABLE_STK " %s", group,
				config->enable_stk ? "yes" : "no");
	}

	/* replaceStrangeOperatorNames */
	if (ril_config_get_boolean(file, group,
					RILCONF_REPLACE_STRANGE_OPER,
					&config->replace_strange_oper)) {
		DBG("%s: " RILCONF_REPLACE_STRANGE_OPER " %s", group,
			config->replace_strange_oper ? "yes" : "no");
	}

	/* networkSelectionManual0 */
	if (ril_config_get_boolean(file, group,
					RILCONF_NETWORK_SELECTION_MANUAL_0,
					&config->network_selection_manual_0)) {
		DBG("%s: " RILCONF_NETWORK_SELECTION_MANUAL_0 " %s", group,
			config->network_selection_manual_0 ? "yes" : "no");
	}

	/* forceGsmWhenRadioOff */
	if (ril_config_get_boolean(file, group,
					RILCONF_FORCE_GSM_WHEN_RADIO_OFF,
					&config->force_gsm_when_radio_off)) {
		DBG("%s: " RILCONF_FORCE_GSM_WHEN_RADIO_OFF " %s", group,
			config->force_gsm_when_radio_off ? "yes" : "no");
	}

	/* useDataProfiles */
	if (ril_config_get_boolean(file, group, RILCONF_USE_DATA_PROFILES,
					&config->use_data_profiles)) {
		DBG("%s: " RILCONF_USE_DATA_PROFILES " %s", group,
			config->use_data_profiles ? "yes" : "no");
	}

	/* mmsDataProfileId */
	if (ril_config_get_integer(file, group, RILCONF_MMS_DATA_PROFILE_ID,
							&ival) && ival >= 0) {
		config->mms_data_profile_id = ival;
		DBG("%s: " RILCONF_MMS_DATA_PROFILE_ID " %u", group,
						config->mms_data_profile_id);
	}

	/* technologies */
	strv = ril_config_get_strings(file, group, RILCONF_TECHNOLOGIES, ',');
	if (strv) {
		char **p;

		config->techs = 0;
		for (p = strv; *p; p++) {
			const char *s = *p;
			enum ofono_radio_access_mode m;

			if (!s[0]) {
				continue;
			}

			if (!strcmp(s, "all")) {
				config->techs = OFONO_RADIO_ACCESS_MODE_ALL;
				break;
			}

			if (!ofono_radio_access_mode_from_string(s, &m)) {
				ofono_warn("Unknown technology %s in [%s] "
					"section of %s", s, group,
					RILMODEM_CONF_FILE);
				continue;
			}

			if (m == OFONO_RADIO_ACCESS_MODE_ANY) {
				config->techs = OFONO_RADIO_ACCESS_MODE_ALL;
				break;
			}

			config->techs |= m;
		}
		g_strfreev(strv);
	}
	
	/* lteNetworkMode */
	if (ril_config_get_integer(file, group, RILCONF_LTE_MODE, &ival)) {
		DBG("%s: " RILCONF_LTE_MODE " %d", group, ival);
		config->lte_network_mode = ival;
	}

	/* umtsNetworkMode */
	if (ril_config_get_integer(file, group, RILCONF_UMTS_MODE, &ival)) {
		DBG("%s: " RILCONF_UMTS_MODE " %d", group, ival);
		config->umts_network_mode = ival;
	}

	/* networkModeTimeout */
	if (ril_config_get_integer(file, group, RILCONF_NETWORK_MODE_TIMEOUT,
					&config->network_mode_timeout)) {
		DBG("%s: " RILCONF_NETWORK_MODE_TIMEOUT " %d", group,
					config->network_mode_timeout);
	}

	/* networkSelectionTimeout */
	if (ril_config_get_integer(file, group,
				RILCONF_NETWORK_SELECTION_TIMEOUT,
				&config->network_selection_timeout)) {
		DBG("%s: " RILCONF_NETWORK_SELECTION_TIMEOUT " %d", group,
				config->network_selection_timeout);
	}

	/* signalStrengthRange */
	ints = ril_config_get_ints(file, group, RILCONF_SIGNAL_STRENGTH_RANGE);
	if (gutil_ints_get_count(ints) == 2) {
		const int* dbms = gutil_ints_get_data(ints, NULL);

		/* MIN,MAX */
		if (dbms[0] < dbms[1]) {
			DBG("%s: " RILCONF_SIGNAL_STRENGTH_RANGE " [%d,%d]",
						group, dbms[0], dbms[1]);
			config->signal_strength_dbm_weak = dbms[0];
			config->signal_strength_dbm_strong = dbms[1];
		}
	}
	gutil_ints_unref(ints);

	/* enable4G (deprecated but still supported) */
	ival = config->techs;
	if (ril_config_get_flag(file, group, RILCONF_4G,
			OFONO_RADIO_ACCESS_MODE_LTE, &ival)) {
		config->techs = ival;
	}

	DBG("%s: technologies 0x%02x", group, config->techs);

	/* emptyPinQuery */
	if (ril_config_get_boolean(file, group, RILCONF_EMPTY_PIN_QUERY,
					&config->empty_pin_query)) {
		DBG("%s: " RILCONF_EMPTY_PIN_QUERY " %s", group,
				config->empty_pin_query ? "on" : "off");
	}

	/* radioPowerCycle */
	if (ril_config_get_boolean(file, group, RILCONF_RADIO_POWER_CYCLE,
					&config->radio_power_cycle)) {
		DBG("%s: " RILCONF_RADIO_POWER_CYCLE " %s", group,
				config->radio_power_cycle ? "on" : "off");
	}

	/* confirmRadioPowerOn */
	if (ril_config_get_boolean(file, group, RILCONF_CONFIRM_RADIO_POWER_ON,
					&config->confirm_radio_power_on)) {
		DBG("%s: " RILCONF_CONFIRM_RADIO_POWER_ON " %s", group,
				config->confirm_radio_power_on ? "on" : "off");
	}

	/* singleDataContext */
	if (ril_config_get_boolean(file, group, RILCONF_SINGLE_DATA_CONTEXT,
							&bval) && bval) {
		DBG("%s: " RILCONF_SINGLE_DATA_CONTEXT " %s", group,
							bval ? "on" : "off");
		slot->slot_flags |= SAILFISH_SLOT_SINGLE_CONTEXT;
	}

	/* uiccWorkaround */
	if (ril_config_get_flag(file, group, RILCONF_UICC_WORKAROUND,
			RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND,
			&slot->sim_flags)) {
		DBG("%s: " RILCONF_UICC_WORKAROUND " %s",
			group, (slot->sim_flags &
			RIL_SIM_CARD_V9_UICC_SUBSCRIPTION_WORKAROUND) ?
			"on" : "off");
	}

	/* allowDataReq */
	if (ril_config_get_enum(file, group, RILCONF_ALLOW_DATA_REQ, &ival,
				"auto", RIL_ALLOW_DATA_AUTO,
				"on", RIL_ALLOW_DATA_ENABLED,
				"off", RIL_ALLOW_DATA_DISABLED, NULL)) {
		DBG("%s: " RILCONF_ALLOW_DATA_REQ " %s", group,
				ival == RIL_ALLOW_DATA_ENABLED ? "enabled":
				ival == RIL_ALLOW_DATA_DISABLED ? "disabled":
				"auto");
		slot->data_opt.allow_data = ival;
	}

	/* dataCallFormat */
	if (ril_config_get_enum(file, group, RILCONF_DATA_CALL_FORMAT, &ival,
				"auto", RIL_DATA_CALL_FORMAT_AUTO,
				"6", RIL_DATA_CALL_FORMAT_6,
				"9", RIL_DATA_CALL_FORMAT_9,
				"11", RIL_DATA_CALL_FORMAT_11, NULL)) {
		if (ival == RIL_DATA_CALL_FORMAT_AUTO) {
			DBG("%s: " RILCONF_DATA_CALL_FORMAT " auto", group);
		} else {
			DBG("%s: " RILCONF_DATA_CALL_FORMAT " %d", group, ival);
		}
		slot->data_opt.data_call_format = ival;
	}

	/* dataCallRetryLimit */
	if (ril_config_get_integer(file, group, RILCONF_DATA_CALL_RETRY_LIMIT,
						&ival) && ival >= 0) {
		DBG("%s: " RILCONF_DATA_CALL_RETRY_LIMIT " %d", group, ival);
		slot->data_opt.data_call_retry_limit = ival;
	}

	/* dataCallRetryDelay */
	if (ril_config_get_integer(file, group, RILCONF_DATA_CALL_RETRY_DELAY,
						&ival) && ival >= 0) {
		DBG("%s: " RILCONF_DATA_CALL_RETRY_DELAY " %d ms", group, ival);
		slot->data_opt.data_call_retry_delay_ms = ival;
	}

	/* ecclistFile */
	slot->ecclist_file = ril_config_get_string(file, group,
						RILCONF_ECCLIST_FILE);
	if (slot->ecclist_file && slot->ecclist_file[0]) {
		DBG("%s: " RILCONF_ECCLIST_FILE " %s", group,
							slot->ecclist_file);
	} else {
		g_free(slot->ecclist_file);
		slot->ecclist_file = NULL;
	}

	/* localHangupReasons */
	config->local_hangup_reasons = ril_config_get_ints(file, group,
						RILCONF_LOCAL_HANGUP_REASONS);
	sval = ril_config_ints_to_string(config->local_hangup_reasons, ',');
	if (sval) {
		DBG("%s: " RILCONF_LOCAL_HANGUP_REASONS " %s", group, sval);
		g_free(sval);
	}

	/* remoteHangupReasons */
	config->remote_hangup_reasons = ril_config_get_ints(file, group,
						RILCONF_REMOTE_HANGUP_REASONS);
	sval = ril_config_ints_to_string(config->remote_hangup_reasons, ',');
	if (sval) {
		DBG("%s: " RILCONF_REMOTE_HANGUP_REASONS " %s", group, sval);
		g_free(sval);
	}

	/* legacyImeiQuery */
	if (ril_config_get_boolean(file, group, RILCONF_LEGACY_IMEI_QUERY,
					&slot->legacy_imei_query)) {
		DBG("%s: " RILCONF_LEGACY_IMEI_QUERY " %s", group,
				slot->legacy_imei_query ? "on" : "off");
	}

	/* cellInfoIntervalShortMs */
	if (ril_config_get_integer(file, group,
				RILCONF_CELL_INFO_INTERVAL_SHORT_MS,
				&config->cell_info_interval_short_ms)) {
		DBG("%s: " RILCONF_CELL_INFO_INTERVAL_SHORT_MS " %d", group,
					config->cell_info_interval_short_ms);
	}

	/* cellInfoIntervalLongMs */
	if (ril_config_get_integer(file, group,
				RILCONF_CELL_INFO_INTERVAL_LONG_MS,
				&config->cell_info_interval_long_ms)) {
		DBG("%s: " RILCONF_CELL_INFO_INTERVAL_LONG_MS " %d",
				group, config->cell_info_interval_long_ms);
	}

	/* rilRequestOnSetUdub */
	if (ril_config_get_integer(file, group,
				RILCONF_RIL_REQUEST_ON_SET_UDUB,
				&config->ril_request_on_set_udub)) {
		DBG("%s: " RILCONF_RIL_REQUEST_ON_SET_UDUB " %d",
				group, config->ril_request_on_set_udub);
	}

	/* Replace devmon with a new one with applied settings */
	ril_devmon_free(slot->devmon);
	slot->devmon = NULL;

	/* deviceStateTracking */
	if (ril_config_get_mask(file, group, RILCONF_DEVMON, &ival,
				"ds", RIL_DEVMON_DS,
				"ss", RIL_DEVMON_SS,
				"ur", RIL_DEVMON_UR, NULL) && ival) {
		int n = 0;
		struct ril_devmon *devmon[3];

		if (ival & RIL_DEVMON_DS) {
			devmon[n++] = ril_devmon_ds_new(config);
		}
		if (ival & RIL_DEVMON_SS) {
			devmon[n++] = ril_devmon_ss_new(config);
		}
		if (ival & RIL_DEVMON_UR) {
			devmon[n++] = ril_devmon_ur_new(config);
		}
		DBG("%s: " RILCONF_DEVMON " 0x%x", group, ival);
		slot->devmon = ril_devmon_combine(devmon, n);
	} else {
		/* Try special values */
		sval = ril_config_get_string(file, group, RILCONF_DEVMON);
		if (sval) {
			if (!g_ascii_strcasecmp(sval, "none")) {
				DBG("%s: " RILCONF_DEVMON " %s", group, sval);
			} else if (!g_ascii_strcasecmp(sval, "auto")) {
				DBG("%s: " RILCONF_DEVMON " %s", group, sval);
				slot->devmon = ril_devmon_auto_new(config);
			}
			g_free(sval);
		} else {
			/* This is the default */
			slot->devmon = ril_devmon_auto_new(config);
		}
	}

	return slot;
}

static GSList *ril_plugin_add_slot(GSList *slots, ril_slot *new_slot)
{
	GSList *link = slots;

	/* Slot numbers and paths must be unique */
	while (link) {
		GSList *next = link->next;
		ril_slot *slot = link->data;
		gboolean delete_this_slot = FALSE;

		if (!strcmp(slot->path, new_slot->path)) {
			ofono_error("Duplicate modem path '%s'", slot->path);
			delete_this_slot = TRUE;
		} else if (slot->config.slot != RILMODEM_DEFAULT_SLOT &&
				slot->config.slot == new_slot->config.slot) {
			ofono_error("Duplicate RIL slot %u", slot->config.slot);
			delete_this_slot = TRUE;
		}

		if (delete_this_slot) {
			slots = g_slist_delete_link(slots, link);
			ril_slot_free(slot);
		}

		link = next;
	}

	return g_slist_append(slots, new_slot);
}

static ril_slot *ril_plugin_find_slot_number(GSList *slots, guint number)
{
	while (slots) {
		ril_slot *slot = slots->data;

		if (slot->config.slot == number) {
			return slot;
		}
		slots = slots->next;
	}
	return NULL;
}

static guint ril_plugin_find_unused_slot(GSList *slots)
{
	guint number = 0;

	while (ril_plugin_find_slot_number(slots, number)) number++;
	return number;
}

static void ril_plugin_parse_identity(struct ril_plugin_identity *identity,
							const char *value)
{
	char *sep = strchr(value, ':');
	const char *user = value;
	const char *group = NULL;
	char *tmp_user = NULL;
	const struct passwd *pw = NULL;
	const struct group *gr = NULL;

	if (sep) {
		/* Group */
		group = sep + 1;
		gr = getgrnam(group);
		user = tmp_user = g_strndup(value, sep - value);

		if (!gr) {
			int n;

			/* Try numeric */
			if (gutil_parse_int(group, 0, &n)) {
				gr = getgrgid(n);
			}
		}
	}

	/* User */
	pw = getpwnam(user);
	if (!pw) {
		int n;

		/* Try numeric */
		if (gutil_parse_int(user, 0, &n)) {
			pw = getpwuid(n);
		}
	}

	if (pw) {
		DBG("User %s -> %d", user, pw->pw_uid);
		identity->uid = pw->pw_uid;
	} else {
		ofono_warn("Invalid user '%s'", user);
	}

	if (gr) {
		DBG("Group %s -> %d", group, gr->gr_gid);
		identity->gid = gr->gr_gid;
	} else if (group) {
		ofono_warn("Invalid group '%s'", group);
	}

	g_free(tmp_user);
}

static GSList *ril_plugin_parse_config_file(GKeyFile *file,
					struct ril_plugin_settings *ps)
{
	GSList *l, *list = NULL;
	gsize i, n = 0;
	gchar **groups = g_key_file_get_groups(file, &n);

	for (i=0; i<n; i++) {
		const char *group = groups[i];
		if (g_str_has_prefix(group, RILCONF_MODEM_PREFIX)) {
			/* Modem configuration */
			ril_slot *slot = ril_plugin_parse_config_group(file,
									group);

			if (slot) {
				list = ril_plugin_add_slot(list, slot);
			}
		} else if (!strcmp(group, RILCONF_SETTINGS_GROUP)) {
			/* Plugin configuration */
			int ival;
			char *sval;

			/* 3GLTEHandover */
			ril_config_get_flag(file, group,
				RILCONF_SETTINGS_3GHANDOVER,
				RIL_DATA_MANAGER_3GLTE_HANDOVER,
				&ps->dm_flags);

			/* ForceGsmForNonDataSlots */
			ril_config_get_flag(file, group,
				RILCONF_SETTINGS_GSM_NON_DATA_SLOTS,
				RIL_DATA_MANAGER_FORCE_GSM_ON_OTHER_SLOTS,
				&ps->dm_flags);

			/* SetRadioCapability */
			if (ril_config_get_enum(file, group,
				RILCONF_SETTINGS_SET_RADIO_CAP, &ival,
				"auto", RIL_SET_RADIO_CAP_AUTO,
				"on", RIL_SET_RADIO_CAP_ENABLED,
				"off", RIL_SET_RADIO_CAP_DISABLED, NULL)) {
				ps->set_radio_cap = ival;
			}

			/* Identity */
			sval = g_key_file_get_string(file, group,
					RILCONF_SETTINGS_IDENTITY, NULL);
			if (sval) {
				ril_plugin_parse_identity(&ps->identity, sval);
				g_free(sval);
			}
		}
	}

	/* Automatically assign slot numbers */
	for (l = list; l; l = l->next) {
		ril_slot *slot = l->data;

		if (slot->config.slot == RILMODEM_DEFAULT_SLOT) {
			slot->config.slot = ril_plugin_find_unused_slot(list);
		}
	}

	g_strfreev(groups);
	return list;
}

static GSList *ril_plugin_load_config(const char *path,
				struct ril_plugin_settings *ps)
{
	GSList *l, *list = NULL;
	GKeyFile *file = g_key_file_new();
	gboolean empty = FALSE;

	config_merge_files(file, path);
	if (ril_config_get_boolean(file, RILCONF_SETTINGS_GROUP,
				RILCONF_SETTINGS_EMPTY, &empty) && empty) {
		DBG("Empty config");
	} else {
		list = ril_plugin_parse_config_file(file, ps);
	}

	if (!list && !empty) {
		list = ril_plugin_create_default_config();
	}

	/* Initialize start timeouts */
	for (l = list; l; l = l->next) {
		ril_slot *slot = l->data;

		GASSERT(!slot->start_timeout_id);
		slot->start_timeout_id = g_timeout_add(slot->start_timeout,
					ril_plugin_slot_start_timeout, slot);
	}

	g_key_file_free(file);
	return list;
}

static void ril_plugin_set_perm(const char *path, mode_t mode,
					const struct ril_plugin_identity *id)
{
	if (chmod(path, mode)) {
		ofono_error("chmod(%s,%o) failed: %s", path, mode,
							strerror(errno));
	}
	if (chown(path, id->uid, id->gid)) {
		ofono_error("chown(%s,%d,%d) failed: %s", path, id->uid,
						id->gid, strerror(errno));
	}
}

/* Recursively updates file and directory ownership and permissions */
static void ril_plugin_set_storage_perm(const char *path,
			const struct ril_plugin_identity *id)
{
	DIR *d;
	const mode_t dir_mode = S_IRUSR | S_IWUSR | S_IXUSR;
	const mode_t file_mode = S_IRUSR | S_IWUSR;

	ril_plugin_set_perm(path, dir_mode, id);
	d = opendir(path);
	if (d) {
		const struct dirent *p;

		while ((p = readdir(d)) != NULL) {
			char *buf;
			struct stat st;

			if (!strcmp(p->d_name, ".") ||
					!strcmp(p->d_name, "..")) {
				continue;
			}

			buf = g_strdup_printf("%s/%s", path, p->d_name);
			if (!stat(buf, &st)) {
				mode_t mode;

				if (S_ISDIR(st.st_mode)) {
					ril_plugin_set_storage_perm(buf, id);
					mode = dir_mode;
				} else {
					mode = file_mode;
				}
				ril_plugin_set_perm(buf, mode, id);
			}
			g_free(buf);
		}
		closedir(d);
	}
}

static void ril_plugin_switch_identity(const struct ril_plugin_identity *id)
{
	ril_plugin_set_storage_perm(ofono_storage_dir(), id);
	if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
		ofono_error("prctl(PR_SET_KEEPCAPS) failed: %s",
							strerror(errno));
	} else if (setgid(id->gid) < 0) {
		ofono_error("setgid(%d) failed: %s", id->gid, strerror(errno));
	} else if (setuid(id->uid) < 0) {
		ofono_error("setuid(%d) failed: %s", id->uid, strerror(errno));
	} else {
		struct __user_cap_header_struct header;
		struct __user_cap_data_struct cap;

		memset(&header, 0, sizeof(header));
		memset(&cap, 0, sizeof(cap));

		header.version = _LINUX_CAPABILITY_VERSION;
		cap.effective = cap.permitted = (1 << CAP_NET_ADMIN) |
							(1 << CAP_NET_RAW);

		if (syscall(SYS_capset, &header, &cap) < 0) {
			ofono_error("syscall(SYS_capset) failed: %s",
							strerror(errno));
		}
	}
}

static void ril_plugin_init_slots(ril_plugin *plugin)
{
	int i;
	GSList *link;

	for (i = 0, link = plugin->slots; link; link = link->next, i++) {
		ril_slot *slot = link->data;

		slot->index = i;
		slot->plugin = plugin;
		slot->sim_settings = ril_sim_settings_new(slot->path,
							slot->config.techs);
		slot->retry_id = g_idle_add(ril_plugin_retry_init_io_cb, slot);
	}
}

static void ril_plugin_drop_orphan_slots(ril_plugin *plugin)
{
	GSList *l = plugin->slots;

	while (l) {
		GSList *next = l->next;
		ril_slot *slot = l->data;

		if (!slot->handle) {
			plugin->slots = g_slist_delete_link(plugin->slots, l);
			ril_slot_free(slot);
		}
		l = next;
	}
}

static gboolean ril_plugin_manager_start_timeout(gpointer user_data)
{
	ril_plugin *plugin = user_data;

	DBG("");
	plugin->start_timeout_id = 0;
	ril_plugin_manager_started(plugin);
	return G_SOURCE_REMOVE;
}

static void ril_plugin_manager_start_done(gpointer user_data)
{
	ril_plugin *plugin = user_data;

	DBG("");
	if (plugin->start_timeout_id) {
		/* Startup was cancelled */
		plugin->start_timeout_id = 0;
		ril_plugin_drop_orphan_slots(plugin);
	}
}

static ril_plugin *ril_plugin_manager_create(struct sailfish_slot_manager *m)
{
	ril_plugin *plugin = g_new0(ril_plugin, 1);
	struct ril_plugin_settings *ps = &plugin->settings;

	DBG("");
	plugin->handle = m;
	ril_plugin_parse_identity(&ps->identity, RILMODEM_DEFAULT_IDENTITY);
	ps->dm_flags = RILMODEM_DEFAULT_DM_FLAGS;
	ps->set_radio_cap = RIL_SET_RADIO_CAP_AUTO;
	return plugin;
}

static void ril_plugin_slot_check_timeout_cb(ril_slot *slot, void *param)
{
	guint *timeout = param;

	if ((*timeout) < slot->start_timeout) {
		(*timeout) = slot->start_timeout;
	}
}

static guint ril_plugin_manager_start(ril_plugin *plugin)
{
	struct ril_plugin_settings *ps = &plugin->settings;
	guint start_timeout = 0;
	char* config_file = g_build_filename(ofono_config_dir(),
						RILMODEM_CONF_FILE, NULL);

	DBG("");
	GASSERT(!plugin->start_timeout_id);
	plugin->slots = ril_plugin_load_config(config_file, ps);
	plugin->data_manager = ril_data_manager_new(ps->dm_flags);
	ril_plugin_init_slots(plugin);
	g_free(config_file);

	ofono_modem_driver_register(&ril_modem_driver);
	ofono_sim_driver_register(&ril_sim_driver);
	ofono_sms_driver_register(&ril_sms_driver);
	ofono_netmon_driver_register(&ril_netmon_driver);
	ofono_netreg_driver_register(&ril_netreg_driver);
	ofono_devinfo_driver_register(&ril_devinfo_driver);
	ofono_voicecall_driver_register(&ril_voicecall_driver);
	ofono_call_barring_driver_register(&ril_call_barring_driver);
	ofono_call_forwarding_driver_register(&ril_call_forwarding_driver);
	ofono_call_settings_driver_register(&ril_call_settings_driver);
	ofono_call_volume_driver_register(&ril_call_volume_driver);
	ofono_radio_settings_driver_register(&ril_radio_settings_driver);
	ofono_gprs_driver_register(&ril_gprs_driver);
	ofono_gprs_context_driver_register(&ril_gprs_context_driver);
	ofono_phonebook_driver_register(&ril_phonebook_driver);
	ofono_ussd_driver_register(&ril_ussd_driver);
	ofono_cbs_driver_register(&ril_cbs_driver);
	ofono_stk_driver_register(&ril_stk_driver);

	ril_plugin_foreach_slot_param(plugin, ril_plugin_slot_check_timeout_cb,
							&start_timeout);

	/* Switch the user to the one RIL expects */
	ril_plugin_switch_identity(&ps->identity);

	plugin->start_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
			start_timeout, ril_plugin_manager_start_timeout,
			plugin, ril_plugin_manager_start_done);
	return plugin->start_timeout_id;
}

static void ril_plugin_manager_cancel_start(ril_plugin *plugin, guint id)
{
	g_source_remove(id);
}

static void ril_plugin_manager_free(ril_plugin *plugin)
{
	if (plugin) {
		GASSERT(!plugin->slots);
		ril_data_manager_unref(plugin->data_manager);
		ril_radio_caps_manager_remove_handler(plugin->caps_manager,
					plugin->caps_manager_event_id);
		ril_radio_caps_manager_unref(plugin->caps_manager);
		g_free(plugin);
	}
}

static void ril_slot_set_data_role(ril_slot *slot, enum sailfish_data_role r)
{
	enum ril_data_role role =
		(r == SAILFISH_DATA_ROLE_INTERNET) ? RIL_DATA_ROLE_INTERNET :
		(r == SAILFISH_DATA_ROLE_MMS) ? RIL_DATA_ROLE_MMS :
		RIL_DATA_ROLE_NONE;
	ril_data_allow(slot->data, role);
	ril_radio_caps_request_free(slot->caps_req);
	if (role == RIL_DATA_ROLE_NONE) {
		slot->caps_req = NULL;
	} else {
		const enum ofono_radio_access_mode mode =
			(r == SAILFISH_DATA_ROLE_MMS) ?
				OFONO_RADIO_ACCESS_MODE_GSM :
				__ofono_radio_access_max_mode
						(slot->sim_settings->techs);

		slot->caps_req = ril_radio_caps_request_new
			(slot->caps, mode, role);
	}
}

static void ril_slot_enabled_changed(struct sailfish_slot_impl *s)
{
	if (s->handle->enabled) {
		ril_plugin_check_modem(s);
		grilio_channel_set_enabled(s->io, TRUE);
	} else {
		grilio_channel_set_enabled(s->io, FALSE);
		ril_plugin_shutdown_slot(s, FALSE);
	}
}

/**
 * RIL socket transport factory
 */
static struct grilio_transport *ril_socket_transport_connect(GHashTable *args)
{
	const char* path = g_hash_table_lookup(args, RIL_TRANSPORT_SOCKET_PATH);
	const char* sub = g_hash_table_lookup(args, RIL_TRANSPORT_SOCKET_SUB);

	GASSERT(path);
	if (path) {
		DBG("%s %s", path, sub);
		return grilio_transport_socket_new_path(path, sub);
	}
	return NULL;
}

/* Global part (that requires access to global variables) */

static struct sailfish_slot_driver_reg *ril_driver = NULL;
static guint ril_driver_init_id = 0;
static const struct ofono_ril_transport ril_socket_transport = {
	.name = RIL_TRANSPORT_SOCKET,
	.api_version = OFONO_RIL_TRANSPORT_API_VERSION,
	.connect = ril_socket_transport_connect
};

static void ril_debug_trace_notify(struct ofono_debug_desc *desc)
{
	ril_plugin_foreach_slot_manager(ril_driver, ril_debug_trace_update);
}

static void ril_debug_dump_notify(struct ofono_debug_desc *desc)
{
	ril_plugin_foreach_slot_manager(ril_driver, ril_debug_dump_update);
}

static void ril_debug_grilio_notify(struct ofono_debug_desc *desc)
{
	grilio_log.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static void ril_debug_mce_notify(struct ofono_debug_desc *desc)
{
	mce_log.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static void ril_plugin_debug_notify(struct ofono_debug_desc *desc)
{
	GLOG_MODULE_NAME.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
		GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static gboolean ril_plugin_start(gpointer user_data)
{
	static const struct sailfish_slot_driver ril_slot_driver = {
		.name = RILMODEM_DRIVER,
		.manager_create = ril_plugin_manager_create,
		.manager_start = ril_plugin_manager_start,
		.manager_cancel_start = ril_plugin_manager_cancel_start,
		.manager_free = ril_plugin_manager_free,
		.slot_enabled_changed = ril_slot_enabled_changed,
		.slot_set_data_role = ril_slot_set_data_role,
		.slot_free = ril_slot_free
	};

	DBG("");
	ril_driver_init_id = 0;

	/* Socket transport can be registered right away */
	ofono_ril_transport_register(&ril_socket_transport);

	/* Register the driver */
	ril_driver = sailfish_slot_driver_register(&ril_slot_driver);
	return G_SOURCE_REMOVE;
}

static int ril_plugin_init(void)
{
	DBG("");
	GASSERT(!ril_driver);

	/*
	 * Log categories (accessible via D-Bus) are generated from
	 * ofono_debug_desc structures, while libglibutil based log
	 * functions receive the log module name. Those should match
	 * otherwise the client receiving the log won't get the category
	 * information.
	 */
	grilio_hexdump_log.name = ril_debug_dump.name;
	grilio_log.name = grilio_debug.name;
	mce_log.name = mce_debug.name;

	/*
	 * The real initialization happens later, to make sure that
	 * sailfish_manager plugin gets initialized first (and we don't
	 * depend on the order of initialization).
	 */
	ril_driver_init_id = g_idle_add(ril_plugin_start, ril_driver);
	return 0;
}

static void ril_plugin_exit(void)
{
	DBG("");

	ofono_ril_transport_unregister(&ril_socket_transport);
	ofono_modem_driver_unregister(&ril_modem_driver);
	ofono_sim_driver_unregister(&ril_sim_driver);
	ofono_sms_driver_unregister(&ril_sms_driver);
	ofono_devinfo_driver_unregister(&ril_devinfo_driver);
	ofono_netmon_driver_unregister(&ril_netmon_driver);
	ofono_netreg_driver_unregister(&ril_netreg_driver);
	ofono_voicecall_driver_unregister(&ril_voicecall_driver);
	ofono_call_barring_driver_unregister(&ril_call_barring_driver);
	ofono_call_forwarding_driver_unregister(&ril_call_forwarding_driver);
	ofono_call_settings_driver_unregister(&ril_call_settings_driver);
	ofono_call_volume_driver_unregister(&ril_call_volume_driver);
	ofono_radio_settings_driver_unregister(&ril_radio_settings_driver);
	ofono_gprs_driver_unregister(&ril_gprs_driver);
	ofono_gprs_context_driver_unregister(&ril_gprs_context_driver);
	ofono_phonebook_driver_unregister(&ril_phonebook_driver);
	ofono_ussd_driver_unregister(&ril_ussd_driver);
	ofono_cbs_driver_unregister(&ril_cbs_driver);
	ofono_stk_driver_unregister(&ril_stk_driver);

	if (ril_driver) {
		sailfish_slot_driver_unregister(ril_driver);
		ril_driver = NULL;
	}

	if (ril_driver_init_id) {
		g_source_remove(ril_driver_init_id);
		ril_driver_init_id = 0;
	}
}

OFONO_PLUGIN_DEFINE(ril, "Sailfish OS RIL plugin", VERSION,
	OFONO_PLUGIN_PRIORITY_DEFAULT, ril_plugin_init, ril_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
