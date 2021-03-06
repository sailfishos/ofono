/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016-2021 Jolla Ltd.
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

#include <ofono/mtu-limit.h>
#include <ofono/log.h>

#include <glib.h>

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <net/if.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

struct ofono_mtu_limit {
	int max_mtu;
	char *ifname;
	void *buf;
	int bufsize;
	GIOChannel *channel;
	guint io_watch;
	int fd;
};

static void mtu_limit_apply(struct ofono_mtu_limit *self)
{
	int fd = socket(PF_INET, SOCK_DGRAM, 0);

	if (fd >= 0) {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, self->ifname, IFNAMSIZ);
		if (ioctl(fd, SIOCGIFMTU, &ifr) < 0 ||
					ifr.ifr_mtu > self->max_mtu) {
			DBG("%s mtu %d => %d", self->ifname, ifr.ifr_mtu,
								self->max_mtu);
			ifr.ifr_mtu = self->max_mtu;
			if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) {
				ofono_error("Failed to set MTU");
			}
		}
		close(fd);
	}
}

static void mtu_limit_handle_rtattr(struct ofono_mtu_limit *self,
				const struct rtattr *rta, unsigned int len)
{
	int mtu = 0;
	const char *ifname = NULL;

	while (len > 0 && RTA_OK(rta, len) && (!mtu || !ifname)) {
		switch (rta->rta_type) {
		case IFLA_IFNAME:
			ifname = RTA_DATA(rta);
			break;
		case IFLA_MTU:
			mtu = *((int*)RTA_DATA(rta));
			break;
		}
		rta = RTA_NEXT(rta, len);
	}
	if (mtu > self->max_mtu && !g_strcmp0(ifname, self->ifname)) {
		DBG("%s %d", ifname, mtu);
		mtu_limit_apply(self);
	}
}

static void mtu_limit_handle_ifinfomsg(struct ofono_mtu_limit *self,
				const struct ifinfomsg *ifi, unsigned int len)
{
	if (ifi->ifi_flags & IFF_UP) {
		const struct rtattr *rta = IFLA_RTA(ifi);

		mtu_limit_handle_rtattr(self, rta,
					len - ((char*)rta - (char*)ifi));
	}
}

static void mtu_limit_handle_nlmsg(struct ofono_mtu_limit *self,
				const struct nlmsghdr *hdr, unsigned int len)
{
	while (len > 0 && NLMSG_OK(hdr, len)) {
		if (hdr->nlmsg_type == RTM_NEWLINK) {
			mtu_limit_handle_ifinfomsg(self, NLMSG_DATA(hdr),
						IFLA_PAYLOAD(hdr));
		}
		hdr = NLMSG_NEXT(hdr, len);
        }
}

static gboolean mtu_limit_event(GIOChannel *ch, GIOCondition cond,
							gpointer data)
{
	struct ofono_mtu_limit *self = data;
	struct sockaddr_nl addr;
	socklen_t addrlen = sizeof(addr);
	ssize_t result = recvfrom(self->fd, self->buf, self->bufsize, 0,
				(struct sockaddr *)&addr, &addrlen);

	if (result > 0) {
		if (!addr.nl_pid) {
			mtu_limit_handle_nlmsg(self, self->buf, result);
		}
		return G_SOURCE_CONTINUE;
	} else if (result == 0 || errno == EINTR || errno == EAGAIN) {
		return G_SOURCE_CONTINUE;
	} else {
		DBG("%s error %d", self->ifname, errno);
		self->io_watch = 0;
		return G_SOURCE_REMOVE;
	}
}

static gboolean mtu_limit_open_socket(struct ofono_mtu_limit *self)
{
	self->fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (self->fd >= 0) {
		struct sockaddr_nl nl;

		memset(&nl, 0, sizeof(nl));
		nl.nl_pid = getpid();
		nl.nl_family = AF_NETLINK;
		nl.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE |
				RTMGRP_IPV6_IFADDR | RTMGRP_IPV6_ROUTE |
				RTMGRP_LINK;

		if (bind(self->fd, (struct sockaddr*)&nl, sizeof(nl)) >= 0) {
			return TRUE;
		}
		close(self->fd);
		self->fd = -1;
	}
	return FALSE;
}

static gboolean mtu_limit_start(struct ofono_mtu_limit *self)
{
	if (self->fd >= 0) {
		return TRUE;
	} else if (mtu_limit_open_socket(self)) {
		self->channel = g_io_channel_unix_new(self->fd);
		if (self->channel) {
			g_io_channel_set_encoding(self->channel, NULL, NULL);
			g_io_channel_set_buffered(self->channel, FALSE);
			self->io_watch = g_io_add_watch(self->channel,
					G_IO_IN | G_IO_NVAL | G_IO_HUP,
					mtu_limit_event, self);
			return TRUE;
		}
		close(self->fd);
		self->fd = -1;
	}
	return FALSE;
}

static void mtu_limit_stop(struct ofono_mtu_limit *self)
{
	if (self->io_watch) {
		g_source_remove(self->io_watch);
		self->io_watch = 0;
	}
	if (self->channel) {
		g_io_channel_shutdown(self->channel, TRUE, NULL);
		g_io_channel_unref(self->channel);
		self->channel = NULL;
	}
	if (self->fd >= 0) {
		close(self->fd);
		self->fd = -1;
	}
}

struct ofono_mtu_limit *ofono_mtu_limit_new(int max_mtu)
{
	struct ofono_mtu_limit *self = g_new0(struct ofono_mtu_limit, 1);

	self->fd = -1;
	self->max_mtu = max_mtu;
	self->bufsize = 4096;
	self->buf = g_malloc(self->bufsize);
	return self;
}

void ofono_mtu_limit_free(struct ofono_mtu_limit *self)
{
	if (self) {
		mtu_limit_stop(self);
		g_free(self->ifname);
		g_free(self->buf);
		g_free(self);
	}
}

void ofono_mtu_limit_set_ifname(struct ofono_mtu_limit *self, const char *name)
{
	if (self && g_strcmp0(self->ifname, name)) {
		g_free(self->ifname);
		if (name) {
			self->ifname = g_strdup(name);
			mtu_limit_apply(self);
			mtu_limit_start(self);
		} else {
			self->ifname = NULL;
			mtu_limit_stop(self);
		}
	}
}
