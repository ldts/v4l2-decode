#ifndef INCLUDE_DRM_FUNCS_H
#define INCLUDE_DRM_FUNCS_H

int drm_init(void);
int drm_display_buf(void *src, unsigned int size, unsigned int width,
		    unsigned int height);


#endif /* INCLUDE_DRM_FUNCS_H */

