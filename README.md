# gleam-qjs

A minimal, zero-bloat pipeline for compiling **Gleam** applications into standalone native binaries (~850 KB) using **Fabrice Bellard's QuickJS** and **CivetWeb**, bypassing both the Erlang/BEAM runtime and Node.js/V8.

---

## 1. Philosophy & "The Why"

Gleam is an expressive, type-safe language offering algebraic data types, immutable data, and pattern matching. However, deploying a Gleam application traditionally demands choosing between two heavy runtimes:

1. **The Erlang BEAM Runtime:** Outstanding for distributed fault tolerance and massive concurrency, but packing ERTS into a standalone release yields a 35 MB to 60 MB bundle with noticeable startup latency—ill-suited for small CLI tools, local daemons, low-power devices, or embedded desktop audio tools.
2. **The Node.js / V8 Ecosystem:** Packaging via Node/Electron/Bun creates 50 MB to 100 MB+ binaries with significant memory footprints and complex runtime baggage.

### The QuickJS Alternative

QuickJS compiles modern JavaScript (ES2020) down to bytecode and links directly into a standard C application via a minimal, embeddable interpreter. Because Gleam's JavaScript compiler outputs clean, idiomatic ES modules without proprietary runtime shims, Gleam compiles onto QuickJS seamlessly.

This architecture offers distinct advantages:

* **Instantaneous Startup (< 5 ms):** Zero JIT warm-up penalty; bytecode executes immediately via direct threaded interpretation.
* **Deterministic, Sub-Megabyte Footprint:** A fully functional HTTP file server, WebSocket router, timer engine, and Gleam application compiles into an ~850 KB ELF/Mach-O binary with an idle RSS under 4 MB.
* **Pure MIT Licensing:** Avoids copyleft infections (such as GPLv3 or AGPL), allowing clean commercial and open-source distribution.
* **Direct C Boundary:** Native modules link directly at compile time into C bytecode tables. No Node-API/FFI glue layers or dynamic library discovery overhead.

---

## 2. Architecture & Execution Model

The system translates functional, typed Gleam code into static C structures, combining a cooperative JS microtask queue with a multi-threaded C I/O substrate.

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

QuickJS uses non-atomic reference counting on `JSValue` structures and is strictly **single-threaded per context**. To maintain rock-solid stability while serving HTTP assets and concurrent WebSockets:

```text
 ┌────────────────────────────────────────────────────────────────────────┐
 │ QuickJS Main Thread (Deterministic Event Loop)                         │
 │                                                                        │
 │  Gleam Application State                                               │
 │    │                                                                   │
 │    ▼                                                                   │
 │  Microtask Queue (JS_ExecutePendingJob - Promise resolution)           │
 │    │                                                                   │
 │    ▼                                                                   │
 │  Macrotask Loop  (js_std_loop - timers, os.setTimeout)                 │
 └───────────────────────────────────┬────────────────────────────────────┘
                                     │
                         thread-safe callbacks / C ABI
                                     │
 ┌───────────────────────────────────▼────────────────────────────────────┐
 │ CivetWeb Worker Pool (POSIX Threads)                                   │
 │                                                                        │
 │   Thread 1: Static HTTP asset file streaming (./public/*)              │
 │   Thread 2: RFC 6455 socket polling & frame decoders                   │
 └────────────────────────────────────────────────────────────────────────┘
```

1. **Microtasks (Promises):** Gleam `use` expressions desugar into `.then()` Promise continuations managed directly by QuickJS's native job queue.
2. **Macrotasks (Timers & I/O):** Driven by `js_std_loop(ctx)`. The runtime keeps execution alive until all timers and active callbacks resolve.
3. **C Background Reactor:** CivetWeb runs on separate native POSIX threads (`num_threads: 2`), handling static HTTP range requests and raw socket frames without stalling the Gleam evaluation thread.

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
| `on_open` | `ConnectionId` (pointer handle) | Triggered when RFC 6455 handshake completes. |
| `on_message` | `ConnectionId`, `String` | Receives incoming UTF-8 text frames. |
| `on_close` | `ConnectionId` | Triggered when client disconnects or aborts socket. |

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

Manage toolchains using [mise](https://mise.jdx.dev/):

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

---

## 8. License

MIT License. Free for open-source and commercial use.