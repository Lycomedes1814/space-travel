# space-travel

Minimal ncdu clone in C with ncurses.

## Build

```sh
make
```

Requires ncurses (`-lncurses`). Compiled with `-std=c11 -Wall -Wextra -Wpedantic`.

## Run

```sh
./space-travel [path]   # defaults to .
```

## Design constraints

- **Minimal**: no features beyond browsing disk usage.
- **Safe**: `lstat` only (no symlink following), bounded recursion (`MAX_DEPTH 128`), all path construction bounds-checked with `snprintf`.
- **Feature-test macros**: `_POSIX_C_SOURCE 200809L` and `_XOPEN_SOURCE 600` (required for `realpath`).
- **No external deps** beyond libc and ncurses.

## Source layout

Single file: `space-travel.c`. Sections in order:
1. `Entry` struct + tree helpers (`entry_new`, `entry_push`, `entry_free`)
2. `scan` — recursive directory walker
3. `fmt_size` — human-readable size formatting
4. UI (`UI` struct, `ui_clamp`, `ui_draw`, `run_ui`)
5. `main`
