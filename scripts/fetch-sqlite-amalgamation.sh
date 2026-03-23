#!/bin/sh
# Tải amalgamation SQLite vào third_party/sqlite/
# Dùng: ./scripts/fetch-sqlite-amalgamation.sh [MÃ_ZIP]
# Ví dụ: ./scripts/fetch-sqlite-amalgamation.sh 3500600
# Mã lấy tên file zip trên https://www.sqlite.org/download.html (sqlite-amalgamation-XXXXXXX.zip)

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/sqlite"
VER="${1:-3500600}"

mkdir -p "$DEST"
cd "$DEST"

# Năm trong URL đổi theo bản phát hành — nếu 404, sửa YEAR hoặc tải tay từ trang download.
YEAR="${SQLITE_DL_YEAR:-2025}"
ZIP="sqlite-amalgamation-3510300.zip"
URL="https://sqlite.org/2026/sqlite-amalgamation-3510300.zip"

echo "Fetching $URL"
curl -fL -o "$ZIP" "$URL"
unzip "$ZIP"
rm -f "$ZIP"
echo "OK: $DEST/sqlite3.c sqlite3.h"
https://sqlite.org/2026/sqlite-amalgamation-3510300.zip