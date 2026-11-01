#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${1:-"$ROOT/dist/app"}
LOG=$(mktemp)
PID=

cleanup() {
    if [ -n "$PID" ]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -f "$LOG"
}
trap cleanup EXIT INT TERM

if [ ! -x "$BIN" ]; then
    echo "smoke: executable not found or not executable: $BIN" >&2
    exit 1
fi
if ! command -v node >/dev/null 2>&1; then
    echo "smoke: Node.js 22+ is required (global WebSocket API)" >&2
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "smoke: curl is required" >&2
    exit 1
fi

"$BIN" >"$LOG" 2>&1 &
PID=$!

ready=0
i=0
while [ "$i" -lt 50 ]; do
    if curl -fsS http://127.0.0.1:8080/index.html >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        cat "$LOG" >&2
        echo "smoke: server exited before becoming ready" >&2
        exit 1
    fi
    sleep 0.1
    i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
    cat "$LOG" >&2
    echo "smoke: server did not become ready on port 8080" >&2
    exit 1
fi

node --input-type=module - <<'JS'
const url = "ws://127.0.0.1:8080/ws";

function checkConnection(message) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    let finished = false;
    let gotEcho = false;
    const timer = setTimeout(() => {
      if (!finished) {
        finished = true;
        ws.close();
        reject(new Error(`timeout waiting for echo/close: ${message}`));
      }
    }, 4000);

    ws.addEventListener("open", () => ws.send(message));
    ws.addEventListener("message", event => {
      if (event.data !== `echo: ${message}` || gotEcho) return; // Ignore welcome.
      gotEcho = true;
      ws.close();
    });
    ws.addEventListener("close", () => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      if (gotEcho) resolve();
      else reject(new Error(`closed before echo: ${message}`));
    });
    ws.addEventListener("error", () => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      reject(new Error(`WebSocket error for ${message}`));
    });
  });
}

for (let i = 1; i <= 3; i++) {
  const message = `gleam-qjs-smoke-${i}`;
  await checkConnection(message);
  console.log(`ok: connect, send, echo, close (${i}/3)`);
}
JS

echo "smoke: HTTP and WebSocket checks passed"
