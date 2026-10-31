# gleam-qjs

A lightweight pipeline to compile **Gleam** projects down to standalone native executables (~700 KB) using **Fabrice Bellard's QuickJS** compiler (`qjsc`), bypassing both Node.js/V8 and the standard BEAM distribution.

---

## Overview

Gleam compiles cleanly to standard ES module JavaScript. Instead of shipping a heavy Node.js runtime or browser bundle, this project:

1. Compiles Gleam code to modern ES modules using `gleam build --target javascript`.
2. Flattens module imports into a single ES bundle using `esbuild`.
3. Preserves native QuickJS platform modules (`std` and `os`).
4. Generates a C wrapper and compiles it into a standalone ELF/Mach-O native executable via `qjsc` and `gcc`.

The resulting binary runs the QuickJS event loop (`js_std_loop`), supporting `setTimeout`, native Promises, async/await, and file I/O out of the box with zero runtime dependencies.

---

## Project Structure

```text
.
├── Makefile                # Self-bootstrapping build pipeline
├── mise.toml               # Toolchain pinning (Gleam & Node)
├── gleam.toml              # Gleam package configuration
├── src/
│   ├── app.gleam           # Gleam entry point and async logic
│   └── ffi.mjs             # QuickJS std/os bridge (timers, file I/O)
└── .gitignore
```

---

## Prerequisites

The project uses [mise](https://mise.jdx.dev/) to manage tooling, alongside a standard C compiler toolchain.

* **mise** (or manually installed `gleam` and `node`)
* **make** and **gcc** / **clang**
* **git** (for fetching QuickJS on first build)

---

## Quick Start

### 1. Provision Environment

Install pinned versions of Gleam and Node:

```bash
mise install
```

### 2. Build and Run

Run `make run` to compile the vendored QuickJS engine, bundle the Gleam project, and execute the native binary:

```bash
make run
```

Expected output:

```text
==> [1/4] Starting async demonstration...
==> [2/4] Woke up after 500ms!
==> [3/4] Writing to file: sample_output.txt
==> [4/4] Read back contents:
    "Written from Gleam running on QuickJS native event loop."
```

### 3. Inspect Binary

```bash
file dist/app
# dist/app: ELF 64-bit LSB pie executable, x86-64 ...

ls -lh dist/app
# -rwxr-xr-x 1 user user 780K dist/app
```

---

## Makefile Targets

| Target | Description |
| :--- | :--- |
| `make` / `make all` | Compiles QuickJS (if needed), builds Gleam, bundles JS, and produces `dist/app`. |
| `make run` | Builds and executes `dist/app`. |
| `make clean` | Cleans Gleam build artifacts and `dist/`. |
| `make distclean` | Runs `clean` and removes the cloned `.vendor/` directory. |

---

## How It Works

### Asynchronous Execution & Microtasks
QuickJS manages microtasks (Promise resolution) internally. The generated C harness uses QuickJS's `js_std_loop(ctx)` to keep the process alive while timers or pending Promises remain unresolved, exiting automatically once the queue settles.

### FFI & Module Preservation
The `Makefile` passes `--external:std` and `--external:os` to `esbuild`. This prevents bundler errors when referencing QuickJS's internal modules:

```javascript
import * as std from 'std';
import * as os from 'os';
```

At link time, `qjsc` resolves these modules directly to native C functions inside `libquickjs.a`.

---

## License

MIT