# SuperCRT (Linux port)

Views a rectangle of your desktop through the CRT simulation from *Super Win the Game* —
scanlines, shadow-mask, composite bleed, NTSC artifacts, bloom and all — on a real
glass-tube-shaped mesh, drawn live over your desktop.

## Credit

**The CRT simulation in this repository is not my work.** It is a port of **CRTSim**, the
reference implementation released by its author:

> **J. Kyle Pittman** — *PirateHearts*
> [piratehearts.itch.io](https://piratehearts.itch.io) ·
> [MinorKeyGames](https://github.com/MinorKeyGames) ·
> [CRTSim source](https://github.com/MinorKeyGames/CRTSim)
>
> "A lightweight reference implementation of the CRT sim from Super Win the Game, etc."

Pittman released CRTSim into the **public domain** under
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/) — the same effect
shipped in *Super Win the Game*, *Gunmetal Arcadia* and *You Have To Win the Game*. The
upstream dedication is preserved verbatim in [`CC0-COPYING.txt`](CC0-COPYING.txt), and the
`assets/` data files (`frame.m3d`, `screen.m3d`, `mask.bmp`, `artifacts.bmp`) are his
original CC0 assets, used unmodified.

Everything here is a derivative: the shader math, tuning constants, defaults, mesh
geometry and asset formats are his. This port only changes *how* it is driven — an X11/GLX
front end in place of the Direct3D 9 / Visual Studio 2005 reference project.

If you find this effect useful, the credit belongs upstream.

## What the port does differently

- **OpenGL instead of Direct3D 9.** Shaders are hand-translated HLSL → GLSL; the API
  mapping notes (texture orientation, cull winding, half-pixel offsets, per-sampler filter
  states) are inline comments where the two disagree. `glXGetProcAddress` resolves
  everything newer than GL 1.1 at runtime, so there is no GLEW/GLAD dependency.
- **X11 desktop capture** via MIT-SHM (`XShmGetImage`), with an `XGetImage` fallback.
- **No Visual Studio, no C++.** Plain C99, one `Makefile`, dependencies are `libX11` and
  `libGL` only.
- **Live settings overlay** written from scratch: an on-screen config menu you can click
  or drive from the keyboard, plus drag-to-place sampling-rectangle targeting. Upstream
  leaves tuning to its source constants and config.
- **Always-on-top and click-through**, so the viewer can sit over the desktop while windows
  behind it are driven — see below.
- **INI config** (`supercrt.ini`) with field names mapped 1:1 onto upstream's
  `Parameters.h` statics, so the port can be diffed against the original by eye. Defaults
  are the upstream values.

## Build

```sh
make            # -> ./supercrt
make install    # PREFIX=/usr/local, installs binary + assets
```

Needs a C99 compiler, `libX11`, `libGL`, `libdl`, `libm` (Debian/Ubuntu:
`build-essential libx11-dev libgl1-mesa-dev`). Builds warning-clean with
`-Wall -Wextra`.

## Run

```sh
./supercrt                 # sample a region of the root window
./supercrt --pattern       # built-in test pattern instead of the desktop
```

Assets are searched for as `assets/*` beside the binary, then `../assets`,
`../../assets`, then `../share/supercrt/assets` (where `make install` puts them).

### Options

| Option | Meaning |
|---|---|
| `--config PATH` | Settings file. Default `./supercrt.ini`, else `$XDG_CONFIG_HOME/supercrt/supercrt.ini` |
| `--src X,Y,W,H` | Capture region on the root window |
| `--dst W,H` | Output window size |
| `--fullscreen` / `--borderless` | Start fullscreen (via WM) or as a borderless screen-sized window |
| `--no-outline` | Hide the capture-region outline |
| `--no-vsync` | Pace frames in software instead of waiting for vblank |
| `--no-shm` | Force the `XGetImage` capture path |
| `--pattern` | Built-in test pattern instead of the desktop |
| `--frame-out FILE` | Render one frame to a binary PPM and exit |
| `--dump-target NAME` | With `--frame-out`: dump `window`, `clean`, `composite`, `full`, `down` or `up` instead of the presented frame |
| `--frames N` | Frames to render before `--frame-out` (default 120) |

### Controls

- **Esc** — settings overlay. **E** — drag/resize the target area. **F11** — fullscreen.
  **Ctrl+Q** / **Q** — quit.
- Mouse: hover the sim window for the bottom bar; **Settings** opens the config menu;
  **Adjust target area** lets you drag the sampled rectangle directly on the desktop.
- In the overlay: **Up/Down** select, **Left/Right** adjust (**Shift** coarse),
  **Home/End** min/max, **Enter** run the selected action, and —
  `c` centre the capture on the cursor, `t` / `b` anchor the capture's top-left /
  bottom-right at the cursor, `e` drag target, `m` outline, `f` window mode,
  `a` always-on-top, `k` click-through, `i` ignore own output, `v` vsync, `s` save,
  `l` reload, `r` defaults, `q` quit. These all need the overlay open, which is why
  click-through also has a global chord: **Ctrl+Alt+C**.

### Configuration

`supercrt.ini` is written in three sections — `[Capture]`, `[Window]`, `[CRT]` — with
**s** in the overlay. The `[CRT]` section is the upstream tunable set (screen-mesh
curvature, scanline and mask strength, persistence, bleed, NTSC artifact amount, bloom,
blur radii, gamma, FOV, …). Delete the file or press **r** to return to upstream defaults.

## Working in the windows behind the viewer

Two `[Window]` settings let the viewer sit over your desktop instead of competing with it.
Both default to **off**.

```ini
[Window]
AlwaysOnTop=true     ; the viewer cannot be covered by other windows
ClickThrough=true    ; the pointer passes through the viewer
IgnoreSelf=true      ; the viewer never samples its own output (on by default)
```

**`AlwaysOnTop`** asks the window manager to keep the viewer above other windows, so
clicking a window underneath raises it *below* the viewer rather than over it. This needs
an EWMH window manager; with none running (the borderless fallback) the viewer keeps itself
in front by re-raising periodically instead.

**`ClickThrough`** becomes the only thing under the pointer that ignores it: clicks, drags
and focus land on whatever is behind, so the desktop can be driven normally with the CRT
view still up. The bottom strip stays visible in this state to say so, with the two mouse
buttons replaced by that notice — they cannot be clicked, so they are not offered.

The consequence to know about is that the viewer's own mouse UI is unreachable while it is
on. Leave it with **Ctrl+Alt+C**, a passive grab on the root window that fires no matter
which window has the keyboard, or by focusing the viewer and pressing **k** in the overlay.
If another client already holds Ctrl+Alt+C the grab fails with a note on stderr; raise the
viewer and press **k** instead. The viewer is normally also reachable through the window
manager's own switcher (**Alt-Tab**), which does not go through the pointer.

## Never sampling itself

The capture reads the desktop, and the viewer draws to the desktop, so if the viewer overlaps
the rectangle it samples it feeds its own output back in: the image cascades, and the sampled
area is no longer what is really there. **`IgnoreSelf`** (on by default) makes that impossible,
two ways:

- **It keeps clear.** `App_WindowPlacement` already worked out a spot beside or below the
  sampled rectangle, but that was only a request — window managers routinely ignore the geometry
  a new window is created with, and KWin did exactly that, leaving the viewer on top of the
  rectangle it was sampling. The placement is now asserted through `USPosition` hints and
  re-requested for the first moment after the window appears, then left alone so a move you make
  yourself is never fought.
- **It punches a hole in itself.** Whenever the window would overlap the rectangle anyway — you
  dragged it there, the screen is too small, or you are in fullscreen or borderless mode where
  covering it is unavoidable — the viewer's bounding shape is cut so it paints everything
  *except* the sampled rectangle. Those pixels stay exactly as the desktop drew them, so the
  capture cannot contain the viewer's output no matter where the window is. The input shape gets
  the same hole, so a pixel the viewer does not paint is also one it does not swallow.

What you see in that case is a raw, un-CRT'd rectangle inside the viewer wherever it overlaps
the sampled area — the desktop showing through the glass, which is the honest picture: those
pixels are what is being sampled.

With `IgnoreSelf=false` the old behaviour returns, including the feedback tunnel, which is
worth having as an effect if you want it. `i` in the overlay toggles it.

## Pipeline

Six passes, matching the reference `Render()`:

1. Grab the desktop region into the `clean` texture.
2. Composite pass — persistence/bleed, NTSC artifacts, unsharp mask → even/odd render
   targets.
3. Screen mesh + frame mesh, lit and masked, into the full-size buffer.
4. Downsample blur of that buffer.
5. Upsample blur.
6. Bloom composite onto the window.

| File | Role |
|---|---|
| `src/main.c` | Window, GL context, render loop, pipeline, input |
| `src/capture.c` | X11 screen capture (MIT-SHM + `XGetImage`) |
| `src/shader.c`, `src/shaders.h` | GLSL programs / the translated shader source |
| `src/assets.c` | BMP and `.m3d` loaders for the upstream data files |
| `src/params.c`, `src/params.h` | Tunables, INI load/save, defaults |
| `src/ui.c` | Settings overlay, sliders, bottom bar |
| `src/marker.c` | Capture-region outline overlay (XShape) |
| `src/xshape.c` | XShape access for the outline and click-through (libXext, `dlopen`ed) |
| `src/gl_api.c` | Runtime GL entry-point resolution |
| `src/font_atlas.h` | Embedded overlay font, baked by `tools/bake_font.py` |

`make font` regenerates the font atlas (needs Pillow); not required to build.

## License

Public domain — **CC0 1.0 Universal**. Upstream CC0 dedication by J. Kyle Pittman in
[`CC0-COPYING.txt`](CC0-COPYING.txt); this port carries the same dedication and the same
original asset files.
