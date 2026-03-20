# X11 Window Activity Tracker (Active Window Time)

## Muc tieu
- Theo dõi cửa sổ đang được focus (active window) trên X11.
- Mỗi khi active window thay đổi:
  - đóng interval cửa sổ cũ và ghi duration.
  - mở interval cửa sổ mới.
- Ghi kết quả vào file CSV để có thể tính tổng thời gian theo ứng dụng/cửa sổ về sau.

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

3) Storage dang interval -> CSV
   - Moi lan active window doi:
     - ghi 1 record:
       - `start_ts_ms, end_ts_ms, duration_ms, window_id, app_class, pid, title`

## Module tach biet
Thuc te de de maintain, tach thanh 2 module:

### Module A: Window Tracker
- Chay event-driven tren X11.
- Snapshot metadata (app_class/title/pid) tai thoi diem mo interval.
- Ghi record interval vao file CSV append-only.

### Module B: Aggregator (tong hop thong ke)
- Doc file CSV do Module A ghi.
- Cong don duration_ms theo key:
  - mac dinh: `app_class`
  - co the mo rong: theo `title`
- (Toi thieu cho v1) Aggregator thuong chay theo che do "one-shot": doc toan bo CSV va tinh tong.
- (Co the mo rong sau) Cong them che do incremental:
  - luu vi tri file (byte offset) da doc vao state file
  - lan chay tiep theo chi doc phan moi.

## Giao tiep giua 2 module
- Dung 1 file CSV duy nhat lam contract.
- Module B chi can biet thu tu cot va cach CSV escape string.

## Vi sao CSV?
- Repo hien tai khong dam bao co libsqlite3-dev trong moi truong build.
- CSV de kiem chung va tinh tong bang tool khac (Python/awk/spreadsheet).

## Dinh dang CSV
- header:
  - `start_ts_ms,end_ts_ms,duration_ms,window_id,app_class,pid,title`
- window_id:
  - ghi dang hex (0x...), de phan biet.

## Escaping CSV
- Neu field co dau phay/dau ngoac kep/newline -> boc bang `"..."`.
- `"` trong chuoi -> thay bang `""`.

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

