/**
 * @file x11.c Video driver for X11
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _XOPEN_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <re.h>
#include <baresip.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>


#if LIBSWSCALE_VERSION_MINOR >= 9
#define SRCSLICE_CAST (const uint8_t **)
#else
#define SRCSLICE_CAST (uint8_t **)
#endif


struct vidisp_st {
	struct vidisp *vd;              /**< Inheritance (1st)     */
	struct vidsz size;              /**< Current size          */
	struct SwsContext *sws;

	Display *disp;
	Window win;
	GC gc;
	XImage *image;
	XShmSegmentInfo shm;
	bool xshmat;
	bool internal;
};


static struct vidisp *vid;       /**< X11 Video-display      */

static struct {
	int shm_error;
	int (*errorh) (Display *, XErrorEvent *);
} x11;


/* NOTE: Global handler */
static int error_handler(Display *d, XErrorEvent *e)
{
	if (e->error_code == BadAccess)
		x11.shm_error = 1;
	else if (x11.errorh)
		return x11.errorh(d, e);

	return 0;
}


static void destructor(void *arg)
{
	struct vidisp_st *st = arg;

	if (st->image) {
		st->image->data = NULL;
		XDestroyImage(st->image);
	}

	if (st->gc)
		XFreeGC(st->disp, st->gc);

	if (st->xshmat)
		XShmDetach(st->disp, &st->shm);

	if (st->shm.shmaddr != (char *)-1)
		shmdt(st->shm.shmaddr);

	if (st->shm.shmid >= 0)
		shmctl(st->shm.shmid, IPC_RMID, NULL);

	if (st->sws)
		sws_freeContext(st->sws);

	if (st->disp) {
		if (st->internal && st->win)
			XDestroyWindow(st->disp, st->win);

		XCloseDisplay(st->disp);
	}

	mem_deref(st->vd);
}


static int create_window(struct vidisp_st *st, const struct vidsz *sz)
{
	st->win = XCreateSimpleWindow(st->disp, DefaultRootWindow(st->disp),
				      0, 0, sz->w, sz->h, 1, 0, 0);
	if (!st->win) {
		re_printf("failed to create X window\n");
		return ENOMEM;
	}

	XClearWindow(st->disp, st->win);
	XMapRaised(st->disp, st->win);

	return 0;
}


static int x11_reset(struct vidisp_st *st, const struct vidsz *sz)
{
	XWindowAttributes attrs;
	XGCValues gcv;
	size_t bufsz;
	int err = 0;

	bufsz = sz->w * sz->h * 4;

	if (st->sws) {
		sws_freeContext(st->sws);
		st->sws = NULL;
	}

	if (st->image) {
		XDestroyImage(st->image);
		st->image = NULL;
	}

	st->shm.shmid = shmget(IPC_PRIVATE, bufsz, IPC_CREAT | 0777);
	if (st->shm.shmid < 0) {
		re_printf("failed to allocate shared memory\n");
		return ENOMEM;
	}

	st->shm.shmaddr = shmat(st->shm.shmid, NULL, 0);
	if (st->shm.shmaddr == (char *)-1) {
		re_printf("failed to attach to shared memory\n");
		return ENOMEM;
	}

	st->shm.readOnly = true;

	x11.shm_error = 0;
	x11.errorh = XSetErrorHandler(error_handler);

	if (!XShmAttach(st->disp, &st->shm)) {
		re_printf("failed to attach X to shared memory\n");
		return ENOMEM;
	}

	XSync(st->disp, False);
	XSetErrorHandler(x11.errorh);

	if (x11.shm_error)
		re_printf("x11: shared memory disabled\n");
	else
		st->xshmat = true;

	gcv.graphics_exposures = false;

	st->gc = XCreateGC(st->disp, st->win, GCGraphicsExposures, &gcv);
	if (!st->gc) {
		re_printf("failed to create graphics context\n");
		return ENOMEM;
	}

	if (!XGetWindowAttributes(st->disp, st->win, &attrs)) {
		re_printf("cant't get window attributes\n");
		return EINVAL;
	}

	if (st->xshmat) {
		st->image = XShmCreateImage(st->disp, attrs.visual,
					    attrs.depth, ZPixmap,
					    st->shm.shmaddr, &st->shm,
					    sz->w, sz->h);
	}
	else {
		st->image = XCreateImage(st->disp, attrs.visual,
					 attrs.depth, ZPixmap, 0,
					 st->shm.shmaddr,
					 sz->w, sz->h, 32, 0);

	}
	if (!st->image) {
		re_printf("Failed to create X image\n");
		return ENOMEM;
	}

	XResizeWindow(st->disp, st->win, sz->w, sz->h);

	st->size = *sz;

	return err;
}


/* prm->view points to the XWINDOW ID */
static int alloc(struct vidisp_st **stp, struct vidisp_st *parent,
		 struct vidisp *vd, struct vidisp_prm *prm, const char *dev,
		 vidisp_input_h *inputh, vidisp_resize_h *resizeh, void *arg)
{
	struct vidisp_st *st;
	int err = 0;

	(void)parent;
	(void)dev;
	(void)inputh;
	(void)resizeh;
	(void)arg;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->vd = mem_ref(vd);
	st->shm.shmaddr = (char *)-1;

	st->disp = XOpenDisplay(NULL);
	if (!st->disp) {
		re_printf("could not open X display\n");
		err = ENODEV;
		goto out;
	}

	/* Use provided view, or create our own */
	if (prm && prm->view)
		st->win = (Window)prm->view;
	else
		st->internal = true;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame)
{
	AVPicture pict_src, pict_dst;
	int i, err = 0;
	int ret;

	if (!vidsz_cmp(&st->size, &frame->size)) {
		char capt[256];

		if (st->size.w && st->size.h) {
			re_printf("x11: reset: %ux%u ---> %ux%u\n",
				  st->size.w, st->size.h,
				  frame->size.w, frame->size.h);
		}

		if (st->internal && !st->win)
			err = create_window(st, &frame->size);

		err |= x11_reset(st, &frame->size);
		if (err)
			return err;

		if (title) {
			re_snprintf(capt, sizeof(capt), "%s - %u x %u",
				    title, frame->size.w, frame->size.h);
		}
		else {
			re_snprintf(capt, sizeof(capt), "%u x %u",
				    frame->size.w, frame->size.h);
		}

		XStoreName(st->disp, st->win, capt);
	}

	/* Convert from YUV420P to RGB32 */
	if (!st->sws) {
		st->sws = sws_getContext(frame->size.w, frame->size.h,
					 PIX_FMT_YUV420P,
					 frame->size.w, frame->size.h,
					 PIX_FMT_RGB32,
					 SWS_BICUBIC, NULL,
					 NULL, NULL);
		if (!st->sws)
			return ENOMEM;
	}

	for (i=0; i<4; i++) {
		pict_src.data[i]     = frame->data[i];
		pict_src.linesize[i] = frame->linesize[i];
	}

	avpicture_fill(&pict_dst, (uint8_t *)st->shm.shmaddr, PIX_FMT_RGB32,
		       frame->size.w, frame->size.h);

	ret = sws_scale(st->sws, SRCSLICE_CAST pict_src.data,
			pict_src.linesize, 0, frame->size.h,
			pict_dst.data, pict_dst.linesize);
	if (ret <= 0)
		return EINVAL;

	/* draw */
	if (st->xshmat)
		XShmPutImage(st->disp, st->win, st->gc, st->image,
			     0, 0, 0, 0, st->size.w, st->size.h, false);
	else
		XPutImage(st->disp, st->win, st->gc, st->image,
			  0, 0, 0, 0, st->size.w, st->size.h);

	XSync(st->disp, false);

	return err;
}


static void hide(struct vidisp_st *st)
{
	if (!st)
		return;

	if (st->win)
		XLowerWindow(st->disp, st->win);
}


static int module_init(void)
{
	return vidisp_register(&vid, "x11", alloc, NULL, display, hide);
}


static int module_close(void)
{
	vid = mem_deref(vid);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(x11) = {
	"x11",
	"vidisp",
	module_init,
	module_close,
};
