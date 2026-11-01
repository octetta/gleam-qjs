#include "quickjs.h"
#include "civetweb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define WS_EVENT_CAPACITY 512
#define WS_CONNECTION_CAPACITY 256
#define WS_MESSAGE_EVENT_LIMIT 256
#define WS_MAX_MESSAGE_SIZE (64u * 1024u)
#define WS_POLL_BATCH 64

enum ws_event_type { WS_EVENT_OPEN, WS_EVENT_MESSAGE, WS_EVENT_CLOSE };
struct ws_event { enum ws_event_type type; int64_t id; char *data; size_t len; };
struct ws_connection { struct mg_connection *conn; int64_t id; };

static struct mg_context *g_mg_ctx;
static JSValue g_on_open_cb = JS_UNDEFINED;
static JSValue g_on_message_cb = JS_UNDEFINED;
static JSValue g_on_close_cb = JS_UNDEFINED;
static struct ws_event g_events[WS_EVENT_CAPACITY];
static size_t g_event_head, g_event_count;
static uint64_t g_dropped_events;
static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ws_connection g_connections[WS_CONNECTION_CAPACITY];
static int64_t g_next_connection_id = 1;
static pthread_mutex_t g_connection_mutex = PTHREAD_MUTEX_INITIALIZER;

static void record_dropped_event(void) {
    pthread_mutex_lock(&g_event_mutex);
    ++g_dropped_events;
    pthread_mutex_unlock(&g_event_mutex);
}

/* CivetWeb worker threads only copy native event data into this bounded queue.
 * QuickJS is accessed exclusively by js_server_poll on its owner thread. */
static bool queue_event(enum ws_event_type type, int64_t id,
                        const char *data, size_t len) {
    char *copy = NULL;
    if (len) {
        copy = (char *)malloc(len);
        if (!copy) return false;
        memcpy(copy, data, len);
    }
    pthread_mutex_lock(&g_event_mutex);
    /* Reserve half the queue for connection lifecycle events. */
    if (g_event_count == WS_EVENT_CAPACITY ||
        (type == WS_EVENT_MESSAGE && g_event_count >= WS_MESSAGE_EVENT_LIMIT)) {
        ++g_dropped_events;
        pthread_mutex_unlock(&g_event_mutex);
        free(copy);
        return false;
    }
    size_t tail = (g_event_head + g_event_count) % WS_EVENT_CAPACITY;
    g_events[tail] = (struct ws_event){ type, id, copy, len };
    ++g_event_count;
    pthread_mutex_unlock(&g_event_mutex);
    return true;
}

static bool pop_event(struct ws_event *event) {
    pthread_mutex_lock(&g_event_mutex);
    if (!g_event_count) {
        pthread_mutex_unlock(&g_event_mutex);
        return false;
    }
    *event = g_events[g_event_head];
    g_event_head = (g_event_head + 1) % WS_EVENT_CAPACITY;
    --g_event_count;
    pthread_mutex_unlock(&g_event_mutex);
    return true;
}

static int64_t add_connection(struct mg_connection *conn) {
    int64_t id = 0;
    pthread_mutex_lock(&g_connection_mutex);
    for (size_t i = 0; i < WS_CONNECTION_CAPACITY; ++i) {
        if (!g_connections[i].conn) {
            id = g_next_connection_id++;
            if (g_next_connection_id <= 0) g_next_connection_id = 1;
            g_connections[i] = (struct ws_connection){ conn, id };
            break;
        }
    }
    pthread_mutex_unlock(&g_connection_mutex);
    return id;
}

static int64_t find_connection_id(const struct mg_connection *conn) {
    int64_t id = 0;
    pthread_mutex_lock(&g_connection_mutex);
    for (size_t i = 0; i < WS_CONNECTION_CAPACITY; ++i)
        if (g_connections[i].conn == conn) { id = g_connections[i].id; break; }
    pthread_mutex_unlock(&g_connection_mutex);
    return id;
}

static void remove_connection(const struct mg_connection *conn) {
    pthread_mutex_lock(&g_connection_mutex);
    for (size_t i = 0; i < WS_CONNECTION_CAPACITY; ++i) {
        if (g_connections[i].conn == conn) {
            g_connections[i] = (struct ws_connection){ 0 };
            break;
        }
    }
    pthread_mutex_unlock(&g_connection_mutex);
}

static int ws_connect_handler(const struct mg_connection *conn, void *cbdata) {
    (void)conn; (void)cbdata;
    return 0;
}

static void reject_websocket(struct mg_connection *conn) {
    /* RFC 6455 close code 1013: try again later (server is overloaded). */
    const unsigned char payload[] = { 0x03, 0xf5 };
    (void)mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,
                             (const char *)payload, sizeof(payload));
}

static void ws_ready_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    int64_t id = add_connection(conn);
    if (!id || !queue_event(WS_EVENT_OPEN, id, NULL, 0)) {
        if (id) remove_connection(conn);
        else {
            pthread_mutex_lock(&g_event_mutex);
            ++g_dropped_events;
            pthread_mutex_unlock(&g_event_mutex);
        }
        reject_websocket(conn);
    }
}

static int ws_data_handler(struct mg_connection *conn, int bits,
                           char *data, size_t len, void *cbdata) {
    (void)cbdata;
    /* This API accepts complete, unfragmented text frames only. */
    if ((bits & 0x0f) != MG_WEBSOCKET_OPCODE_TEXT || !(bits & 0x80) ||
        len > WS_MAX_MESSAGE_SIZE) {
        record_dropped_event();
        return 0;
    }
    int64_t id = find_connection_id(conn);
    if (!id) {
        record_dropped_event();
        return 0;
    }
    if (!queue_event(WS_EVENT_MESSAGE, id, data, len))
        return 0; /* Close a client whose message cannot be queued. */
    return 1;
}

static void ws_close_handler(const struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    int64_t id = find_connection_id(conn);
    remove_connection(conn);
    if (id) (void)queue_event(WS_EVENT_CLOSE, id, NULL, 0);
}

static void report_js_exception(JSContext *ctx, const char *where) {
    JSValue exception = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exception);
    fprintf(stderr, "QuickJS %s callback failed: %s\n", where,
            message ? message : "(unprintable exception)");
    if (message) JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, exception);
}

static void call_handler(JSContext *ctx, JSValueConst callback,
                         const char *name, const struct ws_event *event) {
    if (!JS_IsFunction(ctx, callback)) return;
    JSValue args[2];
    int argc = 1;
    args[0] = JS_NewInt64(ctx, event->id);
    if (event->type == WS_EVENT_MESSAGE) {
        args[1] = JS_NewStringLen(ctx, event->data, event->len);
        argc = 2;
    }
    JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, argc, args);
    if (JS_IsException(result)) report_js_exception(ctx, name);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, args[0]);
    if (argc == 2) JS_FreeValue(ctx, args[1]);
}

static JSValue js_server_poll(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    struct ws_event event;
    int processed = 0;
    while (processed < WS_POLL_BATCH && pop_event(&event)) {
        switch (event.type) {
        case WS_EVENT_OPEN: call_handler(ctx, g_on_open_cb, "on_open", &event); break;
        case WS_EVENT_MESSAGE: call_handler(ctx, g_on_message_cb, "on_message", &event); break;
        case WS_EVENT_CLOSE: call_handler(ctx, g_on_close_cb, "on_close", &event); break;
        }
        free(event.data);
        ++processed;
    }
    pthread_mutex_lock(&g_event_mutex);
    uint64_t dropped = g_dropped_events;
    g_dropped_events = 0;
    pthread_mutex_unlock(&g_event_mutex);
    if (dropped)
        fprintf(stderr, "WebSocket: dropped/rejected %llu event(s) (queue or frame limit)\n",
                (unsigned long long)dropped);
    return JS_NewInt32(ctx, processed);
}

static JSValue js_server_start(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 4 || g_mg_ctx) return JS_FALSE;
    int port = 8080;
    if (JS_ToInt32(ctx, &port, argv[0]) < 0 || port < 1 || port > 65535) return JS_FALSE;
    const char *doc_root = JS_ToCString(ctx, argv[1]);
    const char *ws_path = JS_ToCString(ctx, argv[2]);
    if (!doc_root || !ws_path || !JS_IsObject(argv[3])) {
        if (doc_root) JS_FreeCString(ctx, doc_root);
        if (ws_path) JS_FreeCString(ctx, ws_path);
        return JS_FALSE;
    }

    /* Gleam record fields are snake_case. GetPropertyStr returns owned values. */
    JSValue on_open = JS_GetPropertyStr(ctx, argv[3], "on_open");
    JSValue on_message = JS_GetPropertyStr(ctx, argv[3], "on_message");
    JSValue on_close = JS_GetPropertyStr(ctx, argv[3], "on_close");
    JS_FreeValue(ctx, g_on_open_cb);
    JS_FreeValue(ctx, g_on_message_cb);
    JS_FreeValue(ctx, g_on_close_cb);
    g_on_open_cb = on_open;
    g_on_message_cb = on_message;
    g_on_close_cb = on_close;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    const char *options[] = { "listening_ports", port_str,
                              "document_root", doc_root,
                              "num_threads", "2", NULL };
    mg_init_library(0);
    g_mg_ctx = mg_start(NULL, 0, options);
    if (g_mg_ctx) {
        mg_set_websocket_handler(g_mg_ctx, ws_path, ws_connect_handler,
                                 ws_ready_handler, ws_data_handler,
                                 ws_close_handler, NULL);
    } else {
        mg_exit_library();
    }
    JS_FreeCString(ctx, doc_root);
    JS_FreeCString(ctx, ws_path);
    return g_mg_ctx ? JS_TRUE : JS_FALSE;
}

static JSValue js_server_send(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0 || id <= 0) return JS_FALSE;
    size_t len = 0;
    const char *message = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!message) return JS_FALSE;
    int written = -1;
    /* Hold the registry lock through the write so close cannot invalidate conn. */
    pthread_mutex_lock(&g_connection_mutex);
    for (size_t i = 0; i < WS_CONNECTION_CAPACITY; ++i) {
        if (g_connections[i].id == id && g_connections[i].conn) {
            written = mg_websocket_write(g_connections[i].conn, 1, message, len);
            break;
        }
    }
    pthread_mutex_unlock(&g_connection_mutex);
    JS_FreeCString(ctx, message);
    return written > 0 ? JS_TRUE : JS_FALSE;
}

void js_init_server_global(JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue server_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, server_obj, "start", JS_NewCFunction(ctx, js_server_start, "start", 4));
    JS_SetPropertyStr(ctx, server_obj, "send", JS_NewCFunction(ctx, js_server_send, "send", 2));
    JS_SetPropertyStr(ctx, server_obj, "poll", JS_NewCFunction(ctx, js_server_poll, "poll", 0));
    JS_SetPropertyStr(ctx, global_obj, "__native_server", server_obj);
    JS_FreeValue(ctx, global_obj);
}
