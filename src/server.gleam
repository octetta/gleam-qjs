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
