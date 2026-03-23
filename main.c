/*
 * activity/main.c
 * ---------------
 * X11 active window tracker (ghi interval active time ra SQLite).
 * Doc thiet ke: activity/DESIGN_X11.md
 */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Mặc định khi không truyền --out (triển khai: /opt/lancsmaster/data/). */
#define ACTIVITY_DEFAULT_DB "/opt/lancsmaster/data/activity.db"

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_x11_badwindow_seen = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int x11_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    if (!ev) return 0;

    // Window may disappear between focus event and metadata read.
    // Ignore to keep tracker alive.
    if (ev->error_code == BadWindow)
    {
        g_x11_badwindow_seen = 1;
        return 0;
    }

    char errtxt[128];
    XGetErrorText(dpy, ev->error_code, errtxt, sizeof(errtxt));
    fprintf(stderr, "X11 error ignored: %s (code=%u, req=%u)\n",
            errtxt, ev->error_code, ev->request_code);
    return 0;
}

static long long now_ts_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void local_date_str(char *buf, size_t buf_sz, long long ts_ms)
{
    if (!buf || buf_sz < 11) return;
    time_t sec = (time_t)(ts_ms / 1000LL);
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        buf[0] = '\0';
        return;
    }
    strftime(buf, buf_sz, "%Y-%m-%d", &tmv);
}

static int build_archive_path(const char *db_path, const char *date_tag,
                              char *dst, size_t dst_sz)
{
    const char *slash = strrchr(db_path, '/');
    const char *base = slash ? slash + 1 : db_path;
    char dir[PATH_MAX];
    if (slash)
    {
        size_t l = (size_t)(slash - db_path);
        if (l >= sizeof(dir)) return -1;
        memcpy(dir, db_path, l);
        dir[l] = '\0';
    }
    else
    {
        dir[0] = '\0';
    }

    char stem[256];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    size_t slen = strlen(stem);
    if (slen >= 4 && stem[slen - 3] == '.' &&
        (stem[slen - 2] == 'd' || stem[slen - 2] == 'D') &&
        (stem[slen - 1] == 'b' || stem[slen - 1] == 'B'))
    {
        stem[slen - 3] = '\0';
    }

    if (dir[0])
    {
        if (snprintf(dst, dst_sz, "%s/%s_%s.db", dir, stem, date_tag) >= (int)dst_sz)
            return -1;
    }
    else
    {
        if (snprintf(dst, dst_sz, "%s_%s.db", stem, date_tag) >= (int)dst_sz)
            return -1;
    }
    return 0;
}

static int init_sqlite(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS window_sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "window_title TEXT NOT NULL,"
        "process_name TEXT NOT NULL,"
        "process_id INTEGER NOT NULL,"
        "started_at_utc INTEGER NOT NULL,"
        "last_seen_at_utc INTEGER NOT NULL,"
        "ended_at_utc INTEGER,"
        "duration_seconds INTEGER NOT NULL DEFAULT 0,"
        "is_open INTEGER NOT NULL DEFAULT 1"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite schema: %s\n", err ? err : "error");
        sqlite3_free(err);
        return -1;
    }
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_window_sessions_open ON window_sessions(is_open, last_seen_at_utc);",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    return 0;
}

static unsigned long get_active_window(Display *dpy, Atom net_active_window, Window root)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = NULL;

    unsigned long active = 0;
    int rc = XGetWindowProperty(dpy, root, net_active_window, 0, 1, False,
                                XA_WINDOW, &actual_type, &actual_format, &nitems,
                                &bytes_after, &prop);
    (void)bytes_after;
    if (rc == Success && prop && nitems >= 1 && actual_format == 32)
    {
        active = (unsigned long)*(Window *)prop;
    }

    if (prop) XFree(prop);
    return active;
}

static unsigned long get_window_pid(Display *dpy, Atom net_wm_pid, Window w)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = NULL;

    unsigned long pid = 0;
    int rc = XGetWindowProperty(dpy, w, net_wm_pid, 0, 1, False,
                                XA_CARDINAL, &actual_type, &actual_format, &nitems,
                                &bytes_after, &prop);
    (void)bytes_after;
    if (rc == Success && prop && nitems >= 1 && actual_format == 32)
    {
        pid = (unsigned long)*(unsigned long *)prop;
    }

    if (prop) XFree(prop);
    return pid;
}

static void get_app_title(Display *dpy, Window w, Atom net_wm_name, char *out_class, size_t out_class_sz,
                            char *out_title, size_t out_title_sz)
{
    // app_class via WM_CLASS
    out_class[0] = '\0';
    out_title[0] = '\0';

    XClassHint hint;
    if (XGetClassHint(dpy, w, &hint))
    {
        // hint.res_class is often "org.gnome.Terminal" or similar.
        // Use it as app_class. fallback to instance if class missing.
        if (hint.res_class && hint.res_class[0])
        {
            strncpy(out_class, hint.res_class, out_class_sz - 1);
            out_class[out_class_sz - 1] = '\0';
        }
        else if (hint.res_name && hint.res_name[0])
        {
            strncpy(out_class, hint.res_name, out_class_sz - 1);
            out_class[out_class_sz - 1] = '\0';
        }
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
    }

    // title via _NET_WM_NAME (prefer UTF-8), fallback WM_NAME
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = NULL;

    if (net_wm_name != None)
    {
        int rc = XGetWindowProperty(dpy, w, net_wm_name, 0, 4096, False,
                                    AnyPropertyType, &actual_type, &actual_format, &nitems,
                                    &bytes_after, &prop);
        (void)bytes_after;
        if (rc == Success && prop && nitems > 0)
        {
            // For _NET_WM_NAME, actual_format is typically 8.
            // Copy up to out_title_sz-1 and truncate.
            size_t max_copy = out_title_sz - 1;
            if (actual_format == 8)
            {
                size_t copy_len = (size_t)nitems;
                if (copy_len > max_copy) copy_len = max_copy;
                memcpy(out_title, prop, copy_len);
                out_title[copy_len] = '\0';
            }
            else
            {
                // If unexpected encoding, fallback to string fetch below.
            }
        }
    }

    if (prop) XFree(prop);

    if (out_title[0] == '\0')
    {
        char *name = NULL;
        if (XFetchName(dpy, w, &name) && name)
        {
            strncpy(out_title, name, out_title_sz - 1);
            out_title[out_title_sz - 1] = '\0';
            XFree(name);
        }
    }

    // Normalize empty class/title
    if (out_class[0] == '\0')
        strncpy(out_class, "unknown", out_class_sz - 1), out_class[out_class_sz - 1] = '\0';
    if (out_title[0] == '\0')
        strncpy(out_title, "unknown", out_title_sz - 1), out_title[out_title_sz - 1] = '\0';

    (void)utf8;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--out FILE.db]\n"
            "Track X11 active window/tab by title changes (records start/end in SQLite).\n"
            "Default DB: " ACTIVITY_DEFAULT_DB "\n"
            "Daily rotation: at local midnight, FILE.db is renamed to STEM_YYYY-MM-DD.db\n"
            "and a new FILE.db is created (same window continues with a fresh interval).\n",
            argv0);
}

static int close_stale_open_sessions(sqlite3 *db, long long now_ms)
{
    static const char sql[] =
        "UPDATE window_sessions "
        "SET is_open=0, "
        "    ended_at_utc=?, "
        "    last_seen_at_utc=?, "
        "    duration_seconds=CASE WHEN ? >= started_at_utc THEN ? - started_at_utc ELSE 0 END "
        "WHERE is_open=1;";
    sqlite3_stmt *st = NULL;
    long long now_s = now_ms / 1000LL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite prepare close stale: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now_s);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)now_s);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now_s);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now_s);
    if (sqlite3_step(st) != SQLITE_DONE)
    {
        fprintf(stderr, "sqlite close stale: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

static int open_session(sqlite3_stmt *ins,
                        sqlite3 *db,
                        const char *title,
                        const char *process_name,
                        unsigned long process_id,
                        long long now_ms,
                        long long *session_id_out)
{
    if (!ins || !db) return -1;
    const char *tt = title && title[0] ? title : "unknown";
    const char *pn = process_name && process_name[0] ? process_name : "unknown";
    long long now_s = now_ms / 1000LL;

    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    sqlite3_bind_text(ins, 1, tt, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, pn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 3, (sqlite3_int64)process_id);
    sqlite3_bind_int64(ins, 4, (sqlite3_int64)now_s);
    sqlite3_bind_int64(ins, 5, (sqlite3_int64)now_s);
    sqlite3_bind_int64(ins, 6, (sqlite3_int64)0);

    if (sqlite3_step(ins) != SQLITE_DONE)
    {
        fprintf(stderr, "sqlite insert session: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    if (session_id_out)
        *session_id_out = (long long)sqlite3_last_insert_rowid(db);
    return 0;
}

static int update_session_realtime(sqlite3_stmt *upd, sqlite3 *db, long long session_id, long long now_ms)
{
    if (!upd || !db || session_id <= 0) return 0;
    long long now_s = now_ms / 1000LL;
    sqlite3_reset(upd);
    sqlite3_clear_bindings(upd);
    sqlite3_bind_int64(upd, 1, (sqlite3_int64)now_s);
    sqlite3_bind_int64(upd, 2, (sqlite3_int64)now_s);
    sqlite3_bind_int64(upd, 3, (sqlite3_int64)now_s);
    sqlite3_bind_int64(upd, 4, (sqlite3_int64)session_id);
    if (sqlite3_step(upd) != SQLITE_DONE)
    {
        fprintf(stderr, "sqlite update session: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int close_session(sqlite3_stmt *cls, sqlite3 *db, long long session_id, long long now_ms)
{
    if (!cls || !db || session_id <= 0) return 0;
    long long now_s = now_ms / 1000LL;
    sqlite3_reset(cls);
    sqlite3_clear_bindings(cls);
    sqlite3_bind_int64(cls, 1, (sqlite3_int64)now_s);
    sqlite3_bind_int64(cls, 2, (sqlite3_int64)now_s);
    sqlite3_bind_int64(cls, 3, (sqlite3_int64)now_s);
    sqlite3_bind_int64(cls, 4, (sqlite3_int64)now_s);
    sqlite3_bind_int64(cls, 5, (sqlite3_int64)session_id);
    if (sqlite3_step(cls) != SQLITE_DONE)
    {
        fprintf(stderr, "sqlite close session: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int prepare_session_statements(sqlite3 *db,
                                      sqlite3_stmt **ins,
                                      sqlite3_stmt **upd,
                                      sqlite3_stmt **cls,
                                      const char *ins_sql,
                                      const char *upd_sql,
                                      const char *cls_sql)
{
    if (sqlite3_prepare_v2(db, ins_sql, -1, ins, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite prepare insert session: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_prepare_v2(db, upd_sql, -1, upd, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite prepare update session: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(*ins);
        *ins = NULL;
        return -1;
    }
    if (sqlite3_prepare_v2(db, cls_sql, -1, cls, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite prepare close session: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(*ins);
        sqlite3_finalize(*upd);
        *ins = NULL;
        *upd = NULL;
        return -1;
    }
    return 0;
}

static int rotate_db_at_day_change(
    char db_path[PATH_MAX],
    char active_day[16],
    sqlite3 **db,
    sqlite3_stmt **ins,
    sqlite3_stmt **upd,
    sqlite3_stmt **cls,
    const char *ins_sql,
    const char *upd_sql,
    const char *cls_sql,
    unsigned long *current_window,
    long long *current_start_ms,
    char *current_app,
    char *current_title,
    unsigned long *current_pid,
    long long *current_session_id,
    long long *next_flush_ms,
    long long flush_interval_ms)
{
    long long t_now = now_ts_ms();
    char today[16];
    local_date_str(today, sizeof(today), t_now);
    if (strcmp(today, active_day) == 0)
        return 0;

    char archive_path[PATH_MAX];
    if (build_archive_path(db_path, active_day, archive_path, sizeof(archive_path)) != 0)
    {
        fprintf(stderr, "archive path too long\n");
        return -1;
    }

    if (*current_window != 0 && *current_session_id > 0)
        close_session(*cls, *db, *current_session_id, t_now);

    sqlite3_exec(*db, "PRAGMA wal_checkpoint(FULL);", NULL, NULL, NULL);
    sqlite3_finalize(*ins);
    sqlite3_finalize(*upd);
    sqlite3_finalize(*cls);
    *ins = NULL;
    *upd = NULL;
    *cls = NULL;
    sqlite3_close(*db);
    *db = NULL;

    if (rename(db_path, archive_path) != 0)
    {
        fprintf(stderr, "rename %s -> %s: %s — reopening same file; day stamp advanced.\n",
                db_path, archive_path, strerror(errno));
        if (sqlite3_open(db_path, db) != SQLITE_OK || !*db)
        {
            fprintf(stderr, "Cannot reopen database %s after failed rename\n", db_path);
            if (*db) sqlite3_close(*db);
            *db = NULL;
            return -1;
        }
        if (init_sqlite(*db) != 0)
        {
            sqlite3_close(*db);
            *db = NULL;
            return -1;
        }
        if (prepare_session_statements(*db, ins, upd, cls, ins_sql, upd_sql, cls_sql) != 0)
        {
            sqlite3_close(*db);
            *db = NULL;
            return -1;
        }
        snprintf(active_day, 16, "%s", today);
        if (*current_window != 0)
        {
            *current_start_ms = t_now;
            if (open_session(*ins, *db, current_title, current_app, *current_pid, t_now, current_session_id) != 0)
                return -1;
            *next_flush_ms = t_now + flush_interval_ms;
        }
        return 0;
    }

    if (sqlite3_open(db_path, db) != SQLITE_OK || !*db)
    {
        fprintf(stderr, "Cannot reopen database %s after rotate\n", db_path);
        if (*db) sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    if (init_sqlite(*db) != 0)
    {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    if (prepare_session_statements(*db, ins, upd, cls, ins_sql, upd_sql, cls_sql) != 0)
    {
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }

    snprintf(active_day, 16, "%s", today);

    if (*current_window != 0)
    {
        *current_start_ms = t_now;
        if (open_session(*ins, *db, current_title, current_app, *current_pid, t_now, current_session_id) != 0)
            return -1;
        *next_flush_ms = t_now + flush_interval_ms;
    }
    else
    {
        *current_start_ms = 0;
        *current_session_id = 0;
        *next_flush_ms = 0;
    }

    fprintf(stderr, "DB rotated: %s -> %s (new day %s)\n",
            db_path, archive_path, today);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_arg = ACTIVITY_DEFAULT_DB;
    const long long flush_interval_ms = 5000LL; // flush current tab every ~5s

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
        {
            out_arg = argv[++i];
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    char db_path[PATH_MAX];
    if (strlen(out_arg) >= sizeof(db_path))
    {
        fprintf(stderr, "--out path too long\n");
        return 2;
    }
    strncpy(db_path, out_arg, sizeof(db_path) - 1);
    db_path[sizeof(db_path) - 1] = '\0';

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
    {
        fprintf(stderr, "XOpenDisplay failed. Is DISPLAY set?\n");
        return 1;
    }
    XSetErrorHandler(x11_error_handler);

    Window root = DefaultRootWindow(dpy);
    Atom net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom wm_name_atom = XInternAtom(dpy, "WM_NAME", False);
    Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);

    if (net_active_window == None)
    {
        fprintf(stderr, "Cannot find atom _NET_ACTIVE_WINDOW.\n");
        XCloseDisplay(dpy);
        return 1;
    }

    // Subscribe PropertyNotify on root for active-window changes.
    XSelectInput(dpy, root, PropertyChangeMask);

    static const char ins_sql[] =
        "INSERT INTO window_sessions "
        "(window_title,process_name,process_id,started_at_utc,last_seen_at_utc,duration_seconds,is_open) "
        "VALUES (?,?,?,?,?,?,1);";
    static const char upd_sql[] =
        "UPDATE window_sessions "
        "SET last_seen_at_utc=?, "
        "    duration_seconds=CASE WHEN ? >= started_at_utc THEN ? - started_at_utc ELSE 0 END "
        "WHERE id=? AND is_open=1;";
    static const char cls_sql[] =
        "UPDATE window_sessions "
        "SET last_seen_at_utc=?, ended_at_utc=?, "
        "    duration_seconds=CASE WHEN ? >= started_at_utc THEN ? - started_at_utc ELSE 0 END, "
        "    is_open=0 "
        "WHERE id=? AND is_open=1;";

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK || !db)
    {
        fprintf(stderr, "Cannot open database: %s\n",
                db ? sqlite3_errmsg(db) : db_path);
        if (db) sqlite3_close(db);
        XCloseDisplay(dpy);
        return 1;
    }
    if (init_sqlite(db) != 0)
    {
        sqlite3_close(db);
        XCloseDisplay(dpy);
        return 1;
    }

    sqlite3_stmt *ins = NULL;
    sqlite3_stmt *upd = NULL;
    sqlite3_stmt *cls = NULL;
    if (prepare_session_statements(db, &ins, &upd, &cls, ins_sql, upd_sql, cls_sql) != 0)
    {
        sqlite3_close(db);
        XCloseDisplay(dpy);
        return 1;
    }
    if (close_stale_open_sessions(db, now_ts_ms()) != 0)
    {
        sqlite3_finalize(ins);
        sqlite3_finalize(upd);
        sqlite3_finalize(cls);
        sqlite3_close(db);
        XCloseDisplay(dpy);
        return 1;
    }

    char active_day[16];
    local_date_str(active_day, sizeof(active_day), now_ts_ms());

    unsigned long current_window = get_active_window(dpy, net_active_window, root);
    long long current_start_ms = 0;
    char current_app[128] = {0};
    char current_title[512] = {0};
    unsigned long current_pid = 0;
    long long current_session_id = 0;
    long long next_flush_ms = 0;

    if (current_window != 0)
    {
        current_start_ms = now_ts_ms();
        get_app_title(dpy, (Window)current_window, net_wm_name,
                       current_app, sizeof(current_app),
                       current_title, sizeof(current_title));
        current_pid = get_window_pid(dpy, net_wm_pid, (Window)current_window);
        XSelectInput(dpy, (Window)current_window, PropertyChangeMask);
        if (open_session(ins, db, current_title, current_app, current_pid, current_start_ms, &current_session_id) != 0)
        {
            sqlite3_finalize(ins);
            sqlite3_finalize(upd);
            sqlite3_finalize(cls);
            sqlite3_close(db);
            XCloseDisplay(dpy);
            return 1;
        }
        next_flush_ms = current_start_ms + flush_interval_ms;
    }

    fprintf(stderr, "Tracking active window/tab (title changes). Ctrl+C to stop.\n");

    int xfd = (int)ConnectionNumber(dpy);
    XEvent ev;

    while (!g_stop)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(xfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0)
        {
            if (errno == EINTR) continue;
            break;
        }

        if (rotate_db_at_day_change(db_path, active_day, &db, &ins, &upd, &cls, ins_sql, upd_sql, cls_sql,
                                    &current_window, &current_start_ms,
                                    current_app, current_title,
                                    &current_pid, &current_session_id, &next_flush_ms,
                                    flush_interval_ms) != 0)
        {
            break;
        }

        // Timeout: no X events. If we still have an open tab, flush it every
        // ~30 seconds so the DB is updated even without title/window changes.
        if (!(sel > 0 && FD_ISSET(xfd, &rfds)))
        {
            if (current_window != 0 && current_start_ms > 0 && next_flush_ms > 0)
            {
                long long t_ms = now_ts_ms();
                if (t_ms >= next_flush_ms)
                {
                    update_session_realtime(upd, db, current_session_id, t_ms);
                    next_flush_ms = t_ms + flush_interval_ms;
                }
            }
            continue;
        }

        while (XPending(dpy) > 0)
        {
            XNextEvent(dpy, &ev);
            if (ev.type != PropertyNotify) continue;

            // 1) Active-window changed (focus switch)
            if (ev.xany.window == root && ev.xproperty.atom == net_active_window)
            {
                unsigned long new_window = get_active_window(dpy, net_active_window, root);
                if (new_window == current_window)
                    continue;

                long long t_ms = now_ts_ms();

                if (current_window != 0 && current_session_id > 0)
                    close_session(cls, db, current_session_id, t_ms);

                current_window = new_window;
                current_start_ms = 0;
                current_pid = 0;
                current_session_id = 0;
                current_app[0] = '\0';
                current_title[0] = '\0';
                next_flush_ms = 0;

                if (current_window != 0)
                {
                    current_start_ms = t_ms;
                    get_app_title(dpy, (Window)current_window, net_wm_name,
                                   current_app, sizeof(current_app),
                                   current_title, sizeof(current_title));
                    current_pid = get_window_pid(dpy, net_wm_pid, (Window)current_window);
                    XSelectInput(dpy, (Window)current_window, PropertyChangeMask);
                    if (open_session(ins, db, current_title, current_app, current_pid, current_start_ms, &current_session_id) != 0)
                    {
                        g_stop = 1;
                        break;
                    }
                    next_flush_ms = current_start_ms + flush_interval_ms;
                }
                continue;
            }

            // 2) Title changed inside the active window => treat as tab switch
            if (current_window != 0 && ev.xany.window == (Window)current_window)
            {
                if (ev.xproperty.atom != net_wm_name && ev.xproperty.atom != wm_name_atom)
                    continue;

                long long t_ms = now_ts_ms();
                if (current_start_ms <= 0 || t_ms <= current_start_ms)
                    continue;

                char new_app[128] = {0};
                char new_title[512] = {0};
                unsigned long new_pid = 0;

                get_app_title(dpy, (Window)current_window, net_wm_name,
                               new_app, sizeof(new_app),
                               new_title, sizeof(new_title));
                new_pid = get_window_pid(dpy, net_wm_pid, (Window)current_window);

                if (strcmp(new_title, current_title) == 0 && strcmp(new_app, current_app) == 0 && new_pid == current_pid)
                {
                    update_session_realtime(upd, db, current_session_id, t_ms);
                    continue;
                }

                close_session(cls, db, current_session_id, t_ms);

                current_app[0] = '\0';
                current_title[0] = '\0';
                strncpy(current_app, new_app, sizeof(current_app) - 1);
                current_app[sizeof(current_app) - 1] = '\0';
                strncpy(current_title, new_title, sizeof(current_title) - 1);
                current_title[sizeof(current_title) - 1] = '\0';
                current_pid = new_pid;
                current_start_ms = t_ms;
                if (open_session(ins, db, current_title, current_app, current_pid, current_start_ms, &current_session_id) != 0)
                {
                    g_stop = 1;
                    break;
                }
                next_flush_ms = current_start_ms + flush_interval_ms;
            }
        }
    }

    // Flush last interval on exit.
    long long end_ms = now_ts_ms();
    if (current_window != 0 && current_session_id > 0 && end_ms > current_start_ms)
        close_session(cls, db, current_session_id, end_ms);

    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    sqlite3_finalize(cls);
    sqlite3_close(db);
    XCloseDisplay(dpy);
    return 0;
}
