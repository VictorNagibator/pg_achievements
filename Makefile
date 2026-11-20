# pg_achievements — PGXS build
MODULE_big = pg_achievements
OBJS = \
	src/pg_achievements.o \
	src/guc.o \
	src/shmem.o \
	src/counters.o \
	src/hooks.o \
	src/notice.o \
	src/worker.o

EXTENSION = pg_achievements
DATA = $(wildcard sql/pg_achievements--*.sql)
PGFILEDESC = "pg_achievements - achievements for PostgreSQL users"

# Test build: `make PGA_TESTING=1` adds GUCs that fake the clock.
ifdef PGA_TESTING
PG_CPPFLAGS += -DPGA_TESTING
endif

# pg_regress starts its own instance with the extension preloaded.
REGRESS = $(patsubst test/sql/%.sql,%,$(sort $(wildcard test/sql/*.sql)))
REGRESS_OPTS = --inputdir=test --outputdir=test \
	--temp-instance=tmp_regress \
	--temp-config=test/regress.conf \
	--encoding=UTF8 --no-locale
TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): src/pg_achievements.h

# Release archive for PGXN and GitHub: make dist -> pg_achievements-X.Y.Z.zip
# Built from the committed tree (git archive), so commit before running it.
DIST_VERSION = $(shell sed -n 's/^   "version": "\(.*\)",/\1/p' META.json)
dist:
	git archive --format zip --prefix=$(EXTENSION)-$(DIST_VERSION)/ \
		-o $(EXTENSION)-$(DIST_VERSION).zip HEAD
.PHONY: dist
