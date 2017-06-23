#ifndef INCLUDE_DRM_FUNCS_H
#define INCLUDE_DRM_FUNCS_H

#include "display.h"

struct drm_buffer {
	unsigned int bo_handle;
	unsigned int fb_handle;
	int dbuf_fd;
	void *mmap_buf;
};

int drm_init(void);
int drm_deinit(void);
int drm_create_bufs(struct drm_buffer *buffers, unsigned int count,
		    unsigned int width, unsigned int height, int mmaped);
int drm_destroy_bufs(struct drm_buffer *buffers, unsigned int count,
		     int mmaped);
int drm_display_buf(const void *src, struct drm_buffer *b, unsigned int size,
		    unsigned int width, unsigned int height);

int drm_dmabuf_import(struct drm_buffer *buf, unsigned int width,
		      unsigned int height);
int drm_dmabuf_addfb(struct drm_buffer *buf, uint32_t width, uint32_t height);
void drm_dmabuf_rmfb(struct drm_buffer *buf);
int drm_dmabuf_set_plane(struct drm_buffer *buf, uint32_t width,
			 uint32_t height, int fullscreen);
int drm_dmabuf_vblank(repaint_surface_func_t repaint, int *finish);
int drm_page_flip(struct drm_buffer *buf);

#endif /* INCLUDE_DRM_FUNCS_H */

