/*-------------------------------------------------------------------------
 *
 * pg_achievements.c
 *		Module entry point: GUCs, shared memory, hooks and the worker.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "fmgr.h"
#include "funcapi.h"
#include "access/xact.h"
#if PG_VERSION_NUM >= 160000
#include "nodes/queryjumble.h"
#else
#include "utils/queryjumble.h"	/* EnableQueryId() lived here before 16 */
#endif
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

#define PGA_X_INFO(id, key, kind) {key, kind},
const PgaCounterInfo pga_counter_info[PGA_NUM_COUNTERS] = {
	PGA_COUNTER_LIST(PGA_X_INFO)
};
#undef PGA_X_INFO

const char *const pga_tier_names[PGA_NUM_TIERS] = {
	"bronze", "silver", "gold", "platinum"
};

/*
 * Can this server ever bump counter c?  MERGE appeared in PostgreSQL 15,
 * so on 14 the "merge" counter stays at zero forever.  The worker loads
 * achievements on such counters as inactive, which keeps them out of the
 * "collect them all" meta achievements: otherwise platinum could never be
 * earned on 14.
 */
bool
pga_counter_supported(int c)
{
#if PG_VERSION_NUM < 150000
	if (c == PGA_C_MERGE)
		return false;
#endif
	return c >= 0 && c < PGA_NUM_COUNTERS;
}

/* Counter index by SQL key, or -1. */
int
pga_counter_by_key(const char *key)
{
	for (int i = 0; i < PGA_NUM_COUNTERS; i++)
	{
		if (strcmp(pga_counter_info[i].key, key) == 0)
			return i;
	}
	return -1;
}

/* Tier index by its SQL enum label, or -1. */
int
pga_tier_by_name(const char *name)
{
	for (int i = 0; i < PGA_NUM_TIERS; i++)
	{
		if (strcmp(pga_tier_names[i], name) == 0)
			return i;
	}
	return -1;
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_achievements must be loaded via shared_preload_libraries")));

	pga_define_gucs();
	pga_shmem_install_hooks();
	pga_install_hooks();
	pga_register_worker();

	/*
	 * Query ids let the executor hooks find the features of a prepared
	 * statement analyzed in an earlier transaction (see hooks.c).  As with
	 * pg_stat_statements, compute_query_id = auto now computes them;
	 * compute_query_id = off still turns them off.
	 */
	EnableQueryId();
}

/* ---------- SQL-callable functions ---------- */

PG_FUNCTION_INFO_V1(pga_sql_counter);
PG_FUNCTION_INFO_V1(pga_sql_sync);
PG_FUNCTION_INFO_V1(pga_sql_reset);
PG_FUNCTION_INFO_V1(pga_sql_grant);
PG_FUNCTION_INFO_V1(pga_sql_counter_keys);
PG_FUNCTION_INFO_V1(pga_sql_library_version);
PG_FUNCTION_INFO_V1(pga_sql_counter_unsupported);

/* SQL functions cannot work if the library was not preloaded. */
static void
pga_require_shmem(void)
{
	if (pga_shared == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_achievements must be loaded via shared_preload_libraries")));
}

/* achievements.counter(key text) RETURNS bigint */
Datum
pga_sql_counter(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			c = pga_counter_by_key(key);

	pga_require_shmem();
	if (c < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown achievement counter \"%s\"", key)));

	PG_RETURN_INT64(pga_counter_value(c));
}

/*
 * achievements.sync(timeout_ms int) RETURNS bool
 *
 * Flush the local deltas and wait until the worker has completed a full
 * cycle, so that the tables reflect everything counted so far.
 */
Datum
pga_sql_sync(PG_FUNCTION_ARGS)
{
	int			timeout_ms = PG_GETARG_INT32(0);
	TimestampTz start = GetCurrentTimestamp();
	uint64		request;
	bool		done = false;

	pga_require_shmem();

	(void) pga_sync_role();
	pga_flush_local(pga_tracking_active());

	request = pg_atomic_add_fetch_u64(&pga_shared->flush_requests, 1);
	pga_wake_worker();

	for (;;)
	{
		if (pg_atomic_read_u64(&pga_shared->flush_done) >= request)
		{
			done = true;
			break;
		}
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), timeout_ms))
			break;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		pga_wake_worker();
	}

	/* the worker may have just loaded the catalog: report what is due */
	pga_invalidate_cache();
	pga_flush_local(pga_tracking_active());

	PG_RETURN_BOOL(done);
}

/*
 * achievements._reset(role name) RETURNS void
 *
 * Runs with the privileges of the caller: a role may reset itself, a
 * superuser may reset anyone.
 */
Datum
pga_sql_reset(PG_FUNCTION_ARGS)
{
	Name		role = PG_GETARG_NAME(0);
	PgaRoleEntry *entry;
	char	   *me;

	pga_require_shmem();

	me = GetUserNameFromId(GetUserId(), false);
	if (!superuser() && strcmp(me, NameStr(*role)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser can reset achievements of another role")));

	entry = pga_find_role(NameStr(*role), true);
	if (entry != NULL)
		pga_reset_role_entry(entry);

	if (pga_current_role_name() != NULL &&
		strcmp(pga_current_role_name(), NameStr(*role)) == 0)
		pga_forget_local();

	PG_RETURN_VOID();
}

/* achievements._grant(role name, code text) RETURNS bool, superuser only */
Datum
pga_sql_grant(PG_FUNCTION_ARGS)
{
	Name		role = PG_GETARG_NAME(0);
	char	   *code = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			slot;

	pga_require_shmem();

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only a superuser can grant achievements")));

	slot = pga_local_slot_by_code(code);
	if (slot < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("achievement \"%s\" is not loaded", code),
				 errhint("New achievements become available after the next flush, see achievements.sync().")));

	PG_RETURN_BOOL(pga_grant(NameStr(*role), slot));
}

/*
 * Set up a materialized set-returning call with the expected tuple
 * descriptor.  16 has InitMaterializedSRF() for this (15 has the same
 * thing as SetSingleFuncCall()); before 16 we spell the boilerplate out.
 */
static void
pga_init_materialized_srf(FunctionCallInfo fcinfo)
{
#if PG_VERSION_NUM >= 160000
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
#else
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext oldcxt;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize) ||
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	/* the tuplestore must outlive this call: build it in the query context */
	oldcxt = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setDesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	rsinfo->setResult =
		tuplestore_begin_heap((rsinfo->allowedModes & SFRM_Materialize_Random) != 0,
							  false, work_mem);
	MemoryContextSwitchTo(oldcxt);
#endif
}

/* achievements._counter_keys() RETURNS SETOF text */
Datum
pga_sql_counter_keys(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	pga_init_materialized_srf(fcinfo);
	for (int i = 0; i < PGA_NUM_COUNTERS; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(pga_counter_info[i].key);
		values[1] = CStringGetTextDatum(pga_counter_info[i].kind == PGA_SUM ? "sum" : "max");
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* achievements._library_version() RETURNS text */
Datum
pga_sql_library_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PGA_VERSION));
}

/*
 * achievements._counter_unsupported(key text) RETURNS bool
 *
 * True for a known counter this server can never bump (MERGE on 14, see
 * pga_counter_supported); false for every other key, unknown and meta keys
 * included.  The views hide achievements on such counters.
 */
Datum
pga_sql_counter_unsupported(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			c = pga_counter_by_key(key);

	PG_RETURN_BOOL(c >= 0 && !pga_counter_supported(c));
}
