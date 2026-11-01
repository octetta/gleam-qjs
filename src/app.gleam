import gleam/io
import gleam/int
import server

pub type Promise(a)

@external(javascript, "./ffi.mjs", "sleep")
pub fn sleep(ms: Int) -> Promise(Nil)

@external(javascript, "./ffi.mjs", "await_promise")
pub fn await(promise: Promise(a), next: fn(a) -> Promise(b)) -> Promise(b)

@external(javascript, "./ffi.mjs", "resolve_promise")
pub fn resolve(val: a) -> Promise(a)

@external(javascript, "./ffi.mjs", "write_file")
pub fn write_file(path: String, content: String) -> Bool

@external(javascript, "./ffi.mjs", "read_file")
pub fn read_file(path: String) -> String

pub fn main() -> Promise(Nil) {
  io.println("==================================================")
  io.println("   Gleam + QuickJS Native Native Showcase App     ")
  io.println("==================================================")

  // 1. Showcase Async Promises & Timers
  io.println("--> [1/3] Testing async timer...")
  use Nil <- await(sleep(300))
  io.println("          Timer fired after 300ms!")

  // 2. Showcase File I/O via QuickJS std
  let filepath = "showcase_data.txt"
  io.println("--> [2/3] Testing file write & read (" <> filepath <> ")...")
  let _ = write_file(filepath, "Persisted state from Gleam on QuickJS event loop.")
  let content = read_file(filepath)
  io.println("          Read back: \"" <> content <> "\"")

  // 3. Showcase HTTP & WebSocket Server (CivetWeb)
  io.println("--> [3/3] Starting HTTP & WebSocket Server...")
  let handlers = server.ServerHandlers(
    on_open: fn(conn) {
      io.println("          [WS] Client connected: #" <> int.to_string(conn))
      let _ = server.send(conn, "Connected to Gleam QuickJS backend!")
      Nil
    },
    on_message: fn(conn, msg) {
      io.println("          [WS] Received: " <> msg)
      let _ = server.send(conn, "echo: " <> msg)
      Nil
    },
    on_close: fn(conn) {
      io.println("          [WS] Client disconnected: #" <> int.to_string(conn))
    },
  )

  let ok = server.start(8080, "./public", "/ws", handlers)
  case ok {
    True -> {
      io.println("          Server listening on http://localhost:8080")
      io.println("          WebSocket endpoint at ws://localhost:8080/ws")
      io.println("          Serving static files from ./public")
    }
    False -> io.println("          Failed to bind server port 8080.")
  }

  // Keep event loop active
  use Nil <- await(sleep(86400000))
  resolve(Nil)
}
