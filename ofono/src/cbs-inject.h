/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OFONO_CBS_INJECT_H
#define OFONO_CBS_INJECT_H

#include <glib.h>
#include <sys/types.h>

struct cbs_inject;
typedef void (*cbs_inject_cb_t)(const unsigned char *pdu, int len, void *data);

/* dir must already exist, be owned by the daemon and have mode 0700. */
struct cbs_inject *cbs_inject_new(const char *dir, const char *modem,
			uid_t sender_uid, cbs_inject_cb_t callback, void *data);
void cbs_inject_free(struct cbs_inject *inject);

#endif
