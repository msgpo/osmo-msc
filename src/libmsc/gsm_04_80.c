/* GSM Mobile Radio Interface Layer 3 messages on the A-bis interface
 * 3GPP TS 04.08 version 7.21.0 Release 1998 / ETSI TS 100 940 V7.21.0 */

/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2008, 2009, 2010 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2009 by Mike Haben <michael.haben@btinternet.com>
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

#include <stdint.h>
#include <errno.h>

#include <osmocom/msc/gsm_04_80.h>
#include <osmocom/msc/msc_ifaces.h>

#include <osmocom/gsm/protocol/gsm_04_80.h>
#include <osmocom/gsm/gsm0480.h>
#include <osmocom/core/msgb.h>
#include <osmocom/gsm/tlv.h>

static inline unsigned char *msgb_wrap_with_TL(struct msgb *msgb, uint8_t tag)
{
	uint8_t *data = msgb_push(msgb, 2);

	data[0] = tag;
	data[1] = msgb->len - 2;
	return data;
}

/*! Send a MT RELEASE COMPLETE message with Reject component
 *  (see section 3.6.1) and given error code (see section 3.6.7).
 *
 * \param[in]  conn            Active subscriber connection
 * \param[in]  transaction_id  Transaction ID with TI flag set
 * \param[in]  invoke_id       InvokeID of the request
 * \param[in]  problem_tag     Problem code tag (table 3.13)
 * \param[in]  problem_code    Problem code (tables 3.14-17)
 * \return     result of \ref msc_tx_dtap
 *
 * Note: if InvokeID is not available, e.g. when message parsing
 * failed, any incorrect value can be passed (0x00 > x > 0xff), so
 * the universal NULL-tag (see table 3.6) will be used instead.
 */
int msc_send_ussd_reject(struct gsm_subscriber_connection *conn,
			     uint8_t transaction_id, int invoke_id,
			     uint8_t problem_tag, uint8_t problem_code)
{
	struct gsm48_hdr *gh;
	struct msgb *msg;

	msg = gsm0480_gen_reject(invoke_id, problem_tag, problem_code);
	if (!msg)
		return -1;

	/* Wrap the component in a Facility message */
	msgb_wrap_with_TL(msg, GSM0480_IE_FACILITY);

	/* And finally pre-pend the L3 header */
	gh = (struct gsm48_hdr *) msgb_push(msg, sizeof(*gh));
	gh->proto_discr  = GSM48_PDISC_NC_SS;
	gh->proto_discr |= transaction_id << 4;
	gh->msg_type = GSM0480_MTYPE_RELEASE_COMPLETE;

	return msc_tx_dtap(conn, msg);
}

int msc_send_ussd_notify(struct gsm_subscriber_connection *conn, int level, const char *text)
{
	struct msgb *msg = gsm0480_create_ussd_notify(level, text);
	if (!msg)
		return -1;
	return msc_tx_dtap(conn, msg);
}

int msc_send_ussd_release_complete(struct gsm_subscriber_connection *conn)
{
	struct msgb *msg = gsm0480_create_ussd_release_complete();
	if (!msg)
		return -1;
	return msc_tx_dtap(conn, msg);
}
