# DS Player

A small SDL3 frontend for the [melonDS](https://melonds.kuribo64.net) core.
It runs a Nintendo DS ROM in one window, either as a desktop program or as a
WebAssembly page where you pick a `.nds` file.

It needs only the ROM. melonDS's built-in replacement BIOS and generated
firmware boot the game directly.

## Controls

| DS             | Keyboard                        | Gamepad                  |
| -------------- | ------------------------------- | ------------------------ |
| D-pad          | Arrow keys                      | D-pad / left stick       |
| A / B / X / Y  | X / Z / S / A                   | Face buttons by position |
| L / R          | Q / W                           | Shoulders or triggers    |
| Start / Select | Enter / Backspace               | Start / Back             |
| Touch screen   | Click or drag the bottom screen |                          |

| Action                  | Key                                  |
| ----------------------- | ------------------------------------ |
| Fast-forward (hold)     | Tab                                  |
| Vertical / side by side | F2                                   |
| Smooth / sharp pixels   | F4                                   |
| Fullscreen              | F11, Alt+Enter                       |
| Reset                   | Ctrl+R                               |
| Open a ROM (desktop)    | Ctrl+O, or drop a file on the window |

## Saves

The desktop build keeps the save next to the ROM as `<rom name>.sav`, the
same name melonDS uses. The web build keeps it in the browser's storage.
Use **Export save** and **Import save** on the page to move it in or out.

## Building

Run these from the repository root. Fetch the dependencies first:

```bash
git submodule update --init
```

### Desktop (Windows, MSYS2 UCRT64)

```bash
cmake -S player -B build/player-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/player-desktop
```

This produces `build/player-desktop/ds-player.exe`, a single static
executable. Run it with a ROM path, or start it without one to get a file
picker. To play the decomp's own build:

```bash
build/player-desktop/ds-player.exe build/YR9E00/YR9E00.nds
```

### Web (Emscripten)

From a shell where `emsdk_env` has been run:

```bash
emcmake cmake -S player -B build/player-web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/player-web
```

The page is assembled in `build/player-web/site/`. Serve that folder over
HTTP; opening `index.html` from disk does not load the WebAssembly module:

```bash
python -m http.server 8080 -d build/player-web/site
```

Then open <http://localhost:8080>.

## Limits

- Software 3D renderer only, on one thread.
