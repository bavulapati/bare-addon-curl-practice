#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>

typedef struct {
  js_env_t *env;
  js_deferred_t *deferred;
  uv_buf_t buf;
} state_t;

void
reject_promise(state_t *state, utf8_t *str) {
  js_value_t *resolution;
  js_value_t *message;
  js_handle_scope_t *handle_scope;
  int err = 0;

  err = js_open_handle_scope(state->env, &handle_scope);
  assert(err == 0);
  err = js_create_string_utf8(state->env, str, -1, &message);
  assert(err == 0);
  err = js_create_error(state->env, NULL, message, &resolution);
  assert(err == 0);
  err = js_reject_deferred(state->env, state->deferred, resolution);
  assert(err == 0);
  err = js_close_handle_scope(state->env, handle_scope);
  assert(err == 0);
}

void
resolve_promise(state_t *state) {
  js_value_t *resolution;
  js_handle_scope_t *handle_scope;
  int err = 0;

  err = js_open_handle_scope(state->env, &handle_scope);
  assert(err == 0);
  err = js_create_string_utf8(state->env, (void *) state->buf.base, state->buf.len, &resolution);
  assert(err == 0);
  err = js_resolve_deferred(state->env, state->deferred, resolution);
  assert(err == 0);
  err = js_close_handle_scope(state->env, handle_scope);
  assert(err == 0);
}

void
close_cb(uv_handle_t *handle) {
  free(handle);
}

void
alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}
void
read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  state_t *state = stream->data;
  if (nread < 0) {
    if (nread == UV_EOF) {
      resolve_promise(state);
      uv_close((uv_handle_t *) stream, close_cb);
    } else {
      reject_promise(state, (utf8_t *) uv_strerror(nread));
      uv_close((uv_handle_t *) stream, close_cb);
    }
    free(state->buf.base);
    free(state);
  } else if (nread > 0) {
    state->buf.base = realloc(state->buf.base, nread + state->buf.len);
    memcpy(state->buf.base + state->buf.len, buf->base, nread);
    state->buf.len += nread;
  }

  if (buf != NULL) {
    free(buf->base);
  }
}

void
write_cb(uv_write_t *req, int status) {
  if (status < 0) {
    state_t *state = req->handle->data;
    reject_promise(state, (void *) uv_strerror(status));
    free(state);
    uv_close((void *) req->handle, close_cb);
  }
  uv_buf_t *buf = req->data;
  free(buf->base);
  free(buf);
  free(req);
}

void
connect_cb(uv_connect_t *req, int status) {
  uv_handle_t *handle = (uv_handle_t *) req->handle;
  state_t *state = handle->data;
  if (status < 0) {
    goto cleanup;
  }
  status = uv_read_start((void *) handle, alloc_cb, read_cb);
  if (status != 0) {
    goto cleanup;
  }

  uv_write_t *w_req;
  w_req = malloc(sizeof(*w_req));
  uv_buf_t *buf;
  buf = malloc(sizeof(*buf));
  buf->base = req->data;
  buf->len = strlen(req->data) + 1;
  w_req->data = buf;
  status = uv_write(w_req, req->handle, buf, 1, write_cb);
  if (status != 0) {
    free(buf);
    free(w_req);
    goto cleanup;
  }
  free(req);
  return;

cleanup:
  uv_close(handle, close_cb);
  reject_promise(state, (utf8_t *) uv_strerror(status));
  free(req->data);
  free(req);
  free(state);
  return;
}

js_value_t *
bare_addon_tcp_connect(js_env_t *env, js_callback_info_t *info) {
  js_value_t *promise = NULL;
  uv_connect_t *req = NULL;
  uv_tcp_t *handle = NULL;
  state_t *state = NULL;
  char *msg = NULL;
  char *host = NULL;
  int err;

  size_t argc = 3;
  js_value_t *argv[3];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  if (argc < 3) {
    err = js_throw_type_error(env, NULL, "Wrong number of arguments. Expects 3 arguments.");
    assert(err == 0);
    goto cleanup;
  }

  js_value_type_t value_type;
  err = js_typeof(env, argv[0], &value_type);
  assert(err == 0);

  if (value_type != js_string) {
    err = js_throw_type_error(env, NULL, "Wrong type of arguments. The host must be a string.");
    assert(err == 0);
    goto cleanup;
  }
  err = js_typeof(env, argv[1], &value_type);
  assert(err == 0);

  if (value_type != js_number) {
    err = js_throw_type_error(env, NULL, "Wrong type of arguments. The port must be a number.");
    assert(err == 0);
    goto cleanup;
  }

  err = js_typeof(env, argv[2], &value_type);
  assert(err == 0);
  if (value_type != js_string) {
    err = js_throw_type_error(env, NULL, "Wrong type of arguments. The messeage must be a string.");
    assert(err == 0);
    goto cleanup;
  }

  size_t host_len = 0;

  err = js_get_value_string_utf8(env, argv[0], NULL, 0, &host_len);
  assert(err == 0);

  host = malloc(host_len + 1);
  err = js_get_value_string_utf8(env, argv[0], (utf8_t *) host, host_len + 1, NULL);
  assert(err == 0);

  uint32_t port = 0;
  err = js_get_value_uint32(env, argv[1], &port);
  assert(err == 0);

  struct sockaddr_in addr;
  err = uv_ip4_addr(host, port, &addr);
  if (err != 0) {
    err = js_throw_error(env, NULL, uv_strerror(err));
    assert(err == 0);
    goto cleanup;
  }

  free(host);
  host = NULL;
  size_t msg_len;

  err = js_get_value_string_utf8(env, argv[2], NULL, 0, &msg_len);
  assert(err == 0);

  msg = malloc(msg_len + 1);
  err = js_get_value_string_utf8(env, argv[2], (utf8_t *) msg, msg_len + 1, NULL);
  assert(err == 0);

  handle = malloc(sizeof(*handle));

  err = uv_tcp_init(uv_default_loop(), handle);
  if (err != 0) {
    err = js_throw_error(env, NULL, uv_strerror(err));
    assert(err == 0);
    goto cleanup;
  }

  req = malloc(sizeof(*req));
  req->data = msg;
  err = uv_tcp_connect(req, handle, (struct sockaddr *) &addr, connect_cb);
  if (err != 0) {
    err = js_throw_error(env, NULL, uv_strerror(err));
    assert(err == 0);
    goto cleanup;
  }

  state = malloc(sizeof(*state));
  *state = (state_t) {};
  state->env = env;
  handle->data = state;

  err = js_create_promise(env, &state->deferred, &promise);
  assert(err == 0);

  return promise;

cleanup:
  if (state != NULL) {
    free(state);
  }
  if (req != NULL) {
    free(req);
  }
  if (handle != NULL) {
    free(handle);
  }
  if (msg != NULL) {
    free(msg);
  }
  if (host != NULL) {
    free(host);
  }
  return promise;
}

static js_value_t *
bare_addon_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("tcpConnect", bare_addon_tcp_connect)
#undef V

  return exports;
}

BARE_MODULE(bare_addon, bare_addon_exports)
