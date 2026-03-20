/*
 * activity/main.c
 * ---------------
 * X11 active window tracker (ghi interval active time ra CSV).
 * Doc thiet ke: activity/DESIGN_X11.md
 */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <signal.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static long long now_ts_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void format_ts_ms(char *dst, size_t dst_sz, long long ts_ms)
{
    if (!dst || dst_sz == 0) return;

    time_t sec = (time_t)(ts_ms / 1000LL);
    int ms = (int)(ts_ms % 1000LL);
    if (ms < 0) ms = -ms;

    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        // Fallback: print raw ms if localtime fails.
        snprintf(dst, dst_sz, "%lldms", ts_ms);
        return;
    }

    char buf[32];
    // Example: 2026-03-20T14:12:34
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv) == 0)
    {
        snprintf(dst, dst_sz, "%lldms", ts_ms);
        return;
    }

    // Append milliseconds: ...SS.mmm
    snprintf(dst, dst_sz, "%s.%03d", buf, ms);
}

static void csv_escape(char *dst, size_t dst_sz, const char *src)
{
    // Escape CSV field:
    // - Double quotes: " -> ""
    // - If contains special char, wrap in quotes
    size_t n = 0;
    int needs_quotes = 0;
    if (!src) src = "";

    for (const char *p = src; *p; p++)
    {
        char c = *p;
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
            needs_quotes = 1;
    }

    if (needs_quotes)
    {
        if (dst_sz > 0) dst[n++] = '"';
    }

    for (const char *p = src; *p; p++)
    {
        char c = *p;
        if (n + 2 >= dst_sz) break;
        if (c == '"')
        {
            dst[n++] = '"';
            dst[n++] = '"';
        }
        else if (c == '\n' || c == '\r')
        {
            dst[n++] = ' ';
        }
        else
        {
            dst[n++] = c;
        }
    }

    if (needs_quotes)
    {
        if (n + 1 < dst_sz) dst[n++] = '"';
    }

    if (n < dst_sz) dst[n] = '\0';
    else if (dst_sz > 0) dst[dst_sz - 1] = '\0';
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
            "Usage: %s --out FILE\n"
            "Track X11 active window/tab by title changes (records start/end).\n",
            argv0);
}

static void write_record(FILE *f,
                          long long start_ms,
                          long long end_ms,
                          unsigned long window_id,
                          const char *app_class,
                          unsigned long pid,
                          const char *title)
{
    if (!f) return;
    if (end_ms <= start_ms) return;

    char esc_app[256];
    char esc_title[1024];
    char window_id_buf[32];
    char start_ts_str[64];
    char end_ts_str[64];

    csv_escape(esc_app, sizeof(esc_app), app_class ? app_class : "unknown");
    csv_escape(esc_title, sizeof(esc_title), title ? title : "unknown");
    snprintf(window_id_buf, sizeof(window_id_buf), "0x%lx", window_id);
    format_ts_ms(start_ts_str, sizeof(start_ts_str), start_ms);
    format_ts_ms(end_ts_str, sizeof(end_ts_str), end_ms);

    fprintf(f,
            "%s,%s,%s,%s,%lu,%s\n",
            start_ts_str, end_ts_str,
            window_id_buf, esc_app, pid, esc_title);
    fflush(f);
}

int main(int argc, char **argv)
{
    const char *out_path = "activity_usage.csv";
    const long long flush_interval_ms = 30000LL; // flush current tab every ~30s

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
    {
        fprintf(stderr, "XOpenDisplay failed. Is DISPLAY set?\n");
        return 1;
    }

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

    FILE *f = fopen(out_path, "a+");
    if (!f)
    {
        fprintf(stderr, "Cannot open output: %s: %s\n", out_path, strerror(errno));
        XCloseDisplay(dpy);
        return 1;
    }

    // If file empty, write header.
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz == 0)
    {
        fprintf(f, "start_ts,end_ts,window_id,app_class,pid,title\n");
        fflush(f);
    }

    unsigned long current_window = get_active_window(dpy, net_active_window, root);
    long long current_start_ms = 0;
    char current_app[128] = {0};
    char current_title[512] = {0};
    unsigned long current_pid = 0;
    long long next_flush_ms = 0;

    if (current_window != 0)
    {
        current_start_ms = now_ts_ms();
        get_app_title(dpy, (Window)current_window, net_wm_name,
                       current_app, sizeof(current_app),
                       current_title, sizeof(current_title));
        current_pid = get_window_pid(dpy, net_wm_pid, (Window)current_window);
        XSelectInput(dpy, (Window)current_window, PropertyChangeMask);
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

        // Timeout: no X events. If we still have an open tab, flush it every
        // ~30 seconds so the CSV is updated even without title/window changes.
        if (!(sel > 0 && FD_ISSET(xfd, &rfds)))
        {
            if (current_window != 0 && current_start_ms > 0 && next_flush_ms > 0)
            {
                long long t_ms = now_ts_ms();
                if (t_ms >= next_flush_ms)
                {
                    write_record(f,
                                 current_start_ms, t_ms,
                                 current_window,
                                 current_app,
                                 current_pid,
                                 current_title);
                    // Keep the original current_start_ms until tab/window
                    // actually changes. This way each heartbeat is a snapshot:
                    // (current_start, now).
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

                if (current_window != 0 && current_start_ms > 0 && t_ms > current_start_ms)
                {
                    write_record(f,
                                 current_start_ms, t_ms,
                                 current_window,
                                 current_app,
                                 current_pid,
                                 current_title);
                }

                current_window = new_window;
                current_start_ms = 0;
                current_pid = 0;
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

                if (strcmp(new_title, current_title) == 0)
                    continue;

                // Close previous tab interval
                write_record(f,
                             current_start_ms, t_ms,
                             current_window,
                             current_app,
                             current_pid,
                             current_title);

                // Open new tab interval
                current_app[0] = '\0';
                current_title[0] = '\0';
                strncpy(current_app, new_app, sizeof(current_app) - 1);
                current_app[sizeof(current_app) - 1] = '\0';
                strncpy(current_title, new_title, sizeof(current_title) - 1);
                current_title[sizeof(current_title) - 1] = '\0';
                current_pid = new_pid;
                current_start_ms = t_ms;
                next_flush_ms = current_start_ms + flush_interval_ms;
            }
        }
    }

    // Flush last interval on exit.
    long long end_ms = now_ts_ms();
    if (current_window != 0 && current_start_ms > 0 && end_ms > current_start_ms)
    {
        write_record(f,
                     current_start_ms, end_ms,
                     current_window,
                     current_app,
                     current_pid,
                     current_title);
    }

    fclose(f);
    XCloseDisplay(dpy);
    return 0;
}
