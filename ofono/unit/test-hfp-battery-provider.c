/*
 * Tests of the production HFP battery provider with a controlled D-Bus peer.
 * Copyright (C) 2026 Jolla Mobile Ltd
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <glib.h>
#include <dbus/dbus.h>
#include <ofono/emulator.h>
#include <gdbus.h>
#include <ofono/log.h>

struct pending {
	DBusMessage *request;
	DBusPendingCallNotifyFunction notify;
	void *data;
	gboolean cancelled;
};

struct ofono_emulator {
	ofono_bool_t (*indicator)(struct ofono_emulator *em, int id, int value);
	unsigned int handlers;
	ofono_emulator_request_cb_t callbacks[4];
	void *handler_data[4];
	const char *prefixes[4];
	int result;
};

struct ofono_emulator_request {
	const char *raw;
	enum ofono_emulator_request_type type;
};

struct test_proxy {
	const char *path;
	const char *interface;
};

static GPtrArray *messages;
static GHashTable *interfaces;
static GList *pending_calls;
static guint changes, cancellations, registrations;
static guint serial;
static int dummy_connection, dummy_client;
static const char *fail_interface;
static int fail_handler;

static dbus_bool_t test_send(DBusConnection *conn, DBusMessage *msg,
							dbus_uint32_t *value)
{
	if (!dbus_message_get_serial(msg))
		dbus_message_set_serial(msg, ++serial);
	g_ptr_array_add(messages, dbus_message_ref(msg));
	return TRUE;
}

static dbus_bool_t test_send_with_reply(DBusConnection *conn, DBusMessage *msg,
						DBusPendingCall **call, int timeout)
{
	struct pending *p = g_new0(struct pending, 1);

	test_send(conn, msg, NULL);
	p->request = dbus_message_ref(msg);
	pending_calls = g_list_prepend(pending_calls, p);
	*call = (DBusPendingCall *) p;
	registrations++;
	return TRUE;
}

static dbus_bool_t test_set_notify(DBusPendingCall *call,
		DBusPendingCallNotifyFunction notify, void *data, DBusFreeFunction free)
{
	struct pending *p = (struct pending *) call;

	p->notify = notify;
	p->data = data;
	return TRUE;
}

static DBusMessage *pending_reply;

static DBusMessage *test_steal_reply(DBusPendingCall *call)
{
	DBusMessage *reply = pending_reply;

	pending_reply = NULL;
	return reply;
}

static void test_cancel(DBusPendingCall *call)
{
	struct pending *p = (struct pending *) call;

	p->cancelled = TRUE;
	cancellations++;
}

static void test_unref(DBusPendingCall *call)
{
	struct pending *p = (struct pending *) call;

	pending_calls = g_list_remove(pending_calls, p);
	dbus_message_unref(p->request);
	g_free(p);
}

#define dbus_connection_send test_send
#define dbus_connection_send_with_reply test_send_with_reply
#define dbus_pending_call_set_notify test_set_notify
#define dbus_pending_call_steal_reply test_steal_reply
#define dbus_pending_call_cancel test_cancel
#define dbus_pending_call_unref test_unref
#include "../plugins/hfp_battery.c"

/* Only the transport/emulator boundary is mocked. All publication, parsing,
 * state transitions and message construction above are production code. */
DBusConnection *ofono_dbus_get_connection(void)
{
	return (DBusConnection *) &dummy_connection;
}

void ofono_dbg(const struct ofono_debug_desc *desc, const char *format, ...)
{
}

void __ofono_emulator_register_battery_extension(
		ofono_emulator_battery_attach_cb add,
		ofono_emulator_battery_detach_cb remove)
{
}

void __ofono_emulator_unregister_battery_extension(void)
{
}

void __ofono_emulator_set_hf_battery_indicator(struct ofono_emulator *em,
						ofono_emulator_hf_indicator_cb cb)
{
	em->indicator = cb;
}

ofono_bool_t ofono_emulator_add_handler(struct ofono_emulator *em,
		const char *prefix, ofono_emulator_request_cb_t cb, void *data,
		ofono_destroy_func destroy)
{
	if (fail_handler && em->handlers + 1 == fail_handler)
		return FALSE;
	g_assert_cmpuint(em->handlers, <, G_N_ELEMENTS(em->callbacks));
	em->handlers++;
	em->callbacks[em->handlers - 1] = cb;
	em->handler_data[em->handlers - 1] = data;
	em->prefixes[em->handlers - 1] = prefix;
	return TRUE;
}

ofono_bool_t ofono_emulator_remove_handler(struct ofono_emulator *em,
							const char *prefix)
{
	unsigned int i;

	for (i = 0; i < em->handlers; i++) {
		if (!strcmp(em->prefixes[i], prefix))
			break;
	}
	g_assert_cmpuint(i, <, em->handlers);
	for (; i + 1 < em->handlers; i++) {
		em->callbacks[i] = em->callbacks[i + 1];
		em->handler_data[i] = em->handler_data[i + 1];
		em->prefixes[i] = em->prefixes[i + 1];
	}
	em->handlers--;
	em->callbacks[em->handlers] = NULL;
	em->handler_data[em->handlers] = NULL;
	em->prefixes[em->handlers] = NULL;
	return TRUE;
}

void ofono_emulator_send_info(struct ofono_emulator *em, const char *line,
							ofono_bool_t last)
{
	g_assert_cmpstr(line, ==, "+XAPL=iPhone,2");
}

void ofono_emulator_send_final(struct ofono_emulator *em,
						const struct ofono_error *error)
{
	em->result = error->type;
}

const char *ofono_emulator_request_get_raw(struct ofono_emulator_request *req)
{
	return req->raw;
}

enum ofono_emulator_request_type ofono_emulator_request_get_type(
						struct ofono_emulator_request *req)
{
	return req->type;
}

gboolean g_dbus_register_interface(DBusConnection *conn, const char *path,
		const char *name, const GDBusMethodTable *methods,
		const GDBusSignalTable *signals, const GDBusPropertyTable *properties,
		void *data, GDBusDestroyFunction destroy)
{
	char *key = g_strconcat(path, " ", name, NULL);

	if (g_hash_table_contains(interfaces, key) ||
			!g_strcmp0(fail_interface, name)) {
		g_free(key);
		return FALSE;
	}
	g_hash_table_add(interfaces, key);
	return TRUE;
}

gboolean g_dbus_unregister_interface(DBusConnection *conn, const char *path,
								const char *name)
{
	char *key = g_strconcat(path, " ", name, NULL);
	gboolean found = g_hash_table_remove(interfaces, key);

	g_free(key);
	g_assert_true(found);
	return found;
}

void g_dbus_emit_property_changed(DBusConnection *conn, const char *path,
					const char *interface, const char *name)
{
	g_assert_cmpstr(interface, ==, BATTERY_PROVIDER_INTERFACE);
	g_assert_cmpstr(name, ==, "Percentage");
	changes++;
}

GDBusClient *g_dbus_client_new(DBusConnection *conn, const char *service,
								const char *path)
{
	return (GDBusClient *) &dummy_client;
}

void g_dbus_client_unref(GDBusClient *client)
{
}

gboolean g_dbus_client_set_disconnect_watch(GDBusClient *client,
				GDBusWatchFunction function, void *data)
{
	return TRUE;
}

gboolean g_dbus_client_set_proxy_handlers(GDBusClient *client,
		GDBusProxyFunction added, GDBusProxyFunction removed,
		GDBusPropertyFunction changed, void *data)
{
	return TRUE;
}

const char *g_dbus_proxy_get_path(GDBusProxy *proxy)
{
	return ((struct test_proxy *) proxy)->path;
}

const char *g_dbus_proxy_get_interface(GDBusProxy *proxy)
{
	return ((struct test_proxy *) proxy)->interface;
}

static void setup(void)
{
	messages = g_ptr_array_new_with_free_func((GDestroyNotify) dbus_message_unref);
	interfaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	changes = cancellations = registrations = 0;
	fail_interface = NULL;
	fail_handler = 0;
	g_assert_cmpint(hfp_battery_init(), ==, 0);
}

static void teardown(void)
{
	hfp_battery_exit();
	g_assert_null(sessions);
	g_assert_null(pending_calls);
	g_assert_cmpuint(g_hash_table_size(interfaces), ==, 0);
	g_hash_table_destroy(interfaces);
	g_ptr_array_unref(messages);
}

static void complete(struct battery_provider *p, const char *error)
{
	struct pending *call = (struct pending *) p->pending;

	g_assert_nonnull(call);
	g_assert_false(call->cancelled);
	pending_reply = error ? dbus_message_new_error(call->request, error,
			"test error") : dbus_message_new_method_return(call->request);
	call->notify((DBusPendingCall *) call, call->data);
	g_assert_null(pending_reply);
}

static void request(struct battery_session *s, const char *prefix,
			enum ofono_emulator_request_type type, const char *raw)
{
	struct ofono_emulator_request req = { .raw = raw, .type = type };
	unsigned int i;

	for (i = 0; i < s->em->handlers; i++)
		if (!strcmp(s->em->prefixes[i], prefix))
			break;
	g_assert_cmpuint(i, <, s->em->handlers);
	s->em->result = -1;
	s->em->callbacks[i](s->em, &req, s->em->handler_data[i]);
}

static void apple(struct battery_session *s, const char *raw)
{
	request(s, "+IPHONEACCEV", OFONO_EMULATOR_REQUEST_TYPE_SET, raw);
}

static void xevent(struct battery_session *s, const char *raw)
{
	request(s, "+XEVENT", OFONO_EMULATOR_REQUEST_TYPE_SET, raw);
}

static void check_properties(DBusMessageIter *iter, const char *device,
							int percentage)
{
	DBusMessageIter entries;
	unsigned int count = 0;

	g_assert_cmpint(dbus_message_iter_get_arg_type(iter), ==, DBUS_TYPE_ARRAY);
	dbus_message_iter_recurse(iter, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter item, value;
		const char *key;

		dbus_message_iter_recurse(&entries, &item);
		dbus_message_iter_get_basic(&item, &key);
		dbus_message_iter_next(&item);
		dbus_message_iter_recurse(&item, &value);
		if (!strcmp(key, "Percentage")) {
			unsigned char byte;

			g_assert_cmpint(dbus_message_iter_get_arg_type(&value), ==,
								DBUS_TYPE_BYTE);
			dbus_message_iter_get_basic(&value, &byte);
			g_assert_cmpint(byte, ==, percentage);
		} else {
			const char *text;
			gboolean is_device = !strcmp(key, "Device");

			g_assert_true(is_device || !strcmp(key, "Source"));
			g_assert_cmpint(dbus_message_iter_get_arg_type(&value), ==,
				is_device ? DBUS_TYPE_OBJECT_PATH : DBUS_TYPE_STRING);
			dbus_message_iter_get_basic(&value, &text);
			g_assert_cmpstr(text, ==, is_device ? device : BATTERY_SOURCE);
		}
		count++;
		dbus_message_iter_next(&entries);
	}
	g_assert_cmpuint(count, ==, 3);
}

static void check_interfaces(DBusMessageIter *iter, const char *device,
							int percentage)
{
	DBusMessageIter array, entry;
	const char *name;

	dbus_message_iter_recurse(iter, &array);
	dbus_message_iter_recurse(&array, &entry);
	dbus_message_iter_get_basic(&entry, &name);
	g_assert_cmpstr(name, ==, BATTERY_PROVIDER_INTERFACE);
	dbus_message_iter_next(&entry);
	check_properties(&entry, device, percentage);
	g_assert_false(dbus_message_iter_next(&array));
}

static guint snapshot(struct battery_provider *p, const char *device, int value)
{
	DBusMessage *call = dbus_message_new_method_call("org.ofono", p->path,
					OBJECT_MANAGER_INTERFACE, "GetManagedObjects");
	DBusMessage *reply;
	DBusMessageIter iter, array;
	guint count = 0;

	dbus_message_set_serial(call, ++serial);
	reply = get_managed_objects(connection, call, p);
	g_assert_cmpstr(dbus_message_get_signature(reply), ==, "a{oa{sa{sv}}}");
	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_recurse(&iter, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter item;

		dbus_message_iter_recurse(&array, &item);
		dbus_message_iter_next(&item);
		if (device)
			check_interfaces(&item, device, value);
		count++;
		dbus_message_iter_next(&array);
	}
	dbus_message_unref(reply);
	dbus_message_unref(call);
	return count;
}

#define DEVICE_1 "/org/bluez/hci1/dev_4C_FE_2E_5D_CE_EE"
#define DEVICE_2 "/org/bluez/hci1/dev_11_22_33_44_55_66"
#define DEVICE_3 "/org/bluez/hci2/dev_11_22_33_44_55_66"

static void test_reports(void)
{
	struct ofono_emulator em = { 0 };
	struct battery_session *s;
	DBusMessage *msg;
	DBusMessageIter iter;

	setup();
	manager_added("/org/bluez/hci1");
	attach(&em, DEVICE_1);
	attach(&em, DEVICE_1);
	g_assert_cmpuint(g_list_length(sessions), ==, 1);
	g_assert_cmpuint(em.handlers, ==, 3);
	s = find_session(&em);
	g_assert_cmpuint(snapshot(s->provider, NULL, 0), ==, 0);
	g_assert_cmpuint(registrations, ==, 1);
	apple(s, "1,1,8");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 90), ==, 1);
	msg = g_ptr_array_index(messages, messages->len - 1);
	g_assert_true(dbus_message_is_signal(msg, OBJECT_MANAGER_INTERFACE,
								"InterfacesAdded"));
	g_assert_cmpstr(dbus_message_get_signature(msg), ==, "oa{sa{sv}}");
	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_next(&iter);
	check_interfaces(&iter, DEVICE_1, 90);
	complete(s->provider, NULL);
	g_assert_true(s->provider->registered);
	apple(s, "1,1,7");
	g_assert_cmpint(s->percentage, ==, 80);
	g_assert_cmpuint(changes, ==, 1);
	apple(s, "1,1,7");
	g_assert_cmpuint(changes, ==, 1);
	g_assert_true(indicator_cb(&em, 2, 73));
	apple(s, "1,1,8");
	g_assert_cmpint(s->percentage, ==, 73);
	apple(s, "1,1,8x");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_FAILURE);
	g_assert_cmpint(s->percentage, ==, 73);
	detach(&em);
	g_assert_null(em.indicator);
	g_assert_cmpuint(em.handlers, ==, 0);
	attach(&em, DEVICE_1);
	s = find_session(&em);
	g_assert_false(s->have_value);
	apple(s, "1,1,7");
	g_assert_cmpint(s->percentage, ==, 80);
	teardown();
}

static void test_vendor_events(void)
{
	const enum ofono_emulator_request_type invalid_types[] = {
		OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY,
		OFONO_EMULATOR_REQUEST_TYPE_QUERY,
		OFONO_EMULATOR_REQUEST_TYPE_SUPPORT
	};
	struct ofono_emulator em = { 0 };
	struct battery_session *s;
	unsigned int i, count;

	setup();
	manager_added("/org/bluez/hci1");
	attach(&em, DEVICE_1);
	s = find_session(&em);
	complete(s->provider, NULL);
	count = messages->len;
	request(s, "+XAPL", OFONO_EMULATOR_REQUEST_TYPE_SET,
							"009E-400C-0303,3");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	xevent(s, "BoseSoundLink,158");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_false(s->have_value);
	g_assert_cmpuint(snapshot(s->provider, NULL, 0), ==, 0);
	g_assert_cmpuint(messages->len, ==, count);
	apple(s, "1,1,8");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 90), ==, 1);
	count = messages->len;
	xevent(s, "Headset 2.0 / (test),42");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	xevent(s, "USER-AGENT,Plantronics,model/version");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 90), ==, 1);
	g_assert_cmpuint(messages->len, ==, count);
	xevent(s, "BATTERY,1,3,4,0");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 50), ==, 1);
	xevent(s, "BATTERY,0");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 10), ==, 1);
	count = messages->len;
	xevent(s, "Headset,42,1");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_FAILURE);
	xevent(s, "BATTERY,10");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_FAILURE);
	xevent(s, "BATTERY,9,9");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_FAILURE);
	for (i = 0; i < G_N_ELEMENTS(invalid_types); i++) {
		request(s, "+XEVENT", invalid_types[i], "BATTERY,9");
		g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_FAILURE);
	}
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 10), ==, 1);
	g_assert_cmpuint(messages->len, ==, count);
	g_assert_true(em.indicator(&em, 2, 73));
	count = messages->len;
	xevent(s, "BATTERY,9");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	xevent(s, "BATTERY,0,2,0,0");
	g_assert_cmpint(em.result, ==, OFONO_ERROR_TYPE_NO_ERROR);
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 73), ==, 1);
	g_assert_cmpuint(messages->len, ==, count);
	detach(&em);
	g_assert_cmpuint(em.handlers, ==, 0);
	g_assert_null(em.indicator);
	attach(&em, DEVICE_1);
	s = find_session(&em);
	g_assert_cmpuint(snapshot(s->provider, NULL, 0), ==, 0);
	xevent(s, "USER-AGENT,Plantronics");
	g_assert_false(s->have_value);
	xevent(s, "BATTERY,9");
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 100), ==, 1);
	teardown();
	g_assert_cmpuint(em.handlers, ==, 0);
}

static void test_shared_adapter(void)
{
	struct ofono_emulator a = { 0 }, b = { 0 }, c = { 0 };
	struct battery_session *s;
	struct battery_provider *p;

	setup();
	manager_added("/org/bluez/hci1");
	manager_added("/org/bluez/hci2");
	attach(&a, DEVICE_1);
	attach(&b, DEVICE_2);
	attach(&c, DEVICE_3);
	g_assert_cmpuint(registrations, ==, 2);
	p = find_session(&a)->provider;
	g_assert_true(p == find_session(&b)->provider);
	g_assert_true(p != find_session(&c)->provider);
	apple(find_session(&a), "1,1,8");
	apple(find_session(&b), "1,1,7");
	g_assert_cmpuint(snapshot(p, NULL, 0), ==, 2);
	detach(&a);
	s = find_session(&b);
	g_assert_cmpuint(snapshot(p, DEVICE_2, 80), ==, 1);
	complete(p, NULL);
	apple(s, "1,1,6");
	g_assert_cmpuint(snapshot(p, DEVICE_2, 70), ==, 1);
	teardown();
}

static void test_bluez_restart(void)
{
	struct ofono_emulator em = { 0 };
	struct battery_session *s;
	struct test_proxy dev = { DEVICE_1, DEVICE_INTERFACE };

	setup();
	attach(&em, DEVICE_1);
	s = find_session(&em);
	apple(s, "1,1,8");
	g_assert_cmpuint(registrations, ==, 0);
	manager_added("/org/bluez/hci1");
	g_assert_cmpuint(registrations, ==, 1);
	complete(s->provider, DBUS_ERROR_UNKNOWN_METHOD);
	apple(s, "1,1,7");
	g_assert_cmpuint(registrations, ==, 1);
	bluez_disconnected(connection, NULL);
	g_assert_false(s->have_value);
	g_assert_cmpuint(snapshot(s->provider, NULL, 0), ==, 0);
	manager_added("/org/bluez/hci1");
	g_assert_cmpuint(registrations, ==, 2);
	/* Losing BlueZ before its reply must cancel the outstanding callback. */
	bluez_disconnected(connection, NULL);
	g_assert_cmpuint(cancellations, ==, 1);
	g_assert_null(pending_calls);
	manager_added("/org/bluez/hci1");
	proxy_added((GDBusProxy *) &dev, NULL);
	complete(s->provider, NULL);
	g_assert_false(s->have_value);
	apple(s, "1,1,6");
	g_assert_cmpuint(snapshot(s->provider, DEVICE_1, 70), ==, 1);
	proxy_removed((GDBusProxy *) &dev, NULL);
	g_assert_false(s->have_value);
	apple(s, "1,1,8");
	g_assert_false(s->have_value);
	teardown();
}

static void test_failures(void)
{
	struct ofono_emulator em = { 0 };
	unsigned int failure;

	setup();
	fail_interface = OBJECT_MANAGER_INTERFACE;
	attach(&em, DEVICE_1);
	g_assert_null(sessions);
	fail_interface = NULL;
	for (failure = 1; failure <= 3; failure++) {
		fail_handler = failure;
		attach(&em, DEVICE_1);
		g_assert_null(sessions);
		g_assert_cmpuint(em.handlers, ==, 0);
	}
	fail_handler = 0;
	attach(&em, DEVICE_1);
	fail_interface = BATTERY_PROVIDER_INTERFACE;
	apple(find_session(&em), "1,1,8");
	g_assert_false(find_session(&em)->exported);
	fail_interface = NULL;
	apple(find_session(&em), "1,1,8");
	g_assert_true(find_session(&em)->exported);
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/hfp-battery/provider/reports", test_reports);
	g_test_add_func("/hfp-battery/provider/vendor-events", test_vendor_events);
	g_test_add_func("/hfp-battery/provider/shared-adapter", test_shared_adapter);
	g_test_add_func("/hfp-battery/provider/bluez-restart", test_bluez_restart);
	g_test_add_func("/hfp-battery/provider/failures", test_failures);
	return g_test_run();
}
