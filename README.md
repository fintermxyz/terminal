# terminal

**Website:** https://terminal-site.virtuline.workers.dev

A blazing fast, GPU-accelerated terminal emulator for macOS, written in C,
inspired by [Ghostty](https://ghostty.org): a native, zero-compromise core with
a high-level extension layer. Modules are written in **Lua** and executed by
**LuaJIT**, so extensions are JIT-compiled to native code.

It is also a **file viewer**: open images and 3D models (`.obj` / `.stl`)
straight into the same GPU pipeline, from the CLI, from Finder, or from a Lua
module — and it can register as the default macOS handler for those file types.

## Architecture

```
┌────────────────────────────────────────────────────────┐
│ main.c     window / event loop / input encoding        │
│  ├─ pty.c      forkpty + dedicated reader thread       │
│  ├─ term.c     VT100/xterm parser, grid, scrollback    │
│  ├─ render.c   OpenGL 3.3: glyph atlas, ONE instanced  │
│  │             draw call per frame, damage-driven      │
│  ├─ viewer.c   images (stb_image) + 3D obj/stl viewer  │
│  ├─ plugin.c   LuaJIT module host (events, keybinds)   │
│  └─ mac_open.m Apple "open documents" events           │
└────────────────────────────────────────────────────────┘
```

Performance design:

- **PTY reader thread** — output is consumed the instant the kernel has it and
  the render loop is woken with `glfwPostEmptyEvent`; no polling latency.
- **Bulk parsing** — escape parsing runs over whole output chunks (up to 1 MB
  per frame), not byte-per-wakeup.
- **One draw call** — every cell background, glyph, underline, and the cursor
  are instances in a single `glDrawArraysInstanced` against one atlas texture.
- **Damage-driven** — nothing is drawn (and near-zero CPU is used) unless the
  grid changed; vsync caps redraw at display refresh.

## Install

```sh
brew install --cask fintermxyz/tap/terminal
```

or grab `terminal.dmg` / `terminal.pkg` from the
[latest release](https://github.com/fintermxyz/terminal/releases/latest).
Requires macOS 15+ on Apple Silicon. The app isn't notarized yet: if macOS
blocks the first launch, use System Settings → Privacy & Security → Open Anyway.

## Build

```sh
brew install glfw freetype luajit   # already present on most dev machines
make            # -> ./terminal
make app        # -> terminal.app (Finder integration / file handler)
make register   # register document types (uses duti if installed)
make package    # self-contained app -> dist/terminal.dmg + dist/terminal.pkg
```

Releases are automatic: bump `CFBundleShortVersionString` in `assets/Info.plist`
and push to `main`. The `release` workflow builds the DMG/PKG, publishes
`v<version>`, and updates the Homebrew cask.

## Use

```sh
./terminal              # interactive shell (login shell, TERM=xterm-256color)
./terminal model.obj    # open the 3D viewer over the shell
./terminal photo.png    # open the image viewer
./terminal notes.txt    # unsupported-by-GPU types open in the shell's pager
```

| Input | Action |
|---|---|
| `cmd+v` | paste (bracketed-paste aware) |
| `cmd+=` / `cmd+-` / `cmd+0` | font size live resize |
| `cmd+k` | clear |
| mouse wheel / `shift+pgup/pgdn` | scrollback |
| click a file name | select it (click again or `enter` to open) |
| `cmd`+click a file name | open it straight away |
| `tab` on an empty prompt | select the file name nearest the prompt |
| **link selected:** `tab` / `shift+tab` / arrows | move between file names |
| **link selected:** `enter` / `esc` | open (directories `cd` there) / let go |
| **viewer:** drag | orbit model / pan image |
| **viewer:** wheel | zoom |
| **viewer:** `esc` or `q` | back to the shell |

The terminal supports 256-color + truecolor SGR, alternate screen (vim, htop),
scroll regions, mouse reporting (SGR 1006), bracketed paste, DEC line-drawing
charsets, wide/CJK glyphs, OSC window titles, and 5000 lines of scrollback.

## Modules (LuaJIT)

Config lives in `~/.config/terminal/init.lua`, modules in
`~/.config/terminal/modules/*.lua` (the repo-local `modules/` dir is also
loaded, for development). Example `init.lua`:

```lua
terminal.config = {
  font        = "/System/Library/Fonts/Menlo.ttc",
  font_size   = 14,
  cols        = 120,
  rows        = 34,
  padding     = 10,
  scrollback  = 10000,
  cursor_style = "block",          -- block | bar | underline
  foreground  = "#c0caf5",
  background  = "#1a1b26",
  cursor_color = "#c0caf5",
  palette = {                       -- 16 ANSI colors, optional
    "#15161e", "#f7768e", "#9ece6a", "#e0af68",
    "#7aa2f7", "#bb9af7", "#7dcfff", "#a9b1d6",
    "#414868", "#f7768e", "#9ece6a", "#e0af68",
    "#7aa2f7", "#bb9af7", "#7dcfff", "#c0caf5",
  },
}
```

### API

| Function | Description |
|---|---|
| `terminal.on(event, fn)` | subscribe to an event (below) |
| `terminal.bind("cmd+shift+k", fn)` | keybinding (ctrl/alt/shift/cmd + key) |
| `terminal.send(str)` | write to the shell's stdin |
| `terminal.write(str)` | draw into the terminal (escape codes ok), not sent to shell |
| `terminal.view(path)` | open a file in the built-in viewer |
| `terminal.set_title(str)` | set the window title |
| `terminal.size()` | returns `cols, rows` |
| `terminal.log(str)` | log to stderr |
| `terminal.version` | version string |

### Events

| Event | Signature | Notes |
|---|---|---|
| `startup` | `fn()` | after modules load |
| `output` | `fn(chunk)` | raw shell output stream |
| `input` | `fn(str) -> nil \| false \| str` | `false` swallows, string replaces |
| `key` | `fn(key, spec) -> bool` | `true` consumes the key |
| `title` | `fn(title)` | OSC title changes |
| `resize` | `fn(cols, rows)` | grid resized |
| `bell` | `fn()` | BEL received |

Example — auto-open any image path printed by the shell (put in
`~/.config/terminal/modules/autoview.lua`):

```lua
terminal.on("output", function(chunk)
  local p = chunk:match("VIEW:([%w%p]+%.png)")
  if p then terminal.view(p) end
end)
```

## Default file handler

`make app` builds `terminal.app` whose `Info.plist` declares viewer roles for
images, 3D models, and text/source files. `scripts/set-default-handlers.sh`
registers it with LaunchServices and — if [duti](https://github.com/moretension/duti)
is installed — makes it the *default* app for `.obj`/`.stl` (edit the script to
take over images/text too). Or use Finder: Get Info → Open with → Change All.

## Roadmap

- text selection + `cmd+c`
- font fallback chain / color emoji
- reflow on resize
- sixel / kitty graphics protocol in the grid itself
- more model formats (glTF, PLY) and textured meshes
