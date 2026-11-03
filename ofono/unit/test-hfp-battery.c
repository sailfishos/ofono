/*
 * Copyright (C) 2026 Jolla Mobile Ltd
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../gatchat/gatserver.h"

#include "../plugins/hfp-battery-util.h"

static void test_xapl(void)
{
	const char *invalid[] = { "", "ABCD", "ABCD-1234-0100", "ABCD-1234-0100,",
		"ABCD-1234-0100,+2", "ABCD-1234-0100,-2",
		"ABCD-1234-0100,4294967296", "GBCD-1234-0100,2" };
	unsigned int i, features = 0;
	g_assert_true(hfp_battery_parse_xapl("ABCD-1234-0100,10", &features));
	g_assert_cmpuint(features, ==, 10);
	g_assert_false(hfp_battery_parse_xapl("ABCD-1234-0100,10,", &features));
	for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
		features = 77;
		g_assert_false(hfp_battery_parse_xapl(invalid[i], &features));
		g_assert_cmpuint(features, ==, 77);
	}
}

static void test_iphone_boundaries(void)
{
	int percentage = -1;
	g_assert_true(hfp_battery_parse_iphoneaccev("1,1,0", &percentage));
	g_assert_cmpint(percentage, ==, 10);
	g_assert_true(hfp_battery_parse_iphoneaccev("1,1,9", &percentage));
	g_assert_cmpint(percentage, ==, 100);
}

static void test_iphone_jvc_and_unknown(void)
{
	int percentage = -1;
	g_assert_true(hfp_battery_parse_iphoneaccev("2,1,8,42,7", &percentage));
	g_assert_cmpint(percentage, ==, 90);
	g_assert_true(hfp_battery_parse_iphoneaccev("1,7,123", &percentage));
	g_assert_cmpint(percentage, ==, -1);
}

static void test_iphone_atomic_invalid(void)
{
	const char *invalid[] = { "", "1", "1,1", "1,1,", "1,1,8,",
		"1,1,-1", "1,1,+1", "2,1,8", "1,1,8,2,1",
		"1,1,4294967296", "4294967296", "2,1,8,42,-1" };
	unsigned int i;
	int percentage = 77;

	for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_assert_false(hfp_battery_parse_iphoneaccev(invalid[i], &percentage));
		g_assert_cmpint(percentage, ==, 77);
	}
	g_assert_false(hfp_battery_parse_iphoneaccev("2,1,8,1,7", &percentage));
	g_assert_cmpint(percentage, ==, 77);
	g_assert_false(hfp_battery_parse_iphoneaccev("1,1,10", &percentage));
	g_assert_cmpint(percentage, ==, 77);
	g_assert_false(hfp_battery_parse_iphoneaccev("1,1,8x", &percentage));
	g_assert_cmpint(percentage, ==, 77);
}

static void test_xevent(void)
{
	const struct {
		const char *args;
		int percentage;
	} valid[] = {
		{ "Bose SoundLink,158", -1 }, { "BoseSoundLink,158", -1 },
		{ "Bose SoundLink,159", -1 }, { "UNKNOWN,1", -1 },
		{ "Headset 2.0 / (test),000158", -1 }, { ",0", -1 },
		{ "Headset,999999999999999999999999", -1 },
		{ "USER-AGENT,Plantronics,model/version", -1 },
		{ "BATTERY,0", 10 }, { "BATTERY,4", 50 }, { "BATTERY,9", 100 },
		{ "BATTERY,0,2,0,0", 0 }, { "BATTERY,1,2,1,1", 100 },
		{ "BATTERY,1,3,4,0", 50 }, { "BATTERY,2,7,0,0", 33 },
		{ "BATTERY,2147483646,2147483647,2147483647,1", 100 }
	};
	const char *invalid[] = {
		"", "Bose SoundLink,158x", "Headset", "Headset,", ",",
		"Headset,-1", "Headset,+1", "Headset, 1", "Headset,1 ",
		"Headset,1,2", "Headset,,1", "Headset,1.0", "Headset,1\n",
		"USER-AGENT,", "USER-AGENT,Plantronics\x01", "USER-AGENT,\xff",
		"BATTERY,", "BATTERY,10", "BATTERY,-1", "BATTERY,+1",
		"BATTERY, 1", "BATTERY,1x", "BATTERY,1,", "BATTERY,9,9",
		"BATTERY,1,2,0", "BATTERY,1,2,0,", "BATTERY,1,2,0,0,",
		"BATTERY,1,0,0,0", "BATTERY,1,1,0,0", "BATTERY,3,3,0,0",
		"BATTERY,1,-2,0,0", "BATTERY,1,2,-1,0", "BATTERY,1,2,0,+1",
		"BATTERY,2,3,0,2", "BATTERY,2,3,0,0x",
		"BATTERY,2,3,0,0,extra", "BATTERY,999999999999999999999999,3,0,0",
		"BATTERY,2147483648,3,0,0", "BATTERY,1,2147483648,0,0",
		"BATTERY,1,2,2147483648,0", "BATTERY,1,2,0,2147483648"
	};
	int percentage = 77;
	unsigned int i;

	g_assert_false(hfp_battery_parse_xevent(NULL, &percentage));
	g_assert_cmpint(percentage, ==, 77);
	g_assert_false(hfp_battery_parse_xevent("BATTERY,9", NULL));

	for (i = 0; i < G_N_ELEMENTS(valid); i++) {
		g_test_message("%s", valid[i].args);
		g_assert_true(hfp_battery_parse_xevent(valid[i].args, &percentage));
		g_assert_cmpint(percentage, ==, valid[i].percentage);
	}
	for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_test_message("%s", invalid[i]);
		percentage = 77;
		g_assert_false(hfp_battery_parse_xevent(invalid[i], &percentage));
		g_assert_cmpint(percentage, ==, 77);
	}
}

struct xevent_wire_test {
	const char *command;
	const char *normalized;
	const char *response;
	int percentage;
};

struct xevent_wire_data {
	const struct xevent_wire_test *test;
	GMainLoop *loop;
	GString *response;
	gboolean called;
};

static void xevent_wire_notify(GAtServer *server, GAtServerRequestType type,
					GAtResult *result, gpointer user_data)
{
	struct xevent_wire_data *data = user_data;
	GAtResultIter iter;
	int percentage = 77;
	gboolean valid;

	g_assert_false(data->called);
	data->called = TRUE;
	g_assert_cmpint(type, ==, G_AT_SERVER_REQUEST_TYPE_SET);
	g_at_result_iter_init(&iter, result);
	g_assert_true(g_at_result_iter_next(&iter, ""));
	g_assert_cmpstr(g_at_result_iter_raw_line(&iter), ==,
						data->test->normalized);
	valid = hfp_battery_parse_xevent(g_at_result_iter_raw_line(&iter),
							&percentage);
	g_assert_cmpint(percentage, ==, data->test->percentage);
	g_at_server_send_final(server, valid ? G_AT_SERVER_RESULT_OK :
						G_AT_SERVER_RESULT_ERROR);
}

static gboolean xevent_wire_response(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct xevent_wire_data *data = user_data;
	char buffer[128];
	gsize bytes = 0;
	GIOStatus status;

	g_assert_cmpuint(cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL), ==, 0);
	status = g_io_channel_read_chars(channel, buffer, sizeof(buffer),
							&bytes, NULL);
	g_assert_true(status == G_IO_STATUS_NORMAL || status == G_IO_STATUS_AGAIN);
	g_string_append_len(data->response, buffer, bytes);
	g_assert_cmpuint(data->response->len, <, 512);
	if (strstr(data->response->str, "\r\nOK\r\n") ||
			strstr(data->response->str, "\r\nERROR\r\n"))
		g_main_loop_quit(data->loop);
	return G_SOURCE_CONTINUE;
}

static gboolean xevent_wire_timeout(gpointer user_data)
{
	struct xevent_wire_data *data = user_data;

	g_error("Timed out waiting for response to %s", data->test->command);
	return G_SOURCE_REMOVE;
}

static void test_xevent_wire(void)
{
	const struct xevent_wire_test tests[] = {
		{ "AT+XEVENT=Bose SoundLink,158\r", "BoseSoundLink,158",
			"\r\nOK\r\n", -1 },
		{ "AT+XEVENT=Headset 2.0 / (test),42\r", "Headset2.0/(test),42",
			"\r\nOK\r\n", -1 },
		{ "AT+XEVENT=\"Headset with spaces\",42\r", "\"Headset with spaces\",42",
			"\r\nOK\r\n", -1 },
		{ "AT+XEVENT=Headset,1,2\r", "Headset,1,2",
			"\r\nERROR\r\n", 77 },
		{ "AT+XEVENT=Headset,42x\r", "Headset,42x",
			"\r\nERROR\r\n", 77 },
		{ "AT+XEVENT=BATTERY,10\r", "BATTERY,10",
			"\r\nERROR\r\n", 77 },
		{ "AT+XEVENT=BATTERY,4\r", "BATTERY,4", "\r\nOK\r\n", 50 }
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS(tests); i++) {
		struct xevent_wire_data data = { .test = tests + i };
		GIOChannel *channel, *peer;
		GAtServer *server;
		guint watch, timeout;
		int sockets[2];

		g_test_message("%s", data.test->command);
		g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
		channel = g_io_channel_unix_new(sockets[0]);
		server = g_at_server_new(channel);
		g_assert_nonnull(server);
		g_io_channel_unref(channel);
		g_assert_true(g_at_server_set_echo(server, FALSE));
		g_assert_true(g_at_server_register(server, "+XEVENT",
						xevent_wire_notify, &data, NULL));
		peer = g_io_channel_unix_new(sockets[1]);
		g_assert_true(g_at_util_setup_io(peer, G_IO_FLAG_NONBLOCK));
		data.loop = g_main_loop_new(NULL, FALSE);
		data.response = g_string_new(NULL);
		watch = g_io_add_watch(peer, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
							xevent_wire_response, &data);
		timeout = g_timeout_add_seconds(5, xevent_wire_timeout, &data);
		g_assert_cmpint(write(sockets[1], data.test->command,
				strlen(data.test->command)), ==,
				strlen(data.test->command));
		g_main_loop_run(data.loop);
		g_assert_true(data.called);
		g_assert_cmpstr(data.response->str, ==, data.test->response);
		g_source_remove(timeout);
		g_source_remove(watch);
		g_at_server_unref(server);
		g_io_channel_unref(peer);
		g_string_free(data.response, TRUE);
		g_main_loop_unref(data.loop);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/hfp-battery/xapl", test_xapl);
	g_test_add_func("/hfp-battery/iphone-boundaries", test_iphone_boundaries);
	g_test_add_func("/hfp-battery/iphone-jvc", test_iphone_jvc_and_unknown);
	g_test_add_func("/hfp-battery/iphone-atomic", test_iphone_atomic_invalid);
	g_test_add_func("/hfp-battery/xevent", test_xevent);
	g_test_add_func("/hfp-battery/xevent-wire", test_xevent_wire);
	return g_test_run();
}
