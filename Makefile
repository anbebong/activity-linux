CC ?= gcc
CFLAGS ?= -O2 -Wall -std=c11
LDFLAGS ?= -lX11

TRACKER_TARGET ?= out/activity-tracker
AGG_TARGET ?= out/aggregate-app-title
SRC ?= main.c
AGG_SRC ?= aggregate_app_title.c

.PHONY: all clean

all: $(TRACKER_TARGET) $(AGG_TARGET)

$(TRACKER_TARGET): $(SRC)
	@mkdir -p $(dir $(TRACKER_TARGET))
	$(CC) $(CFLAGS) $(SRC) -o $(TRACKER_TARGET) $(LDFLAGS)

$(AGG_TARGET): $(AGG_SRC)
	@mkdir -p $(dir $(AGG_TARGET))
	$(CC) $(CFLAGS) $(AGG_SRC) -o $(AGG_TARGET)

clean:
	rm -f $(TRACKER_TARGET) $(AGG_TARGET)

