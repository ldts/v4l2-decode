#ifndef DISPLAY_H_
# define DISPLAY_H_

#ifdef WAYLAND_SUPPORT
#include <wayland-client.h>
#endif
#include <stdint.h>

struct display;
struct window;
struct fb;

typedef void (*fb_release_cb_t)(struct fb *fb, void *data);

typedef void (*window_key_cb_t)(struct window *w, uint32_t time, uint32_t key,
				uint32_t state);

typedef void (*repaint_surface_func_t)(struct fb *fb);
typedef void * (*repaint_surface_func2_t)(struct fb *fb);

typedef void (*release_buffer_func_t)(void *data);

struct fb {
	struct window *window;
	int group;
	int index;
	int fd;
	int offset;
	int width;
	int height;
	int stride;
	int busy;
	int ar_x, ar_y;
	int crop_x, crop_y, crop_w, crop_h;
	uint32_t format;
	struct wl_buffer *buffer;
//	struct wp_presentation_feedback *presentation_feedback;
	fb_release_cb_t release_cb;
	void *cb_data;
	void *inst;
};

struct wl_display *display_get_wl_display(struct display *display);

struct display *display_create(void);
int display_is_running(struct display *display);
struct window *display_create_window(struct display *display,
				     repaint_surface_func_t repaint_func);
void display_destroy(struct display *display);

void window_set_user_data(struct window *w, void *data);
void *window_get_user_data(struct window *w);
void window_set_key_callback(struct window *w, window_key_cb_t handler);
void window_set_aspect_ratio(struct window *w, int ar_x, int ar_y);
void window_toggle_fullscreen(struct window *w);

void window_show_buffer(struct window *window, struct fb *fb,
			fb_release_cb_t release_cb, void *cb_data);
struct fb *window_create_buffer(struct window *window, int group,
				int index, int fd,
				int offset, uint32_t format,
				int width, int height, int stride);
void window_destroy(struct window *window);

void fb_destroy(struct fb *fb);

#endif /* !DISPLAY_H_ */
