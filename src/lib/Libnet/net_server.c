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
#include <pbs_config.h>   /* the master config generated by configure */

#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>

#ifndef WIN32
#include <stdlib.h>
#include <poll.h>
#include <sys/resource.h>
#endif

#include "portability.h"
#include "server_limits.h"
#include "pbs_ifl.h"
#include "net_connect.h"
#include "log.h"
#include "libsec.h"
#include "pbs_error.h"
#include "pbs_internal.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "svrfunc.h"
#include "tpp.h"

/**
 * @file	net_server.c
 */
/* Global Data (I wish I could make it private to the library, sigh, but
 * C don't support that scope of control.)
 *
 * This array of connection structures is used by the server to maintain
 * a record of the open I/O connections, it is indexed by the socket number.
 */

static conn_t **svr_conn;    /* list of pointers to connections indexed by the socket fd. List is dynamically allocated */
#define CONNS_ARRAY_INCREMENT	100 /* Increases this many more connection pointers when dynamically allocating memory for svr_conn */
static int conns_array_size = 0;  /* Size of the svr_conn list, initialized to 0 */
pbs_list_head svr_allconns; /* head of the linked list of active connections */

/*
 * The following data is private to this set of network interface routines.
 */
int	max_connection = -1;
static int	num_connections = 0;
static int	net_is_initialized = 0;
static void	*poll_context;  /* This is the context of the descriptors being polled */
void 	*priority_context;
static int      init_poll_context();  /* Initialize the tpp context */
static void	(*read_func[2])(int);
static int	(*ready_read_func)(conn_t *);
static char	logbuf[256];

/* Private function within this file */
static int 	conn_find_usable_index(int);
static int 	conn_find_actual_index(int);
static void 	accept_conn();
static void 	cleanup_conn(int);

/**
 * @brief
 * 	Makes the socket fd as index in the connection array usable and returns
 *  the socket fd.
 *
 * @par Functionality
 * 	Checks if the socket fd can be indexed into the connection array
 * 	If it is out of bounds, allocates enough slots for the connection array
 * 	and returns the index (the socket fd itself)
 *
 * @param[in] sd - The socket fd for the connection
 *
 * @return Error code
 * @retval 0 - Success
 * @retval -1 - Failure
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 */
static int
conn_find_usable_index(int sd)
{
	void *p;
	unsigned int new_conns_array_size = 0;

	if (sd < 0)
		return -1;

	if (sd >= conns_array_size) {
		new_conns_array_size = sd + CONNS_ARRAY_INCREMENT;
		p = realloc(svr_conn, new_conns_array_size * sizeof(conn_t *));
		if (!p)
			return -1;

		svr_conn = (conn_t **) (p);
		memset((svr_conn + conns_array_size), 0,
				(new_conns_array_size - conns_array_size) * sizeof(conn_t *));
		conns_array_size = new_conns_array_size;

	}
	return sd;
}

/**
 * @brief
 *	Returns the index of the connection for the socket fd provided
 *
 * @par Functionality
 *	Checks if the socket fd is valid and connection is available and
 *	returns the index to the connection in the array. The index is the
 *	socket identifier itself.
 *
 * @param[in] sd - The socket fd for the connection
 *
 * @return Error code
 * @retval  0 - Success
 * @retval -1 - Failure
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
static int
conn_find_actual_index(int sd)
{
	if (sd >= 0 && sd < conns_array_size) {
		if (svr_conn[sd])
			return sd;
	}
	return -1;
}

/**
 * @brief
 *	Given a socket fd, this function provides the handle to the connection
 *
 * @par Functionality
 *	Checks if the socket fd has a valid connection and if present returns
 *	pointer to the connection structure
 *
 * @param[in] sock - The socket fd for the connection
 *
 * @return Error code
 * @retval conn_t * - Success
 * @retval NULL - Failure
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
conn_t *
get_conn(int sd)
{
	int idx = conn_find_actual_index(sd);
	if (idx < 0)
		return NULL;

	return svr_conn[idx];
}

/**
 * @brief
 *	initialize the connection.
 *
 */
void
connection_init(void) {
	conn_t *cp = NULL;

	if(!svr_allconns.ll_next) {
		CLEAR_HEAD(svr_allconns);
		return;
	}

	cp = (conn_t *)GET_NEXT(svr_allconns);
	while(cp) {
		int sock = cp->cn_sock;
		cp = GET_NEXT(cp->cn_link);
		close_conn(sock);
	}
	CLEAR_HEAD(svr_allconns);
}

/**
 * @brief
 * 	init_network - initialize the network interface
 *
 * @par	Functionality:
 *    	Normal call, port > 0
 *	allocate a socket and bind it to the service port,
 *	add the socket to the readset/pollfds for select()/poll(),
 *	add the socket to the connection structure and set the
 *	processing function to accept_conn()
 *    	Special call, port == 0
 *	Only initial the connection table and poll pollfds or select readset.
 *
 * @param[in] port - port number
 * @param[in] readfunc - callback function which indicates type of request
 *
 * @return	Error code
 * @retval	0	success
 * @retval	-1	error
 */
int
init_network(unsigned int port)
{
	int			i;
	size_t		j;
	int 		sd;
#ifdef WIN32
	struct  linger		li;
#endif
	struct sockaddr_in	socname;

	if (port == 0)
		return 0;	/* that all for the special init only call */

	sd = socket(AF_INET, SOCK_STREAM, 0);
	if (sd < 0) {
#ifdef WIN32
		errno = WSAGetLastError();
#endif
		log_err(errno, __func__, "socket() failed");
		return (-1);
	}

	i = 1;
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (char *)&i, sizeof(i));

#ifdef WIN32
	li.l_onoff = 1;
	li.l_linger = 5;
	setsockopt(sd, SOL_SOCKET, SO_LINGER, (char *)&li, sizeof(li));
#endif

	/* name that socket "in three notes" */

	j = sizeof(socname);
	memset((void *)&socname, 0, j);
	socname.sin_port= htons((unsigned short)port);
	socname.sin_addr.s_addr = INADDR_ANY;
	socname.sin_family = AF_INET;
	if (bind(sd, (struct sockaddr *)&socname, sizeof(socname)) < 0) {
#ifdef WIN32
		errno = WSAGetLastError();
		(void)closesocket(sd);
#else
		(void)close(sd);
#endif
		log_err(errno, __func__ , "bind failed");
		return (-1);
	}
	return sd;
}

/**
 * @brief
 * 	init_network_add - initialize the network interface
 * 	and save the routine which should do the reading on connections.
 *
 * @param[in] sd - socket descriptor
 * @param[in] readfunc - routine which should do the reading on connections
 *
 * @return	Error code
 * @retval	 0	success
 * @retval	-1	error
 */
int
init_network_add(int sd, int (*readyreadfunc)(conn_t *), void (*readfunc)(int))
{
	static int		initialized = 0;
	enum conn_type  type;

	if (initialized == 0) {
		connection_init();
		if (init_poll_context() < 0)
			return (-1);
		type = Primary;
	} else if (initialized == 1)
		type = Secondary;
	else
		return (-1);  /* too many main connections */

	net_is_initialized = 1;  /* flag that net stuff is initialized */


	if(sd == -1)
		return -1;

	ready_read_func = readyreadfunc;

	/* for normal calls ...						*/
	/* save the routine which should do the reading on connections	*/
	/* accepted from the parent socket				*/
	read_func[initialized++] = readfunc;

	/* record socket in connection structure and select set
	 *
	 * remark: passing 0 as port value causing entry's member
	 *         cn_authen to have bit PBS_NET_CONN_PRIVIL set
	 */
	if (add_conn(sd, type, (pbs_net_t)0, 0, NULL, accept_conn) == NULL) {
#ifdef WIN32
		errno = WSAGetLastError();
		(void)closesocket(sd);
#else
		(void)close(sd);
#endif
		log_err(errno, __func__, "add_conn failed");
		return -1;
	}

	/* start listening for connections */
	if (listen(sd, 256) < 0) {
		log_err(errno, __func__ , "listen failed");
#ifdef WIN32
		errno = WSAGetLastError();
		(void)closesocket(sd);
#else
		(void)close(sd);
#endif
		return (-1);
	}

	return 0;
}

/**
 * @brief
 *	checks for any connection timeout.
 *
 */
void
connection_idlecheck(void)
{
	static time_t last_checked = (time_t) 0;
	time_t now;
	conn_t *next_cp = (conn_t *) GET_NEXT(svr_allconns);

	now = time(NULL);
	if (now - last_checked < 60)
		return;

	/* have any connections timed out ?? */
	while (next_cp) {
		u_long ipaddr;
		conn_t *cp = next_cp;
		next_cp = GET_NEXT(cp->cn_link);

		if (cp->cn_active != FromClientDIS)
			continue;
		if ((now - cp->cn_lasttime) <= PBS_NET_MAXCONNECTIDLE)
			continue;
		if (cp->cn_authen & PBS_NET_CONN_NOTIMEOUT)
			continue; /* do not time-out this connection */

		ipaddr = cp->cn_addr;
		snprintf(logbuf, sizeof(logbuf),
				"timeout connection from %lu.%lu.%lu.%lu",
				(ipaddr & 0xff000000) >> 24, (ipaddr & 0x00ff0000) >> 16,
				(ipaddr & 0x0000ff00) >> 8, (ipaddr & 0x000000ff));
		log_err(0, __func__, logbuf);
		close_conn(cp->cn_sock);
	}
	last_checked = now;
}

/**
 * @brief
 *	engage_authentication - Use the security library interface to
 * 	engage the appropriate connection authentication.
 *
 * @param[in] pconn  pointer to a "conn_t" variable
 *
 * @return Error code
 * @return	 0  successful
 * @retval	-1 unsuccessful
 *
 * @par Remark:
 *	If the authentication fails, messages are logged to
 *	the server's log file and the connection's security
 *	information is closed out (freed).
 */
static int
engage_authentication(conn_t *pconn)
{
	int ret;
	int sd;
	char ebuf[PBS_MAXHOSTNAME + 1] = {'\0'};
	char *msgbuf;

	if (pconn == NULL || (sd = pconn->cn_sock) <0) {
		log_err(-1, __func__, "bad arguments, unable to authenticate");
		return (-1);
	}

	if ((ret = CS_server_auth(sd)) == CS_SUCCESS) {
		pconn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
		return (0);
	}

	if (ret == CS_AUTH_CHECK_PORT) {
		/*dealing with STD security's  "equivalent of"  CS_sever_auth*/
		if (pconn->cn_authen & PBS_NET_CONN_FROM_PRIVIL)
			pconn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
		return (0);
	}

	(void)get_connecthost(sd, ebuf, sizeof(ebuf));

	pbs_asprintf(&msgbuf,
		"unable to authenticate connection from (%s:%d)",
		ebuf, pconn->cn_port);
	log_err(-1, __func__ , msgbuf);
	free(msgbuf);

	return (-1);
}

/*
 * @brief
 * process_socket  -  The static method processes given socket and
 *                    engages the appropriate connection authentication.
 *
 * @param[in]   sock 	- socket fd to process
 *
 * @retval	-1 for failure
 * @retval	0  for success
 *
 */
static int
process_socket(int sock)
{
	int idx = conn_find_actual_index(sock);
	if (idx < 0) {
		return -1;
	}
	svr_conn[idx]->cn_lasttime = time(NULL);
	if ((svr_conn[idx]->cn_active != Primary) &&
		(svr_conn[idx]->cn_active != TppComm) &&
		(svr_conn[idx]->cn_active != Secondary)) {
		if (!(svr_conn[idx]->cn_authen & PBS_NET_CONN_AUTHENTICATED)) {
			if (engage_authentication(svr_conn[idx]) == -1) {
				close_conn(sock);
				return -1;
			}
		}
	}

	if (svr_conn[idx]->cn_ready_func != NULL) {
		int ret = 0;
		ret = svr_conn[idx]->cn_ready_func(svr_conn[idx]);
		if (ret == -1) {
			close_conn(sock);
			return -1;
		} else if (ret == 0) {
			/* no data for cn_func */
			return 0;
		}
		/* EOF will be handled in cn_func */
	}
	svr_conn[idx]->cn_func(svr_conn[idx]->cn_sock);
	return 0;
}

/**
 * @brief
 *	Waits for events on a set of sockets and calls processing function
 *	corresponding to the socket fd.
 *
 * @par Functionality
 * wait_request - wait for a request (socket with data to read)
 *	This routine does a tpp_em_wait - which internally does poll()/epoll()/select()
 *	based on the platform on the socket fds.
 *	It loops through the socket fds which has events on them and the processing
 *	routine associated with the socket is invoked.
 *
 * @param[in] waittime - Timeout for tpp_em_wait (poll)
 * @param[in] priority_context - context consists of high priority socket connections
 *
 * @return Error code
 * @retval 0 - Success
 * @retval -1 - Failure
 *
 * @par Side Effects:
 *	None
 *
 * @par MT-safe: No
 *
 */
int
wait_request(time_t waittime, void *priority_context)
{
	int nfds;
	int pnfds;
	int i;
	em_event_t *events;
	int err;
	int prio_sock_processed;
	int em_fd;
	int em_pfd;
	int timeout = (int) (waittime * 1000); /* milli seconds */
	/* Platform specific declarations */

#ifndef WIN32
	sigset_t pendingsigs;
	sigset_t emptyset;
	extern sigset_t allsigs;

	/* wait after unblocking signals in an atomic call */
	sigemptyset(&emptyset);
	nfds = tpp_em_pwait(poll_context, &events, timeout, &emptyset);
	err = errno;
#else
	errno = 0;
	nfds = tpp_em_wait(poll_context, &events, timeout);
	err = errno;
#endif /* WIN32 */
	if (nfds < 0) {
		if (!(err == EINTR || err == EAGAIN || err == 0)) {
			snprintf(logbuf, sizeof(logbuf), " tpp_em_wait() error, errno=%d", err);
			log_err(err, __func__, logbuf);
			return (-1);
		}
	} else {
		prio_sock_processed = 0;
		if (priority_context) {
			em_event_t *pevents;
			timeout = 0;
#ifndef WIN32
        		/* wait after unblocking signals in an atomic call */
        		sigemptyset(&emptyset);
        		pnfds = tpp_em_pwait(priority_context, &pevents, timeout, &emptyset);
        		err = errno;
#else
        		pnfds = tpp_em_wait(priority_context, &pevents, timeout);
        		err = errno;
#endif /* WIN32 */
                	for (i = 0; i < pnfds; i++) {
                        	em_pfd = EM_GET_FD(pevents, i);
				log_event(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER,
                                        LOG_DEBUG, __func__, "processing priority socket");
				if (process_socket(em_pfd) == -1) {
                                	log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER,
                                        	LOG_DEBUG, __func__, "process priority socket failed");
                        	} else {
					prio_sock_processed = 1;
				}
			}
		}

		for (i = 0; i < nfds; i++) {
			em_fd = EM_GET_FD(events, i);
#ifndef WIN32
			/* If there is any of the following signals pending, allow a small window to handle the signal */
			if( sigpending( &pendingsigs ) == 0) {
				if (sigismember(&pendingsigs, SIGCHLD)
					|| sigismember(&pendingsigs, SIGHUP)
					|| sigismember(&pendingsigs, SIGINT)
					|| sigismember(&pendingsigs, SIGTERM)) {

					if (sigprocmask(SIG_UNBLOCK, &allsigs, NULL) == -1)
						log_err(errno, __func__, "sigprocmask(UNBLOCK)");
					if (sigprocmask(SIG_BLOCK, &allsigs, NULL) == -1)
						log_err(errno, __func__, "sigprocmask(BLOCK)");

					return (0);
				}
			}
#endif
			if (prio_sock_processed) {
				int idx = conn_find_actual_index(em_fd);
				if (idx < 0)
					continue;
				if (svr_conn[idx]->cn_prio_flag == 1)
					continue;
			}
			if (process_socket(em_fd) == -1) {
				log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER,
					LOG_DEBUG, __func__, "process socket failed");
			}
		}
	}

#ifndef WIN32
	connection_idlecheck();
#endif

	return (0);
}

/**
 * @brief
 *	accept request for new connection
 *	this routine is normally associated with the main socket,
 *	requests for connection on the socket are accepted and
 *	the new socket is added to the select set and the connection
 *	structure - the processing routine is set to the external
 *	function: process_request(socket)Makes a PBS_BATCH_Connect request to
 *	'server'.
 *
 * @param[in]   sd - main socket with connection request pending
 *
 * @return void
 *
 */
static void
accept_conn(int sd)
{
	int newsock;
	struct sockaddr_in from;
	pbs_socklen_t fromsize;

	int idx = conn_find_actual_index(sd);
	if (idx == -1)
		return;

	/* update last-time of main socket */

	svr_conn[idx]->cn_lasttime = time(NULL);

	fromsize = sizeof(from);
	newsock = accept(sd, (struct sockaddr *)&from, &fromsize);
	if (newsock == -1) {
#ifdef WIN32
		errno = WSAGetLastError();
#endif
		log_err(errno, __func__ , "accept failed");
		return;
	}

	/*
	 * Disable Nagle's algorithm on this TCP connection to server.
	 * Nagle's algorithm is hurting cmd-server communication.
	 */
	if (set_nodelay(newsock) == -1) {
		log_err(errno, __func__, "set_nodelay failed");
		(void)close(newsock);
		return;		/* set_nodelay failed */
	}

	/* add the new socket to the select set and connection structure */

	(void)add_conn(newsock, FromClientDIS,
		(pbs_net_t)ntohl(from.sin_addr.s_addr),
		(unsigned int)ntohs(from.sin_port),
		ready_read_func,
		read_func[(int)svr_conn[idx]->cn_active]);
}

/**
 * @brief
 *      add_conn - add a connection to the svr_conn array.
 *
 * @par Functionality:
 *	wrapper function to add_conn_priority called with priority_flag set to 0
 *
 * @param[in]   sd: socket descriptor
 * @param[in]   type: (enumb conn_type)
 * @param[in]   addr: host IP address in host byte order
 * @param[in]   port: port number in host byte order
 * @param[in]   func: pointer to function to call when data is ready to read
 *
 * @return      pointer to conn_t
 * @retval      NULL - failure.
 */
conn_t *
add_conn(int sd, enum conn_type type, pbs_net_t addr, unsigned int port, int (*ready_func)(conn_t *), void (*func)(int))
{
	return add_conn_priority(sd, type, addr, port, ready_func, func, 0);
}

/**
 * @brief
 *	add_conn_priority - add a connection to the svr_conn array.
 *
 * @par Functionality:
 *	Find an empty slot in the connection table.  This is done by hashing
 *	on the socket (file descriptor).  On Windows, this is not a small
 *	interger.  The socket is then added to the poll/select set.
 *
 * @param[in]	sd: socket descriptor
 * @param[in]	type: (enumb conn_type)
 * @param[in]	addr: host IP address in host byte order
 * @param[in]	port: port number in host byte order
 * @param[in]	func: pointer to function to call when data is ready to read
 * @param[in]	priority_flag: 1 if connection is priority else 0
 *
 * @return	pointer to conn_t
 * @retval	NULL - failure.
 */
conn_t *
add_conn_priority(int sd, enum conn_type type, pbs_net_t addr, unsigned int port, int (*ready_func)(conn_t *), void (*func)(int), int priority_flag)
{
	int 	idx;
	conn_t *conn;

	idx = conn_find_usable_index(sd);
	if (idx == -1)
		return NULL;

	conn = (conn_t *) calloc(1, sizeof(conn_t));
	if (!conn) {
		return NULL;
	}

	conn->cn_sock = sd;
	conn->cn_active = type;
	conn->cn_addr = addr;
	conn->cn_port = (unsigned short) port;
	conn->cn_lasttime = time(NULL);
	conn->cn_ready_func = ready_func;
	conn->cn_func = func;
	conn->cn_oncl = 0;
	conn->cn_authen = 0;
	conn->cn_prio_flag = 0;
	conn->cn_auth_config = NULL;
	conn->is_sched_conn = 0;

	num_connections++;

	if (port < IPPORT_RESERVED)
		conn->cn_authen |= PBS_NET_CONN_FROM_PRIVIL;

	svr_conn[idx] = conn;

	/* Add to list of connections */
	CLEAR_LINK(conn->cn_link);
	append_link(&svr_allconns, &conn->cn_link, conn);

	if (tpp_em_add_fd(poll_context, sd, EM_IN | EM_HUP | EM_ERR) < 0) {
		int err = errno;
		snprintf(logbuf, sizeof(logbuf),
			"could not add socket %d to the poll list", sd);
		log_err(err, __func__, logbuf);
		close_conn(sd);
		return NULL;
	}
	if (priority_flag) {
		conn->cn_prio_flag = 1;
		if (tpp_em_add_fd(priority_context, sd, EM_IN | EM_HUP | EM_ERR) < 0) {
			int err = errno;
			snprintf(logbuf, sizeof(logbuf),
				"could not add socket %d to the priority poll list", sd);
			log_err(err, __func__, logbuf);
			close_conn(sd);
			return NULL;
        	}
	}

	return svr_conn[idx];
}

/**
 * @brief
 *	add_conn_data - add some data to a connection
 *
 * @par Functionality:
 *	This function identifies the connection based on index provided
 *  and sets cn_data value
 *
 * @param[in]	sd: socket descriptor
 * @param[in]	data: void pointer to the data
 * @param[in]	func: pointer to function to call when connection is to be deleted
 *
 * @return Connection index
 * @return  0 if the connection index is valid
 * @return -1 if the connection index is invalid
 */
int
add_conn_data(int sd, void * data)
{
	int idx = conn_find_actual_index(sd);
	if (idx < 0) {
		return -1;
	}

	svr_conn[idx]->cn_data = data;
	return 0;
}

/**
 * @brief
 *	get_conn_data - get cn_data from the connection
 *
 * @par Functionality:
 *	This function identifies the connection based on index provided
 *  and sets cn_data value
 *
 * @param[in]	sd: socket descriptor
 *
 * @return pointer to the connection related data
 * @retval - Null, if sd not found
 *
 */
void *
get_conn_data(int sd)
{
	int idx = conn_find_actual_index(sd);
	if (idx < 0) {
		snprintf(logbuf, sizeof(logbuf), "could not find index for the socket %d", sd);
		log_err(-1, __func__, logbuf);
		return NULL;
	}

	return svr_conn[idx]->cn_data;
}

/**
 * @brief
 *	close_conn - close a connection in the svr_conn array.
 *
 * @par Functionality:
 *	Validate the socket (file descriptor).  For Unix/Linux it is a small
 *	integer less than the max number of connections.  For Windows, it is
 *	a valid socket value (not equal to INVALID_SOCKET).
 *	The entry in the table corresponding to the socket is found.
 *	If the entry is for a network socket (not a pipe), it is closed via
 *	CS_close_socket() which typically just does a close; for Windows,
 *	closesocket() is used.
 *	For a pipe (not a network socket), plain close() is called.
 *	If there is a function to be called, see cn_oncl table entry, that
 *	function is called.
 *	The table entry is cleared and marked "Idle" meaning it is free for
 *	reuse.
 *
 * @param[in]	sock: socket or file descriptor
 *
 */
void
close_conn(int sd)
{
	int idx;

#ifdef WIN32
	if ((sd == INVALID_SOCKET))
#else
	if ((sd < 0))
#endif
		return;

	idx = conn_find_actual_index(sd);
	if (idx == -1)
		return;

	if (svr_conn[idx]->cn_active != ChildPipe) {
		dis_destroy_chan(sd);
	}

	if (svr_conn[idx]->cn_active != ChildPipe) {
		if (CS_close_socket(sd) != CS_SUCCESS) {
			char ebuf[PBS_MAXHOSTNAME + 1] = {'\0'};
			char *msgbuf;

			(void)get_connecthost(sd, ebuf, sizeof(ebuf));
			pbs_asprintf(&msgbuf,
				"problem closing security context for %s:%d",
				ebuf, svr_conn[idx]->cn_port);
			log_err(-1, __func__ , msgbuf);
			free(msgbuf);
		}

		/* if there is a function to call on close, do it */
		if (svr_conn[idx]->cn_oncl != 0)
			svr_conn[idx]->cn_oncl(sd);

		cleanup_conn(idx);
		num_connections--;

		CLOSESOCKET(sd);
	} else {
		/* if there is a function to call on close, do it */
		if (svr_conn[idx]->cn_oncl != 0)
			svr_conn[idx]->cn_oncl(sd);

		cleanup_conn(idx);
		num_connections--;
		CLOSESOCKET(sd); /* pipe so use normal close */
	}
}

/**
 * @brief
 *	cleanup_conn - reset a connection entry in the svr_conn array.
 *
 * @par Functionality:
 * 	Given an index within the svr_conn array, reset all fields back to
 * 	their defaults and clear any select/poll related flags.
 *
 * @param[in]	idx: index of the svr_conn entry
 *
 */
static void
cleanup_conn(int idx)
{
	if (tpp_em_del_fd(poll_context, svr_conn[idx]->cn_sock) < 0) {
		int err = errno;
		snprintf(logbuf, sizeof(logbuf),
			"could not remove socket %d from poll list", svr_conn[idx]->cn_sock);
		log_err(err, __func__, logbuf);
	}
	if (svr_conn[idx]->cn_prio_flag)
	{
		if (tpp_em_del_fd(priority_context, svr_conn[idx]->cn_sock) < 0) {
			int err = errno;
			snprintf(logbuf, sizeof(logbuf),
				"could not remove socket %d from priority poll list", svr_conn[idx]->cn_sock);
			log_err(err, __func__, logbuf);
        	}
	}

	/* Remove connection from the linked list */
	delete_link(&svr_conn[idx]->cn_link);

	svr_conn[idx]->cn_physhost[0] = '\0';
	if (svr_conn[idx]->cn_credid) {
		free(svr_conn[idx]->cn_credid);
		svr_conn[idx]->cn_credid = NULL;
	}

	if (svr_conn[idx]->cn_auth_config) {
		free_auth_config(svr_conn[idx]->cn_auth_config);
		svr_conn[idx]->cn_auth_config = NULL;
	}

	/* Free the connection memory */
	free(svr_conn[idx]);
	svr_conn[idx] = NULL;
}

/**
 * @brief
 * 	net_close - close all network connections but the one specified,
 *	if called with impossible socket number (-1), all will be closed.
 *	This function is typically called when a server is closing down and
 *	when it is forking a child.
 *
 * @par	Note:
 *	We clear the cn_oncl field in the connection table to prevent any
 *	"special on close" functions from being called.
 *
 * @param[in] but - socket number to leave open
 *
 * @par	Note:
 *	free() the dynamically allocated data
 *
 */
void
net_close(int but)
{
	conn_t *cp = NULL;

	if (net_is_initialized == 0)
		return;

	cp = (conn_t *)GET_NEXT(svr_allconns);
	while(cp) {
		int sock = cp->cn_sock;
		cp = GET_NEXT(cp->cn_link);
		if(sock != but) {
			if (svr_conn[sock]->cn_oncl != NULL)
				svr_conn[sock]->cn_oncl = NULL;
			close_conn(sock);
			destroy_connection(sock);
		}
	}

	if (but == -1) {
		tpp_em_destroy(poll_context);
		tpp_em_destroy(priority_context);
		net_is_initialized = 0;
	}
}

/**
 * @brief
 * 	get_connectaddr - return address of host connected via the socket
 *	This is in host order.
 *
 * @param[in] sd - socket descriptor
 *
 * @return address of host
 * @retval !0		success
 * @retval 0		error
 *
 */
pbs_net_t
get_connectaddr(int sd)
{
	int idx = conn_find_actual_index(sd);
	if (idx == -1)
		return (0);

	return (svr_conn[idx]->cn_addr);
}

/**
 * @brief
 * 	get_connecthost - return name of host connected via the socket
 *
 * @param[in] sd - socket descriptor
 * @param[out] namebuf - buffer to hold host name
 * @param[out] size - size of buffer
 *
 * @return Error code
 * @retval	0	success
 * @retval -1	error
 *
 */
int
get_connecthost(int sd, char *namebuf, int size)
{
	int             i;
	struct hostent *phe;
	struct in_addr  addr;
	int	namesize = 0;
#if !defined(WIN32)
	char	dst[INET_ADDRSTRLEN + 1]; /* for inet_ntop */
#endif

	int	idx = conn_find_actual_index(sd);
	if (idx == -1)
		return (-1);

	size--;
	addr.s_addr = htonl(svr_conn[idx]->cn_addr);

	if ((phe = gethostbyaddr((char *) &addr, sizeof(struct in_addr),
		AF_INET)) == NULL) {
#if defined(WIN32)
			/* inet_ntoa is thread-safe on windows */
			(void)strcpy(namebuf, inet_ntoa(addr));
#else
			(void)strcpy(namebuf,
				inet_ntop(AF_INET, (void *) &addr, dst, INET_ADDRSTRLEN));
#endif
	} else {
		namesize = strlen(phe->h_name);
		for (i=0; i<size; i++) {
			*(namebuf+i) = tolower((int)*(phe->h_name+i));
			if (*(phe->h_name+i) == '\0')
				break;
		}
		*(namebuf+size) = '\0';
	}
	if (namesize > size)
		return (-1);

	return (0);
}

/**
 * @brief
 *	Initialize maximum connetions.
 *	Init the pollset i.e. socket descriptors to be polled.
 *
 * @par Functionality:
 *	For select() in WIN32, max_connection is decided based on the
 *	FD_SETSIZE (max vaue, select() can handle) but for Unix variants
 *	that is decided by getrlimit() or getdtablesize(). For poll(),
 *	allocate memory for pollfds[] and init the table.
 *
 * @par Linkage scope:
 *	static (local)
 *
 * @return Error code
 * @retval	 0 for success
 * @retval	-1 for failure
 *
 * @par Reentrancy
 *	MT-unsafe
 *
 */
static int
init_poll_context(void)
{
#ifdef WIN32
	int sd_dummy;
	max_connection = FD_SETSIZE;
#else
	int idx;
	int nfiles;
	struct rlimit rl;

	idx = getrlimit(RLIMIT_NOFILE, &rl);
	if ((idx == 0) && (rl.rlim_cur != RLIM_INFINITY))
		nfiles = rl.rlim_cur;
	else
		nfiles = getdtablesize();

	if ((nfiles > 0))
		max_connection = nfiles;

#endif
	DBPRT(("#init_poll_context: initializing poll_context for %d", max_connection))
	poll_context = tpp_em_init(max_connection);
	if (poll_context == NULL) {
		log_err(errno, __func__, "could not initialize poll_context");
		return (-1);
	}
	priority_context = tpp_em_init(max_connection);
	if (priority_context == NULL) {
		log_err(errno, __func__, "could not initialize priority_context");
		return (-1);
	}
#ifdef WIN32
	/* set a dummy fd in the read set so that	*/
	/* select() does not return WSAEINVAL 		*/
	sd_dummy = socket(AF_INET, SOCK_STREAM, 0);
	if (sd_dummy < 0) {
		errno = WSAGetLastError();
		log_err(errno, __func__, "socket() failed");
		return -1;
	}
	if ((tpp_em_add_fd(poll_context, sd_dummy, EM_IN) == -1)) {
		int err = errno;
		snprintf(logbuf, sizeof(logbuf),
			"Could not add socket %d to the read set", sd_dummy);
		log_err(err, __func__, logbuf);
		CLOSESOCKET(sd_dummy);
		return -1;
	}
	if ((tpp_em_add_fd(priority_context, sd_dummy, EM_IN) == -1)) {
		int err = errno;
		snprintf(logbuf, sizeof(logbuf),
			"Could not add socket %d to the read set for priority socket", sd_dummy);
		log_err(err, __func__, logbuf);
		CLOSESOCKET(sd_dummy);
		return -1;
	}
#endif /* WIN32 */

	return 0;
}

/**
 * @brief
 *	Close the socket descriptor.
 *
 * @param[in]   sd: socket descriptor.
 *
 */
void
close_socket(int sd) {
#ifdef WIN32
	(void)closesocket(sd);
#else
	(void) close(sd);
#endif
}
