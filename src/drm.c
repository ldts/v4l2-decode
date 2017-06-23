
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "drm-funcs.h"
#include "common.h"

#define DBG_TAG "  drm"

static const char *dri_path = "/dev/dri/card0";

enum {
	DEPTH = 24,
	BPP = 32,
};

#define DRM_ALIGN(val, align)	((val + (align - 1)) & ~(align - 1))

struct buffer {
	unsigned int bo_handle;
	unsigned int fb_handle;
	int dbuf_fd;
};

struct drm_dev {
	int fd;
	uint32_t conn_id, enc_id, crtc_id, fb_id, plane_id;
	uint32_t width, height;
	uint32_t pitch, size, handle;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	struct drm_dev *next;
};

static struct drm_dev *pdev;

static int drm_open(const char *path)
{
	int fd, flags;
	uint64_t has_dumb;
	int ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		err("cannot open \"%s\"\n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 ||
	     fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		err("fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		err("drmGetCap DRM_CAP_DUMB_BUFFER failed or doesn't have dumb buffer\n");
		goto err;
	}

	return fd;
err:
	close(fd);
	return -1;
}

static struct drm_dev *drm_find_dev(int fd)
{
	int i;
	struct drm_dev *dev = NULL, *dev_head = NULL;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;

	if ((res = drmModeGetResources(fd)) == NULL) {
		err("drmModeGetResources() failed");
		return NULL;
	}

	if (res->count_crtcs <= 0) {
		err("no Crtcs");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn) {
			if (conn->connection == DRM_MODE_CONNECTED) {
				dbg("drm: connector: connected");
			} else if (conn->connection == DRM_MODE_DISCONNECTED) {
				dbg("drm: connector: disconnected");
			} else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
				dbg("drm: connector: unknownconnection");
			} else {
				dbg("drm: connector: unknown");
			}
		}

		if (conn != NULL && conn->connection == DRM_MODE_CONNECTED
		    && conn->count_modes > 0) {
			dev = (struct drm_dev *) malloc(sizeof(struct drm_dev));
			memset(dev, 0, sizeof(struct drm_dev));

			dev->conn_id = conn->connector_id;
			dev->enc_id = conn->encoder_id;
			dev->next = NULL;

			memcpy(&dev->mode, &conn->modes[0], sizeof(drmModeModeInfo));
			dev->width = conn->modes[0].hdisplay;
			dev->height = conn->modes[0].vdisplay;

			/* FIXME: use default encoder/crtc pair */
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL) {
				err("drmModeGetEncoder() faild");
				goto free_res;
			}

			dev->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);

			dev->saved_crtc = NULL;

			/* create dev list */
			dev->next = dev_head;
			dev_head = dev;
		}
		drmModeFreeConnector(conn);
	}

free_res:
	drmModeFreeResources(res);

	return dev_head;
}

static int buffer_destroy(int fd, struct drm_buffer *b)
{
	struct drm_mode_destroy_dumb gem_destroy;
	int ret;

	close(b->dbuf_fd);

	memset(&gem_destroy, 0, sizeof gem_destroy);
	gem_destroy.handle = b->bo_handle,

	ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &gem_destroy);
	if (ret < 0) {
		err("DESTROY_DUMB failed: %s\n", strerror(errno));
		return ret;
	}

	return 0;
}

static int buffer_create(int fd, struct drm_buffer *b, struct drm_dev *dev,
			 uint64_t *size, uint32_t pitch,
			 uint32_t width, uint32_t height)
{
	struct drm_mode_create_dumb gem;
	struct drm_mode_destroy_dumb gem_destroy;
	struct drm_prime_handle prime;
	uint32_t virtual_height;
	int ret;

	/* for NV12 virtual height = height * 3 / 2 */
	virtual_height = dev->height * 3 / 2;
	virtual_height = DRM_ALIGN(dev->height, 32) * 3 / 2;

	info("drm bo: %dx%d, virtual_height:%d", dev->width, dev->height,
	     virtual_height);

	memset(&gem, 0, sizeof gem);
	gem.width = dev->width;
	gem.height = virtual_height;
	gem.bpp = 8;	/* for NV12 bpp is 8 */

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem);
	if (ret) {
		err("CREATE_DUMB failed: %s\n", strerror(errno));
		return -1;
	}

	info("drm bo: %u, %ux%u, pitch %u, bpp %u, size %llu (%lu)", gem.handle,
	     gem.width, gem.height, gem.pitch, gem.bpp, gem.size, *size);

	b->bo_handle = gem.handle;
	*size = gem.size;

	memset(&prime, 0, sizeof prime);
	prime.handle = b->bo_handle;

	ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret) {
		err("PRIME_HANDLE_TO_FD failed: %s\n", strerror(errno));
		goto fail_gem;
	}

	dbg("dbuf_fd = %d\n", prime.fd);

	b->dbuf_fd = prime.fd;

	uint32_t stride = DRM_ALIGN(width, 128);
	uint32_t y_scanlines = DRM_ALIGN(height, 32);
/*	uint32_t uv_scanlines = DRM_ALIGN(height / 2, 16);*/

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t handles[4] = { 0 };
	unsigned int fourcc = DRM_FORMAT_NV12;

	offsets[0] = 0;
	handles[0] = b->bo_handle;
	pitches[0] = stride;

	offsets[1] = stride * y_scanlines;
	handles[1] = b->bo_handle;
	pitches[1] = pitches[0];

	ret = drmModeAddFB2(fd, width, height, fourcc, handles,
			    pitches, offsets, &b->fb_handle, 0);
	if (ret) {
		err("drmModeAddFB2 failed: %d (%s)\n", ret, strerror(errno));
		goto fail_prime;
	}

	return 0;

fail_prime:
	close(b->dbuf_fd);

fail_gem:
	memset(&gem_destroy, 0, sizeof gem_destroy);
	gem_destroy.handle = b->bo_handle,

	ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &gem_destroy);
	if (ret)
		err("DESTROY_DUMB failed: %s\n", strerror(errno));

	return -1;
}

static int find_plane(int fd, uint32_t *plane_id, uint32_t crtc_id)
{
	drmModePlaneResPtr planes;
	drmModePlanePtr plane;
	unsigned int i;
	unsigned int j;
	int ret = 0;
	unsigned int format = DRM_FORMAT_NV12;

	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		err("drmModeGetPlaneResources failed\n");
		return -1;
	}

	info("drm: found planes %u", planes->count_planes);

	for (i = 0; i < planes->count_planes; ++i) {
		plane = drmModeGetPlane(fd, planes->planes[i]);
		if (!plane) {
			err("drmModeGetPlane failed: %s\n", strerror(errno));
			break;
		}
/*
		if (!(plane->possible_crtcs & (1 << crtc_id))) {
			drmModeFreePlane(plane);
			continue;
		}
*/
		for (j = 0; j < plane->count_formats; ++j) {
			if (plane->formats[j] == format)
				break;
		}

		if (j == plane->count_formats) {
			drmModeFreePlane(plane);
			continue;
		}

		*plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		break;
	}

	if (i == planes->count_planes)
		ret = -1;

	drmModeFreePlaneResources(planes);

	return ret;
}

int drm_dmabuf_import(struct drm_buffer *buf, unsigned int width,
		      unsigned int height)
{
	return drmPrimeFDToHandle(pdev->fd, buf->dbuf_fd, &buf->bo_handle);
}

int drm_dmabuf_addfb(struct drm_buffer *buf, uint32_t width, uint32_t height)
{
	int ret;

	if (width > pdev->width)
		width = pdev->width;
	if (height > pdev->height)
		height = pdev->height;

	width = ALIGN(width, 8);

	uint32_t stride = DRM_ALIGN(width, 128);
	uint32_t y_scanlines = DRM_ALIGN(height, 32);

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t handles[4] = { 0 };
	unsigned int fourcc = DRM_FORMAT_NV12;

	offsets[0] = 0;
	handles[0] = buf->bo_handle;
	pitches[0] = stride;

	offsets[1] = stride * y_scanlines;
	handles[1] = buf->bo_handle;
	pitches[1] = pitches[0];

	ret = drmModeAddFB2(pdev->fd, width, height, fourcc, handles,
			    pitches, offsets, &buf->fb_handle, 0);
	if (ret) {
		err("drmModeAddFB2 failed: %d (%s)\n", ret, strerror(errno));
		return ret;
	}

	return 0;
}

void drm_dmabuf_rmfb(struct drm_buffer *buf)
{
	drmModeRmFB(pdev->fd, buf->fb_handle);
}

int drm_dmabuf_set_plane(struct drm_buffer *buf, uint32_t width,
			 uint32_t height, int fullscreen)
{
	uint32_t crtc_w, crtc_h;

	crtc_w = width;
	crtc_h = height;

	if (fullscreen) {
		crtc_w = pdev->width;
		crtc_h = pdev->height;
	}

	return drmModeSetPlane(pdev->fd, pdev->plane_id, pdev->crtc_id,
		      buf->fb_handle, 0,
		      0, 0, crtc_w, crtc_h,
		      0, 0, width << 16, height << 16);
}

struct vbl_info {
	unsigned int vbl_count;
	struct timeval start;
	repaint_surface_func_t repaint;
};

static void vblank_handler(int fd, unsigned int frame, unsigned int sec,
			   unsigned int usec, void *data)
{
	struct vbl_info *info = data;
	struct timeval end;
	drmVBlank vbl;
	double t;

	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 1;
	vbl.request.signal = (unsigned long)data;

	drmWaitVBlank(fd, &vbl);

	if (info->repaint)
		info->repaint(NULL);

	info->vbl_count++;

	if (info->vbl_count == 60) {
		gettimeofday(&end, NULL);
		t = end.tv_sec + end.tv_usec * 1e-6 -
			(info->start.tv_sec + info->start.tv_usec * 1e-6);
		dbg("freq: %.02fHz", info->vbl_count / t);
		info->vbl_count = 0;
		info->start = end;
	}
}

int drm_dmabuf_vblank(repaint_surface_func_t repaint, int *finish)
{
	struct vbl_info handler_info;
	drmEventContext evctx;
	int fd = pdev->fd;
	drmVBlank vbl;
	int ret;

	handler_info.vbl_count = 0;
	gettimeofday(&handler_info.start, NULL);
	handler_info.repaint = repaint;

	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 1;
	vbl.request.signal = (unsigned long)&handler_info;

	ret = drmWaitVBlank(fd, &vbl);
	if (ret) {
		err("drmWaitVBlank (relative, event) failed ret: %i", ret);
		return -1;
	}

	/* Set up our event handler */
	memset(&evctx, 0, sizeof evctx);
	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.vblank_handler = vblank_handler;
	evctx.page_flip_handler = NULL;

	/* Poll for events */
	while (!*finish) {
		struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		ret = select(fd + 1, &fds, NULL, NULL, &timeout);

		if (ret <= 0) {
			err("select timed out or error (ret %d)", ret);
			break;
		} else if (FD_ISSET(0, &fds)) {
			err("user interrupted!");
			break;
		}

		ret = drmHandleEvent(fd, &evctx);
		if (ret) {
			err("drmHandleEvent failed: %i", ret);
			return -1;
		}
	}

	return 0;
}

static void page_flip_handler(int fd, unsigned int frame,
			      unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;

	*waiting_for_flip = 0;
}

int drm_page_flip(struct drm_buffer *buf)
{
	uint32_t crtc_id = pdev->crtc_id;
	drmEventContext evctx;
	int fd = pdev->fd;
	fd_set fds;
	int ret;
	int waiting_for_flip = 1;

#if 0
	ret = drmModeSetCrtc(fd, crtc_id, buf->fb_handle, 0, 0,
			&pdev->conn_id, 1, &pdev->mode);
	if (ret) {
		err("failed to set mode: %s", strerror(errno));
		return ret;
	}
#endif
	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(fd, &fds);

	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.vblank_handler = NULL;
	evctx.page_flip_handler = page_flip_handler;

//	info("fd:%d, crtc_id:%u, fb_handle:%u",
//		fd, crtc_id, buf->fb_handle);

	ret = drmModePageFlip(fd, crtc_id, buf->fb_handle,
			      DRM_MODE_PAGE_FLIP_EVENT,
			      &waiting_for_flip);
	if (ret) {
		err("failed to queue page flip: %d (%s)", ret, strerror(errno));
		return -1;
	}

	while (waiting_for_flip) {
		ret = select(fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			err("select err: %s", strerror(errno));
			return ret;
		} else if (ret == 0) {
			err("select timeout!");
			return -1;
		} else if (FD_ISSET(0, &fds)) {
			err("user interrupted!");
			break;
		}

		drmHandleEvent(fd, &evctx);
	}

	return 0;
}

int drm_create_bufs(struct drm_buffer *buffers, unsigned int count,
		    unsigned int width, unsigned int height, int mmaped)
{
	struct drm_dev *dev = pdev;
	int fd = dev->fd;
	uint64_t size = (dev->width * dev->height * 3 / 2) / 2;
	uint32_t pitch = dev->width;
	struct drm_buffer *buf;
	int ret;

	info("drm: create %u %s buffers with %ux%u", count,
		mmaped ? "mmaped" : "", width, height);

	if (!buffers)
		return -EINVAL;

	for (unsigned int i = 0; i < count; ++i) {
		buf = &buffers[i];

		ret = buffer_create(fd, buf, dev, &size, pitch, width, height);
		if (ret) {
			err("failed to create buffer%d\n", i);
			break;
		}

		if (!mmaped)
			continue;

		struct drm_mode_map_dumb mreq;
		memset(&mreq, 0, sizeof(mreq));
		mreq.handle = buf->bo_handle;

		ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
		if (ret) {
			err("DRM_IOCTL_MODE_MAP_DUMB failed");
			break;
		}

		buf->mmap_buf = mmap(0, size, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, mreq.offset);
		if (buf->mmap_buf == MAP_FAILED) {
			ret = -1;
			err("mmap failed\n");
			break;
		}
	}

	return ret ? ret : 0;
}

int drm_destroy_bufs(struct drm_buffer *buffers, unsigned int count, int mmaped)
{
	struct drm_dev *dev = pdev;
	uint64_t size = (dev->width * dev->height * 3 / 2) / 2;
	struct drm_buffer *buf;
	int fd = dev->fd;

	if (!buffers)
		return -EINVAL;

	for (unsigned int i = 0; i < count; ++i) {
		buf = &buffers[i];

		if (mmaped)
			munmap(buf->mmap_buf, size);

		drmModeRmFB(fd, buf->fb_handle);

		buffer_destroy(fd, buf);
	}

	return 0;
}

int drm_init(void)
{
	struct drm_dev *dev_head, *dev;
	int fd;
	int ret;

	fd = drm_open(dri_path);
	if (fd < 0)
		return -1;

	dev_head = drm_find_dev(fd);
	if (dev_head == NULL) {
		err("available drm devices not found\n");
		goto err;
	}

	dbg("available connector(s)");

	for (dev = dev_head; dev != NULL; dev = dev->next) {
		dbg("connector id:%d", dev->conn_id);
		dbg("\tencoder id:%d crtc id:%d fb id:%d", dev->enc_id,
		    dev->crtc_id, dev->fb_id);
		dbg("\twidth:%d height:%d", dev->width, dev->height);
	}

	/* FIXME: use first drm_dev */
	dev = dev_head;
	dev->fd = fd;
	pdev = dev;

	ret = find_plane(fd, &dev->plane_id, dev->crtc_id);
	if (ret) {
		err("Cannot find plane\n");
		goto err;
	}

	info("drm: Found NV12 plane_id: %x", dev->plane_id);

	return 0;

err:
	close(fd);
	pdev = NULL;
	return -1;
}

int drm_deinit(void)
{
	struct drm_dev *dev = pdev;

	if (!dev)
		return -EINVAL;

	close(dev->fd);

	return 0;
}

int drm_display_buf(const void *src, struct drm_buffer *b, unsigned int size,
		    unsigned int width, unsigned int height)
{
	struct drm_dev *dev = pdev;
	uint32_t y_stride = DRM_ALIGN(width, 128);
	uint32_t y_scanlines = DRM_ALIGN(height, 32);
	uint32_t uv_stride = DRM_ALIGN(width, 128);
	uint32_t uv_scanlines = DRM_ALIGN(height / 2, 16);
	uint8_t *from;
	uint8_t *to;
	int ret;
	int i;

	if (!b)
		return -EINVAL;

	if (b->mmap_buf == MAP_FAILED || b->mmap_buf == NULL)
		goto set_plane;

	from = (uint8_t *)src;
	to = b->mmap_buf;

	memcpy(to, from, size);
	goto set_plane;

	/* Y plane */
	for (i = 0; i < y_scanlines; ++i) {
		memcpy(to, from, y_stride);

		to += dev->width;
		from += y_stride;
	}

	/* UV plane */
	from = (uint8_t *)src;
	to = b->mmap_buf;

	from += y_stride * y_scanlines;
	to += dev->width * dev->height;

	for (i = 0; i < uv_scanlines; ++i) {
		memcpy(to, from, uv_stride);

		to += dev->width;
		from += uv_stride;
	}

set_plane:

	ret = drmModeSetPlane(dev->fd, dev->plane_id, dev->crtc_id,
			      b->fb_handle, 0,
			      0, 0, width, height,
			      0, 0, width << 16, height << 16);

	return ret;
}

