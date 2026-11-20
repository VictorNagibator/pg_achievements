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
	--temp-instance=tmp_check/regress \
	--temp-config=test/regress.conf \
	--encoding=UTF8 --no-locale \
	--dbname=contrib_regression
TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
