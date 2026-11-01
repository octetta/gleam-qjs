import * as os from 'os';

// CivetWeb worker callbacks enqueue events; poll() dispatches them on the
// QuickJS owner/event-loop thread.
let polling = false;

function poll_events() {
  if (!polling) return;
  globalThis.__native_server.poll();
  os.setTimeout(poll_events, 10);
}

export function start(port, doc_root, ws_path, handlers) {
  const native = globalThis.__native_server;
  if (!native || !native.start || !native.poll) return false;
  const ok = native.start(port, doc_root, ws_path, handlers);
  if (ok && !polling) {
    polling = true;
    os.setTimeout(poll_events, 0);
  }
  return ok;
}

export function send(conn, message) {
  const native = globalThis.__native_server;
  return native && native.send ? native.send(conn, message) : false;
}
