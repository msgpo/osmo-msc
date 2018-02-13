/* Osmocom MSC+VLR end-to-end tests */

/* (C) 2017 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
 *
 * All Rights Reserved
 *
 * Author: Neels Hofmeyr <nhofmeyr@sysmocom.de>
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

#include <getopt.h>
#include <stdlib.h>

#include <osmocom/core/logging.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/application.h>
#include <osmocom/gsm/protocol/gsm_04_11.h>
#include <osmocom/gsm/gsup.h>
#include <osmocom/msc/gsup_client.h>
#include <osmocom/msc/gsm_04_11.h>
#include <osmocom/msc/debug.h>
#include <osmocom/msc/gsm_04_08.h>
#include <osmocom/msc/transaction.h>

#if BUILD_IU
#include <osmocom/msc/iucs_ranap.h>
#include <osmocom/ranap/iu_client.h>
#else
#include <osmocom/msc/iu_dummy.h>
#endif

#include "msc_vlr_tests.h"

bool _log_lines = false;

struct gsm_network *net = NULL;

const char *gsup_tx_expected = NULL;
bool gsup_tx_confirmed;

struct msgb *dtap_tx_expected = NULL;
bool dtap_tx_confirmed;

enum result_sent lu_result_sent;
enum result_sent cm_service_result_sent;
bool auth_request_sent;
const char *auth_request_expect_rand;
const char *auth_request_expect_autn;
bool cipher_mode_cmd_sent;
bool cipher_mode_cmd_sent_with_imeisv;

bool iu_release_expected = false;
bool iu_release_sent = false;
bool bssap_clear_expected = false;
bool bssap_clear_sent = false;

uint32_t cc_to_mncc_tx_expected_msg_type = 0;
const char *cc_to_mncc_tx_expected_imsi = NULL;
bool cc_to_mncc_tx_confirmed = false;
uint32_t cc_to_mncc_tx_got_callref = 0;

extern int gsm0407_pdisc_ctr_bin(uint8_t pdisc);

/* static state variables for the L3 send sequence numbers */
static uint8_t n_sd[4];

/* patch a correct send sequence number into the given message */
static void patch_l3_seq_nr(struct msgb *msg)
{
	struct gsm48_hdr *gh = msgb_l3(msg);
	uint8_t pdisc = gsm48_hdr_pdisc(gh);
	uint8_t *msg_type_oct = &msg->l3h[1];
	int bin = gsm0407_pdisc_ctr_bin(pdisc);

	if (bin >= 0 && bin < ARRAY_SIZE(n_sd)) {
		/* patch in n_sd into the msg_type octet */
		*msg_type_oct = (*msg_type_oct & 0x3f) | ((n_sd[bin] & 0x3) << 6);
		//fprintf(stderr, "pdisc=0x%02x bin=%d, patched n_sd=%u\n\n", pdisc, bin, n_sd[bin] & 3);
		/* increment N(SD) */
		n_sd[bin] = (n_sd[bin] + 1) % 4;
	} else {
		//fprintf(stderr, "pdisc=0x%02x NO SEQ\n\n", pdisc);
	}
}

/* reset L3 sequence numbers (e.g. new RR connection) */
static void reset_l3_seq_nr()
{
	memset(n_sd, 0, sizeof(n_sd));
}

struct msgb *msgb_from_hex(const char *label, uint16_t size, const char *hex)
{
	struct msgb *msg = msgb_alloc(size, label);
	unsigned char *rc;
	msg->l2h = msg->head;
	rc = msgb_put(msg, osmo_hexparse(hex, msg->head, msgb_tailroom(msg)));
	OSMO_ASSERT(rc == msg->l2h);
	return msg;
}

static const char *gh_type_name(struct gsm48_hdr *gh)
{
	return gsm48_pdisc_msgtype_name(gsm48_hdr_pdisc(gh),
					gsm48_hdr_msg_type(gh));
}

void dtap_expect_tx(const char *hex)
{
	/* Has the previously expected dtap been received? */
	OSMO_ASSERT(!dtap_tx_expected);
	if (!hex)
		return;
	dtap_tx_expected = msgb_from_hex("dtap_tx_expected", 1024, hex);
	/* Mask the sequence number out */
	if (msgb_length(dtap_tx_expected) >= 2)
		dtap_tx_expected->data[1] &= 0x3f;
	dtap_tx_confirmed = false;
}

void dtap_expect_tx_ussd(char *ussd_text)
{
	uint8_t ussd_enc[128];
	int len;
	/* header */
	char ussd_msg_hex[128] = "8b2a1c27a225020100302002013b301b04010f0416";

	log("expecting USSD:\n  %s", ussd_text);
	/* append encoded USSD text */
	gsm_7bit_encode_n_ussd(ussd_enc, sizeof(ussd_enc), ussd_text,
			       &len);
	strncat(ussd_msg_hex, osmo_hexdump_nospc(ussd_enc, len),
		sizeof(ussd_msg_hex) - strlen(ussd_msg_hex));
	dtap_expect_tx(ussd_msg_hex);
}

int vlr_gsupc_read_cb(struct gsup_client *gsupc, struct msgb *msg);

void gsup_rx(const char *rx_hex, const char *expect_tx_hex)
{
	int rc;
	struct msgb *msg;
	const char *label;

	gsup_expect_tx(expect_tx_hex);

	msg = msgb_from_hex("gsup", 1024, rx_hex);
	label = osmo_gsup_message_type_name(msg->l2h[0]);
	fprintf(stderr, "<-- GSUP rx %s: %s\n", label,
		osmo_hexdump_nospc(msgb_l2(msg), msgb_l2len(msg)));
	/* GSUP read cb takes ownership of msgb */
	rc = vlr_gsupc_read_cb(net->vlr->gsup_client, msg);
	fprintf(stderr, "<-- GSUP rx %s: vlr_gsupc_read_cb() returns %d\n",
		label, rc);
	if (expect_tx_hex)
		OSMO_ASSERT(gsup_tx_confirmed);
}

bool conn_exists(struct gsm_subscriber_connection *conn)
{
	struct gsm_subscriber_connection *c;
	llist_for_each_entry(c, &net->subscr_conns, entry) {
		if (c == conn)
			return true;
	}
	return false;
}

enum ran_type rx_from_ran = RAN_GERAN_A;

struct gsm_subscriber_connection *conn_new(void)
{
	struct gsm_subscriber_connection *conn;
	conn = msc_subscr_con_allocate(net);
	conn->via_ran = rx_from_ran;
	conn->lac = 23;
	if (conn->via_ran == RAN_UTRAN_IU) {
		struct ranap_ue_conn_ctx *ue_ctx = talloc_zero(conn, struct ranap_ue_conn_ctx);
		*ue_ctx = (struct ranap_ue_conn_ctx){
			.conn_id = 42,
		};
		conn->iu.ue_ctx = ue_ctx;
	}
	return conn;
}

struct gsm_subscriber_connection *g_conn = NULL;

void rx_from_ms(struct msgb *msg)
{
	int rc;

	struct gsm48_hdr *gh = msgb_l3(msg);

	log("MSC <--%s-- MS: %s",
	    ran_type_name(rx_from_ran),
	    gh_type_name(gh));

	if (g_conn && !conn_exists(g_conn))
		g_conn = NULL;

	if (!g_conn) {
		log("new conn");
		g_conn = conn_new();
		reset_l3_seq_nr();
		patch_l3_seq_nr(msg);
		rc = msc_compl_l3(g_conn, msg, 23);
		if (rc != MSC_CONN_ACCEPT) {
			msc_subscr_con_free(g_conn);
			g_conn = NULL;
		}
	} else {
		patch_l3_seq_nr(msg);
		if ((gsm48_hdr_pdisc(gh) == GSM48_PDISC_RR)
		    && (gsm48_hdr_msg_type(gh) == GSM48_MT_RR_CIPH_M_COMPL))
			msc_cipher_mode_compl(g_conn, msg, 0);
		else
			msc_dtap(g_conn, 23, msg);
	}

	if (g_conn && !conn_exists(g_conn))
		g_conn = NULL;
}

void ms_sends_msg(const char *hex)
{
	struct msgb *msg;

	msg = msgb_from_hex("ms_sends_msg", 1024, hex);
	msg->l1h = msg->l2h = msg->l3h = msg->data;
	rx_from_ms(msg);
	talloc_free(msg);
}

static int ms_sends_msg_fake(uint8_t pdisc, uint8_t msg_type)
{
	int rc;
	struct msgb *msg;
	struct gsm48_hdr *gh;

	msg = msgb_alloc(1024, "ms_sends_msg_fake");
	msg->l1h = msg->l2h = msg->l3h = msg->data;

	gh = (struct gsm48_hdr*)msgb_put(msg, sizeof(*gh));
	gh->proto_discr = pdisc;
	gh->msg_type = msg_type;
	/* some amount of data, whatever */
	msgb_put(msg, 123);

	patch_l3_seq_nr(msg);
	rc = gsm0408_dispatch(g_conn, msg);

	talloc_free(msg);
	return rc;
}

static inline void ms_msg_log_err(uint8_t val, uint8_t msgtype)
{
	int rc = ms_sends_msg_fake(val, msgtype);
	if (rc != -EACCES)
		log("Unexpected return value %u != %u for %s/%s",
		    -rc, -EACCES, gsm48_pdisc_name(val), gsm48_cc_msg_name(msgtype));
}

void thwart_rx_non_initial_requests()
{
	log("requests shall be thwarted");

	ms_msg_log_err(GSM48_PDISC_CC, GSM48_MT_CC_SETUP);
	ms_msg_log_err(GSM48_PDISC_MM, 0x33); /* nonexistent */
	ms_msg_log_err(GSM48_PDISC_RR, GSM48_MT_RR_SYSINFO_1);
	ms_msg_log_err(GSM48_PDISC_SMS, GSM411_MT_CP_DATA);
}

void send_sms(struct vlr_subscr *receiver,
	      struct vlr_subscr *sender,
	      char *str)
{
	struct gsm_sms *sms = sms_from_text(receiver, sender, 0, str);
	gsm411_send_sms_subscr(receiver, sms);
}

unsigned char next_rand_byte = 0;
/* override, requires '-Wl,--wrap=osmo_get_rand_id' */
int __real_osmo_get_rand_id(uint8_t *buf, size_t num);
int __wrap_osmo_get_rand_id(uint8_t *buf, size_t num)
{
	size_t i;
	for (i = 0; i < num; i++)
		buf[i] = next_rand_byte++;
	return 1;
}

/* override, requires '-Wl,--wrap=gsm340_gen_scts' */
void __real_gsm340_gen_scts(uint8_t *scts, time_t time);
void __wrap_gsm340_gen_scts(uint8_t *scts, time_t time)
{
	/* Write fixed time bytes for deterministic test results */
	osmo_hexparse("07101000000000", scts, 7);
}

const char *paging_expecting_imsi = NULL;
uint32_t paging_expecting_tmsi;
bool paging_sent;
bool paging_stopped;

void paging_expect_imsi(const char *imsi)
{
	paging_expecting_imsi = imsi;
	paging_expecting_tmsi = GSM_RESERVED_TMSI;
}

void paging_expect_tmsi(uint32_t tmsi)
{
	paging_expecting_tmsi = tmsi;
	paging_expecting_imsi = NULL;
}

static int _paging_sent(enum ran_type via_ran, const char *imsi, uint32_t tmsi, uint32_t lac)
{
	log("%s sends out paging request to IMSI %s, TMSI 0x%08x, LAC %u",
	    ran_type_name(via_ran), imsi, tmsi, lac);
	OSMO_ASSERT(paging_expecting_imsi || (paging_expecting_tmsi != GSM_RESERVED_TMSI));
	if (paging_expecting_imsi)
		VERBOSE_ASSERT(strcmp(paging_expecting_imsi, imsi), == 0, "%d");
	if (paging_expecting_tmsi != GSM_RESERVED_TMSI) {
		VERBOSE_ASSERT(paging_expecting_tmsi, == tmsi, "0x%08x");
	}
	paging_sent = true;
	paging_stopped = false;
	return 1;
}

/* override, requires '-Wl,--wrap=ranap_iu_page_cs' */
int __real_ranap_iu_page_cs(const char *imsi, const uint32_t *tmsi, uint16_t lac);
int __wrap_ranap_iu_page_cs(const char *imsi, const uint32_t *tmsi, uint16_t lac)
{
	return _paging_sent(RAN_UTRAN_IU, imsi, tmsi ? *tmsi : GSM_RESERVED_TMSI, lac);
}

/* override, requires '-Wl,--wrap=a_iface_tx_paging' */
int __real_a_iface_tx_paging(const char *imsi, uint32_t tmsi, uint16_t lac);
int __wrap_a_iface_tx_paging(const char *imsi, uint32_t tmsi, uint16_t lac)
{
	return _paging_sent(RAN_GERAN_A, imsi, tmsi, lac);
}

/* override, requires '-Wl,--wrap=msc_stop_paging' */
void __real_msc_stop_paging(struct vlr_subscr *vsub);
void __wrap_msc_stop_paging(struct vlr_subscr *vsub)
{
	paging_stopped = true;
}

void clear_vlr()
{
	struct vlr_subscr *vsub, *n;
	llist_for_each_entry_safe(vsub, n, &net->vlr->subscribers, list) {
		vlr_subscr_free(vsub);
	}

	net->authentication_required = false;
	net->a5_encryption_mask = (1 << 0);
	net->vlr->cfg.check_imei_rqd = false;
	net->vlr->cfg.assign_tmsi = false;
	net->vlr->cfg.retrieve_imeisv_early = false;
	net->vlr->cfg.retrieve_imeisv_ciphered = false;
	net->vlr->cfg.auth_tuple_max_reuse_count = 0;
	net->vlr->cfg.auth_reuse_old_sets_on_error = false;

	rx_from_ran = RAN_GERAN_A;
	auth_request_sent = false;
	auth_request_expect_rand = NULL;
	auth_request_expect_autn = NULL;

	next_rand_byte = 0;

	iu_release_expected = false;
	iu_release_sent = false;
	bssap_clear_expected = false;
	bssap_clear_sent = false;

	osmo_gettimeofday_override = false;
}

static struct log_info_cat test_categories[] = {
	[DMSC] = {
		.name = "DMSC",
		.description = "Mobile Switching Center",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DRLL] = {
		.name = "DRLL",
		.description = "A-bis Radio Link Layer (RLL)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DMM] = {
		.name = "DMM",
		.description = "Layer3 Mobility Management (MM)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DRR] = {
		.name = "DRR",
		.description = "Layer3 Radio Resource (RR)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DCC] = {
		.name = "DCC",
		.description = "Layer3 Call Control (CC)",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
	[DMM] = {
		.name = "DMM",
		.description = "Layer3 Mobility Management (MM)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DVLR] = {
		.name = "DVLR",
		.description = "Visitor Location Register",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DREF] = {
		.name = "DREF",
		.description = "Reference Counting",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DPAG]	= {
		.name = "DPAG",
		.description = "Paging Subsystem",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DIUCS] = {
		.name = "DIUCS",
		.description = "Iu-CS Protocol",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DMNCC] = {
		.name = "DMNCC",
		.description = "MNCC API for Call Control application",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
};

static struct log_info info = {
	.cat = test_categories,
	.num_cat = ARRAY_SIZE(test_categories),
};

extern void *tall_bsc_ctx;

int mncc_recv(struct gsm_network *net, struct msgb *msg)
{
	struct gsm_mncc *mncc = (void*)msg->data;
	log("MSC --> MNCC: callref 0x%x: %s", mncc->callref,
	    get_mncc_name(mncc->msg_type));

	OSMO_ASSERT(cc_to_mncc_tx_expected_msg_type);
	if (cc_to_mncc_tx_expected_msg_type != mncc->msg_type) {
		log("Mismatch! Expected MNCC msg type: %s",
		    get_mncc_name(cc_to_mncc_tx_expected_msg_type));
		abort();
	}

	if (strcmp(cc_to_mncc_tx_expected_imsi, mncc->imsi)) {
		log("Mismatch! Expected MNCC msg IMSI: '%s', got '%s'",
		    cc_to_mncc_tx_expected_imsi,
		    mncc->imsi);
		abort();
	}

	cc_to_mncc_tx_confirmed = true;
	cc_to_mncc_tx_got_callref = mncc->callref;
	cc_to_mncc_tx_expected_imsi = NULL;
	cc_to_mncc_tx_expected_msg_type = 0;
	talloc_free(msg);
	return 0;
}

/* override, requires '-Wl,--wrap=gsup_client_create' */
struct gsup_client *
__real_gsup_client_create(const char *ip_addr, unsigned int tcp_port,
			  gsup_client_read_cb_t read_cb,
			  struct oap_client_config *oap_config);
struct gsup_client *
__wrap_gsup_client_create(const char *ip_addr, unsigned int tcp_port,
			  gsup_client_read_cb_t read_cb,
			  struct oap_client_config *oap_config)
{
	struct gsup_client *gsupc;
	gsupc = talloc_zero(tall_bsc_ctx, struct gsup_client);
	OSMO_ASSERT(gsupc);
	return gsupc;
}

/* override, requires '-Wl,--wrap=gsup_client_send' */
int __real_gsup_client_send(struct gsup_client *gsupc, struct msgb *msg);
int __wrap_gsup_client_send(struct gsup_client *gsupc, struct msgb *msg)
{
	const char *is = osmo_hexdump_nospc(msg->data, msg->len);
	fprintf(stderr, "GSUP --> HLR: %s: %s\n",
		osmo_gsup_message_type_name(msg->data[0]), is);

	OSMO_ASSERT(gsup_tx_expected);
	if (strcmp(gsup_tx_expected, is)) {
		fprintf(stderr, "Mismatch! Expected:\n%s\n", gsup_tx_expected);
		abort();
	}

	talloc_free(msg);
	gsup_tx_confirmed = true;
	gsup_tx_expected = NULL;
	return 0;
}

static int _validate_dtap(struct msgb *msg, enum ran_type to_ran)
{
	btw("DTAP --%s--> MS: %s: %s",
	    ran_type_name(to_ran), gh_type_name((void*)msg->data),
	    osmo_hexdump_nospc(msg->data, msg->len));

	OSMO_ASSERT(dtap_tx_expected);

	/* Mask the sequence number out before comparing */
	msg->data[1] &= 0x3f;
	if (msg->len != dtap_tx_expected->len
	    || memcmp(msg->data, dtap_tx_expected->data, msg->len)) {
		fprintf(stderr, "Mismatch! Expected:\n%s\n",
		       osmo_hexdump_nospc(dtap_tx_expected->data,
					  dtap_tx_expected->len));
		abort();
	}

	btw("DTAP matches expected message");

	talloc_free(msg);
	dtap_tx_confirmed = true;
	talloc_free(dtap_tx_expected);
	dtap_tx_expected = NULL;
	return 0;
}

/* override, requires '-Wl,--wrap=ranap_iu_tx' */
int __real_ranap_iu_tx(struct msgb *msg, uint8_t sapi);
int __wrap_ranap_iu_tx(struct msgb *msg, uint8_t sapi)
{
	return _validate_dtap(msg, RAN_UTRAN_IU);
}

/* override, requires '-Wl,--wrap=ranap_iu_tx_release' */
int __real_ranap_iu_tx_release(struct ranap_ue_conn_ctx *ctx, const struct RANAP_Cause *cause);
int __wrap_ranap_iu_tx_release(struct ranap_ue_conn_ctx *ctx, const struct RANAP_Cause *cause)
{
	btw("Iu Release --%s--> MS", ran_type_name(RAN_UTRAN_IU));
	OSMO_ASSERT(iu_release_expected);
	iu_release_expected = false;
	iu_release_sent = true;
	return 0;
}

/* override, requires '-Wl,--wrap=iu_tx_common_id' */
int __real_ranap_iu_tx_common_id(struct ranap_ue_conn_ctx *ue_ctx, const char *imsi);
int __wrap_ranap_iu_tx_common_id(struct ranap_ue_conn_ctx *ue_ctx, const char *imsi)
{
	btw("Iu Common ID --%s--> MS (IMSI=%s)", ran_type_name(RAN_UTRAN_IU), imsi);
	return 0;
}

/* override, requires '-Wl,--wrap=a_iface_tx_dtap' */
int __real_a_iface_tx_dtap(struct msgb *msg);
int __wrap_a_iface_tx_dtap(struct msgb *msg)
{
	return _validate_dtap(msg, RAN_GERAN_A);
}

/* override, requires '-Wl,--wrap=a_iface_tx_clear_cmd' */
int __real_a_iface_tx_clear_cmd(struct gsm_subscriber_connection *conn);
int __wrap_a_iface_tx_clear_cmd(struct gsm_subscriber_connection *conn)
{
	btw("BSSAP Clear --%s--> MS", ran_type_name(RAN_GERAN_A));
	OSMO_ASSERT(bssap_clear_expected);
	bssap_clear_expected = false;
	bssap_clear_sent = true;
	return 0;
}

/* override, requires '-Wl,--wrap=msc_mgcp_call_assignment' */
int __real_msc_mgcp_call_assignment(struct gsm_trans *trans);
int __wrap_msc_mgcp_call_assignment(struct gsm_trans *trans)
{
	log("MS <--Call Assignment-- MSC: subscr=%s callref=0x%x",
	    vlr_subscr_name(trans->vsub), trans->callref);
	return 0;
}

/* override, requires '-Wl,--wrap=msc_mgcp_call_release' */
void __real_msc_mgcp_call_release(struct gsm_trans *trans);
void __wrap_msc_mgcp_call_release(struct gsm_trans *trans)
{
	log("MS <--Call Release-- MSC: subscr=%s callref=0x%x",
	    vlr_subscr_name(trans->vsub), trans->callref);
}

static int fake_vlr_tx_lu_acc(void *msc_conn_ref, uint32_t send_tmsi)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	if (send_tmsi == GSM_RESERVED_TMSI)
		btw("sending LU Accept for %s", vlr_subscr_name(conn->vsub));
	else
		btw("sending LU Accept for %s, with TMSI 0x%08x",
		    vlr_subscr_name(conn->vsub), send_tmsi);
	lu_result_sent |= RES_ACCEPT;
	return 0;
}

static int fake_vlr_tx_lu_rej(void *msc_conn_ref, uint8_t cause)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	btw("sending LU Reject for %s, cause %u", vlr_subscr_name(conn->vsub), cause);
	lu_result_sent |= RES_REJECT;
	return 0;
}

static int fake_vlr_tx_cm_serv_acc(void *msc_conn_ref)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	btw("sending CM Service Accept for %s", vlr_subscr_name(conn->vsub));
	cm_service_result_sent |= RES_ACCEPT;
	return 0;
}

static int fake_vlr_tx_cm_serv_rej(void *msc_conn_ref,
				   enum vlr_proc_arq_result result)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	btw("sending CM Service Reject for %s, result %s",
	    vlr_subscr_name(conn->vsub),
	    vlr_proc_arq_result_name(result));
	cm_service_result_sent |= RES_REJECT;
	return 0;
}

static int fake_vlr_tx_auth_req(void *msc_conn_ref, struct gsm_auth_tuple *at,
				bool send_autn)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	char *hex;
	bool ok = true;
	btw("sending %s Auth Request for %s: tuple use_count=%d key_seq=%d auth_types=0x%x and...",
	    send_autn? "UMTS" : "GSM", vlr_subscr_name(conn->vsub),
	    at->use_count, at->key_seq, at->vec.auth_types);

	hex = osmo_hexdump_nospc((void*)&at->vec.rand, sizeof(at->vec.rand));
	btw("...rand=%s", hex);
	if (!auth_request_expect_rand
	    || strcmp(hex, auth_request_expect_rand) != 0) {
		ok = false;
		log("FAILURE: expected rand=%s",
		    auth_request_expect_rand ? auth_request_expect_rand : "-");
	}

	if (send_autn) {
		hex = osmo_hexdump_nospc((void*)&at->vec.autn, sizeof(at->vec.autn));
		btw("...autn=%s", hex);
		if (!auth_request_expect_autn
		    || strcmp(hex, auth_request_expect_autn) != 0) {
			ok = false;
			log("FAILURE: expected autn=%s",
			    auth_request_expect_autn ? auth_request_expect_autn : "-");
		}
	} else if (auth_request_expect_autn) {
		ok = false;
		log("FAILURE: no AUTN sent, expected AUTN = %s",
		    auth_request_expect_autn);
	}

	if (send_autn)
		btw("...expecting res=%s",
		    osmo_hexdump_nospc((void*)&at->vec.res, at->vec.res_len));
	else
		btw("...expecting sres=%s",
		    osmo_hexdump_nospc((void*)&at->vec.sres, sizeof(at->vec.sres)));

	auth_request_sent = ok;
	return 0;
}

static int fake_vlr_tx_auth_rej(void *msc_conn_ref)
{
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	btw("sending Auth Reject for %s", vlr_subscr_name(conn->vsub));
	return 0;
}

static int fake_vlr_tx_ciph_mode_cmd(void *msc_conn_ref, bool umts_aka, bool retrieve_imeisv)
{
	/* FIXME: we actually would like to see the message bytes checked here,
	 * not possible while msc_vlr_set_ciph_mode() calls
	 * gsm0808_cipher_mode() directly. When the MSCSPLIT is ready, check
	 * the tx bytes in the sense of dtap_expect_tx() above. */
	struct gsm_subscriber_connection *conn = msc_conn_ref;
	switch (conn->via_ran) {
	case RAN_GERAN_A:
		btw("sending Ciphering Mode Command for %s: ciphers=0x%02x kc=%s"
		    " retrieve_imeisv=%d",
		    vlr_subscr_name(conn->vsub),
		    conn->network->a5_encryption_mask,
		    osmo_hexdump_nospc(conn->vsub->last_tuple->vec.kc, 8),
		    retrieve_imeisv);
		break;
	case RAN_UTRAN_IU:
		btw("sending SecurityModeControl for %s",
		    vlr_subscr_name(conn->vsub));
		break;
	default:
		btw("UNKNOWN RAN TYPE %d", conn->via_ran);
		OSMO_ASSERT(false);
		return -1;
	}
	cipher_mode_cmd_sent = true;
	cipher_mode_cmd_sent_with_imeisv = retrieve_imeisv;
	return 0;
}

void ms_sends_security_mode_complete()
{
	OSMO_ASSERT(g_conn);
	OSMO_ASSERT(g_conn->via_ran == RAN_UTRAN_IU);
	OSMO_ASSERT(g_conn->iu.ue_ctx);
	msc_rx_sec_mode_compl(g_conn);
}

const struct timeval fake_time_start_time = { 123, 456 };

void fake_time_start()
{
	osmo_gettimeofday_override_time = fake_time_start_time;
	osmo_gettimeofday_override = true;
	fake_time_passes(0, 0);
}

static void check_talloc(void *msgb_ctx, void *tall_bsc_ctx, int expected_blocks)
{
	talloc_report_full(msgb_ctx, stderr);
	/* Expecting these to stick around in tall_bsc_ctx:
full talloc report on 'msgb' (total      0 bytes in   1 blocks)
talloc_total_blocks(tall_bsc_ctx) == 8
full talloc report on 'subscr_conn_test_ctx' (total   2642 bytes in   8 blocks)
    struct gsup_client             contains    248 bytes in   1 blocks (ref 0) 0x61300000dee0
    struct gsm_network             contains   2410 bytes in   6 blocks (ref 0) 0x61700000fce0
        struct vlr_instance            contains    160 bytes in   1 blocks (ref 0) 0x611000009a60
        no_gsup_server                 contains     15 bytes in   1 blocks (ref 0) 0x60b00000ade0
        ../../../src/libosmocore/src/rate_ctr.c:199 contains   1552 bytes in   1 blocks (ref 0) 0x61b00001eae0
        .*                             contains      3 bytes in   1 blocks (ref 0) 0x60b00000af40
    msgb                           contains      0 bytes in   1 blocks (ref 0) 0x60800000bf80
	*/
	fprintf(stderr, "talloc_total_blocks(tall_bsc_ctx) == %zu\n",
		talloc_total_blocks(tall_bsc_ctx));
	if (talloc_total_blocks(tall_bsc_ctx) != expected_blocks)
		talloc_report_full(tall_bsc_ctx, stderr);
	fprintf(stderr, "\n");
}

static struct {
	bool verbose;
	int run_test_nr;
} cmdline_opts = {
	.verbose = false,
	.run_test_nr = -1,
};

static void print_help(const char *program)
{
	printf("Usage:\n"
	       "  %s [-v] [N [N...]]\n"
	       "Options:\n"
	       "  -h --help      show this text.\n"
	       "  -v --verbose   print source file and line numbers\n"
	       "  N              run only the Nth test (first test is N=1)\n",
	       program
	       );
}

static void handle_options(int argc, char **argv)
{
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"verbose", 1, 0, 'v'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "hv",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_help(argv[0]);
			exit(0);
		case 'v':
			cmdline_opts.verbose = true;
			break;
		default:
			/* catch unknown options *as well as* missing arguments. */
			fprintf(stderr, "Error in command line options. Exiting.\n");
			exit(-1);
			break;
		}
	}
}

void *msgb_ctx = NULL;

static void run_tests(int nr, const char *imsi)
{
	uint8_t test_nr;

	printf("Testing for IMSI %s\n", imsi);

	nr--; /* arg's first test is 1, in here it's 0 */
	for (test_nr = 0; msc_vlr_tests[test_nr]; test_nr++) {
		if (nr >= 0 && test_nr != nr)
			continue;

		msc_vlr_tests[test_nr](test_nr + 1, imsi);

		check_talloc(msgb_ctx, tall_bsc_ctx, 8);
	}
}

struct gsm_network *test_net(void *ctx)
{
	struct gsm_network *net = gsm_network_init(ctx, 1, 1, mncc_recv);

	net->gsup_server_addr_str = talloc_strdup(net, "no_gsup_server");
	net->gsup_server_port = 0;

	OSMO_ASSERT(msc_vlr_alloc(net) == 0);
	OSMO_ASSERT(msc_vlr_start(net) == 0);
	OSMO_ASSERT(net->vlr);
	OSMO_ASSERT(net->vlr->gsup_client);

	net->vlr->ops.tx_lu_acc = fake_vlr_tx_lu_acc;
	net->vlr->ops.tx_lu_rej = fake_vlr_tx_lu_rej;
	net->vlr->ops.tx_cm_serv_acc = fake_vlr_tx_cm_serv_acc;
	net->vlr->ops.tx_cm_serv_rej = fake_vlr_tx_cm_serv_rej;
	net->vlr->ops.tx_auth_req = fake_vlr_tx_auth_req;
	net->vlr->ops.tx_auth_rej = fake_vlr_tx_auth_rej;
	net->vlr->ops.set_ciph_mode = fake_vlr_tx_ciph_mode_cmd;

	return net;
}

int main(int argc, char **argv)
{
	handle_options(argc, argv);

	tall_bsc_ctx = talloc_named_const(NULL, 0, "subscr_conn_test_ctx");
	msgb_ctx = msgb_talloc_ctx_init(tall_bsc_ctx, 0);
	osmo_init_logging(&info);

	_log_lines = cmdline_opts.verbose;

	OSMO_ASSERT(osmo_stderr_target);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_timestamp(osmo_stderr_target, 0);
	log_set_print_filename(osmo_stderr_target, _log_lines? 1 : 0);
	log_set_print_category(osmo_stderr_target, 1);

	if (cmdline_opts.verbose)
		log_set_category_filter(osmo_stderr_target, DLSMS, 1, LOGL_DEBUG);

	net = test_net(tall_bsc_ctx);

	osmo_fsm_log_addr(false);

	msc_subscr_conn_init();

	clear_vlr();

	if (optind >= argc) {
		run_tests(-1, "901700000004620");
	} else {
		int arg;
		long int nr;
		for (arg = optind; arg < argc; arg++) {
			errno = 0;
			nr = strtol(argv[arg], NULL, 10);
			if (errno) {
				fprintf(stderr, "Invalid argument: %s\n",
					argv[arg]);
				exit(1);
			}

			run_tests(nr, "901700000004620");
		}
	}

	printf("Done\n");

	check_talloc(msgb_ctx, tall_bsc_ctx, 8);
	return 0;
}
