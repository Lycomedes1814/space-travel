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
#include <sys/wait.h>
#include <unistd.h>

#define MAX_NAME  256
#define PAIR_BAR  1   /* header/footer: bright white on deep purple */
#define PAIR_MAIN 2   /* list body:     bright white on deep-space blue-black */
#define PAIR_SEL  3   /* selected row:  bright white on nebula purple */
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
    int64_t       item_count;  /* total items inside (recursive), 0 for files */
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

/* Remove e from its parent's children array and subtract its usage from all ancestors. */
static void
entry_detach(Entry *e)
{
    Entry *parent = e->parent;
    if (!parent) return;

    size_t i;
    for (i = 0; i < parent->nchildren; i++)
        if (parent->children[i] == e) break;
    if (i == parent->nchildren) return;

    memmove(&parent->children[i], &parent->children[i + 1],
            (parent->nchildren - i - 1) * sizeof *parent->children);
    parent->nchildren--;

    int64_t du = e->disk_usage;
    int64_t ic = e->item_count + 1;  /* items inside + the entry itself */
    for (Entry *p = parent; p; p = p->parent) {
        p->disk_usage -= du;
        p->item_count -= ic;
    }
}

static int
path_append(char *buf, size_t n, size_t *pos, const char *s)
{
    if (*pos >= n) return -1;

    int r = snprintf(buf + *pos, n - *pos, "%s", s);
    if (r < 0) return -1;

    size_t written = (size_t)r;
    if (written >= n - *pos) {
        *pos = n - 1;
        return -1;
    }

    *pos += written;
    return 0;
}

static int
path_append_char(char *buf, size_t n, size_t *pos, char c)
{
    char tmp[2] = { c, '\0' };
    return path_append(buf, n, pos, tmp);
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

        if (entry_push(e, child) != 0)
            entry_free(child);   /* OOM: discard child rather than corrupt tree */
        else
            e->disk_usage += child->disk_usage;
    }

    closedir(dir);

    for (size_t i = 0; i < e->nchildren; i++)
        e->item_count += 1 + e->children[i]->item_count;

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
        size_t pos = 0;
        for (int i = d - 1; i >= 0; i--) {
            const char *seg = (i == d - 1) ? ui->root_path : chain[i]->name;
            if (i < d - 1 && (pos == 0 || path[pos - 1] != '/')) {
                if (path_append_char(path, sizeof path, &pos, '/') != 0) break;
            }
            if (path_append(path, sizeof path, &pos, seg) != 0) break;
        }
        if (pos == 0) { path[0] = '\0'; }
        attron(COLOR_PAIR(PAIR_BAR));
        mvprintw(0, 0, "%-*s", cols, " space-travel");
        if (cols > 14)
            mvprintw(0, 14, "%.*s", cols - 14, path);
        attroff(COLOR_PAIR(PAIR_BAR));
    }

#define BAR_WIDTH 20

    int list_h = rows - 2;
    if (list_h < 1) { refresh(); return; }

    int64_t max_du = (ui->dir->nchildren > 0 && ui->dir->children[0]->disk_usage > 0)
                     ? ui->dir->children[0]->disk_usage : 1;

    for (int i = 0; i < list_h; i++) {
        int idx = ui->off + i;
        if ((size_t)idx >= ui->dir->nchildren) break;

        const Entry *c = ui->dir->children[idx];
        char sz[24];
        fmt_size(c->disk_usage, sz, sizeof sz);

        char cnt[20];
        if (c->is_dir)
            snprintf(cnt, sizeof cnt, "%6" PRId64 " items", c->item_count);
        else
            snprintf(cnt, sizeof cnt, "            ");

        char bar[BAR_WIDTH + 3];
        int fill = (int)((double)c->disk_usage / (double)max_du * BAR_WIDTH + 0.5);
        if (fill > BAR_WIDTH) fill = BAR_WIDTH;
        bar[0] = '[';
        for (int b = 0; b < BAR_WIDTH; b++)
            bar[1 + b] = b < fill ? '#' : ' ';
        bar[BAR_WIDTH + 1] = ']';
        bar[BAR_WIDTH + 2] = '\0';

        if (idx == ui->sel) {
            attron(COLOR_PAIR(PAIR_SEL));
            mvhline(1 + i, 0, ' ', cols);   /* paint full row for highlight */
        }
        mvprintw(1 + i, 0, "  %s  %s  %s  ", cnt, sz, bar);
        addstr(c->name);
        if (c->is_dir) addch('/');
        if (idx == ui->sel) attroff(COLOR_PAIR(PAIR_SEL));
    }

    if (ui->dir->nchildren == 0)
        mvprintw(1, 0, "  (empty)");

    /* footer: keybindings left, count right */
    attron(COLOR_PAIR(PAIR_BAR));
    mvprintw(rows - 1, 0, "%-*s", cols,
             "  up/k  down/j  right|enter: open dir  left|backspace: up  d: trash  q: quit");
    if (ui->dir->nchildren > 0) {
        char info[64];
        snprintf(info, sizeof info, "%d / %zu  ", ui->sel + 1, ui->dir->nchildren);
        int info_col = cols - (int)strlen(info);
        if (info_col > 0)
            mvprintw(rows - 1, info_col, "%s", info);
    }
    attroff(COLOR_PAIR(PAIR_BAR));

    refresh();
}

/* Build the full filesystem path for a direct child of ui->dir. */
static int
entry_full_path(const UI *ui, const Entry *e, char *buf, size_t n)
{
    const Entry *chain[MAX_DEPTH + 2];
    int d = 0;
    size_t pos = 0;

    if (n == 0) return -1;
    buf[0] = '\0';

    for (const Entry *p = ui->dir; p && d < MAX_DEPTH + 1; p = p->parent)
        chain[d++] = p;

    for (int i = d - 1; i >= 0; i--) {
        const char *seg = (i == d - 1) ? ui->root_path : chain[i]->name;
        if (i < d - 1 && (pos == 0 || buf[pos - 1] != '/')) {
            if (path_append_char(buf, n, &pos, '/') != 0) return -1;
        }
        if (path_append(buf, n, &pos, seg) != 0) return -1;
    }
    if (pos > 0 && buf[pos - 1] != '/') {
        if (path_append_char(buf, n, &pos, '/') != 0) return -1;
    }
    if (path_append(buf, n, &pos, e->name) != 0) return -1;
    return 0;
}

static int
run_trash_command(const char *cmd, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        execvp(cmd, argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) return 0;
        if (code == 127) {
            errno = ENOENT;
            return -1;
        }
        errno = EIO;
        return -1;
    }

    errno = EIO;
    return -1;
}

/* Move the selected entry to the system trash via an installed helper command. */
static const char *
do_trash(UI *ui)
{
    if (ui->dir->nchildren == 0) return NULL;
    Entry *e = ui->dir->children[(size_t)ui->sel];

    char src[MAX_PATH];
    if (entry_full_path(ui, e, src, sizeof src) != 0)
        return "path too long";

    char *const gio_argv[] = { "gio", "trash", "--", src, NULL };
    if (run_trash_command("gio", gio_argv) == 0) {
        entry_detach(e);
        entry_free(e);
        return NULL;
    }
    if (errno != ENOENT) return "gio trash failed";

    char *const trash_put_argv[] = { "trash-put", "--", src, NULL };
    if (run_trash_command("trash-put", trash_put_argv) == 0) {
        entry_detach(e);
        entry_free(e);
        return NULL;
    }
    if (errno != ENOENT) return "trash-put failed";

    return "no trash command available";
}

static void
run_ui(Entry *root, const char *root_path)
{
    initscr();
    start_color();
    init_pair(PAIR_BAR,  15, 54);  /* bright white on deep purple  (#5f0087) */
    init_pair(PAIR_MAIN, 15, 17);  /* bright white on space black  (#00005f) */
    init_pair(PAIR_SEL,  15, 55);  /* bright white on nebula purple (#5f00af) */
    bkgd(COLOR_PAIR(PAIR_MAIN));
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    UI ui = { .dir = root, .sel = 0, .off = 0 };
    snprintf(ui.root_path, sizeof ui.root_path, "%s", root_path);
    if (strlen(root_path) >= sizeof ui.root_path) {
        endwin();
        fprintf(stderr, "error: path too long: %s\n", root_path);
        return;
    }
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

        case 'd':
            if (ui.dir->nchildren > 0) {
                Entry *sel = ui.dir->children[(size_t)ui.sel];
                int rows, cols;
                getmaxyx(stdscr, rows, cols);
                char prompt[MAX_NAME + 32];
                snprintf(prompt, sizeof prompt, "  Trash \"%s\"? (y/n)", sel->name);
                attron(COLOR_PAIR(PAIR_BAR));
                mvprintw(rows - 1, 0, "%-*.*s", cols, cols, prompt);
                attroff(COLOR_PAIR(PAIR_BAR));
                refresh();
                int ans = getch();
                if (ans == 'y' || ans == 'Y') {
                    const char *err = do_trash(&ui);
                    if (err) {
                        char errmsg[256];
                        snprintf(errmsg, sizeof errmsg, "  Error: %s (press any key)", err);
                        attron(COLOR_PAIR(PAIR_BAR));
                        mvprintw(rows - 1, 0, "%-*.*s", cols, cols, errmsg);
                        attroff(COLOR_PAIR(PAIR_BAR));
                        refresh();
                        getch();
                    }
                }
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
    if (!root->is_dir) {
        fprintf(stderr, "error: '%s' is not a directory\n", path);
        entry_free(root);
        return 1;
    }

    entry_sort_recursive(root);
    run_ui(root, real_path);
    entry_free(root);
    return 0;
}
