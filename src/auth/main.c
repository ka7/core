/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "ioloop.h"
#include "network.h"
#include "lib-signals.h"
#include "restrict-access.h"
#include "fd-close-on-exec.h"
#include "randgen.h"
#include "password-scheme.h"
#include "mech.h"
#include "auth.h"
#include "auth-request-handler.h"
#include "auth-master-connection.h"
#include "auth-client-connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

struct ioloop *ioloop;
int standalone = FALSE;
time_t process_start_time;

static buffer_t *masters_buf;
static struct auth *auth;

static void sig_quit(int signo __attr_unused__)
{
	io_loop_stop(ioloop);
}

static void open_logfile(void)
{
	if (getenv("LOG_TO_MASTER") != NULL) {
		i_set_failure_internal();
		return;
	}

	if (getenv("USE_SYSLOG") != NULL)
		i_set_failure_syslog("dovecot-auth", LOG_NDELAY, LOG_MAIL);
	else {
		/* log to file or stderr */
		i_set_failure_file(getenv("LOGFILE"), "dovecot-auth");
	}

	if (getenv("INFOLOGFILE") != NULL)
		i_set_info_file(getenv("INFOLOGFILE"));

	i_set_failure_timestamp_format(getenv("LOGSTAMP"));
}

static uid_t get_uid(const char *user)
{
	struct passwd *pw;

	if (user == NULL)
		return (uid_t)-1;

	if ((pw = getpwnam(user)) == NULL)
		i_fatal("User doesn't exist: %s", user);
	return pw->pw_uid;
}

static gid_t get_gid(const char *group)
{
	struct group *gr;

	if (group == NULL)
		return (gid_t)-1;

	if ((gr = getgrnam(group)) == NULL)
		i_fatal("Group doesn't exist: %s", group);
	return gr->gr_gid;
}

static int create_unix_listener(const char *env, int backlog)
{
	const char *path, *mode, *user, *group;
	mode_t old_umask;
	unsigned int mask;
	uid_t uid;
	gid_t gid;
	int fd, i;

	path = getenv(env);
	if (path == NULL)
		return -1;

	mode = getenv(t_strdup_printf("%s_MODE", env));
	if (mode == NULL)
		mask = 0177; /* default to 0600 */
	else {
		if (sscanf(mode, "%o", &mask) != 1)
			i_fatal("%s: Invalid mode %s", env, mode);
		mask = (mask ^ 0777) & 0777;
	}

	old_umask = umask(mask);
	for (i = 0; i < 5; i++) {
		fd = net_listen_unix(path, backlog);
		if (fd != -1)
			break;

		if (errno != EADDRINUSE)
			i_fatal("net_listen_unix(%s) failed: %m", path);

		/* see if it really exists */
		if (net_connect_unix(path) != -1 || errno != ECONNREFUSED)
			i_fatal("Socket already exists: %s", path);

		/* delete and try again */
		if (unlink(path) < 0)
			i_fatal("unlink(%s) failed: %m", path);
	}
	umask(old_umask);

	user = getenv(t_strdup_printf("%s_USER", env));
	group = getenv(t_strdup_printf("%s_GROUP", env));

	uid = get_uid(user); gid = get_gid(group);
	if (chown(path, uid, gid) < 0) {
		i_fatal("chown(%s, %s, %s) failed: %m",
			path, dec2str(uid), dec2str(gid));
	}

	return fd;
}

static void add_extra_listeners(void)
{
	struct auth_master_connection *master;
	const char *str, *client_path, *master_path;
	int client_fd, master_fd;
	unsigned int i;

	for (i = 1;; i++) {
		t_push();
		client_path = getenv(t_strdup_printf("AUTH_%u", i));
		master_path = getenv(t_strdup_printf("AUTH_%u_MASTER", i));
		if (client_path == NULL && master_path == NULL) {
			t_pop();
			break;
		}

		str = t_strdup_printf("AUTH_%u", i);
		client_fd = create_unix_listener(str, 16);
		str = t_strdup_printf("AUTH_%u_MASTER", i);
		master_fd = create_unix_listener(str, 1);

		master = auth_master_connection_create(auth, -1);
		if (master_fd != -1) {
			auth_master_connection_add_listener(master, master_fd,
							    master_path, FALSE);
		}
		if (client_fd != -1) {
			auth_master_connection_add_listener(master, client_fd,
							    client_path, TRUE);
		}
		auth_client_connections_init(master);
		buffer_append(masters_buf, &master, sizeof(master));
		t_pop();
	}
}

static void drop_privileges(void)
{
	open_logfile();

	/* Open /dev/urandom before chrooting */
	random_init();

	/* Initialize databases so their configuration files can be readable
	   only by root. Also load all modules here. */
	auth = auth_preinit();
        password_schemes_init();

	masters_buf = buffer_create_dynamic(default_pool, 64);
	add_extra_listeners();

	/* Password lookups etc. may require roots, allow it. */
	restrict_access_by_env(FALSE);
}

static void main_init(int nodaemon)
{
	struct auth_master_connection *master, **master_p;
	size_t i, size;

	process_start_time = ioloop_time;

	process_start_time = ioloop_time;

	mech_init();
	auth_init(auth);
	auth_request_handlers_init();

	lib_init_signals(sig_quit);

	standalone = getenv("DOVECOT_MASTER") == NULL;
	if (standalone) {
		/* starting standalone */
		if (getenv("AUTH_1") == NULL) {
			i_fatal("dovecot-auth is usually started through "
				"dovecot master process. If you wish to run "
				"it standalone, you'll need to set AUTH_* "
				"environment variables (AUTH_1 isn't set).");
		}

		if (!nodaemon) {
			switch (fork()) {
			case -1:
				i_fatal("fork() failed: %m");
			case 0:
				break;
			default:
				exit(0);
			}

			if (setsid() < 0)
				i_fatal("setsid() failed: %m");

			if (chdir("/") < 0)
				i_fatal("chdir(/) failed: %m");
		}
       } else {
		master = auth_master_connection_create(auth, MASTER_SOCKET_FD);
		auth_master_connection_add_listener(master, LOGIN_LISTEN_FD,
						    NULL, TRUE);
		auth_client_connections_init(master);
		buffer_append(masters_buf, &master, sizeof(master));
	}

	/* everything initialized, notify masters that all is well */
	master_p = buffer_get_modifyable_data(masters_buf, &size);
	size /= sizeof(*master_p);
	for (i = 0; i < size; i++)
		auth_master_connection_send_handshake(master_p[i]);
}

static void main_deinit(void)
{
	struct auth_master_connection **master;
	size_t i, size;

        if (lib_signal_kill != 0)
		i_warning("Killed with signal %d", lib_signal_kill);

	auth_request_handlers_flush_failures();

	master = buffer_get_modifyable_data(masters_buf, &size);
	size /= sizeof(*master);
	for (i = 0; i < size; i++)
		auth_master_connection_destroy(master[i]);

        password_schemes_deinit();
	auth_request_handlers_deinit();
	auth_deinit(auth);
	mech_deinit();

	random_deinit();

	closelog();
}

int main(int argc, char *argv[])
{
#ifdef DEBUG
	if (getenv("GDB") == NULL)
		fd_debug_verify_leaks(4, 1024);
#endif
	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();
	ioloop = io_loop_create(system_pool);

	drop_privileges();

	main_init(argc > 1 && strcmp(argv[1], "-F") == 0);
        io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(ioloop);
	lib_deinit();

        return 0;
}
