/**
 * @file rtpkeep.c  RTP Keepalive
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "rtpkeep"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/*
 * See draft-ietf-avt-app-rtp-keepalive:
 *
 *  "zero"     4.1.  Transport Packet of 0-byte
 *  "rtcp"     4.3.  RTCP Packets Multiplexed with RTP Packets
 *  "stun"     4.4.  STUN Indication Packet
 *  "dyna"     4.6.  RTP Packet with Unknown Payload Type
 */


enum {
	Tr_UDP =   15,
	Tr_TCP = 7200
};

struct rtpkeep {
	struct rtp_sock *rtp;
	struct sdp_media *sdp;
	struct tmr tmr;
	char *method;
	uint32_t ts;
	bool flag;
};


static void destructor(void *arg)
{
	struct rtpkeep *rk = arg;

	tmr_cancel(&rk->tmr);
	mem_deref(rk->method);
}


/**
 * Find a dynamic payload type that is not used
 *
 * @param m SDP Media
 *
 * @return Unused payload type, -1 if no found
 */
static int find_unused_pt(const struct sdp_media *m)
{
	int pt;

	for (pt = PT_DYN_MAX; pt>=PT_DYN_MIN; pt--) {

		if (!sdp_media_format(m, false, NULL, pt, NULL, -1, -1))
			return pt;
	}

	return -1;
}


static int send_keepalive(struct rtpkeep *rk)
{
	int err = 0;

	if (!str_casecmp(rk->method, "zero")) {
		struct mbuf *mb = mbuf_alloc(1);
		if (!mb)
			return ENOMEM;
		err = udp_send(rtp_sock(rk->rtp),
			       sdp_media_raddr(rk->sdp), mb);
		mem_deref(mb);
	}
	else if (!str_casecmp(rk->method, "stun")) {
		err = stun_indication(IPPROTO_UDP, rtp_sock(rk->rtp),
				      sdp_media_raddr(rk->sdp), 0,
				      STUN_METHOD_BINDING, NULL, 0, false, 0);
	}
	else if (!str_casecmp(rk->method, "dyna")) {
		struct mbuf *mb = mbuf_alloc(RTP_HEADER_SIZE);
		int pt = find_unused_pt(rk->sdp);
		if (!mb)
			return ENOMEM;
		if (pt == -1)
			return ENOENT;
		mb->pos = mb->end = RTP_HEADER_SIZE;

		err = rtp_send(rk->rtp, sdp_media_raddr(rk->sdp), false,
			       pt, rk->ts, mb);

		mem_deref(mb);
	}
	else if (!str_casecmp(rk->method, "rtcp")) {

		if (config.avt.rtcp_mux &&
		    sdp_media_rattr(rk->sdp, "rtcp-mux")) {
			/* do nothing */
			;
		}
		else {
			DEBUG_WARNING("rtcp-mux is disabled\n");
		}
	}
	else {
		DEBUG_WARNING("unknown method: %s\n", rk->method);
		return ENOSYS;
	}

	return err;
}


/**
 * Logic:
 *
 * We check for RTP activity every 15 seconds, and clear the flag.
 * The flag is set for every transmitted RTP packet. If the flag
 * is not set, it means that we have not sent any RTP packet in the
 * last period of 0 - 15 seconds. Start transmitting RTP keepalives
 * now and every 15 seconds after that.
 */
static void timeout(void *arg)
{
	struct rtpkeep *rk = arg;
	int err;

	tmr_start(&rk->tmr, Tr_UDP * 1000, timeout, rk);

	if (rk->flag) {
		rk->flag = false;
		return;
	}

	err = send_keepalive(rk);
	if (err) {
		DEBUG_WARNING("keepalive: %s\n", strerror(err));
	}
}


int rtpkeep_alloc(struct rtpkeep **rkp, const char *method, int proto,
		  struct rtp_sock *rtp, struct sdp_media *sdp)
{
	struct rtpkeep *rk;
	int err;

	if (!rkp || !method || proto != IPPROTO_UDP || !rtp || !sdp)
		return EINVAL;

	rk = mem_zalloc(sizeof(*rk), destructor);
	if (!rk)
		return ENOMEM;

	rk->rtp = rtp;
	rk->sdp = sdp;

	err = str_dup(&rk->method, method);
	if (err)
		goto out;

	tmr_start(&rk->tmr, 20, timeout, rk);

 out:
	if (err)
		mem_deref(rk);
	else
		*rkp = rk;

	return err;
}


void rtpkeep_refresh(struct rtpkeep *rk, uint32_t ts)
{
	if (!rk)
		return;

	rk->ts = ts;
	rk->flag = true;
}
