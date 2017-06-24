/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Main file of the application
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "args.h"
#include "common.h"
#include "video.h"
#include "display.h"

#define DBG_TAG "  main"

#define V4L2_PIX_FMT_HEVC	v4l2_fourcc('H', 'E', 'V', 'C') /* HEVC */
#define V4L2_PIX_FMT_VP9	v4l2_fourcc('V', 'P', '9', '0') /* VP9 */

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

/* This is the size of the buffer for the compressed stream.
 * It limits the maximum compressed frame size. */
#define STREAM_BUFFER_SIZE	(1024 * 1024)

static void stream_close(struct instance *i);

static const int event_type[] = {
	V4L2_EVENT_EOS,
	V4L2_EVENT_SOURCE_CHANGE,
};

static int
subscribe_events(struct instance *i)
{
	const int n_events = sizeof(event_type) / sizeof(event_type[0]);
	int idx;

	for (idx = 0; idx < n_events; idx++) {
		if (video_subscribe_event(i, event_type[idx]))
			return -1;
	}

	return 0;
}

#ifndef WAYLAND_SUPPORT
void fb_destroy(struct fb *fb) {}
struct fb *
window_create_buffer(struct window *window, int group,
		     int index, int fd, int offset,
		     uint32_t format, int width, int height, int stride)
{ return NULL; }

#endif

static void repaint_surface(struct fb *fb);

static int
restart_capture(struct instance *i)
{
	struct video *vid = &i->video;
	int n, ret;

	/*
	 * Destroy window buffers that are not in use by the
	 * wayland compositor; buffers in use will be destroyed
	 * when the release callback is called
	 */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		struct fb *fb = i->disp_buffers[n];
		if (fb && !fb->busy)
			fb_destroy(fb);
	}

	/* Stop capture and release buffers */
	if (vid->cap_buf_cnt > 0 && video_stop_capture(i))
		return -1;

	/* Setup capture queue with new parameters */
	if (video_setup_capture(i, 8, i->width, i->height))
		return -1;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_export_capture(i, n, &i->v4l_dmabuf_fd[n]))
			return -1;

		dbg("exported capture buffer index:%u, fd:%d", n,
			i->v4l_dmabuf_fd[n]);

		if (i->use_kms) {
			i->drm_bufs[n].dbuf_fd = i->v4l_dmabuf_fd[n];

			ret = drm_dmabuf_import(&i->drm_bufs[n], i->width,
						i->height);
			if (ret) {
				err("cannot import dmabuf %d", ret);
				return -1;
			}

			ret = drm_dmabuf_addfb(&i->drm_bufs[n], i->width,
						i->height);
			if (ret)
				return -1;
		}
	}

	/* Queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_queue_buf_cap(i, n))
			return -1;
	}

	/* Start streaming */
	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	if (i->use_kms)
		return 0;

	if (!i->use_kms && !i->use_wayland)
		return 0;

	/* Recreate the window frame buffers */
	i->group++;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		unsigned int uv_off = vid->cap_buf_stride[0] * vid->cap_h;

		i->disp_buffers[n] =
			window_create_buffer(i->window, i->group, n,
					     i->v4l_dmabuf_fd[n],
					     uv_off,
					     vid->cap_buf_format,
					     i->width, i->height,
					     vid->cap_buf_stride[0]);
		if (!i->disp_buffers[n])
			return -1;

		i->disp_buffers[n]->inst = i;
	}

	return 0;
}

static int
handle_video_event(struct instance *i)
{
	struct v4l2_event event;
	unsigned int w, h;
	int ret;

	if (video_dequeue_event(i, &event))
		return -1;

	switch (event.type) {
	case V4L2_EVENT_EOS:
		info("EOS reached");
		break;
	case V4L2_EVENT_SOURCE_CHANGE:
		info("Source changed");
		ret = video_g_fmt(i->video.fd, &w, &h);
		if (!ret)
			info("new resolution %ux%u", w, h);

		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE);
		break;
	default:
		dbg("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

static void
cleanup(struct instance *i)
{
	stream_close(i);

#ifdef WAYLAND_SUPPORT
	if (i->window)
		window_destroy(i->window);
	if (i->display)
		display_destroy(i->display);
#endif
	if (i->sigfd != 1)
		close(i->sigfd);
	if (i->video.fd)
		video_close(i);
	if (i->use_kms)
		drm_deinit();
}

static int
save_frame(struct instance *i, const void *buf, unsigned int size)
{
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	char filename[64];
	int fd;
	int ret;
	static unsigned int frame_num = 0;

	if (!i->save_frames)
		return 0;

	if (!i->save_path)
		ret = sprintf(filename, "/mnt/frame%04d.nv12", frame_num);
	else
		ret = sprintf(filename, "%s/frame%04d.nv12", i->save_path,
			      frame_num);
	if (ret < 0) {
		err("sprintf fail (%s)", strerror(errno));
		return -1;
	}

	dbg("create file %s", filename);

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, mode);
	if (fd < 0) {
		err("cannot open file (%s)", strerror(errno));
		return -1;
	}

	ret = write(fd, buf, size);
	if (ret < 0) {
		err("cannot write to file (%s)", strerror(errno));
		return -1;
	}

	close(fd);

	frame_num++;

	return 0;
}

static int
parse_frame(struct instance *i, AVPacket *pkt)
{
	int ret;

	if (!i->bsf_data_pending) {
		ret = av_read_frame(i->avctx, pkt);
		if (ret < 0)
			return ret;

		if (pkt->stream_index != i->stream->index) {
			av_packet_unref(pkt);
			return AVERROR(EAGAIN);
		}

		if (i->bsf) {
			ret = av_bsf_send_packet(i->bsf, pkt);
			if (ret < 0)
				return ret;

			i->bsf_data_pending = 1;
		}
	}

	if (i->bsf) {
		ret = av_bsf_receive_packet(i->bsf, pkt);
		if (ret == AVERROR(EAGAIN))
			i->bsf_data_pending = 0;

		if (ret < 0)
			return ret;
	}

	return 0;
}

static void
finish(struct instance *i)
{
	dbg("finish: enter");
//	pthread_mutex_lock(&i->lock);
	i->finish = 1;
//	pthread_cond_signal(&i->cond);
//	pthread_mutex_unlock(&i->lock);

	queue_flush(&i->out_queue);
	queue_flush(&i->cap_queue);
	dbg("finish: exit");
}

static int
send_eos(struct instance *i, int buf_index)
{
	struct video *vid = &i->video;
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (video_queue_buf_out(i, buf_index, 1, V4L2_BUF_FLAG_LAST, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

static int
send_pkt(struct instance *i, int buf_index, AVPacket *pkt)
{
	struct video *vid = &i->video;
	struct timeval tv;
	int flags;

	memcpy(vid->out_buf_addr[buf_index], pkt->data, pkt->size);
	flags = 0;

	if (pkt->pts != AV_NOPTS_VALUE) {
		AVRational vid_timebase;
		AVRational v4l_timebase = { 1, 1000000 };
		int64_t v4l_pts;

		if (i->bsf)
			vid_timebase = i->bsf->time_base_out;
		else
			vid_timebase = i->stream->time_base;

		v4l_pts = av_rescale_q(pkt->pts, vid_timebase, v4l_timebase);
		tv.tv_sec = v4l_pts / 1000000;
		tv.tv_usec = v4l_pts % 1000000;
	} else {
		/* invalid timestamp */
		flags = 0;
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	if (video_queue_buf_out(i, buf_index, pkt->size, flags, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}


/* This thread is responsible for parsing the stream and
 * feeding video decoder with consecutive frames to decode
 */
static void *
parser_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	AVPacket pkt;
	int buf, parse_ret;

	dbg("Parser thread started");

	av_init_packet(&pkt);

	while (1) {
		parse_ret = parse_frame(i, &pkt);
		if (parse_ret == AVERROR(EAGAIN))
			continue;

		if (i->finish)
			break;

		buf = queue_remove(&i->out_queue, 0);

		/* decoding stopped before parsing ended, abort */
		if (buf < 0)
			break;

		if (parse_ret < 0) {
			if (parse_ret == AVERROR_EOF)
				dbg("Queue end of stream");
			else
				av_err(parse_ret, "Parsing failed");

			send_eos(i, buf);
			break;
		}

		if (send_pkt(i, buf, &pkt) < 0)
			break;

		av_packet_unref(&pkt);
	}

	av_packet_unref(&pkt);

	dbg("Parser thread finished");

	return NULL;
}

static void
finish_capture_buffer(struct instance *i, int n)
{
	video_queue_buf_cap(i, n);
}

static void
buffer_released(struct fb *fb, void *data)
{
	struct instance *i = data;
	int n = fb->index;

	if (fb->group != i->group) {
		fb_destroy(fb);
		return;
	}

	dbg("%s %02d", __func__, n);

	finish_capture_buffer(i, n);
}

#ifdef WAYLAND_SUPPORT
static void repaint_surface_wayland(struct fb *fb)
{
	struct instance *inst = fb->inst;
	static uint64_t last = 0;
	struct timeval tv_now;
	uint64_t diff, now;
	int n;

	gettimeofday(&tv_now, NULL);

	now = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
	diff = now - last;
	last = now;

	n = queue_remove(&inst->cap_queue, 1);
	if (n < 0) {
		err("cannot get buffer from decoder");
		window_show_buffer(inst->window, NULL,
			   NULL, NULL);
		return;
	}

	info("repaint %02d at %lu (queued %d)", n, diff,
		queue_num(&inst->cap_queue));

	if (!inst->use_kms)
		window_show_buffer(inst->window, inst->disp_buffers[n],
				buffer_released, inst);
}
#else
static void repaint_surface_wayland(struct fb *fb) {}
#endif

static struct instance *global_inst;
static void repaint_surface_kms(struct fb *fb)
{
	struct instance *inst = global_inst;
	static uint64_t last = 0;
	struct timeval tv_now;
	uint64_t diff, now;
	int n;
	static int prev_idx = -1;

	gettimeofday(&tv_now, NULL);

	now = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
	diff = now - last;
	last = now;

	n = queue_num(&inst->cap_queue);
	if (n < 1)
		return;

	if (prev_idx != -1)
		finish_capture_buffer(inst, prev_idx);

	n = queue_remove(&inst->cap_queue, 1);
	if (n < 0) {
		err("cannot get buffer from decoder");
		return;
	}

	dbg("repaint %02d (%02d) at %lu (queued %d)", n, prev_idx, diff,
		queue_num(&inst->cap_queue));

	prev_idx = n;
}

static uint64_t ts_diff_func(struct timeval *new, uint64_t old_ts)
{
	uint64_t new_ts;

	new_ts = new->tv_sec * 1000000 + new->tv_usec;

	return new_ts - old_ts;
}

static int
handle_video_capture(struct instance *i)
{
	struct video *vid = &i->video;
	struct timeval tv;
	static uint64_t prev_ts;
	uint64_t df;
	unsigned int bytesused, buf_flags;
	int ret, n, finished;
	struct timeval tv_now;
	static uint64_t last_ts, now_ts, diff_ts;

	ret = video_dequeue_capture(i, &n, &finished, &bytesused, &tv,
				    &buf_flags);
	if (ret < 0) {
		err("dequeue capture buffer fail (%d)", ret);
		return ret;
	}

	if (bytesused > 0) {

		queue_add(&i->cap_queue, n);
		dbg("dequeued capture buffer %d", n);

		save_frame(i, vid->cap_buf_addr[n][0], bytesused);

		if (buf_flags & V4L2_BUF_FLAG_ERROR) {
			finish_capture_buffer(i, n);
			info("corrupted frame from decoder");
			return 0;
		}

		vid->total_captured++;

		if (i->use_wayland) {
			static int once;
			if (!once) {
				if (queue_num(&i->cap_queue) < 2)
					return 0;

				once = 1;
				repaint_surface_wayland(i->disp_buffers[n]);
			}
		} else if (i->use_kms) {
			gettimeofday(&tv_now, NULL);
			now_ts = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
			diff_ts = now_ts - last_ts;
			last_ts = now_ts;

			df = ts_diff_func(&tv, prev_ts);
			dbg("dq capture %02d diff %lu (%lu)", n, diff_ts, df);

			prev_ts = tv.tv_sec * 1000000 + tv.tv_usec;

			drm_dmabuf_set_plane(&i->drm_bufs[n],
					     i->width, i->height,
					     i->fullscreen);
		} else {
			gettimeofday(&tv_now, NULL);
			now_ts = tv_now.tv_sec * 1000000 + tv_now.tv_usec;
			diff_ts = now_ts - last_ts;
			last_ts = now_ts;

			df = ts_diff_func(&tv, prev_ts);
			info("dq capture %02d diff %lu (%lu) buf_flags:%x", n,
				diff_ts, df, buf_flags);

			prev_ts = tv.tv_sec * 1000000 + tv.tv_usec;

			finish_capture_buffer(i, n);
			queue_remove(&i->cap_queue, 0);

			fprintf(stdout, "%08ld\b\b\b\b\b\b\b\b",
				vid->total_captured);
			fflush(stdout);
		}
	} else if (!i->reconfigure_pending) {
		finish_capture_buffer(i, n);
	}

	if (finished) {
		info("End of stream");
		finish(i);
	}

	return 0;
}

static int
handle_video_output(struct instance *i)
{
	int ret, n;

	ret = video_dequeue_output(i, &n, NULL);
	if (ret < 0) {
		err("dequeue output buffer fail");
		return ret;
	}

	ret = queue_add(&i->out_queue, n);
	if (ret) {
		err("queue add failed");
		return ret;
	}

	return 0;
}

static int
handle_signal(struct instance *i)
{
	struct signalfd_siginfo siginfo;
	sigset_t sigmask;

	if (read(i->sigfd, &siginfo, sizeof (siginfo)) < 0) {
		perror("signalfd/read");
		return -1;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, siginfo.ssi_signo);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

	finish(i);

	return 0;
}

static int
setup_signal(struct instance *i)
{
	sigset_t sigmask;
	int fd;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	fd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	if (fd < 0) {
		perror("signalfd");
		return -1;
	}

	sigprocmask(SIG_BLOCK, &sigmask, NULL);
	i->sigfd = fd;

	return 0;
}

#ifdef WAYLAND_SUPPORT
static void main_loop_wayland(struct instance *i)
{
	struct video *vid = &i->video;
	struct wl_display *wl_display;
	struct pollfd pfd[3];
	short revents;
	int nfds;
	int ret;
	unsigned int idx;

	dbg("main thread started");

	pfd[0].fd = vid->fd;
	pfd[0].events = POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM | POLLPRI;

	wl_display = display_get_wl_display(i->display);
	pfd[1].fd = wl_display_get_fd(wl_display);
	pfd[1].events = POLLIN;

	nfds = 2;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		nfds++;
	}

	while (!i->finish) {

		while (wl_display_prepare_read(wl_display) != 0)
			wl_display_dispatch_pending(wl_display);

		ret = wl_display_flush(wl_display);
		if (ret < 0) {
			if (errno == EAGAIN)
				pfd[1].events |= POLLOUT;
			else if (errno != EPIPE) {
				err("wl_display_flush: %m");
				wl_display_cancel_read(wl_display);
				break;
			}
		}

		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

		ret = wl_display_read_events(wl_display);
		if (ret < 0) {
			err("wl_display_read_events: %m");
			break;
		}

		ret = wl_display_dispatch_pending(wl_display);
		if (ret < 0) {
			err("wl_display_dispatch_pending: %m");
			break;
		}

		if (i->paused)
			pfd[0].events &= ~(POLLIN | POLLRDNORM);
		else
			pfd[0].events |= POLLIN | POLLRDNORM;

		for (idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			ret = 0;

			switch (idx) {
			case 0:
				if (revents & (POLLIN | POLLRDNORM))
					ret = handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);
				break;
			case 1:
				if (revents & POLLOUT)
					pfd[1].events &= ~POLLOUT;
				break;
			case 2:
				handle_signal(i);
				break;
			}

			if (ret == -EPIPE)
				break;
		}

		if (ret == -EPIPE)
			break;
	}

	finish(i);

	dbg("main thread finished");
}
#else
static void main_loop_wayland(struct instance *i) {}
#endif

static void main_loop_kms(struct instance *i)
{
	struct video *vid = &i->video;
	struct pollfd pfd[2];
	short revents;
	int nfds;
	int ret;
	unsigned int idx;

	dbg("main thread started");

	pfd[0].fd = vid->fd;
	pfd[0].events = POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM | POLLPRI;

	nfds = 1;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		nfds++;
	}

	while (!i->finish) {

		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

		for (idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			ret = 0;

			switch (idx) {
			case 0:
				if (revents & (POLLIN | POLLRDNORM))
					ret = handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);
				break;
			case 1:
				handle_signal(i);
				break;
			}

			if (ret == -EPIPE)
				break;
		}

		if (ret == -EPIPE)
			break;
	}

	finish(i);

	dbg("main thread finished");
}

static void main_loop(struct instance *i)
{
	if (i->use_wayland)
		main_loop_wayland(i);
	else
		main_loop_kms(i);
}

#ifdef WAYLAND_SUPPORT
static void
handle_window_key(struct window *window, uint32_t time, uint32_t key,
		  enum wl_keyboard_key_state state)
{
	struct instance *i = window_get_user_data(window);

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	switch (key) {
	case KEY_ESC:
		finish(i);
		break;

	case KEY_SPACE:
		info("%s", i->paused ? "Resume" : "Pause");
		i->paused = !i->paused;
		if (i->paused)
			av_read_pause(i->avctx);
		else
			av_read_play(i->avctx);
		break;

	case KEY_F:
		window_toggle_fullscreen(i->window);
		break;
	}
}
#endif

static int setup_display(struct instance *i)
{
	AVRational ar;

	if (!i->use_kms && !i->use_wayland)
		return 0;

	if (i->use_kms)
		return drm_init();

#ifdef WAYLAND_SUPPORT
	i->display = display_create();
	if (!i->display)
		return -1;

	i->window = display_create_window(i->display, i->repaint);
	if (!i->window)
		return -1;

	window_set_user_data(i->window, i);
	window_set_key_callback(i->window, handle_window_key);

	ar = av_guess_sample_aspect_ratio(i->avctx, i->stream, NULL);
	window_set_aspect_ratio(i->window, ar.num, ar.den);

	if (i->fullscreen)
		window_toggle_fullscreen(i->window);
#endif
	return 0;
}

static void
stream_close(struct instance *i)
{
	i->stream = NULL;
	if (i->bsf)
		av_bsf_free(&i->bsf);
	if (i->avctx)
		avformat_close_input(&i->avctx);
}

static int
get_av_log_level(void)
{
	if (debug_level >= 5)
		return AV_LOG_TRACE;
	if (debug_level >= 4)
		return AV_LOG_DEBUG;
	if (debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (debug_level >= 2)
		return AV_LOG_INFO;
	if (debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

static int
stream_open(struct instance *i)
{
	const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	int codec_id = AV_CODEC_ID_H264;
	int codec;
	int ret;

	av_log_set_level(get_av_log_level());

	av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = avformat_find_stream_info(i->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(i->avctx, -1, i->url, 0);

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	i->framerate = i->stream->r_frame_rate;

	dbg("framerate: %d/%d", i->framerate.num, i->framerate.den);

	i->width = codecpar->width;
	i->height = codecpar->height;

	filter = NULL;

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_H263:
		codec = V4L2_PIX_FMT_H263;
		break;
	case AV_CODEC_ID_H264:
		codec = V4L2_PIX_FMT_H264;
		filter = av_bsf_get_by_name("h264_mp4toannexb");
		break;
	case AV_CODEC_ID_HEVC:
		codec = V4L2_PIX_FMT_HEVC;
		filter = av_bsf_get_by_name("hevc_mp4toannexb");
		break;
	case AV_CODEC_ID_MPEG2VIDEO:
		codec = V4L2_PIX_FMT_MPEG2;
		break;
	case AV_CODEC_ID_MPEG4:
		codec = V4L2_PIX_FMT_MPEG4;
		break;
#if 0
	case AV_CODEC_ID_MSMPEG4V3:
		codec = V4L2_PIX_FMT_DIVX_311;
		break;
#endif
	case AV_CODEC_ID_WMV3:
		codec = V4L2_PIX_FMT_VC1_ANNEX_G;
		break;
	case AV_CODEC_ID_VC1:
		codec = V4L2_PIX_FMT_VC1_ANNEX_L;
		break;
	case AV_CODEC_ID_VP8:
		codec = V4L2_PIX_FMT_VP8;
		break;
	case AV_CODEC_ID_VP9:
		codec = V4L2_PIX_FMT_VP9;
		break;
	default:
		err("cannot decode %s", avcodec_get_name(codecpar->codec_id));
		goto fail;
	}

	i->fourcc = codec;

	if (filter) {
		ret = av_bsf_alloc(filter, &i->bsf);
		if (ret < 0) {
			av_err(ret, "cannot allocate bistream filter");
			goto fail;
		}

		avcodec_parameters_copy(i->bsf->par_in, codecpar);
		i->bsf->time_base_in = i->stream->time_base;

		ret = av_bsf_init(i->bsf);
		if (ret < 0) {
			av_err(ret, "failed to initialize bitstream filter");
			goto fail;
		}
	}

	return 0;

fail:
	stream_close(i);
	return -1;
}

static void *
vbl_thread_func(void *args)
{
	struct instance *inst = args;

	drm_dmabuf_vblank(repaint_surface_kms, &inst->finish);

	return NULL;
}

int main(int argc, char **argv)
{
	struct instance inst;
	pthread_t parser_thread, vbl_thread;
	unsigned int i;
	int ret;

	ret = parse_args(&inst, argc, argv);
	if (ret) {
		print_usage(argv[0]);
		return 1;
	}

	inst.sigfd = -1;

	inst.repaint = repaint_surface_wayland;
	global_inst = &inst;

	ret = queue_init(&inst.out_queue, MAX_OUT_BUF);
	if (ret)
		goto err;

	ret = queue_init(&inst.cap_queue, MAX_CAP_BUF);
	if (ret)
		goto err;

	ret = stream_open(&inst);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_events(&inst);
	if (ret)
		goto err;

	ret = video_setup_output(&inst, inst.fourcc, STREAM_BUFFER_SIZE, 2);
	if (ret)
		goto err;

	for (i = 0; i < inst.video.out_buf_cnt; i++)
		queue_add(&inst.out_queue, i);

	ret = setup_display(&inst);
	if (ret)
		goto err;

	ret = video_set_control(&inst);
	if (ret)
		goto err;

	ret = video_stream(&inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		goto err;

	ret = restart_capture(&inst);
	if (ret)
		goto err;

	setup_signal(&inst);

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

	if (inst.use_kms) {
		if (pthread_create(&vbl_thread, NULL, vbl_thread_func, &inst))
			goto err;
	}

	main_loop(&inst);

	pthread_join(parser_thread, 0);

	if (inst.use_kms)
		pthread_join(vbl_thread, 0);

	dbg("Threads have finished");

	video_stop_capture(&inst);
	video_stop_output(&inst);

	cleanup(&inst);

	queue_free(&inst.out_queue);
	queue_free(&inst.cap_queue);

	info("Total frames captured %ld", inst.video.total_captured);

	return 0;
err:
	cleanup(&inst);
	return 1;
}

