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

#ifndef INCLUDE_VIDEO_H
#define INCLUDE_VIDEO_H

#include "common.h"

/* Open the video decoder device */
int video_open(struct instance *i, char *name);

/* Close the video decoder devices */
void video_close(struct instance *i);

/* Setup the OUTPUT queue. The size determines the size for the stream
 * buffer. This is the maximum size a single compressed frame can have.
 * The count is the number of the stream buffers to allocate. */
int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count);

int video_g_fmt(struct video *vid, unsigned int *width, unsigned int *height);

/* Setup the CAPTURE queue. The argument extra_buf means the number of extra
 * buffers that should added to the minimum number of buffers required
 * by MFC. The final number of buffers allocated is stored in the instance
 * structure. */
int video_setup_capture(struct instance *i, unsigned int extra_buf,
			unsigned int w, unsigned int h);
int video_setup_capture_dmabuf(struct instance *i, unsigned int count,
			       unsigned int w, unsigned int h);

/* Queue OUTPUT buffer */
int video_queue_buf_out(struct instance *i, unsigned int n, unsigned int length);

/* Queue CAPTURE buffer */
int video_queue_buf_cap(struct instance *i, unsigned int n);
int video_queue_buf_cap_dmabuf(struct instance *i, unsigned int index,
			       struct drm_buffer *b);

/* Control MFC streaming */
int video_stream(struct instance *i, enum v4l2_buf_type type, unsigned int status);

int video_export_buf(struct instance *i, unsigned int index);
int video_create_bufs(struct instance *i, unsigned int index, unsigned int count);

/* Dequeue a buffer, the structure *buf is used to return the parameters of the
 * dequeued buffer. */
int video_dequeue_output(struct instance *i, unsigned int *n);
int video_dequeue_capture(struct instance *i, unsigned int *n,
			  unsigned int *finished, unsigned int *bytesused);
int video_dequeue_capture_dmabuf(struct instance *i, unsigned int *n,
				 unsigned int *finished,
				 unsigned int *bytesused);

int video_set_control(struct instance *i);
int video_set_framerate(struct instance *i, unsigned int framerate);

int video_stop(struct instance *i);

#endif /* INCLUDE_VIDEO_H */

