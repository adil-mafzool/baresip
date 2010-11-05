/**
 * @file auloop.c  Audio loop
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "auloop"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/* Configurable items */
#define PTIME 20

static const char *device = "/dev/dsp";
static const char *codec = NULL; /*"pcmu";*/


struct audio_loop {
	uint32_t index;
	struct aubuf *ab;
	struct ausrc_st *ausrc;
	struct auplay_st *auplay;
	struct aucodec_st *codec;
	uint32_t srate;
	uint32_t ch;
	uint32_t fs;
	uint32_t n_read;
	uint32_t n_write;
};

static const struct {
	uint32_t srate;
	uint32_t ch;
} configv[] = {
	{ 8000, 1},
	{16000, 1},
	{32000, 1},
	{48000, 1},
	{ 8000, 2},
	{16000, 2},
	{32000, 2},
	{48000, 2},
};

static struct audio_loop *gal = NULL;


static void auloop_destructor(void *data)
{
	struct audio_loop *al = data;

	mem_deref(al->ausrc);
	mem_deref(al->auplay);
	mem_deref(al->ab);
	mem_deref(al->codec);
}


static void print_stats(struct audio_loop *al)
{
	(void)re_fprintf(stderr, "\r%uHz %dch frame_size=%u"
			 " n_read=%u n_write=%u"
			 " aubuf=%u codec=%s",
			 al->srate, al->ch, al->fs,
			 al->n_read, al->n_write,
			 aubuf_cur_size(al->ab), codec);
}


static int codec_read(struct audio_loop *al, uint8_t *buf, size_t sz)
{
	struct mbuf *mbr = NULL, *mbc = NULL, *mbw = NULL;
	int err;

	mbr = mbuf_alloc(sz);
	mbc = mbuf_alloc(sz);
	mbw = mbuf_alloc(sz);
	if (!mbr || !mbc || !mbw) {
		err = ENOMEM;
		goto out;
	}

	aubuf_read(al->ab, mbr->buf, sz);

	mbr->pos = 0;
	mbr->end = sz;

	err = aucodec_get(al->codec)->ench(al->codec, mbc, mbr);
	if (err) {
		DEBUG_WARNING("read: codec_enc: %s\n", strerror(err));
	}

	mbc->pos = 0;

	err = aucodec_get(al->codec)->dech(al->codec, mbw, mbc);
	if (err) {
		DEBUG_WARNING("read: codec_dec: %s\n", strerror(err));
	}

	memcpy(buf, mbw->buf, sz);

 out:
	mem_deref(mbr);
	mem_deref(mbc);
	mem_deref(mbw);

	return err;
}


static void read_handler(const uint8_t *buf, size_t sz, void *arg)
{
	struct audio_loop *al = arg;
	int err;

	++al->n_read;

	err = aubuf_write(al->ab, buf, sz);
	if (err) {
		DEBUG_WARNING("aubuf_write: %s\n", strerror(err));
	}

	print_stats(al);
}


static bool write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct audio_loop *al = arg;

	++al->n_write;

	/* read from beginning */
	if (al->codec) {
		(void)codec_read(al, buf, sz);
	}
	else {
		aubuf_read(al->ab, buf, sz);
	}

	return true;
}


static void error_handler(int err, const char *str, void *arg)
{
	(void)arg;
	DEBUG_WARNING("error: %s (%s)\n", strerror(err), str);
	gal = mem_deref(gal);
}


static void start_codec(struct audio_loop *al)
{
	struct aucodec_prm prm;
	int err;

	prm.srate = configv[al->index].srate;
	prm.ptime = PTIME;

	al->codec = mem_deref(al->codec);
	err = aucodec_alloc(&al->codec, codec,
			    configv[al->index].srate,
			    configv[al->index].ch,
			    NULL, NULL, NULL);
	if (err) {
		DEBUG_WARNING("codec_alloc: %s\n", strerror(err));
	}
}


static int auloop_reset(struct audio_loop *al)
{
	struct auplay_prm auplay_prm;
	struct ausrc_prm ausrc_prm;
	int err;

	al->auplay = mem_deref(al->auplay);
	al->ausrc = mem_deref(al->ausrc);
	al->ab = mem_deref(al->ab);

	al->srate = configv[al->index].srate;
	al->ch    = configv[al->index].ch;
	al->fs    = calc_nsamp(al->srate, al->ch, PTIME);

	(void)re_printf("Audio-loop: %uHz, %dch\n", al->srate, al->ch);

	err = aubuf_alloc(&al->ab, 320, 0);
	if (err)
		return err;

	auplay_prm.fmt        = AUFMT_S16LE;
	auplay_prm.srate      = al->srate;
	auplay_prm.ch         = al->ch;
	auplay_prm.frame_size = al->fs;
	err = auplay_alloc(&al->auplay, NULL, &auplay_prm, device,
			   write_handler, al);
	if (err) {
		DEBUG_WARNING("auplay failed: %s\n", strerror(err));
		return err;
	}

	ausrc_prm.fmt        = AUFMT_S16LE;
	ausrc_prm.srate      = al->srate;
	ausrc_prm.ch         = al->ch;
	ausrc_prm.frame_size = al->fs;
	err = ausrc_alloc(&al->ausrc, NULL, &ausrc_prm, device,
			  read_handler, error_handler, al);
	if (err) {
		DEBUG_WARNING("ausrc failed: %s\n", strerror(err));
		return err;
	}

	return err;
}


static int audio_loop_alloc(struct audio_loop **alp)
{
	struct audio_loop *al;
	int err;

	al = mem_zalloc(sizeof(*al), auloop_destructor);
	if (!al)
		return ENOMEM;

	/* Optional audio codec */
	if (codec) {
		start_codec(al);
	}

	err = auloop_reset(al);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(al);
	else
		*alp = al;

	return err;
}


static int audio_loop_cycle(struct audio_loop *al)
{
	int err;

	++al->index;

	if (al->index >= ARRAY_SIZE(configv)) {
		gal = mem_deref(gal);
		(void)re_printf("\nAudio-loop stopped\n");
		return 0;
	}

	if (codec)
		start_codec(al);

	err = auloop_reset(al);
	if (err)
		return err;

	(void)re_printf("\nAudio-loop started: %uHz, %dch\n",
			al->srate, al->ch);

	return 0;
}


void audio_loop_test(bool stop)
{
	int err;

	if (stop) {
		if (gal) {
			(void)re_printf("audio-loop stopped\n");
			gal = mem_deref(gal);
		}
	}
	else if (gal) {
		err = audio_loop_cycle(gal);
		if (err) {
			DEBUG_WARNING("cycle: %s\n", strerror(err));
		}
	}
	else {
		err = audio_loop_alloc(&gal);
		if (err) {
			DEBUG_WARNING("auloop: %s\n", strerror(err));
		}
	}
}
