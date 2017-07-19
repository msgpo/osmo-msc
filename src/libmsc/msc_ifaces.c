/* Implementation for MSC decisions which interface to send messages out on. */

/* (C) 2016 by sysmocom s.m.f.c GmbH <info@sysmocom.de>
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
 */

#include <osmocom/core/logging.h>
#include <osmocom/ranap/iu_client.h>

#include <openbsc/debug.h>
#include <openbsc/gsm_data.h>
#include <openbsc/msc_ifaces.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/transaction.h>
#include <osmocom/legacy_mgcp/mgcp.h>
#include <osmocom/legacy_mgcp/mgcpgw_client.h>
#include <openbsc/vlr.h>
#include <openbsc/a_iface.h>

#include "../../bscconfig.h"

extern struct msgb *ranap_new_msg_rab_assign_voice(uint8_t rab_id,
						   uint32_t rtp_ip,
						   uint16_t rtp_port,
						   bool use_x213_nsap);

static int msc_tx(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	DEBUGP(DMSC, "msc_tx %u bytes to %s via %s\n",
	       msg->len, vlr_subscr_name(conn->vsub),
	       ran_type_name(conn->via_ran));
	switch (conn->via_ran) {
	case RAN_GERAN_A:
		msg->dst = conn;
		return a_iface_tx_dtap(msg);

	case RAN_UTRAN_IU:
		msg->dst = conn->iu.ue_ctx;
		return ranap_iu_tx(msg, 0);

	default:
		LOGP(DMSC, LOGL_ERROR,
		     "msc_tx(): conn->via_ran invalid (%d)\n",
		     conn->via_ran);
		return -1;
	}
}


int msc_tx_dtap(struct gsm_subscriber_connection *conn,
		struct msgb *msg)
{
	return msc_tx(conn, msg);
}


/* 9.2.5 CM service accept */
int msc_gsm48_tx_mm_serv_ack(struct gsm_subscriber_connection *conn)
{
	struct msgb *msg = gsm48_msgb_alloc_name("GSM 04.08 SERV ACC");
	struct gsm48_hdr *gh = (struct gsm48_hdr *) msgb_put(msg, sizeof(*gh));

	gh->proto_discr = GSM48_PDISC_MM;
	gh->msg_type = GSM48_MT_MM_CM_SERV_ACC;

	DEBUGP(DMM, "-> CM SERVICE ACCEPT %s\n",
	       vlr_subscr_name(conn->vsub));

	return msc_tx_dtap(conn, msg);
}

/* 9.2.6 CM service reject */
int msc_gsm48_tx_mm_serv_rej(struct gsm_subscriber_connection *conn,
			     enum gsm48_reject_value value)
{
	struct msgb *msg;
	conn->received_cm_service_request = false;

	msg = gsm48_create_mm_serv_rej(value);
	if (!msg) {
		LOGP(DMM, LOGL_ERROR, "Failed to allocate CM Service Reject.\n");
		return -1;
	}

	DEBUGP(DMM, "-> CM SERVICE Reject cause: %d\n", value);

	return msc_tx_dtap(conn, msg);
}

int msc_tx_common_id(struct gsm_subscriber_connection *conn)
{
	/* Common ID is only sent over IuCS */
	if (conn->via_ran != RAN_UTRAN_IU) {
		LOGP(DMM, LOGL_INFO,
		     "%s: Asked to transmit Common ID, but skipping"
		     " because this is not on UTRAN\n",
		     vlr_subscr_name(conn->vsub));
		return 0;
	}

#ifdef BUILD_IU
	DEBUGP(DIUCS, "%s: tx CommonID %s\n",
	       vlr_subscr_name(conn->vsub), conn->vsub->imsi);
	return ranap_iu_tx_common_id(conn->iu.ue_ctx, conn->vsub->imsi);
#else
	LOGP(DMM, LOGL_ERROR,
	     "Cannot send CommonID: RAN_UTRAN_IU but IuCS support not built\n");
	return -ENOTSUP;
#endif
}

#ifdef BUILD_IU
static void iu_rab_act_cs(struct ranap_ue_conn_ctx *uectx, uint8_t rab_id,
			  uint32_t rtp_ip, uint16_t rtp_port)
{
	struct msgb *msg;
	bool use_x213_nsap;
	uint32_t conn_id = uectx->conn_id;

	use_x213_nsap = (uectx->rab_assign_addr_enc == RANAP_NSAP_ADDR_ENC_X213);

	LOGP(DIUCS, LOGL_DEBUG, "Assigning RAB: conn_id=%u, rab_id=%d,"
	     " rtp=%x:%u, use_x213_nsap=%d\n", conn_id, rab_id, rtp_ip,
	     rtp_port, use_x213_nsap);

	msg = ranap_new_msg_rab_assign_voice(rab_id, rtp_ip, rtp_port,
					     use_x213_nsap);
	msg->l2h = msg->data;

	if (ranap_iu_rab_act(uectx, msg))
		LOGP(DIUCS, LOGL_ERROR, "Failed to send RAB Assignment:"
		     " conn_id=%d rab_id=%d rtp=%x:%u\n",
		     conn_id, rab_id, rtp_ip, rtp_port);
}

static void mgcp_response_rab_act_cs_crcx(struct mgcp_response *r, void *priv)
{
	struct gsm_trans *trans = priv;
	struct gsm_subscriber_connection *conn = trans->conn;
	struct ranap_ue_conn_ctx *uectx = conn->iu.ue_ctx;
	uint32_t rtp_ip;
	int rc;

	if (r->head.response_code != 200) {
		LOGP(DMGCP, LOGL_ERROR,
		     "MGCPGW response yields error: %d %s\n",
		     r->head.response_code, r->head.comment);
		goto rab_act_cs_error;
	}

	rc = mgcp_response_parse_params(r);
	if (rc) {
		LOGP(DMGCP, LOGL_ERROR,
		     "Cannot parse MGCP response, for %s\n",
		     vlr_subscr_name(trans->vsub));
		goto rab_act_cs_error;
	}

	conn->rtp.port_cn = r->audio_port;

	rtp_ip = mgcpgw_client_remote_addr_n(conn->network->mgcpgw.client);

	if (trans->conn->via_ran == RAN_UTRAN_IU) {
		/* Assign a voice channel via RANAP on 3G */
		iu_rab_act_cs(uectx, conn->iu.rab_id, rtp_ip,
			      conn->rtp.port_subscr);
		/* use_x213_nsap == 0 for ip.access nano3G */
	} else if (trans->conn->via_ran == RAN_GERAN_A) {
		/* Assign a voice channel via A on 2G */
		if (a_iface_tx_assignment(trans))
			goto rab_act_cs_error;
	} else
		goto rab_act_cs_error;

rab_act_cs_error:
	/* FIXME abort call, invalidate conn, ... */
	return;
}

static int conn_iu_rab_act_cs(struct gsm_trans *trans)
{
	struct gsm_subscriber_connection *conn = trans->conn;
	struct mgcpgw_client *mgcp = conn->network->mgcpgw.client;
	struct msgb *msg;
	struct msgb *msg_dlcx;
	uint16_t bts_base;

	/* HACK. where to scope the RAB Id? At the conn / subscriber /
	 * ranap_ue_conn_ctx? */
	static uint8_t next_rab_id = 1;
	conn->iu.rab_id = next_rab_id ++;

	conn->iu.mgcp_rtp_endpoint =
		mgcpgw_client_next_endpoint(conn->network->mgcpgw.client);

	/* This will calculate the port we assign to the BTS via AoIP
	 * assignment command (or rab-assignment on 3G) The BTS will send
	 * its RTP traffic to that port on the MGCPGW side. The MGCPGW only
	 * gets the endpoint ID via the CRCX. It will do the same calculation
	 * on his side too to get knowledge of the rtp port. */
	bts_base = mgcpgw_client_conf_actual(mgcp)->bts_base;
	conn->rtp.port_subscr = bts_base + 2 * conn->iu.mgcp_rtp_endpoint;

	/* Since we know now the endpoint number, we enforce a DLCX on tha
	 * endpoint in order to ensure that this endpoint is not occupied
	 * with some old connection that was not properly cleared during
	 * some crash or restart event */
	msg_dlcx =
	    mgcp_msg_dlcx(mgcp, conn->iu.mgcp_rtp_endpoint,
			  conn->iu.mgcp_rtp_endpoint);
	if (mgcpgw_client_tx(mgcp, msg_dlcx, NULL, NULL))
		LOGP(DMGCP, LOGL_ERROR,
		     "Failed to send DLCX message for %s\n",
		     vlr_subscr_name(trans->vsub));

	/* Establish the RTP stream first as looping back to the originator.
	 * The MDCX will patch through to the counterpart. TODO: play a ring
	 * tone instead. */
	msg = mgcp_msg_crcx(mgcp, conn->iu.mgcp_rtp_endpoint,
			    conn->iu.mgcp_rtp_endpoint, MGCP_CONN_LOOPBACK);
	return mgcpgw_client_tx(mgcp, msg, mgcp_response_rab_act_cs_crcx, trans);
}
#endif

int msc_call_assignment(struct gsm_trans *trans)
{
	struct gsm_subscriber_connection *conn = trans->conn;

	switch (conn->via_ran) {
	case RAN_GERAN_A:
		/* FIXME We first go for conn_iu_rab_act_cs(), this function
		 * will create a loopback rtp connection first and then call
		 * a_iface_tx_assignment(). Probably we need to find another
		 * name for conn_iu_rab_act_cs? */
		return conn_iu_rab_act_cs(trans);

	case RAN_UTRAN_IU:
#ifdef BUILD_IU
		return conn_iu_rab_act_cs(trans);
#else
		LOGP(DMSC, LOGL_ERROR,
		     "msc_call_assignment(): IuCS RAB Activation not supported"
		     " in this build\n");
		return -ENOTSUP;
#endif

	default:
		LOGP(DMSC, LOGL_ERROR,
		     "msc_tx(): conn->via_ran invalid (%d)\n",
		     conn->via_ran);
		return -EINVAL;
	}
}

static void mgcp_response_bridge_mdcx(struct mgcp_response *r, void *priv);

static void mgcp_bridge(struct gsm_trans *from, struct gsm_trans *to,
			enum bridge_state state,
			enum mgcp_connection_mode mode)
{
	struct gsm_subscriber_connection *conn1 = from->conn;
	struct gsm_subscriber_connection *conn2 = to->conn;
	struct mgcpgw_client *mgcp = conn1->network->mgcpgw.client;
	const char *ip;
	struct msgb *msg;

	OSMO_ASSERT(mgcp);

	from->bridge.peer = to;
	from->bridge.state = state;

	/* Loop back to the same MGCP GW */
	ip = mgcpgw_client_remote_addr_str(mgcp);

	msg = mgcp_msg_mdcx(mgcp,
			    conn1->iu.mgcp_rtp_endpoint,
			    ip, conn2->rtp.port_cn,
			    mode);
	if (mgcpgw_client_tx(mgcp, msg, mgcp_response_bridge_mdcx, from))
		LOGP(DMGCP, LOGL_ERROR,
		     "Failed to send MDCX message for %s\n",
		     vlr_subscr_name(from->vsub));
}

static void mgcp_response_bridge_mdcx(struct mgcp_response *r, void *priv)
{
	struct gsm_trans *trans = priv;
	struct gsm_trans *peer = trans->bridge.peer;

	switch (trans->bridge.state) {
	case BRIDGE_STATE_LOOPBACK_PENDING:
		trans->bridge.state = BRIDGE_STATE_LOOPBACK_ESTABLISHED;

		switch (peer->bridge.state) {
		case BRIDGE_STATE_LOOPBACK_PENDING:
			/* Wait until the other is done as well. */
			return;
		case BRIDGE_STATE_LOOPBACK_ESTABLISHED:
			/* Now that both are in loopback, switch both to
			 * forwarding. */
			mgcp_bridge(trans, peer, BRIDGE_STATE_BRIDGE_PENDING,
				    MGCP_CONN_RECV_SEND);
			mgcp_bridge(peer, trans, BRIDGE_STATE_BRIDGE_PENDING,
				    MGCP_CONN_RECV_SEND);
			break;
		default:
			LOGP(DMGCP, LOGL_ERROR,
			     "Unexpected bridge state: %d for %s\n",
			     trans->bridge.state, vlr_subscr_name(trans->vsub));
			break;
		}
		break;

	case BRIDGE_STATE_BRIDGE_PENDING:
		trans->bridge.state = BRIDGE_STATE_BRIDGE_ESTABLISHED;
		break;

	default:
		LOGP(DMGCP, LOGL_ERROR,
		     "Unexpected bridge state: %d for %s\n",
		     trans->bridge.state, vlr_subscr_name(trans->vsub));
		break;
	}
}

int msc_call_bridge(struct gsm_trans *trans1, struct gsm_trans *trans2)
{
	/* First setup as loopback and configure the counterparts' endpoints,
	 * so that when transmission starts the originating addresses are
	 * already known to be valid. The mgcp callback will continue. */
	mgcp_bridge(trans1, trans2, BRIDGE_STATE_LOOPBACK_PENDING,
		    MGCP_CONN_LOOPBACK);
	mgcp_bridge(trans2, trans1, BRIDGE_STATE_LOOPBACK_PENDING,
		    MGCP_CONN_LOOPBACK);

	return 0;
}

void msc_call_release(struct gsm_trans *trans)
{
	struct msgb *msg;
	struct gsm_subscriber_connection *conn = trans->conn;
	struct mgcpgw_client *mgcp = conn->network->mgcpgw.client;

	/* Send DLCX */
	msg = mgcp_msg_dlcx(mgcp, conn->iu.mgcp_rtp_endpoint,
			    conn->iu.mgcp_rtp_endpoint);
	if (mgcpgw_client_tx(mgcp, msg, NULL, NULL))
		LOGP(DMGCP, LOGL_ERROR,
		     "Failed to send DLCX message for %s\n",
		     vlr_subscr_name(trans->vsub));

	/* Release endpoint id */
	mgcpgw_client_release_endpoint(conn->iu.mgcp_rtp_endpoint, mgcp);
}
