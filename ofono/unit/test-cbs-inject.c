/* SPDX-License-Identifier: GPL-2.0-only */
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cbs-inject.h"

static guint deliveries;
static GBytes *last_pdu;

static void received(const unsigned char *pdu, int len, void *data)
{
	g_assert(data == &deliveries);
	deliveries++;
	if (last_pdu)
		g_bytes_unref(last_pdu);
	last_pdu = g_bytes_new(pdu, len);
}

static void dispatch(int fd, const char *path, const void *pdu, gsize len,
						const char *expected)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char reply[100];
	ssize_t n;
	gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

	strcpy(address.sun_path, path);
	g_assert_cmpint(sendto(fd, pdu, len, 0, (struct sockaddr *) &address,
					sizeof(address)), ==, len);
	do {
		g_main_context_iteration(NULL, FALSE);
		n = recv(fd, reply, sizeof(reply) - 1, MSG_DONTWAIT);
	} while (n < 0 && g_get_monotonic_time() < deadline);
	g_assert_cmpint(n, >, 0);
	reply[n] = 0;
	g_assert_cmpstr(reply, ==, expected);
}

static void test_input(void)
{
	char *dir = g_dir_make_tmp("cbs-inject-XXXXXX", NULL);
	char *path = g_strconcat(dir, "/ril_0.sock", NULL);
	struct sockaddr_un client = { .sun_family = AF_UNIX };
	struct cbs_inject *inject;
	struct stat st;
	const unsigned char pdu[] = { 0, 0xff, 0x11, 0, 0x48, 0x11 };
	unsigned char *oversized = g_malloc0(65536);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	gsize len;
	const void *bytes;

	g_assert(dir);
	g_assert_cmpint(fd, >=, 0);
	g_assert(!cbs_inject_new(NULL, "/ril_0", geteuid(), received, &deliveries));
	g_assert(!cbs_inject_new(dir, "/../invalid", geteuid(), received, &deliveries));
	chmod(dir, 0755);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*mode-0700*");
	g_assert(!cbs_inject_new(dir, "/ril_0", geteuid(), received, &deliveries));
	g_test_assert_expected_messages();
	chmod(dir, 0700);
	inject = cbs_inject_new(dir, "/ril_0", geteuid(), received, &deliveries);
	g_assert(inject);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*Cannot bind*");
	g_assert(!cbs_inject_new(dir, "/ril_0", geteuid(), received, &deliveries));
	g_test_assert_expected_messages();

	g_snprintf(client.sun_path, sizeof(client.sun_path), "%s/client", dir);
	g_assert_cmpint(bind(fd, (struct sockaddr *) &client, sizeof(client)), ==, 0);
	dispatch(fd, path, pdu, sizeof(pdu), "DISPATCHED");
	g_assert_cmpuint(deliveries, ==, 1);
	bytes = g_bytes_get_data(last_pdu, &len);
	g_assert_cmpuint(len, ==, sizeof(pdu));
	g_assert(!memcmp(bytes, pdu, len));
	dispatch(fd, path, pdu, 0, "REJECTED: empty or oversized PDU");
	dispatch(fd, path, oversized, 65536,
					"REJECTED: empty or oversized PDU");
	g_assert_cmpuint(deliveries, ==, 1);
	dispatch(fd, path, oversized, 65535, "DISPATCHED");
	g_assert_cmpuint(deliveries, ==, 2);
	cbs_inject_free(inject);
	g_assert(!g_file_test(path, G_FILE_TEST_EXISTS));
	/* Modem removal and re-registration can recreate the endpoint. */
	inject = cbs_inject_new(dir, "/ril_0", geteuid(), received, &deliveries);
	g_assert(inject);
	dispatch(fd, path, pdu, sizeof(pdu), "DISPATCHED");
	g_assert_cmpuint(deliveries, ==, 3);
	cbs_inject_free(inject);
	/* Kernel credentials must be checked independently of socket permissions. */
	inject = cbs_inject_new(dir, "/ril_0", (uid_t) -1, received, &deliveries);
	g_assert(inject);
	dispatch(fd, path, pdu, sizeof(pdu), "REJECTED: sender uid");
	g_assert_cmpuint(deliveries, ==, 3);
	cbs_inject_free(inject);
	close(fd);
	unlink(client.sun_path);
	rmdir(dir);
	g_bytes_unref(last_pdu);
	g_free(oversized);
	g_free(path);
	g_free(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/cbs-inject/transport-and-lifecycle", test_input);
	return g_test_run();
}
