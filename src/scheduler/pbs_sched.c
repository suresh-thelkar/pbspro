/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file    pbs_sched.c
 *
 * @brief
 * 		pbs_sched.c - contains functions related to Pbs scheduler.
 *
 * Functions included are:
 *	on_segv()
 *	sigfunc_pipe()
 *	die()
 *	server_disconnect()
 *	socket_to_conn()
 *	addclient()
 *	read_config()
 *	restart()
 *	soft_cycle_interrupt()
 *	hard_cycle_interrupt()
 *	badconn()
 *	server_command()
 *	engage_authentication()
 *	update_svr_schedobj()
 *	lock_out()
 *	are_we_primary()
 *	log_rppfail()
 *	log_tppmsg()
 *	main()
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include        <sys/types.h>
#include	<grp.h>
#include	<stdio.h>
#include        <stdlib.h>
#include        <string.h>
#include        <signal.h>
#include	<time.h>
#include        <ctype.h>
#include        <limits.h>
#include        <fcntl.h>
#include	<errno.h>
#include	<netdb.h>
#include	<unistd.h>
#include	<sys/wait.h>
#include	<sys/time.h>
#include        <sys/stat.h>
#include        <sys/socket.h>
#include	<sys/resource.h>
#include        <netinet/in.h>
#include        <arpa/inet.h>
#ifdef _POSIX_MEMLOCK
#include <sys/mman.h>
#endif	/* _POSIX_MEMLOCK */

#if defined(FD_SET_IN_SYS_SELECT_H)
#include <sys/select.h>
#endif
#include <sys/resource.h>

#include	"pbs_version.h"
#include	"portability.h"
#include	"libpbs.h"
#include	"pbs_error.h"
#include	"pbs_ifl.h"
#include	"log.h"
#include	"sched_cmds.h"
#include	"server_limits.h"
#include	"net_connect.h"
#include	"rm.h"
#include	"rpp.h"
#include	"libsec.h"
#include	"pbs_ecl.h"
#include	"pbs_share.h"
#include	"config.h"
#include	"fifo.h"
#include	"globals.h"
#include	"pbs_undolr.h"
#include	"multi_threading.h"

struct		connect_handle connection[NCONNECTS];
int		connector;
int		server_sock;
int		second_connection = -1;

#define		START_CLIENTS	2	/* minimum number of clients */
#define		MAX_PORT_NUM 65535
#define		STARTING_PORT_NUM 15050
pbs_net_t	*okclients = NULL;	/* accept connections from */
int		numclients = 0;		/* the number of clients */
char		*configfile = NULL;	/* name of file containing
						 client names to be added */

extern char		*msg_daemonname;
char		**glob_argv;
char		usage[] =
	"[-d home][-L logfile][-p file][-I schedname][-S port][-R port][-n][-N][-c clientsfile][-t num threads]";
struct	sockaddr_in	saddr;
sigset_t	allsigs;
int		pbs_rm_port;

/* if we received a sigpipe, this probably means the server went away. */
int		got_sigpipe = 0;

/* used in segv restart */
time_t segv_start_time;
time_t segv_last_time;
struct tpp_config tpp_conf; /* global settings for tpp */

#ifdef NAS /* localmod 030 */
extern int do_soft_cycle_interrupt;
extern int do_hard_cycle_interrupt;
#endif /* localmod 030 */

static int	engage_authentication(struct connect_handle *);

extern char *msg_startup1;

static pthread_mutex_t cleanup_lock;

/**
 * @brief
 * 		cleanup after a segv and re-exec.  Trust as little global mem
 * 		as possible... we don't know if it could be corrupt
 *
 * @param[in]	sig	-	signal
 */
void
on_segv(int sig)
{
	int ret_lock = -1;

	/* We want any other threads to block here, we want them alive until abort() is called
	 * as it dumps core for all threads
	 */
	ret_lock = pthread_mutex_lock(&cleanup_lock);
	if (ret_lock != 0)
		pthread_exit(NULL);

	/* we crashed less then 5 minutes ago, lets not restart ourself */
	if ((segv_last_time - segv_start_time) < 300) {
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__,
				"received a sigsegv within 5 minutes of start: aborting.");

		/* Not unlocking mutex on purpose, we need to hold on to it until the process is killed */
		abort();
	}

	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__,
			"received segv and restarting");

	if (fork() > 0) { /* the parent rexec's itself */
		sleep(10); /* allow the child to die */
		execv(glob_argv[0], glob_argv);
		exit(3);
	} else {
		abort(); /* allow to core and exit */
	}
}

/**
 * @brief
 * 		signal function for receiving a sigpipe - set flag so we know not to talk
 * 		to the server any more and leave the cycle as soon as possible
 *
 * @param[in]	sig	-	sigpipe
 */
void
sigfunc_pipe(int sig)
{
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, "sigfunc_pipe", "We've received a sigpipe: The server probably died.");
	got_sigpipe = 1;
}


/**
 * @brief
 *       Clean up after a signal.
 *
 *  @param[in]	sig	-	signal
 */
void
die(int sig)
{
	int ret_lock = -1;

	ret_lock = pthread_mutex_trylock(&cleanup_lock);
	if (ret_lock != 0)
		pthread_exit(NULL);

	if (sig > 0) {
		sprintf(log_buffer, "caught signal %d", sig);
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
			__func__, log_buffer);
	}
	else {
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				__func__, "abnormal termination");
	}

	schedexit();

	{
		int csret;
		if ((csret = CS_close_app()) != CS_SUCCESS) {
			/*had some problem closing the security library*/

			sprintf(log_buffer, "problem closing security library (%d)", csret);
			log_err(-1, "pbs_sched", log_buffer);
		}
	}

	log_close(1);
	exit(1);
}

/**
 *	@brief
 *		specialized disconnect function similar to pbs_disconnect()
 *		We need a specialized function to disconnect the socket
 *		between the server and scheduler.  If we use the regular one,
 *		we can have timeout problems and run out of reserved ports
 *
 *	@param[in]	connect	-	connection descriptor to disconnect
 *
 *	@return	success/failure
 *	@retval	1	: successfully disconnected
 *	@retval 0	: failure to disconnect
 */
int
server_disconnect(int connect)
{
	int	sd;
	int	ret;

	if ((connect < 0) || (connect > NCONNECTS))
		return 0;

	if (pbs_client_thread_lock_connection(connect) != 0)
		return 0;

	if ((sd = connection [connect].ch_socket) >= 0) {

		if ((ret = CS_close_socket(sd)) != CS_SUCCESS) {

			sprintf(log_buffer,
				"Problem closing connection security (%d)", ret);
			log_err(-1, "close_conn", log_buffer);
		}
		close(sd);
	}

	if (connection[connect].ch_errtxt != NULL) {
		free(connection[connect].ch_errtxt);
		connection[connect].ch_errtxt = NULL;

	}
	connection[connect].ch_errno = 0;
	connection[connect].ch_inuse = 0;

	(void)pbs_client_thread_unlock_connection(connect);
	pbs_client_thread_destroy_connect_context(connect);

	return 1;
}
/**
 * @brief
 * 		assign socket to the connect handle and unlock the connectable thread.
 *
 * @param[in]	sock	-	opened socket
 *
 * @return	int
 * @return	index,i	: if thread is unlocked
 * @retval	-1	: error, not able to connect.
 */
int
socket_to_conn(int sock)
{
	int     i;

	for (i=0; i<NCONNECTS; i++) {
		if (connection[i].ch_inuse == 0) {

			if (pbs_client_thread_lock_conntable() != 0)
				return -1;

			connection[i].ch_inuse = 1;
			connection[i].ch_errno = 0;
			connection[i].ch_socket= sock;
			connection[i].ch_errtxt = NULL;

			if (pbs_client_thread_unlock_conntable() != 0)
				return -1;

			return (i);
		}
	}
	pbs_errno = PBSE_NOCONNECTS;
	return (-1);
}
/**
 * @brief
 * 		add a new client to the list of clients.
 *
 * @param[in]	name	-	Client name.
 */
int
addclient(char *name)
{
	int	i;
	struct	hostent		*host, *gethostbyname();
	struct  in_addr saddr;

	if ((host = gethostbyname(name)) == NULL) {
		sprintf(log_buffer, "host %s not found", name);
		log_err(-1, __func__, log_buffer);
		return -1;
	}

	for (i = 0; host->h_addr_list[i]; i++) {
		if (numclients >= START_CLIENTS) {
			pbs_net_t	*newclients;

			newclients = realloc(okclients,
				sizeof(pbs_net_t)*(numclients+1));
			if (newclients == NULL)
				return -1;
			okclients = newclients;
		}
		memcpy((char *)&saddr, host->h_addr_list[i], host->h_length);
		okclients[numclients++] = saddr.s_addr;
	}
	return 0;
}

/**
 * @brief
 * 		read_config - read and process the configuration file (see -c option)
 * @par
 *		Currently, the only statement is $clienthost to specify which systems
 *		can contact the scheduler.
 *
 * @param[in]	file	-	configuration file
 *
 * @return	int
 * @retval	0	: Ok
 * @retval	-1	: !nOtOk!
 */
#define CONF_LINE_LEN 120

static
int
read_config(char *file)
{
	FILE	*conf;
	int	i;
	char	line[CONF_LINE_LEN];
	char	*token;
	struct	specialconfig {
		char	*name;
		int	(*handler)();
	} special[] = {
		{"clienthost",	addclient },
		{ NULL,		NULL }
	};


#if !defined(DEBUG) && !defined(NO_SECURITY_CHECK)
	if (chk_file_sec(file, 0, 0, S_IWGRP|S_IWOTH, 1))
		return (-1);
#endif

	if ((conf = fopen(file, "r")) == NULL) {
		log_err(errno, __func__, "cannot open config file");
		return (-1);
	}
	while (fgets(line, CONF_LINE_LEN, conf)) {

		if ((line[0] == '#') || (line[0] == '\n'))
			continue;		/* ignore comment & null line */
		else if (line[0] == '$') {	/* special */

			if ((token = strtok(line, " \t")) == NULL)
				token = "";
			for (i=0; special[i].name; i++) {
				if (strcmp(token+1, special[i].name) == 0)
					break;
			}
			if (special[i].name == NULL) {
				sprintf(log_buffer, "config name %s not known",
					token);
				log_record(PBSEVENT_ERROR,
					PBS_EVENTCLASS_SERVER, LOG_INFO,
					msg_daemonname, log_buffer);
				continue;
			}
			token = strtok(NULL, " \t");
			if (*(token+strlen(token)-1) == '\n')
				*(token+strlen(token)-1) = '\0';
			if (special[i].handler(token)) {
				fclose(conf);
				return (-1);
			}

		} else {
			log_record(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER,
				LOG_INFO, msg_daemonname,
				"invalid line in config file");
			fclose(conf);
			return (-1);
		}
	}
	fclose(conf);
	return (0);
}
/**
 * @brief
 * 		restart on signal
 *
 * @param[in]	sig	-	signal
 */
void
restart(int sig)
{
	if (sig) {
		log_close(1);
		log_open(logfile, path_log);
		sprintf(log_buffer, "restart on signal %d", sig);
	} else {
		sprintf(log_buffer, "restart command");
	}
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, log_buffer);
	if (configfile) {
		if (read_config(configfile) != 0)
			die(0);
	}
	schedule(SCH_CONFIGURE, -1, NULL);
}

#ifdef NAS /* localmod 030 */
/**
 * @brief
 * 		make soft cycle interrupt active
 *
 * @param[in]	sig	-	signal
 */
void
soft_cycle_interrupt(int sig)
{
	do_soft_cycle_interrupt = 1;
}
/**
 * @brief
 * 		make hard cycle interrupt active
 *
 * @param[in]	sig	-	signal
 */
void
hard_cycle_interrupt(int sig)
{
	do_hard_cycle_interrupt = 1;
}
#endif /* localmod 030 */
/**
 * @brief
 * 		log the bad connection message
 *
 * @param[in]	msg	-	The message to be logged.
 */
void
badconn(char *msg)
{
	struct	in_addr	addr;
	char		buf[5*sizeof(addr) + 100];
	struct	hostent	*phe;

	addr = saddr.sin_addr;
	phe = gethostbyaddr((void *)&addr, sizeof(addr), AF_INET);
	if (phe == NULL) {
		char	hold[6];
		int	i;
		union {
			struct	in_addr aa;
			u_char		bb[sizeof(addr)];
		} uu;

		uu.aa = addr;
#ifdef NAS /* localmod 005 */
		sprintf(buf, "%u", (unsigned int) uu.bb[0]);
#else
		sprintf(buf, "%u", uu.bb[0]);
#endif /* localmod 005 */
		for (i=1; i<sizeof(addr); i++) {
#ifdef NAS /* localmod 005 */
			sprintf(hold, ".%u", (unsigned int) uu.bb[i]);
#else
			sprintf(hold, ".%u", uu.bb[i]);
#endif /* localmod 005 */
			strcat(buf, hold);
		}
	}
	else {
		strncpy(buf, phe->h_name, sizeof(buf));
		buf[sizeof(buf)-1] = '\0';
	}

	sprintf(log_buffer, "%s on port %u %s",
#ifdef NAS /* localmod 005 */
		buf, (unsigned int) ntohs(saddr.sin_port), msg);
#else
		buf, ntohs(saddr.sin_port), msg);
#endif /* localmod 005 */
	log_err(-1, __func__, log_buffer);
	return;
}

/**
 *
 * @brief
 *		Gets a scheduling command  from the server from the primary
 *		socket connection to the server. This would also attempt to
 *		get a secondary socket connection to the server, which will
 *		contain high priority scheduling commands like
 *		SCH_SCHEDULER_RESTART_CYCLE.
 *
 * @param[in]	jid	-	if command received is SCH_SCHEDULE_AJOB, then
 *			  			*jid will hold the jobid.
 *
 * @return	int
 * @retval	SCH_ERROR	-	if an error occured.
 * @reval	<scheduling command>	-	for example, SCH_SCHEDULE_CMD>
 *
 * @note
 *		The returned *jid is a malloc-ed string which must be freed by the
 *		caller.
 */
int
server_command(char **jid)
{
	int		new_socket;
	pbs_socklen_t	slen;
	int		i;
	int		cmd;
	pbs_net_t	addr;
	extern	int	get_sched_cmd(int sock, int *val, char **jobid);
	fd_set		fdset;
	struct timeval  timeout;

	slen = sizeof(saddr);
	new_socket = accept(server_sock,
		(struct sockaddr *)&saddr, &slen);
	if (new_socket == -1) {
		log_err(errno, __func__, "accept");
		return SCH_ERROR;
	}

	if (set_nodelay(new_socket) == -1) {
		snprintf(log_buffer, sizeof(log_buffer), "cannot set nodelay on primary socket connection %d (errno=%d)\n", new_socket, errno);
		log_err(-1, __func__, log_buffer);
		return SCH_ERROR;
	}

	if (ntohs(saddr.sin_port) >= IPPORT_RESERVED) {
		badconn("non-reserved port");
		close(new_socket);
		return SCH_ERROR;
	}

	addr = (pbs_net_t)saddr.sin_addr.s_addr;
	for (i=0; i<numclients; i++) {
		if (addr == okclients[i])
			break;
	}
	if (i == numclients) {
		badconn("unauthorized host");
		close(new_socket);
		return SCH_ERROR;
	}

	if ((connector = socket_to_conn(new_socket)) < 0) {
		log_err(errno, __func__, "socket_to_conn");
		close(new_socket);
		return SCH_ERROR;
	}

	if (engage_authentication(&connection [connector]) == -1) {
		CS_close_socket(new_socket);
		close(new_socket);
		return SCH_ERROR;
	}

	/* get_sched_cmd() located in file get_4byte.c */
	if (get_sched_cmd(new_socket, &cmd, jid) != 1) {
		log_err(errno, __func__, "get_sched_cmd");
		CS_close_socket(new_socket);
		close(new_socket);
		return SCH_ERROR;
	}

	/* Obtain the second server socket connnection		*/
	/* this second connection is for server to communicate	*/
	/* "super" high priority command like			*/
	/* SCH_SCHEDULE_RESTART_CYCLE				*/
	/* This won't cause scheduling to quit if an error      */
	/* resulted in obtaining this second connection.	*/
	timeout.tv_usec = 0;
	timeout.tv_sec  = 1;

	FD_ZERO(&fdset);
	FD_SET(server_sock, &fdset);
	if ((select(FD_SETSIZE, &fdset, NULL, NULL,
		&timeout) != -1)  && (FD_ISSET(server_sock, &fdset))) {
		int	cmd2;
		char	*jid2 = NULL;

		second_connection = accept(server_sock,
			(struct sockaddr *)&saddr, &slen);
		if (second_connection == -1) {
			log_err(errno, __func__,
				"warning: failed to get second_connection");
			return cmd; /* bail out early */
		}

		if (set_nodelay(second_connection) == -1) {
			snprintf(log_buffer, sizeof(log_buffer), "cannot set nodelay on secondary socket connection %d (errno=%d)\n", second_connection, errno);
			log_err(-1, __func__, log_buffer);
			return cmd;
		}

		if (ntohs(saddr.sin_port) >= IPPORT_RESERVED) {
			badconn("second_connection: non-reserved port");
			close(second_connection);
			second_connection = -1;
			return cmd;
		}

		addr = (pbs_net_t)saddr.sin_addr.s_addr;
		for (i=0; i<numclients; i++) {
			if (addr == okclients[i])
				break;
		}

		if (i == numclients) {
			badconn("second_connection: unauthorized host");
			close(second_connection);
			second_connection = -1;
			return cmd;
		}

		if (get_sched_cmd(second_connection, &cmd2, &jid2) != 1) {
			log_err(errno, __func__, "get_sched_cmd");
			close(second_connection);
			second_connection = -1;
		}

		if (jid2 != NULL) {
			free(jid2);
		}
	} else {
		log_event(PBSEVENT_DEBUG, LOG_DEBUG,
			PBS_EVENTCLASS_SERVER, __func__,
			"warning: timed-out getting second_connection");
	}

	return cmd;
}

/**
 * @brief
 *  	engage_authentication - Use the security library interface to
 * 		engage the appropriate connection authentication.
 *
 * @param[in]	phandle	-	pointer to a "struct connect_handle"
 *
 * @return	int
 * @retval	0	: successful
 * @retval	-1	: unsuccessful
 *
 * @par Remark:	If the authentication fails, messages are logged to
 *              the scheduler's log file and the connection's security
 *              information is closed out (freed).
 */
static int
engage_authentication(struct connect_handle *phandle)
{
	int	ret;
	int	sd;

	if (phandle == NULL || (sd = phandle->ch_socket) <0) {

		cs_logerr(0, "engage_authentication",
			"Bad arguments, unable to authenticate.");
		return (-1);
	}

	if ((ret = CS_server_auth(sd)) == CS_SUCCESS)
		return (0);

	if (ret == CS_AUTH_CHECK_PORT) {

		/* authentication based on iff and reserved ports
		 * caller has already checked port range
		 */

		return (0);
	}

	sprintf(log_buffer,
		"Unable to authenticate connection (%d)",
		ret);

	log_err(-1, "engage_authentication:", log_buffer);

	return (-1);
}

/**
 * @brief
 * 		sends scheduler object attributes to pbs_server
 *	  	What we send: who we are (ATTR_SchedHost), our version (ATTR_version),
 *			The alarm cmd line -a value (ATTR_sched_cycle_len)
 * @par
 *	  When do we send the attributes:
 *			The first call to this function after scheduler restart
 *			First cycle after any server restart(SCH_SCHEDULE_FIRST)
 *
 *
 * @param	cmd[in]	-	scheduling command from the server -- see sched_cmds.h
 * @param	alarm[in]	-	alarm value (cmd line option -a) set if > 0
 *
 * @par Side-Effects: none
 *
 * @par	MT-Unsafe
 *
 * @return	void
 */

/**
 * @brief
 * 		lock_out - lock out other daemons from this directory.
 *
 * @param[in]	fds	-	file descriptor
 * @param[in]	op	-	F_WRLCK  or  F_UNLCK
 *
 * @return	1
 */

static void
lock_out(int fds, int op)
{
	struct flock flock;

	(void)lseek(fds, (off_t)0, SEEK_SET);
	flock.l_type   = op;
	flock.l_whence = SEEK_SET;
	flock.l_start  = 0;
	flock.l_len    = 0;	/* whole file */
	if (fcntl(fds, F_SETLK, &flock) < 0) {
		(void)strcpy(log_buffer, "pbs_sched: another scheduler running\n");
		log_err(errno, msg_daemonname, log_buffer);
		fprintf(stderr, "%s", log_buffer);
		exit(1);
	}
}

/**
 * @brief
 * 		are_we_primary - are we on the primary Server host
 *		If either the only configured Server or the Primary in a failover
 *		configuration - return true
 *
 * @return	int
 * @retval	0	: we are the secondary
 * @retval	-1	: cannot be neither
 * @retval	1	: we are the listed primary
 */
static int
are_we_primary()
{
	char server_host[PBS_MAXHOSTNAME+1];
	char hn1[PBS_MAXHOSTNAME+1];

	if (pbs_conf.pbs_leaf_name) {
		char *endp;
		snprintf(server_host, sizeof(server_host), "%s", pbs_conf.pbs_leaf_name);
		endp = strchr(server_host, ','); /* find the first name */
		if (endp)
			*endp = '\0';
		endp = strchr(server_host, ':'); /* cut out the port */
		if (endp)
			*endp = '\0';
	} else if ((gethostname(server_host, (sizeof(server_host) - 1)) == -1) ||
		(get_fullhostname(server_host, server_host, (sizeof(server_host) - 1)) == -1)) {
		log_err(-1, __func__, "Unable to get my host name");
		return -1;
	}

	/* both secondary and primary should be set or neither set */
	if ((pbs_conf.pbs_secondary == NULL) && (pbs_conf.pbs_primary == NULL))
		return 1;
	if ((pbs_conf.pbs_secondary == NULL) || (pbs_conf.pbs_primary == NULL))
		return -1;

	if (get_fullhostname(pbs_conf.pbs_primary, hn1, (sizeof(hn1) - 1))==-1) {
		log_err(-1, __func__, "Unable to get full host name of primary");
		return -1;
	}

	if (strcmp(hn1, server_host) == 0)
		return 1;	/* we are the listed primary */

	if (get_fullhostname(pbs_conf.pbs_secondary, hn1, (sizeof(hn1) - 1))==-1) {
		log_err(-1, __func__, "Unable to get full host name of secondary");
		return -1;
	}
	if (strcmp(hn1, server_host) == 0)
		return 0;	/* we are the secondary */

	return -1;		/* cannot be neither */
}

/**
 * @brief
 * 		log the rpp failure message
 *
 * @param[in]	mess	-	message to be logged.
 */
void
log_rppfail(char *mess)
{
	log_event(PBSEVENT_DEBUG, LOG_DEBUG,
		PBS_EVENTCLASS_SERVER, "rpp", mess);
}

/*
 * @brief
 *		This is the log handler for tpp implemented in the daemon. The pointer to
 *		this function is used by the Libtpp layer when it needs to log something to
 *		the daemon logs
 *
 * @param[in]	level	-	Logging level
 * @param[in]	objname	-	Name of the object about which logging is being done
 * @param[in]	mess	-	The log message
 *
 */
static void
log_tppmsg(int level, const char *objname, char *mess)
{
	char id[2*PBS_MAXHOSTNAME];
	int thrd_index;
	int etype = log_level_2_etype(level);

	thrd_index = tpp_get_thrd_index();
	if (thrd_index == -1)
		snprintf(id, sizeof(id), "%s(Main Thread)", (objname != NULL) ? objname : msg_daemonname);
	else
		snprintf(id, sizeof(id), "%s(Thread %d)", (objname != NULL) ? objname : msg_daemonname, thrd_index);

	log_event(etype, PBS_EVENTCLASS_TPP, level, id, mess);
	DBPRT((mess));
	DBPRT(("\n"));
}
/**
 * @brief
 * 		the entry point of the pbs_sched.
 */
int
main(int argc, char *argv[])
{
	int		go, c, rc, errflg = 0;
	int		lockfds;
	int		t = 1;
	pid_t		pid;
	char		host[PBS_MAXHOSTNAME+1];
#ifndef DEBUG
	char		*dbfile = "sched_out";
#endif
	struct	sigaction	act;
	sigset_t	oldsigs;
	extern	char	*optarg;
	extern	int	optind, opterr;
	char	       *runjobid = NULL;
	extern	int	rpp_fd;
	fd_set		fdset;
	int		opt_no_restart = 0;
#ifdef NAS /* localmod 031 */
	time_t		now;
#endif /* localmod 031 */
	int		stalone = 0;
#ifdef _POSIX_MEMLOCK
	int		do_mlockall = 0;
#endif	/* _POSIX_MEMLOCK */
	int		alarm_time = 0;
	char 		logbuf[1024];
	time_t		rpp_advise_timeout = 30;	/* rpp_read timeout */
	extern char     *msg_corelimit;
#ifdef  RLIMIT_CORE
	int      	char_in_cname = 0;
#endif  /* RLIMIT_CORE */
	int nthreads = -1;
	int num_cores;
	char *endp = NULL;
	pthread_mutexattr_t attr;
	static int update_svr = 1;

	/*the real deal or show version and exit?*/

	PRINT_VERSION_AND_EXIT(argc, argv);

	num_cores = sysconf(_SC_NPROCESSORS_ONLN);

	if(set_msgdaemonname("pbs_sched")) {
		fprintf(stderr, "Out of memory\n");
		return (1);
	}

#ifndef DEBUG
	if ((geteuid() != 0) || (getuid() != 0)) {
		fprintf(stderr, "%s: Must be run by root\n", argv[0]);
		return (1);
	}
#endif	/* DEBUG */

	/* disable attribute verification */
	set_no_attribute_verification();

	/* initialize the thread context */
	if (pbs_client_thread_init_thread_context() != 0) {
		fprintf(stderr, "%s: Unable to initialize thread context\n",
			argv[0]);
		return (1);
	}

	if (pbs_loadconf(0) == 0)
		return (1);

	nthreads = pbs_conf.pbs_sched_threads;

	glob_argv = argv;
	segv_start_time = segv_last_time = time(NULL);


	sched_port = pbs_conf.scheduler_service_port;
	pbs_rm_port = pbs_conf.manager_service_port;

	opterr = 0;
	while ((c = getopt(argc, argv, "lL:NS:I:R:d:p:c:a:nt:")) != EOF) {
		switch (c) {
			case 'l':
#ifdef _POSIX_MEMLOCK
				do_mlockall = 1;
#else
				fprintf(stderr, "-l option - mlockall not supported\n");
#endif	/* _POSIX_MEMLOCK */
				break;
			case 'L':
				logfile = optarg;
				break;
			case 'N':
				stalone = 1;
				break;
			case 'I':
				sc_name = optarg;
				break;
			case 'S':
				sched_port = atoi(optarg);
				if (sched_port == 0) {
					fprintf(stderr,
						"%s: illegal port\n", optarg);
					errflg = 1;
				}
				break;
			case 'R':
				if ((pbs_rm_port = atoi(optarg)) == 0) {
					(void)fprintf(stderr, "%s: bad -R %s\n",
						argv[0], optarg);
					return 1;
				}
				break;
			case 'd':
				if (pbs_conf.pbs_home_path != NULL)
					free(pbs_conf.pbs_home_path);
				pbs_conf.pbs_home_path = optarg;
				break;
			case 'p':
#ifndef DEBUG
				dbfile = optarg;
#endif
				break;
			case 'c':
				configfile = optarg;
				break;
			case 'a':
				alarm_time = atoi(optarg);
				if (alarm_time == 0) {
					fprintf(stderr,
						"%s: bad alarm time\n", optarg);
					errflg = 1;
				}
				fprintf(stderr, "The -a option is deprecated.  Please see the \'%s\' scheduler attribute.\n", ATTR_sched_cycle_len);
				break;
			case 'n':
				opt_no_restart = 1;
				break;
			case 't':
				nthreads = strtol(optarg, &endp, 10);
				if (*endp != '\0') {
					fprintf(stderr, "%s: bad num threads value\n", optarg);
					errflg = 1;
				}
				if (nthreads < 1) {
					fprintf(stderr, "%s: bad num threads value (should be in range 1-99999)\n", optarg);
					errflg = 1;
				}
				if (nthreads > num_cores) {
					fprintf(stderr, "%s: cannot be larger than number of cores %d, using number of cores instead\n",
							optarg, num_cores);
					nthreads = num_cores;
				}
				break;
			default:
				errflg = 1;
				break;
		}
	}

	if (sc_name == NULL) {
		sc_name = PBS_DFLT_SCHED_NAME;
		dflt_sched = 1;
	}

	if (errflg) {
		fprintf(stderr, "usage: %s %s\n", argv[0], usage);
		fprintf(stderr, "       %s --version\n", argv[0]);
		exit(1);
	}

	if (dflt_sched) {
		(void)sprintf(log_buffer, "%s/sched_priv", pbs_conf.pbs_home_path);
	} else {
		(void)sprintf(log_buffer, "%s/sched_priv_%s", pbs_conf.pbs_home_path, sc_name);
	}
#if !defined(DEBUG) && !defined(NO_SECURITY_CHECK)
	c  = chk_file_sec(log_buffer, 1, 0, S_IWGRP|S_IWOTH, 1);
	c |= chk_file_sec(pbs_conf.pbs_environment, 0, 0, S_IWGRP|S_IWOTH, 0);
	if (c != 0) exit(1);
#endif  /* not DEBUG and not NO_SECURITY_CHECK */
	if (chdir(log_buffer) == -1) {
		perror("chdir");
		exit(1);
	}
	if (dflt_sched) {
		(void)sprintf(path_log,   "%s/sched_logs", pbs_conf.pbs_home_path);
	} else {
		(void)sprintf(path_log,   "%s/sched_logs_%s", pbs_conf.pbs_home_path, sc_name);
	}
	if (log_open(logfile, path_log) == -1) {
		fprintf(stderr, "%s: logfile could not be opened\n", argv[0]);
		exit(1);
	}

	/* The following is code to reduce security risks                */
	/* start out with standard umask, system resource limit infinite */

	umask(022);
	if (setup_env(pbs_conf.pbs_environment)==-1)
		exit(1);
	c = getgid();
	(void)setgroups(1, (gid_t *)&c);	/* secure suppl. groups */
#ifndef DEBUG
	{
		struct rlimit rlimit;
		int curerror;

		rlimit.rlim_cur = RLIM_INFINITY;
		rlimit.rlim_max = RLIM_INFINITY;
		(void)setrlimit(RLIMIT_CPU,   &rlimit);
		(void)setrlimit(RLIMIT_FSIZE, &rlimit);
		(void)setrlimit(RLIMIT_DATA,  &rlimit);
#ifdef  RLIMIT_RSS
		(void)setrlimit(RLIMIT_RSS  , &rlimit);
#endif  /* RLIMIT_RSS */
#ifdef  RLIMIT_VMEM
		(void)setrlimit(RLIMIT_VMEM  , &rlimit);
#endif  /* RLIMIT_VMEM */
#ifdef  RLIMIT_CORE
		if (pbs_conf.pbs_core_limit) {
			struct rlimit corelimit;

			char *pc = pbs_conf.pbs_core_limit;
			while (*pc != '\0') {
				if (!isdigit(*pc)) {
					/* there is a character in core limit */
					char_in_cname = 1;
					break;
				}
				pc++;
			}

			corelimit.rlim_max = RLIM_INFINITY;
			if (strcmp("unlimited", pbs_conf.pbs_core_limit) == 0) {
				corelimit.rlim_cur = RLIM_INFINITY;
				char_in_cname = 0;
			} else if (char_in_cname == 1)
				corelimit.rlim_cur = RLIM_INFINITY;
			else
				corelimit.rlim_cur =
					(rlim_t)atol(pbs_conf.pbs_core_limit);

			(void)setrlimit(RLIMIT_CORE, &corelimit);
		}
#endif  /* RLIMIT_CORE */
		if (getrlimit(RLIMIT_STACK, &rlimit) != -1) {
			if((rlimit.rlim_cur != RLIM_INFINITY) && (rlimit.rlim_cur < MIN_STACK_LIMIT)) {
				rlimit.rlim_cur = MIN_STACK_LIMIT;
				rlimit.rlim_max = MIN_STACK_LIMIT;
				if (setrlimit(RLIMIT_STACK, &rlimit) == -1) {
					char errmsg[] = "Stack limit setting failed";
					curerror = errno;
					log_err(curerror, __func__, errmsg);
					sprintf(log_buffer, "%s errno=%d", errmsg, curerror);
					log_record(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, (char *)__func__, log_buffer);
					exit(1);
				}
			}
		} else {
			char errmsg[] = "Getting current Stack limit failed";
			curerror = errno;
			log_err(curerror, __func__, errmsg);
			sprintf(log_buffer, "%s errno=%d", errmsg, curerror);
			log_record(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, (char *)__func__, log_buffer);
			exit(1);
		}
	}
#endif	/* DEBUG */

	/*we log here because the log wasn't open when the options were parsed*/
#ifdef  RLIMIT_CORE
	if (char_in_cname == 1)
		log_record(PBSEVENT_ERROR, PBS_EVENTCLASS_SCHED, LOG_WARNING,
			__func__, msg_corelimit);
#endif  /* RLIMIT_CORE */

	if (alarm_time) {
		snprintf(logbuf, sizeof(logbuf), "The -a option was given on the command line.  This is deprecated.  Please see the \'%s\' scheduler attribute", ATTR_sched_cycle_len);
		log_record(PBSEVENT_SCHED, PBS_EVENTCLASS_SCHED, LOG_NOTICE, "", logbuf);
	}

	if (gethostname(host, (sizeof(host) - 1)) == -1) {
		log_err(errno, __func__, "gethostname");
		die(0);
	}
	if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		log_err(errno, __func__, "socket");
		die(0);
	}
	if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR,
		(char *)&t, sizeof(t)) == -1) {
		log_err(errno, __func__, "setsockopt");
		die(0);
	}

	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(sched_port);
	saddr.sin_addr.s_addr = INADDR_ANY;
	if (bind(server_sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
		log_err(errno, __func__, "bind");
		die(0);
	}

	/*Initialize security library's internal data structures*/
	{
		int	csret;

		/* let Libsec do logging if part of PBS daemon code */
		p_cslog = log_err;

		if ((csret = CS_server_init()) != CS_SUCCESS) {
			sprintf(log_buffer,
				"Problem initializing security library (%d)", csret);
			log_err(-1, "pbs_sched", log_buffer);
			die(0);
		}
	}

	if (listen(server_sock, 5) < 0) {
		log_err(errno, __func__, "listen");
		die(0);
	}

	okclients = (pbs_net_t *)calloc(START_CLIENTS, sizeof(pbs_net_t));
	if (okclients == NULL) {
		log_err(errno, __func__, "Unable to allocate memory (malloc error)");
		die(0);
	}
	addclient("localhost");   /* who has permission to call MOM */
	addclient(host);
	if (pbs_conf.pbs_primary && pbs_conf.pbs_secondary) {
		/* Failover is configured when both primary and secondary are set. */
		addclient(pbs_conf.pbs_primary);
		addclient(pbs_conf.pbs_secondary);
	} else if (pbs_conf.pbs_server_host_name) {
		/* Failover is not configured, but PBS_SERVER_HOST_NAME is. */
		addclient(pbs_conf.pbs_server_host_name);
	}

	if (configfile) {
		if (read_config(configfile) != 0)
			die(0);
	}

	if ((c = are_we_primary()) == 1) {
		lockfds = open("sched.lock", O_CREAT|O_WRONLY, 0644);
	} else if (c == 0) {
		lockfds = open("sched.lock.secondary", O_CREAT|O_WRONLY, 0644);
	} else {
		log_err(-1, "pbs_sched", "neither primary or secondary server");
		exit(1);
	}
	if (lockfds < 0) {
		log_err(errno, __func__, "open lock file");
		exit(1);
	}

	fullresp(0);
	if (sigemptyset(&allsigs) == -1) {
		perror("sigemptyset");
		exit(1);
	}
	if (sigprocmask(SIG_SETMASK, &allsigs, NULL) == -1) {	/* unblock */
		perror("sigprocmask");
		exit(1);
	}
	act.sa_flags = 0;
	sigaddset(&allsigs, SIGHUP);    /* remember to block these */
	sigaddset(&allsigs, SIGINT);    /* during critical sections */
	sigaddset(&allsigs, SIGTERM);   /* so we don't get confused */
	sigaddset(&allsigs, SIGUSR1);
	act.sa_mask = allsigs;

	act.sa_handler = restart;       /* do a restart on SIGHUP */
	sigaction(SIGHUP, &act, NULL);

#ifdef PBS_UNDOLR_ENABLED	
	extern void catch_sigusr1(int);
	act.sa_handler = catch_sigusr1;
	sigaction(SIGUSR1, &act, NULL);
#endif

#ifdef NAS /* localmod 030 */
	act.sa_handler = soft_cycle_interrupt; /* do a cycle interrupt on */
					       /* SIGUSR1, subject to     */
					       /* configurable parameters */
	sigaction(SIGUSR1, &act, NULL);
	act.sa_handler = hard_cycle_interrupt; /* do a cycle interrupt on */
					       /* SIGUSR2                 */
	sigaction(SIGUSR2, &act, NULL);
#endif /* localmod 030 */

	act.sa_handler = die;           /* bite the biscuit for all following */
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);

	act.sa_handler = sigfunc_pipe;
	sigaction(SIGPIPE, &act, NULL);

	if (!opt_no_restart) {
		act.sa_handler = on_segv;
		sigaction(SIGSEGV, &act, NULL);
		sigaction(SIGBUS, &act, NULL);
	}

#ifndef	DEBUG
	if (stalone != 1) {
		if ((pid = fork()) == -1) {     /* error on fork */
			perror("fork");
			exit(1);
		}
		else if (pid > 0)               /* parent exits */
			exit(0);

		if (setsid() == -1) {
			perror("setsid");
			exit(1);
		}
	}
	lock_out(lockfds, F_WRLCK);
	freopen(dbfile, "a", stdout);
	setvbuf(stdout, NULL, _IOLBF, 0);
	dup2(fileno(stdout), fileno(stderr));
#else
	if (stalone != 1) {
		(void) sprintf(log_buffer, "Debug build does not fork.");
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				__func__, log_buffer);
	}
	lock_out(lockfds, F_WRLCK);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
#endif
	pid = getpid();
	daemon_protect(0, PBS_DAEMON_PROTECT_ON);
	freopen("/dev/null", "r", stdin);

	/* write schedulers pid into lockfile */
	(void)ftruncate(lockfds, (off_t)0);
	(void)sprintf(log_buffer, "%ld\n", (long)pid);
	(void)write(lockfds, log_buffer, strlen(log_buffer));

#ifdef _POSIX_MEMLOCK
	if (do_mlockall == 1) {
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			log_err(errno, __func__, "mlockall failed");
		}
	}
#endif	/* _POSIX_MEMLOCK */

	(void)sprintf(log_buffer, msg_startup1, PBS_VERSION, 0);
	log_event(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE,
		LOG_NOTICE, PBS_EVENTCLASS_SERVER, msg_daemonname, log_buffer);

	sprintf(log_buffer, "%s startup pid %ld", argv[0], (long)pid);
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, log_buffer);

	/*
	 *  Local initialization stuff
	 */
	if (schedinit(nthreads)) {
		(void) sprintf(log_buffer,
			"local initialization failed, terminating");
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				__func__, log_buffer);
		exit(1);
	}

	rpp_fd = -1;
	if (pbs_conf.pbs_use_tcp == 1) {
		fd_set selset;
		struct timeval tv;
		char *nodename = NULL;

		sprintf(log_buffer, "Out of memory");
		if (pbs_conf.pbs_leaf_name) {
			char *p;
			nodename = strdup(pbs_conf.pbs_leaf_name);

			/* reset pbs_leaf_name to only the first leaf name with port */
			p = strchr(pbs_conf.pbs_leaf_name, ','); /* keep only the first leaf name */
			if (p)
				*p = '\0';
			p = strchr(pbs_conf.pbs_leaf_name, ':'); /* cut out the port */
			if (p)
				*p = '\0';
		} else {
			nodename = get_all_ips(host, log_buffer, sizeof(log_buffer) - 1);
		}
		if (!nodename) {
			log_err(-1, "pbsd_main", log_buffer);
			fprintf(stderr, "%s\n", "Unable to determine TPP node name");
			return (1);
		}

		/* set tpp function pointers */
		set_tpp_funcs(log_tppmsg);

		if (pbs_conf.auth_method == AUTH_RESV_PORT || pbs_conf.auth_method == AUTH_GSS) {
			rc = set_tpp_config(&pbs_conf, &tpp_conf, nodename, sched_port,
								pbs_conf.pbs_leaf_routers, pbs_conf.pbs_use_compression,
								TPP_AUTH_RESV_PORT, NULL, NULL);
		} else {
			/* for all non-resv-port based authentication use a callback from TPP */
			rc = set_tpp_config(&pbs_conf, &tpp_conf, nodename, sched_port,
								pbs_conf.pbs_leaf_routers, pbs_conf.pbs_use_compression,
								TPP_AUTH_EXTERNAL, get_ext_auth_data, validate_ext_auth_data);
		}

		free(nodename);

		if (rc == -1) {
			fprintf(stderr, "Error setting TPP config\n");
			return -1;
		}

		if ((rpp_fd = tpp_init(&tpp_conf)) == -1) {
			fprintf(stderr, "rpp_init failed\n");
			return -1;
		}
		/*
		 * Wait for net to get restored, ie, app to connect to routers
		 */
		FD_ZERO(&selset);
		FD_SET(rpp_fd, &selset);
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		select(FD_SETSIZE, &selset, NULL, NULL, &tv);

		rpp_poll(); /* to clear off the read notification */
	} else {
		/* set rpp function pointers */
		set_rpp_funcs(log_rppfail);

		/* set a timeout for rpp_read operations */
		if (rpp_advise(RPP_ADVISE_TIMEOUT, &rpp_advise_timeout) != 0) {
			log_err(errno, __func__, "rpp_advise");
			die(0);
		}
	}

	/* Initialize cleanup lock */
	if (init_mutex_attr_recursive(&attr) == 0)
		die(0);

	pthread_mutex_init(&cleanup_lock, &attr);

	FD_ZERO(&fdset);
	for (go=1; go;) {
		int	cmd;

		/*
		 * with TPP, we don't need to drive rpp_io(), so
		 * no need to add it to be monitored
		 */
		if (pbs_conf.pbs_use_tcp == 0 && rpp_fd != -1)
			FD_SET(rpp_fd, &fdset);

		FD_SET(server_sock, &fdset);
		if (select(FD_SETSIZE, &fdset, NULL, NULL, NULL) == -1) {
			if (errno != EINTR) {
				log_err(errno, __func__, "select");
				die(0);
			}
			continue;
		}

#ifdef PBS_UNDOLR_ENABLED
		if (sigusr1_flag)
			undolr();
#endif

		if (pbs_conf.pbs_use_tcp == 0 && rpp_fd != -1 && FD_ISSET(rpp_fd, &fdset)) {
			if (rpp_io() == -1)
				log_err(errno, __func__, "rpp_io");
		}
		if (!FD_ISSET(server_sock, &fdset))
			continue;

		/* connector is set in server_connect() */
		cmd = server_command(&runjobid);

		if (connector >= 0) {
			if (update_svr) {
				/* update sched object attributes on server */
				update_svr_schedobj(connector, cmd, alarm_time);
				update_svr = 0;
			}

			if (sigprocmask(SIG_BLOCK, &allsigs, &oldsigs) == -1)
				log_err(errno, __func__, "sigprocmask(SIG_BLOCK)");

			/* Keep track of time to use in SIGSEGV handler */
#ifdef NAS /* localmod 031 */
			now = time(NULL);
			if (!opt_no_restart)
				segv_last_time = now;
			{
				strftime(log_buffer, sizeof(log_buffer),
					"%Y-%m-%d %H:%M:%S", localtime(&now));
				printf("%s Scheduler received command %d\n", log_buffer, cmd);
			}
#else
			if (!opt_no_restart)
				segv_last_time = time(NULL);

			DBPRT(("Scheduler received command %d\n", cmd));
#endif /* localmod 031 */

			if (schedule(cmd, connector, runjobid)) /* magic happens here */ {
				go = 0;
			}
			if (second_connection != -1) {
				close(second_connection);
				second_connection = -1;
			}

			if (server_disconnect(connector))
				connector = -1;

			if (runjobid != NULL) {
				free(runjobid);
				runjobid = NULL;
			}

			if (sigprocmask(SIG_SETMASK, &oldsigs, NULL) == -1)
				log_err(errno, __func__, "sigprocmask(SIG_SETMASK)");
		}
	}
	schedexit();

	sprintf(log_buffer, "%s normal finish pid %ld", argv[0], (long)pid);
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, log_buffer);
	lock_out(lockfds, F_UNLCK);

	(void)close(server_sock);
	exit(0);
}
