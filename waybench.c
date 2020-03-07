#define _POSIX_C_SOURCE 200112L
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>

#include <bmp.h>

/**
 * Pixel formatis always RGBA
 */
struct wb_swsurf {
	int w, h;
	struct wlr_texture *texture;
};

struct wb_swsurf* wb_swsurf_create(unsigned int width, unsigned int height,
				   uint32_t stride, uint8_t *data,
				   struct wlr_renderer *renderer)
{
	if (!data)
		return NULL;

	struct wb_swsurf *surf = calloc(1, sizeof (struct wb_swsurf));
	if (!surf)
		return NULL;

	surf->w = width;
	surf->h = height;

	surf->texture = wlr_texture_from_pixels(renderer,
						WL_SHM_FORMAT_ARGB8888,
						stride,
						width, height, data);

	if (!surf->texture) {
		free(surf);
		return NULL;
	}

	return surf;
}

void wb_swsurf_destroy(struct wb_swsurf *surf) {
	if (!surf)
		return;

	wlr_texture_destroy(surf->texture);
	free(surf);
}

/* For brevity's sake, struct members are annotated where they are used. */
enum waybench_cursor_mode {
	WAYBENCH_CURSOR_PASSTHROUGH,
	WAYBENCH_CURSOR_MOVE,
	WAYBENCH_CURSOR_RESIZE,
};

struct waybench_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_compositor *compositor;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_list views;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener layer_shell_surface;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_list keyboards;
	enum waybench_cursor_mode cursor_mode;
	struct waybench_view *grabbed_view;
	double grab_x, grab_y;
	int grab_width, grab_height;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct waybench_output *crt_output;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener xdg_decoration;
	struct wl_list xdg_decorations; // sway_xdg_decoration::link
};

struct waybench_output {
	struct wl_list link;
	struct waybench_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_list layers[4]; // waybench_layer_surface::link
};

struct waybench_window_frame {
	double x, y;
	double w, h;

	struct wb_swsurf *titlebar;
	struct wb_swsurf *left_win_margin;
	struct wb_swsurf *right_win_margin;
	struct wb_swsurf *bottom_win_bar;

	struct wb_swsurf *btn_left[5];
	int num_btn_left;
	struct wb_swsurf *btn_right[5];
	int num_btn_right;

	struct waybench_view *view;
};

void waybench_window_frame_destroy(struct waybench_window_frame *frame) {
	wb_swsurf_destroy(frame->titlebar);
	wb_swsurf_destroy(frame->left_win_margin);
	wb_swsurf_destroy(frame->right_win_margin);
	wb_swsurf_destroy(frame->bottom_win_bar);

	free(frame);
}

#define WB_TITLEBAR_HEIGHT 18
#define WB_WINMARGIN_WIDTH 4
#define WB_BOTTOMBAR_HEIGHT 4

#define WB_TITLEBAR_BTN_HEIGHT 18
#define WB_TITLEBAR_BTN_WIDTH  32

#define WB_INACTIVE_BRIGHT    0xFFFFFFFF
#define WB_INACTIVE_NORMAL    0xFF888888
#define WB_INACTIVE_DARK      0xFF000000
#define WB_INACTIVE_NORMAL_BG WB_INACTIVE_NORMAL
#define WB_INACTIVE_BRIGHT_BG WB_INACTIVE_NORMAL + (0x00222222)
#define WB_INACTIVE_DARK_BG   WB_INACTIVE_NORMAL - (0x00222222)
#define WB_ACTIVE_BRIGHT      0xFFFFFFFF
#define WB_ACTIVE_NORMAL      0xFF506091
#define WB_ACTIVE_DARK        0xFF000000
#define WB_ACTIVE_NORMAL_BG   WB_ACTIVE_NORMAL
#define WB_ACTIVE_BRIGHT_BG   WB_ACTIVE_NORMAL + (0x00222222)
#define WB_ACTIVE_DARK_BG     WB_ACTIVE_NORMAL - (0x00222222)

struct waybench_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_xdg_decoration;
	struct wl_list link;

	struct waybench_view *view;
	struct waybench_window_frame *frame;

	struct wl_listener destroy;
	struct wl_listener request_mode;
};

struct waybench_view {
	struct wl_list link;
	struct waybench_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	bool mapped;
	int x, y;

	struct waybench_decoration *decoration;
};

struct waybench_keyboard {
	struct wl_list link;
	struct waybench_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

struct waybench_layer_surface {
	struct wlr_layer_surface_v1 *layer_surface;
	struct wl_list link;

	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener output_destroy;
	struct wl_listener new_popup;

	struct wlr_box geo;
	enum zwlr_layer_shell_v1_layer layer;
};

// Global for easier access?
static struct waybench_server server = {0};

/**
 * TODO: Break this down, obviously.
 */
static void paint_frame_titlebar(struct waybench_window_frame *frame,
				 unsigned int bright_col,
				 unsigned int normal_col,
				 unsigned int dark_col,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	int height = WB_TITLEBAR_HEIGHT;
	int width = view->xdg_surface->surface->current.width + (2 * WB_WINMARGIN_WIDTH);

	Bitmap *bmp = bm_create(width, height);

	bm_set_color(bmp, normal_col);
	bm_fillrect(bmp, 0, 0, width, height);

	bm_set_color(bmp, bright_col);
	bm_line(bmp, 0, 0, width, 0);
	bm_line(bmp, 0, 0, 0, height);

	bm_set_color(bmp, dark_col);
	bm_line(bmp, 0, height - 1, width, height - 1);
	bm_line(bmp, width - 1, 0, width - 1, height);

	frame->titlebar = wb_swsurf_create(width, height, 4 * width, bmp->data, renderer);

	bm_free(bmp);
}

static void paint_frame_lr_win_margin(struct waybench_window_frame *frame,
				      unsigned int bright_col,
				      unsigned int normal_col,
				      unsigned int dark_col,
				      struct waybench_view *view,
				      struct wlr_renderer *renderer)
{
	int height = view->xdg_surface->surface->current.height;
	int width = WB_WINMARGIN_WIDTH;

	Bitmap *bmp = bm_create(width, height);

	bm_set_color(bmp, normal_col);
	bm_fillrect(bmp, 0, 0, width, height);

	bm_set_color(bmp, bright_col);
	bm_line(bmp, 0, 0, 0, height);

	bm_set_color(bmp, dark_col);
	bm_line(bmp, width - 1, 0, width - 1, height);

	frame->left_win_margin = wb_swsurf_create(width, height, 4 * width, bmp->data, renderer);
	frame->right_win_margin = wb_swsurf_create(width, height, 4 * width, bmp->data, renderer);

	bm_free(bmp);
}

static void paint_frame_bottom(struct waybench_window_frame *frame,
			       unsigned int bright_col,
			       unsigned int normal_col,
			       unsigned int dark_col,
			       struct waybench_view *view,
			       struct wlr_renderer *renderer)
{
	int height = WB_BOTTOMBAR_HEIGHT;
	int width = view->xdg_surface->surface->current.width + (2 * WB_WINMARGIN_WIDTH);

	Bitmap *bmp = bm_create(width, height);

	bm_set_color(bmp, normal_col);
	bm_fillrect(bmp, 0, 0, width, height);

	bm_set_color(bmp, bright_col);
	bm_line(bmp, 0, 0, 0, height);
	bm_line(bmp, WB_WINMARGIN_WIDTH - 1, 0, width - WB_WINMARGIN_WIDTH, 0);

	bm_set_color(bmp, dark_col);
	bm_line(bmp, 0, height - 1, width, height - 1);
	bm_line(bmp, width - 1, 0, width - 1, height);

	frame->bottom_win_bar = wb_swsurf_create(width, height, 4 * width, bmp->data, renderer);
	bm_free(bmp);
}

static void paint_inactive_frame_titlebar(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_frame_titlebar(frame, WB_INACTIVE_BRIGHT,
			     WB_INACTIVE_NORMAL, WB_INACTIVE_DARK,
			     view, renderer);
}

static void paint_inactive_frame_lr_win_margin(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_frame_lr_win_margin(frame, WB_INACTIVE_BRIGHT,
				  WB_INACTIVE_NORMAL, WB_INACTIVE_DARK,
				  view, renderer);
}

static void paint_inactive_frame_bottom(struct waybench_window_frame *frame,
					struct waybench_view *view,
					struct wlr_renderer *renderer)
{
	paint_frame_bottom(frame, WB_INACTIVE_BRIGHT,
			   WB_INACTIVE_NORMAL, WB_INACTIVE_DARK,
			   view, renderer);
}

static void paint_active_frame_titlebar(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_frame_titlebar(frame, WB_ACTIVE_BRIGHT,
			     WB_ACTIVE_NORMAL, WB_ACTIVE_DARK,
			     view, renderer);
}

static void paint_active_frame_lr_win_margin(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_frame_lr_win_margin(frame, WB_ACTIVE_BRIGHT,
				  WB_ACTIVE_NORMAL, WB_ACTIVE_DARK,
				  view, renderer);
}

static void paint_active_frame_bottom(struct waybench_window_frame *frame,
					struct waybench_view *view,
					struct wlr_renderer *renderer)
{
	paint_frame_bottom(frame, WB_ACTIVE_BRIGHT,
			   WB_ACTIVE_NORMAL, WB_ACTIVE_DARK,
			   view, renderer);
}

static void paint_btn_close(struct waybench_window_frame *frame,
			    unsigned int bright_border_col,
			    unsigned int dark_border_col,
			    unsigned int normal_bg_col,
			    unsigned int bright_bg_col,
			    unsigned int dark_bg_col,
			    struct waybench_view *view,
			    struct wlr_renderer *renderer)
{
	int btn_width = WB_TITLEBAR_BTN_WIDTH;
	int btn_height = WB_TITLEBAR_BTN_HEIGHT;
	int sqr_size = btn_height / 3;

	Bitmap *bmp = bm_create(btn_width, btn_height);

	bm_set_color(bmp, normal_bg_col);
	bm_fillrect(bmp, 0, 0, btn_width, btn_height);

	bm_set_color(bmp, bright_border_col);
	bm_line(bmp, 0, 0, 0, btn_width);
	bm_line(bmp, 0, 0, btn_width, 0);

	bm_set_color(bmp, dark_border_col);
	bm_line(bmp, 0, btn_height - 1, btn_width - 1, btn_height - 1);
	bm_line(bmp, btn_width - 1, 0, btn_width - 1, btn_height - 1);

	bm_rect(bmp,
		(btn_width - sqr_size) / 2, sqr_size,
		(btn_width - sqr_size) / 2 + sqr_size, 2 * sqr_size);

	frame->btn_left[0] = wb_swsurf_create(btn_width, btn_height,
					      4 * btn_width, bmp->data,
					      renderer);
	
	bm_free(bmp);
}

static void paint_btn_iconify(struct waybench_window_frame *frame,
			      unsigned int bright_border_col,
			      unsigned int dark_border_col,
			      unsigned int normal_bg_col,
			      unsigned int bright_bg_col,
			      unsigned int dark_bg_col,
			      struct waybench_view *view,
			      struct wlr_renderer *renderer)
{
	int btn_width = WB_TITLEBAR_BTN_WIDTH;
	int btn_height = WB_TITLEBAR_BTN_HEIGHT;
	int sqr_size = btn_height / 3;

	Bitmap *bmp = bm_create(btn_width, btn_height);

	bm_set_color(bmp, normal_bg_col);
	bm_fillrect(bmp, 0, 0, btn_width, btn_height);

	bm_set_color(bmp, bright_border_col);
	bm_line(bmp, 0, 0, 0, btn_width);
	bm_line(bmp, 0, 0, btn_width, 0);

	bm_set_color(bmp, dark_border_col);
	bm_line(bmp, 0, btn_height - 1, btn_width - 1, btn_height - 1);
	bm_line(bmp, btn_width - 1, 0, btn_width - 1, btn_height - 1);

	bm_rect(bmp,
		(btn_width - sqr_size) / 2, sqr_size,
		(btn_width - sqr_size) / 2 + sqr_size, 2 * sqr_size);

	frame->btn_right[4] = wb_swsurf_create(btn_width, btn_height,
					       4 * btn_width, bmp->data,
					       renderer);
	
	bm_free(bmp);
}

static void paint_btn_raise(struct waybench_window_frame *frame,
			    unsigned int bright_border_col,
			    unsigned int dark_border_col,
			    unsigned int normal_bg_col,
			    unsigned int bright_bg_col,
			    unsigned int dark_bg_col,
			    struct waybench_view *view,
			    struct wlr_renderer *renderer)
{
	int btn_width = WB_TITLEBAR_BTN_WIDTH;
	int btn_height = WB_TITLEBAR_BTN_HEIGHT;
	int sqr_size = btn_height / 3;

	Bitmap *bmp = bm_create(btn_width, btn_height);

	bm_set_color(bmp, normal_bg_col);
	bm_fillrect(bmp, 0, 0, btn_width, btn_height);

	bm_set_color(bmp, bright_border_col);
	bm_line(bmp, 0, 0, 0, btn_width);
	bm_line(bmp, 0, 0, btn_width, 0);

	bm_set_color(bmp, dark_border_col);
	bm_line(bmp, 0, btn_height - 1, btn_width - 1, btn_height - 1);
	bm_line(bmp, btn_width - 1, 0, btn_width - 1, btn_height - 1);

	bm_rect(bmp,
		(btn_width - sqr_size) / 2, sqr_size,
		(btn_width - sqr_size) / 2 + sqr_size, 2 * sqr_size);

	frame->btn_right[3] = wb_swsurf_create(btn_width, btn_height,
					      4 * btn_width, bmp->data,
					      renderer);
	
	bm_free(bmp);
}

static void paint_active_btn_close(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_close(frame,
			WB_ACTIVE_BRIGHT,
			WB_ACTIVE_DARK,
			WB_ACTIVE_NORMAL_BG,
			WB_ACTIVE_BRIGHT_BG,
			WB_ACTIVE_DARK_BG,
			view, renderer);
}

static void paint_inactive_btn_close(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_close(frame,
			WB_INACTIVE_BRIGHT,
			WB_INACTIVE_DARK,
			WB_INACTIVE_NORMAL_BG,
			WB_INACTIVE_BRIGHT_BG,
			WB_INACTIVE_DARK_BG,
			view, renderer);
}

static void paint_active_btn_iconify(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_iconify(frame,
			WB_ACTIVE_BRIGHT,
			WB_ACTIVE_DARK,
			WB_ACTIVE_NORMAL_BG,
			WB_ACTIVE_BRIGHT_BG,
			WB_ACTIVE_DARK_BG,
			view, renderer);
}

static void paint_inactive_btn_iconify(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_iconify(frame,
			WB_INACTIVE_BRIGHT,
			WB_INACTIVE_DARK,
			WB_INACTIVE_NORMAL_BG,
			WB_INACTIVE_BRIGHT_BG,
			WB_INACTIVE_DARK_BG,
			view, renderer);
}

static void paint_active_btn_raise(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_raise(frame,
			WB_ACTIVE_BRIGHT,
			WB_ACTIVE_DARK,
			WB_ACTIVE_NORMAL_BG,
			WB_ACTIVE_BRIGHT_BG,
			WB_ACTIVE_DARK_BG,
			view, renderer);
}

static void paint_inactive_btn_raise(struct waybench_window_frame *frame,
				   struct waybench_view *view,
				   struct wlr_renderer *renderer)
{
	paint_btn_raise(frame,
			WB_INACTIVE_BRIGHT,
			WB_INACTIVE_DARK,
			WB_INACTIVE_NORMAL_BG,
			WB_INACTIVE_BRIGHT_BG,
			WB_INACTIVE_DARK_BG,
			view, renderer);
}

static void paint_inactive_frame(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_inactive_frame_titlebar(frame, view, renderer);
	paint_inactive_frame_lr_win_margin(frame, view, renderer);
	paint_inactive_frame_bottom(frame, view, renderer);

	paint_inactive_btn_close(frame, view, renderer);
	paint_inactive_btn_iconify(frame, view, renderer);
	paint_inactive_btn_raise(frame, view, renderer);
}

static void paint_active_frame(struct waybench_window_frame *frame,
				 struct waybench_view *view,
				 struct wlr_renderer *renderer)
{
	paint_active_frame_titlebar(frame, view, renderer);
	paint_active_frame_lr_win_margin(frame, view, renderer);
	paint_active_frame_bottom(frame, view, renderer);

	paint_active_btn_close(frame, view, renderer);
	paint_active_btn_iconify(frame, view, renderer);
	paint_active_btn_raise(frame, view, renderer);
}

static struct waybench_window_frame* wbframe_create(struct waybench_view *view,
						    struct wlr_renderer *renderer,
						    bool active)
{
	int width = view->xdg_surface->surface->current.width;
	int height = view->xdg_surface->surface->current.height;

	struct waybench_window_frame *frame = calloc(1, sizeof(struct waybench_window_frame));
	if (!frame)
		return NULL;

	/* Currently hardcoded */
	frame->num_btn_left = 1;
	frame->num_btn_right = 2;

	if (active)
		paint_active_frame(frame, view, renderer);
	else
		paint_inactive_frame(frame, view, renderer);

	// TODO: These are really *titlebar* coordinates, not frame!
	frame->w = width + WB_WINMARGIN_WIDTH;
	frame->h = height;
	frame->x = view->x - WB_WINMARGIN_WIDTH;
	frame->y = view->y - WB_TITLEBAR_HEIGHT;

	frame->view = view;

	return frame;
}

static struct waybench_view* wb_frame_view(struct waybench_window_frame *frame) {
	return frame->view;
}

static void unfocus_view(struct waybench_view *view) {
	if (!view)
		return;

	struct waybench_decoration *deco = view->decoration;

	if (!deco)
		return;

	if (deco->frame) {
		waybench_window_frame_destroy(deco->frame);
		deco->frame = NULL;
	}
	
	deco->frame = wbframe_create(view, server.renderer, false);
}

static void focus_view(struct waybench_view *view, struct wlr_surface *surface) {
	if (view == NULL) {
		return;
	}
	struct waybench_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		wlr_log(WLR_INFO, "Already focused: %p\n", prev_surface);
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		struct waybench_view *view = (struct waybench_view *)(previous->data);
		unfocus_view(view);
		wlr_xdg_toplevel_set_activated(previous, false);
	}

	struct waybench_decoration *deco = view->decoration;
	if (!deco)
		return;

	if (deco->frame) {
		waybench_window_frame_destroy(deco->frame);
		deco->frame = NULL;
	}
	
	deco->frame = wbframe_create(view, server->renderer, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the view to the front */
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct waybench_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}

static bool handle_keybinding(struct waybench_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the next view */
		if (wl_list_length(&server->views) < 2) {
			break;
		}
		struct waybench_view *current_view = wl_container_of(
			server->views.next, current_view, link);
		struct waybench_view *next_view = wl_container_of(
			current_view->link.next, next_view, link);
		focus_view(next_view, next_view->xdg_surface->surface);
		/* Move the previous view to the end of the list */
		wl_list_remove(&current_view->link);
		wl_list_insert(server->views.prev, &current_view->link);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct waybench_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct waybench_server *server = keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) && event->state == WLR_KEY_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void server_new_keyboard(struct waybench_server *server,
		struct wlr_input_device *device) {
	struct waybench_keyboard *keyboard =
		calloc(1, sizeof(struct waybench_keyboard));
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_rule_names rules = { 0 };
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct waybench_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct waybench_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In Waybench we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct waybench_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is rasied by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void begin_interactive(struct waybench_view *view,
		enum waybench_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propegating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct waybench_server *server = view->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
#if 0
	if (view->xdg_surface->surface != focused_surface) {
		/* Deny move/resize requests from unfocused clients. */
		wlr_log(WLR_INFO, "Candidate: %p but focused is %p\n",
			view->xdg_surface->surface, focused_surface);
		return;
	}
#endif
	server->grabbed_view = view;
	server->cursor_mode = mode;
	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);
	if (mode == WAYBENCH_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		server->grab_x = server->cursor->x + geo_box.x;
		server->grab_y = server->cursor->y + geo_box.y;
	}
	server->grab_width = geo_box.width;
	server->grab_height = geo_box.height;
	server->resize_edges = edges;
}

static struct waybench_window_frame* frame_at(double sx, double sy) {

	struct waybench_view *view;
	struct waybench_window_frame *f;

	wl_list_for_each(view, &server.views, link) {
		if (!view->decoration)
			continue;

		f = view->decoration->frame;
		if (!f)
			continue;

		if (f->x < sx && f->x + f->w > sx &&
		    f->y < sy && f->y + f->h > sy) {
			return f;
		}
	}

	return NULL;
}

static bool view_at(struct waybench_view *view,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	/*
	 * XDG toplevels may have nested surfaces, such as popup windows for context
	 * menus or tooltips. This function tests if any of those are underneath the
	 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
	 * surface pointer to that wlr_surface and the sx and sy coordinates to the
	 * coordinates relative to that surface's top-left corner.
	 */
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

	struct wlr_surface_state *state = &view->xdg_surface->surface->current;

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
	_surface = wlr_xdg_surface_surface_at(
			view->xdg_surface, view_sx, view_sy, &_sx, &_sy);

	if (_surface != NULL) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}

	return false;
}

static struct waybench_view *desktop_view_at(
		struct waybench_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This iterates over all of our surfaces and attempts to find one under the
	 * cursor. This relies on server->views being ordered from top-to-bottom. */
	struct waybench_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}
	return NULL;
}

static struct waybench_decoration *desktop_titlebar_at(
	struct waybench_server *server, double lx, double ly) {
	/* This iterates over all of our surfaces and attempts to find one under the
	 * cursor. This relies on server->views being ordered from top-to-bottom. */
	struct waybench_view *view;
	struct waybench_decoration *deco;

	return NULL;
}

static void process_cursor_move(struct waybench_server *server, uint32_t time) {
	/* Move the grabbed view to the new position. */
	struct waybench_decoration *deco = server->grabbed_view->decoration;

	server->grabbed_view->x = server->cursor->x - server->grab_x;
	server->grabbed_view->y = server->cursor->y - server->grab_y;

	deco->frame->x = server->grabbed_view->x - WB_WINMARGIN_WIDTH;
	deco->frame->y = server->grabbed_view->y - WB_TITLEBAR_HEIGHT;
}

static void process_cursor_resize(struct waybench_server *server, uint32_t time) {
	/*
	 * Resizing the grabbed view can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the view
	 * on one or two axes, but can also move the view if you resize from the top
	 * or left edges (or top-left corner).
	 *
	 * Note that I took some shortcuts here. In a more fleshed-out compositor,
	 * you'd wait for the client to prepare a buffer at the new size, then
	 * commit any movement that was prepared.
	 */
	struct waybench_view *view = server->grabbed_view;
	double dx = server->cursor->x - server->grab_x;
	double dy = server->cursor->y - server->grab_y;
	double x = view->x;
	double y = view->y;
	int width = server->grab_width;
	int height = server->grab_height;
	if (server->resize_edges & WLR_EDGE_TOP) {
		y = server->grab_y + dy;
		height -= dy;
		if (height < 1) {
			y += height;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		height += dy;
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		x = server->grab_x + dx;
		width -= dx;
		if (width < 1) {
			x += width;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		width += dx;
	}
	view->x = x;
	view->y = y;
	wlr_xdg_toplevel_set_size(view->xdg_surface, width, height);
}

static void process_cursor_motion(struct waybench_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == WAYBENCH_CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == WAYBENCH_CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	/* Otherwise, find the view under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct waybench_view *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!view) {
		/* If there's no view under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any views. */
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr", server->cursor);
	}

	if (surface) {
		bool focus_changed = seat->pointer_state.focused_surface != surface;
		/*
		 * "Enter" the surface if necessary. This lets the client know that the
		 * cursor has entered one of its surfaces.
		 *
		 * Note that this gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		if (!focus_changed) {
			/* The enter event contains coordinates, so we only need to notify
			 * on motion if the focus did not change. */
			wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		}
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct waybench_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, event->device,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct waybench_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;
	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct waybench_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface;
	struct waybench_view *view = desktop_view_at(server, server->cursor->x,
						     server->cursor->y, &surface, &sx, &sy);

	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		server->cursor_mode = WAYBENCH_CURSOR_PASSTHROUGH;
	} else {
		/* Focus that client if the button was _pressed_ */
		if (view) {
			focus_view(view, surface);
		} else {
			struct waybench_window_frame *frame = frame_at(server->cursor->x,
								       server->cursor->y);
			if (frame) {
				view = wb_frame_view(frame);
				wlr_log(WLR_INFO, "Start interactive move for"
					"frame %p, view %p, surf %p\n",
					frame, view, view->xdg_surface->surface);
				focus_view(view, view->xdg_surface->surface);
				begin_interactive(view, WAYBENCH_CURSOR_MOVE, 0);
			}
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct waybench_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct waybench_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Used to move all of the data necessary to render a surface from the top-level
 * frame handler to the per-surface render function. */
struct render_data {
	struct wlr_output *output;
	struct wlr_renderer *renderer;
	struct waybench_view *view;
	struct timespec *when;
};

static void render_layer_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct render_data *rdata = data;
	struct wlr_output *output = rdata->output;

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == NULL) {
		return;
	}

	// TODO: I guess we should get ox, oy adjusted by output layout?
	float matrix[9];
	struct wlr_box box = {
		.x = sx,
		.y = sy,
		.width = surface->current.width,
		.height = surface->current.height,
		
	};
	enum wl_output_transform transform =
		wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0,
		output->transform_matrix);

	/* This takes our matrix, the texture, and an alpha, and performs the actual
	 * rendering on the GPU. */
	wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

	/* This lets the client know that we've displayed that frame and it can
	 * prepare another one now if it likes. */
	wlr_surface_send_frame_done(surface, rdata->when);
	
}

static void render_surface(struct wlr_surface *surface,
		int sx, int sy, void *data) {
	/* This function is called for every surface that needs to be rendered. */
	struct render_data *rdata = data;
	struct waybench_view *view = rdata->view;
	struct wlr_output *output = rdata->output;

	/* We first obtain a wlr_texture, which is a GPU resource. wlroots
	 * automatically handles negotiating these with the client. The underlying
	 * resource could be an opaque handle passed from the client, or the client
	 * could have sent a pixel buffer which we copied to the GPU, or a few other
	 * means. You don't have to worry about this, wlroots takes care of it. */
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (texture == NULL) {
		return;
	}

	/* The view has a position in layout coordinates. If you have two displays,
	 * one next to the other, both 1080p, a view on the rightmost display might
	 * have layout coordinates of 2000,100. We need to translate that to
	 * output-local coordinates, or (2000 - 1920). */
	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(
			view->server->output_layout, output, &ox, &oy);
	ox += view->x + sx, oy += view->y + sy;

	/* We also have to apply the scale factor for HiDPI outputs. This is only
	 * part of the puzzle, Waybench does not fully support HiDPI. */
	struct wlr_box box = {
		.x = ox * output->scale,
		.y = oy * output->scale,
		.width = surface->current.width * output->scale,
		.height = surface->current.height * output->scale,
	};

	/*
	 * Those familiar with OpenGL are also familiar with the role of matricies
	 * in graphics programming. We need to prepare a matrix to render the view
	 * with. wlr_matrix_project_box is a helper which takes a box with a desired
	 * x, y coordinates, width and height, and an output geometry, then
	 * prepares an orthographic projection and multiplies the necessary
	 * transforms to produce a model-view-projection matrix.
	 *
	 * Naturally you can do this any way you like, for example to make a 3D
	 * compositor.
	 */
	float matrix[9];
	enum wl_output_transform transform =
		wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0,
		output->transform_matrix);

	/* This takes our matrix, the texture, and an alpha, and performs the actual
	 * rendering on the GPU. */
	wlr_render_texture_with_matrix(rdata->renderer, texture, matrix, 1);

	/* This lets the client know that we've displayed that frame and it can
	 * prepare another one now if it likes. */
	wlr_surface_send_frame_done(surface, rdata->when);
}

static void render_win_frame(struct render_data *rdata)
{
	struct waybench_window_frame *frame = rdata->view->decoration->frame;

	wlr_render_texture(rdata->renderer, frame->titlebar->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x - WB_WINMARGIN_WIDTH,
			   rdata->view->y - WB_TITLEBAR_HEIGHT, 1);

	wlr_render_texture(rdata->renderer, frame->btn_left[0]->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x - WB_WINMARGIN_WIDTH,
			   rdata->view->y - WB_TITLEBAR_HEIGHT, 1);

	wlr_render_texture(rdata->renderer, frame->btn_right[4]->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x + frame->w - WB_TITLEBAR_BTN_WIDTH,
			   rdata->view->y - WB_TITLEBAR_HEIGHT, 1);

	wlr_render_texture(rdata->renderer, frame->btn_right[3]->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x + frame->w - (2 * WB_TITLEBAR_BTN_WIDTH),
			   rdata->view->y - WB_TITLEBAR_HEIGHT, 1);

	wlr_render_texture(rdata->renderer, frame->left_win_margin->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x - WB_WINMARGIN_WIDTH,
			   rdata->view->y, 1);

	wlr_render_texture(rdata->renderer, frame->right_win_margin->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x + rdata->view->xdg_surface->surface->current.width,
			   rdata->view->y, 1);

	wlr_render_texture(rdata->renderer, frame->bottom_win_bar->texture,
			   rdata->output->transform_matrix,
			   rdata->view->x - WB_WINMARGIN_WIDTH,
			   rdata->view->y + rdata->view->xdg_surface->surface->current.height,
			   1);
}

static void render_layer(struct waybench_output *output, struct wl_list *layer_surfaces) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct waybench_layer_surface *layer_surface;
	struct render_data rdata = {
		.output = output->wlr_output,
		.renderer = output->server->renderer,
		.when = &now
	};

	wl_list_for_each(layer_surface, layer_surfaces, link) {
		struct wlr_layer_surface_v1 *wlr_layer_surface_v1 =
			layer_surface->layer_surface;

		wlr_surface_for_each_surface(wlr_layer_surface_v1->surface,
			render_layer_surface, &rdata);
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct waybench_output *output =
		wl_container_of(listener, output, frame);
	struct wlr_renderer *renderer = output->server->renderer;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	/* wlr_output_attach_render makes the OpenGL context current. */
	if (!wlr_output_attach_render(output->wlr_output, NULL)) {
		return;
	}
	/* The "effective" resolution can change if you rotate your outputs. */
	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);
	/* Begin the renderer (calls glViewport and some other GL sanity checks) */
	wlr_renderer_begin(renderer, width, height);

	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

	render_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]);
	render_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]);

	/* Each subsequent window we render is rendered on top of the last. Because
	 * our view list is ordered front-to-back, we iterate over it backwards. */
	struct waybench_view *view;
	wl_list_for_each_reverse(view, &output->server->views, link) {
		if (!view->mapped) {
			/* An unmapped view should not be rendered. */
			continue;
		}
		struct render_data rdata = {
			.output = output->wlr_output,
			.view = view,
			.renderer = renderer,
			.when = &now,
		};
		/* Render decoration */
		render_win_frame(&rdata);

		/* This calls our render_surface function for each surface among the
		 * xdg_surface's toplevel and popups. */
		wlr_xdg_surface_for_each_surface(view->xdg_surface,
				render_surface, &rdata);
	}

	render_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]);

	/* Hardware cursors are rendered by the GPU on a separate plane, and can be
	 * moved around without re-rendering what's beneath them - which is more
	 * efficient. However, not all hardware supports hardware cursors. For this
	 * reason, wlroots provides a software fallback, which we ask it to render
	 * here. wlr_cursor handles configuring hardware vs software cursors for you,
	 * and this function is a no-op when hardware cursors are in use. */
	wlr_output_render_software_cursors(output->wlr_output, NULL);

	/* Conclude rendering and swap the buffers, showing the final frame
	 * on-screen. */
	wlr_renderer_end(renderer);
	wlr_output_commit(output->wlr_output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is rasied by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct waybench_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output, mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}

	/* Allocates and configures our state for this output */
	struct waybench_output *output =
		calloc(1, sizeof(struct waybench_output));
	output->wlr_output = wlr_output;
	output->wlr_output->data = output;
	output->server = server;


	/* Sets up a listener for the frame notify event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout. */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	/* Initialize layers */
	size_t len = sizeof(output->layers) / sizeof(output->layers[0]);
	for (size_t i = 0; i < len; ++i) {
		wl_list_init(&output->layers[i]);
	}

	/* Creating the global adds a wl_output global to the display, which Wayland
	 * clients can see to find out information about the output (such as
	 * DPI, scale factor, manufacturer, etc). */
	wlr_output_create_global(wlr_output);

	if (server->crt_output == NULL)
		server->crt_output = output;
}

static void xdg_surface_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct waybench_view *view = wl_container_of(listener, view, map);
	view->mapped = true;

	struct waybench_decoration *deco = view->decoration;

	if (deco->frame) {
		waybench_window_frame_destroy(deco->frame);
		deco->frame = NULL;
	}
	deco->frame = wbframe_create(view, server.renderer, true);
	wlr_log(WLR_INFO, "New frame: %p, view=%p\n", deco->frame, deco->frame->view);
	wlr_log(WLR_INFO, "Mapped and focusing: %p, surf=%p\n", view, view->xdg_surface->surface);
	focus_view(view, view->xdg_surface->surface);
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct waybench_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct waybench_view *view = wl_container_of(listener, view, destroy);

	if (view->decoration)
		waybench_window_frame_destroy(view->decoration->frame);

	wl_list_remove(&view->link);
	free(view);
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provied serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct waybench_view *view = wl_container_of(listener, view, request_move);
	begin_interactive(view, WAYBENCH_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provied serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct waybench_view *view = wl_container_of(listener, view, request_resize);
	begin_interactive(view, WAYBENCH_CURSOR_RESIZE, event->edges);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct waybench_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;
	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	/* Allocate a waybench_view for this surface */
	struct waybench_view *view =
		calloc(1, sizeof(struct waybench_view));
	view->server = server;
	view->xdg_surface = xdg_surface;
	/* TODO: this should be a waybench_xdg_shell_view */
	view->xdg_surface->data = view;

	/* Listen to the various events it can emit */
	view->map.notify = xdg_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	view->x = 120;
	view->y = 80;

	/* cotd */
	struct wlr_xdg_toplevel *toplevel = xdg_surface->toplevel;
	view->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);

	/* Add it to the list of views. */
	wl_list_insert(&server->views, &view->link);
}

static void xdg_decoration_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct waybench_decoration *deco = wl_container_of(listener, deco, destroy);
	wlr_log(WLR_INFO, "Destroy handler called for decoration %p", deco);

	if (deco->view)
		deco->view->decoration = NULL;

	wl_list_remove(&deco->destroy.link);
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->link);

	free(deco);
}

static void xdg_decoration_handle_request_mode(struct wl_listener *listener,
		void *data) {
	struct waybench_decoration *deco = wl_container_of(listener, deco, request_mode);
	wlr_log(WLR_INFO, "Decoration %p set mode: server side", deco);
	wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr_xdg_decoration,
						WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void handle_xdg_decoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *wlr_deco = data;
	struct waybench_decoration *deco= calloc(1, sizeof(*deco));
	if (deco == NULL)
		return;

	deco->view = wlr_deco->surface->data;
	deco->view->decoration = deco;
	deco->wlr_xdg_decoration = wlr_deco;

	wl_signal_add(&wlr_deco->events.destroy, &deco->destroy);
	deco->destroy.notify = xdg_decoration_handle_destroy;

	wl_signal_add(&wlr_deco->events.request_mode, &deco->request_mode);
	deco->request_mode.notify = xdg_decoration_handle_request_mode;

	wl_list_insert(&server.xdg_decorations, &deco->link);

	xdg_decoration_handle_request_mode(&deco->request_mode, wlr_deco);

	wlr_log(WLR_INFO, "XDG decoration: surface %p, view %p", wlr_deco->surface, deco->view);
}

static void apply_exclusive(struct wlr_box *usable_area,
		uint32_t anchor, int32_t exclusive,
		int32_t margin_top, int32_t margin_right,
		int32_t margin_bottom, int32_t margin_left) {
	if (exclusive <= 0) {
		return;
	}
	struct {
		uint32_t anchors;
		int *positive_axis;
		int *negative_axis;
		int margin;
	} edges[] = {
		{
			.anchors =
				ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
			.positive_axis = &usable_area->y,
			.negative_axis = &usable_area->height,
			.margin = margin_top,
		},
		{
			.anchors =
				ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = NULL,
			.negative_axis = &usable_area->height,
			.margin = margin_bottom,
		},
		{
			.anchors =
				ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = &usable_area->x,
			.negative_axis = &usable_area->width,
			.margin = margin_left,
		},
		{
			.anchors =
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
			.positive_axis = NULL,
			.negative_axis = &usable_area->width,
			.margin = margin_right,
		},
	};
	for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
		if ((anchor & edges[i].anchors) == edges[i].anchors && exclusive + edges[i].margin > 0) {
			if (edges[i].positive_axis) {
				*edges[i].positive_axis += exclusive + edges[i].margin;
			}
			if (edges[i].negative_axis) {
				*edges[i].negative_axis -= exclusive + edges[i].margin;
			}
		}
	}
}

static void arrange_layer(struct waybench_output *output, struct wl_list *list,
		struct wlr_box *usable_area, bool exclusive) {

	struct waybench_layer_surface *waybench_layer;
	struct wlr_box full_area = {0};

	wlr_output_effective_resolution(output->wlr_output,
			&full_area.width, &full_area.height);

	wl_list_for_each(waybench_layer, list, link) {
		struct wlr_layer_surface_v1 *layer = waybench_layer->layer_surface;
		struct wlr_layer_surface_v1_state *state = &layer->current;
		if (exclusive != (state->exclusive_zone > 0)) {
			continue;
		}
		struct wlr_box bounds;
		if (state->exclusive_zone == -1) {
			bounds = full_area;
		} else {
			bounds = *usable_area;
		}
		struct wlr_box box = {
			.width = state->desired_width,
			.height = state->desired_height
		};
		// Horizontal axis
		const uint32_t both_horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		if ((state->anchor & both_horiz) && box.width == 0) {
			box.x = bounds.x;
			box.width = bounds.width;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
			box.x = bounds.x;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			box.x = bounds.x + (bounds.width - box.width);
		} else {
			box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
		}
		// Vertical axis
		const uint32_t both_vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
			| ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		if ((state->anchor & both_vert) && box.height == 0) {
			box.y = bounds.y;
			box.height = bounds.height;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
			box.y = bounds.y;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			box.y = bounds.y + (bounds.height - box.height);
		} else {
			box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
		}
		// Margin
		if ((state->anchor & both_horiz) == both_horiz) {
			box.x += state->margin.left;
			box.width -= state->margin.left + state->margin.right;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
			box.x += state->margin.left;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			box.x -= state->margin.right;
		}
		if ((state->anchor & both_vert) == both_vert) {
			box.y += state->margin.top;
			box.height -= state->margin.top + state->margin.bottom;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
			box.y += state->margin.top;
		} else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			box.y -= state->margin.bottom;
		}
		if (box.width < 0 || box.height < 0) {
			// TODO: Bubble up a protocol error?
			wlr_layer_surface_v1_close(layer);
			continue;
		}
		
		waybench_layer->geo = box;
		apply_exclusive(usable_area, state->anchor, state->exclusive_zone,
				state->margin.top, state->margin.right,
				state->margin.bottom, state->margin.left);
		wlr_layer_surface_v1_configure(layer, box.width, box.height);
	}
}

void arrange_layers(struct waybench_output *output) {
	struct wlr_box usable_area = { 0 };
	wlr_output_effective_resolution(output->wlr_output,
			&usable_area.width, &usable_area.height);

	// Arrange exclusive surfaces from top->bottom
	arrange_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
			&usable_area, true);
	arrange_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
			&usable_area, true);
	arrange_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM],
			&usable_area, true);
	arrange_layer(output, &output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
			&usable_area, true);
}

static void layer_handle_surface_commit(struct wl_listener *listener, void *data) {
	struct waybench_layer_surface *layer =
		wl_container_of(listener, layer, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	struct wlr_output *wlr_output = layer_surface->output;

	if (wlr_output == NULL) {
		wlr_log(WLR_DEBUG, "Surface output is NULL, bailing out.");
		return;
	}

	struct waybench_output *output = wlr_output->data;
	struct wlr_box old_geo = layer->geo;
	arrange_layers(output);

	// TODO: Damage support, but I guess that's a global TODO :-)
}

static void layer_handle_destroy(struct wl_listener *listener, void *data) {
	struct waybench_layer_surface *waybench_layer = 
		wl_container_of(listener, waybench_layer, destroy);
	wlr_log(WLR_DEBUG, "Layer surface destroyed (%s)",
		waybench_layer->layer_surface->namespace);

#if 0 // TODO: unamp is NYI
	if (waybench_layer->layer_surface->mapped)
		unmap(waybench_layer);
#endif

	wl_list_remove(&waybench_layer->link);
	wl_list_remove(&waybench_layer->destroy.link);
	wl_list_remove(&waybench_layer->map.link);
	wl_list_remove(&waybench_layer->unmap.link);
	wl_list_remove(&waybench_layer->surface_commit.link);
	wl_list_remove(&waybench_layer->new_popup.link);

	if (waybench_layer->layer_surface->output != NULL) {
		struct waybench_output *output = waybench_layer->layer_surface->output->data;

		if (output) {
			arrange_layers(output);
		}
#if 0 // TODO
		wl_list_remove(&waybench_layer->output_destroy.link);
#endif
		waybench_layer->layer_surface->output = NULL;
	}

	free(waybench_layer);
}

static void layer_handle_map(struct wl_listener *listener, void *data) {
	struct waybench_layer_surface *waybench_layer = wl_container_of(listener,
									waybench_layer,
									map);
	struct waybench_output *output = waybench_layer->layer_surface->output->data;

	wlr_surface_send_enter(waybench_layer->layer_surface->surface,
			waybench_layer->layer_surface->output);
}

static void layer_handle_unmap(struct wl_listener *listener, void *data) {
}

static void layer_handle_new_popup(struct wl_listener *listener, void *data) {
}

static void layer_handle_output_destroy(struct wl_listener *listener, void *data) {
}

void handle_layer_shell_surface(struct wl_listener *listener, void *data) {
	struct wlr_layer_surface_v1 *layer_surface = data;

	wlr_log(WLR_INFO, "new layer surface: namespace %s layer %d anchor %d "
		"size %dx%d margin %d,%d,%d,%d",
		layer_surface->namespace,
		layer_surface->client_pending.layer,
		layer_surface->client_pending.layer,
		layer_surface->client_pending.desired_width,
		layer_surface->client_pending.desired_height,
		layer_surface->client_pending.margin.top,
		layer_surface->client_pending.margin.right,
		layer_surface->client_pending.margin.bottom,
		layer_surface->client_pending.margin.left);

	if (!layer_surface->output) {
		struct waybench_output *output = server.crt_output;
		layer_surface->output = output->wlr_output;
	}

	struct waybench_layer_surface *waybench_layer =
		calloc(1, sizeof(struct waybench_layer_surface));
	if (!waybench_layer)
		return;

	waybench_layer->surface_commit.notify = layer_handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit,
		&waybench_layer->surface_commit);

	waybench_layer->destroy.notify = layer_handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &waybench_layer->destroy);
	waybench_layer->map.notify = layer_handle_map;
	wl_signal_add(&layer_surface->events.map, &waybench_layer->map);
	waybench_layer->unmap.notify = layer_handle_unmap;
	wl_signal_add(&layer_surface->events.unmap, &waybench_layer->unmap);
	waybench_layer->new_popup.notify = layer_handle_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &waybench_layer->new_popup);

	waybench_layer->layer_surface = layer_surface;
	layer_surface->data = waybench_layer;

	struct waybench_output *output = layer_surface->output->data;
#if 0 // TODO: We ought to handle this, yeah?
	waybench_layer->output_destroy.notify = layer_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &waybench_layer->output_destroy);
#endif

	wl_list_insert(&output->layers[layer_surface->client_pending.layer],
			&waybench_layer->link);

	// Temporarily set the layer's current state to client_pending
	// So that we can easily arrange it
	struct wlr_layer_surface_v1_state old_state = layer_surface->current;
	layer_surface->current = layer_surface->client_pending;
	arrange_layers(output);
	layer_surface->current = old_state;
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. The NULL argument here optionally allows you
	 * to pass in a custom renderer if wlr_renderer doesn't meet your needs. The
	 * backend uses the renderer, for example, to fall back to software cursors
	 * if the backend does not support hardware cursors (some older GPUs
	 * don't). */
	server.backend = wlr_backend_autocreate(server.wl_display, NULL);

	/* If we don't provide a renderer, autocreate makes a GLES2 renderer for us.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_backend_get_renderer(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. */
	server.compositor = wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create();

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/**
	 * Create & configure layer shell.
	 */
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display);
	wl_signal_add(&server.layer_shell->events.new_surface,
		      &server.layer_shell_surface);
	server.layer_shell_surface.notify = handle_layer_shell_surface;

	/* Set up our list of views and the xdg-shell. The xdg-shell is a Wayland
	 * protocol which is used for application windows. For more detail on
	 * shells, refer to my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server.views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). We add a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/**
	 * Configure decoration manager.
	 */
	server.xdg_decoration_manager = 
		wlr_xdg_decoration_manager_v1_create(server.wl_display);
	wl_signal_add(
		      &server.xdg_decoration_manager->events.new_toplevel_decoration,
		      &server.xdg_decoration);
	server.xdg_decoration.notify = handle_xdg_decoration;
	wl_list_init(&server.xdg_decorations);
	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);

	/* Once wl_display_run returns, we shut down the server. */
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
