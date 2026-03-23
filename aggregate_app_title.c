/*
 * aggregate_app_title.c
 * ----------------------
 * Aggregate activity sessions from SQLite (default) or legacy CSV.
 *
 * SQLite table `window_sessions`:
 *   id, window_title, process_name, process_id,
 *   started_at_utc, last_seen_at_utc, ended_at_utc, duration_seconds, is_open
 *
 * CSV schema (header may exist):
 *   start_ts,end_ts,window_id,app_class,pid,title
 *   start_ts/end_ts: ISO local time with milliseconds
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

/* Mặc định khi không truyền --db (triển khai: /opt/lancsmaster/data/). */
#define ACTIVITY_DEFAULT_DB "/opt/lancsmaster/data/activity.db"

typedef struct
{
    char app_class[256];
    char title[512];
    long long total_ms;
    long long covered_end_ms; // latest end timestamp included in total_ms
} Entry;

typedef struct
{
    long long start_ms;
    long long end_ms;
    char window_id[64];
    unsigned long pid;
    char app_class[256];
    char title[512];
} ActivityRec;

static void format_ts_iso_ms(char *dst, size_t dst_sz, long long ts_ms)
{
    if (!dst || dst_sz == 0) return;
    time_t sec = (time_t)(ts_ms / 1000LL);
    int ms = (int)(ts_ms % 1000LL);
    if (ms < 0) ms = -ms;
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        snprintf(dst, dst_sz, "%lldms", ts_ms);
        return;
    }
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv) == 0)
    {
        snprintf(dst, dst_sz, "%lldms", ts_ms);
        return;
    }
    snprintf(dst, dst_sz, "%s.%03d", buf, ms);
}

static int append_rec(ActivityRec **arr, size_t *len, size_t *cap, const ActivityRec *r)
{
    if (*len == *cap)
    {
        size_t nc = *cap ? *cap * 2 : 64;
        ActivityRec *p = realloc(*arr, nc * sizeof(*p));
        if (!p) return -1;
        *arr = p;
        *cap = nc;
    }
    (*arr)[*len] = *r;
    (*len)++;
    return 0;
}

static long long parse_iso_ms(const char *s, int *ok)
{
    if (ok) *ok = 0;
    if (!s || !s[0]) return 0;

    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0, ms = 0;
    int n = 0;

    // With milliseconds:
    n = sscanf(s, "%d-%d-%dT%d:%d:%d.%d", &Y, &M, &D, &h, &m, &sec, &ms);
    if (n == 7)
    {
        // ok
    }
    else
    {
        // Without milliseconds (fallback):
        ms = 0;
        n = sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec);
        if (n != 6) return 0;
    }

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = m;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1; // let libc determine DST

    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return 0;

    if (ok) *ok = 1;
    return (long long)t * 1000LL + (long long)ms;
}

static void format_ts_hms_ms(char *dst, size_t dst_sz, long long ts_ms)
{
    if (!dst || dst_sz == 0) return;
    time_t sec = (time_t)(ts_ms / 1000LL);
    int ms = (int)(ts_ms % 1000LL);
    if (ms < 0) ms = -ms;

    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        snprintf(dst, dst_sz, "%lldms", ts_ms);
        return;
    }

    // HH:MM:SS.mmm
    snprintf(dst, dst_sz, "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
}

static void format_duration_hms(char *dst, size_t dst_sz, long long duration_ms)
{
    if (!dst || dst_sz == 0) return;
    if (duration_ms < 0) duration_ms = -duration_ms;

    long long total_sec = duration_ms / 1000LL;
    int ms = (int)(duration_ms % 1000LL);

    long long hh = total_sec / 3600LL;
    long long mm = (total_sec % 3600LL) / 60LL;
    long long ss = total_sec % 60LL;

    // HH:MM:SS
    (void)ms;
    snprintf(dst, dst_sz, "%lld:%02lld:%02lld", hh, mm, ss);
}

static void format_duration_hms_ms(char *dst, size_t dst_sz, long long duration_ms)
{
    if (!dst || dst_sz == 0) return;
    if (duration_ms < 0) duration_ms = -duration_ms;

    long long total_sec = duration_ms / 1000LL;
    int ms = (int)(duration_ms % 1000LL);

    long long hh = total_sec / 3600LL;
    long long mm = (total_sec % 3600LL) / 60LL;
    long long ss = total_sec % 60LL;

    snprintf(dst, dst_sz, "%lld:%02lld:%02lld.%03d", hh, mm, ss, ms);
}

static int csv_split_6(const char *line, char out[6][2048])
{
    // Basic CSV parser:
    // - Fields separated by commas not inside quotes
    // - Quotes wrap fields; inside quotes, "" escapes a single "
    if (!line) return 0;

    int field = 0;
    int in_quotes = 0;
    size_t pos = 0;
    for (int i = 0; i < 6; i++)
        out[i][0] = '\0';

    for (const char *p = line; *p; p++)
    {
        char c = *p;

        if (c == '\r' || c == '\n')
            break;

        if (in_quotes)
        {
            if (c == '"')
            {
                // Escaped quote?
                if (p[1] == '"')
                {
                    if (pos + 1 < 2048) out[field][pos++] = '"';
                    p++; // skip second "
                    continue;
                }
                in_quotes = 0;
                continue;
            }
            if (pos + 1 < 2048) out[field][pos++] = c;
            continue;
        }
        else
        {
            if (c == '"')
            {
                in_quotes = 1;
                continue;
            }
            if (c == ',')
            {
                out[field][pos] = '\0';
                field++;
                if (field > 5) return 0;
                pos = 0;
                continue;
            }

            if (pos + 1 < 2048) out[field][pos++] = c;
            continue;
        }
    }

    out[field][pos] = '\0';

    return 1;
}

static int load_records_csv(const char *path, ActivityRec **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    size_t cap = 0;

    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "Cannot open CSV: %s\n", path);
        return -1;
    }

    char line[16384];
    int is_header_checked = 0;

    while (fgets(line, sizeof(line), f))
    {
        if (!is_header_checked)
        {
            is_header_checked = 1;
            if (strncmp(line, "start_ts", 9) == 0)
                continue;
        }

        char fields6[6][2048];
        if (!csv_split_6(line, fields6))
            continue;

        int ok1 = 0, ok2 = 0;
        long long start_ms = parse_iso_ms(fields6[0], &ok1);
        long long end_ms = parse_iso_ms(fields6[1], &ok2);
        if (!ok1 || !ok2 || end_ms <= start_ms)
            continue;

        ActivityRec r;
        memset(&r, 0, sizeof(r));
        r.start_ms = start_ms;
        r.end_ms = end_ms;
        strncpy(r.window_id, fields6[2], sizeof(r.window_id) - 1);
        strncpy(r.app_class, fields6[3], sizeof(r.app_class) - 1);
        r.pid = (unsigned long)strtoul(fields6[4], NULL, 10);
        strncpy(r.title, fields6[5], sizeof(r.title) - 1);

        if (append_rec(out, n_out, &cap, &r) != 0)
        {
            fclose(f);
            free(*out);
            *out = NULL;
            *n_out = 0;
            fprintf(stderr, "Out of memory loading CSV\n");
            return -1;
        }
    }

    fclose(f);
    return 0;
}

static int load_records_db(const char *path, ActivityRec **out, size_t *n_out)
{
    *out = NULL;
    *n_out = 0;
    size_t cap = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK || !db)
    {
        fprintf(stderr, "Cannot open database: %s\n",
                db ? sqlite3_errmsg(db) : path);
        if (db) sqlite3_close(db);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id, started_at_utc, "
        "       COALESCE(ended_at_utc, last_seen_at_utc) AS end_utc, "
        "       process_name, process_id, window_title "
        "FROM window_sessions "
        "ORDER BY started_at_utc ASC, id ASC;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "sqlite prepare: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        ActivityRec r;
        memset(&r, 0, sizeof(r));
        long long row_id = (long long)sqlite3_column_int64(st, 0);
        long long started_utc = (long long)sqlite3_column_int64(st, 1);
        long long end_utc = (long long)sqlite3_column_int64(st, 2);
        const char *ac = (const char *)sqlite3_column_text(st, 3);
        r.pid = (unsigned long)sqlite3_column_int64(st, 4);
        const char *tt = (const char *)sqlite3_column_text(st, 5);

        r.start_ms = started_utc * 1000LL;
        r.end_ms = end_utc * 1000LL;
        snprintf(r.window_id, sizeof(r.window_id), "sess:%lld", row_id);
        if (ac)
            strncpy(r.app_class, ac, sizeof(r.app_class) - 1);
        if (tt)
            strncpy(r.title, tt, sizeof(r.title) - 1);

        if (r.end_ms <= r.start_ms)
            continue;

        if (append_rec(out, n_out, &cap, &r) != 0)
        {
            sqlite3_finalize(st);
            sqlite3_close(db);
            free(*out);
            *out = NULL;
            *n_out = 0;
            fprintf(stderr, "Out of memory loading database\n");
            return -1;
        }
    }

    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

static Entry *find_entry(Entry *arr, size_t len, const char *app_class, const char *title)
{
    for (size_t i = 0; i < len; i++)
    {
        if (strcmp(arr[i].app_class, app_class) == 0 && strcmp(arr[i].title, title) == 0)
            return &arr[i];
    }
    return NULL;
}

static int cmp_total_desc(const void *a, const void *b)
{
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    if (ea->total_ms < eb->total_ms) return 1;
    if (ea->total_ms > eb->total_ms) return -1;
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--db FILE.db | --in FILE.csv] [--mode history|totals] [--top N] [--limit N]\n"
            "\n"
            "Input (default: SQLite activity.db):\n"
            "  --db PATH   Read sessions from SQLite table `window_sessions`.\n"
            "  --in PATH   Read legacy CSV (same columns as before).\n"
            "\n"
            "Modes:\n"
            "  history  Print merged history (removes heartbeat-overlap duplicates) by (window_id,pid,app_class,title).\n"
            "  history_raw  Print records (may contain overlapping heartbeat snapshots).\n"
            "  totals    Compute total active time by (app_class,title) using start/end union.\n"
            "\n"
            "Output formats (optional):\n"
            "  --timefmt iso/hms  start/end format (default: hms)\n"
            "  --durfmt sec/hms   duration format (default: hms)\n"
            "\n"
            "Notes:\n"
            "- CSV schema: start_ts,end_ts,window_id,app_class,pid,title\n",
            argv0);
}

int main(int argc, char **argv)
{
    int src_use_csv = 0;
    const char *input_path = ACTIVITY_DEFAULT_DB;
    int mode_history = 0;      // 1 => merged history, 2 => raw history
    size_t topn = 10;
    size_t limit = 0;
    // Default output formats:
    // - history: HH:MM:SS.mmm (time-only)
    // - totals:  HH:MM:SS.mmm duration
    int timefmt_hms = 1; // 0 => ISO, 1 => HH:MM:SS.mmm
    int durfmt_hms = 1;  // 0 => print seconds, 1 => print HH:MM:SS.mmm

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--in") == 0 && i + 1 < argc)
        {
            src_use_csv = 1;
            input_path = argv[++i];
        }
        else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc)
        {
            src_use_csv = 0;
            input_path = argv[++i];
        }
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
        {
            const char *m = argv[++i];
            if (strcmp(m, "history") == 0)
                mode_history = 1;
            else if (strcmp(m, "history_raw") == 0)
                mode_history = 2;
            else if (strcmp(m, "totals") == 0)
                mode_history = 0;
            else
            {
                usage(argv[0]);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--history") == 0)
        {
            mode_history = 1;
        }
        else if (strcmp(argv[i], "--totals") == 0)
        {
            mode_history = 0;
        }
        else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc)
        {
            topn = (size_t)strtoull(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
        {
            limit = (size_t)strtoull(argv[++i], NULL, 10);
        }
        else if (strcmp(argv[i], "--timefmt") == 0 && i + 1 < argc)
        {
            const char *tf = argv[++i];
            if (strcmp(tf, "hms") == 0)
                timefmt_hms = 1;
            else if (strcmp(tf, "iso") == 0)
                timefmt_hms = 0;
            else
            {
                usage(argv[0]);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--durfmt") == 0 && i + 1 < argc)
        {
            const char *df = argv[++i];
            if (strcmp(df, "hms") == 0)
                durfmt_hms = 1;
            else if (strcmp(df, "sec") == 0)
                durfmt_hms = 0;
            else
            {
                usage(argv[0]);
                return 2;
            }
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    ActivityRec *recs = NULL;
    size_t nrec = 0;
    if (src_use_csv)
    {
        if (load_records_csv(input_path, &recs, &nrec) != 0)
            return 1;
    }
    else
    {
        if (load_records_db(input_path, &recs, &nrec) != 0)
            return 1;
    }

    if (nrec == 0)
    {
        fprintf(stderr, "No records found in %s\n", input_path);
        free(recs);
        return 0;
    }

    if (mode_history == 2)
    {
        if (!timefmt_hms)
        {
            printf("start_ts,end_ts,window_id,app_class,pid,title\n");
            for (size_t i = 0; i < nrec; i++)
            {
                if (limit > 0 && i >= limit) break;
                ActivityRec *R = &recs[i];
                char start_iso[64];
                char end_iso[64];
                format_ts_iso_ms(start_iso, sizeof(start_iso), R->start_ms);
                format_ts_iso_ms(end_iso, sizeof(end_iso), R->end_ms);
                printf("%s,%s,%s,%s,%lu,%s\n",
                       start_iso, end_iso,
                       R->window_id, R->app_class, R->pid, R->title);
            }
            free(recs);
            return 0;
        }

        printf("start_ts,end_ts,window_id,app_class,pid,title\n");
        for (size_t i = 0; i < nrec; i++)
        {
            if (limit > 0 && i >= limit) break;
            ActivityRec *R = &recs[i];
            char start_s[64];
            char end_s[64];
            format_ts_hms_ms(start_s, sizeof(start_s), R->start_ms);
            format_ts_hms_ms(end_s, sizeof(end_s), R->end_ms);
            printf("%s,%s,%s,%s,%lu,%s\n",
                   start_s, end_s,
                   R->window_id, R->app_class, R->pid, R->title);
        }
        free(recs);
        return 0;
    }
    else if (mode_history == 1)
    {
            // Merge overlapping heartbeat snapshots for the same key:
            // (window_id, pid, app_class, title)
        typedef struct
        {
            char window_id[64];
            unsigned long pid;
            char app_class[256];
            char title[512];
            long long start_ms;
            long long end_ms;
            int has_interval;
        } State;

        typedef struct
        {
            char window_id[64];
            unsigned long pid;
            char app_class[256];
            char title[512];
            long long start_ms;
            long long end_ms;
        } Out;

        State *states = NULL;
        size_t states_len = 0;
        size_t states_cap = 0;
        Out *outs = NULL;
        size_t outs_len = 0;
        size_t outs_cap = 0;

        for (size_t ri = 0; ri < nrec; ri++)
        {
            long long start_ms = recs[ri].start_ms;
            long long end_ms = recs[ri].end_ms;
            if (end_ms <= start_ms) continue;

            const char *window_id = recs[ri].window_id;
            const char *app_class = recs[ri].app_class;
            unsigned long pid = recs[ri].pid;
            const char *title = recs[ri].title;
            if (!window_id || !app_class || !title ||
                !window_id[0] || !app_class[0] || !title[0])
                continue;

            // Find or create state for this key.
            State *s = NULL;
            for (size_t i = 0; i < states_len; i++)
            {
                if (states[i].pid == pid &&
                    strcmp(states[i].window_id, window_id) == 0 &&
                    strcmp(states[i].app_class, app_class) == 0 &&
                    strcmp(states[i].title, title) == 0)
                {
                    s = &states[i];
                    break;
                }
            }

            if (!s)
            {
                if (states_len == states_cap)
                {
                    size_t new_cap = states_cap == 0 ? 32 : states_cap * 2;
                    State *p = realloc(states, new_cap * sizeof(*p));
                    if (!p) break;
                    states = p;
                    states_cap = new_cap;
                }
                s = &states[states_len++];
                memset(s, 0, sizeof(*s));
                strncpy(s->window_id, window_id, sizeof(s->window_id) - 1);
                s->pid = pid;
                strncpy(s->app_class, app_class, sizeof(s->app_class) - 1);
                strncpy(s->title, title, sizeof(s->title) - 1);
                s->has_interval = 0;
            }

            if (!s->has_interval)
            {
                s->has_interval = 1;
                s->start_ms = start_ms;
                s->end_ms = end_ms;
            }
            else
            {
                // Merge overlapping / adjacent intervals.
                if (start_ms <= s->end_ms)
                {
                    if (end_ms > s->end_ms) s->end_ms = end_ms;
                }
                else
                {
                    // Disjoint: push finished interval, then start new one.
                    if (outs_len == outs_cap)
                    {
                        size_t new_cap = outs_cap == 0 ? 64 : outs_cap * 2;
                        Out *p = realloc(outs, new_cap * sizeof(*p));
                        if (!p) break;
                        outs = p;
                        outs_cap = new_cap;
                    }
                    Out *o = &outs[outs_len++];
                    memset(o, 0, sizeof(*o));
                    strncpy(o->window_id, s->window_id, sizeof(o->window_id) - 1);
                    o->pid = s->pid;
                    strncpy(o->app_class, s->app_class, sizeof(o->app_class) - 1);
                    strncpy(o->title, s->title, sizeof(o->title) - 1);
                    o->start_ms = s->start_ms;
                    o->end_ms = s->end_ms;

                    s->start_ms = start_ms;
                    s->end_ms = end_ms;
                }
            }
        }

        // Flush remaining intervals.
        for (size_t i = 0; i < states_len; i++)
        {
            if (!states[i].has_interval) continue;
            if (outs_len == outs_cap)
            {
                size_t new_cap = outs_cap == 0 ? 64 : outs_cap * 2;
                Out *p = realloc(outs, new_cap * sizeof(*p));
                if (!p) break;
                outs = p;
                outs_cap = new_cap;
            }
            Out *o = &outs[outs_len++];
            memset(o, 0, sizeof(*o));
            strncpy(o->window_id, states[i].window_id, sizeof(o->window_id) - 1);
            o->pid = states[i].pid;
            strncpy(o->app_class, states[i].app_class, sizeof(o->app_class) - 1);
            strncpy(o->title, states[i].title, sizeof(o->title) - 1);
            o->start_ms = states[i].start_ms;
            o->end_ms = states[i].end_ms;
        }

        // Sort outs by start_ms then end_ms (simple O(n^2) sort).
        for (size_t i = 0; i < outs_len; i++)
        {
            for (size_t j = i + 1; j < outs_len; j++)
            {
                int swap = 0;
                if (outs[j].start_ms < outs[i].start_ms) swap = 1;
                else if (outs[j].start_ms == outs[i].start_ms && outs[j].end_ms < outs[i].end_ms) swap = 1;
                if (swap)
                {
                    Out tmp = outs[i];
                    outs[i] = outs[j];
                    outs[j] = tmp;
                }
            }
        }

        printf("start_ts,end_ts,app_class,pid,title\n");
        for (size_t i = 0; i < outs_len; i++)
        {
            if (limit > 0 && i >= limit) break;

            char start_s[64];
            char end_s[64];
            if (timefmt_hms)
            {
                format_ts_hms_ms(start_s, sizeof(start_s), outs[i].start_ms);
                format_ts_hms_ms(end_s, sizeof(end_s), outs[i].end_ms);
            }
            else
            {
                time_t sec1 = (time_t)(outs[i].start_ms / 1000LL);
                int ms1 = (int)(outs[i].start_ms % 1000LL);
                if (ms1 < 0) ms1 = -ms1;
                time_t sec2 = (time_t)(outs[i].end_ms / 1000LL);
                int ms2 = (int)(outs[i].end_ms % 1000LL);
                if (ms2 < 0) ms2 = -ms2;

                struct tm tmv1;
                struct tm tmv2;
                localtime_r(&sec1, &tmv1);
                localtime_r(&sec2, &tmv2);

                char buf1[32];
                char buf2[32];
                if (strftime(buf1, sizeof(buf1), "%Y-%m-%dT%H:%M:%S", &tmv1) == 0)
                    snprintf(buf1, sizeof(buf1), "%lldms", outs[i].start_ms);
                if (strftime(buf2, sizeof(buf2), "%Y-%m-%dT%H:%M:%S", &tmv2) == 0)
                    snprintf(buf2, sizeof(buf2), "%lldms", outs[i].end_ms);

                snprintf(start_s, sizeof(start_s), "%s.%03d", buf1, ms1);
                snprintf(end_s, sizeof(end_s), "%s.%03d", buf2, ms2);
            }
            printf("%s,%s,%s,%lu,%s\n",
                   start_s, end_s,
                   outs[i].app_class,
                   outs[i].pid,
                   outs[i].title);
        }

        free(states);
        free(outs);
        free(recs);
        return 0;
    }

    Entry *arr = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (size_t ri = 0; ri < nrec; ri++)
    {
        long long start_ms = recs[ri].start_ms;
        long long end_ms = recs[ri].end_ms;
        if (end_ms <= start_ms) continue;

        const char *app_class = recs[ri].app_class;
        const char *title = recs[ri].title;
        if (!app_class || !title || !app_class[0] || !title[0]) continue;

        long long duration_ms = end_ms - start_ms;

        Entry *e = find_entry(arr, len, app_class, title);
        if (!e)
        {
            if (len == cap)
            {
                size_t new_cap = cap == 0 ? 32 : cap * 2;
                Entry *p = realloc(arr, new_cap * sizeof(*p));
                if (!p) break;
                arr = p;
                cap = new_cap;
            }

            e = &arr[len++];
            memset(e, 0, sizeof(*e));
            strncpy(e->app_class, app_class, sizeof(e->app_class) - 1);
            strncpy(e->title, title, sizeof(e->title) - 1);
            e->total_ms = 0;
            e->covered_end_ms = 0;
        }

        // Records are not guaranteed to be "non-overlapping". With the updated
        // tracker (heartbeat snapshots), you can get multiple rows with the same
        // (start,end) lineage for a key (nested intervals). We compute the UNION
        // length per key incrementally:
        //
        // additional = max(0, end - covered_end_ms)
        // But if this is a new disjoint interval (start >= covered_end_ms),
        // we add full (end - start).
        long long additional = 0;
        if (e->covered_end_ms <= 0)
        {
            // First interval for this key.
            additional = duration_ms;
        }
        else if (end_ms <= e->covered_end_ms)
        {
            additional = 0;
        }
        else if (start_ms >= e->covered_end_ms)
        {
            additional = end_ms - start_ms;
        }
        else
        {
            // Overlap with already-covered time; only add the tail.
            additional = end_ms - e->covered_end_ms;
        }

        if (additional > 0)
            e->total_ms += additional;
        if (end_ms > e->covered_end_ms)
            e->covered_end_ms = end_ms;
    }

    free(recs);
    recs = NULL;

    if (len == 0)
    {
        fprintf(stderr, "No records found in %s\n", input_path);
        free(arr);
        return 0;
    }

    Entry *copy = malloc(len * sizeof(*copy));
    if (!copy)
    {
        free(arr);
        return 1;
    }

    memcpy(copy, arr, len * sizeof(*copy));
    qsort(copy, len, sizeof(*copy), cmp_total_desc);

    if (topn > len) topn = len;

    printf("Top %zu by (app_class,title) total active time:\n", topn);
    for (size_t i = 0; i < topn; i++)
    {
        if (durfmt_hms)
        {
            char d[64];
            format_duration_hms_ms(d, sizeof(d), copy[i].total_ms);
            printf("%s | %s: %s\n", copy[i].app_class, copy[i].title, d);
        }
        else
        {
            double sec = (double)copy[i].total_ms / 1000.0;
            printf("%s | %s: %.2f sec\n", copy[i].app_class, copy[i].title, sec);
        }
    }

    free(copy);
    free(arr);
    return 0;
}

