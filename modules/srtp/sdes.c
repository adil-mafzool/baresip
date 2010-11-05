/**
 * @file sdes.c  SDP Security Descriptions for Media Streams (RFC 4568)
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "sdes.h"


const char sdp_attr_crypto[] = "crypto";


int sdes_encode_crypto(struct sdp_media *m, const char *suite,
		       const char *key, size_t key_len)
{
	return sdp_media_set_lattr(m, true, sdp_attr_crypto, "1 %s inline:%b",
				   suite, key, key_len);
}


/* http://tools.ietf.org/html/rfc4568
 * a=crypto:<tag> <crypto-suite> <key-params> [<session-params>]
 */
int sdes_decode_crypto(struct crypto *c, const char *val)
{
	struct pl tag, key_prms;
	int err;

	err = re_regex(val, str_len(val), "[0-9]+ [^ ]+ [^ ]+[]*[^]*",
		       &tag, &c->suite, &key_prms, NULL, &c->sess_prms);
	if (err)
		return err;

	c->tag = pl_u32(&tag);

	err = re_regex(key_prms.p, key_prms.l, "[^:]+:[^]+",
		       &c->key_method, &c->key_info);
	if (err)
		return err;

	return 0;
}
