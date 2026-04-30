-- PCSX-Redux agent HTTP handlers.
-- Loaded via: pcsx-redux.exe -webserver -dofile tools/agent/handlers.lua
--
-- Each handler is registered at PCSX.WebServer.Handlers.<name> and is
-- reached via: http://localhost:<port>/api/v1/lua/<name>
--
-- A handler receives one table argument with fields:
--   urlData  = { schema, host, port, path, query, fragment, userInfo }
--   method   = "GET" | "POST" | ...
--   headers  = { [name] = { values... } }
--   form     = { [name] = { values... } }
--   body     = raw request body as a string
--
-- A handler returns a string. If it starts with "HTTP/" it is passed as a
-- raw HTTP response; otherwise it is wrapped in a 200 OK response with
-- Content-Length.

PCSX.WebServer = PCSX.WebServer or {}
PCSX.WebServer.Handlers = PCSX.WebServer.Handlers or {}
local H = PCSX.WebServer.Handlers

----------------------------------------------------------------------------
-- Internal state (event-driven so we don't need to poll emulator flags)
----------------------------------------------------------------------------

local state = {
    vsync_count = 0,
    shell_reached_vsync = nil,   -- set once ShellReached fires
    shell_reached = false,
    paused = true,               -- flipped by Run/Pause events; -run triggers Run
    save_state_loaded_count = 0,
    started_at_ms = nil,
    pad_releases = {},
}

do
    local ok, err = pcall(function()
        local gettime = os and os.time
        if gettime then state.started_at_ms = gettime() * 1000 end
    end)
end

-- One-time event listener registration. Keep handles alive by stashing
-- them in a module-level table so GC doesn't drop them.
local listeners = {}
table.insert(listeners, PCSX.Events.createEventListener("GPU::Vsync", function()
    state.vsync_count = state.vsync_count + 1
    for i = #state.pad_releases, 1, -1 do
        local release = state.pad_releases[i]
        if state.vsync_count >= release.vsync then
            release.pad.clearOverride(release.button)
            table.remove(state.pad_releases, i)
        end
    end
end))
table.insert(listeners, PCSX.Events.createEventListener("ExecutionFlow::ShellReached", function()
    state.shell_reached = true
    state.shell_reached_vsync = state.vsync_count
end))
table.insert(listeners, PCSX.Events.createEventListener("ExecutionFlow::Run", function()
    state.paused = false
end))
table.insert(listeners, PCSX.Events.createEventListener("ExecutionFlow::Pause", function()
    state.paused = true
end))
table.insert(listeners, PCSX.Events.createEventListener("ExecutionFlow::SaveStateLoaded", function()
    state.save_state_loaded_count = state.save_state_loaded_count + 1
end))
table.insert(listeners, PCSX.Events.createEventListener("ExecutionFlow::Reset", function()
    state.shell_reached = false
    state.shell_reached_vsync = nil
end))

----------------------------------------------------------------------------
-- Helpers
----------------------------------------------------------------------------

local function respond(body, content_type)
    content_type = content_type or "text/plain; charset=utf-8"
    return "HTTP/1.1 200 OK\r\nContent-Type: " .. content_type
        .. "\r\nContent-Length: " .. tostring(#body) .. "\r\n\r\n" .. body
end

local function respond_error(code, reason, body)
    body = body or reason
    return "HTTP/1.1 " .. tostring(code) .. " " .. reason
        .. "\r\nContent-Type: text/plain; charset=utf-8"
        .. "\r\nContent-Length: " .. tostring(#body) .. "\r\n\r\n" .. body
end

-- Minimal JSON encoder for the shapes we emit (numbers, bools, strings,
-- arrays, and flat tables of the above). We don't depend on an external
-- JSON lib because upstream doesn't ship one for Lua.
local function json_encode(v)
    local t = type(v)
    if t == "nil" then return "null" end
    if t == "boolean" then return v and "true" or "false" end
    if t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        return tostring(v)
    end
    if t == "string" then
        local escaped = v:gsub('\\', '\\\\'):gsub('"', '\\"')
            :gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
            :gsub('[%z\1-\31]', function(c)
                return string.format('\\u%04x', string.byte(c))
            end)
        return '"' .. escaped .. '"'
    end
    if t == "table" then
        -- Array if keys are 1..N contiguous integers.
        local n = 0
        local is_array = true
        for k, _ in pairs(v) do
            n = n + 1
            if type(k) ~= "number" or k ~= math.floor(k) or k < 1 then
                is_array = false
            end
        end
        if is_array then
            local count = 0
            for _ in pairs(v) do count = count + 1 end
            if count ~= n then is_array = false end
        end
        local parts = {}
        if is_array then
            for i = 1, n do parts[#parts + 1] = json_encode(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            for k, val in pairs(v) do
                parts[#parts + 1] = json_encode(tostring(k)) .. ":" .. json_encode(val)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return "null"
end

local function json_respond(t)
    return respond(json_encode(t), "application/json")
end

local function parse_int(s, default)
    if type(s) ~= "string" then return default end
    local n = tonumber(s)
    if not n then return default end
    return math.floor(n)
end

local pad_buttons = {
    select = PCSX.CONSTS.PAD.BUTTON.SELECT,
    start = PCSX.CONSTS.PAD.BUTTON.START,
    enter = PCSX.CONSTS.PAD.BUTTON.START,
    up = PCSX.CONSTS.PAD.BUTTON.UP,
    right = PCSX.CONSTS.PAD.BUTTON.RIGHT,
    down = PCSX.CONSTS.PAD.BUTTON.DOWN,
    left = PCSX.CONSTS.PAD.BUTTON.LEFT,
    l2 = PCSX.CONSTS.PAD.BUTTON.L2,
    r2 = PCSX.CONSTS.PAD.BUTTON.R2,
    l1 = PCSX.CONSTS.PAD.BUTTON.L1,
    r1 = PCSX.CONSTS.PAD.BUTTON.R1,
    triangle = PCSX.CONSTS.PAD.BUTTON.TRIANGLE,
    circle = PCSX.CONSTS.PAD.BUTTON.CIRCLE,
    cross = PCSX.CONSTS.PAD.BUTTON.CROSS,
    x = PCSX.CONSTS.PAD.BUTTON.CROSS,
    confirm = PCSX.CONSTS.PAD.BUTTON.CROSS,
    square = PCSX.CONSTS.PAD.BUTTON.SQUARE,
}

local function normalize_button(button)
    if type(button) == "number" then return button end
    if type(button) ~= "string" then return nil end
    local key = button:lower():gsub("^%s+", ""):gsub("%s+$", "")
    key = key:gsub("^pad_", "")
    return pad_buttons[key]
end

local function get_pad(port)
    port = tonumber(port) or 1
    port = math.floor(port)
    local slot = PCSX.SIO0 and PCSX.SIO0.slots and PCSX.SIO0.slots[port]
    local pad = slot and slot.pads and slot.pads[1]
    return pad, port
end

local function split_words(s)
    local words = {}
    for word in string.gmatch(s or "", "%S+") do words[#words + 1] = word end
    return words
end

-- Extract a single-valued query param from the raw query string
-- (the server splits form data but keeps query as a raw string).
local function query_param(req, key)
    local q = req.urlData and req.urlData.query or ""
    if q == "" then return nil end
    for pair in string.gmatch(q, "[^&]+") do
        local k, v = string.match(pair, "^([^=]+)=?(.*)$")
        if k == key then return v end
    end
    return nil
end

----------------------------------------------------------------------------
-- Handlers
----------------------------------------------------------------------------

-- GET /api/v1/lua/ping -> "pong"
H.ping = function(req) return respond("pong\n") end

-- GET /api/v1/lua/heartbeat -> JSON state snapshot
H.heartbeat = function(req)
    -- getCPUCycles returns a LuaJIT uint64 (cdata); tostring yields "123ULL".
    -- Strip the literal suffix for JSON consumers.
    local cycles_str = tostring(PCSX.getCPUCycles()):gsub("U?L?L?$", "")
    return json_respond({
        cycles = cycles_str,
        vsync = state.vsync_count,
        paused = state.paused,
        shellReached = state.shell_reached,
        shellReachedVsync = state.shell_reached_vsync,
        saveStateLoadedCount = state.save_state_loaded_count,
    })
end

-- GET /api/v1/lua/ready[?vsyncs=N] -> { ready, reason, vsyncsSinceShell }
H.ready = function(req)
    local required = parse_int(query_param(req, "vsyncs"), 600)
    if not state.shell_reached then
        return json_respond({ ready = false, reason = "shell not reached",
            vsyncsSinceShell = 0, vsyncsRequired = required })
    end
    local elapsed = state.vsync_count - state.shell_reached_vsync
    local ready = elapsed >= required
    return json_respond({
        ready = ready,
        reason = ready and "ready" or "settling",
        vsyncsSinceShell = elapsed,
        vsyncsRequired = required,
    })
end

-- POST /api/v1/lua/exec  body: lua source
-- The Lua source is executed. If it returns a value, we stringify the
-- first return. Errors become HTTP 500.
H.exec = function(req)
    local src = req.body or ""
    if src == "" then
        return respond_error(400, "Bad Request", "empty body; POST Lua source as the request body\n")
    end
    local chunk, load_err = load(src, "agent:exec")
    if not chunk then
        return respond_error(500, "Internal Server Error", "load error: " .. tostring(load_err) .. "\n")
    end
    local ok, result = pcall(chunk)
    if not ok then
        return respond_error(500, "Internal Server Error", "runtime error: " .. tostring(result) .. "\n")
    end
    if result == nil then return respond("") end
    return respond(tostring(result) .. "\n")
end

-- GET /api/v1/lua/console[?since=N] -> JSON lines array
-- "since" is 1-based; the returned "nextSince" is what to pass next call
-- to only see new lines (useful for incremental polling).
H.console = function(req)
    local lines = PCSX.getLuaConsole() or {}
    local since = parse_int(query_param(req, "since"), 1)
    if since < 1 then since = 1 end
    local slice = {}
    for i = since, #lines do slice[#slice + 1] = lines[i] end
    return json_respond({
        total = #lines,
        since = since,
        nextSince = #lines + 1,
        lines = slice,
    })
end

-- POST /api/v1/lua/console_clear -> clears the Lua console widget
H.console_clear = function(req)
    PCSX.clearLuaConsole()
    return respond("")
end

-- POST /api/v1/lua/pad_set  body/query: button [pressed] [port]
-- Examples: "start", "down 1 1", "cross false". Query params also work.
H.pad_set = function(req)
    local words = split_words(req.body or "")
    local button_name = query_param(req, "button") or words[1]
    local pressed_arg = query_param(req, "pressed") or words[2] or "true"
    local port_arg = query_param(req, "port") or words[3] or "1"
    local button = normalize_button(button_name)
    if not button then return respond_error(400, "Bad Request", "unknown pad button\n") end
    local pad, port = get_pad(port_arg)
    if not pad then return respond_error(400, "Bad Request", "pad port not available\n") end
    local pressed = not (pressed_arg == "0" or pressed_arg == "false" or pressed_arg == "up" or pressed_arg == "release")
    if pressed then pad.setOverride(button) else pad.clearOverride(button) end
    return json_respond({ port = port, button = button_name, buttonIndex = button, pressed = pressed })
end

-- POST /api/v1/lua/pad_press  body/query: button [frames] [port]
-- Presses immediately and releases after the requested number of GPU vsyncs.
H.pad_press = function(req)
    local words = split_words(req.body or "")
    local button_name = query_param(req, "button") or words[1]
    local frames = parse_int(query_param(req, "frames") or words[2], 6)
    local port_arg = query_param(req, "port") or words[3] or "1"
    if frames < 1 then frames = 1 end
    local button = normalize_button(button_name)
    if not button then return respond_error(400, "Bad Request", "unknown pad button\n") end
    local pad, port = get_pad(port_arg)
    if not pad then return respond_error(400, "Bad Request", "pad port not available\n") end
    pad.setOverride(button)
    state.pad_releases[#state.pad_releases + 1] = {
        pad = pad, button = button, vsync = state.vsync_count + frames,
    }
    return json_respond({
        port = port, button = button_name, buttonIndex = button,
        pressed = true, releaseVsync = state.vsync_count + frames,
    })
end

-- POST /api/v1/lua/pad_clear  body/query: [port]
H.pad_clear = function(req)
    local words = split_words(req.body or "")
    local port_arg = query_param(req, "port") or words[1] or "1"
    local pad, port = get_pad(port_arg)
    if not pad then return respond_error(400, "Bad Request", "pad port not available\n") end
    for _, button in pairs(pad_buttons) do pad.clearOverride(button) end
    for i = #state.pad_releases, 1, -1 do
        if state.pad_releases[i].pad == pad then table.remove(state.pad_releases, i) end
    end
    return json_respond({ port = port, cleared = true })
end

-- GET /api/v1/lua/pad_status[?port=1]
H.pad_status = function(req)
    local port_arg = query_param(req, "port") or "1"
    local pad, port = get_pad(port_arg)
    if not pad then return respond_error(400, "Bad Request", "pad port not available\n") end
    local pressed = {}
    for name, button in pairs(pad_buttons) do
        if pad.getButton(button) then pressed[#pressed + 1] = name end
    end
    return json_respond({ port = port, vsync = state.vsync_count, pendingReleases = #state.pad_releases, pressed = pressed })
end

-- POST /api/v1/lua/pause
H.pause = function(req)
    PCSX.pauseEmulator()
    return respond("")
end

-- POST /api/v1/lua/resume
H.resume = function(req)
    PCSX.resumeEmulator()
    return respond("")
end

-- POST /api/v1/lua/reset  body: "soft" (default) or "hard"
H.reset = function(req)
    local kind = (req.body or "soft"):gsub("%s+$", "")
    if kind == "hard" then
        PCSX.hardResetEmulator()
    else
        PCSX.softResetEmulator()
    end
    return respond("")
end

-- POST /api/v1/lua/quit  -> terminate the emulator process
H.quit = function(req)
    -- Return first so the client sees a 200 before the socket dies.
    PCSX.nextTick(function() PCSX.quit(0) end)
    return respond("")
end

PCSX.log("agent handlers loaded: "
    .. "ping, heartbeat, ready, exec, console, console_clear, "
    .. "pad_set, pad_press, pad_clear, pad_status, "
    .. "pause, resume, reset, quit")
