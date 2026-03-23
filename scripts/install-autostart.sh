#!/usr/bin/env bash
# Cài autostart: copy từ file mẫu autostart/*.desktop.example, thay placeholder đường dẫn.
# Dùng: ./scripts/install-autostart.sh [--binary PATH] [--db PATH]
# Env: LANCSMASTER_ROOT, ACTIVITY_TRACKER_BIN, ACTIVITY_TRACKER_DB, ACTIVITY_DESKTOP_TEMPLATE
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ACTIVITY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LANCS_ROOT="${LANCSMASTER_ROOT:-/opt/lancsmaster}"
DEFAULT_BIN="${LANCS_ROOT}/activity-tracker"
DEFAULT_DB="${LANCS_ROOT}/data/activity.db"
TEMPLATE="${ACTIVITY_DESKTOP_TEMPLATE:-${ACTIVITY_ROOT}/autostart/activity-tracker.desktop.example}"
AUTOSTART_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
DESKTOP_NAME="lancsmaster-activity-tracker.desktop"

BIN="${ACTIVITY_TRACKER_BIN:-$DEFAULT_BIN}"
DB="${ACTIVITY_TRACKER_DB:-$DEFAULT_DB}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) BIN="$2"; shift 2 ;;
        --db)     DB="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--binary PATH] [--db PATH]"
            echo "  Sao chép từ: \${ACTIVITY_DESKTOP_TEMPLATE:-autostart/activity-tracker.desktop.example}"
            echo "  Env: LANCSMASTER_ROOT, ACTIVITY_TRACKER_BIN, ACTIVITY_TRACKER_DB, ACTIVITY_DESKTOP_TEMPLATE"
            echo "  Mặc định: \${LANCSMASTER_ROOT}/activity-tracker --out \${LANCSMASTER_ROOT}/data/activity.db"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ ! -f "$TEMPLATE" ]]; then
    echo "Không tìm thấy file mẫu .desktop: $TEMPLATE" >&2
    exit 1
fi

if [[ ! -x "$BIN" ]]; then
    echo "Không tìm thấy binary thực thi: $BIN" >&2
    echo "Sao chép binary (vd: sudo install -m755 \"${ACTIVITY_ROOT}/out/activity-tracker\" \"${BIN}\")" >&2
    echo "Hoặc dev: $0 --binary \"${ACTIVITY_ROOT}/out/activity-tracker\" --db \"${ACTIVITY_ROOT}/activity.db\"" >&2
    exit 1
fi

if [[ "$BIN" == *\'* ]] || [[ "$DB" == *\'* ]]; then
    echo "Đường dẫn không được chứa dấu nháy đơn (')." >&2
    exit 1
fi

DATA_DIR="$(dirname "$DB")"
mkdir -p "$DATA_DIR" "$AUTOSTART_DIR"

OUT_FILE="${AUTOSTART_DIR}/${DESKTOP_NAME}"

# Thay placeholder trong file mẫu (bash ${var//a/b}; không dùng nháy đơn trong đường dẫn).
content=$(cat "$TEMPLATE")
content="${content//@ACTIVITY_TRACKER_BIN@/$BIN}"
content="${content//@ACTIVITY_TRACKER_DB@/$DB}"
printf '%s' "$content" >"$OUT_FILE"

echo "Đã tạo từ mẫu: $TEMPLATE"
echo " -> $OUT_FILE"
echo "Binary: $BIN"
echo "Database: $DB"
echo ""
echo "Đăng xuất / đăng nhập lại (hoặc đăng nhập GUI) để chạy. Gỡ: rm \"$OUT_FILE\""
