/**
 * @file vidcodec.c Video Codec
 *
 * Copyright (C) 2010 Creytiv.com
 */

#include <re.h>
#include <baresip.h>
#include "core.h"


/** Video Codec state */
struct vidcodec_st {
	struct vidcodec *vc;  /**< Video Codec */
};

static struct list vidcodecl = LIST_INIT;


static void destructor(void *arg)
{
	struct vidcodec *vc = arg;

	list_unlink(&vc->le);
}


/**
 * Register a Video Codec
 *
 * @param vp      Pointer to allocated Video Codec
 * @param pt      Payload Type
 * @param name    Name of Video Codec
 * @param fmtp    Format parameters
 * @param alloch  Allocation handler
 * @param ench    Encode handler
 * @param dech    Decode handler
 *
 * @return 0 if success, otherwise errorcode
 */
int vidcodec_register(struct vidcodec **vp, const char *pt, const char *name,
		      const char *fmtp, vidcodec_alloc_h *alloch,
		      vidcodec_enc_h *ench, vidcodec_dec_h *dech)
{
	struct vidcodec *vc;

	if (!vp)
		return EINVAL;

	vc = mem_zalloc(sizeof(*vc), destructor);
	if (!vc)
		return ENOMEM;

	list_append(&vidcodecl, &vc->le, vc);

	vc->pt     = pt;
	vc->name   = name;
	vc->fmtp   = fmtp;
	vc->alloch = alloch;
	vc->ench   = ench;
	vc->dech   = dech;

	(void)re_printf("vidcodec: %s\n", name);

	*vp = vc;

	return 0;
}


int vidcodec_clone(struct list *l, const struct vidcodec *src)
{
	struct vidcodec *vc;

	if (!l || !src)
		return EINVAL;

	vc = mem_zalloc(sizeof(*vc), destructor);
	if (!vc)
		return ENOMEM;

	*vc = *src;

	vc->le.list = NULL;
	list_append(l, &vc->le, vc);

	return 0;
}


/**
 * Find a Video Codec by name
 *
 * @param name Name of the Video Codec to find
 *
 * @return Matching Video Codec if found, otherwise NULL
 */
const struct vidcodec *vidcodec_find(const char *name)
{
	struct le *le;

	for (le=vidcodecl.head; le; le=le->next) {

		struct vidcodec *vc = le->data;

		if (name && 0 != str_casecmp(name, vc->name))
			continue;

		return vc;
	}

	return NULL;
}


/**
 * Allocate a Video Codec state
 *
 * @param sp        Pointer to allocated Video Codec state
 * @param name      Name of Video Codec
 * @param encp      Encoding parameters (optional)
 * @param decp      Decoding parameters (optional)
 * @param sdp_fmtp  SDP Format parameters
 * @param sendh     Send handler
 * @param arg       Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int vidcodec_alloc(struct vidcodec_st **sp, const char *name,
		   struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		   const struct pl *sdp_fmtp,
		   vidcodec_send_h *sendh, void *arg)
{
	struct vidcodec *vc = (struct vidcodec *)vidcodec_find(name);
	if (!vc)
		return ENOENT;

	return vc->alloch(sp, vc, name, encp, decp, sdp_fmtp,
			  sendh, arg);
}


/**
 * Get the list of Video Codecs
 *
 * @return List of Video Codecs
 */
struct list *vidcodec_list(void)
{
	return &vidcodecl;
}


struct vidcodec *vidcodec_get(const struct vidcodec_st *st)
{
	return st ? st->vc : NULL;
}


/**
 * Get the Payload Type of a Video Codec
 *
 * @param vc Video Codec
 *
 * @return Payload Type
 */
const char *vidcodec_pt(const struct vidcodec *vc)
{
	return vc ? vc->pt : NULL;
}


/**
 * Get the name of a Video Codec
 *
 * @param vc Video Codec
 *
 * @return Name of the Video Codec
 */
const char *vidcodec_name(const struct vidcodec *vc)
{
	return vc ? vc->name : NULL;
}


/**
 * Set the Format Parameters for a Video Codec
 *
 * @param vc    Video Codec
 * @param fmtp  Format Parameters
 */
void vidcodec_set_fmtp(struct vidcodec *vc, const char *fmtp)
{
	if (vc)
		vc->fmtp = fmtp;
}


bool vidcodec_cmp(const struct vidcodec *l, const struct vidcodec *r)
{
	if (!l || !r)
		return false;

	if (l == r)
		return true;

	if (0 != str_casecmp(l->name, r->name))
		return false;

	return true;
}


int vidcodec_debug(struct re_printf *pf, const struct list *vcl)
{
	struct le *le;
	int err;

	err = re_hprintf(pf, "Video codecs: (%u)\n", list_count(vcl));
	for (le = list_head(vcl); le; le = le->next) {
		const struct vidcodec *vc = le->data;
		err |= re_hprintf(pf, " %3s %-8s\n", vc->pt, vc->name);
	}

	return err;
}
