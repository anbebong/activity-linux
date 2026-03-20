# activity (X11 active window / tab tracker)

## Build

```bash
cd /home/an/Desktop/activity
make clean && make
```

After build you will get:
- `out/activity-tracker` (X11 tracker)
- `out/aggregate-app-title` (tool to read `activity_usage.csv` and aggregate)

## Run tracker (write CSV)

The tracker writes to `activity_usage.csv` (or any file you pass via `--out`).

```bash
cd /home/an/Desktop/activity
rm -f activity_usage.csv
./out/activity-tracker --out activity_usage.csv
```

Stop with `Ctrl+C`.

CSV schema:
- `start_ts,end_ts,window_id,app_class,pid,title`
- `start_ts` / `end_ts` are ISO local time with milliseconds

## Show history (chronological)

Mode `history` prints a merged timeline per key to reduce overlap caused by the 30s heartbeat.

```bash
cd /home/an/Desktop/activity
./out/aggregate-app-title --in activity_usage.csv --mode history --limit 50
```

Mode `history_raw` prints records as-is from the CSV (may contain overlapping intervals).

```bash
./out/aggregate-app-title --in activity_usage.csv --mode history_raw --limit 50
```

## Totals aggregation (total active time)

```bash
./out/aggregate-app-title --in activity_usage.csv --mode totals --top 10
```

This aggregates total active time by `(app_class,title)` using `start_ts/end_ts` and handles overlap.

