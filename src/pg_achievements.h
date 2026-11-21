/*-------------------------------------------------------------------------
 *
 * pg_achievements.h
 *		Shared declarations of the pg_achievements extension.
 *
 * Copyright (c) 2026, Viktor Donskoi
 * Released under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ACHIEVEMENTS_H
#define PG_ACHIEVEMENTS_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "port/atomics.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/spin.h"

#define PGA_VERSION			"1.2"
#define PGA_SCHEMA			"achievements"
#define PGA_EXTNAME			"pg_achievements"

/*
 * Counter list.  Single source of truth: the enum, the SQL-visible keys and
 * the aggregation type are all generated from this table.  New counters go
 * to the end; the keys are stored in achievements.counters, so a key must
 * never be renamed.
 *
 * streak_state is internal bookkeeping of streak_days: the local day number
 * of the last active day in the high 32 bits and the length of the current
 * run of consecutive days in the low 32 bits.  It only grows, so it can be
 * merged like any other "max" counter (see pga_note_active_day).
 */
typedef enum PgaCounterKind
{
	PGA_SUM,					/* value accumulates deltas */
	PGA_MAX						/* value keeps the largest observed number */
} PgaCounterKind;

#define PGA_COUNTER_LIST(X) \
	X(CONNECT,					"connect",					PGA_SUM) \
	X(STMT_SELECT,				"stmt_select",				PGA_SUM) \
	X(SELECT_STAR,				"select_star",				PGA_SUM) \
	X(ERR_SYNTAX,				"err_syntax",				PGA_SUM) \
	X(EXPLAIN_ANY,				"explain_any",				PGA_SUM) \
	X(EXPLAIN_ANALYZE,			"explain_analyze",			PGA_SUM) \
	X(CTE_ANY,					"cte_any",					PGA_SUM) \
	X(CTE_RECURSIVE,			"cte_recursive",			PGA_SUM) \
	X(WINDOW_FUNC,				"window_func",				PGA_SUM) \
	X(LATERAL,					"lateral",					PGA_SUM) \
	X(JSONB_OPS,				"jsonb_ops",				PGA_SUM) \
	X(JOIN_COUNT_MAX,			"join_count_max",			PGA_MAX) \
	X(NULL_EQ,					"null_eq",					PGA_SUM) \
	X(UPDATE_NO_WHERE,			"update_no_where",			PGA_SUM) \
	X(DELETE_NO_WHERE,			"delete_no_where",			PGA_SUM) \
	X(SEQSCAN_BIG,				"seqscan_big",				PGA_SUM) \
	X(QUERY_OVER_60S,			"query_over_60s",			PGA_SUM) \
	X(QUERY_OVER_1H,			"query_over_1h",			PGA_SUM) \
	X(DEADLOCK,					"deadlock",					PGA_SUM) \
	X(SERIALIZATION_FAILURE,	"serialization_failure",	PGA_SUM) \
	X(ROLLBACK_LONG_XACT,		"rollback_long_xact",		PGA_SUM) \
	X(DDL_FRIDAY_EVENING,		"ddl_friday_evening",		PGA_SUM) \
	X(NIGHT_QUERY,				"night_query",				PGA_SUM) \
	X(CREATE_TABLE,				"create_table",				PGA_SUM) \
	X(CREATE_INDEX,				"create_index",				PGA_SUM) \
	X(CREATE_INDEX_CONCURRENTLY, "create_index_concurrently", PGA_SUM) \
	X(CREATE_FUNCTION,			"create_function",			PGA_SUM) \
	X(CREATE_TRIGGER,			"create_trigger",			PGA_SUM) \
	X(CREATE_EXTENSION,			"create_extension",			PGA_SUM) \
	X(PARTITION_CREATE,			"partition_create",			PGA_SUM) \
	X(DROP_TABLE,				"drop_table",				PGA_SUM) \
	X(DROP_DATABASE,			"drop_database",			PGA_SUM) \
	X(TRUNCATE,					"truncate",					PGA_SUM) \
	X(VACUUM_FULL,				"vacuum_full",				PGA_SUM) \
	X(TERMINATE_BACKEND,		"terminate_backend",		PGA_SUM) \
	/* added in 1.1 */ \
	X(ROLLBACK,					"rollback",					PGA_SUM) \
	X(ROLLBACK_TO_SAVEPOINT,	"rollback_to_savepoint",	PGA_SUM) \
	X(UPSERT,					"upsert",					PGA_SUM) \
	X(RETURNING,				"returning",				PGA_SUM) \
	X(COMMENT_ON,				"comment_on",				PGA_SUM) \
	X(MERGE,					"merge",					PGA_SUM) \
	X(REFRESH_MATVIEW_CONCURRENTLY, "refresh_matview_concurrently", PGA_SUM) \
	X(SKIP_LOCKED,				"skip_locked",				PGA_SUM) \
	X(GROUPING_SETS,			"grouping_sets",			PGA_SUM) \
	X(CREATE_POLICY,			"create_policy",			PGA_SUM) \
	X(LISTEN_NOTIFY,			"listen_notify",			PGA_SUM) \
	X(ERR_UNDEFINED_TABLE,		"err_undefined_table",		PGA_SUM) \
	X(ERR_DIVISION_BY_ZERO,		"err_division_by_zero",		PGA_SUM) \
	X(CARTESIAN_PRODUCT,		"cartesian_product",		PGA_SUM) \
	X(NOT_IN_SUBQUERY,			"not_in_subquery",			PGA_SUM) \
	X(LOCK_EXCLUSIVE,			"lock_exclusive",			PGA_SUM) \
	X(EMOJI_NAME,				"emoji_name",				PGA_SUM) \
	X(LONG_NAME,				"long_name",				PGA_SUM) \
	X(NEW_YEAR_QUERY,			"new_year_query",			PGA_SUM) \
	X(DISABLE_ATTEMPT,			"disable_attempt",			PGA_SUM) \
	X(STREAK_STATE,				"streak_state",				PGA_MAX) \
	X(STREAK_DAYS,				"streak_days",				PGA_MAX) \
	X(ROWS_PROCESSED,			"rows_processed",			PGA_SUM)

#define PGA_ENUM_ENTRY(id, key, kind) PGA_C_##id,
typedef enum PgaCounter
{
	PGA_COUNTER_LIST(PGA_ENUM_ENTRY)
	PGA_NUM_COUNTERS
} PgaCounter;
#undef PGA_ENUM_ENTRY

typedef struct PgaCounterInfo
{
	const char *key;
	PgaCounterKind kind;
} PgaCounterInfo;

extern const PgaCounterInfo pga_counter_info[PGA_NUM_COUNTERS];

extern int	pga_counter_by_key(const char *key);

/* Tiers, in the same order as the SQL enum achievements.tier */
typedef enum PgaTier
{
	PGA_TIER_BRONZE,
	PGA_TIER_SILVER,
	PGA_TIER_GOLD,
	PGA_TIER_PLATINUM,
	PGA_NUM_TIERS
} PgaTier;

extern const char *const pga_tier_names[PGA_NUM_TIERS];
extern int	pga_tier_by_name(const char *name);

/* Special counter keys of meta achievements */
#define PGA_META_PREFIX			"meta:"
#define PGA_META_TIER_PREFIX	"meta:tier:"
#define PGA_META_ALL			"meta:all"

/* "counter" index values for meta achievements */
#define PGA_META_KIND_TIER		(-1)	/* all builtin achievements of a tier */
#define PGA_META_KIND_ALL		(-2)	/* all other builtin achievements */

/*
 * Catalog snapshot kept in shared memory.  Slots are stable for the lifetime
 * of the shared memory segment: a code always maps to the same slot, so the
 * per-role "awarded" bitmaps can refer to slot numbers.
 */
#define PGA_MAX_CATALOG			256
#define PGA_AWARD_WORDS			(PGA_MAX_CATALOG / 64)
#define PGA_CODE_LEN			64
#define PGA_TITLE_LEN			128
#define PGA_DESC_LEN			256

typedef struct PgaCatalogSlot
{
	bool		used;			/* slot holds a code */
	bool		active;			/* present in the table and enabled */
	bool		builtin;
	bool		secret;
	int16		tier;			/* PgaTier */
	int16		counter;		/* PgaCounter, or PGA_META_KIND_* */
	int16		meta_tier;		/* tier for PGA_META_KIND_TIER */
	int32		points;
	int64		threshold;
	char		code[PGA_CODE_LEN];
	char		title[PGA_TITLE_LEN];
	char		description[PGA_DESC_LEN];
} PgaCatalogSlot;

/*
 * Per-role state in the shared hash table, keyed by role name.  Values are
 * absolute totals (persisted value plus every delta flushed since).
 */
typedef struct PgaRoleEntry
{
	NameData	rolname;		/* hash key, must be first */
	pg_atomic_uint64 values[PGA_NUM_COUNTERS];
	pg_atomic_uint64 awarded[PGA_AWARD_WORDS];
	pg_atomic_uint32 reset_requested;

	/* fields below are touched by the worker only */
	int64		persisted[PGA_NUM_COUNTERS];
	uint64		persisted_awarded[PGA_AWARD_WORDS];
} PgaRoleEntry;

/* Queue of fresh awards, drained by the worker */
#define PGA_AWARD_QUEUE_SIZE	256

typedef struct PgaQueuedAward
{
	NameData	rolname;
	int			slot;
	TimestampTz awarded_at;
} PgaQueuedAward;

typedef struct PgaShared
{
	LWLock	   *lock;			/* protects the role hash and the catalog */
	slock_t		queue_mutex;	/* protects the award queue */

	/* worker state */
	pid_t		worker_pid;
	Latch	   *worker_latch;
	Latch	   *launcher_latch;
	pg_atomic_uint32 ready;		/* catalog and progress loaded from tables */
	pg_atomic_uint32 catalog_version;
	pg_atomic_uint64 flush_requests;	/* bumped by backends asking for sync */
	pg_atomic_uint64 flush_done;	/* last request number served */
	pg_atomic_uint32 hash_full_warned;

	/* catalog */
	int			catalog_size;	/* number of used slots */
	PgaCatalogSlot catalog[PGA_MAX_CATALOG];

	/* award queue (ring buffer) */
	int			queue_head;
	int			queue_len;
	PgaQueuedAward queue[PGA_AWARD_QUEUE_SIZE];
} PgaShared;

/* shmem.c */
extern PgaShared *pga_shared;
extern struct HTAB *pga_roles;

extern void pga_shmem_install_hooks(void);
extern PgaRoleEntry *pga_find_role(const char *rolname, bool create);
extern bool pga_enqueue_award(const char *rolname, int slot, TimestampTz ts);
extern void pga_purge_queue_role(const char *rolname);
extern int	pga_drain_queue(PgaQueuedAward *out, int max);
extern void pga_reset_role_entry(PgaRoleEntry *entry);
extern void pga_reset_all_entries(void);
extern void pga_slot_rarity(int slot, int *holders, int *players);

static inline bool
pga_bit_test(pg_atomic_uint64 *words, int slot)
{
	return (pg_atomic_read_u64(&words[slot / 64]) & (UINT64CONST(1) << (slot % 64))) != 0;
}

/* returns true if this call set the bit */
static inline bool
pga_bit_set(pg_atomic_uint64 *words, int slot)
{
	uint64		bit = UINT64CONST(1) << (slot % 64);

	return (pg_atomic_fetch_or_u64(&words[slot / 64], bit) & bit) == 0;
}

static inline void
pga_bit_clear(pg_atomic_uint64 *words, int slot)
{
	pg_atomic_fetch_and_u64(&words[slot / 64], ~(UINT64CONST(1) << (slot % 64)));
}

/* guc.c */
extern bool pga_enabled;
extern char *pga_database;
extern bool pga_opt_out;
extern bool pga_notify;
extern int	pga_notice_style;
extern bool pga_track_nested;
extern int	pga_flush_interval_ms;
extern int	pga_big_table_rows;
extern int	pga_max_roles;
extern char *pga_notify_channel;
#ifdef PGA_TESTING
extern char *pga_test_now;
extern int	pga_test_time_shift;
extern bool pga_test_inject_error;
#endif

typedef enum PgaNoticeStyle
{
	PGA_NOTICE_EMOJI,
	PGA_NOTICE_ASCII
} PgaNoticeStyle;

extern void pga_define_gucs(void);
extern TimestampTz pga_now(void);
extern int64 pga_duration_shift_us(void);

/* counters.c */
extern void pga_count(PgaCounter c, int64 n);
extern void pga_count_max(PgaCounter c, int64 v);
extern void pga_count_error(int sqlerrcode);
extern void pga_note_active_day(int32 day);
extern void pga_flush_local(bool check);
extern void pga_invalidate_cache(void);
extern void pga_maybe_flush_by_time(TimestampTz now);
extern void pga_set_role_name(const char *rolname);
extern bool pga_sync_role(void);
extern int64 pga_counter_value(PgaCounter c);
extern void pga_forget_local(void);
extern void pga_check_all(void);
extern bool pga_session_disabled;
extern void pga_disable_session(ErrorData *edata);
extern bool pga_tracking_active(void);
extern int	pga_award_slot(PgaRoleEntry *entry, const char *rolname, int slot,
						   bool notify);
extern const PgaCatalogSlot *pga_local_slot(int slot);
extern int	pga_local_slot_by_code(const char *code);
extern const char *pga_current_role_name(void);
extern bool pga_grant(const char *rolname, int slot);

/* notice.c */
extern void pga_send_notice(PgaRoleEntry *entry, int slot);
extern int64 pga_total_score(PgaRoleEntry *entry);

/* hooks.c */
extern void pga_install_hooks(void);

/* worker.c */
extern void pga_register_worker(void);
extern void pga_wake_worker(void);
PGDLLEXPORT void pga_worker_main(Datum main_arg);
PGDLLEXPORT void pga_launcher_main(Datum main_arg);

#define PGA_WORKER_RESTART_S	10

#endif							/* PG_ACHIEVEMENTS_H */
