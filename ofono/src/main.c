/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2015-2021  Jolla Ltd.
 *  Copyright (C) 2019-2020  Open Mobile Platform LLC.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <gdbus.h>

#ifdef HAVE_ELL
#include <ell/ell.h>
#endif

#include "ofono.h"

#define SHUTDOWN_GRACE_SECONDS 10

static GMainLoop *event_loop;

void __ofono_exit(void)
{
	g_main_loop_quit(event_loop);
}

static gboolean quit_eventloop(gpointer user_data)
{
	__ofono_exit();
	return FALSE;
}

static unsigned int __terminated = 0;

static gboolean signal_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		if (__terminated == 0) {
			ofono_info("Terminating");
			g_timeout_add_seconds(SHUTDOWN_GRACE_SECONDS,
						quit_eventloop, NULL);
			__ofono_modem_shutdown();
		}

		__terminated = 1;
		break;
	}

	return TRUE;
}

static guint setup_signalfd(void)
{
	GIOChannel *channel;
	guint source;
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Failed to set signal mask");
		return 0;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		perror("Failed to create signal descriptor");
		return 0;
	}

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				signal_handler, NULL);

	g_io_channel_unref(channel);

	return source;
}

static void system_bus_disconnected(DBusConnection *conn, void *user_data)
{
	ofono_error("System bus has disconnected!");

	g_main_loop_quit(event_loop);
}

static gchar *option_debug = NULL;
static gchar *option_plugin = NULL;
static gchar *option_noplugin = NULL;
static gboolean option_detach = TRUE;
static gboolean option_version = FALSE;
static gboolean option_backtrace = TRUE;

static gboolean parse_debug(const char *key, const char *value,
					gpointer user_data, GError **error)
{
	if (value) {
		if (option_debug) {
			char *prev = option_debug;

			option_debug = g_strconcat(prev, ",", value, NULL);
			g_free(prev);
		} else {
			option_debug = g_strdup(value);
		}
	} else {
		g_free(option_debug);
		option_debug = g_strdup("*");
	}

	return TRUE;
}

static GOptionEntry options[] = {
	{ "debug", 'd', G_OPTION_FLAG_OPTIONAL_ARG,
				G_OPTION_ARG_CALLBACK, parse_debug,
				"Specify debug options to enable", "DEBUG" },
	{ "plugin", 'p', 0, G_OPTION_ARG_STRING, &option_plugin,
				"Specify plugins to load", "NAME,..," },
	{ "noplugin", 'P', 0, G_OPTION_ARG_STRING, &option_noplugin,
				"Specify plugins not to load", "NAME,..." },
	{ "nodetach", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Don't run as daemon in background" },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ "nobacktrace", 0, G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_backtrace,
				"Don't print out backtrace information" },
	{ NULL },
};

#ifdef HAVE_ELL
struct ell_event_source {
	GSource source;
	GPollFD pollfd;
};

static gboolean event_prepare(GSource *source, gint *timeout)
{
	int r = l_main_prepare();
	*timeout = r;

	return FALSE;
}

static gboolean event_check(GSource *source)
{
	l_main_iterate(0);
	return FALSE;
}

static GSourceFuncs event_funcs = {
	.prepare = event_prepare,
	.check = event_check,
};
#endif

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	DBusConnection *conn;
	DBusError error;
	guint signal;
#ifdef HAVE_ELL
	struct ell_event_source *source;
#endif

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
			return 1;
		}

		g_printerr("An unknown error occurred\n");
		return 1;
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		printf("%s\n", VERSION);
		exit(0);
	}

	if (option_detach == TRUE) {
		if (daemon(0, 0)) {
			perror("Can't start daemon");
			return 1;
		}
	}

	event_loop = g_main_loop_new(NULL, FALSE);

#ifdef HAVE_ELL
	l_log_set_stderr();
	l_debug("");
	l_debug_enable("*");
	l_main_init();

	source = (struct ell_event_source *) g_source_new(&event_funcs,
					sizeof(struct ell_event_source));

	source->pollfd.fd = l_main_get_epoll_fd();
	source->pollfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;

	g_source_add_poll((GSource *)source, &source->pollfd);
	g_source_attach((GSource *) source,
					g_main_loop_get_context(event_loop));
#endif


	signal = setup_signalfd();

	__ofono_log_init(argv[0], option_debug, option_detach,
							option_backtrace);

	dbus_error_init(&error);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, &error);
	if (conn == NULL) {
		if (dbus_error_is_set(&error) == TRUE) {
			ofono_error("Unable to hop onto D-Bus: %s",
					error.message);
			dbus_error_free(&error);
		} else {
			ofono_error("Unable to hop onto D-Bus");
		}

		goto cleanup;
	}

	g_dbus_set_disconnect_function(conn, system_bus_disconnected,
					NULL, NULL);

	__ofono_dbus_init(conn);

	__ofono_modemwatch_init();

	__ofono_manager_init();

        __ofono_slot_manager_init();

	__ofono_plugin_init(option_plugin, option_noplugin);

	g_free(option_plugin);
	g_free(option_noplugin);

	if (g_dbus_request_name(conn, OFONO_SERVICE, &error)) {
		g_main_loop_run(event_loop);
	} else {
		ofono_error("Unable to register D-Bus name: %s", error.message);
		dbus_error_free(&error);
	}

	__ofono_plugin_cleanup();

        __ofono_slot_manager_cleanup();

	__ofono_manager_cleanup();

	__ofono_modemwatch_cleanup();

	__ofono_dbus_cleanup();
	dbus_connection_unref(conn);

cleanup:
	g_source_remove(signal);

#ifdef HAVE_ELL
	g_source_destroy((GSource *) source);
	l_main_exit();
#endif
	g_main_loop_unref(event_loop);

	__ofono_log_cleanup(option_backtrace);

	g_free(option_debug);

	return 0;
}
