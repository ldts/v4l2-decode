/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
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

#include <linux/videodev2.h>
#include "msm-v4l2-controls.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "common.h"

/* mem2mem encoder/decoder */
#define V4L2_BUF_FLAG_LAST			0x00100000

static char *dbg_type[2] = {"OUTPUT", "CAPTURE"};
static char *dbg_status[2] = {"ON", "OFF"};

int video_open(struct instance *i, char *name)
{
	struct v4l2_capability cap;
	int ret;

	i->video.fd = open(name, O_RDWR, 0);
	if (i->video.fd < 0) {
		err("Failed to open video decoder: %s", name);
		return -1;
	}

	memzero(cap);
	ret = ioctl(i->video.fd, VIDIOC_QUERYCAP, &cap);
	if (ret) {
		err("Failed to verify capabilities");
		return -1;
	}

	info("caps (%s): driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x",
	     name, cap.driver, cap.bus_info, cap.card, i->video.fd);

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_STREAMING)) {
		err("Insufficient capabilities for video device (is %s correct?)",
		    name);
		return -1;
	}

        return 0;
}

void video_close(struct instance *i)
{
	close(i->video.fd);
}

int video_set_control(struct instance *i)
{
	struct v4l2_control control = {0};
	int ret;

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
	control.value = 1;

	ret = ioctl(i->video.fd, VIDIOC_S_CTRL, &control);
	if (ret < 0)
		err("setting cont data transfer (%s)", strerror(errno));

	return ret;
}

int video_set_framerate(struct instance *i, unsigned int framerate)
{
	struct v4l2_streamparm parm = {0};

	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = framerate;

	return ioctl(i->video.fd, VIDIOC_S_PARM, &parm);
}

int video_export_buf(struct instance *i, unsigned int index)
{
	struct video *vid = &i->video;
	struct v4l2_exportbuffer expbuf;
	unsigned int num_planes = CAP_PLANES;
	unsigned int n;

	for (n = 0; n < num_planes; n++) {
		memset(&expbuf, 0, sizeof(expbuf));

		expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		expbuf.index = index;
		expbuf.flags = O_CLOEXEC | O_RDWR;
		expbuf.plane = n;

		if (ioctl(vid->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
			err("CAPTURE: Failed to export buffer index%u (%s)",
			    index, strerror(errno));
			return -1;
		}

		info("CAPTURE: Exported buffer index%u (plane%u) with fd %d",
		     index, n, expbuf.fd);
	}

	return 0;
}

int video_create_bufs(struct instance *i, unsigned int index, unsigned int count)
{
	struct video *vid = &i->video;
	struct v4l2_create_buffers b;
	int ret;

	memzero(b);
	b.index = index;
	b.count = count;
	b.memory = V4L2_MEMORY_MMAP;
	b.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.format.fmt.pix_mp.width = 1280;
	b.format.fmt.pix_mp.height = 720;
	b.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	ret = ioctl(vid->fd, VIDIOC_CREATE_BUFS, &b);
	if (ret) {
		err("Failed to create bufs index%u (%s)", b.index,
			strerror(errno));
		return -1;
	}

	info("create_bufs: index %u, count %u", b.index, b.count);

	return 0;
}

static int video_queue_buf(struct instance *i, unsigned int index,
			   unsigned int l1, unsigned int l2,
			   unsigned int type, unsigned int nplanes)
{
	struct video *vid = &i->video;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];
	int ret;

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index;
	buf.length = nplanes;
	buf.m.planes = planes;

	buf.m.planes[0].bytesused = l1;
	buf.m.planes[1].bytesused = l2;

	buf.m.planes[0].data_offset = 0;
	buf.m.planes[1].data_offset = 0;

	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		buf.m.planes[0].length = vid->cap_buf_size[0];
	} else {
		buf.m.planes[0].length = vid->out_buf_size;
		if (l1 == 0) {
			buf.m.planes[0].bytesused = 1;
			buf.flags |= V4L2_BUF_FLAG_LAST;
		}
	}

	ret = ioctl(vid->fd, VIDIOC_QBUF, &buf);
	if (ret) {
		err("QBUF: Failed to queue buffer (index=%u) on %s (%s)",
		    buf.index,
		    dbg_type[type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE],
		    strerror(errno));
		return -1;
	}

	dbg("QBUF: buffer on %s queue with index %u",
	    dbg_type[type==V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE], buf.index);

	return 0;
}

int video_queue_buf_out(struct instance *i, unsigned int n, unsigned int length)
{
	struct video *vid = &i->video;

	if (n >= vid->out_buf_cnt) {
		err("Tried to queue a non exisiting buffer");
		return -1;
	}

	return video_queue_buf(i, n, length, 0,
			       V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			       OUT_PLANES);
}

int video_queue_buf_cap(struct instance *i, unsigned int index)
{
	struct video *vid = &i->video;

	if (index >= vid->cap_buf_cnt) {
		err("Tried to queue a non exisiting buffer");
		return -1;
	}

	return video_queue_buf(i, index, vid->cap_buf_size[0],
			       vid->cap_buf_size[1],
			       V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			       CAP_PLANES);
}

int video_queue_buf_cap_dmabuf(struct instance *i, unsigned int index,
			       struct drm_buffer *b)
{
	struct video *vid = &i->video;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];
	int ret;

	if (index >= vid->cap_buf_cnt) {
		err("Tried to queue a non exisiting buffer");
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = index;
	buf.length = CAP_PLANES;
	buf.m.planes = planes;

	buf.m.planes[0].m.fd = b->dbuf_fd;

	buf.m.planes[0].bytesused = vid->cap_buf_size[0];
	buf.m.planes[1].bytesused = vid->cap_buf_size[1];

	buf.m.planes[0].data_offset = 0;
	buf.m.planes[1].data_offset = 0;

	buf.m.planes[0].length = vid->cap_buf_size[0];

	ret = ioctl(vid->fd, VIDIOC_QBUF, &buf);
	if (ret) {
		err("QBUF: Failed to queue buffer (index=%u) on CAPTURE (%s)",
		    buf.index, strerror(errno));
		return -1;
	}

	dbg("  QBUF: Queued buffer on %s queue with index %u",
	    dbg_type[1], buf.index);

	return 0;
}

static int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		err("Failed to dequeue buffer (%d)", -errno);
		return -errno;
	}

	dbg("Dequeued buffer on %s queue with index %u (flags:%x, bytesused:%u)",
	    dbg_type[buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE],
	    buf->index, buf->flags, buf->m.planes[0].bytesused);

	return 0;
}

int video_dequeue_output(struct instance *i, unsigned int *n)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

	ret = video_dequeue_buf(i, &buf);
	if (ret < 0)
		return ret;

	*n = buf.index;

	return 0;
}

int video_dequeue_capture(struct instance *i, unsigned int *n,
			  unsigned int *finished, unsigned int *bytesused)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = CAP_PLANES;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*finished = 0;

	if (buf.flags & V4L2_QCOM_BUF_FLAG_EOS ||
	    buf.flags & V4L2_BUF_FLAG_LAST ||
	    buf.m.planes[0].bytesused == 0)
		*finished = 1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	return 0;
}

int video_dequeue_capture_dmabuf(struct instance *i, unsigned int *n,
				 unsigned int *finished,
				 unsigned int *bytesused)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.m.planes = planes;
	buf.length = CAP_PLANES;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*finished = 0;

	if (buf.flags & V4L2_QCOM_BUF_FLAG_EOS ||
	    buf.m.planes[0].bytesused == 0)
		*finished = 1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	return 0;
}

int video_stream(struct instance *i, enum v4l2_buf_type type, unsigned int status)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, status, &type);
	if (ret) {
		err("Failed to change streaming (type=%s, status=%s)",
		    dbg_type[type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE],
		    dbg_status[status == VIDIOC_STREAMOFF]);
		return -1;
	}

	dbg("Stream %s on %s queue", dbg_status[status==VIDIOC_STREAMOFF],
	    dbg_type[type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE]);

	return 0;
}

int video_stop(struct instance *i)
{
	struct video *vid = &i->video;
	struct v4l2_requestbuffers reqbuf;
	int ret;

#if 0
	struct v4l2_decoder_cmd dec;

	memzero(dec);
	dec.cmd = V4L2_DEC_CMD_STOP;
	ret = ioctl(vid->fd, VIDIOC_DECODER_CMD, &dec);
	if (ret < 0) {
		err("DECODER_CMD failed (%s)", strerror(errno));
		return -1;
	}
#endif
	ret = video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			   VIDIOC_STREAMOFF);
	if (ret < 0)
		err("STREAMOFF CAPTURE queue failed (%s)", strerror(errno));

	ret = video_stream(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMOFF);
	if (ret < 0)
		err("STREAMOFF OUTPUT queue failed (%s)", strerror(errno));

	memzero(reqbuf);
	reqbuf.count = 0;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	info("calling reqbuf(0)");

	ret = ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf);
	if (ret < 0)
		err("REQBUFS(0) on CAPTURE queue (%s)", strerror(errno));

	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	ret = ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf);
	if (ret < 0)
		err("REQBUFS(0) on OUTPUT queue (%s)", strerror(errno));

	return 0;
}

int video_g_fmt(struct video *vid, unsigned int *width, unsigned int *height)
{
	struct v4l2_format g_fmt;
	int ret;

	memzero(g_fmt);
	g_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	g_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	ret = ioctl(vid->fd, VIDIOC_G_FMT, &g_fmt);
	if (ret) {
		err("CAPTURE: Failed to set format (%s)", strerror(errno));
		return -1;
	}

	*width = g_fmt.fmt.pix_mp.width;
	*height = g_fmt.fmt.pix_mp.height;
	return 0;
}

int video_setup_capture(struct instance *i, unsigned int count, unsigned int w,
			unsigned int h)
{
	struct video *vid = &i->video;
	struct v4l2_format fmt, try_fmt, g_fmt;
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];
	int ret;
	int n;
#if 1
	memzero(try_fmt);
	try_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	try_fmt.fmt.pix_mp.width = w;
	try_fmt.fmt.pix_mp.height = h;
	try_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	ret = ioctl(vid->fd, VIDIOC_TRY_FMT, &try_fmt);
	if (ret) {
		err("CAPTURE: Failed to try format (%s)", strerror(errno));
	}

	info("CAPTURE: Try format %ux%u sizeimage %u, bpl %u",
	    try_fmt.fmt.pix_mp.width, try_fmt.fmt.pix_mp.height,
	    try_fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	    try_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
#endif
	memzero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	ret = ioctl(vid->fd, VIDIOC_S_FMT, &fmt);
	if (ret) {
		err("CAPTURE: Failed to set format (%s)", strerror(errno));
		return -1;
	}

	vid->cap_w = fmt.fmt.pix_mp.width;
	vid->cap_h = fmt.fmt.pix_mp.height;

	vid->cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	vid->cap_buf_size[1] = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;

	vid->cap_buf_cnt = count;
	vid->cap_buf_cnt_min = 1;
	vid->cap_buf_queued = 0;

	info("CAPTURE: Set format %ux%d sizeimage %u, bpl %u",
	     fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	     fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	     fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

#if 1
	memzero(g_fmt);
	g_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	g_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	ret = ioctl(vid->fd, VIDIOC_G_FMT, &g_fmt);
	if (ret) {
		err("CAPTURE: Failed to get format (%s)", strerror(errno));
		return -1;
	} else {
		info("CAPTURE: Get format %ux%u sizeimage %u, bpl %u",
		     g_fmt.fmt.pix_mp.width, g_fmt.fmt.pix_mp.height,
		     g_fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
		     g_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
	}

	vid->cap_w = g_fmt.fmt.pix_mp.width;
	vid->cap_h = g_fmt.fmt.pix_mp.height;
#endif

	memzero(reqbuf);
	reqbuf.count = vid->cap_buf_cnt;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf);
	if (ret != 0) {
		err("CAPTURE: REQBUFS failed (%s)", strerror(errno));
		return -1;
	}

	info("CAPTURE: Number of buffers is %u (requested %u)",
		reqbuf.count, vid->cap_buf_cnt);

	vid->cap_buf_cnt = reqbuf.count;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		memzero(buf);
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n;
		buf.m.planes = planes;
		buf.length = CAP_PLANES;

		ret = ioctl(vid->fd, VIDIOC_QUERYBUF, &buf);
		if (ret != 0) {
			err("CAPTURE: QUERYBUF failed (%s)", strerror(errno));
			return -1;
		}

		vid->cap_buf_off[n][0] = buf.m.planes[0].m.mem_offset;

		vid->cap_buf_addr[n][0] = mmap(NULL, buf.m.planes[0].length,
					       PROT_READ | PROT_WRITE,
					       MAP_SHARED,
					       vid->fd,
					       buf.m.planes[0].m.mem_offset);

		if (vid->cap_buf_addr[n][0] == MAP_FAILED) {
			err("CAPTURE: Failed to MMAP buffer");
			return -1;
		}

		vid->cap_buf_size[0] = buf.m.planes[0].length;
	}

	info("CAPTURE: querybuf: sizeimage %u", vid->cap_buf_size[0]);

	return 0;
}

int video_setup_capture_dmabuf(struct instance *i, unsigned int count,
			       unsigned int w, unsigned int h)
{
	struct video *vid = &i->video;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	int ret;

	memzero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	ret = ioctl(vid->fd, VIDIOC_S_FMT, &fmt);
	if (ret) {
		err("CAPTURE: Failed to set format (%s)", strerror(errno));
		return -1;
	}

	vid->cap_w = fmt.fmt.pix_mp.width;
	vid->cap_h = fmt.fmt.pix_mp.height;

	vid->cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	vid->cap_buf_size[1] = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;

	vid->cap_buf_cnt = count;
	vid->cap_buf_cnt_min = 1;
	vid->cap_buf_queued = 0;

	info("CAPTURE: Set format %ux%d sizeimage %u, bpl %u",
	     fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	     fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	     fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

	memzero(reqbuf);
	reqbuf.count = vid->cap_buf_cnt;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory = V4L2_MEMORY_DMABUF;

	ret = ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf);
	if (ret != 0) {
		err("CAPTURE: REQBUFS failed (%s)", strerror(errno));
		return -1;
	}

	info("CAPTURE: Number of buffers is %u (requested %u)",
		reqbuf.count, vid->cap_buf_cnt);

	vid->cap_buf_cnt = reqbuf.count;

	return 0;
}

int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, unsigned int count)
{
	struct video *vid = &i->video;
	struct v4l2_format fmt, try_fmt, g_fmt;
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;
	int n;
#if 1
	memzero(try_fmt);
	try_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	try_fmt.fmt.pix_mp.width = i->width;
	try_fmt.fmt.pix_mp.height = i->height;
	try_fmt.fmt.pix_mp.pixelformat = codec;

	ret = ioctl(vid->fd, VIDIOC_TRY_FMT, &try_fmt);
	if (ret) {
		err("OUTPUT: Failed to try format (%s)", strerror(errno));
		return -1;
	}

	info("OUTPUT: Try format %ux%u sizeimage %u, bpl %u",
	    try_fmt.fmt.pix_mp.width, try_fmt.fmt.pix_mp.height,
	    try_fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	    try_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
#endif
	memzero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.width = i->width;
	fmt.fmt.pix_mp.height = i->height;
	fmt.fmt.pix_mp.pixelformat = codec;

	ret = ioctl(vid->fd, VIDIOC_S_FMT, &fmt);
	if (ret) {
		err("OUTPUT: Failed to set format (%s)", strerror(errno));
		return -1;
	}

	info("OUTPUT: Set format %ux%u sizeimage %u, bpl %u",
	     fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	     fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	     fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

	vid->out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
#if 1
	memzero(g_fmt);
	g_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	g_fmt.fmt.pix_mp.pixelformat = codec;

	ret = ioctl(vid->fd, VIDIOC_G_FMT, &g_fmt);
	if (ret) {
		err("OUTPUT: Failed to get format (%s)", strerror(errno));
		return -1;
	} else {
		info("OUTPUT: Get format %ux%u sizeimage %u, bpl %u",
		     g_fmt.fmt.pix_mp.width, g_fmt.fmt.pix_mp.height,
		     g_fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
		     g_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
	}

#endif
	memzero(reqbuf);
	reqbuf.count = count;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	ret = ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf);
	if (ret) {
		err("OUTPUT: REQBUFS failed (%s)", strerror(errno));
		return -1;
	}

	vid->out_buf_cnt = reqbuf.count;

	info("OUTPUT: Number of buffers is %u (requested %u)",
	     vid->out_buf_cnt, count);

	for (n = 0; n < vid->out_buf_cnt; n++) {
		memzero(buf);
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n;
		buf.m.planes = planes;
		buf.length = OUT_PLANES;

		ret = ioctl(vid->fd, VIDIOC_QUERYBUF, &buf);
		if (ret != 0) {
			err("OUTPUT: QUERYBUF failed (%s)", strerror(errno));
			return -1;
		}

		vid->out_buf_off[n] = buf.m.planes[0].m.mem_offset;
		vid->out_buf_size = buf.m.planes[0].length;

		vid->out_buf_addr[n] = mmap(NULL, buf.m.planes[0].length,
					    PROT_READ | PROT_WRITE, MAP_SHARED,
					    vid->fd,
					    buf.m.planes[0].m.mem_offset);

		if (vid->out_buf_addr[n] == MAP_FAILED) {
			err("OUTPUT: Failed to MMAP buffer");
			return -1;
		}

		vid->out_buf_flag[n] = 0;
	}

	info("OUTPUT: querybuf sizeimage %u", vid->out_buf_size);

	return 0;
}
