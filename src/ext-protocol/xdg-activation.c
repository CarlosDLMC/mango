#include "mango/ext-protocol/xdg-activation.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/ipc/ipc.h"
#include "mango/manage/client.h"
#include "xdg-activation-v1-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>

static struct wlr_xdg_activation_v1 *activation;
static struct wl_listener activation_request_activate_listener;
static struct wl_listener activation_new_token_listener;
static struct wl_listener activation_destroy_listener;

/*
 * activation_unfocused_tokens: mango answers the xdg-activation protocol
 * itself instead of wlroots, which keeps the tokens, their timeout and the
 * request_activate event. wlroots refuses a token whose source surface has
 * neither keyboard nor pointer focus when the token names an input serial,
 * and hands back a token it does not know - so an app raising itself from a
 * clicked notification (Telegram, Slack: their window is on another
 * workspace, or a hidden tab) was dropped before mango ever saw it. Hyprland
 * does not ask for focus. With the option on, the serial still has to be one
 * the client really received; only the focus check goes. Off, this is
 * wlroots' behaviour exactly.
 */
struct pending_token {
	struct wlr_surface *surface;
	struct wlr_seat *seat;
	uint32_t serial;
	char *app_id;
	struct wl_listener surface_destroy;
	struct wl_listener seat_destroy;
};

static void pending_token_free(struct pending_token *p) {
	wl_list_remove(&p->surface_destroy.link);
	wl_list_remove(&p->seat_destroy.link);
	free(p->app_id);
	free(p);
}

static void pending_token_surface_destroy(struct wl_listener *listener,
										  void *data) {
	struct pending_token *p = wl_container_of(listener, p, surface_destroy);
	wl_list_remove(&p->surface_destroy.link);
	wl_list_init(&p->surface_destroy.link);
	p->surface = NULL;
}

static void pending_token_seat_destroy(struct wl_listener *listener,
									   void *data) {
	struct pending_token *p = wl_container_of(listener, p, seat_destroy);
	wl_list_remove(&p->seat_destroy.link);
	wl_list_init(&p->seat_destroy.link);
	p->seat = NULL;
	p->serial = 0;
}

static struct pending_token *pending_token_get(struct wl_resource *resource) {
	struct pending_token *p = wl_resource_get_user_data(resource);
	if (!p)
		wl_resource_post_error(resource,
							   XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
							   "The activation token has already been used");
	return p;
}

static void pending_token_set_serial(struct wl_client *client,
									 struct wl_resource *resource,
									 uint32_t serial,
									 struct wl_resource *seat_resource) {
	struct pending_token *p = pending_token_get(resource);
	if (!p)
		return;
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_resource(seat_resource);
	if (!seat_client)
		return;
	p->seat = seat_client->seat;
	p->serial = serial;
	wl_list_remove(&p->seat_destroy.link);
	wl_signal_add(&p->seat->events.destroy, &p->seat_destroy);
}

static void pending_token_set_app_id(struct wl_client *client,
									 struct wl_resource *resource,
									 const char *app_id) {
	struct pending_token *p = pending_token_get(resource);
	if (!p)
		return;
	free(p->app_id);
	p->app_id = strdup(app_id);
}

static void pending_token_set_surface(struct wl_client *client,
									  struct wl_resource *resource,
									  struct wl_resource *surface_resource) {
	struct pending_token *p = pending_token_get(resource);
	if (!p)
		return;
	p->surface = wlr_surface_from_resource(surface_resource);
	wl_list_remove(&p->surface_destroy.link);
	wl_signal_add(&p->surface->events.destroy, &p->surface_destroy);
}

static void pending_token_commit(struct wl_client *client,
								 struct wl_resource *resource) {
	struct pending_token *p = pending_token_get(resource);
	if (!p)
		return;
	wl_resource_set_user_data(resource, NULL);

	bool valid = true;
	if (p->seat) {
		struct wlr_seat_client *seat_client =
			wlr_seat_client_for_wl_client(p->seat, client);
		if (!seat_client ||
			!wlr_seat_client_validate_event_serial(seat_client, p->serial))
			valid = false;
		else if (!config.activation_unfocused_tokens && p->surface &&
				 p->surface != p->seat->keyboard_state.focused_surface &&
				 p->surface != p->seat->pointer_state.focused_surface)
			valid = false;
	}

	struct wlr_xdg_activation_token_v1 *token =
		valid ? wlr_xdg_activation_token_v1_create(activation) : NULL;
	if (token) {
		token->seat = p->seat;
		token->serial = p->serial;
		token->app_id = p->app_id ? strdup(p->app_id) : NULL;
		/* Only for the new_token listener; wlroots does not watch the
		 * surface of a token made this way. */
		token->surface = p->surface;
		wl_signal_emit_mutable(&activation->events.new_token, token);
		token->surface = NULL;
		xdg_activation_token_v1_send_done(
			resource, wlr_xdg_activation_token_v1_get_name(token));
	} else {
		/* As wlroots: a token nobody knows, so activate does nothing. */
		unsigned char bytes[16] = {0};
		char name[33];
		if (getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes))
			bytes[0] = 1;
		for (size_t i = 0; i < sizeof(bytes); i++)
			snprintf(name + i * 2, 3, "%02x", bytes[i]);
		xdg_activation_token_v1_send_done(resource, name);
	}
	pending_token_free(p);
}

static void pending_token_destroy(struct wl_client *client,
								  struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void pending_token_resource_destroy(struct wl_resource *resource) {
	struct pending_token *p = wl_resource_get_user_data(resource);
	if (p)
		pending_token_free(p);
}

static const struct xdg_activation_token_v1_interface pending_token_impl = {
	.set_serial = pending_token_set_serial,
	.set_app_id = pending_token_set_app_id,
	.set_surface = pending_token_set_surface,
	.commit = pending_token_commit,
	.destroy = pending_token_destroy,
};

static void activation_get_token(struct wl_client *client,
								 struct wl_resource *resource, uint32_t id) {
	struct pending_token *p = calloc(1, sizeof(*p));
	if (!p) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&p->surface_destroy.link);
	wl_list_init(&p->seat_destroy.link);
	p->surface_destroy.notify = pending_token_surface_destroy;
	p->seat_destroy.notify = pending_token_seat_destroy;

	struct wl_resource *token_resource =
		wl_resource_create(client, &xdg_activation_token_v1_interface,
						   wl_resource_get_version(resource), id);
	if (!token_resource) {
		free(p);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(token_resource, &pending_token_impl, p,
								   pending_token_resource_destroy);
}

static void activation_activate(struct wl_client *client,
								struct wl_resource *resource,
								const char *token_str,
								struct wl_resource *surface_resource) {
	struct wlr_xdg_activation_token_v1 *token =
		wlr_xdg_activation_v1_find_token(activation, token_str);
	if (!token)
		return;
	struct wlr_xdg_activation_v1_request_activate_event event = {
		.activation = activation,
		.token = token,
		.surface = wlr_surface_from_resource(surface_resource),
	};
	wl_signal_emit_mutable(&activation->events.request_activate, &event);
	wlr_xdg_activation_token_v1_destroy(token);
}

static void activation_destroy_resource(struct wl_client *client,
										struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct xdg_activation_v1_interface activation_impl = {
	.destroy = activation_destroy_resource,
	.get_activation_token = activation_get_token,
	.activate = activation_activate,
};

static void activation_bind(struct wl_client *client, void *data,
							uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_activation_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &activation_impl, NULL, NULL);
}

void handle_xdg_activation_token_destroy(struct wl_listener *listener,
										 void *data) {
	struct mango_xdg_activation_token *token =
		wl_container_of(listener, token, destroy);
	wl_list_remove(&token->destroy.link);
	free(token);
}

void handle_xdg_activation_new_token(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_token_v1 *wlr_token = data;

	struct mango_xdg_activation_token *token = ecalloc(1, sizeof(*token));
	if (!token)
		return;

	token->wlr_token = wlr_token;
	token->had_focused_surface = wlr_token->surface != NULL;
	wlr_token->data = token;

	token->destroy.notify = handle_xdg_activation_token_destroy;
	wl_signal_add(&wlr_token->events.destroy, &token->destroy);
}

/* Tokens from spawn are trusted; client tokens need a focused surface. */
bool xdg_activation_token_can_activate(
	struct wlr_xdg_activation_token_v1 *wlr_token) {
	if (!wlr_token)
		return false;
	struct mango_xdg_activation_token *token = wlr_token->data;
	if (token && token->internal)
		return true;
	if (!wlr_token->seat)
		return false;
	return token && token->had_focused_surface;
}

void handle_xdg_activation_request_activate(struct wl_listener *listener,
											void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);

	/* A hidden tab of a group has no foreign toplevel (the group gives it
	 * up when another tab is shown), but it can still be activated. */
	if (!c || (!c->foreign_toplevel && !client_is_group_member(c)))
		return;

	/* activation_bypass skips auth and goes straight to the normal path */
	if (c->activation_bypass) {
		handle_client_activation_request(listener, data);
		return;
	}

	if (xdg_activation_token_can_activate(event->token)) {
		handle_client_activation_request(listener, data);
	} else {
		/* valid token, just not usable for activation; flag urgent,
		 * but leave the focused window alone */
		if (c == client_focus_top(server.selected_monitor))
			return;
		c->isurgent = 1;
		if (client_surface(c)->mapped)
			client_update_border_color(c);
		printstatus(IPC_WATCH_ARRANGGE);
	}
}
void handle_xdg_activation_destroy(struct wl_listener *listener, void *data) {
	wl_list_remove(&activation_request_activate_listener.link);
	wl_list_remove(&activation_new_token_listener.link);
	wl_list_remove(&activation_destroy_listener.link);
	activation = NULL;
}

void xdg_activation_init() {
	activation = wlr_xdg_activation_v1_create(server.display);
	if (!activation)
		return;

	/* mango's own protocol front (see pending_token above) in place of
	 * wlroots' global; wlroots destroys whichever global is stored here when
	 * the display goes. */
	struct wl_global *global =
		wl_global_create(server.display, &xdg_activation_v1_interface, 1,
						 NULL, activation_bind);
	if (global) {
		wl_global_destroy(activation->global);
		activation->global = global;
	}

	activation_request_activate_listener.notify =
		handle_xdg_activation_request_activate;
	wl_signal_add(&activation->events.request_activate,
				  &activation_request_activate_listener);

	activation_new_token_listener.notify = handle_xdg_activation_new_token;
	wl_signal_add(&activation->events.new_token,
				  &activation_new_token_listener);

	activation_destroy_listener.notify = handle_xdg_activation_destroy;
	wl_signal_add(&activation->events.destroy, &activation_destroy_listener);
}

/* Make a token for spawn to export as XDG_ACTIVATION_TOKEN. */
const char *xdg_activation_v1_export_token(void) {
	if (!activation)
		return NULL;
	struct wlr_xdg_activation_token_v1 *wlr_token =
		wlr_xdg_activation_token_v1_create(activation);
	if (!wlr_token)
		return NULL;

	struct mango_xdg_activation_token *token = ecalloc(1, sizeof(*token));
	if (!token) {
		wlr_xdg_activation_token_v1_destroy(wlr_token);
		return NULL;
	}
	token->wlr_token = wlr_token;
	token->had_focused_surface = false;
	token->internal = true;
	wlr_token->data = token;
	token->destroy.notify = handle_xdg_activation_token_destroy;
	wl_signal_add(&wlr_token->events.destroy, &token->destroy);

	return wlr_xdg_activation_token_v1_get_name(wlr_token);
}

static struct wlr_xdg_activation_v1 *activation;
static struct wl_listener activation_request_activate_listener;
static struct wl_listener activation_new_token_listener;
static struct wl_listener activation_destroy_listener;
