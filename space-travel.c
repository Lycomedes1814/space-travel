/*
 * space-travel: minimal ncurses disk usage browser.
 *
 * Safety notes:
 *   - lstat() is used throughout; symlinks are never followed.
 *   - Recursion depth is capped at MAX_DEPTH to prevent stack overflow.
 *   - All path construction uses snprintf with bounds checking.
 *   - Entry names are copied with snprintf, which always NUL-terminates.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   600

#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_NAME  256
#define MAX_PATH  4096
#define MAX_DEPTH 128

/* ------------------------------------------------------------------ entry */

typedef struct Entry {
    char          name[MAX_NAME];
    int64_t       disk_usage;   /* st_blocks * 512, summed recursively */
    int           is_dir;
    struct Entry  *parent;
    struct Entry **children;
    size_t        nchildren;
    size_t        cap;
    int           saved_sel;
    int           saved_off;
} Entry;

static Entry *
entry_new(const char *name, int64_t du, int is_dir, Entry *parent)
{
    Entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->disk_usage = du;
    e->is_dir     = is_dir;
    e->parent     = parent;
    return e;
}

static int
entry_push(Entry *parent, Entry *child)
{
    if (parent->nchildren == parent->cap) {
        size_t newcap = parent->cap ? parent->cap * 2 : 16;
        Entry **tmp = realloc(parent->children, newcap * sizeof *tmp);
        if (!tmp) return -1;
        parent->children = tmp;
        parent->cap      = newcap;
    }
    parent->children[parent->nchildren++] = child;
    return 0;
}

static void
entry_free(Entry *e)
{
    if (!e) return;
    for (size_t i = 0; i < e->nchildren; i++)
        entry_free(e->children[i]);
    free(e->children);
    free(e);
}

/* ------------------------------------------------------------------ scan */

static int
cmp_du(const void *a, const void *b)
{
    const Entry *ea = *(const Entry **)a;
    const Entry *eb = *(const Entry **)b;
    if (eb->disk_usage > ea->disk_usage) return  1;
    if (eb->disk_usage < ea->disk_usage) return -1;
    return strcmp(ea->name, eb->name);
}

static void
entry_sort_recursive(Entry *e)
{
    if (e->nchildren == 0) return;
    qsort(e->children, e->nchildren, sizeof *e->children, cmp_du);
    for (size_t i = 0; i < e->nchildren; i++)
        if (e->children[i]->is_dir)
            entry_sort_recursive(e->children[i]);
}

/*
 * Derive the display name (basename) from a full path string.
 * Returns a pointer into 'path'; does not allocate.
 */
static const char *
basename_of(const char *path)
{
    const char *p = strrchr(path, '/');
    if (!p || p[1] == '\0') return path;  /* no slash, or root "/" */
    return p + 1;
}

static Entry *
scan(const char *path, Entry *parent, int depth)
{
    if (depth > MAX_DEPTH) return NULL;

    struct stat st;
    if (lstat(path, &st) != 0) return NULL;

    int64_t du     = (int64_t)st.st_blocks * 512;
    int     is_dir = S_ISDIR(st.st_mode);

    Entry *e = entry_new(basename_of(path), du, is_dir, parent);
    if (!e) return NULL;
    if (!is_dir) return e;

    DIR *dir = opendir(path);
    if (!dir) return e;   /* unreadable dir: keep entry with its own blocks */

    struct dirent *de;
    char child_path[MAX_PATH];

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        int r = snprintf(child_path, sizeof child_path, "%s/%s", path, de->d_name);
        if (r < 0 || (size_t)r >= sizeof child_path) continue; /* path too long */

        Entry *child = scan(child_path, e, depth + 1);
        if (!child) continue;

        e->disk_usage += child->disk_usage;
        if (entry_push(e, child) != 0)
            entry_free(child);   /* OOM: discard child rather than corrupt tree */
    }

    closedir(dir);
    return e;
}

/* ------------------------------------------------------------------ format */

static void
fmt_size(int64_t b, char *buf, size_t n)
{
    static const char *units[] = { "  B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)b;
    int    u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0)
        snprintf(buf, n, "%7"PRId64" %s", b, units[0]);
    else
        snprintf(buf, n, "%7.1f %s", v, units[u]);
}

/* ------------------------------------------------------------------ ui */

typedef struct { Entry *dir; int sel; int off; char root_path[MAX_PATH]; } UI;

/*
 * Keep sel and off consistent with the current directory and terminal size.
 * Must be called after any change to ui->dir, ui->sel, or the terminal size.
 */
static void
ui_clamp(UI *ui)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    int list_h = rows - 2;
    if (list_h < 1) list_h = 1;

    int max_sel = ui->dir->nchildren > 0 ? (int)(ui->dir->nchildren - 1) : 0;
    if (ui->sel < 0)       ui->sel = 0;
    if (ui->sel > max_sel) ui->sel = max_sel;

    if (ui->off < 0)                    ui->off = 0;
    if (ui->sel < ui->off)              ui->off = ui->sel;
    if (ui->sel >= ui->off + list_h)    ui->off = ui->sel - list_h + 1;
}

static void
ui_draw(const UI *ui)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    /* header: title left, path right */
    {
        Entry *chain[MAX_DEPTH + 2];
        int d = 0;
        for (Entry *e = ui->dir; e && d < MAX_DEPTH + 1; e = e->parent)
            chain[d++] = e;

        char path[MAX_PATH];
        int pos = 0;
        for (int i = d - 1; i >= 0; i--) {
            const char *seg = (i == d - 1) ? ui->root_path : chain[i]->name;
            if (i < d - 1 && (pos == 0 || path[pos - 1] != '/'))
                pos += snprintf(path + pos, sizeof path - (size_t)pos, "/");
            pos += snprintf(path + pos, sizeof path - (size_t)pos, "%s", seg);
            if (pos >= (int)sizeof path - 1) { pos = (int)sizeof path - 1; break; }
        }
        if (pos == 0) { path[0] = '\0'; }
        attron(A_REVERSE);
        mvprintw(0, 0, "%-*s", cols, " space-travel");
        mvprintw(0, 14, "%.*s", cols - 14, path);
        attroff(A_REVERSE);
    }

    int list_h = rows - 2;
    if (list_h < 1) { refresh(); return; }

    for (int i = 0; i < list_h; i++) {
        int idx = ui->off + i;
        if ((size_t)idx >= ui->dir->nchildren) break;

        const Entry *c = ui->dir->children[idx];
        char sz[24];
        fmt_size(c->disk_usage, sz, sizeof sz);

        char line[512];
        snprintf(line, sizeof line, "  %s     %s%s",
                 sz, c->name, c->is_dir ? "/" : "");

        if (idx == ui->sel) attron(A_REVERSE);
        mvprintw(1 + i, 0, "%-*.*s", cols, cols, line);
        if (idx == ui->sel) attroff(A_REVERSE);
    }

    if (ui->dir->nchildren == 0)
        mvprintw(1, 0, "  (empty)");

    /* footer: keybindings left, count right */
    attron(A_REVERSE);
    mvprintw(rows - 1, 0, "%-*s", cols,
             "  up/k  down/j  right|enter: open dir  left|backspace: up  q: quit");
    if (ui->dir->nchildren > 0) {
        char info[64];
        snprintf(info, sizeof info, "%d / %zu  ", ui->sel + 1, ui->dir->nchildren);
        mvprintw(rows - 1, cols - (int)strlen(info), "%s", info);
    }
    attroff(A_REVERSE);

    refresh();
}

static void
run_ui(Entry *root, const char *root_path)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    UI ui = { .dir = root, .sel = 0, .off = 0 };
    snprintf(ui.root_path, sizeof ui.root_path, "%s", root_path);
    ui_clamp(&ui);
    ui_draw(&ui);

    int ch;
    while ((ch = getch()) != 'q' && ch != 'Q') {
        switch (ch) {
        case KEY_UP:   case 'k': ui.sel--;  break;
        case KEY_DOWN: case 'j': ui.sel++;  break;

        case KEY_RIGHT: case '\n': case KEY_ENTER:
            if (ui.dir->nchildren > 0) {
                Entry *child = ui.dir->children[(size_t)ui.sel];
                if (child->is_dir) {
                    ui.dir->saved_sel = ui.sel;
                    ui.dir->saved_off = ui.off;
                    ui.dir = child;
                    ui.sel = child->saved_sel;
                    ui.off = child->saved_off;
                }
            }
            break;

        case KEY_LEFT: case KEY_BACKSPACE: case 127:
            if (ui.dir->parent) {
                Entry *parent = ui.dir->parent;
                ui.dir = parent;
                ui.sel = parent->saved_sel;
                ui.off = parent->saved_off;
            }
            break;

        case KEY_RESIZE:
            /* terminal was resized; just redraw */
            break;
        }

        ui_clamp(&ui);
        ui_draw(&ui);
    }

    endwin();
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s", argc > 1 ? argv[1] : ".");
    /* strip trailing slashes; preserve bare "/" */
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/') path[--plen] = '\0';

    /* resolve to absolute path for display */
    char real_path[MAX_PATH];
    if (!realpath(path, real_path))
        snprintf(real_path, sizeof real_path, "%s", path);

    fprintf(stderr, "Scanning %s ...\n", real_path);
    fflush(stderr);

    Entry *root = scan(path, NULL, 0);
    if (!root) {
        int err = errno;
        fprintf(stderr, "error: cannot scan '%s': %s\n", path, strerror(err));
        return 1;
    }

    entry_sort_recursive(root);
    run_ui(root, real_path);
    entry_free(root);
    return 0;
}
