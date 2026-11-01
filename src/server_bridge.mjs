// Bridge between Gleam JS and C-injected native server functions
export function start(port, doc_root, ws_path, handlers) {
  if (globalThis.__native_server && globalThis.__native_server.start) {
    return globalThis.__native_server.start(port, doc_root, ws_path, handlers);
  }
  return false;
}

export function send(conn, message) {
  if (globalThis.__native_server && globalThis.__native_server.send) {
    return globalThis.__native_server.send(conn, message);
  }
  return false;
}
