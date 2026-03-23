# SQLite: CHỈ dùng amalgamation (sqlite3.c + sqlite3.h) trong SQLITE_DIR — không dùng libsqlite3 hệ thống.
# Thiếu file → make báo lỗi; xem third_party/sqlite/README.md
#
CC ?= gcc
CFLAGS ?= -O2 -Wall -std=c11

SQLITE_DIR ?= third_party/sqlite
SQLITE_OBJ ?= out/sqlite3.o

ifeq ($(wildcard $(SQLITE_DIR)/sqlite3.c),)
$(error Missing $(SQLITE_DIR)/sqlite3.c — add amalgamation from sqlite.org; see third_party/sqlite/README.md)
endif

ifeq ($(wildcard $(SQLITE_DIR)/sqlite3.h),)
$(error Missing $(SQLITE_DIR)/sqlite3.h — add amalgamation from sqlite.org; see third_party/sqlite/README.md)
endif

SQLITE3_CFLAGS := -I$(SQLITE_DIR) \
	-DSQLITE_THREADSAFE=1 \
	-DSQLITE_OMIT_LOAD_EXTENSION=1

VENDOR_SQLITE_LDLIBS ?= -lpthread -ldl

TRACKER_TARGET ?= out/activity-tracker
AGG_TARGET ?= out/aggregate-app-title
SRC ?= main.c
AGG_SRC ?= aggregate_app_title.c

.PHONY: all clean

all: $(TRACKER_TARGET) $(AGG_TARGET)

$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SQLITE3_CFLAGS) -c $(SQLITE_DIR)/sqlite3.c -o $@

$(TRACKER_TARGET): $(SRC) $(SQLITE_OBJ)
	@mkdir -p $(dir $(TRACKER_TARGET))
	$(CC) $(CFLAGS) -I$(SQLITE_DIR) $(SRC) $(SQLITE_OBJ) -o $@ -lX11 $(VENDOR_SQLITE_LDLIBS)

$(AGG_TARGET): $(AGG_SRC) $(SQLITE_OBJ)
	@mkdir -p $(dir $(AGG_TARGET))
	$(CC) $(CFLAGS) -I$(SQLITE_DIR) $(AGG_SRC) $(SQLITE_OBJ) -o $@ $(VENDOR_SQLITE_LDLIBS)

clean:
	rm -f $(TRACKER_TARGET) $(AGG_TARGET) $(SQLITE_OBJ)
