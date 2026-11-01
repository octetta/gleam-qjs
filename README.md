# gleam-qjs

A small pipeline for packaging **Gleam** applications as native executables that embed **Fabrice Bellard's QuickJS** and **CivetWeb**. It avoids shipping the Erlang/BEAM runtime or Node.js/V8, but the Gleam application still runs as JavaScript inside QuickJS.

---

## 1. Philosophy & "The Why"

Gleam is an expressive, type-safe language offering algebraic data types, immutable data, and pattern matching. However, deploying a Gleam application traditionally demands choosing between two heavy runtimes:

1. **The Erlang BEAM Runtime:** Outstanding for distributed fault tolerance and massive concurrency, but packing ERTS into a standalone release yields a 35 MB to 60 MB bundle with noticeable startup latency—ill-suited for small CLI tools, local daemons, low-power devices, or embedded desktop audio tools.
2. **The Node.js / V8 Ecosystem:** Packaging via Node/Electron/Bun creates 50 MB to 100 MB+ binaries with significant memory footprints and complex runtime baggage.

### The QuickJS Alternative

QuickJS is a small embeddable JavaScript engine. The Gleam JavaScript backend generates JavaScript modules; esbuild bundles those modules, and `qjsc` converts the bundle into bytecode embedded in a C executable. The resulting program runs the Gleam application inside QuickJS.

This architecture can offer:

* **A compact runtime:** QuickJS is substantially smaller than a Node.js/V8 deployment, although executable size and memory use depend on the target, compiler, linker, and enabled features.
* **No BEAM or Node.js dependency:** The executable embeds QuickJS and links the native C components directly.
* **A direct C integration point:** Native functionality can be exposed to the embedded JavaScript runtime through QuickJS's C API.

Binary size, startup time, and RSS are build- and platform-dependent. Treat numbers reported for a particular build as measurements, not guarantees; include the target platform, compiler flags, and measurement method when publishing them.

Licensing is component-specific. Check the licenses of Gleam and its dependencies, QuickJS, CivetWeb, and any other bundled components before redistributing a build.

---

## 2. Architecture & Execution Model

The system compiles Gleam to JavaScript, bundles the generated modules, converts the bundle to QuickJS bytecode, and embeds that bytecode in a C executable. The C server handles network I/O while QuickJS runs the Gleam application.

### Compilation Pipeline

```text
 ┌─────────────────┐
 │  src/*.gleam    │ ───► gleam build --target javascript
 └─────────────────┘              │
                                  ▼
 ┌─────────────────┐      build/dev/javascript/**/*.mjs
 │  src/*.mjs      │ ───► (ES module definitions)
 └─────────────────┘              │
                                  ▼
                       esbuild (bundle, dead-code drop, footer: "main();")
                                  │
                                  ▼
 ┌─────────────────┐      dist/bundle.mjs
 │  dist/bundle.mjs│ ───► qjsc -m -e (compile ES module to C bytecode arrays)
 └─────────────────┘              │
                                  ▼
 ┌─────────────────┐      dist/wrapper.c (generated C bytecode table)
 │ src/server_qjs.c│              │
 │ civetweb.c      │ ───► gcc -Os -s (strip debug symbols, link static QuickJS)
 │ libquickjs.a    │              │
 └─────────────────┘              ▼
                         dist/app (~850 KB ELF/Mach-O executable)
```

### Event Loop & Thread Separation

QuickJS contexts must only be accessed from their owning thread. In particular,
CivetWeb worker callbacks must not call `JS_Call`, create or free `JSValue`s, or
otherwise touch a `JSContext`. WebSocket events are transferred from CivetWeb
workers to the QuickJS thread through a bounded native event queue:

```text
  CivetWeb worker threads                 QuickJS owner thread
  -----------------------                 -------------------
  WebSocket open/message/close             timer-driven poll
           |                                       |
           v                                       v
  copy event and payload                   dequeue events
           |                                       |
           v                                       v
  bounded, mutex-protected queue  ------> invoke Gleam handlers
                                                   |
                                                   v
                                            QuickJS JSContext
```

1. **QuickJS ownership:** JavaScript callbacks and `JSValue` operations run on the
   QuickJS thread, not in CivetWeb worker callbacks.
2. **WebSocket event delivery:** The current queue holds up to 256 events. Each
   poll processes at most 64 events before returning to the QuickJS timer loop.
   If the queue is full, an event is dropped rather than blocking a CivetWeb
   worker. The current API does not report queue overflow to Gleam.
3. **Polling:** `server_bridge.mjs` schedules queue polling with
   `os.setTimeout` (10 ms between polls). Promise continuations are handled by
   QuickJS's job queue.
4. **CivetWeb:** CivetWeb performs HTTP and WebSocket network I/O on its worker
   threads. WebSocket events reach Gleam through the native queue.

This is a small event-queue bridge, not a general-purpose cross-thread QuickJS
runtime. Keep all QuickJS API use on its owning thread.
---

## 3. API Reference & Interface Contracts

The system isolates platform dependencies behind type-safe Gleam facades and lightweight JS bridges.

### 3.1. Async Timers & Continuations (`ffi.mjs`)

Gleam models asynchronous control flow natively using higher-order functions and the `use` syntactic sugar:

```gleam
pub type Promise(a)

@external(javascript, "./ffi.mjs", "sleep")
pub fn sleep(ms: Int) -> Promise(Nil)

@external(javascript, "./ffi.mjs", "await_promise")
pub fn await(promise: Promise(a), next: fn(a) -> Promise(b)) -> Promise(b)

@external(javascript, "./ffi.mjs", "resolve_promise")
pub fn resolve(val: a) -> Promise(a)
```

**Usage:**
```gleam
pub fn main() -> Promise(Nil) {
  use Nil <- await(sleep(300))
  io.println("Awake after 300ms!")
  resolve(Nil)
}
```

### 3.2. Synchronous File I/O (`ffi.mjs`)

Directly bound to QuickJS's built-in `std` module (`std.open`, `std.loadFile`):

```gleam
@external(javascript, "./ffi.mjs", "write_file")
pub fn write_file(path: String, content: String) -> Bool

@external(javascript, "./ffi.mjs", "read_file")
pub fn read_file(path: String) -> String
```

### 3.3. HTTP & WebSocket Server (`server.gleam`)

The server exposes an event-driven API wrapping CivetWeb:

```gleam
pub type ConnectionId = Int

pub type ServerHandlers {
  ServerHandlers(
    on_open: fn(ConnectionId) -> Nil,
    on_message: fn(ConnectionId, String) -> Nil,
    on_close: fn(ConnectionId) -> Nil,
  )
}

@external(javascript, "./server_bridge.mjs", "start")
pub fn start(
  port: Int,
  document_root: String,
  ws_path: String,
  handlers: ServerHandlers,
) -> Bool

@external(javascript, "./server_bridge.mjs", "send")
pub fn send(conn: ConnectionId, message: String) -> Bool
```

#### Lifecycle Hooks

| Handler | Parameter | Behavior |
| :--- | :--- | :--- |
| `on_open` | `ConnectionId` | Triggered when the WebSocket connection is ready. |
| `on_message` | `ConnectionId`, `String` | Receives incoming WebSocket text messages. |
| `on_close` | `ConnectionId` | Triggered when the client disconnects. |

`ConnectionId` is an integer handle allocated by the native server; it is not a
pointer. The native connection registry removes the handle when the connection
closes. `send` returns `False` if the handle is no longer active or the write
fails. The current interface supports text messages; it does not expose binary
WebSocket frames or a server stop/restart API.

The event queue and connection registry each have a fixed capacity of 256
entries. The current implementation does not provide application-visible
backpressure, queue-overflow reporting, or graceful server shutdown.

---

## 4. Project Structure

```text
.
├── Makefile                # Self-bootstrapping build pipeline
├── mise.toml               # Toolchain pinning (Gleam, Node)
├── gleam.toml              # Gleam package definition
├── public/
│   └── index.html          # Interactive in-browser WS test harness
├── src/
│   ├── app.gleam           # Application entry point & showcase flow
│   ├── ffi.mjs             # QuickJS std/os bridge (timers, file I/O)
│   ├── server.gleam        # Type-safe Gleam server interface
│   ├── server_bridge.mjs   # Indirection layer to C-injected global
│   └── server_qjs.c        # CivetWeb HTTP/WebSocket C bindings
└── .vendor/                # Automated dependencies (git-ignored)
    ├── quickjs/            # Fabrice Bellard's QuickJS engine
    └── civetweb/           # MIT embedded HTTP/WS server (pinned v1.16)
```

---

## 5. Prerequisites & Tooling

Manage the Gleam and Node.js toolchains using [mise](https://mise.jdx.dev/):

```toml
# mise.toml
[tools]
gleam = "1.8.1"
node = "22.13.0"
```

System requirements:
* `make`
* `gcc` or `clang`
* `git` (to fetch vendored dependencies on first build)
* `npm`/`npx` for the default esbuild invocation, unless `ESBUILD` is overridden

---

## 6. Build & Run

### 1. Bootstrap Toolchain
```bash
mise install
```

### 2. Build and Launch
```bash
make run
```

On first invocation, the build harness automatically:
1. Clones and compiles QuickJS into `qjsc` and `libquickjs.a`.
2. Clones CivetWeb pinned to stable release `v1.16`.
3. Compiles Gleam source to modern ES modules.
4. Bundles JS dependencies using `esbuild`.
5. Emits C bytecode tables via `qjsc -m -e`.
6. Links everything into `dist/app` using `gcc -Os -s`.

The Makefile uses Bellard's QuickJS repository for `qjsc` and `libquickjs.a`.
The separate QuickJS-NG entry in `mise.toml` is not the compiler/library used by
that Makefile build. The Makefile's vendored QuickJS path is `.vendor/quickjs`;
`mise.toml` currently adds `./vendor/quickjs` to `PATH`, which is a different
directory.

The default build defines `NO_SSL`, so the CivetWeb build does not provide
HTTPS or WSS. Use a suitable TLS-enabled build and configure certificates
before exposing the server beyond a trusted local network.

### 3. Verify Binary
```bash
$ ls -lh dist/app
-rwxr-xr-x 1 user user 856K dist/app

$ file dist/app
dist/app: ELF 64-bit LSB pie executable, x86-64, dynamically linked ...
```

---

## 7. Interactive Verification

When running `dist/app`, the server starts an HTTP server on port `8080` serving `./public` alongside a WebSocket endpoint on `/ws`:

1. Navigate to `http://localhost:8080/index.html`.
2. The browser automatically connects to `ws://localhost:8080/ws`.
3. View the live connection confirmation and exchange echo messages directly from the in-browser console.

For a quick local test, run `make run` and open the page in a browser on the
same machine. Do not assume the server is restricted to loopback: check the
actual listening address and firewall rules before running it on an untrusted
network.

The current example is a prototype rather than a hardened internet-facing
server. Add an explicit shutdown path, queue-overflow reporting, TLS, and
connection/message limits before using it as a public service.

---

## 8. License

The project license applies to this repository's own code. Bundled and fetched dependencies retain their respective licenses; review those licenses before redistributing a combined executable.