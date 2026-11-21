# DAP GUI Debugger — Functional & DAP Specification for an Avalonia (C#) Port

**Full path of this document:** `/home/ronny/repos/libdap/tools/dap-debugger/AVALONIA_PORT_SPEC.md`

This document describes, in implementation‑agnostic terms, the **complete functionality** of the
reference `dap_gui_debugger` (a SDL3 + Dear ImGui desktop app written in C++). The goal is to let an
Avalonia/C# debugger replicate the same behaviour and the same DAP wire contract against the same
backend (the libdap-based mock server and the nd100x emulator).

It is organised in three layers:

1. **Application model & lifecycle** — how the app is structured, states, threading, connection flow.
2. **The panels (the UI surface)** — every window/tab, what it shows, and every user action it offers.
3. **The DAP contract** — every request the client sends, every event it consumes, and the custom
   (non‑standard) extensions. This is the part your Avalonia DAP layer must hook up to.

The reference targets a **retro CPU emulator** (Norsk Data ND‑100, a 16‑bit machine with an 8‑register
architecture, split I/D address spaces, and PIL interrupt levels). Several features (physical vs
virtual memory, PIL selection, CPU trace ring, register watchpoints) exist specifically for that
domain. Keep them — they are part of the value of this tool — but they are all expressed through DAP.

---

## 1. Application Model & Lifecycle

### 1.1 The two objects that matter

The whole app is built around **one client object** and **one UI object**:

- **`DebuggerClient`** — owns the socket, the DAP protocol, and *all debug state* (stack frames,
  scopes/variables, breakpoints, disassembly cache, symbols, modules, threads, trace ring, console
  logs, terminal capture buffers, server capabilities). The UI never talks DAP directly; it calls
  methods on this object and reads its public state. In C# this is your `DebuggerClient`/`DapSession`
  view‑model — ideally a single object exposing observable collections and an async DAP API.

- **`UIMain`** — owns the panels and the menu/toolbar/status bar. Every frame it calls
  `client.poll()` then renders each panel, passing the client by reference. In Avalonia you do **not**
  poll every frame; instead drive `poll()`-equivalent logic from a background read loop / async
  message pump and raise change notifications (`INotifyPropertyChanged` / `ObservableCollection`).

### 1.2 Client state machine

```
Disconnected ──connect──▶ Connected ──initialize──▶ Initialized
                                                         │
                                          launch / attach ▼
                                                      Running ◀────────┐
                                                         │  continue/step
                                       stopped event ▼   │ (also re-arm)
                                                      Stopped ─────────┘
                                                         │
                                       terminated/exited ▼
                                                     Terminated
```

State enum: `Disconnected, Connected, Initialized, Running, Stopped, Terminated`.
A human-readable `state_string()` is shown in the status bar and Debug Status panel.

**Gating rules** used throughout the UI (replicate these — they decide which buttons are enabled):

- `canRun` (enable Run/Continue) = `Stopped || Initialized`
- `running` (enable Break/Pause) = `Running`
- `stopped` (enable all stepping, disassemble, memory read, variable edits, symbol fetch, trace
  refresh) = `Stopped`
- Step Back is additionally gated on the server capability `supportsStepBack`.
- The Watchpoints tab only appears when the server reports `supportsDataBreakpoints`.

### 1.3 Threading model

C++ reference: single thread, immediate-mode UI; `poll()` runs once per render frame and uses a short
(10 ms) receive timeout. Many requests are **synchronous** (block for their response); a few
(launch/attach/continue/steps/configurationDone) are **fire‑and‑forget**, with their results handled
later in `poll()`.

**For Avalonia**, do this instead:

- Run the socket read loop on a background task. Parse incoming DAP messages and marshal state updates
  onto the UI thread (`Dispatcher.UIThread.Post`) so observable collections update safely.
- Make the request methods `async Task<T>` (await the matching response by `seq`), but keep the
  *fire‑and‑forget* set (launch/attach/continue/steps/configurationDone) as send‑and‑return; their
  outcome is observed through events and response handling, not by awaiting.
- Preserve the **deferred auto-refresh** concept (§3.13): when a `stopped` event arrives, set a
  "needs refresh" flag and, once the inbound burst settles, run the refresh cascade. This avoids
  issuing blocking queries while async responses are still in flight.

### 1.4 Startup / command-line

CLI flags (keep equivalents): `--host` (default `localhost`), `--port` (default `4711`),
`--program`/positional path, `--debug` (verbose logging). If a program is supplied on launch, the app
auto-connects → initializes → launches. Otherwise it starts **Disconnected** and waits for the user
to connect via the UI.

### 1.5 Window chrome

- **Menu bar**: `File` (Connect…, Attach to Running…, Launch…, Disconnect, Quit) and
  `Debug` (Run F5, Break F6, Step Over F10, Step In F11, Step Out Shift+F11, Step Back F9). Items are
  enabled/disabled by the gating rules above.
- **Toolbar** (always visible, below the menu): Connect/Disconnect (+ a **Reconnect** button and an
  orange "Connection lost" label that appears when the session drops), then Run (F5), Break (F6),
  Step Over, Step In, Step Out, Step Back. Step Back shows a tooltip "Server does not support reverse
  execution" when disabled.
- **Status bar** (bottom, single line, colour-coded by state): state text; stop reason in
  parentheses; `| <function> @ 0xIP (source:line)` for the top frame; `| <process name>`.
- **Docking**: the reference uses a dockspace with a saved layout. Default layout:
  - Left/center large area: **Source** (also hosts Symbols, Memory, CPU Trace as tabs by default).
  - Right column top: **Variables** / **Debug Status** / **Watch**.
  - Right column bottom: **Stack** / **Threads** / **Breakpoints**.
  - Bottom strip: **Console** / **Terminal I/O** / **DAP Protocol Log**.
  - In Avalonia use a docking library (e.g. Dock.Avalonia) to reproduce a movable, persisted layout.

### 1.6 Dialogs

- **Connect to Server** (modal): Host, Port, optional Program. Two actions:
  - **Connect & Launch** — connect → initialize → (if program set) launch.
  - **Attach & Inspect** — connect → initialize → `attach` → `pause` → poll once (snapshots a running
    emulator). Shows an inline red error on failure.
- **Launch Program** (modal): Program path (empty = run without loading, e.g. boot from floppy),
  Source file, Map file, Working directory, **Stop on entry** checkbox.

### 1.7 Keyboard shortcuts

F5 continue, F6 pause, F9 step back (if supported), F10 step over, F11 step in, Shift+F11 step out.
Suppress shortcuts while a text field has focus.

---

## 2. The Panels (UI surface)

Every panel shows "Not connected" when `Disconnected`. Colour conventions used consistently:
current/active line = yellow; changed value = orange (often with a trailing `*`); verified = green
"OK"; pending = yellow "Pend"; breakpoint marker = red `*`; symbols/cross-refs = cyan.

### 2.1 Source panel (tabbed — the centerpiece)

A tab bar with five tabs:

**Source tab**
- Auto-loads the file from the **top stack frame's `source.path`** whenever it changes (reads the file
  from local disk — the path is the server's path; in C# read it the same way, fall back to a "could
  not load" message).
- Renders line numbers + text with a **clickable gutter**. A red `*` marks lines that have a source
  breakpoint; clicking the gutter **toggles** a breakpoint on that line (add if none, remove if
  present). The **current execution line** (top frame's `line`) is highlighted yellow with a `>` marker
  and the view auto-scrolls so it sits ~5 lines from the top.
- Use a virtualized list (the reference uses a clipper); files can be large.

**Location tab**
- Shows the top frame's function name, IP (`0xXXXX`), source name + line, full path, and a
  "Call Location Trail" listing every frame (`#i 0xIP name source:line`), top frame highlighted.

**Disassembly tab**
- Inputs: **Address** (hex), **Count** (1–200), **Disassemble** button (enabled only when Stopped),
  **Go to IP** (sets address to current IP and disassembles), **Copy** (copies the listing to clipboard).
- A 4-column table: **Address | Bytes | Instruction | Symbol**. The current IP row is highlighted
  yellow with `>`; rows with an instruction breakpoint show a red `*` prefix and red text.
- **Click the address cell** to toggle an instruction breakpoint at that address.
- **Right-click a row** → context menu: Set/Remove breakpoint, **Run to here** (set temp BP +
  continue), Copy address, **Add to watches** (adds `*0xADDR`).
- Address values arrive from the server as **strings**; parse hex-aware.

**Memory tab** (a simpler inline hex view; the full Memory panel §2.10 is separate)
- Address (hex) + Bytes (1–1024) + **Read** (Stopped only). Decodes the base64 response and shows a
  classic `offset  hex×16  |ascii|` hex dump.

**Modules tab**
- **Refresh Modules** (Stopped only). Table: Name | Path | Version | Symbols (status) | Range.

### 2.2 Variables panel (title: "Variables")

- Shows **scopes as collapsible trees**, each with a table **Name | Value | Type**. For the ND-100
  the scopes are typically `Locals`, `Registers`, `Memory` — but render whatever scopes the server
  returns. Header shows the per-scope variable count.
- **Change highlighting**: values that differ from the previous stop are drawn orange with a trailing
  ` *`. (The client keeps a `prev_scopes` snapshot to compute this — replicate it.)
- **Refresh** button (Stopped only) re-pulls stack→scopes→variables.
- **Inline edit**: click a value (or right-click → "Edit value…") to edit it; Enter commits via
  `setVariable`; Escape/click-away cancels. Editing a register (e.g. the PC) triggers a stack refresh.
- **Right-click a variable** → Copy value, **Add to watches**, **Break on change** (arms a
  one-click any-change register watch `reg:NAME`), **Register watch…** (opens the visual builder,
  §2.4), and Edit value.

### 2.3 Watch panel

- Text box + **Add** (or Enter) to add an expression. Table **Expression | Value | [delete]**.
- Values are (re)evaluated on every stop via `evaluate` with `context:"watch"`; changed values are
  orange with `*`. Expressions can be variable names, or `*0xADDR` (memory deref), etc. — the server
  defines the expression grammar. "(not evaluated)" until first evaluation. Per-row delete button.

### 2.4 Register Watch dialog (shared modal — the visual watchpoint builder)

A single reusable modal, openable from **Variables → Register watch…** and from **Breakpoints →
Watchpoints → + Register watch…** and per-row **Edit (E)**. It builds a *register* data breakpoint
visually and is the one place that emits `reg:NAME` + a condition string.

- **Register selector**: a dropdown of live register names (from the Registers scope) for a new watch;
  fixed when editing an existing watch.
- **Mode** (radio): *changes (any)* / *matches a value* / *has a bit condition*.
  - **Value mode**: operator dropdown (`==`, `!=`, `<`, `>`, `<=`, `>=`), a value field, and an
    optional mask field. Both accept hex (`0x…`) or decimal. Emits `[& MASK] OP VALUE`.
  - **Bit mode**: a **clickable bit grid** (MSB→LSB, 8 per row, width inferred from the register's
    type string e.g. "32-bit"). Bits that are set in the register's live value are green; the chosen
    target bit is blue. Sub-mode (radio): `-> 1 (rises)` / `-> 0 (falls)` / `changed`.
    Emits `bit N -> 1` | `bit N -> 0` | `bit N changed`.
  - **Any change**: empty condition.
- A **live preview** line shows exactly what will be armed: `reg:NAME  <condition>`.
- **Set/Update**: editing is implemented as *remove old + add new* (DAP has no in-place edit). Both go
  through `add_data_breakpoint("reg:"+name, accessType=write, condition)`.
- The dialog also implements **reverse parsing** of an existing condition string back into the fields
  (for the Edit prefill). The condition grammar is interpreted **server-side**; the client only
  builds/parses the string. Document the grammar in your C# port comments exactly as below.

### 2.5 Stack panel

- Table **# | Function | Address | Location**. Selecting a frame (when Stopped) calls
  `refresh_variables(frame.id)` so Variables reflect the chosen frame. Right-click a frame: Switch to
  frame, Copy frame address, **Go to in disassembly** (disassembles at the frame IP).

### 2.6 Threads panel

- **Refresh Threads** (Stopped). Table **ID | Name**; current thread highlighted with `>`. Header
  shows the current thread id.

### 2.7 Breakpoints panel (tabbed)

**Source tab** — File + Line + **Add** + **Clear All** (for that file). Table
**ID | Source | Line | Condition | Status | [delete]**. Auto-fills the file field from the current
frame's source. Status = green OK / yellow Pend.

**Instruction tab** — Address (hex) + **Add** + **Clear All**. Table
**ID | Address | Condition | Status | [delete]**.

**Watchpoints tab** (only when `supportsDataBreakpoints`) — Address/ID + Access (Read/Write/ReadWrite)
+ **Add**, **Clear All**, a free-text **Condition** field (with a tooltip documenting the register
watch grammar), and **+ Register watch…** (opens §2.4). Table
**ID | Data ID | Address | Access | Condition | Status | [edit/delete]**. Register watches
(`reg:NAME`) show `—` for address and get an **Edit (E)** button; memory watches show the numeric
address.

### 2.8 Symbols panel

- Primary source: the custom `symbolList` request — **Fetch Symbols** (Stopped). Filter box, Type
  dropdown (All / Functions / Labels / Variables).
- Fallback: **Scan Disasm** over a Start..End address range (extracts symbol names from disassembly,
  capped at 2000 instructions) and **+ Stack** (harvest names from stack frames).
- Table **Address | Symbol | Type | Source | Line**, sortable, current-IP row highlighted.
- Right-click a symbol: Go to address (disassemble), Set breakpoint, **Set BP and continue**, Copy
  address, Add to watches.

### 2.9 Console panel (REPL + log)

- A scrolling, copyable log of everything the client logs: info/warning/error, DAP events, DAP
  responses, user input. Buttons: Clear, Copy All, Auto-scroll.
- An **input line + Eval**: sends the text via `evaluate` with `context:"repl"` against the top frame;
  the result is logged.
- Log categories to model: `Info, Warning, Error, DapEvent, DapResponse, UserInput`.

### 2.10 Memory panel (full-featured, ND-100 address spaces)

- **Address space** radio: Virtual / Physical / I-space / D-space.
- Address (hex) + Bytes (1–4096) + **PIL** dropdown (Current, 0–15).
- **Read** builds a `memoryReference` string with the space prefix and optional `@pil` suffix (see
  §3.8), reads, base64-decodes, and shows a `offset  hex×16  |ascii|` dump with the real (space, addr)
  in the header.
- **Write hex** parses a space/comma-separated hex byte string, base64-encodes it and writes at the
  address (virtual/physical). Field hint: `'d1 00 00 00'`.

### 2.11 CPU Trace panel (custom RetroCore extension — the "reverse execution" substitute)

The emulator is forward-only; instead you arm a **ring buffer** of retired instructions and read back
the last N after a stop/crash.

- **Tracing controls**: Enabled checkbox, **Ring capacity** (int), **PC filter** checkbox + hex PC,
  **Apply** (sends `setCpuTracing`).
- **Server-reported status** line: tracing on/off, ring on/off + capacity, total instructions
  executed, active PC filter.
- **Ring read controls**: **Max entries**, **Refresh** (`getCpuTraceRing`, Stopped only),
  **Auto-refresh on stop** (auto-reads once on each transition into Stopped if the ring is enabled).
- **Table**: PC | Opcode | Mnemonic | Instruction (the `text` field is a fully formatted
  disassembly + register line). Oldest entry first; a server-provided header line is shown above.

### 2.12 Terminal I/O panel (custom console extension)

For interacting with the emulated machine's serial terminals.

- Terminal selector (a list of known ND-100 IOX device addresses, e.g. Console `0300`=192, Terminal 1
  `0310`=200 … Terminal 7 `0370`=248) + a custom numeric id field.
- **Enable Capture / Disable Capture** (toggles `consoleEnable`; only one active terminal at a time).
- Output area (green-on-black, copyable) fed by `output` events while capture is active. **Clear** and
  **Copy All**.
- Input line + **Send**, a **CR** button (sends `\r`), an "Append CR on send" checkbox, Auto-scroll.
  Sends via `consoleWrite`.

### 2.13 Debug Status panel

A read-only dashboard: connection state; Process (name, PID, stop reason, hit breakpoint ids, current
thread, thread count); Current Location (function, IP, source:line, path, stack depth, variable/scope
counts); **Server Capabilities** table (name → Yes/No); Session stats (counts of source/instruction/
data breakpoints, modules, symbols, console/protocol log sizes, terminal capture state,
auto-disassemble flag).

### 2.14 DAP Protocol Log panel (essential for development)

A raw, copyable transcript of every JSON message, prefixed `>>>` (sent) / `<<<` (received). Filters:
Sent/Received, Requests/Responses/Events, and a text search box. Clear, Copy All, Auto-scroll. Your
DebuggerClient must log every outbound and inbound message here (see `log_protocol` hooks).

---

## 3. The DAP Contract (what to hook up)

Transport: DAP over TCP, JSON messages framed with `Content-Length:` headers (standard DAP). Every
request is `{"seq":N,"type":"request","command":...,"arguments":{...}}` with a per-session
incrementing `seq`. Responses carry `request_seq`, `success`, `command`, optional `message`, and a
`body`. Events are `{"type":"event","event":...,"body":{...}}`.

> **Two response-shape conventions exist** (replicate exactly):
> - **Standard** commands read result fields from a nested **`body`** object
>   (stackTrace, scopes, variables, threads, modules, readMemory, writeMemory, setBreakpoints,
>   evaluate, …).
> - **Custom** commands (`symbolList`, `setCpuTracing`, `getCpuTraceRing`) read result fields from the
>   **top level** of the response body returned by the libdap request helper. Mirror this when you
>   parse, or normalize on the server side.

### 3.1 Connection & initialization

**`initialize`** (synchronous). Sent when `Connected`. Arguments:
```json
{
  "clientID": "dap-gui-debugger",
  "clientName": "DAP GUI Debugger",
  "adapterID": "nd100x",
  "pathFormat": "path",
  "linesStartAt1": true,
  "columnsStartAt1": true,
  "supportsVariableType": true,
  "supportsRunInTerminalRequest": false,
  "supportsMemoryReferences": true,
  "supportsInvalidatedEvent": true,
  "supportsMemoryEvent": true
}
```
Response `body` (or bare object) → **capabilities**: every named boolean field (except `success`)
becomes a `(name, supported)` capability. Store them; query by name (default false). On success set
state `Initialized` and **immediately call `threads`** (`refresh_threads`). Capabilities are also
re-parsed from any `capabilities` event and from the initialize response seen in the poll loop.

Capabilities the UI actually checks: `supportsStepBack`, `supportsDataBreakpoints`. (Show all of them
in Debug Status.)

### 3.2 Launch / Attach / Configuration

**`launch`** (fire-and-forget). Sent when `Initialized`. Arguments (optional fields omitted when
empty):
```json
{
  "request": "launch",
  "program": "<path>",
  "sourceFile": "<path>",
  "mapFile": "<path>",
  "cwd": "<path>",
  "stopOnEntry": true,
  "noDebug": false,
  "name": "GUI Debug Session",
  "type": "nd100"
}
```
After sending, send **`configurationDone`** (no args), set state `Running`. A `launch` response with
`success:false` reverts state to `Initialized`. The real stop comes via `process` + `stopped` events.

**`attach`** (fire-and-forget). `{"name":"GUI Debug Session"}` then `configurationDone`, state
`Running`. `success:false` reverts to `Initialized`. The Attach flow then issues `pause` to snapshot a
running target.

**`configurationDone`** — no arguments.

**`disconnect`** — DAP `disconnect` (restart=false, terminate=true). Then clear *all* local debug
state and set `Disconnected`. A separate hard `force_close()` (socket close, no request) is used when
the app is killed by a signal or the transport reports the server died.

### 3.3 Execution control (all fire-and-forget; `threadId` = current thread)

| Method | command | arguments |
|---|---|---|
| Continue | `continue` | `{"threadId":<id>,"singleThread":false}` |
| Step Over | `next` | `{"threadId":<id>}` |
| Step In | `stepIn` | `{"threadId":<id>}` |
| Step Out | `stepOut` | `{"threadId":<id>}` |
| Step Back | `stepBack` | `{"threadId":<id>}` |
| Pause | `pause` | `{"threadId":<id>}` |

All set state `Running` (pause leaves it). A `success:false` response to any step/continue reverts to
`Stopped`.

### 3.4 Stack / scopes / variables

**`stackTrace`** (sync). `{"threadId":<id>,"startFrame":0,"levels":20}`. Reads
`body.stackFrames[]`: `id`, `name`, `line`, `instructionPointerReference` (**string**, hex-aware
parse → IP), `source.path`, `source.name`. After populating, set `current_source` = top frame path and
cascade to scopes.

**`scopes`** (sync). `{"frameId":<id>}` → `body.scopes[]` (`name`, `variablesReference`). Note: a
frameId of literal `0` may be omitted by the libdap helper — be aware when porting.

**`variables`** (sync, once per scope). `{"variablesReference":<ref>}` (add `start`/`count` only when
>0) → `body.variables[]` (`name`, `value`, `type`, `variablesReference`). Save the previous scope
snapshot first (for change highlighting), then rebuild scopes + a flat variable list.

**`threads`** (sync, no args) → `body.threads[]` (`id`, `name`).

**`modules`** (sync). `{"startModule":0,"moduleCount":100}` → `body.modules[]`
(`id`, `name`, `path`, `version`, `symbolStatus`, `addressRange`, `isOptimized`, `isUserCode`).

### 3.5 Source breakpoints (full-set replace per file)

The client keeps the breakpoint list as the source of truth and **re-sends the entire set for a file**
on every add/remove/clear.

**`setBreakpoints`** (sync):
```json
{
  "source": { "path": "<file>" },
  "breakpoints": [
    { "line": <int>, "condition": "<opt>", "hitCondition": "<opt>", "logMessage": "<opt>" }
  ]
}
```
Response `body.breakpoints[]` (index-aligned with the request): `id`, `verified`, `source.path`,
`line`, `instructionReference`, `condition`, `hitCondition`, `logMessage`, `message`. Replace the local
entries for that file.

### 3.6 Instruction breakpoints (full-set replace)

**`setInstructionBreakpoints`** (sync):
```json
{
  "breakpoints": [
    { "instructionReference": "0xHEX", "offset": <opt,!=0>,
      "condition": "<opt>", "hitCondition": "<opt>" }
  ]
}
```
> `instructionReference` is **sent as a string** formatted `0x%X` (no leading zeros / width). Response
> `breakpoints[]` (possibly under `body`) gives `id`, `verified`, `message`; the address/offset/
> condition are recovered **by index** from the request (server doesn't echo them).

### 3.7 Data breakpoints / watchpoints (full-set replace)

**`setDataBreakpoints`** (sync). Each entry:
```json
{ "dataId": "<id>", "accessType": "read|write|readWrite",
  "condition": "<opt>", "hitCondition": "<opt>" }
```
- `dataId` is either a numeric address string (`"0x1000"`) **or** a register watch `"reg:NAME"`
  (e.g. `reg:A`, `reg:P`). The client special-cases the `reg:` prefix (no numeric address).
- `accessType` maps from int 0/1/2 → `read`/`write`/`readWrite`.
- Response `body.breakpoints[]`: `id`, `verified`, `message` only — `dataId`/`accessType`/`condition`
  are recovered **by index** from the request. A `success:false` pops the just-added entry.

**Register-watch condition grammar** (built/parsed by the client, *interpreted by the server* — this
is the single source of truth, keep the strings identical):
```
(empty)            break on any change
== 0x50000204      break when the register equals a value   (also != < > <= >=)
& 0xFF == 0x10     optional mask:  [& MASK] OP VALUE
bit 27 -> 1        break when bit 27 goes 0 -> 1
bit 27 -> 0        break when bit 27 goes 1 -> 0
bit 27 changed     break when bit 27 toggles
```
Values/masks accept `0x` hex or decimal. Register bit width is inferred from the variable's `type`
string (e.g. `"32-bit"` → 32; default 32).

### 3.8 Disassembly

**`disassemble`** (sync):
```json
{ "memoryReference": "0x%08x"<addr>, "offset": <opt>, "instructionOffset": <opt>,
  "instructionCount": <count>, "resolveSymbols": <bool> }
```
Response `body.instructions[]`: `address` (**string**), `instruction`, `instructionBytes`, `symbol`,
`source.path`, `line`. The client maintains a **disassembly cache** with `[cache_start, cache_end]`
and an `disassemble_extend` variant that appends only instructions past `cache_end` and trims the top
(keeps ~120 lines). Replicate the cache + extend so scrolling/stepping doesn't re-fetch constantly.

### 3.9 Memory (base64 payloads; ND-100 address-space encoding)

**`readMemory`** (sync): `{"memoryReference":"<ref>","offset":<int>,"count":<int>}` →
`body.data` (**base64**), `body.address`, `body.unreadableBytes`. Returns the base64 string;
decode to bytes for display.

**`writeMemory`** (sync):
`{"memoryReference":"<ref>","offset":<int>,"data":"<base64>","allowPartial":false}` →
`body.bytesWritten`.

**`memoryReference` encoding convention** (server-parsed). Form: `"[prefix:]address[@pil]"`:
- Virtual (default): `0xADDR` (or `virt:` / `V:`)
- Physical: `phys:0xADDR` (or `P:`)
- I-space: `ispace:0xADDR` (or `I:`)
- D-space: `dspace:0xADDR` (or `D:`)
- Optional PIL level suffix: `@N` (e.g. `ispace:0x40@3`).

The numeric overloads only emit the long prefixes (no `@pil`); to send `@pil` use the **string** form
of the reference (the Memory panel builds the full string itself).

### 3.10 Evaluate & set variable

**`evaluate`** (sync): `{"expression":"<expr>","frameId":<opt,>0>,"context":"repl|watch"}` →
`body.result`. REPL console uses `repl`; the Watch panel uses `watch` and stores the result per-watch,
flagging changes vs the previous value.

**`setVariable`** (sync): `{"variablesReference":<int>,"name":"<str>","value":"<str>"}`. After success
the client calls `stackTrace` again (setting the PC moves the cursor / disasm).

### 3.11 Custom extension: symbol list

**`symbolList`** (sync). Arguments `{}`. Response is read from the **top level** (not `body`):
`symbols[]` each `{ name, address (number), type ("function"|"label"|"variable"), sourcePath, line }`.

### 3.12 Custom extension: CPU execution tracing (RetroCore)

**`setCpuTracing`** (sync):
```json
{ "enabled": <bool>, "ringCapacity": <int>, "pcFilter": <number> }
```
> Omit `pcFilter` entirely to **clear** the server-side filter. Response (top level):
> `enabled`, `ringEnabled`, `ringCapacity`, `pcFilter` (presence ⇒ filter active).

**`getCpuTraceRing`** (sync): `{"maxEntries":<int>}` (omit if ≤0). Response (top level):
`header` (string), `ringCapacity` (int), `totalInstructionsExecuted` (number/uint64),
`entries[]` each `{ pc (number), opCode (number), opCodeName (string), text (string) }`.

### 3.13 Custom extension: terminal console I/O

**`consoleEnable`** (sync): `{"terminal":<int>,"enable":<bool>}`. Track a single `active_terminal`.

**`consoleWrite`** (sync): `{"terminal":<int>,"input":"<str>","hex":<opt bool>}`. The reference always
sends without `hex`. Captured output arrives as `output` events with `category:"stdout"` while a
terminal is active.

### 3.14 The receive loop & deferred auto-refresh

On each poll/drain of inbound messages:
- **Responses**: dispatch by `command` for the state reversions noted above (`launch`/`attach`/steps/
  `continue` failures, `initialize` capabilities, `disconnect`).
- **After** the inbound burst, if a refresh is pending and state is `Stopped`, run the **refresh
  cascade in order**: `threads` → `stackTrace` (→ scopes → variables) → evaluate all watches → then
  **auto-disassemble**:
  - If disassembly is empty or the IP is outside the cache range → `disassemble(ip>20?ip-20:0, 120)`.
  - Else if fewer than 5 cached lines are above the IP → `disassemble_extend(cache_end+1, 100)`.

The `stopped` event handler itself does nothing but set the "needs refresh" flag; the cascade runs
once the message burst settles. This is important so blocking queries don't eat pending async
responses — preserve this ordering in the C# async design.

### 3.15 Events consumed

| event | body fields | effect | follow-up |
|---|---|---|---|
| `stopped` | `reason`, `threadId`, `description`, `hitBreakpointIds[]` | state `Stopped`; store reason / thread / hit ids | trigger refresh cascade |
| `continued` | `threadId`, `allThreadsContinued` | state `Running`; clear reason, hit ids, stack, scopes, variables | — |
| `terminated` | — | state `Terminated`; clear stack/scopes/vars/disasm/reason/hit ids | — |
| `exited` | `exitCode` | state `Terminated` (log code) | — |
| `output` | `output`, `category` | if a terminal is active and `category=="stdout"` → append to terminal buffer; else log (`stderr`→Error, `important`→Warning, else Info; strip trailing newlines) | — |
| `initialized` | — | log only | — |
| `thread` | `reason` (`started`/`exited`), `threadId` | add/remove from threads (name `"Thread N"`) | — |
| `breakpoint` | `reason` (`new`/`changed`/`removed`), `breakpoint{id,verified,message,line,source.path}` | update/insert/remove in breakpoints | — |
| `process` | `name`, `systemProcessId`, `startMethod` | store process name / pid | — |
| `module` | `reason` (`new`/`removed`), `module{id,name,path,symbolStatus}` | add/remove module | — |
| `memory` | `memoryReference`, `offset`, `count` | log only (user re-reads) | — |
| `capabilities` | `capabilities` (or body) | re-parse capabilities | — |
| `invalidated` | — | set "needs refresh" | trigger refresh cascade |
| `progressStart/Update/End` | `title`/`message` | log | — |

### 3.16 Encoding / parsing notes for C#

- `instructionPointerReference` and disassembly `address` arrive as **strings**; parse hex-aware
  (accept `0x…`). Format outgoing `instructionReference` as `0x%X`.
- Memory `data` (read response & write argument) is **standard base64**.
- Custom commands read from the **top level**; standard commands read from **`body`**. Don't mix them.
- Instruction- and data-breakpoint fields the server doesn't echo are recovered by **index alignment**
  between request and response — keep the request snapshot to merge back.
- libdap helpers omit some numeric args when `0` (`frameId`, `start`, `count`, `offset`,
  `instructionOffset`). If your C# server is stricter, send them explicitly or match the omission.

---

## 4. Minimal porting checklist

1. **Transport**: `Content-Length` framed JSON over TCP; per-session `seq`; background read loop;
   await responses by `request_seq`.
2. **Session view-model** holding observable: state, stackFrames, scopes/variables (+prev snapshot),
   breakpoints (source/instruction/data), disassembly (+cache range), symbols, modules, threads,
   trace ring + status, console log, protocol log, terminal buffer + active terminal, capabilities.
3. **Requests** §3.1–3.13 with the exact command strings and argument shapes; respect the two
   response-shape conventions.
4. **Events** §3.15 + the **deferred refresh cascade** §3.14.
5. **Panels** §2 — Source(+Disassembly/Memory/Modules/Location tabs), Variables, Watch, Register-Watch
   builder, Stack, Threads, Breakpoints(3 tabs), Symbols, Console, Memory, CPU Trace, Terminal I/O,
   Debug Status, DAP Protocol Log.
6. **Chrome**: menu, toolbar, status bar, Connect/Launch dialogs, keyboard shortcuts, dockable layout.
7. **Domain features**: physical/I/D/virtual + PIL memory references, register watchpoint grammar,
   CPU trace ring, ND-100 terminal IOX addresses.
8. **Gating**: enable/disable controls by the state rules and by `supportsStepBack` /
   `supportsDataBreakpoints`.
