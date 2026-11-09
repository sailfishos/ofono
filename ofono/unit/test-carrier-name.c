/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
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

#include "ofono.h"

#include <glib/gstdio.h>

static char *config_dir;

const char *ofono_config_dir(void)
{
	return config_dir;
}

static void test_write(const char *name, const char *contents)
{
	char *path = g_build_filename(config_dir, name, NULL);

	g_assert(g_file_set_contents(path, contents, -1, NULL));
	g_free(path);
}

static void test_remove(const char *name)
{
	char *path = g_build_filename(config_dir, name, NULL);

	g_assert_cmpint(g_remove(path), ==, 0);
	g_free(path);
}

static void test_names(const char *iccid, const char *spn, const char *network)
{
	g_assert_cmpstr(__ofono_carrier_name(iccid, FALSE), ==, spn);
	g_assert_cmpstr(__ofono_carrier_name(iccid, TRUE), ==, network);
}

static void test_setup(void)
{
	config_dir = g_dir_make_tmp("test-carrier-name-XXXXXX", NULL);
	g_assert(config_dir);
}

static void test_cleanup(void)
{
	__ofono_carrier_name_cleanup();
	g_assert_cmpint(g_rmdir(config_dir), ==, 0);
	g_free(config_dir);
	config_dir = NULL;
}

struct test_case {
	const char *name;
	const char *config;
	const char *iccid;
	const char *spn;
	const char *network;
};

static const struct test_case cases[] = {
	{ "missing", NULL, "100", NULL, NULL },
	{ "defaults", "[First]\nIccidRanges=100:199\n",
		"150", "First", "First" },
	{ "separate", "[First]\nIccidRanges=100:199\n"
		"ServiceProviderName=SIM\nNetworkName=Network\n",
		"150", "SIM", "Network" },
	{ "empty-spn", "[First]\nIccidRanges=100:199\n"
		"ServiceProviderName=\n", "150", NULL, "First" },
	{ "empty-network", "[First]\nIccidRanges=100:199\n"
		"NetworkName=\n", "150", "First", NULL },
	{ "invalid-name", "[First]\nIccidRanges=100:199\n"
		"NetworkName=\xff\n", "150", "First", NULL },
	{ "second-carrier", "[First]\nIccidRanges=100:199\n"
		"[Second]\nIccidRanges=200:299\n",
		"250", "Second", "Second" },
	{ "overlap", "[First]\nIccidRanges=100:199\n"
		"[Second]\nIccidRanges=150:250\n",
		"150", "First", "First" },
	{ "missing-ranges", "[First]\nNetworkName=Alias\n",
		"150", NULL, NULL },
	{ "whitespace", "[First]\nIccidRanges= 100 : 199 , 300:399\n",
		"350", "First", "First" },
	{ "invalid-ranges", "[First]\nIccidRanges=199:100,100:1999,"
		"ab:cd,100,1:2:3,:2,1:,\n", "150", NULL, NULL }
};

static void test_config(gconstpointer data)
{
	const struct test_case *test = data;

	test_setup();
	if (test->config)
		test_write("carrier_name.conf", test->config);
	test_names(test->iccid, test->spn, test->network);
	if (test->config)
		test_remove("carrier_name.conf");
	test_cleanup();
}

static void test_boundaries(void)
{
	test_setup();
	test_write("carrier_name.conf", "[Carrier]\n"
		"IccidRanges=8900000000000000000:8900000000000000099,"
		"89440000000000000000:89440000000000000099\n");
	test_names("8900000000000000000", "Carrier", "Carrier");
	test_names("8900000000000000099", "Carrier", "Carrier");
	test_names("8899999999999999999", NULL, NULL);
	test_names("8900000000000000100", NULL, NULL);
	test_names("89440000000000000000", "Carrier", "Carrier");
	test_names("89440000000000000099", "Carrier", "Carrier");
	test_names("89439999999999999999", NULL, NULL);
	test_names("89440000000000000100", NULL, NULL);
	test_names("08900000000000000000", NULL, NULL);
	test_names("890000000000000000", NULL, NULL);
	test_names("89000000000000000a0", NULL, NULL);
	test_names("", NULL, NULL);
	test_names(NULL, NULL, NULL);
	test_remove("carrier_name.conf");
	test_cleanup();
}

static void test_dropins(void)
{
	char *dir;

	test_setup();
	dir = g_build_filename(config_dir, "carrier_name.d", NULL);
	g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
	test_write("carrier_name.d/10-first.conf",
		"[First]\nIccidRanges=100:199\n");
	test_write("carrier_name.d/20-second.conf",
		"[Second]\nIccidRanges=200:299\n"
		"[First]\nIccidRanges+=300:399\nNetworkName=Alias\n");
	test_names("150", "First", "Alias");
	test_names("250", "Second", "Second");
	test_names("350", "First", "Alias");
	test_write("carrier_name.d/30-change.conf",
		"[First]\nIccidRanges=400:499\n[!Second]\n");
	/* Configuration is cached until cleanup. */
	test_names("150", "First", "Alias");
	__ofono_carrier_name_cleanup();
	test_names("150", NULL, NULL);
	test_names("250", NULL, NULL);
	test_names("350", NULL, NULL);
	test_names("450", "First", "Alias");
	test_remove("carrier_name.d/10-first.conf");
	test_remove("carrier_name.d/20-second.conf");
	test_remove("carrier_name.d/30-change.conf");
	g_assert_cmpint(g_rmdir(dir), ==, 0);
	g_free(dir);
	__ofono_carrier_name_cleanup();
	test_names("450", NULL, NULL);
	test_cleanup();
}

int main(int argc, char *argv[])
{
	unsigned int i;

	g_test_init(&argc, &argv, NULL);
	__ofono_log_init("test-carrier-name",
		g_test_verbose() ? "*" : NULL, FALSE, FALSE);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		char *name = g_strconcat("/carrier-name/", cases[i].name, NULL);

		g_test_add_data_func(name, cases + i, test_config);
		g_free(name);
	}
	g_test_add_func("/carrier-name/boundaries", test_boundaries);
	g_test_add_func("/carrier-name/dropins", test_dropins);
	return g_test_run();
}
