/**
 * @file coreaudio/player.c  Apple Coreaudio sound driver - player
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <AudioToolbox/AudioQueue.h>
#include <pthread.h>
#include <re.h>
#include <baresip.h>
#include "coreaudio.h"


/* This value can be tuned */
#if TARGET_OS_IPHONE
#define BUFC 20
#else
#define BUFC 6
#endif


struct auplay_st {
	struct auplay *ap;      /* inheritance */
	AudioQueueRef queue;
	AudioQueueBufferRef buf[BUFC];
	pthread_mutex_t mutex;
	auplay_write_h *wh;
	void *arg;
};


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;
	uint32_t i;

	pthread_mutex_lock(&st->mutex);
	st->wh = NULL;
	pthread_mutex_unlock(&st->mutex);

	audio_session_disable();

	if (st->queue) {
		AudioQueuePause(st->queue);
		AudioQueueStop(st->queue, true);

		for (i=0; i<ARRAY_SIZE(st->buf); i++)
			if (st->buf[i])
				AudioQueueFreeBuffer(st->queue, st->buf[i]);

		AudioQueueDispose(st->queue, true);
	}

	mem_deref(st->ap);

	pthread_mutex_destroy(&st->mutex);
}


static void play_handler(void *userData, AudioQueueRef outQ,
			 AudioQueueBufferRef outQB)
{
	struct auplay_st *st = userData;
	auplay_write_h *wh;
	void *arg;

	pthread_mutex_lock(&st->mutex);
	wh  = st->wh;
	arg = st->arg;
	pthread_mutex_unlock(&st->mutex);

	if (!wh)
		return;

	if (!wh(outQB->mAudioData, outQB->mAudioDataByteSize, arg)) {
		/* Set the buffer to silence */
		memset(outQB->mAudioData, 0, outQB->mAudioDataByteSize);
	}

	AudioQueueEnqueueBuffer(outQ, outQB, 0, NULL);
}


int coreaudio_player_alloc(struct auplay_st **stp, struct auplay *ap,
			   struct auplay_prm *prm, const char *device,
			   auplay_write_h *wh, void *arg)
{
	AudioStreamBasicDescription fmt;
	struct auplay_st *st;
	uint32_t bytc, i;
	OSStatus status;
	int err;

	(void)device;

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->ap  = mem_ref(ap);
	st->wh  = wh;
	st->arg = arg;

	err = pthread_mutex_init(&st->mutex, NULL);
	if (err)
		goto out;

	err = audio_session_enable();
	if (err)
		goto out;

	fmt.mSampleRate       = (Float64)prm->srate;
	fmt.mFormatID         = audio_fmt(prm->fmt);
	fmt.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger |
		                kAudioFormatFlagIsPacked;
#ifdef __BIG_ENDIAN__
	fmt.mFormatFlags     |= kAudioFormatFlagIsBigEndian;
#endif
	fmt.mFramesPerPacket  = 1;
	fmt.mBytesPerFrame    = prm->ch * bytesps(prm->fmt);
	fmt.mBytesPerPacket   = prm->ch * bytesps(prm->fmt);
	fmt.mChannelsPerFrame = prm->ch;
	fmt.mBitsPerChannel   = 8*bytesps(prm->fmt);

	status = AudioQueueNewOutput(&fmt, play_handler, st, NULL,
				     kCFRunLoopCommonModes, 0, &st->queue);
	if (status) {
		re_fprintf(stderr, "AudioQueueNewOutput error: %i\n", status);
		err = ENODEV;
		goto out;
	}

	bytc = prm->frame_size * bytesps(prm->fmt);

	for (i=0; i<ARRAY_SIZE(st->buf); i++)  {

		status = AudioQueueAllocateBuffer(st->queue, bytc,
						  &st->buf[i]);
		if (status)  {
			err = ENOMEM;
			goto out;
		}

		st->buf[i]->mAudioDataByteSize = bytc;

		memset(st->buf[i]->mAudioData, 0,
		       st->buf[i]->mAudioDataByteSize);

		(void)AudioQueueEnqueueBuffer(st->queue, st->buf[i], 0, NULL);
	}

	status = AudioQueueStart(st->queue, NULL);
	if (status)  {
		re_fprintf(stderr, "AudioQueueStart error %i\n", status);
		err = ENODEV;
		goto out;
	}

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}
