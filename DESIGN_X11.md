# X11 Window Activity Tracker (Active Window Time)

## Muc tieu
- Theo dõi cửa sổ đang được focus (active window) trên X11.
- Mỗi khi active window thay đổi:
  - đóng interval cửa sổ cũ và ghi duration.
  - mở interval cửa sổ mới.
- Ghi kết quả vào **SQLite** (`activity.db` mặc định) để tính tổng thời gian theo ứng dụng/title về sau (aggregator vẫn có thể đọc CSV cũ qua `--in`).

## Gia dinh & pham vi
- Chi ho tro X11 (khong ho tro Wayland thuan).
- Do "active time" (thoi gian cua so dang duoc focus), khong tinh occlusion/che phu.
- Metadata (app/title/pid) duoc "snapshot" tai thoi diem mo interval.

## Kien truc
1) X11 Event Loop (event-driven)
   - Subscribe PropertyNotify tren root window cho property:
     - `_NET_ACTIVE_WINDOW`
   - Khi `_NET_ACTIVE_WINDOW` doi -> active_window_id moi

2) Metadata snapshot cho window moi
   - app_class: `WM_CLASS` (XGetClassHint)
     - `WM_CLASS` gom instance va class. Luc nay ta dung field `res_class` lam `app_class` chinh.
   - title:
     - uu tien `_NET_WM_NAME` (UTF-8), fallback `WM_NAME` (XFetchName)
   - pid (tuy chon):
     - `_NET_WM_PID`

3) Storage dang interval -> SQLite (bang `intervals`)
   - Moi lan doi cua so / doi title (tab) hoac heartbeat ~30s:
     - ghi 1 hang: `start_ms, end_ms, window_id, app_class, pid, title`
   - **Xoay file theo ngay (local):** khi doi ngay duong lich, file dang ghi (vd. `activity.db`) duoc `rename` thanh `STEM_YYYY-MM-DD.db` (ngay vua ket thuc), tao lai `activity.db` rong; flush interval hien tai truoc khi dong DB.

## Module tach biet
Thuc te de de maintain, tach thanh 2 module:

### Module A: Window Tracker
- Chay event-driven tren X11.
- Snapshot metadata (app_class/title/pid) tai thoi diem mo interval.
- Ghi record interval vao SQLite (append INSERT).

### Module B: Truy van / UI (ngoai thu muc activity)
- Doc SQLite bang `window_sessions` qua tool Go **`tools/activitydb`**: `query`, `stats`, `raw`, `serve` (web + API).

## Giao tiep giua tracker va tool phan tich
- Contract chinh: file SQLite + bang `window_sessions` (xem README activity).

## Dinh dang CSV (legacy)
- header: `start_ts,end_ts,window_id,app_class,pid,title` (ISO local + ms).
- window_id: hex `0x...`.

## Summary khi thoat (khong UI)
- In Top-N theo tong `duration_ms` nhom theo `app_class` trong pham vi 1 lan chay.
- Co the tat summary bang option `--no-summary`.

## Edge cases
- Khi `_NET_ACTIVE_WINDOW` tra ve `0`:
  - dong interval cua so cu (neu co), va khong mo interval moi.
- Title/app_class co the dai:
  - gioi han toi da 512 ky tu (truncate) de tranh record qua lon.

## Cach build & chay
- Build (can X11 dev headers):
  - `gcc -O2 -Wall -std=c11 main.c -o window-activity -lX11`
- Chay:
  - `./window-activity --out activity_usage.csv`
- Tuỳ chon:
  - `--out FILE`             duong dan CSV output (default: `activity_usage.csv`)
  - `--summary-top N`       in Top-N app_class theo thoi gian (default: `10`)
  - `--no-summary`          tat phan in summary khi thoat

## Ghi chu trien khai
- Dung Xlib de lay property. Event subscribe dua vao `PropertyNotify`.
- XGetWindowProperty de doc:
  - `_NET_ACTIVE_WINDOW`: `XA_WINDOW`
  - `_NET_WM_PID`: `CARDINAL` (32-bit)
  - `_NET_WM_NAME`: `AnyPropertyType` (uu tien UTF-8)

