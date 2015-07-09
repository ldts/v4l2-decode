
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

static const char *dri_path = "/dev/dri/card0";

enum {
	DEPTH = 24,
	BPP = 32,
};

#define ALIGN(val, align)	((val + (align - 1)) & ~(align - 1))

struct buffer {
	unsigned int bo_handle;
	unsigned int fb_handle;
	int dbuf_fd;
};

struct drm_dev {
	uint32_t conn_id, enc_id, crtc_id, fb_id, plane_id;
	uint32_t width, height;
	uint32_t pitch, size, handle;

	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;

	int fd;

	uint8_t *buf;
	struct drm_buffer buffers[32];

	struct drm_dev *next;
};

static struct drm_dev *pdev;

static void fatal(char *str)
{
	fprintf(stderr, "%s\n", str);
	exit(EXIT_FAILURE);
}

static void error(char *str)
{
	perror(str);
	exit(EXIT_FAILURE);
}

static void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset)
{
	uint32_t *fp;

	if ((fp = (uint32_t *) mmap(0, len, prot, flag, fd, offset)) == MAP_FAILED)
		error("mmap");
	return fp;
}

static int drm_open(const char *path)
{
	int fd, flags;
	uint64_t has_dumb;
	int ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cannot open \"%s\"\n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 ||
	     fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		fprintf(stderr, "fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		fprintf(stderr, "drmGetCap DRM_CAP_DUMB_BUFFER failed or "
			"doesn't have dumb buffer\n");
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

	if ((res = drmModeGetResources(fd)) == NULL)
		fatal("drmModeGetResources() failed");

	if (res->count_crtcs <= 0) {
		fprintf(stderr, "no Crtcs");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (conn) {
			printf("connector: ");

			if (conn->connection == DRM_MODE_CONNECTED)
				printf("connected");
			else if (conn->connection == DRM_MODE_DISCONNECTED)
				printf("disconnected");
			else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION)
				printf("unknownconnection");
			else
				printf("unknown");
			printf("\n");
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
			if ((enc = drmModeGetEncoder(fd, dev->enc_id)) == NULL)
				fatal("drmModeGetEncoder() faild");
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
		fprintf(stderr, "DESTROY_DUMB failed: %s\n", strerror(errno));
		return ret;
	}

	return 0;
}

static int buffer_create(int fd, struct drm_buffer *b, struct drm_dev *dev,
			 uint64_t *size, uint32_t pitch)
{
	struct drm_mode_create_dumb gem;
	struct drm_mode_destroy_dumb gem_destroy;
	struct drm_prime_handle prime;
	uint32_t virtual_height;
	int ret;

	/* for NV12 virtual height = height * 3 / 2 */
	virtual_height = dev->height * 3 / 2;

	memset(&gem, 0, sizeof gem);
	gem.width = dev->width;
	gem.height = virtual_height;
	gem.bpp = 8;	/* for NV12 bpp is 8 */
	gem.size = *size;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem);
	if (ret) {
		fprintf(stderr, "CREATE_DUMB failed: %s\n", strerror(errno));
		return -1;
	}

	printf("bo %u, %ux%u, pitch %u, bpp %u, size %lu (%lu)\n",
		gem.handle,
		gem.width, gem.height,
		gem.pitch,
		gem.bpp,
		gem.size,
		*size);

	b->bo_handle = gem.handle;
	*size = gem.size;

	memset(&prime, 0, sizeof prime);
	prime.handle = b->bo_handle;

	ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret) {
		fprintf(stderr, "PRIME_HANDLE_TO_FD failed: %s\n",
			strerror(errno));
		goto fail_gem;
	}

	printf("dbuf_fd = %d\n", prime.fd);

	b->dbuf_fd = prime.fd;

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t handles[4] = { 0 };
	unsigned int fourcc = DRM_FORMAT_NV12;

	offsets[0] = 0;
	handles[0] = b->bo_handle;
	pitches[0] = gem.pitch;

	offsets[1] = pitches[0] * dev->height;
	handles[1] = b->bo_handle;
	pitches[1] = pitches[0];

//	planes[0] = virtual;
//	planes[1] = virtual + offsets[1];

	ret = drmModeAddFB2(fd, dev->width, dev->height, fourcc, handles,
			    pitches, offsets, &b->fb_handle, 0);
	if (ret) {
		fprintf(stderr, "drmModeAddFB2 failed: %s\n", strerror(errno));
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
		fprintf(stderr, "DESTROY_DUMB failed: %s\n", strerror(errno));

	return -1;
}

static void drm_setup_fb(int fd, struct drm_dev *dev)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
	creq.width = dev->width;
	creq.height = dev->height;
	creq.bpp = BPP; // hard conding

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		fatal("drmIoctl DRM_IOCTL_MODE_CREATE_DUMB failed");

	dev->pitch = creq.pitch;
	dev->size = creq.size;
	dev->handle = creq.handle;

	if (drmModeAddFB(fd, dev->width, dev->height,
		DEPTH, BPP, dev->pitch, dev->handle, &dev->fb_id))
		fatal("drmModeAddFB failed");

	memset(&mreq, 0, sizeof(struct drm_mode_map_dumb));
	mreq.handle = dev->handle;

	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq))
		fatal("drmIoctl DRM_IOCTL_MODE_MAP_DUMB failed");

	dev->buf = (uint8_t *) emmap(0, dev->size, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, mreq.offset);

	/* must store crtc data */
	dev->saved_crtc = drmModeGetCrtc(fd, dev->crtc_id);

	if (drmModeSetCrtc(fd, dev->crtc_id, dev->fb_id, 0, 0, &dev->conn_id,
			   1, &dev->mode))
		fatal("drmModeSetCrtc() failed");
}

static void drm_destroy(int fd, struct drm_dev *dev_head)
{
	struct drm_dev *devp, *devp_tmp;
	struct drm_mode_destroy_dumb dreq;

	for (devp = dev_head; devp != NULL;) {
		if (devp->saved_crtc)
			drmModeSetCrtc(fd, devp->saved_crtc->crtc_id,
				       devp->saved_crtc->buffer_id,
				       devp->saved_crtc->x, devp->saved_crtc->y,
				       &devp->conn_id, 1,
				       &devp->saved_crtc->mode);

		drmModeFreeCrtc(devp->saved_crtc);

		munmap(devp->buf, devp->size);

		drmModeRmFB(fd, devp->fb_id);

		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = devp->handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

		devp_tmp = devp;
		devp = devp->next;
		free(devp_tmp);
	}

	close(fd);
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
	if (!planes)
		error("drmModeGetPlaneResources failed\n");

	printf("%s: count_planes: %u\n", __func__, planes->count_planes);

	for (i = 0; i < planes->count_planes; ++i) {
		plane = drmModeGetPlane(fd, planes->planes[i]);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane failed: %s\n", strerror(errno));
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

static int display_file(struct drm_dev *dev)
{
	struct drm_buffer *buffer = &dev->buffers[0];
	struct stat in_stat;
	int inputfd;
	uint32_t y_stride = 1280;
	uint32_t y_scanlines = 736;
	uint32_t uv_stride = 1280;
	uint32_t uv_scanlines = 368;
	uint8_t *from, *p, *to;
	int ret;
	int i;

	inputfd = open("/mnt/sdcard/frame0005.nv12", O_RDONLY);
	if (inputfd < 0) {
		fprintf(stderr, "Failed to open input file\n");
		return -1;
	}

	fstat(inputfd, &in_stat);

	p = mmap(0, in_stat.st_size, PROT_READ, MAP_SHARED, inputfd, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "Failed to map input file\n");
		goto err_close;
	}

	printf("input file size : %u\n", (unsigned int)in_stat.st_size);

	from = p;
	to = (uint8_t *)dev->buf;

	/* Y plane */
	for (i = 0; i < y_scanlines; ++i) {
		memcpy(to, from, y_stride);

		to += dev->width;
		from += y_stride;
	}

	/* UV plane */
	from = p;
	to = (uint8_t *)dev->buf;

	from += y_stride * y_scanlines;
	to += dev->width * dev->height;

	for (i = 0; i < uv_scanlines; ++i) {
		memcpy(to, from, uv_stride);

		to += dev->width;
		from += uv_stride;
	}

	munmap(p, in_stat.st_size);
	close(inputfd);

	ret = drmModeSetPlane(dev->fd, dev->plane_id, dev->crtc_id,
			      buffer->fb_handle, 0,
			      0, 0, 1280, 720,
			      0, 0, 1280 << 16, 720 << 16);
	if (ret) {
		fprintf(stderr, "SetPlane failed\n");
		goto err_unmap;
	}

	getchar();

	return 0;

err_unmap:
	munmap(p, in_stat.st_size);
err_close:
	close(inputfd);

	return -1;
}

int drm_create_bufs(struct drm_buffer *buffers, unsigned int count, int mmaped)
{
	struct drm_dev *dev = pdev;
	int fd = dev->fd;
	uint64_t size = (dev->width * dev->height * 3 / 2) / 2;
	uint32_t pitch = dev->width;
	struct drm_buffer *buf;
	int ret;

	for (unsigned int i = 0; i < count; ++i) {
		buf = &buffers[i];

		ret = buffer_create(fd, buf, dev, &size, pitch);
		if (ret) {
			fprintf(stderr, "failed to create buffer%d\n", i);
			break;
		}

		if (!mmaped)
			continue;

		struct drm_mode_map_dumb mreq;
		memset(&mreq, 0, sizeof(mreq));
		mreq.handle = buf->bo_handle;

		if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
			fprintf(stderr, "DRM_IOCTL_MODE_MAP_DUMB failed");
			goto err;
		}

		buf->mmap_buf = mmap(0, size, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, mreq.offset);
		if (buf->mmap_buf == MAP_FAILED) {
			fprintf(stderr, "mmap failed\n");
			goto err;
		}
	}

	return 0;

err:
	return -1;
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
		fprintf(stderr, "available drm_dev not found\n");
		goto err;
	}

	printf("available connector(s)\n\n");

	for (dev = dev_head; dev != NULL; dev = dev->next) {
		printf("connector id:%d\n", dev->conn_id);
		printf("\tencoder id:%d crtc id:%d fb id:%d\n", dev->enc_id,
			dev->crtc_id, dev->fb_id);
		printf("\twidth:%d height:%d\n", dev->width, dev->height);
	}

	/* FIXME: use first drm_dev */
	dev = dev_head;
	dev->fd = fd;
	pdev = dev;

	ret = find_plane(fd, &dev->plane_id, dev->crtc_id);
	if (ret) {
		fprintf(stderr, "Cannot find plane\n");
		goto err;
	}

	printf("Found NV12 plane_id: %x\n", dev->plane_id);

	return 0;

err:
	close(fd);
	return -1;
}

int drm_deinit(void)
{
	struct drm_dev *dev = pdev;

	munmap(dev->buf, dev->size);

	return 0;
}

int drm_display_buf(const void *src, struct drm_buffer *b, unsigned int size,
		    unsigned int width, unsigned int height)
{
	struct drm_dev *dev = pdev;
	uint32_t y_stride = ALIGN(width, 128);
	uint32_t y_scanlines = ALIGN(height, 32);
	uint32_t uv_stride = ALIGN(width, 128);
	uint32_t uv_scanlines = ALIGN(height / 2, 16);
	uint8_t *from;
	uint8_t *to;
	int ret;
	int i;

	if (b->mmap_buf == MAP_FAILED || b->mmap_buf == NULL)
		goto set_plane;

	from = (uint8_t *)src;
	to = b->mmap_buf;

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

