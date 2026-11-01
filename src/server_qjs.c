#include "quickjs.h"
#include "civetweb.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static struct mg_context *g_mg_ctx = NULL;
static JSContext *g_js_ctx = NULL;

static JSValue g_on_open_cb    = JS_UNDEFINED;
static JSValue g_on_message_cb = JS_UNDEFINED;
static JSValue g_on_close_cb   = JS_UNDEFINED;

// --- CivetWeb WebSocket Callbacks ---

static int ws_connect_handler(const struct mg_connection *conn, void *cbdata) {
    (void)conn; (void)cbdata;
    return 0; // Accept connection
}

static void ws_ready_handler(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    if (g_js_ctx && JS_IsFunction(g_js_ctx, g_on_open_cb)) {
        JSValue arg = JS_NewInt64(g_js_ctx, (intptr_t)conn);
        JSValue ret = JS_Call(g_js_ctx, g_on_open_cb, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(g_js_ctx, ret);
        JS_FreeValue(g_js_ctx, arg);
    }
}

static int ws_data_handler(struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata) {
    (void)bits; (void)cbdata;
    if (g_js_ctx && JS_IsFunction(g_js_ctx, g_on_message_cb)) {
        JSValue args[2];
        args[0] = JS_NewInt64(g_js_ctx, (intptr_t)conn);
        args[1] = JS_NewStringLen(g_js_ctx, data, len);
        JSValue ret = JS_Call(g_js_ctx, g_on_message_cb, JS_UNDEFINED, 2, args);
        JS_FreeValue(g_js_ctx, ret);
        JS_FreeValue(g_js_ctx, args[0]);
        JS_FreeValue(g_js_ctx, args[1]);
    }
    return 1; // Keep connection open
}

static void ws_close_handler(const struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    if (g_js_ctx && JS_IsFunction(g_js_ctx, g_on_close_cb)) {
        JSValue arg = JS_NewInt64(g_js_ctx, (intptr_t)conn);
        JSValue ret = JS_Call(g_js_ctx, g_on_close_cb, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(g_js_ctx, ret);
        JS_FreeValue(g_js_ctx, arg);
    }
}

// --- Native C Functions exposed to JS ---

static JSValue js_server_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 4) return JS_FALSE;

    int port = 8080;
    JS_ToInt32(ctx, &port, argv[0]);
    const char *doc_root = JS_ToCString(ctx, argv[1]);
    const char *ws_path  = JS_ToCString(ctx, argv[2]);

    if (JS_IsObject(argv[3])) {
        g_js_ctx = ctx;
        g_on_open_cb    = JS_DupValue(ctx, JS_GetPropertyStr(ctx, argv[3], "onOpen"));
        g_on_message_cb = JS_DupValue(ctx, JS_GetPropertyStr(ctx, argv[3], "onMessage"));
        g_on_close_cb   = JS_DupValue(ctx, JS_GetPropertyStr(ctx, argv[3], "onClose"));
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    const char *options[] = {
        "listening_ports", port_str,
        "document_root",   doc_root ? doc_root : "./public",
        "num_threads",     "2",
        NULL
    };

    mg_init_library(0);
    g_mg_ctx = mg_start(NULL, 0, options);

    if (!g_mg_ctx) {
        if (doc_root) JS_FreeCString(ctx, doc_root);
        if (ws_path) JS_FreeCString(ctx, ws_path);
        return JS_FALSE;
    }

    mg_set_websocket_handler(g_mg_ctx,
                             ws_path ? ws_path : "/ws",
                             ws_connect_handler,
                             ws_ready_handler,
                             ws_data_handler,
                             ws_close_handler,
                             NULL);

    if (doc_root) JS_FreeCString(ctx, doc_root);
    if (ws_path) JS_FreeCString(ctx, ws_path);
    return JS_TRUE;
}

static JSValue js_server_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_FALSE;

    int64_t conn_ptr = 0;
    JS_ToInt64(ctx, &conn_ptr, argv[0]);
    struct mg_connection *conn = (struct mg_connection *)(intptr_t)conn_ptr;
    if (!conn) return JS_FALSE;

    const char *msg = JS_ToCString(ctx, argv[1]);
    if (!msg) return JS_FALSE;

    mg_websocket_write(conn, 1 /* WEBSOCKET_OPCODE_TEXT */, msg, strlen(msg));

    JS_FreeCString(ctx, msg);
    return JS_TRUE;
}

// Attach directly to globalThis.__native_server
void js_init_server_global(JSContext *ctx) {
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue server_obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, server_obj, "start", JS_NewCFunction(ctx, js_server_start, "start", 4));
    JS_SetPropertyStr(ctx, server_obj, "send", JS_NewCFunction(ctx, js_server_send, "send", 2));

    JS_SetPropertyStr(ctx, global_obj, "__native_server", server_obj);
    JS_FreeValue(ctx, global_obj);
}
