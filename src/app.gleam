import gleam/io

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
  io.println("==> [1/4] Starting async demonstration...")

  // Await a timer (500ms delay)
  use Nil <- await(sleep(500))
  io.println("==> [2/4] Woke up after 500ms!")

  let filepath = "sample_output.txt"
  let sample_data = "Written from Gleam running on QuickJS native event loop."
  io.println("==> [3/4] Writing to file: " <> filepath)
  let _ = write_file(filepath, sample_data)

  // Pause again before reading
  use Nil <- await(sleep(300))
  let content = read_file(filepath)
  io.println("==> [4/4] Read back contents:")
  io.println("    \"" <> content <> "\"")

  resolve(Nil)
}
