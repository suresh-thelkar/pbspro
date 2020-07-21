/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/** @file	int_status.c
 * @brief
 * The function that underlies all the status requests
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "libpbs.h"
#include "pbs_ecl.h"
#include "libutil.h"
#include "attribute.h"


static struct batch_status *alloc_bs();

enum state { TRANSIT_STATE,
	     QUEUE_STATE,
	     HELD_STATE,
	     WAIT_STATE,
	     RUN_STATE,
	     EXITING_STATE,
	     BEGUN_STATE,
	     MAX_STATE };

static char *statename[] = { "Transit", "Queued", "Held", "Waiting",
		"Running", "Exiting", "Begun"};

/**
 * @brief
 *	decoded state attribute to count array
 *
 * @param[in] string - string holding state of job
 * @param[out] count    - count array having job per state
 *
 */
static void
decode_states(char *string, long *count)
{
	char *c, *s;
	long *d;
	int i;

	c = string;
	while (isspace(*c) && *c != '\0')
		c++;
	while (c && *c != '\0') {
		s = c;
		if ((c = strchr(s, ':')) == NULL)
			break;
		*c = '\0';
		d = NULL;

		for (i = 0; i < MAX_STATE; i++) {
			if (strcmp(s, statename[i]) == 0) {
				d = &count[i];
				break;
			}
		}
		*c = ':';
		c++;
		if (d) {
			s = c;
			*d = strtol(s, &c, 10);
			if (*c != '\0')
				c++;
		} else {
			while (*c != ' ' && *c != '\0')
				c++;
		}
	}
}

static void
encode_states(char **val, long *cur, long *nxt)
{
	int index;
	int len = 0;
	char buf[256];

	buf[0] = '\0';
	for (index = 0; index < MAX_STATE; index++) {
		len += sprintf(buf + len, "%s:%ld ", statename[index],
			       cur[index] + nxt[index]);
	}
	free(*val);
	*val = strdup(buf);
}

/**
 * @brief
 *	get one of the available connection from multisvr sd
 *
 */
int
get_available_conn(svr_conn_t *svr_connections)
{
	int i;

	for (i = 0; i < get_num_servers(); i++)
		if (svr_connections[i].state == SVR_CONN_STATE_CONNECTED)
			return svr_connections[i].sd;

	return -1;
}

/**
 * @brief
 *	get random server sd - It will choose a random sd from available no of servers.
 *
 */
int
random_srv_conn(svr_conn_t *svr_connections)
{
	int ind = 0;

	ind =  rand_num() % get_num_servers();

	if (svr_connections[ind].state == SVR_CONN_STATE_CONNECTED)
		return svr_connections[ind].sd;
		
	return get_available_conn(svr_connections);
}

/**
 * @brief
 *	-wrapper function for PBSD_status_put which sends
 *	status batch request
 *
 * @param[in] c - socket descriptor
 * @param[in] function - request type
 * @param[in] id - object id
 * @param[in] attrib - pointer to attribute list
 * @param[in] extend - extention string for req encode
 *
 * @return	structure handle
 * @retval 	pointer to batch status on SUCCESS
 * @retval 	NULL on failure
 *
 */
struct batch_status *
PBSD_status(int c, int function, char *objid, struct attrl *attrib, char *extend, svr_conn_t *svr_conns)
{
	int rc;
	struct batch_status *PBSD_status_get(int c, int type, svr_conn_t *svr_conns);

	/* send the status request */

	if (objid == NULL)
		objid = "";	/* set to null string for encoding */

	rc = PBSD_status_put(c, function, objid, attrib, extend, PROT_TCP, NULL);
	if (rc) {
		return NULL;
	}

	/* get the status reply */
	return (PBSD_status_get(c, function, svr_conns));
}

static void
aggr_job_ct(struct batch_status *cur, struct batch_status *nxt)
{
	long cur_st_ct[MAX_STATE] = {0};
	long nxt_st_ct[MAX_STATE] = {0};
	struct attrl *a = NULL;
	struct attrl *b = NULL;
	char *tot_jobs_attr = NULL;
	long tot_jobs = 0;
	char *endp;
	int found;

	if (!cur || !nxt)
		return;

	for (a = cur->attribs, found = 0; a; a = a->next) {
		if (a->name && strcmp(a->name, ATTR_count) == 0) {
			decode_states(a->value, cur_st_ct);
			found++;
		} else if (a->name && strcmp(a->name, ATTR_total) == 0) {
			tot_jobs_attr = a->value;
			tot_jobs += strtol(a->value, &endp, 10);
			found++;
		}
		if (found == 2)
			break;
	}
	for (b = nxt->attribs, found = 0; b; b = b->next) {
		if (b->name && strcmp(b->name, ATTR_count) == 0) {
			decode_states(b->value, nxt_st_ct);
			found++;
		} else if (b->name && strcmp(b->name, ATTR_total) == 0) {
			tot_jobs += strtol(b->value, &endp, 10);
			found++;
		}
		if (found == 2)
			break;
	}

	if (a && b)
		encode_states(&a->value, cur_st_ct, nxt_st_ct);
	if (tot_jobs_attr)
		sprintf(tot_jobs_attr, "%ld", tot_jobs);
}

#define DOUBLE 0
#define LONG 1
#define STRING 2
#define SIZE 3

static void
assess_type(char *val, int *type, double *val_double, long *val_long)
{
	char *pc;

	if (strchr(val, '.')) {
		if ((*val_double = strtod(val, &pc)))
			*type = DOUBLE;
		else if (pc && *pc != '\0')
			*type = STRING;
	} else {
		if ((*val_long = strtol(val, &pc, 10))) {
			*type = LONG;
			if (pc && (!strcasecmp(pc, "kb") || !strcasecmp(pc, "mb") || !strcasecmp(pc, "gb") ||
			    !strcasecmp(pc, "tb") || !strcasecmp(pc, "pb")))
				*type = SIZE;
		} else if (pc && *pc != '\0')
			*type = STRING;
	}
}

struct attrl_holder {
	struct attrl *atr_list;
	struct attrl_holder *next;
};

static void
accumulate_values(struct attrl_holder *a, struct attrl *b, struct attrl *orig)
{
	double val_double = 0;
	long val_long = 0;
	char *pc;
	int type = -1;
	struct attrl_holder *itr;
	struct attribute attr;
	struct attribute new;
	struct attrl *cur;
	char buf[32];

	if (!b || !b->resource || *b->resource == '\0' || !b->value || *b->value == '\0')
		return;

	assess_type(b->value, &type, &val_double, &val_long);

	if (type == STRING)
		return;

	for (itr = a; itr && itr->atr_list; itr = itr->next) {
		cur = itr->atr_list;
		if (cur->resource && !strcmp(cur->resource, b->resource)) {
			switch (type) {
			case DOUBLE:
				val_double += strtod(cur->value, &pc);
				sprintf(buf, "%f", val_double);
				break;
			case LONG:
				val_long += strtol(cur->value, &pc, 10);
				sprintf(buf, "%ld", val_long);
				break;
			case SIZE:
				decode_size(&attr, NULL, NULL, b->value);
				decode_size(&new, NULL, NULL, cur->value);
				set_size(&attr, &new, INCR);
				from_size(&attr.at_val.at_size, buf);
			default:
				break;
			}
			free(cur->value);
			cur->value = strdup(buf);
			break;
		}
	}
	/* value exists in next but not in cur. Create it */
	if (!itr) {
		struct attrl *at = dup_attrl(b);
		for (cur = orig; cur->next; cur = cur->next)
			;
		cur->next = at;
	}
}

static void
aggr_resc_ct(struct batch_status *st1, struct batch_status *st2)
{
	struct attrl *a = NULL;
	struct attrl *b = NULL;
	struct attrl_holder *resc_assn = NULL;
	struct attrl_holder *cur = NULL;
	struct attrl_holder *nxt = NULL;

	if (!st1 || !st2)
		return;

	/* In the first pass gather all resources assigned attr from st1
		so we do not have to loop through all attributes */
	for (a = st1->attribs; a; a = a->next) {
		if (a->name && strcmp(a->name, ATTR_rescassn) == 0) {
			nxt = malloc(sizeof(struct attrl_holder));
			nxt->atr_list = a;
			nxt->next = NULL;
			if (cur) {
				cur->next = nxt;
				cur = cur->next;
			} else
				resc_assn = cur = nxt;
		}
	}

	for (b = st2->attribs; b; b = b->next) {
		if (b->name && strcmp(b->name, ATTR_rescassn) == 0)
			accumulate_values(resc_assn, b, st1->attribs);
	}

	for (cur = resc_assn; cur; cur = nxt) {
		nxt = cur->next;
		free(cur);
	}
}

static void
aggregate_queue(struct batch_status *sv1, struct batch_status *sv2)
{
	struct batch_status *a = NULL;
	struct batch_status *b = NULL;

	for (b = sv2; b; b = b->next) {
		for (a = sv1; a; a = a->next) {
			if (a->name && b->name && !strcmp(a->name, b->name)) {
				aggr_job_ct(a, b);
				aggr_resc_ct(a, b);
				break;
			}
		}
	}
}

static void
aggregate_svr(struct batch_status *sv1, struct batch_status *sv2)
{
	aggr_job_ct(sv1, sv2);
	aggr_resc_ct(sv1, sv2);
}

/**
 * @brief
 *	wrapper function for PBSD_status
 *	gets aggregated value for all servers.
 *
 * @param[in] c - communication handle
 * @param[in] id - job id
 * @param[in] attrib - pointer to attribute list
 * @param[in] extend - extend string for req
 * @param[in] cmd - command
 *
 * @return	structure handle
 * @retval	pointer to batch_status struct		success
 * @retval	NULL					error
 *
 */
struct batch_status *
PBSD_status_aggregate(int c, int cmd, char *id, struct attrl *attrib, char *extend, int parent_object)
{
	int i;
	struct batch_status *ret = NULL;
	struct batch_status *next = NULL;
	struct batch_status *cur = NULL;
	svr_conn_t *svr_connections = get_conn_servers();

	if (!svr_connections)
		return NULL;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return NULL;

	/* first verify the attributes, if verification is enabled */
	if ((pbs_verify_attributes(random_srv_conn(svr_connections), cmd,
		parent_object, MGR_CMD_NONE, (struct attropl *) attrib)))
		return NULL;

	for (i = 0; i < get_num_servers(); i++) {
		if (svr_connections[i].state != SVR_CONN_STATE_CONNECTED)
			continue;

		c = svr_connections[i].sd;

		if (pbs_client_thread_lock_connection(c) != 0)
			return NULL;

		if ((next = PBSD_status(c, cmd, id, attrib, extend, svr_connections))) {
			if (!ret) {
				ret = next;
				cur = next->last;
			} else {
				switch(parent_object) {
					case MGR_OBJ_SERVER:
						aggregate_svr(ret, next);
						pbs_statfree(next);
						next = NULL;
						break;
					case MGR_OBJ_QUEUE:
						aggregate_queue(ret, next);
						pbs_statfree(next);
						next = NULL;
						break;
					default:
						cur->next = next;
						cur = next->last;
				}
			}
		}

		/* unlock the thread lock and update the thread context data */
		if (pbs_client_thread_unlock_connection(c) != 0)
			return NULL;
	}

	return ret;
}

/**
 * @brief
 *	wrapper function for PBSD_status
 *	gets status randomly from one of the configured server.
 *
 * @param[in] c - communication handle
 * @param[in] id - job id
 * @param[in] attrib - pointer to attribute list
 * @param[in] extend - extend string for req
 * @param[in] cmd - command
 *
 * @return	structure handle
 * @retval	pointer to batch_status struct		success
 * @retval	NULL					error
 *
 */
struct batch_status *
PBSD_status_random(int c, int cmd, char *id, struct attrl *attrib, char *extend, int parent_object)
{
	struct batch_status *ret = NULL;
	svr_conn_t *svr_connections = get_conn_servers();

	if (!svr_connections)
		return NULL;

	if ((c = random_srv_conn(svr_connections)) < 0)
		return NULL;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return NULL;

	/* first verify the attributes, if verification is enabled */
	if ((pbs_verify_attributes(c, cmd, parent_object, MGR_CMD_NONE, (struct attropl *) attrib)))
		return NULL;

	if (pbs_client_thread_lock_connection(c) != 0)
		return NULL;

	ret = PBSD_status(c, cmd, id, attrib, extend, svr_connections);

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		return NULL;

	return ret;
}

/**
 * @brief
 *	Returns pointer to status record
 *
 * @param[in] c - index into connection table
 *
 * @return returns a pointer to a batch_status structure
 * @retval pointer to batch status on SUCCESS
 * @retval NULL on failure
 */
struct batch_status *
PBSD_status_get(int c, int type, svr_conn_t *svr_conns)
{
	struct brp_cmdstat  *stp; /* pointer to a returned status record */
	struct batch_status *bsp  = NULL;
	struct batch_status *rbsp = NULL;
	struct batch_reply  *reply;
	int i;
	struct attrl *pat;

	/* read reply from stream into presentation element */

	reply = PBSD_rdrpy(c);
	if (reply == NULL) {
		pbs_errno = PBSE_PROTOCOL;
	} else if (reply->brp_choice != BATCH_REPLY_CHOICE_NULL  &&
		reply->brp_choice != BATCH_REPLY_CHOICE_Text &&
		reply->brp_choice != BATCH_REPLY_CHOICE_Status) {
		pbs_errno = PBSE_PROTOCOL;
	} else if (get_conn_errno(c) == 0) {
		/* have zero or more attrl structs to decode here */
		stp = reply->brp_un.brp_statc;
		i = 0;
		pbs_errno = 0;
		while (stp != NULL) {
			if (i++ == 0) {
				rbsp = bsp = alloc_bs();
				if (bsp == NULL) {
					pbs_errno = PBSE_SYSTEM;
					break;
				}
			} else {
				bsp->next = alloc_bs();
				bsp = bsp->next;
				if (bsp == NULL) {
					pbs_errno = PBSE_SYSTEM;
					break;
				}
			}
			if ((bsp->name = strdup(stp->brp_objname)) == NULL) {
				pbs_errno = PBSE_SYSTEM;
				break;
			}
			bsp->attribs = stp->brp_attrl;
			if (stp->brp_attrl)
				stp->brp_attrl = 0;
			bsp->next = NULL;
			rbsp->last = bsp;

			if (type == PBS_BATCH_StatusJob || type == PBS_BATCH_SelStat || type == PBS_BATCH_StatusNode) {
				/*Add server_idx attribute */
				pat = new_attrl();
				if (pat == NULL) {
					pbs_errno = PBSE_SYSTEM;
					return NULL;
				}
				pat->name = strdup(ATTR_server_index);
				if (pat->name == NULL) {
					pbs_errno = PBSE_SYSTEM;
					free_attrl(pat);
					return NULL;
				}

				/* 3 because we at most can have 99 servers.  So to represent this in string
				we need an array of len 2 for server index + 1 for NULL char */
				pat->value = malloc(3);
				if (pat->value == NULL) {
					pbs_errno = PBSE_SYSTEM;
					free_attrl(pat);
					free(pat->name);
					return NULL;
				}
				sprintf(pat->value, "%d", get_svr_index_sock(c, svr_conns));
				
				pat->next = bsp->attribs;			
				bsp->attribs = pat;					
			}

			stp = stp->brp_stlink;
		}
		if (pbs_errno) {
			pbs_statfree(rbsp);
			rbsp = NULL;
		}
	}
	PBSD_FreeReply(reply);
	return rbsp;
}

/**
 * @brief
 *	Allocate a batch status reply structure
 */
static struct batch_status *
alloc_bs()
{
	struct batch_status *bsp;

	bsp = (struct batch_status *)malloc(sizeof(struct batch_status));
	if (bsp) {

		bsp->next = NULL;
		bsp->name = NULL;
		bsp->attribs = NULL;
		bsp->text = NULL;
	}
	return bsp;
}
