#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>

// #define MEMORY_DEBUG
#ifdef MEMORY_DEBUG

void *
debug_mem_malloc(size_t size, const char *func, uint line) {
  void *memory = malloc(size);
  printf("%15s memory %p of size %6zu at line:%5u in func:%s\n", "allocating", memory, size, line, func);
  return memory;
}

void *
debug_mem_realloc(void *memory, size_t size, const char *func, uint line) {
  void *temp = realloc(memory, size);
  printf("%15s memory %p of size %6zu at line:%5u in func:%s\n", "re-allocating", temp, size, line, func);
  return temp;
}
void
debug_mem_free(void *memory, size_t size, const char *func, uint line) {
  printf("%15s memory %p of size %6zu at line:%5u in func:%s\n", "freeing", memory, size, line, func);
  return free(memory);
}

#define malloc(n) \
  debug_mem_malloc(n, __func__, __LINE__) /* Replaces malloc. \
                                           */
#define realloc(n, m) \
  debug_mem_realloc(n, m, __func__, __LINE__)               /* Replaces realloc. */
#define free(n, m) debug_mem_free(n, m, __func__, __LINE__) /* Replaces free. */

#else

#define free(n, m) free(n)

#endif

typedef struct {
  js_env_t *env;
  js_deferred_t *deferred;
  uv_buf_t buf;
  uv_connect_t *req;
} req_state;

void
reject_promise(req_state *state, utf8_t *str) {
  js_value_t *resolution;
  js_value_t *message;
  js_handle_scope_t *handle_scope;

  js_open_handle_scope(state->env, &handle_scope);
  js_create_string_utf8(state->env, str, -1, &message);
  js_create_error(state->env, NULL, message, &resolution);
  js_reject_deferred(state->env, state->deferred, resolution);
  js_close_handle_scope(state->env, handle_scope);
}

void
resolve_promise(req_state *state) {
  js_value_t *resolution;
  js_handle_scope_t *handle_scope;

  js_open_handle_scope(state->env, &handle_scope);
  js_create_string_utf8(state->env, (void *) state->buf.base, state->buf.len, &resolution);
  js_resolve_deferred(state->env, state->deferred, resolution);
  js_close_handle_scope(state->env, handle_scope);
}

void
close_cb(uv_handle_t *handle) {
  free(handle, sizeof(*(uv_tcp_t *) handle));
}

void
alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}
void
read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  req_state *state = stream->data;
  if (nread < 0) {
    if (nread == UV_EOF) {
      resolve_promise(state);
      uv_close((uv_handle_t *) stream, close_cb);
    } else {
      reject_promise(state, (utf8_t *) uv_strerror(nread));
      uv_close((uv_handle_t *) stream, close_cb);
    }
    free(state->buf.base, state->buf.len);
    free(state->req, sizeof(*state->req));
    free(state, sizeof(*state));
  } else if (nread > 0) {
    state->buf.base = realloc(state->buf.base, nread + state->buf.len);
    memcpy(state->buf.base + state->buf.len, buf->base, nread);
    state->buf.len += nread;
  }

  if (buf != NULL) {
    free(buf->base, buf->len);
  }
}

void
write_cb(uv_write_t *req, int status) {
  if (status < 0) {
    req_state *state = req->handle->data;
    reject_promise(state, (void *) uv_strerror(status));
    free(state->req, sizeof(*(state->req)));
    free(state, sizeof(*state));
    uv_close((void *) req->handle, close_cb);
  }
  uv_buf_t *buf = req->data;
  free(buf->base, buf->len);
  free(buf, sizeof(*buf));
  free(req, sizeof(*req));
}

void
connect_cb(uv_connect_t *req, int status) {
  uv_handle_t *handle = (uv_handle_t *) req->handle;
  req_state *state = handle->data;
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
    free(buf, sizeof(*buf));
    free(w_req, sizeof(*w_req));
    goto cleanup;
  }
  return;

cleanup:
  uv_close(handle, close_cb);
  reject_promise(state, (utf8_t *) uv_strerror(status));
  free(req->data, strlen(req->data) + 1);
  free(req, sizeof(*req));
  free(state, sizeof(*state));
  return;
}

js_value_t *
bare_addon_tcp_connect(js_env_t *env, js_callback_info_t *info) {
  js_value_t *promise = NULL;
  uv_connect_t *req = NULL;
  uv_tcp_t *handle = NULL;
  req_state *state = NULL;
  char *msg = NULL;
  char *host = NULL;
  int err;

  size_t argc = 3;
  js_value_t *argv[3];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  if (err != 0) {
    goto cleanup;
  }

  if (argc < 3) {
    js_throw_type_error(env, NULL, "Wrong number of arguments. Expects 3 arguments.");
    goto cleanup;
  }

  js_value_type_t value_type;
  err = js_typeof(env, argv[0], &value_type);
  if (err != 0) {
    goto cleanup;
  }

  if (value_type != js_string) {
    js_throw_type_error(env, NULL, "Wrong type of arguments. The host must be a string.");
    goto cleanup;
  }
  err = js_typeof(env, argv[1], &value_type);
  if (err != 0) {
    goto cleanup;
  }

  if (value_type != js_number) {
    js_throw_type_error(env, NULL, "Wrong type of arguments. The port must be a number.");
    goto cleanup;
  }

  err = js_typeof(env, argv[2], &value_type);
  if (err != 0) {
    goto cleanup;
  }
  if (value_type != js_string) {
    js_throw_type_error(env, NULL, "Wrong type of arguments. The messeage must be a string.");
    goto cleanup;
  }

  size_t host_len = 0;

  err = js_get_value_string_utf8(env, argv[0], NULL, 0, &host_len);
  if (err != 0) {
    goto cleanup;
  }

  host = malloc(host_len + 1);
  if (host == NULL) {
    js_throw_error(env, NULL, "Error allocating memory");
    goto cleanup;
  }
  memset(host, 'G', host_len);
  host[host_len] = '\0';
  err = js_get_value_string_utf8(env, argv[0], (utf8_t *) host, host_len + 1, NULL);
  if (err != 0) {
    goto cleanup;
  }

  uint32_t port = 0;
  err = js_get_value_uint32(env, argv[1], &port);
  if (err != 0) {
    goto cleanup;
  }

  struct sockaddr_in addr;
  err = uv_ip4_addr(host, port, &addr);
  if (err != 0) {
    js_throw_error(env, NULL, uv_strerror(err));
    goto cleanup;
  }

  size_t msg_len;

  err = js_get_value_string_utf8(env, argv[2], NULL, 0, &msg_len);
  if (err != 0) {
    goto cleanup;
  }

  msg = malloc(msg_len + 1);
  if (msg == NULL) {
    js_throw_error(env, NULL, "Error allocating memory");
    goto cleanup;
  }
  memset(msg, 'G', msg_len);
  msg[msg_len] = '\0';
  err = js_get_value_string_utf8(env, argv[2], (utf8_t *) msg, msg_len + 1, NULL);
  if (err != 0) {
    goto cleanup;
  }

  handle = malloc(sizeof(*handle));
  if (handle == NULL) {
    js_throw_error(env, NULL, "Error allocating memory");
    goto cleanup;
  }

  err = uv_tcp_init(uv_default_loop(), handle);
  if (err != 0) {
    js_throw_error(env, NULL, uv_strerror(err));
    goto cleanup;
  }

  req = malloc(sizeof(*req));
  if (req == NULL) {
    js_throw_error(env, NULL, "Error allocating memory");
    goto cleanup;
  }
  req->data = msg;
  err = uv_tcp_connect(req, handle, (struct sockaddr *) &addr, connect_cb);
  if (err != 0) {
    js_throw_error(env, NULL, uv_strerror(err));
    goto cleanup;
  }

  state = malloc(sizeof(*state));
  if (state == NULL) {
    js_throw_error(env, NULL, "Error allocating memory");
    goto cleanup;
  }
  memset(state, 0, sizeof(*state));
  state->env = env;
  state->req = req;
  handle->data = state;

  err = js_create_promise(env, &state->deferred, &promise);
  if (err != 0) {
    goto cleanup;
  } else {
    goto clean_host;
  }

cleanup:
  if (state != NULL) {
    free(state, sizeof(*state));
  }
  if (req != NULL) {
    free(req, sizeof(*req));
  }
  if (handle != NULL) {
    free(handle, sizeof(*handle));
  }
  if (msg != NULL) {
    free(msg, msg_len + 1);
  }
clean_host:
  if (host != NULL) {
    free(host, host_len + 1);
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
