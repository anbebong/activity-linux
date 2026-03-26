# SQLite amalgamation (build từ source, không dùng libsqlite3-dev)

1. Vào https://www.sqlite.org/download.html  
2. Tải gói **sqlite-amalgamation-XXXXXXX.zip** (một file zip chứa đúng 2 file quan trọng).  
3. Giải nén, copy **`sqlite3.c`** và **`sqlite3.h`** vào **thư mục này** (cùng cấp với file README này).

Sau đó từ thư mục gốc project:

```bash
make clean && make
```

Nếu có `third_party/sqlite/sqlite3.c`, Makefile sẽ **tự dùng source này** (compile `out/sqlite3.o` và link vào `activity-tracker`), **không** link `-lsqlite3` hệ thống.

Chỉ cần X11 dev:

```bash
sudo apt install build-essential libx11-dev
```

### Gợi ý tải nhanh (đổi mã phiên bản theo trang download)

```bash
cd third_party/sqlite
# ví dụ (kiểm tra số phiên bản mới trên sqlite.org):
curl -LO https://www.sqlite.org/2025/sqlite-amalgamation-3500600.zip
unzip -j sqlite-amalgamation-3500600.zip sqlite3.c sqlite3.h
rm -f sqlite-amalgamation-3500600.zip
```

Nếu URL 404, mở trang download và chỉnh lại năm trong path + mã amalgamation.
