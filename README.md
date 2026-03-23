# activity (X11 active window / tab tracker)

## Build

### SQLite: bắt buộc amalgamation (từ sqlite.org, **không** dùng `libsqlite3` hệ thống)

1. Đặt **`sqlite3.c`** và **`sqlite3.h`** vào `third_party/sqlite/` (xem `third_party/sqlite/README.md`), hoặc chạy script (đổi mã bản nếu URL 404):

   ```bash
   ./scripts/fetch-sqlite-amalgamation.sh 3500600
   # hoặc: export SQLITE_DL_YEAR=2024  # nếu năm trong URL không khớp
   ```

2. Chỉ cài X11:

   ```bash
   sudo apt install build-essential libx11-dev
   ```

3. Build — Makefile compile `out/sqlite3.o` từ amalgamation và link vào cả hai binary. Thiếu `sqlite3.c`/`sqlite3.h` → `make` báo lỗi rõ ràng.

   ```bash
   cd /home/an/Desktop/activity
   make clean && make
   ```

Sau khi build:

- `out/activity-tracker` — tracker X11, ghi vào SQLite
- `out/aggregate-app-title` — đọc DB (mặc định) hoặc CSV cũ để tổng hợp

## Run tracker (SQLite)

Mặc định ghi file `activity.db` (hoặc chỉ định `--out`):

```bash
cd /home/an/Desktop/activity
rm -f activity.db
./out/activity-tracker --out activity.db
```

Dừng bằng `Ctrl+C`.

Bảng `intervals`:

| Cột | Kiểu | Ý nghĩa |
|-----|------|--------|
| `start_ms`, `end_ms` | INTEGER | Unix epoch milliseconds |
| `window_id` | TEXT | Ví dụ `0x12345678` |
| `app_class`, `title` | TEXT | |
| `pid` | INTEGER | |

Logic giữ như trước: đổi cửa sổ / đổi title (tab) ghi interval; heartbeat ~30s ghi snapshot `(current_start, now)` **không** đổi `current_start` cho đến khi đổi tab/cửa sổ.

**Xoay DB theo ngày (local midnight):** khi sang ngày mới, file hiện tại (vd. `activity.db`) được đổi tên thành `activity_YYYY-MM-DD.db` (ngày vừa kết thúc), rồi tạo lại `activity.db` trống và ghi tiếp. Interval đang mở được flush trước khi đóng file; trên cửa sổ vẫn focus thì bắt đầu interval mới tại thời điểm xoay.

## Xem history (theo thời gian)

Chế độ `history` gộp các snapshot heartbeat trùng (theo `window_id`, pid, app, title):

```bash
./out/aggregate-app-title --db activity.db --mode history --limit 50
```

Mặc định đọc `--db activity.db` nên có thể bỏ `--db` nếu file tên đúng vậy:

```bash
./out/aggregate-app-title --mode history --limit 50
```

`history_raw` in từng dòng bản ghi (có thể overlap do heartbeat):

```bash
./out/aggregate-app-title --mode history_raw --limit 50
```

## Tổng thời gian theo (app_class, title)

```bash
./out/aggregate-app-title --mode totals --top 10
```

## CSV cũ (tuỳ chọn)

```bash
./out/aggregate-app-title --in activity_usage.csv --mode totals --top 10
```

Schema CSV: `start_ts,end_ts,window_id,app_class,pid,title` (ISO local có ms).
