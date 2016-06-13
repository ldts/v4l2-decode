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

#include <stdio.h>
#include <string.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "args.h"
#include "common.h"
#include "fileops.h"
#include "video.h"
#include "parser.h"
#include "drm-funcs.h"

/* This is the size of the buffer for the compressed stream.
 * It limits the maximum compressed frame size. */
#define STREAM_BUUFER_SIZE	(1024 * 1024)

/* The number of compress4ed stream buffers */
#define STREAM_BUFFER_CNT	2

/* The number of extra buffers for the decoded output.
 * This is the number of buffers that the application can keep
 * used and still enable video device to decode with the hardware. */
#define RESULT_EXTRA_BUFFER_CNT 2
#if 0
struct v4l2_ops {
	int (*setup_cap)(struct instance *i, int count, int w, int h);
	int (*setup_out)(struct instance *i, unsigned long codec,
			 unsigned int size, int count);
	int (*dqbuf_cap)(struct instance *i, int *n, int *finished,
			 unsigned int *bytesused);
	int (*qbuf_cap)(struct instance *i, int index, struct drm_buffer *b);
	int (*dqbuf_out)(struct instance *i, int *n);
	int (*qbuf_out)(struct instance *i, int n, int length);
};
#endif
static const unsigned int event_types[] = {
	V4L2_EVENT_EOS,
	V4L2_EVENT_SOURCE_CHANGE,
};

static struct timeval start, end;

static void time_start(void)
{
	gettimeofday(&start, NULL);
}

static void print_time_delta(const char *prefix)
{
	unsigned long delta;

	gettimeofday(&end, NULL);

	delta = (end.tv_sec * 1000000 + end.tv_usec) -
		(start.tv_sec * 1000000 + start.tv_usec);

	delta = delta;

	dbg("%s: %ld\n", prefix, delta);
}

static int subscribe_for_events(int fd)
{
	int size_event = sizeof(event_types) / sizeof(event_types[0]);
	struct v4l2_event_subscription sub;
	int i, ret;

	for (i = 0; i < size_event; i++) {
		memset(&sub, 0, sizeof(sub));
		sub.type = event_types[i];
		ret = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
		if (ret < 0)
			err("cannot subscribe for event type %d (%s)",
				sub.type, strerror(errno));
	}

	return 0;
}

static int handle_v4l_events(struct instance *inst)
{
	struct video *vid = &inst->video;
	struct v4l2_event event;
	unsigned int w, h;
	int ret;

	memset(&event, 0, sizeof(event));
	ret = ioctl(vid->fd, VIDIOC_DQEVENT, &event);
	if (ret < 0) {
		err("vidioc_dqevent failed (%s) %d", strerror(errno), -errno);
		return -errno;
	}

	switch (event.type) {
	case V4L2_EVENT_EOS:
		info("EOS reached");
		break;
	case V4L2_EVENT_SOURCE_CHANGE:
		info("Source changed");
		ret = video_g_fmt(vid, &w, &h);
		if (!ret)
			info("new resolution %ux%u", w, h);

		break;
	default:
		dbg("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

void cleanup(struct instance *i)
{
	if (i->video.fd)
		video_close(i);
	if (i->in.fd)
		input_close(i);
}

int extract_and_process_header(struct instance *i)
{
	int used, fs;
	int ret;
	int n;
	struct video *vid = &i->video;

	ret = i->parser.func(&i->parser.ctx,
			     i->in.p + i->in.offs,
			     i->in.size - i->in.offs,
			     i->video.out_buf_addr[0],
			     i->video.out_buf_size,
			     &used, &fs, 1);

	if (ret == 0) {
		err("Failed to extract header from stream");
		return -1;
	}

	/* For H263 the header is passed with the first frame, so we should
	 * pass it again */
	if (i->parser.codec != V4L2_PIX_FMT_H263)
		i->in.offs += used;
	else
		/* To do this we shall reset the stream parser to the initial
	 	 * configuration */
		parse_stream_init(&i->parser.ctx);

	dbg("Extracted header of size %d", fs);

	ret = video_queue_buf_out(i, 0, fs);
	if (ret)
		return -1;

	dbg("queued output buffer %d", 0);

	i->video.out_buf_flag[0] = 1;
#if 1
	for (n = 1; n < vid->out_buf_cnt; n++) {
		ret = video_queue_buf_out(i, n, 1);
		if (ret)
			return -1;

		i->video.out_buf_flag[n] = 1;
	}
#endif
	ret = video_stream(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		return -1;

	return 0;
}

int save_frame(struct instance *i, const void *buf, unsigned int size)
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

/* This threads is responsible for parsing the stream and
 * feeding video decoder with consecutive frames to decode */
void *parser_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	struct video *vid = &i->video;
	int used, fs, n;
	int ret;

	dbg("Parser thread started");

	while (!i->error && !i->finish && !i->parser.finished) {
		n = 0;
		pthread_mutex_lock(&i->lock);
		while (n < vid->out_buf_cnt && vid->out_buf_flag[n])
			n++;
		pthread_mutex_unlock(&i->lock);

		if (n < vid->out_buf_cnt && !i->parser.finished) {

			ret = i->parser.func(&i->parser.ctx,
					     i->in.p + i->in.offs,
					     i->in.size - i->in.offs,
					     vid->out_buf_addr[n],
					     vid->out_buf_size,
					     &used, &fs, 0);

			if (ret == 0 && i->in.offs == i->in.size) {
				dbg("Parser has extracted all frames");
				i->parser.finished = 1;
				fs = 0;
			}

			dbg("Extracted frame of size %d", fs);

			if (fs >= vid->out_buf_size)
				err("fs %u, consumed %u, out buf sz %u",
					fs, used, vid->out_buf_size);

			ret = video_queue_buf_out(i, n, fs);

			pthread_mutex_lock(&i->lock);
			vid->out_buf_flag[n] = 1;
			pthread_mutex_unlock(&i->lock);

			dbg("queued output buffer %d", n);

			i->in.offs += used;
		} else {
			pthread_mutex_lock(&i->lock);
			pthread_cond_wait(&i->cond, &i->lock);
			pthread_mutex_unlock(&i->lock);
		}
	}

	dbg("Parser thread finished");

	return NULL;
}

void *main_thread_func(void *args)
{
	struct instance *i = args;
	struct video *vid = &i->video;
	struct pollfd pfd;
	short revents;
	unsigned int n, finished, disp_idx;
	int ret;

	dbg("main thread started");

	pfd.fd = vid->fd;
	pfd.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM |
		     POLLRDBAND | POLLPRI;

	fprintf(stdout, "decoded frame ");

	while (1) {
		ret = poll(&pfd, 1, 10000);
		if (!ret) {
			err("poll timeout");
			break;
		} else if (ret < 0) {
			err("poll error");
			break;
		}

		revents = pfd.revents;

		if (revents & POLLPRI)
			handle_v4l_events(i);

		if (revents & (POLLIN | POLLRDNORM)) {
			unsigned int bytesused;

			/* capture buffer is ready */

			dbg("dequeuing capture buffer");

			if (i->use_dmabuf)
				ret = video_dequeue_capture_dmabuf(
						i, &n, &finished, &bytesused);
			else
				ret = video_dequeue_capture(i, &n, &finished,
							    &bytesused);
			if (ret < 0)
				goto next_event;

			vid->cap_buf_flag[n] = 0;

			dbg("decoded frame %ld", vid->total_captured);

			fprintf(stdout, "%03ld\b\b\b", vid->total_captured);
			fflush(stdout);

			if (finished)
				break;

			vid->total_captured++;

			disp_idx = i->use_dmabuf ? n : 0;

			time_start();

			if (i->use_drm)
				drm_display_buf(vid->cap_buf_addr[n][0],
						&i->disp_buf[disp_idx],
						bytesused,
						vid->cap_w, vid->cap_h);

			print_time_delta("disp");

			save_frame(i, (void *)vid->cap_buf_addr[n][0],
				   bytesused);

			if (i->use_dmabuf)
				ret = video_queue_buf_cap_dmabuf(
							i, n, &i->disp_buf[n]);
			else
				ret = video_queue_buf_cap(i, n);

			if (!ret)
				vid->cap_buf_flag[n] = 1;
		}

next_event:
		if (revents & (POLLOUT | POLLWRNORM)) {

			dbg("dequeuing output buffer");

			if (i->parser.finished)
				continue;

			ret = video_dequeue_output(i, &n);
			if (ret < 0) {
				err("dequeue output buffer fail");
			} else {
				pthread_mutex_lock(&i->lock);
				vid->out_buf_flag[n] = 0;
				pthread_mutex_unlock(&i->lock);
				pthread_cond_signal(&i->cond);
			}

			dbg("dequeued output buffer %d", n);
		}
	}

	dbg("main thread finished");

	return NULL;
}

int main(int argc, char **argv)
{
	struct instance inst;
	struct video *vid = &inst.video;
	pthread_t parser_thread;
	pthread_t main_thread;
	int ret, n;

	ret = parse_args(&inst, argc, argv);
	if (ret) {
		print_usage(argv[0]);
		return 1;
	}

	info("decoding resolution is %dx%d", inst.width, inst.height);

	pthread_mutex_init(&inst.lock, 0);
	pthread_condattr_init(&inst.attr);
	pthread_cond_init(&inst.cond, &inst.attr);

	vid->total_captured = 0;

	ret = drm_init();
	if (inst.use_drm && ret)
		goto err;

	ret = parse_stream_init(&inst.parser.ctx);
	if (ret)
		goto err;

	ret = input_open(&inst, inst.in.name);
	if (ret)
		goto err;

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_for_events(vid->fd);
	if (ret)
		goto err;

	ret = video_setup_output(&inst, inst.parser.codec,
				 STREAM_BUUFER_SIZE, 1);
	if (ret)
		goto err;

	ret = video_set_control(&inst);
	if (ret)
		goto err;

	if (inst.use_dmabuf)
		ret = video_setup_capture_dmabuf(&inst, 2, inst.width,
						 inst.height);
	else
		ret = video_setup_capture(&inst, 2, inst.width, inst.height);
	if (ret)
		goto err;

	ret = video_set_framerate(&inst, 30);
	if (ret)
		goto err;

	if (inst.use_dmabuf && inst.use_drm)
		ret = drm_create_bufs(inst.disp_buf, vid->cap_buf_cnt,
				      vid->cap_w, vid->cap_h, 0);
	else if (inst.use_drm)
		ret = drm_create_bufs(inst.disp_buf, 1, vid->cap_w,
				      vid->cap_h, 1);
	if (inst.use_drm && ret)
		goto err;

	ret = extract_and_process_header(&inst);
	if (ret)
		goto err;

#if 0
	for (n = 0; n < vid->cap_buf_cnt; n++)
		video_export_buf(&inst, n);
#endif

#if 0
	unsigned int index;

	video_create_bufs(&inst, &index, 1);
#endif
	/* queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (inst.use_dmabuf)
			ret = video_queue_buf_cap_dmabuf(&inst, n,
							 &inst.disp_buf[n]);
		else
			ret = video_queue_buf_cap(&inst, n);

		if (ret)
			goto err;

		vid->cap_buf_flag[n] = 1;
	}

	ret = video_stream(&inst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		goto err;

	dbg("Launching threads");

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

	if (pthread_create(&main_thread, NULL, main_thread_func, &inst))
		goto err;

	pthread_join(parser_thread, 0);
	pthread_join(main_thread, 0);

	dbg("Threads have finished");

	video_stop(&inst);

	info("Total frames captured %ld", vid->total_captured);

	if (inst.use_dmabuf)
		drm_destroy_bufs(inst.disp_buf, vid->cap_buf_cnt, 0);
	else
		drm_destroy_bufs(inst.disp_buf, 1, 1);

	drm_deinit();

	cleanup(&inst);

	pthread_mutex_destroy(&inst.lock);
	pthread_cond_destroy(&inst.cond);
	pthread_condattr_destroy(&inst.attr);

	return 0;
err:
	cleanup(&inst);
	return 1;
}

