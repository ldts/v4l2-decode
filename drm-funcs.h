#ifndef INCLUDE_DRM_FUNCS_H
#define INCLUDE_DRM_FUNCS_H

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

#endif /* INCLUDE_DRM_FUNCS_H */

