import * as std from 'std';
import * as os from 'os';

// 1. setTimeout wrapped in a native Promise
export function sleep(ms) {
  return new Promise((resolve) => {
    // quickjs-libc puts setTimeout on globalThis or in module 'os'
    if (typeof setTimeout !== 'undefined') {
      setTimeout(resolve, ms);
    } else {
      os.setTimeout(resolve, ms);
    }
  });
}

// 2. Promise chaining helper for Gleam's `use` syntax
export function await_promise(promise, callback) {
  return promise.then(callback);
}

export function resolve_promise(value) {
  return Promise.resolve(value);
}

// 3. Simple synchronous File I/O via QuickJS std
export function write_file(path, content) {
  try {
    const f = std.open(path, "w");
    if (!f) return false;
    f.puts(content);
    f.close();
    return true;
  } catch (_e) {
    return false;
  }
}

export function read_file(path) {
  try {
    const content = std.loadFile(path);
    return content !== null ? content : "";
  } catch (_e) {
    return "";
  }
}
