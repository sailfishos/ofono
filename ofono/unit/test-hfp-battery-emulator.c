/*
 * Production HF indicator callbacks and shared-emulator teardown tests.
 * Copyright (C) 2026 Jolla Mobile Ltd
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "../src/emulator.c"

static GPtrArray *info;
static GAtServerResult final_result;
static unsigned int reports, detachments, server_releases;
static int percentage;
static struct ofono_emulator *test_em;

void ofono_dbg(const struct ofono_debug_desc *desc, const char *format, ...) {}
void ofono_info(const char *format, ...) {}

void g_at_server_send_final(GAtServer *server, GAtServerResult result)
{
	final_result = result;
}

void g_at_server_send_info(GAtServer *server, const char *line, gboolean last)
{
	g_ptr_array_add(info, g_strdup(line));
}

int ofono_handsfree_card_register(struct ofono_handsfree_card *card)
{
	return 0;
}

void ofono_handsfree_card_remove(struct ofono_handsfree_card *card) {}
void g_at_ppp_unref(GAtPPP *ppp) {}
void __ofono_private_network_release(int id) {}

void *__ofono_atom_get_data(struct ofono_atom *atom)
{
	return test_em;
}

void g_at_server_unref(GAtServer *server)
{
	g_assert_cmpuint(detachments, ==, 1);
	server_releases++;
}

static ofono_bool_t battery_cb(struct ofono_emulator *em, int id, int value)
{
	g_assert_cmpint(id, ==, 2);
	reports++;
	percentage = value;
	return TRUE;
}

static void request(struct ofono_emulator *em, gboolean bind,
			GAtServerRequestType type, const char *args, gboolean ok)
{
	GAtResult result = { 0 };

	result.lines = g_slist_append(NULL, (void *) args);
	g_ptr_array_set_size(info, 0);
	final_result = G_AT_SERVER_RESULT_ERROR;
	if (bind)
		bind_cb(em->server, type, &result, em);
	else
		biev_cb(em->server, type, &result, em);
	g_assert_cmpint(final_result, ==, ok ? G_AT_SERVER_RESULT_OK :
						G_AT_SERVER_RESULT_ERROR);
	g_slist_free(result.lines);
}

static void test_indicators(void)
{
	struct ofono_emulator *em = ofono_emulator_create(OFONO_EMULATOR_TYPE_HFP);
	const char *invalid[] = { "2,-1", "2,+1", "2,101", "2,1,", "2,1x",
		"2,", "2", "2,429496729600", "4294967298,10" };
	const char *invalid_bind[] = { "2,", "2x", "65536", "4294967298", "" };
	unsigned int i, before;

	info = g_ptr_array_new_with_free_func(g_free);
	reports = 0;
	request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_SUPPORT, "", TRUE);
	g_assert_cmpstr(g_ptr_array_index(info, 0), ==, "+BIND: (1)");
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,50", FALSE);
	__ofono_emulator_set_hf_battery_indicator(em, battery_cb);
	request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_SUPPORT, "", TRUE);
	g_assert_cmpstr(g_ptr_array_index(info, 0), ==, "+BIND: (1,2)");
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,50", FALSE);
	request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_SET, "1,2", TRUE);
	g_assert_true(em->battery_negotiated);
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,50", FALSE);
	request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_QUERY, "", TRUE);
	g_assert_true(em->slc);
	g_assert_cmpuint(info->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(info, 1), ==, "+BIND: 2,1");
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,0", TRUE);
	g_assert_cmpint(percentage, ==, 0);
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,100", TRUE);
	g_assert_cmpint(percentage, ==, 100);
	before = reports;
	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
		request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, invalid[i], FALSE);
	g_assert_cmpuint(reports, ==, before);
	for (i = 0; i < G_N_ELEMENTS(invalid_bind); i++) {
		request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_SET, invalid_bind[i], FALSE);
		g_assert_true(em->battery_negotiated);
	}
	request(em, TRUE, G_AT_SERVER_REQUEST_TYPE_SET, "1", TRUE);
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "2,50", FALSE);
	em->ddr_active = TRUE;
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "1,0", TRUE);
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "1,1", TRUE);
	request(em, FALSE, G_AT_SERVER_REQUEST_TYPE_SET, "1,2", FALSE);
	__ofono_emulator_set_hf_battery_indicator(em, NULL);
	g_assert_false(em->battery_negotiated);
	emulator_free(em);
	g_ptr_array_free(info, TRUE);
}

static void detached(struct ofono_emulator *em)
{
	g_assert_nonnull(em->server);
	g_assert_cmpuint(server_releases, ==, 0);
	detachments++;
	__ofono_emulator_set_hf_battery_indicator(em, NULL);
}

static void test_shared_teardown(void)
{
	int atom1, atom2, server;
	struct ofono_emulator *em = ofono_emulator_create(OFONO_EMULATOR_TYPE_HFP);

	detachments = server_releases = 0;
	test_em = em;
	__ofono_emulator_register_battery_extension(NULL, detached);
	__ofono_emulator_set_device_path(em, "/org/bluez/hci1/dev_00_11_22_33_44_55");
	em->server = (GAtServer *) &server;
	em->emulator_registered = TRUE;
	em->battery_attached = TRUE;
	em->battery_indicator = battery_cb;
	em->registered_atoms = g_list_append(NULL, &atom1);
	em->registered_atoms = g_list_append(em->registered_atoms, &atom2);
	emulator_unregister((struct ofono_atom *) &atom1);
	g_assert_cmpuint(detachments, ==, 0);
	g_assert_nonnull(em->battery_indicator);
	emulator_unregister((struct ofono_atom *) &atom2);
	g_assert_cmpuint(detachments, ==, 1);
	g_assert_cmpuint(server_releases, ==, 1);
	g_assert_null(em->battery_indicator);
	emulator_unregister((struct ofono_atom *) &atom2);
	emulator_free(em);
	g_assert_cmpuint(detachments, ==, 1);
	__ofono_emulator_unregister_battery_extension();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/hfp-battery/emulator/indicators", test_indicators);
	g_test_add_func("/hfp-battery/emulator/shared-teardown", test_shared_teardown);
	return g_test_run();
}
