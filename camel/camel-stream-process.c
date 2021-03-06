/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; fill-column: 160 -*- */
/* camel-stream-process.c : stream over piped process
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *          Jeffrey Stedfast <fejj@ximian.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib/gi18n-lib.h>

#include "camel-file-utils.h"
#include "camel-stream-process.h"

#define CHECK_CALL(x) G_STMT_START { \
	if ((x) == -1) { \
		g_debug ("%s: Call of '" #x "' failed: %s", G_STRFUNC, g_strerror (errno)); \
	} \
	} G_STMT_END

extern gint camel_verbose_debug;

G_DEFINE_TYPE (CamelStreamProcess, camel_stream_process, CAMEL_TYPE_STREAM)

static void
stream_process_finalize (GObject *object)
{
	/* Ensure we clean up after ourselves -- kill
	 * the child process and reap it. */
	camel_stream_close (CAMEL_STREAM (object), NULL, NULL);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (camel_stream_process_parent_class)->finalize (object);
}

static gssize
stream_process_read (CamelStream *stream,
                     gchar *buffer,
                     gsize n,
                     GCancellable *cancellable,
                     GError **error)
{
	CamelStreamProcess *stream_process = CAMEL_STREAM_PROCESS (stream);
	gint fd = stream_process->sockfd;

	return camel_read (fd, buffer, n, cancellable, error);
}

static gssize
stream_process_write (CamelStream *stream,
                      const gchar *buffer,
                      gsize n,
                      GCancellable *cancellable,
                      GError **error)
{
	CamelStreamProcess *stream_process = CAMEL_STREAM_PROCESS (stream);
	gint fd = stream_process->sockfd;

	return camel_write (fd, buffer, n, cancellable, error);
}

static gint
stream_process_close (CamelStream *object,
                      GCancellable *cancellable,
                      GError **error)
{
	CamelStreamProcess *stream = CAMEL_STREAM_PROCESS (object);

	if (camel_verbose_debug)
		fprintf (
			stderr,
			"Process stream close. sockfd %d, childpid %d\n",
			stream->sockfd, stream->childpid);

	if (stream->sockfd != -1) {
		close (stream->sockfd);
		stream->sockfd = -1;
	}

	if (stream->childpid) {
		gint ret, i;
		for (i = 0; i < 4; i++) {
			ret = waitpid (stream->childpid, NULL, WNOHANG);
			if (camel_verbose_debug)
				fprintf (
					stderr,
					"waitpid() for pid %d returned %d (errno %d)\n",
					stream->childpid, ret, ret == -1 ? errno : 0);
			if (ret == stream->childpid || errno == ECHILD)
				break;
			switch (i) {
			case 0:
				if (camel_verbose_debug)
					fprintf (
						stderr,
						"Sending SIGTERM to pid %d\n",
						stream->childpid);
				kill (stream->childpid, SIGTERM);
				break;
			case 2:
				if (camel_verbose_debug)
					fprintf (
						stderr,
						"Sending SIGKILL to pid %d\n",
						stream->childpid);
				kill (stream->childpid, SIGKILL);
				break;
			case 1:
			case 3:
				sleep (1);
				break;
			}
		}

		stream->childpid = 0;
	}

	return 0;
}

static gint
stream_process_flush (CamelStream *stream,
                      GCancellable *cancellable,
                      GError **error)
{
	return 0;
}

static void
camel_stream_process_class_init (CamelStreamProcessClass *class)
{
	GObjectClass *object_class;
	CamelStreamClass *stream_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->finalize = stream_process_finalize;

	stream_class = CAMEL_STREAM_CLASS (class);
	stream_class->read = stream_process_read;
	stream_class->write = stream_process_write;
	stream_class->close = stream_process_close;
	stream_class->flush = stream_process_flush;
}

static void
camel_stream_process_init (CamelStreamProcess *stream)
{
	stream->sockfd = -1;
	stream->childpid = 0;
}

/**
 * camel_stream_process_new:
 *
 * Returns a PROCESS stream.
 *
 * Returns: the stream
 **/
CamelStream *
camel_stream_process_new (void)
{
	return g_object_new (CAMEL_TYPE_STREAM_PROCESS, NULL);
}

G_GNUC_NORETURN static void
do_exec_command (gint fd,
                 const gchar *command,
                 gchar **env)
{
	gint i, maxopen;

	/* Not a lot we can do if there's an error other than bail. */
	if (dup2 (fd, 0) == -1)
		exit (1);
	if (dup2 (fd, 1) == -1)
		exit (1);

	/* What to do with stderr? Possibly put it through a separate pipe
	 * and bring up a dialog box with its output if anything does get
	 * spewed to it? It'd help the user understand what was going wrong
	 * with their command, but it's hard to do cleanly. For now we just
	 * leave it as it is. Perhaps we should close it and reopen /dev/null? */

	maxopen = sysconf (_SC_OPEN_MAX);
	for (i = 3; i < maxopen; i++) {
		CHECK_CALL (fcntl (i, F_SETFD, FD_CLOEXEC));
	}

	setsid ();
#ifdef TIOCNOTTY
	/* Detach from the controlling tty if we have one. Otherwise,
	 * SSH might do something stupid like trying to use it instead
	 * of running $SSH_ASKPASS. Doh. */
	if ((fd = open ("/dev/tty", O_RDONLY)) != -1) {
		ioctl (fd, TIOCNOTTY, NULL);
		close (fd);
	}
#endif /* TIOCNOTTY */

	/* Set up child's environment. We _add_ to it, don't use execle,
	 * because otherwise we'd destroy stuff like SSH_AUTH_SOCK etc. */
	for (; env && *env; env++)
		putenv (*env);

	execl ("/bin/sh", "/bin/sh", "-c", command, NULL);

	if (camel_verbose_debug)
		fprintf (stderr, "exec failed %d\n", errno);

	exit (1);
}

gint
camel_stream_process_connect (CamelStreamProcess *stream,
                              const gchar *command,
                              const gchar **env,
                              GError **error)
{
	gint sockfds[2];

	g_return_val_if_fail (CAMEL_IS_STREAM_PROCESS (stream), -1);
	g_return_val_if_fail (command != NULL, -1);

	if (stream->sockfd != -1 || stream->childpid)
		camel_stream_close (CAMEL_STREAM (stream), NULL, NULL);

	if (socketpair (AF_UNIX, SOCK_STREAM, 0, sockfds))
		goto fail;

	stream->childpid = fork ();
	if (!stream->childpid) {
		do_exec_command (sockfds[1], command, (gchar **) env);
	} else if (stream->childpid == -1) {
		close (sockfds[0]);
		close (sockfds[1]);
		stream->sockfd = -1;
		goto fail;
	}

	close (sockfds[1]);
	stream->sockfd = sockfds[0];

	return 0;

fail:
	if (errno == EINTR)
		g_set_error (
			error, G_IO_ERROR,
			G_IO_ERROR_CANCELLED,
			_("Connection cancelled"));
	else
		g_set_error (
			error, G_IO_ERROR,
			g_io_error_from_errno (errno),
			_("Could not connect with command \"%s\": %s"),
			command, g_strerror (errno));

	return -1;
}
