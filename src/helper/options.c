/***************************************************************************
 *   Copyright (C) 2004, 2005 by Dominic Rath                              *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007-2010 ?yvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "configuration.h"
#include "log.h"
#include "command.h"

#include <getopt.h>

#if IS_DARWIN
#include <libproc.h>
#endif

#ifndef _WIN32
#define HAVE_REALPATH
#endif

static int help_flag, version_flag;

static const struct option long_options[] = {
	{"help",		no_argument,			&help_flag,		1},
	{"version",		no_argument,			&version_flag,	1},
	{"debug",		optional_argument,		0,				'd'},
	{"file",		required_argument,		0,				'f'},
	{"search",		required_argument,		0,				's'},
	{"log_output",	required_argument,		0,				'l'},
	{"command",		required_argument,		0,				'c'},
	{"pipe",		no_argument,			0,				'p'},
	{0, 0, 0, 0}
};

int configuration_output_handler(struct command_context *context, const char *line)
{
	LOG_USER_N("%s", line);

	return ERROR_OK;
}

/* Return the canonical path to the directory the openocd executable is in.
 * The path should be absolute, use / as path separator and have all symlinks
 * resolved. The returned string is malloc'd. */
static char *find_exe_path(void)
{
	char *exepath = NULL;

	do {
#if IS_WIN32 && !IS_CYGWIN
		exepath = malloc(MAX_PATH);
		if (exepath == NULL)
			break;
		GetModuleFileName(NULL, exepath, MAX_PATH);

		/* Convert path separators to UNIX style, should work on Windows also. */
		for (char *p = exepath; *p; p++) {
			if (*p == '\\')
				*p = '/';
		}

#elif IS_DARWIN
		exepath = malloc(PROC_PIDPATHINFO_MAXSIZE);
		if (exepath == NULL)
			break;
		if (proc_pidpath(getpid(), exepath, PROC_PIDPATHINFO_MAXSIZE) <= 0) {
			free(exepath);
			exepath = NULL;
		}

#elif defined(CTL_KERN) && defined(KERN_PROC) && defined(KERN_PROC_PATHNAME) /* *BSD */
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
		char *path = malloc(PATH_MAX);
		if (path == NULL)
			break;
		int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
		size_t size = PATH_MAX;

		if (sysctl(mib, (u_int)ARRAY_SIZE(mib), path, &size, NULL, 0) != 0)
			break;

#ifdef HAVE_REALPATH
		exepath = realpath(path, NULL);
		free(path);
#else
		exepath = path;
#endif

#elif defined(HAVE_REALPATH) /* Assume POSIX.1-2008 */
		/* Try Unices in order of likelihood. */
		exepath = realpath("/proc/self/exe", NULL); /* Linux/Cygwin */
		if (exepath == NULL)
			exepath = realpath("/proc/self/path/a.out", NULL); /* Solaris */
		if (exepath == NULL)
			exepath = realpath("/proc/curproc/file", NULL); /* FreeBSD (Should be covered above) */
#endif
	} while (0);

	if (exepath != NULL) {
		/* Strip executable file name, leaving path */
		*strrchr(exepath, '/') = '\0';
	} else {
		LOG_WARNING("Could not determine executable path, using configured BINDIR.");
		LOG_DEBUG("BINDIR = %s", BINDIR);
#ifdef HAVE_REALPATH
		exepath = realpath(BINDIR, NULL);
#else
		exepath = strdup(BINDIR);
#endif
	}

	return exepath;
}

static char *find_suffix(const char *text, const char *suffix)
{
	size_t text_len = strlen(text);
	size_t suffix_len = strlen(suffix);

	if (suffix_len == 0)
		return (char *)text + text_len;

	if (suffix_len > text_len || strncmp(text + text_len - suffix_len, suffix, suffix_len) != 0)
		return NULL; /* Not a suffix of text */

	return (char *)text + text_len - suffix_len;
}

static void add_default_dirs(void)
{
	const char *run_prefix;
	char *path;
	char *exepath = find_exe_path();

	/* Strip executable file name, leaving path */
	*strrchr(exepath, '/') = '\0';

#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	char *end_of_prefix = find_suffix(exepath, "bin");
	if (end_of_prefix != NULL)
		*end_of_prefix = '\0';
#else
	char *end_of_prefix = find_suffix(exepath, BINDIR);
	if (end_of_prefix != NULL)
		*end_of_prefix = '\0';
#endif

	run_prefix = exepath;
	LOG_DEBUG("bindir=%s", BINDIR);
	LOG_DEBUG("pkgdatadir=%s", PKGDATADIR);
	LOG_DEBUG("run_prefix=%s", run_prefix);

	/*
	 * The directory containing OpenOCD-supplied scripts should be
	 * listed last in the built-in search order, so the user can
	 * override these scripts with site-specific customizations.
	 */
	const char *home = getenv("HOME");

	if (home) {
		path = alloc_printf("%s/.openocd", home);
		if (path) {
			add_script_search_dir(path);
			free(path);
		}
	}

	path = getenv("OPENOCD_SCRIPTS");

	if (path)
		add_script_search_dir(path);

#ifdef _WIN32
	const char *appdata = getenv("APPDATA");

	if (appdata) {
		path = alloc_printf("%s/OpenOCD", appdata);
		if (path) {
			add_script_search_dir(path);
			free(path);
		}
	}
#endif

#if defined(_WIN32) && (NUVOTON_CUSTOMIZED)
	path = alloc_printf("%s%s", run_prefix, "/site");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}

	path = alloc_printf("%s%s", run_prefix, "/scripts");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}
#else
	path = alloc_printf("%s%s%s", run_prefix, PKGDATADIR, "/site");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}

	path = alloc_printf("%s%s%s", run_prefix, PKGDATADIR, "/scripts");
	if (path) {
		add_script_search_dir(path);
		free(path);
	}
#endif
	free(exepath);
}

int parse_cmdline_args(struct command_context *cmd_ctx, int argc, char *argv[])
{
	int c;

	while (1) {
		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long(argc, argv, "hvd::l:f:s:c:p", long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c) {
			case 0:
				break;
			case 'h':		/* --help | -h */
				help_flag = 1;
				break;
			case 'v':		/* --version | -v */
				version_flag = 1;
				break;
			case 'f':		/* --file | -f */
			{
				char *command = alloc_printf("script {%s}", optarg);
				add_config_command(command);
				free(command);
				break;
			}
			case 's':		/* --search | -s */
				add_script_search_dir(optarg);
				break;
			case 'd':		/* --debug | -d */
			{
				char *command = alloc_printf("debug_level %s", optarg ? optarg : "3");
				command_run_line(cmd_ctx, command);
				free(command);
				break;
			}
			case 'l':		/* --log_output | -l */
				if (optarg) {
					char *command = alloc_printf("log_output %s", optarg);
					command_run_line(cmd_ctx, command);
					free(command);
				}
				break;
			case 'c':		/* --command | -c */
				if (optarg)
				    add_config_command(optarg);
				break;
			case 'p':
				/* to replicate the old syntax this needs to be synchronous
				 * otherwise the gdb stdin will overflow with the warning message */
				command_run_line(cmd_ctx, "gdb_port pipe; log_output openocd.log");
				LOG_WARNING("deprecated option: -p/--pipe. Use '-c \"gdb_port pipe; "
						"log_output openocd.log\"' instead.");
				break;
		}
	}

	if (help_flag) {
		LOG_OUTPUT("Open On-Chip Debugger\nLicensed under GNU GPL v2\n");
		LOG_OUTPUT("--help       | -h\tdisplay this help\n");
		LOG_OUTPUT("--version    | -v\tdisplay OpenOCD version\n");
		LOG_OUTPUT("--file       | -f\tuse configuration file <name>\n");
		LOG_OUTPUT("--search     | -s\tdir to search for config files and scripts\n");
		LOG_OUTPUT("--debug      | -d\tset debug level <0-4>\n");
		LOG_OUTPUT("--log_output | -l\tredirect log output to file <name>\n");
		LOG_OUTPUT("--command    | -c\trun <command>\n");
		exit(-1);
	}

	if (version_flag) {
		/* Nothing to do, version gets printed automatically. */
		/* It is not an error to request the VERSION number. */
		exit(0);
	}

	/* paths specified on the command line take precedence over these
	 * built-in paths
	 */
	add_default_dirs();

	return ERROR_OK;
}
