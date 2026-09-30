/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicitly enabled local test input; never used by normal modem drivers. */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbs-inject.h"

#define MAX_TEST_PDU 65535

struct cbs_inject {
	int fd;
	guint watch;
	char *path;
	cbs_inject_cb_t callback;
	void *data;
	uid_t sender_uid;
};

static gboolean cbs_inject_read(GIOChannel *channel, GIOCondition condition,
						gpointer data)
{
	struct cbs_inject *inject = data;
	struct sockaddr_un sender;
	unsigned char pdu[MAX_TEST_PDU];
	union {
		struct cmsghdr align;
		unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))];
	} control;
	struct iovec iov = { .iov_base = pdu, .iov_len = sizeof(pdu) };
	struct msghdr message = {
		.msg_name = &sender,
		.msg_namelen = sizeof(sender),
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control.bytes,
		.msg_controllen = sizeof(control.bytes)
	};
	struct cmsghdr *cmsg;
	gboolean authorized = FALSE;
	const char *reply;
	ssize_t len;

	if (condition & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		inject->watch = 0;
		return G_SOURCE_REMOVE;
	}

	len = recvmsg(inject->fd, &message, MSG_TRUNC);
	if (len < 0)
		return G_SOURCE_CONTINUE;

	if (!(message.msg_flags & MSG_CTRUNC)) {
		for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
					cmsg = CMSG_NXTHDR(&message, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET &&
					cmsg->cmsg_type == SCM_CREDENTIALS &&
					cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
				struct ucred credentials;

				memcpy(&credentials, CMSG_DATA(cmsg), sizeof(credentials));
				authorized = credentials.uid == inject->sender_uid;
				break;
			}
		}
	}
	if (!authorized) {
		reply = "REJECTED: sender uid";
	} else if (len > 0 && len <= MAX_TEST_PDU) {
		inject->callback(pdu, (int) len, inject->data);
		/* Receipt means dispatched, not decoded or presented successfully. */
		reply = "DISPATCHED";
	} else {
		reply = "REJECTED: empty or oversized PDU";
	}

	if (message.msg_namelen > offsetof(struct sockaddr_un, sun_path))
		sendto(inject->fd, reply, strlen(reply), MSG_NOSIGNAL,
				(struct sockaddr *) &sender, message.msg_namelen);

	return G_SOURCE_CONTINUE;
}

struct cbs_inject *cbs_inject_new(const char *dir, const char *modem,
			uid_t sender_uid, cbs_inject_cb_t callback, void *data)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct stat st;
	struct cbs_inject *inject;
	GIOChannel *channel;
	const char *name;
	char *path;
	int fd;
	int passcred = 1;

	if (!dir || !dir[0])
		return NULL;

	/* A private directory prevents access even during bind/chmod. */
	if (dir[0] != '/' || lstat(dir, &st) || !S_ISDIR(st.st_mode) ||
			st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
		g_warning("CBS test input requires an owned mode-0700 directory");
		return NULL;
	}

	if (!callback || !modem || modem[0] != '/' || !modem[1])
		return NULL;
	for (name = modem + 1; *name; name++) {
		if (!g_ascii_isalnum(*name) && *name != '_' && *name != '-')
			return NULL;
	}

	path = g_strdup_printf("%s/%s.sock", dir, modem + 1);
	if (strlen(path) >= sizeof(address.sun_path)) {
		g_free(path);
		return NULL;
	}
	strcpy(address.sun_path, path);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto fail;
	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) < 0)
		goto fail;
	/* Never unlink an existing endpoint: it may belong to another daemon. */
	if (bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0)
		goto fail;
	if (chmod(path, 0600) < 0) {
		unlink(path);
		goto fail;
	}

	inject = g_new0(struct cbs_inject, 1);
	inject->fd = fd;
	inject->path = path;
	inject->callback = callback;
	inject->data = data;
	inject->sender_uid = sender_uid;
	channel = g_io_channel_unix_new(fd);
	inject->watch = g_io_add_watch(channel,
			G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			cbs_inject_read, inject);
	g_io_channel_unref(channel);
	return inject;

fail:
	g_warning("Cannot bind CBS test input %s: %s", path, g_strerror(errno));
	if (fd >= 0)
		close(fd);
	g_free(path);
	return NULL;
}

void cbs_inject_free(struct cbs_inject *inject)
{
	if (inject) {
		if (inject->watch)
			g_source_remove(inject->watch);
		close(inject->fd);
		unlink(inject->path);
		g_free(inject->path);
		g_free(inject);
	}
}
