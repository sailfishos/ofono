/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2017-2021  Jolla Ltd.
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

#ifndef __OFONO_GPRS_H
#define __OFONO_GPRS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/gprs-context.h>

struct ofono_gprs;

typedef void (*ofono_gprs_status_cb_t)(const struct ofono_error *error,
						int status, void *data);

typedef void (*ofono_gprs_cb_t)(const struct ofono_error *error, void *data);

struct ofono_gprs_driver {
	const char *name;
	int (*probe)(struct ofono_gprs *gprs, unsigned int vendor,
			void *data);
	void (*remove)(struct ofono_gprs *gprs);
	void (*set_attached)(struct ofono_gprs *gprs, int attached,
				ofono_gprs_cb_t cb, void *data);
	void (*attached_status)(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb, void *data);
	void (*list_active_contexts)(struct ofono_gprs *gprs,
					ofono_gprs_cb_t cb, void *data);
};

enum gprs_suspend_cause {
	GPRS_SUSPENDED_DETACHED,
	GPRS_SUSPENDED_SIGNALLING,
	GPRS_SUSPENDED_CALL,
	GPRS_SUSPENDED_NO_COVERAGE,
	GPRS_SUSPENDED_UNKNOWN_CAUSE,
};

void ofono_gprs_status_notify(struct ofono_gprs *gprs, int status);
void ofono_gprs_detached_notify(struct ofono_gprs *gprs);
void ofono_gprs_suspend_notify(struct ofono_gprs *gprs, int cause);
void ofono_gprs_resume_notify(struct ofono_gprs *gprs);
void ofono_gprs_bearer_notify(struct ofono_gprs *gprs, int bearer);

struct ofono_modem *ofono_gprs_get_modem(struct ofono_gprs *gprs);

int ofono_gprs_driver_register(const struct ofono_gprs_driver *d);
void ofono_gprs_driver_unregister(const struct ofono_gprs_driver *d);

struct ofono_gprs *ofono_gprs_create(struct ofono_modem *modem,
					unsigned int vendor, const char *driver,
					void *data);
void ofono_gprs_register(struct ofono_gprs *gprs);
void ofono_gprs_remove(struct ofono_gprs *gprs);

void ofono_gprs_set_data(struct ofono_gprs *gprs, void *data);
void *ofono_gprs_get_data(struct ofono_gprs *gprs);

void ofono_gprs_set_cid_range(struct ofono_gprs *gprs,
				unsigned int min, unsigned int max);
void ofono_gprs_add_context(struct ofono_gprs *gprs,
				struct ofono_gprs_context *gc);

void ofono_gprs_cid_activated(struct ofono_gprs  *gprs, unsigned int cid,
				const char *apn);

void ofono_gprs_attached_update(struct ofono_gprs *gprs);

const struct ofono_gprs_primary_context *ofono_gprs_context_settings_by_type
		(struct ofono_gprs *gprs, enum ofono_gprs_context_type type);

/* Since mer/1.24+git2 */
ofono_bool_t ofono_gprs_get_roaming_allowed(struct ofono_gprs *gprs);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_GPRS_H */
