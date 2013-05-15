/*
 * unshare(1) - command-line interface for unshare(2)
 *
 * Copyright (C) 2009 Mikhail Gusarov <dottedmag@dottedmag.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "nls.h"
#include "c.h"
#include "closestream.h"
#include "namespace.h"
#include "exec_shell.h"

static void usage(int status)
{
	FILE *out = status == EXIT_SUCCESS ? stdout : stderr;

	fputs(USAGE_HEADER, out);
	fprintf(out,
	      _(" %s [options] <program> [args...]\n"),	program_invocation_short_name);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -m, --mount       unshare mounts namespace\n"), out);
	fputs(_(" -u, --uts         unshare UTS namespace (hostname etc)\n"), out);
	fputs(_(" -i, --ipc         unshare System V IPC namespace\n"), out);
	fputs(_(" -n, --net         unshare network namespace\n"), out);
	fputs(_(" -p, --pid         unshare pid namespace\n"), out);
	fputs(_(" -U, --user        unshare user namespace\n"), out);
	fputs(_(" -M, --usermap <a:b:c,...>\n"), out);
	fputs(_("                   specify the user namespace UID map\n"), out);
	fputs(_("                   'a' is the starting UID on the current namespace\n"), out);
	fputs(_("                   'b' is the starting UID on the new namespace\n"), out);
	fputs(_("                   'c' is the length of this block\n"), out);
	fputs(_(" -N, --groupmap <a:b:c,...>\n"), out);
	fputs(_("                   specify the user namespace GID map\n"), out);
	fputs(_("                   'a' is the starting GID on the current namespace\n"), out);
	fputs(_("                   'b' is the starting GID on the new namespace\n"), out);
	fputs(_("                   'c' is the length of this block\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("unshare(1)"));

	exit(status);
}

static int write_idmap(char *type, char *map, long pid)
{
	FILE *file;
	unsigned int a, b, c;
	char path[PATH_MAX], *ptr, *saved = NULL;

	snprintf(path, PATH_MAX, "/proc/%ld/%s_map", pid, type);
	file = fopen(path, "w");
	if (file == NULL)
		return 1;

	for (ptr = strtok_r(map, ",", &saved); ptr;
	     ptr = strtok_r(map, ",", &saved)) {
		if (sscanf(ptr, "%u:%u:%u", &a, &b, &c) != 3)
			return 1;
		if (fprintf(file, "%u %u %u\n", a, b, c) <= 0)
			return 1;
	}
	fclose(file);
	return 0;
}

static void prepare_userns(char *usermap, char *groupmap, int *pipe_fd)
{
	pid_t pid;
	int rc, i;
	char c;

	if (pipe(pipe_fd) == -1)
		err(EXIT_FAILURE, _("pipe creation failed"));

	pid = fork();
	if (pid < 0)
		err(EXIT_FAILURE, _("fork failed"));

	if (!pid) {
		close(pipe_fd[1]);

		/* wait for the parent to write the rules */
		rc = read(pipe_fd[0], &c, 1);
		if (rc < 0)
			err(EXIT_FAILURE, _("pipe read error"));
		else if (rc > 0)
			errx(EXIT_FAILURE, _("aborted"));

		return;
	}

	for (i = 0; i < 3; i++) {
		if (tcsetpgrp(i, pid)) {
			write(pipe_fd[0], "a", 1);
			err(EXIT_FAILURE, _("tcsetpgrp"));
		}
	}

	/* will wait for the child to be ready */
	rc = read(pipe_fd[1], &c, 1);
	if (rc < 0)
		err(EXIT_FAILURE, _("pipe read error"));
	else if (rc > 0)
		errx(EXIT_FAILURE, _("pipe read should be empty"));

	if (usermap && write_idmap("uid", usermap, pid)) {
		write(pipe_fd[0], "a", 1);
		err(EXIT_FAILURE, _("failed to write usermap"));
	}

	if (groupmap && write_idmap("gid", groupmap, pid)) {
		write(pipe_fd[0], "a", 1);
		err(EXIT_FAILURE, _("failed to write groupmap"));
	}

	close(pipe_fd[0]);

	if (wait(&rc) == -1)
		err(EXIT_FAILURE, _("waiting for process to end"));
	exit(rc);
}

int main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 'V'},
		{ "mount", no_argument, 0, 'm' },
		{ "uts", no_argument, 0, 'u' },
		{ "ipc", no_argument, 0, 'i' },
		{ "net", no_argument, 0, 'n' },
		{ "pid", no_argument, 0, 'p' },
		{ "user", no_argument, 0, 'U' },
		{ "usermap", 1, 0, 'M' },
		{ "groupmap", 1, 0, 'M' },
		{ NULL, 0, 0, 0 }
	};

	int unshare_flags = 0;

	int c;

	int pipe_fd[2];

	char *usermap = NULL;

	char *groupmap = NULL;

	setlocale(LC_MESSAGES, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long(argc, argv, "hVmuinpUM:N:", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage(EXIT_SUCCESS);
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'm':
			unshare_flags |= CLONE_NEWNS;
			break;
		case 'u':
			unshare_flags |= CLONE_NEWUTS;
			break;
		case 'i':
			unshare_flags |= CLONE_NEWIPC;
			break;
		case 'n':
			unshare_flags |= CLONE_NEWNET;
			break;
		case 'p':
			unshare_flags |= CLONE_NEWPID;
			break;
		case 'U':
			unshare_flags |= CLONE_NEWUSER;
			break;
		case 'M':
			usermap = strdup(optarg);
			break;
		case 'N':
			groupmap = strdup(optarg);
			break;
		default:
			usage(EXIT_FAILURE);
		}
	}

	if (-1 == unshare(unshare_flags))
		err(EXIT_FAILURE, _("unshare failed"));

	if ((unshare_flags & CLONE_NEWUSER) && (usermap || groupmap))
		prepare_userns(usermap, groupmap, pipe_fd);

	if (optind < argc) {
		execvp(argv[optind], argv + optind);
		err(EXIT_FAILURE, _("failed to execute %s"), argv[optind]);
	}
	exec_shell();
}
