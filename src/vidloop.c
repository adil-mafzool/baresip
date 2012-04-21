/**
 * @file vidloop.c  Video loop
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <string.h>
#include <time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "vidloop"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/** Video Statistics */
struct vstat {
	uint64_t tsamp;
	uint32_t frames;
	size_t bytes;
	uint32_t bitrate;
	double efps;
};


/** Video loop */
struct video_loop {
	struct vidcodec_st *codec;
	struct vidisp_st *vidisp;
	struct vidsrc_st *vsrc;
	struct vstat stat;
	struct tmr tmr_bw;
};


static void vidsrc_frame_handler(const struct vidframe *frame, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe *f2 = NULL;

	++vl->stat.frames;

	if (frame->fmt != VID_FMT_YUV420P) {

		if (vidframe_alloc(&f2, VID_FMT_YUV420P, &frame->size))
			return;

		vidconv(f2, frame, 0);

		frame = f2;
	}

	if (vl->codec)
		(void)vidcodec_get(vl->codec)->ench(vl->codec, false, frame);
	else {
		vl->stat.bytes += vidframe_size(frame);
		(void)vidisp_display(vl->vidisp, "Video Loop", frame);
	}

	mem_deref(f2);
}


static void vidloop_destructor(void *arg)
{
	struct video_loop *vl = arg;

	tmr_cancel(&vl->tmr_bw);
	mem_deref(vl->vsrc);
	mem_deref(vl->vidisp);
	mem_deref(vl->codec);
}


static void vidisp_input_handler(char key, void *arg)
{
	(void)arg;
	ui_input(key, NULL);
}


static int vidcodec_send_handler(bool marker, struct mbuf *mb, void *arg)
{
	struct video_loop *vl = arg;
	struct vidframe frame;
	int err;

	vl->stat.bytes += mbuf_get_left(mb);

	/* decode */
	frame.data[0] = NULL;
	err = vidcodec_get(vl->codec)->dech(vl->codec, &frame, marker, mb);
	if (err) {
		DEBUG_WARNING("codec_decode: %s\n", strerror(err));
		return err;
	}

	/* display - if valid picture frame */
	if (vidframe_isvalid(&frame))
		(void)vidisp_display(vl->vidisp, "Video Loop", &frame);

	return 0;
}


static int enable_codec(struct video_loop *vl)
{
	struct vidcodec_prm prm;
	int err;

	prm.fps     = config.video.fps;
	prm.bitrate = config.video.bitrate;

	/* Use the first video codec */
	err = vidcodec_alloc(&vl->codec, vidcodec_name(vidcodec_find(NULL)),
			     &prm, NULL, NULL, vidcodec_send_handler, vl);
	if (err) {
		DEBUG_WARNING("alloc encoder: %s\n", strerror(err));
		return err;
	}

	/* OK */
	return 0;
}


static void print_status(struct video_loop *vl)
{
	(void)re_fprintf(stderr, "\rstatus: EFPS=%.1f      %u kbit/s       \r",
			 vl->stat.efps, vl->stat.bitrate);
}


static void calc_bitrate(struct video_loop *vl)
{
	const uint64_t now = tmr_jiffies();

	if (now > vl->stat.tsamp) {

		const uint32_t dur = (uint32_t)(now - vl->stat.tsamp);

		vl->stat.efps = 1000.0f * vl->stat.frames / dur;

		vl->stat.bitrate = (uint32_t) (8 * vl->stat.bytes / dur);
	}

	vl->stat.frames = 0;
	vl->stat.bytes = 0;
	vl->stat.tsamp = now;
}


static void timeout_bw(void *arg)
{
	struct video_loop *vl = arg;

	tmr_start(&vl->tmr_bw, 5000, timeout_bw, vl);

	calc_bitrate(vl);
	print_status(vl);
}


static int vsrc_reopen(struct video_loop *vl, const struct vidsz *sz)
{
	struct vidsrc *vs;
	struct vidsrc_prm prm;
	int err;

	vs = (struct vidsrc *)vidsrc_find(config.video.src_mod);
	if (!vs)
		return ENOENT;

	(void)re_printf("%s,%s: open video source: %u x %u\n",
			vs->name, config.video.src_dev, sz->w, sz->h);

	prm.orient = VIDORIENT_PORTRAIT;
	prm.fps    = config.video.fps;

	vl->vsrc = mem_deref(vl->vsrc);

	err = vs->alloch(&vl->vsrc, vs, NULL, &prm, sz, NULL,
			  config.video.src_dev, vidsrc_frame_handler,
			  NULL, vl);
	if (err) {
		DEBUG_WARNING("vidsrc %s failed: %s\n",
			      vs->name, strerror(err));
	}

	return err;
}


static int video_loop_alloc(struct video_loop **vlp, const struct vidsz *size)
{
	struct video_loop *vl;
	struct vidisp *vd;
	int err;

	vd = (struct vidisp *)vidisp_find(NULL);
	if (!vd)
		return ENOENT;

	vl = mem_zalloc(sizeof(*vl), vidloop_destructor);
	if (!vl)
		return ENOMEM;

	tmr_init(&vl->tmr_bw);

	err = vsrc_reopen(vl, size);
	if (err)
		goto out;

	err = vd->alloch(&vl->vidisp, NULL, vd, NULL, NULL,
			 vidisp_input_handler, NULL, vl);
	if (err) {
		DEBUG_WARNING("video display failed: %s\n", strerror(err));
		goto out;
	}

	tmr_start(&vl->tmr_bw, 1000, timeout_bw, vl);

 out:
	if (err)
		mem_deref(vl);
	else
		*vlp = vl;

	return err;
}


/**
 * Start the video loop (for testing)
 *
 * @param stop True to force stopping, otherwise false
 */
void video_loop_test(bool stop)
{
	static struct video_loop *vl = NULL;
	struct vidsz size;
	int err;

	if (stop) {
		if (vl)
			(void)re_printf("Disable video-loop\n");
		vl = mem_deref(vl);
		return;
	}

	size.w = config.video.width;
	size.h = config.video.height;

	if (vl) {
		if (vl->codec)
			vl->codec = mem_deref(vl->codec);
		else
			(void)enable_codec(vl);

		(void)re_printf("%sabled codec: %s\n",
				vl->codec ? "En" : "Dis",
				vidcodec_name(vidcodec_get(vl->codec)));
	}
	else {
		(void)re_printf("Enable video-loop on %s,%s: %u x %u\n",
				config.video.src_mod, config.video.src_dev,
				size.w, size.h);

		err = video_loop_alloc(&vl, &size);
		if (err) {
			DEBUG_WARNING("vidloop alloc: %s\n", strerror(err));
		}
	}
}
