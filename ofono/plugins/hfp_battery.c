/*
 * oFono HFP headset battery provider
 *
 * Copyright (C) 2026 Jolla Mobile Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <string.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <ofono.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/dbus.h>
#include <ofono/emulator.h>
#include <ofono/log.h>

#include "hfp-battery-util.h"

#define BATTERY_PROVIDER_INTERFACE "org.bluez.BatteryProvider1"
#define BATTERY_MANAGER_INTERFACE "org.bluez.BatteryProviderManager1"
#define DEVICE_INTERFACE "org.bluez.Device1"
#define OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"
#define BLUEZ_SERVICE "org.bluez"
#define PROVIDER_ROOT "/org/ofono/hfp_battery"
#define BATTERY_SOURCE "oFono HFP"

struct battery_provider {
	char *adapter;
	char *path;
	DBusPendingCall *pending;
	gboolean available;
	gboolean attempted;
	gboolean registered;
};

struct battery_session {
	struct ofono_emulator *em;
	struct battery_provider *provider;
	char *device;
	char *path;
	unsigned char percentage;
	gboolean have_value;
	gboolean standard;
	gboolean exported;
	gboolean present;
};

static GList *sessions;
static GHashTable *providers;
static GHashTable *managers;
static DBusConnection *connection;
static GDBusClient *bluez;

static struct battery_session *find_session(struct ofono_emulator *em)
{
	GList *l;

	for (l = sessions; l; l = l->next) {
		struct battery_session *s = l->data;

		if (s->em == em)
			return s;
	}
	return NULL;
}

static void append_property(DBusMessageIter *dict, const char *name,
				int type, const char *signature, const void *value)
{
	DBusMessageIter entry, variant;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature,
								&variant);
	dbus_message_iter_append_basic(&variant, type, value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_properties(DBusMessageIter *dict, struct battery_session *s)
{
	const char *source = BATTERY_SOURCE;

	append_property(dict, "Device", DBUS_TYPE_OBJECT_PATH, "o", &s->device);
	append_property(dict, "Percentage", DBUS_TYPE_BYTE, "y", &s->percentage);
	append_property(dict, "Source", DBUS_TYPE_STRING, "s", &source);
}

static void append_interfaces(DBusMessageIter *iter, struct battery_session *s)
{
	DBusMessageIter interfaces, entry, dict;
	const char *name = BATTERY_PROVIDER_INTERFACE;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sa{sv}}",
								&interfaces);
	dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, NULL,
								&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &dict);
	append_properties(&dict, s);
	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(&interfaces, &entry);
	dbus_message_iter_close_container(iter, &interfaces);
}

static gboolean battery_get(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct battery_session *s = data;
	const char *source = BATTERY_SOURCE;

	if (!strcmp(property->name, "Device"))
		return dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH,
								&s->device);
	if (!strcmp(property->name, "Percentage"))
		return dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE,
								&s->percentage);
	if (!strcmp(property->name, "Source"))
		return dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &source);
	return FALSE;
}

static const GDBusPropertyTable battery_properties[] = {
	{ "Device", "o", battery_get },
	{ "Percentage", "y", battery_get },
	{ "Source", "s", battery_get },
	{ }
};

static void emit_added(struct battery_session *s)
{
	DBusMessage *msg = dbus_message_new_signal(s->provider->path,
				OBJECT_MANAGER_INTERFACE, "InterfacesAdded");
	DBusMessageIter iter;

	if (!msg)
		return;
	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &s->path);
	append_interfaces(&iter, s);
	dbus_connection_send(connection, msg, NULL);
	dbus_message_unref(msg);
}

static void unpublish(struct battery_session *s)
{
	if (s->exported) {
		const char *name = BATTERY_PROVIDER_INTERFACE;
		DBusMessage *msg = dbus_message_new_signal(s->provider->path,
				OBJECT_MANAGER_INTERFACE, "InterfacesRemoved");
		DBusMessageIter iter, interfaces;

		s->exported = FALSE;
		if (msg) {
			dbus_message_iter_init_append(msg, &iter);
			dbus_message_iter_append_basic(&iter,
						DBUS_TYPE_OBJECT_PATH, &s->path);
			dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
								"s", &interfaces);
			dbus_message_iter_append_basic(&interfaces,
						DBUS_TYPE_STRING, &name);
			dbus_message_iter_close_container(&iter, &interfaces);
			dbus_connection_send(connection, msg, NULL);
			dbus_message_unref(msg);
		}
		g_dbus_unregister_interface(connection, s->path,
						BATTERY_PROVIDER_INTERFACE);
	}
}

static void reset_session(struct battery_session *s)
{
	unpublish(s);
	s->have_value = FALSE;
	s->standard = FALSE;
	s->present = FALSE;
}

static gboolean publish(struct battery_session *s)
{
	if (!s->exported && s->have_value && s->present) {
		if (!g_dbus_register_interface(connection, s->path,
				BATTERY_PROVIDER_INTERFACE, NULL, NULL,
				battery_properties, s, NULL))
			return FALSE;
		s->exported = TRUE;
		emit_added(s);
	}
	return s->exported;
}

static void report(struct battery_session *s, unsigned char percentage,
							gboolean standard)
{
	gboolean changed;
	gboolean exported;

	if (!s->present || (s->standard && !standard))
		return;
	changed = !s->have_value || s->percentage != percentage;
	exported = s->exported;
	s->percentage = percentage;
	s->have_value = TRUE;
	s->standard = standard;
	if (publish(s) && exported && changed)
		g_dbus_emit_property_changed(connection, s->path,
					BATTERY_PROVIDER_INTERFACE, "Percentage");
}

static DBusMessage *get_managed_objects(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct battery_provider *provider = data;
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter, array;
	GList *l;

	if (!reply)
		return NULL;
	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
								&array);
	for (l = sessions; l; l = l->next) {
		struct battery_session *s = l->data;
		DBusMessageIter entry;

		if (s->provider != provider || !s->exported)
			continue;
		dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
								NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
								&s->path);
		append_interfaces(&entry, s);
		dbus_message_iter_close_container(&array, &entry);
	}
	dbus_message_iter_close_container(&iter, &array);
	return reply;
}

static const GDBusMethodTable object_manager_methods[] = {
	{ GDBUS_METHOD("GetManagedObjects", NULL,
			GDBUS_ARGS({ "objects", "a{oa{sa{sv}}}" }),
			get_managed_objects) },
	{ }
};

static const GDBusSignalTable object_manager_signals[] = {
	{ GDBUS_SIGNAL("InterfacesAdded", GDBUS_ARGS({ "object", "o" },
						{ "interfaces", "a{sa{sv}}" })) },
	{ GDBUS_SIGNAL("InterfacesRemoved", GDBUS_ARGS({ "object", "o" },
						{ "interfaces", "as" })) },
	{ }
};

static void cancel_registration(struct battery_provider *p)
{
	if (p->pending) {
		dbus_pending_call_cancel(p->pending);
		dbus_pending_call_unref(p->pending);
		p->pending = NULL;
	}
	p->registered = FALSE;
	p->attempted = FALSE;
}

static void registration_reply(DBusPendingCall *call, void *data)
{
	struct battery_provider *p = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);

	p->pending = NULL;
	p->registered = reply &&
		dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
	if (!p->registered)
		DBG("Battery provider %s not registered: %s", p->adapter,
			reply ? dbus_message_get_error_name(reply) : "no reply");
	if (reply)
		dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static void register_provider(struct battery_provider *p)
{
	DBusMessage *msg;

	if (!p->available || p->attempted)
		return;
	p->attempted = TRUE;
	msg = dbus_message_new_method_call(BLUEZ_SERVICE, p->adapter,
				BATTERY_MANAGER_INTERFACE, "RegisterBatteryProvider");
	if (!msg)
		return;
	dbus_message_set_auto_start(msg, FALSE);
	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &p->path,
							DBUS_TYPE_INVALID);
	if (dbus_connection_send_with_reply(connection, msg, &p->pending, -1) &&
			p->pending)
		dbus_pending_call_set_notify(p->pending, registration_reply, p, NULL);
	dbus_message_unref(msg);
}

static void provider_free(gpointer data)
{
	struct battery_provider *p = data;

	/* Also undo registration if its reply has not arrived yet. The two
	 * method calls are ordered on our single D-Bus connection. */
	if (p->available && (p->pending || p->registered)) {
		DBusMessage *msg = dbus_message_new_method_call(BLUEZ_SERVICE,
				p->adapter, BATTERY_MANAGER_INTERFACE,
				"UnregisterBatteryProvider");

		if (msg) {
			dbus_message_set_auto_start(msg, FALSE);
			dbus_message_set_no_reply(msg, TRUE);
			dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH,
						&p->path, DBUS_TYPE_INVALID);
			dbus_connection_send(connection, msg, NULL);
			dbus_message_unref(msg);
		}
	}
	cancel_registration(p);
	g_dbus_unregister_interface(connection, p->path, OBJECT_MANAGER_INTERFACE);
	g_free(p->adapter);
	g_free(p->path);
	g_free(p);
}

static struct battery_provider *get_provider(const char *device)
{
	const char *slash = strrchr(device, '/');
	struct battery_provider *p;
	char *adapter;

	if (!g_str_has_prefix(device, "/org/bluez/") || !slash ||
			!g_str_has_prefix(slash, "/dev_"))
		return NULL;
	adapter = g_strndup(device, slash - device);
	p = g_hash_table_lookup(providers, adapter);
	if (!p) {
		p = g_new0(struct battery_provider, 1);
		p->adapter = g_strdup(adapter);
		p->path = g_strconcat(PROVIDER_ROOT,
					adapter + strlen("/org/bluez"), NULL);
		if (!g_dbus_register_interface(connection, p->path,
				OBJECT_MANAGER_INTERFACE, object_manager_methods,
				object_manager_signals, NULL, p, NULL)) {
			g_free(p->adapter);
			g_free(p->path);
			g_free(p);
			g_free(adapter);
			return NULL;
		}
		p->available = g_hash_table_contains(managers, adapter);
		g_hash_table_insert(providers, g_strdup(adapter), p);
	}
	g_free(adapter);
	register_provider(p);
	return p;
}

static void xapl_cb(struct ofono_emulator *em,
				struct ofono_emulator_request *req, void *data)
{
	unsigned int features;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };

	if (ofono_emulator_request_get_type(req) ==
					OFONO_EMULATOR_REQUEST_TYPE_SET &&
			hfp_battery_parse_xapl(ofono_emulator_request_get_raw(req),
								&features)) {
		/* Some silly devices only report battery if we call ourselves
		 * 'iPhone' because they don't understand what 'example' means.
		 */
		if (features & 2)
			ofono_emulator_send_info(em, "+XAPL=iPhone,2", TRUE);
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
	}
	ofono_emulator_send_final(em, &error);
}

static void iphoneaccev_cb(struct ofono_emulator *em,
				struct ofono_emulator_request *req, void *data)
{
	struct battery_session *s = data;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	int percentage;

	if (ofono_emulator_request_get_type(req) ==
					OFONO_EMULATOR_REQUEST_TYPE_SET &&
			hfp_battery_parse_iphoneaccev(
				ofono_emulator_request_get_raw(req), &percentage)) {
		if (percentage >= 0)
			report(s, percentage, FALSE);
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
	}
	ofono_emulator_send_final(em, &error);
}

static void xevent_cb(struct ofono_emulator *em,
				struct ofono_emulator_request *req, void *data)
{
	struct battery_session *s = data;
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	int percentage;

	if (ofono_emulator_request_get_type(req) ==
				OFONO_EMULATOR_REQUEST_TYPE_SET &&
			hfp_battery_parse_xevent(ofono_emulator_request_get_raw(req),
						&percentage)) {
		if (percentage >= 0)
			report(s, percentage, FALSE);
		error.type = OFONO_ERROR_TYPE_NO_ERROR;
	}
	ofono_emulator_send_final(em, &error);
}

static ofono_bool_t indicator_cb(struct ofono_emulator *em, int indicator,
								int value)
{
	struct battery_session *s = find_session(em);

	if (!s || indicator != 2 || value < 0 || value > 100)
		return FALSE;
	report(s, value, TRUE);
	return TRUE;
}

static void attach(struct ofono_emulator *em, const char *device)
{
	struct battery_session *s;
	struct battery_provider *p;

	if (find_session(em))
		return;
	p = get_provider(device);
	if (!p)
		return;
	s = g_new0(struct battery_session, 1);
	s->em = em;
	s->provider = p;
	s->device = g_strdup(device);
	s->path = g_strconcat(p->path, strrchr(device, '/'), NULL);
	s->present = TRUE;
	if (!ofono_emulator_add_handler(em, "+XAPL", xapl_cb, s, NULL))
		goto fail;
	if (!ofono_emulator_add_handler(em, "+IPHONEACCEV", iphoneaccev_cb,
								s, NULL)) {
		ofono_emulator_remove_handler(em, "+XAPL");
		goto fail;
	}
	if (!ofono_emulator_add_handler(em, "+XEVENT", xevent_cb, s, NULL)) {
		ofono_emulator_remove_handler(em, "+IPHONEACCEV");
		ofono_emulator_remove_handler(em, "+XAPL");
		goto fail;
	}
	__ofono_emulator_set_hf_battery_indicator(em, indicator_cb);
	sessions = g_list_prepend(sessions, s);
	return;
fail:
	g_free(s->path);
	g_free(s->device);
	g_free(s);
}

static void detach(struct ofono_emulator *em)
{
	struct battery_session *s = find_session(em);

	if (!s)
		return;
	__ofono_emulator_set_hf_battery_indicator(em, NULL);
	ofono_emulator_remove_handler(em, "+XAPL");
	ofono_emulator_remove_handler(em, "+IPHONEACCEV");
	ofono_emulator_remove_handler(em, "+XEVENT");
	unpublish(s);
	sessions = g_list_remove(sessions, s);
	g_free(s->path);
	g_free(s->device);
	g_free(s);
}

static void manager_added(const char *path)
{
	struct battery_provider *p = g_hash_table_lookup(providers, path);

	g_hash_table_add(managers, g_strdup(path));
	if (p && !p->available) {
		p->available = TRUE;
		register_provider(p);
	}
}

static void manager_removed(const char *path)
{
	struct battery_provider *p = g_hash_table_lookup(providers, path);
	GList *l;

	g_hash_table_remove(managers, path);
	if (!p)
		return;
	p->available = FALSE;
	cancel_registration(p);
	for (l = sessions; l; l = l->next) {
		struct battery_session *s = l->data;

		if (s->provider == p)
			reset_session(s);
	}
}

static void proxy_added(GDBusProxy *proxy, void *data)
{
	const char *interface = g_dbus_proxy_get_interface(proxy);
	const char *path = g_dbus_proxy_get_path(proxy);
	GList *l;

	if (!strcmp(interface, BATTERY_MANAGER_INTERFACE))
		manager_added(path);
	else if (!strcmp(interface, DEVICE_INTERFACE)) {
		for (l = sessions; l; l = l->next) {
			struct battery_session *s = l->data;

			if (!strcmp(s->device, path))
				s->present = TRUE;
		}
	}
}

static void proxy_removed(GDBusProxy *proxy, void *data)
{
	const char *interface = g_dbus_proxy_get_interface(proxy);
	const char *path = g_dbus_proxy_get_path(proxy);
	GList *l;

	if (!strcmp(interface, BATTERY_MANAGER_INTERFACE))
		manager_removed(path);
	else if (!strcmp(interface, DEVICE_INTERFACE)) {
		for (l = sessions; l; l = l->next) {
			struct battery_session *s = l->data;

			if (!strcmp(s->device, path))
				reset_session(s);
		}
	}
}

static void bluez_disconnected(DBusConnection *conn, void *data)
{
	GHashTableIter iter;
	gpointer value;
	GList *l;

	g_hash_table_remove_all(managers);
	g_hash_table_iter_init(&iter, providers);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		struct battery_provider *p = value;

		p->available = FALSE;
		cancel_registration(p);
	}
	for (l = sessions; l; l = l->next)
		reset_session(l->data);
}

static int hfp_battery_init(void)
{
	connection = ofono_dbus_get_connection();
	bluez = g_dbus_client_new(connection, BLUEZ_SERVICE, "/org/bluez");
	if (!bluez)
		return -EIO;

	providers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								provider_free);
	managers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_dbus_client_set_disconnect_watch(bluez, bluez_disconnected, NULL);
	g_dbus_client_set_proxy_handlers(bluez, proxy_added, proxy_removed, NULL, NULL);
	__ofono_emulator_register_battery_extension(attach, detach);
	return 0;
}

static void hfp_battery_exit(void)
{
	__ofono_emulator_unregister_battery_extension();
	while (sessions)
		detach(((struct battery_session *) sessions->data)->em);
	g_dbus_client_set_disconnect_watch(bluez, NULL, NULL);
	g_dbus_client_set_proxy_handlers(bluez, NULL, NULL, NULL, NULL);
	g_dbus_client_unref(bluez);
	g_hash_table_destroy(providers);
	g_hash_table_destroy(managers);
	bluez = NULL;
	providers = managers = NULL;
	connection = NULL;
}

OFONO_PLUGIN_DEFINE(hfp_battery, "HFP headset battery provider", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_battery_init, hfp_battery_exit)
