# space-travel

Minimal ncdu clone in C with ncurses.

## Build

```sh
make
```

Requires ncursesw (`-lncursesw`). Compiled with `-std=c11 -Wall -Wextra -Wpedantic`.

## Run

```sh
./space-travel [path]   # defaults to .
```

## Design constraints

- **Minimal**: no features beyond browsing disk usage.
- **Safe**: `lstat` only (no symlink following), bounded recursion (`MAX_DEPTH 128`), all path construction bounds-checked with `snprintf`.
- **Trash behavior**: prefer platform trash tools (`gio trash`, then `trash-put`) and fall back to a minimal freedesktop-style trash implementation when neither is installed.
- **Feature-test macros**: `_POSIX_C_SOURCE 200809L` and `_XOPEN_SOURCE 600` (required for `realpath`).
- **No external deps** beyond libc and ncurses.

## Source layout

Single file: `space-travel.c`. Sections in order:
1. `Entry` struct + tree helpers (`entry_new`, `entry_push`, `entry_free`, `entry_detach`)
2. `scan` — recursive directory walker (`cmp_du`, `entry_sort_recursive`, `basename_of`, `scan`)
3. `fmt_size` — human-readable size formatting
4. Trash helpers (`remove_entry_recursive`, `run_trash_command`, `try_system_trash`, `trashinfo_escape_path`, `write_trashinfo`, `do_trash`)
5. UI (`UI` struct, `ui_clamp`, `ui_draw`, `entry_full_path`, `run_ui`)
6. `main`

## Keys

| Key | Action |
|-----|--------|
| `j` / `down` | Move down |
| `k` / `up` | Move up |
| `enter` / `right` | Enter directory |
| `backspace` / `left` | Go up to parent |
| `d` | Move selected entry to the system trash when available, otherwise `~/.local/share/Trash/files/` (confirms with y/n) |
| `q` | Quit |
