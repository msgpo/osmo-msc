/*
 * (C) 2014 by Holger Hans Peter Freyther
 * (C) 2014 by sysmocom s.f.m.c. GmbH
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <osmocom/ctrl/control_cmd.h>
#include <osmocom/core/utils.h>
#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/gsm_subscriber.h>
#include <osmocom/msc/db.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/vlr.h>

#include <stdbool.h>

static struct gsm_network *msc_ctrl_net = NULL;

static int get_subscriber_list(struct ctrl_cmd *cmd, void *d)
{
	struct vlr_subscr *vsub;

	if (!msc_ctrl_net) {
		cmd->reply = "MSC CTRL commands not initialized";
		return CTRL_CMD_ERROR;
	}

	if (!msc_ctrl_net->vlr) {
		cmd->reply = "VLR not initialized";
		return CTRL_CMD_ERROR;
	}

	cmd->reply = talloc_strdup(cmd, "");

	llist_for_each_entry(vsub, &msc_ctrl_net->vlr->subscribers, list) {
		/* Do not list subscribers that aren't successfully attached. */
		if (!vsub->lu_complete)
			continue;
		cmd->reply = talloc_asprintf_append(cmd->reply, "%s,%s\n",
						    vsub->imsi, vsub->msisdn);
	}
	printf("%s\n", cmd->reply); /* <-- what? */
	return CTRL_CMD_REPLY;
}
CTRL_CMD_DEFINE_RO(subscriber_list, "subscriber-list-active-v1");

int msc_ctrl_cmds_install(struct gsm_network *net)
{
	int rc = 0;
	msc_ctrl_net = net;

	rc |= ctrl_cmd_install(CTRL_NODE_ROOT, &cmd_subscriber_list);
	return rc;
}
