# PCSX-Redux agent control

Drive PCSX-Redux from the outside via plain HTTP. No wrapper binary — `curl`
is the CLI. Useful for AI agents, CI, and scripted reverse-engineering runs.

## Launch

```cmd
pcsx-redux.exe -iso <path.iso> -run -webserver -webserver-port 8080 ^
  -dofile tools\agent\handlers.lua
```

Flags:

- `-webserver` / `-no-webserver` — enables the HTTP server at startup.
  Equivalent to flipping the `WebServer` debug setting in the GUI.
- `-webserver-port N` — overrides the default port 8080.
- `-dofile tools/agent/handlers.lua` — registers the agent request handlers
  into `PCSX.WebServer.Handlers`. Without this the server is up but the
  `/api/v1/lua/*` namespace returns 404.

The server binds `0.0.0.0:8080` by default (upstream behavior). If that's
undesirable, firewall the host or set the bind address through the config.

## Endpoints

All agent endpoints live under `/api/v1/lua/`. Upstream endpoints
(`/api/v1/state/…`, `/api/v1/cpu/ram/raw`, `/api/v1/gpu/vram/raw`, …) are
also available unchanged.

| Method | Path | Purpose |
| :-- | :-- | :-- |
| GET  | `/api/v1/lua/ping` | Liveness probe. Returns `pong`. |
| GET  | `/api/v1/lua/heartbeat` | JSON snapshot: CPU cycle count, vsync count, paused flag, ShellReached flag, save-state-loaded count. |
| GET  | `/api/v1/lua/ready?vsyncs=N` | Becomes `ready:true` once BIOS hands off (`ShellReached`) **and** at least `N` vsyncs have elapsed since. Default `N=600` (~10s NTSC). |
| POST | `/api/v1/lua/exec` | Body is a Lua source snippet. `load()` + `pcall()`-ed on the emulator's Lua VM. Returns the stringified first return value (empty if none). Errors become HTTP 500. |
| GET  | `/api/v1/lua/console?since=N` | JSON: `{total, since, nextSince, lines:[{type,text}...]}`. Pass the previous response's `nextSince` to poll incrementally. `type` is `"normal" \| "command" \| "error"`. |
| POST | `/api/v1/lua/console_clear` | Clears the GUI's Lua console buffer (same as clicking the **Clear** button). |
| POST | `/api/v1/lua/pause` | `PCSX.pauseEmulator()`. |
| POST | `/api/v1/lua/resume` | `PCSX.resumeEmulator()`. |
| POST | `/api/v1/lua/reset` | Body `soft` (default) or `hard`. |
| POST | `/api/v1/lua/quit` | Terminates pcsx-redux.exe with exit 0. |

## Typical agent flow

```bash
# Wait for a freshly-booted game to be safe to save-state.
until curl -sf localhost:8080/api/v1/lua/ready | grep -q '"ready":true'; do sleep 1; done

# Run a probe.
curl -s -X POST --data 'return PCSX.getCPUCycles()' localhost:8080/api/v1/lua/exec

# Read the console, then clear it.
curl -s localhost:8080/api/v1/lua/console
curl -s -X POST localhost:8080/api/v1/lua/console_clear

# Save state slot 3 (upstream endpoint, no handler needed).
curl -s -X POST localhost:8080/api/v1/state/3

# Detect a hung game: call heartbeat twice; if cycles are equal and
# paused=false, the game is frozen.
curl -s localhost:8080/api/v1/lua/heartbeat

# Done.
curl -s -X POST localhost:8080/api/v1/lua/quit
```

## Detecting a crash

There is no dedicated endpoint. Any connection attempt that returns
`Connection refused` means the emulator process is gone. Monitor the child
process exit code from whatever you used to launch it.

## Extending

Open `handlers.lua` and add to `PCSX.WebServer.Handlers`. Every function on
that table is served at `/api/v1/lua/<function-name>`. The request table
includes `urlData`, `method`, `headers`, `form`, and (this fork's addition)
the raw `body` string. Restart pcsx-redux after editing.

Anything not yet exposed as a dedicated endpoint can be done ad hoc through
`/api/v1/lua/exec` — input injection, breakpoint watches, memory pokes,
fast-forward, etc. — since that runs arbitrary Lua on the emulator VM.
