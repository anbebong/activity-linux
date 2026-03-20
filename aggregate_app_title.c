/*
 * aggregate_app_title.c
 * ----------------------
 * Aggregate activity_usage.csv by (app_class, title).
 *
 * Expected CSV schema (header may exist):
 *   start_ts,end_ts,window_id,app_class,pid,title
 *
 * Where start_ts/end_ts are ISO local time with milliseconds:
 *   2026-03-20T15:52:10.123
 *
 * This tool computes duration_ms = end_ts - start_ts and sums it per key.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    char app_class[256];
    char title[512];
    long long total_ms;
    long long covered_end_ms; // latest end timestamp included in total_ms
} Entry;

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
            "Usage: %s --in FILE [--mode history|totals] [--top N] [--limit N]\n"
            "\n"
            "Modes:\n"
            "  history  Print merged history (removes heartbeat-overlap duplicates) by (window_id,pid,app_class,title).\n"
            "  history_raw  Print records as-is from CSV (may contain overlapping heartbeat snapshots).\n"
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
    const char *in_path = "activity_usage.csv";
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
            in_path = argv[++i];
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

    FILE *f = fopen(in_path, "r");
    if (!f)
    {
        fprintf(stderr, "Cannot open input: %s\n", in_path);
        return 1;
    }

    if (mode_history == 2)
    {
        char line[16384];
        if (!timefmt_hms)
        {
            // Print header + records as-is.
            while (fgets(line, sizeof(line), f))
            {
                if (limit > 0)
                {
                    // Count only data lines (skip header).
                    if (strncmp(line, "start_ts", 9) == 0)
                    {
                        fputs(line, stdout);
                        continue;
                    }
                    static size_t seen = 0;
                    if (seen >= limit) break;
                    seen++;
                }
                fputs(line, stdout);
            }
            fclose(f);
            return 0;
        }

        // timefmt_hms: parse & reformat start/end while printing records.
        // Header stays the same (start_ts,end_ts,...).
        int is_header_printed = 0;
        while (fgets(line, sizeof(line), f))
        {
            if (limit > 0)
            {
                if (!is_header_printed)
                    ;
                static size_t seen = 0;
                if (seen >= limit) break;
                // Only count data lines.
                if (strncmp(line, "start_ts", 9) != 0)
                    seen++;
            }

            if (strncmp(line, "start_ts", 9) == 0)
            {
                if (!is_header_printed)
                {
                    fputs(line, stdout);
                    is_header_printed = 1;
                }
                continue;
            }

            char fields6[6][2048];
            if (!csv_split_6(line, fields6))
                continue;

            int ok1 = 0, ok2 = 0;
            long long start_ms = parse_iso_ms(fields6[0], &ok1);
            long long end_ms = parse_iso_ms(fields6[1], &ok2);
            if (!ok1 || !ok2) continue;

            char start_s[64];
            char end_s[64];
            format_ts_hms_ms(start_s, sizeof(start_s), start_ms);
            format_ts_hms_ms(end_s, sizeof(end_s), end_ms);

            const char *window_id = fields6[2];
            const char *app_class = fields6[3];
            unsigned long pid = (unsigned long)strtoul(fields6[4], NULL, 10);
            const char *title = fields6[5];
            if (!window_id || !app_class || !title) continue;

            printf("%s,%s,%s,%s,%lu,%s\n",
                   start_s, end_s,
                   window_id, app_class, pid, title);
        }
        fclose(f);
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
            if (!ok1 || !ok2) continue;
            if (end_ms <= start_ms) continue;

            const char *window_id = fields6[2];
            const char *app_class = fields6[3];
            unsigned long pid = (unsigned long)strtoul(fields6[4], NULL, 10);
            const char *title = fields6[5];
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
        fclose(f);
        return 0;
    }

    Entry *arr = NULL;
    size_t len = 0;
    size_t cap = 0;

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
        if (!ok1 || !ok2) continue;
        if (end_ms <= start_ms) continue;

        const char *app_class = fields6[3];
        const char *title = fields6[5];
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

    fclose(f);

    if (len == 0)
    {
        fprintf(stderr, "No records found in %s\n", in_path);
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

