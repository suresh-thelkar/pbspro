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
 * @file	int_manager.c
 *
 * @brief
 * The function that underlies most of the job manipulation
 * routines...
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include "libpbs.h"
#include "pbs_ecl.h"


/**
 * @brief
 *	-send manager request and read reply.
 *
 * @param[in] c - communication handle
 * @param[in] function - req type
 * @param[in] command - command
 * @param[in] objtype - object type
 * @param[in] objname - object name
 * @param[in] aoplp - attribute list
 * @param[in] extend - extend string for req
 *
 * @return	int
 * @retval	0	success
 * @retval	!0	error
 *
 */
int
PBSD_manager(int c, int function, int command, int objtype, char *objname, struct attropl *aoplp, char *extend)
{
	int i;
	struct batch_reply *reply;
	int rc;

	/* initialize the thread context data, if not initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return pbs_errno;

	/* verify the object name if creating a new one */
	if (command == MGR_CMD_CREATE)
		if (pbs_verify_object_name(objtype, objname) != 0)
			return pbs_errno;

	/* now verify the attributes, if verification is enabled */
	if ((pbs_verify_attributes(c, function, objtype,
		command, aoplp)) != 0)
		return pbs_errno;

	/* lock pthread mutex here for this connection */
	/* blocking call, waits for mutex release */
	if (pbs_client_thread_lock_connection(c) != 0)
		return pbs_errno;

	/* Below reset would force the next connection request to select a random server */
	set_new_shard_context(c);

	/* send the manage request */
	i = PBSD_mgr_put(c, function, command, objtype, objname, aoplp, extend, PROT_TCP, NULL);
	if (i) {
		(void)pbs_client_thread_unlock_connection(c);
		return i;
	}

	/* read reply from stream into presentation element */
	reply = PBSD_rdrpy(c);
	PBSD_FreeReply(reply);

	rc = get_conn_errno(c);

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		return pbs_errno;

	return rc;
}
