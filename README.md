# activity — theo dõi cửa sổ đang focus (X11)

Tracker đọc `_NET_ACTIVE_WINDOW` trên X11, ghi **phiên cửa sổ** (session) vào SQLite. Mỗi lần đổi cửa sổ hoặc đổi tiêu đề (ví dụ đổi tab), phiên cũ được đóng và phiên mới mở; trong lúc giữ focus, tracker **cập nhật định kỳ** (~5 giây) thời lượng phiên đang mở.

Tài liệu thiết kế chi tiết: [`DESIGN_X11.md`](DESIGN_X11.md) (một số đoạn có thể còn nhắc schema cũ `intervals`; **schema thực tế hiện tại là `window_sessions`** — xem bên dưới).

## Thành phần trong thư mục này

| Output (sau `make`) | Mô tả |
|---------------------|--------|
| `out/activity-tracker` | Daemon X11, ghi `activity.db` |

**Xem / truy vấn dữ liệu:** dùng **`tools/activitydb`** (Go) — lệnh `query`, `stats`, `raw` hoặc `serve` (web UI + API). Build: `tools/activitydb/build_activitydb.sh` hoặc `cd tools/activitydb && go build`.

## Build

### SQLite: bắt buộc amalgamation (trong repo)

Dự án **chỉ** biên dịch SQLite từ amalgamation (`third_party/sqlite/sqlite3.c` + `sqlite3.h`), không link `libsqlite3` hệ thống. Nếu thiếu file, `make` sẽ báo lỗi; xem `third_party/sqlite/README.md`.

Có thể tải bản mới bằng script (đổi mã bản nếu URL 404):

```bash
cd activity
./scripts/fetch-sqlite-amalgamation.sh 3500600
# hoặc: export SQLITE_DL_YEAR=2024
```

### Phụ thuộc hệ thống (Debian/Ubuntu)

```bash
sudo apt install build-essential libx11-dev
```

### Build cục bộ

```bash
cd activity
make clean && make
```

Binary nằm trong `activity/out/`.

### Build trong Docker (môi trường CentOS 7 cố định)

```bash
cd activity
./scripts/build-in-docker.sh
```

Artifact được copy về `activity/out/` trên host.

## Chạy tracker

Mặc định ghi **`/opt/lancsmaster/data/activity.db`** nếu không truyền `--out`:

```bash
sudo mkdir -p /opt/lancsmaster/data
./out/activity-tracker
# hoặc: ./out/activity-tracker --out ./activity.db
```

Dừng: `Ctrl+C` hoặc gửi SIGTERM.

### Xoay file theo ngày (local midnight)

Khi sang ngày mới (theo timezone local), file đang ghi (ví dụ `activity.db`) được đổi tên thành `activity_YYYY-MM-DD.db` (ngày vừa kết thúc), rồi tạo lại `activity.db` trống. Phiên đang mở được flush trước khi đóng file.

## Schema SQLite: `window_sessions`

| Cột | Kiểu | Ý nghĩa |
|-----|------|--------|
| `id` | INTEGER PK | |
| `window_title` | TEXT | Tiêu đề cửa sổ (snapshot) |
| `process_name` | TEXT | `WM_CLASS.res_class` (hoặc tương đương) |
| `process_id` | INTEGER | PID (`_NET_WM_PID`) |
| `started_at_utc` | INTEGER | Unix **giây** (UTC) bắt đầu phiên |
| `last_seen_at_utc` | INTEGER | Unix **giây** cập nhật cuối (heartbeat) |
| `ended_at_utc` | INTEGER | Unix **giây** kết thúc (NULL nếu đang mở) |
| `duration_seconds` | INTEGER | Độ dài tính đến thời điểm cập nhật |
| `is_open` | INTEGER | 1 = phiên đang mở, 0 = đã đóng |

**Ghi chú:** Tên cột có `_utc` nhưng giá trị là **Unix epoch seconds** (số nguyên), không phải mili giây.

## Autostart (XDG)

Triển khai khớp mẫu **`autostart/activity-tracker.desktop.example`**: binary và DB thường đặt dưới `/opt/lancsmaster/` (không bắt buộc thư mục `bin/`).

| Đường dẫn | Nội dung |
|-----------|----------|
| `/opt/lancsmaster/activity-tracker` | Binary đã cài |
| `/opt/lancsmaster/data/activity.db` | SQLite (thư mục `data/` cần quyền ghi cho user GUI) |

### Cài binary + quyền thư mục dữ liệu

```bash
sudo mkdir -p /opt/lancsmaster/data
sudo install -m755 out/activity-tracker /opt/lancsmaster/activity-tracker
sudo chown USER:USER /opt/lancsmaster/data
```

### File `.desktop`

Mẫu có placeholder `@ACTIVITY_TRACKER_BIN@` và `@ACTIVITY_TRACKER_DB@`. **Khuyến nghị** dùng script:

```bash
./scripts/install-autostart.sh
```

Mặc định: `/opt/lancsmaster/activity-tracker` và `/opt/lancsmaster/data/activity.db`. Dev có thể:

```bash
./scripts/install-autostart.sh \
  --binary "$(pwd)/out/activity-tracker" \
  --db "$(pwd)/activity.db"
```

Gỡ autostart: `rm ~/.config/autostart/lancsmaster-activity-tracker.desktop`

## Truy vấn & giao diện (Go)

Ví dụ với binary `activitydb` đã build:

```bash
activitydb query --activity-db ./activity.db --from "2026-03-01 00:00:00" --top 10
activitydb serve --addr 127.0.0.1:8765 --activity-db ./activity.db
```

Xem `activitydb` (không tham số) để in usage đầy đủ.
