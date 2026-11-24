/*-------------------------------------------------------------------------
 *
 * hooks.c
 *		Event detection: parse analysis, utility commands, executor, errors,
 *		transactions and authentication.
 *
 * Rules:
 *	- only top-level statements are counted unless track_nested is on;
 *	- update_no_where, delete_no_where, null_eq, select_star,
 *	  cartesian_product and not_in_subquery are counted at parse analysis
 *	  (the attempt matters); everything else derived from a query is kept as
 *	  "pending" and counted only when the statement finishes successfully
 *	  (ExecutorEnd or the end of ProcessUtility);
 *	- our own code runs inside PG_TRY: an internal failure prints a single
 *	  WARNING, disables tracking for the session and never breaks the user's
 *	  query.  Errors of the hooked functions themselves propagate unchanged.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/dbcommands.h"
#if PG_VERSION_NUM >= 190000
#include "catalog/pg_database.h"	/* get_database_oid() moved here in 19 */
#endif
#include "commands/extension.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "libpq/auth.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

/* PostgreSQL 19 passes the query jumble to post_parse_analyze_hook as const */
#if PG_VERSION_NUM >= 190000
typedef const JumbleState PgaJumbleState;
#else
typedef JumbleState PgaJumbleState;
#endif

/* JSONB operators: -> (text, int4), ->> (text, int4), @>, ? */
#define OID_JSONB_OBJECT_FIELD		3211
#define OID_JSONB_ARRAY_ELEMENT		3212
#define OID_JSONB_OBJECT_FIELD_TEXT 3477
#define OID_JSONB_ARRAY_ELEMENT_TEXT 3481
#define OID_JSONB_CONTAINS			3246
#define OID_JSONB_EXISTS			3247

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;
static ClientAuthentication_hook_type prev_ClientAuthentication = NULL;

/* depth of executor / utility nesting, as in pg_stat_statements */
static int	nesting_level = 0;

/* the session has just authenticated: count "connect" at startup commit */
static bool startup_pending = false;

/*
 * Nesting level of the statement prepared by a top-level PREPARE or run by
 * a top-level EXECUTE, which counts like a top-level statement; -1 when
 * there is none.
 */
static int	execute_level = -1;

/* structural features of an analyzed query, counted each time it succeeds */
#define PF_CTE			0x0001
#define PF_RECURSIVE	0x0002
#define PF_WINDOW		0x0004
#define PF_LATERAL		0x0008
#define PF_SELECT		0x0020
#define PF_UPSERT		0x0040
#define PF_RETURNING	0x0080
#define PF_MERGE		0x0100
#define PF_SKIP_LOCKED	0x0200
#define PF_GROUPING		0x0400

typedef struct PgaFeatures
{
	uint32		flags;			/* PF_* */
	int			joins;
	int			jsonb_ops;
	int			terminate;
	int			notify;			/* pg_notify() calls */
} PgaFeatures;

/*
 * The statement analyzed last at each nesting level, counted when it
 * succeeds.  "valid" is cleared once it has been counted.
 */
#define PGA_MAX_NESTING 32

typedef struct PgaPending
{
	bool		valid;
	uint64		query_id;		/* 0 if query ids are not computed */
	PgaFeatures features;
} PgaPending;

static PgaPending pending[PGA_MAX_NESTING];

/*
 * Features by query id.  A prepared statement is analyzed once and may run
 * any number of times later, in other transactions: its features can only
 * be found again through the query id the plan carries.  This needs query
 * ids, which _PG_init turns on with EnableQueryId() as pg_stat_statements
 * does; with compute_query_id = off only the execution that follows the
 * analysis counts.
 *
 * Every lookup or insert ticks feature_clock.  When the cache is full, the
 * entries not used during the last half of the capacity are dropped, so
 * the prepared statements a session keeps using stay.
 */
#define PGA_FEATURE_CACHE_SIZE 4096

typedef struct PgaFeatureEntry
{
	uint64		query_id;		/* hash key */
	uint64		last_used;		/* feature_clock of the last use */
	PgaFeatures features;
} PgaFeatureEntry;

static HTAB *feature_cache = NULL;
static uint64 feature_clock = 0;

/* walker state */
typedef struct PgaWalkContext
{
	PgaFeatures features;
	int			null_eq;
	int			cartesian;
	int			not_in;
	int			update_no_where;
	int			delete_no_where;
} PgaWalkContext;

/* Local wall-clock time of a statement, see pga_local_time() */
typedef struct PgaLocalTime
{
	int			hour;			/* -1 if the time could not be converted */
	int			minute;
	int			dow;			/* 0 = Sunday */
	int			mon;			/* 1..12 */
	int			mday;
	int32		day;			/* day number as returned by date2j() */
} PgaLocalTime;

#define PGA_GUARD_BEGIN \
	do { \
		MemoryContext pga_guard_cxt = CurrentMemoryContext; \
		PG_TRY(); \
		{

#define PGA_GUARD_END \
		} \
		PG_CATCH(); \
		{ \
			ErrorData  *pga_edata; \
			MemoryContextSwitchTo(pga_guard_cxt); \
			pga_edata = CopyErrorData(); \
			FlushErrorState(); \
			if (pga_shared != NULL && LWLockHeldByMe(pga_shared->lock)) \
				LWLockRelease(pga_shared->lock); \
			pga_disable_session(pga_edata); \
			FreeErrorData(pga_edata); \
		} \
		PG_END_TRY(); \
	} while (0)

/*
 * Is the current nesting level counted: top level, the statement of a
 * top-level PREPARE or EXECUTE, or anything with track_nested?
 */
static inline bool
pga_level_tracked(void)
{
	return nesting_level == 0 || nesting_level == execute_level ||
		pga_track_nested;
}

/* Is the current nesting level that of a top-level statement? */
static inline bool
pga_level_top(void)
{
	return nesting_level == 0 || nesting_level == execute_level;
}

/* Pending features of the statement at the current nesting level. */
static inline PgaPending *
pga_pending_slot(void)
{
	return &pending[Min(nesting_level, PGA_MAX_NESTING - 1)];
}

/*
 * Local wall-clock time of a timestamp, in the session time zone.  All
 * fields but the minute are cached for the current hour to keep the hooks
 * cheap; the minute is derived from the offset into that hour.
 */
static void
pga_local_time(TimestampTz ts, PgaLocalTime *lt)
{
	static TimestampTz hour_start = 0;
	static TimestampTz hour_end = 0;
	static PgaLocalTime cached_lt = {-1};
	static pg_tz *c_tz = NULL;
	struct pg_tm tm;
	fsec_t		fsec;
	int			tz;

	if (!(ts >= hour_start && ts < hour_end && c_tz == session_timezone))
	{
		if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, NULL) != 0)
		{
			memset(lt, 0, sizeof(*lt));
			lt->hour = -1;
			return;
		}

		cached_lt.hour = tm.tm_hour;
		cached_lt.day = date2j(tm.tm_year, tm.tm_mon, tm.tm_mday);
		cached_lt.dow = j2day(cached_lt.day);
		cached_lt.mon = tm.tm_mon;
		cached_lt.mday = tm.tm_mday;
		c_tz = session_timezone;
		hour_start = ts - ((int64) tm.tm_min * SECS_PER_MINUTE + tm.tm_sec) * USECS_PER_SEC - fsec;
		hour_end = hour_start + USECS_PER_HOUR;
	}

	*lt = cached_lt;
	lt->minute = (int) ((ts - hour_start) / USECS_PER_MINUTE);
}

/* time of the current statement, faked in test builds */
static TimestampTz
pga_statement_time(void)
{
#ifdef PGA_TESTING
	if (pga_test_now != NULL && pga_test_now[0] != '\0')
		return pga_now();
#endif
	return GetCurrentStatementStartTimestamp();
}

/* Statement started between 03:00 and 05:00 local time. */
static bool
pga_is_night(const PgaLocalTime *lt)
{
	return lt->hour >= 3 && lt->hour < 5;
}

/* Statement started in the very first minute of a year, local time. */
static bool
pga_is_new_year(const PgaLocalTime *lt)
{
	return lt->mon == 1 && lt->mday == 1 && lt->hour == 0 && lt->minute == 0;
}

/* Statement started on a Friday at 18:00 or later, local time. */
static bool
pga_is_friday_evening(void)
{
	PgaLocalTime lt;

	pga_local_time(pga_statement_time(), &lt);
	return lt.dow == 5 && lt.hour >= 18;
}

/* One of the JSONB operators counted by jsonb_ops. */
static bool
pga_is_jsonb_op(Oid opno)
{
	switch (opno)
	{
		case OID_JSONB_OBJECT_FIELD:
		case OID_JSONB_ARRAY_ELEMENT:
		case OID_JSONB_OBJECT_FIELD_TEXT:
		case OID_JSONB_ARRAY_ELEMENT_TEXT:
		case OID_JSONB_CONTAINS:
		case OID_JSONB_EXISTS:
			return true;
		default:
			return false;
	}
}

/* "x = NULL": an equality operator with a NULL constant argument */
static bool
pga_is_null_equality(OpExpr *op)
{
	ListCell   *lc;
	bool		has_null = false;
	char	   *name;
	bool		result;

	if (list_length(op->args) != 2)
		return false;
	foreach(lc, op->args)
	{
		Node	   *arg = (Node *) lfirst(lc);

		while (arg && IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (arg && IsA(arg, Const) && ((Const *) arg)->constisnull)
			has_null = true;
	}
	if (!has_null)
		return false;

	name = get_opname(op->opno);
	result = (name != NULL && strcmp(name, "=") == 0);
	if (name)
		pfree(name);
	return result;
}

/* Range table entry of a plain table reference in a join tree, or NULL. */
static RangeTblEntry *
pga_plain_relation(Query *q, Node *item)
{
	RangeTblEntry *rte;

	if (item == NULL || !IsA(item, RangeTblRef))
		return NULL;
	rte = rt_fetch(((RangeTblRef *) item)->rtindex, q->rtable);
	return (rte->rtekind == RTE_RELATION && !rte->lateral) ? rte : NULL;
}

/*
 * Does the query join tables without any condition at all: "FROM a, b" or
 * "a CROSS JOIN b" with no WHERE clause?  Only plain tables count, so that
 * "FROM t, LATERAL f(t.x)" or "FROM t, generate_series(...)" do not.
 */
static bool
pga_is_cartesian(Query *q)
{
	ListCell   *lc;
	int			tables = 0;

	if (q->jointree == NULL || q->jointree->quals != NULL)
		return false;

	foreach(lc, q->jointree->fromlist)
	{
		Node	   *item = (Node *) lfirst(lc);

		if (pga_plain_relation(q, item) != NULL)
			tables++;
		else if (IsA(item, JoinExpr))
		{
			JoinExpr   *j = (JoinExpr *) item;

			if (j->jointype == JOIN_INNER && j->quals == NULL &&
				pga_plain_relation(q, j->larg) != NULL &&
				pga_plain_relation(q, j->rarg) != NULL)
				return true;
		}
	}
	return tables >= 2;
}

/*
 * Walk the analyzed query, including subqueries, CTEs and sublinks, and
 * collect the features counted by jsonb_ops, join_count_max, null_eq,
 * terminate_backend, listen_notify, cartesian_product, not_in_subquery and
 * the per-query flags.
 */
static bool
pga_walker(Node *node, void *context)
{
	PgaWalkContext *ctx = (PgaWalkContext *) context;

	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *q = (Query *) node;
		ListCell   *lc;

		PgaFeatures *f = &ctx->features;

		if (q->cteList != NIL)
			f->flags |= PF_CTE;
		if (q->hasRecursive)
			f->flags |= PF_RECURSIVE;
		if (q->hasWindowFuncs)
			f->flags |= PF_WINDOW;
		if (q->onConflict != NULL)
			f->flags |= PF_UPSERT;
		if (q->returningList != NIL)
			f->flags |= PF_RETURNING;
		if (q->commandType == CMD_MERGE)
			f->flags |= PF_MERGE;
		if (q->groupingSets != NIL)
			f->flags |= PF_GROUPING;
		if (pga_is_cartesian(q))
			ctx->cartesian++;

		/* also inside a CTE: WITH x AS (UPDATE t SET ...) SELECT ... */
		if (q->commandType == CMD_UPDATE && q->jointree->quals == NULL)
			ctx->update_no_where++;
		if (q->commandType == CMD_DELETE && q->jointree->quals == NULL)
			ctx->delete_no_where++;

		foreach(lc, q->rowMarks)
		{
			RowMarkClause *rc = (RowMarkClause *) lfirst(lc);

			if (rc->waitPolicy == LockWaitSkip)
				f->flags |= PF_SKIP_LOCKED;
		}

		foreach(lc, q->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_JOIN)
				f->joins++;
			if (rte->lateral)
				f->flags |= PF_LATERAL;
		}

		return query_tree_walker(q, pga_walker, context, 0);
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		if (pga_is_jsonb_op(op->opno))
			ctx->features.jsonb_ops++;
		else if (pga_is_null_equality(op))
			ctx->null_eq++;
	}
	else if (IsA(node, FuncExpr))
	{
		Oid			funcid = ((FuncExpr *) node)->funcid;

		if (funcid == F_PG_TERMINATE_BACKEND)
			ctx->features.terminate++;
		else if (funcid == F_PG_NOTIFY)
			ctx->features.notify++;
	}
	else if (IsA(node, BoolExpr) && ((BoolExpr *) node)->boolop == NOT_EXPR)
	{
		Node	   *arg = (Node *) linitial(((BoolExpr *) node)->args);

		/* "x NOT IN (SELECT ...)" is parsed as NOT (x = ANY (SELECT ...)) */
		if (IsA(arg, SubLink) && ((SubLink *) arg)->subLinkType == ANY_SUBLINK)
			ctx->not_in++;
	}

	return expression_tree_walker(node, pga_walker, context);
}

/*
 * After parse analysis "*" is already expanded into columns, but the Vars
 * created by the expansion keep the location of the "*" (or "t.*") in the
 * source text.  The text is only inspected, never stored.
 */
static bool
pga_has_select_star(ParseState *pstate, Query *query)
{
	const char *src = pstate->p_sourcetext;
	ListCell   *lc;
	size_t		srclen;

	if (src == NULL)
		return false;
	srclen = strlen(src);

	foreach(lc, query->targetList)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Var		   *var;
		const char *p;

		if (te->resjunk || te->expr == NULL || !IsA(te->expr, Var))
			continue;
		var = (Var *) te->expr;
		if (var->location < 0 || (size_t) var->location >= srclen)
			continue;

		/* skip an optional qualifier such as t. or "T". */
		p = src + var->location;
		while (*p && (isalnum((unsigned char) *p) || *p == '_' || *p == '$' ||
					  *p == '"' || *p == '.' || IS_HIGHBIT_SET(*p)))
			p++;
		if (*p == '*')
			return true;
	}
	return false;
}

/* Drop the cached features not used lately, see PgaFeatureEntry. */
static void
pga_trim_feature_cache(void)
{
	HASH_SEQ_STATUS status;
	PgaFeatureEntry *entry;

	hash_seq_init(&status, feature_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (entry->last_used + PGA_FEATURE_CACHE_SIZE / 2 <= feature_clock)
			(void) hash_search(feature_cache, &entry->query_id, HASH_REMOVE, NULL);
	}
}

/* Remember the features of an analyzed query under its query id. */
static void
pga_remember_features(uint64 query_id, const PgaFeatures *features)
{
	PgaFeatureEntry *entry;

	if (query_id == UINT64CONST(0))
		return;

	if (feature_cache == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(PgaFeatureEntry);
		ctl.hcxt = TopMemoryContext;
		feature_cache = hash_create("pg_achievements query features", 256,
									&ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	else if (hash_get_num_entries(feature_cache) >= PGA_FEATURE_CACHE_SIZE)
		pga_trim_feature_cache();

	entry = hash_search(feature_cache, &query_id, HASH_ENTER, NULL);
	entry->last_used = ++feature_clock;
	entry->features = *features;
}

/* Features remembered for a query id, or NULL. */
static const PgaFeatures *
pga_lookup_features(uint64 query_id)
{
	PgaFeatureEntry *entry;

	if (query_id == UINT64CONST(0) || feature_cache == NULL)
		return NULL;
	entry = hash_search(feature_cache, &query_id, HASH_FIND, NULL);
	if (entry == NULL)
		return NULL;
	entry->last_used = ++feature_clock;
	return &entry->features;
}

/*
 * post_parse_analyze_hook: inspect every analyzed statement.  Attempt
 * counters are bumped here; the structural features are kept as pending
 * (and by query id) until the statement succeeds.
 */
static void
pga_post_parse_analyze(ParseState *pstate, Query *query, PgaJumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	if (!pga_tracking_active() || !pga_level_tracked())
		return;

	PGA_GUARD_BEGIN;
	{
		PgaPending *p = pga_pending_slot();

#ifdef PGA_TESTING
		if (pga_test_inject_error)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("pg_achievements: injected error")));
#endif

		memset(p, 0, sizeof(PgaPending));

		if (pga_sync_role())
		{
			p->valid = true;
			p->query_id = query->queryId;

			if (query->commandType != CMD_UTILITY)
			{
				PgaWalkContext ctx;

				memset(&ctx, 0, sizeof(ctx));
				(void) pga_walker((Node *) query, &ctx);

				p->features = ctx.features;
				if (query->commandType == CMD_SELECT)
					p->features.flags |= PF_SELECT;
				pga_remember_features(query->queryId, &p->features);

				/* counted immediately: the attempt is what matters */
				if (ctx.update_no_where > 0)
					pga_count(PGA_C_UPDATE_NO_WHERE, 1);
				if (ctx.delete_no_where > 0)
					pga_count(PGA_C_DELETE_NO_WHERE, 1);
				if (ctx.null_eq > 0)
					pga_count(PGA_C_NULL_EQ, 1);
				if (ctx.cartesian > 0)
					pga_count(PGA_C_CARTESIAN_PRODUCT, 1);
				if (ctx.not_in > 0)
					pga_count(PGA_C_NOT_IN_SUBQUERY, 1);
				if (query->commandType == CMD_SELECT &&
					pga_has_select_star(pstate, query))
					pga_count(PGA_C_SELECT_STAR, 1);
			}
		}
	}
	PGA_GUARD_END;
}

/*
 * Count a statement that has just succeeded: its structural features (none
 * for a utility command), and what depends on the time it started.
 */
static void
pga_count_success(const PgaFeatures *f)
{
	PgaLocalTime lt;
	uint32		flags = f->flags;

	if (flags & PF_SELECT)
		pga_count(PGA_C_STMT_SELECT, 1);
	if (flags & PF_CTE)
		pga_count(PGA_C_CTE_ANY, 1);
	if (flags & PF_RECURSIVE)
		pga_count(PGA_C_CTE_RECURSIVE, 1);
	if (flags & PF_WINDOW)
		pga_count(PGA_C_WINDOW_FUNC, 1);
	if (flags & PF_LATERAL)
		pga_count(PGA_C_LATERAL, 1);
	if (f->jsonb_ops > 0)
		pga_count(PGA_C_JSONB_OPS, f->jsonb_ops);
	if (f->joins > 0)
		pga_count_max(PGA_C_JOIN_COUNT_MAX, f->joins);
	if (f->terminate > 0)
		pga_count(PGA_C_TERMINATE_BACKEND, f->terminate);
	if (flags & PF_UPSERT)
		pga_count(PGA_C_UPSERT, 1);
	if (flags & PF_RETURNING)
		pga_count(PGA_C_RETURNING, 1);
	if (flags & PF_MERGE)
		pga_count(PGA_C_MERGE, 1);
	if (flags & PF_SKIP_LOCKED)
		pga_count(PGA_C_SKIP_LOCKED, 1);
	if (flags & PF_GROUPING)
		pga_count(PGA_C_GROUPING_SETS, 1);
	if (f->notify > 0)
		pga_count(PGA_C_LISTEN_NOTIFY, f->notify);

	pga_local_time(pga_statement_time(), &lt);
	if (lt.hour < 0)
		return;
	if (pga_is_night(&lt))
		pga_count(PGA_C_NIGHT_QUERY, 1);
	if (pga_is_new_year(&lt))
		pga_count(PGA_C_NEW_YEAR_QUERY, 1);
	pga_note_active_day(lt.day);
}

/* Count the pending statement of a nesting level, if not done yet. */
static void
pga_apply_pending(PgaPending *p)
{
	if (!p->valid)
		return;
	p->valid = false;
	pga_count_success(&p->features);
}

/*
 * Count a statement the executor has finished.  It is usually the one
 * analyzed last at this level; otherwise (a prepared statement analyzed in
 * an earlier transaction) its features are looked up by query id.  If
 * they are unknown, only what depends on the time is counted.
 */
static void
pga_apply_executed(PlannedStmt *stmt)
{
	PgaPending *p = pga_pending_slot();
	const PgaFeatures *f;
	PgaFeatures none;

	if (p->valid && p->query_id == stmt->queryId)
	{
		pga_apply_pending(p);
		return;
	}
	if (!pga_sync_role())
		return;

	f = pga_lookup_features(stmt->queryId);
	if (f == NULL)
	{
		memset(&none, 0, sizeof(none));
		f = &none;
	}
	pga_count_success(f);
}

/*
 * Find a sequential scan of a big table (reltuples >= big_table_rows) in
 * the plan state tree; stops at the first one.
 */
static bool
pga_seqscan_walker(PlanState *ps, void *context)
{
	if (ps == NULL)
		return false;

	if (IsA(ps, SeqScanState))
	{
		Relation	rel = ((ScanState *) ps)->ss_currentRelation;

		if (rel != NULL && rel->rd_rel->reltuples >= (float4) pga_big_table_rows)
		{
			*(bool *) context = true;
			return true;
		}
	}
	return planstate_tree_walker(ps, pga_seqscan_walker, context);
}

#if PG_VERSION_NUM >= 180000
static void
pga_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
#else
static void
pga_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				bool execute_once)
#endif
{
	nesting_level++;
	PG_TRY();
	{
#if PG_VERSION_NUM >= 180000
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
#else
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

static void
pga_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

static void
pga_ExecutorEnd(QueryDesc *queryDesc)
{
	if (pga_tracking_active() && pga_level_tracked())
	{
		PGA_GUARD_BEGIN;
		{
			bool		big_seqscan = false;

			pga_apply_executed(queryDesc->plannedstmt);

			if (queryDesc->planstate != NULL)
			{
				(void) pga_seqscan_walker(queryDesc->planstate, &big_seqscan);
				if (big_seqscan)
					pga_count(PGA_C_SEQSCAN_BIG, 1);
			}

			/* rows returned by a SELECT, or affected by a DML statement */
			if (queryDesc->estate != NULL && queryDesc->estate->es_processed > 0)
				pga_count(PGA_C_ROWS_PROCESSED,
						  (int64) Min(queryDesc->estate->es_processed,
									  (uint64) PG_INT64_MAX));

			if (pga_level_top())
			{
				TimestampTz now = GetCurrentTimestamp();
				int64		duration;

				duration = now - GetCurrentStatementStartTimestamp() +
					pga_duration_shift_us();
				if (duration >= 60 * USECS_PER_SEC)
					pga_count(PGA_C_QUERY_OVER_60S, 1);
				if (duration >= USECS_PER_HOUR)
					pga_count(PGA_C_QUERY_OVER_1H, 1);

				pga_maybe_flush_by_time(now);
			}
		}
		PGA_GUARD_END;
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/* What a utility statement counts once it has succeeded. */
typedef struct PgaUtilityEvents
{
	int			n;
	PgaCounter	counters[8];
	int64		amounts[8];
} PgaUtilityEvents;

/* Remember a counter to bump after the utility command succeeds. */
static void
pga_add_event(PgaUtilityEvents *ev, PgaCounter c, int64 amount)
{
	if (ev->n < lengthof(ev->counters))
	{
		ev->counters[ev->n] = c;
		ev->amounts[ev->n] = amount;
		ev->n++;
	}
}

/*
 * Value of a boolean option such as EXPLAIN (ANALYZE) or VACUUM (FULL).
 *
 * This runs before the command, on a parse tree nobody has validated yet,
 * so it must never throw: defGetBoolean() would turn a typo of the user,
 * such as EXPLAIN (ANALYZE maybe), into an "internal error" that disables
 * tracking for the whole session.  An invalid value counts as off; the
 * command itself reports the error.  As in the commands, the last
 * occurrence of an option wins.
 */
static bool
pga_option_enabled(List *options, const char *name)
{
	ListCell   *lc;
	bool		result = false;

	foreach(lc, options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);
		bool		value;

		if (strcmp(opt->defname, name) != 0)
			continue;

		if (opt->arg == NULL)
			result = true;		/* bare option: EXPLAIN (ANALYZE) */
		else if (IsA(opt->arg, Integer))
			result = intVal(opt->arg) == 1;
		else if (IsA(opt->arg, Boolean))
			result = boolVal(opt->arg);
		else if (IsA(opt->arg, String))
			result = parse_bool(strVal(opt->arg), &value) && value;
		else
			result = false;
	}
	return result;
}

/*
 * RangeVar of a possibly qualified name list, or NULL if the name has more
 * than three parts.  makeRangeVarFromNameList() throws in that case, and
 * this runs before the command has validated its own parse tree.
 */
static RangeVar *
pga_rangevar_from_names(List *names)
{
	if (list_length(names) < 1 || list_length(names) > 3)
		return NULL;
	return makeRangeVarFromNameList(names);
}

/*
 * Does a relation exist?  Never throws: a cross-database reference, which
 * RangeVarGetRelid() refuses with an error, simply does not exist here (the
 * command fails on it anyway).
 */
static bool
pga_relation_exists(RangeVar *rv)
{
	if (rv == NULL || rv->relname == NULL)
		return false;
	if (rv->catalogname != NULL &&
		strcmp(rv->catalogname, get_database_name(MyDatabaseId)) != 0)
		return false;
	return OidIsValid(RangeVarGetRelid(rv, NoLock, true));
}

/*
 * Would CREATE INDEX IF NOT EXISTS find its index already there?  The index
 * lives in the schema of its table.
 */
static bool
pga_index_exists(IndexStmt *stmt)
{
	Oid			relid;

	if (stmt->idxname == NULL || !pga_relation_exists(stmt->relation))
		return false;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	return OidIsValid(get_relname_relid(stmt->idxname, get_rel_namespace(relid)));
}

/*
 * Log level of a utility command, as GetCommandLogLevel() computes it, but
 * without throwing: for EXPLAIN that function parses the ANALYZE option
 * with defGetBoolean().  Only EXPLAIN ANALYZE of a DDL statement (CREATE
 * TABLE AS) is DDL, as there.
 */
static LogStmtLevel
pga_command_log_level(Node *parsetree)
{
	if (IsA(parsetree, ExplainStmt))
	{
		ExplainStmt *stmt = (ExplainStmt *) parsetree;

		if (!pga_option_enabled(stmt->options, "analyze"))
			return LOGSTMT_ALL;
		return pga_command_log_level(stmt->query);
	}
	return GetCommandLogLevel(parsetree);
}

/*
 * Does a name contain an emoji?  Only the common emoji blocks are checked:
 * Miscellaneous Symbols and Dingbats (U+2600..U+27BF) and everything from
 * Mahjong Tiles to Symbols and Pictographs Extended-A (U+1F000..U+1FAFF).
 */
static bool
pga_has_emoji(const char *name)
{
	const unsigned char *p = (const unsigned char *) name;

	if (name == NULL || GetDatabaseEncoding() != PG_UTF8)
		return false;

	while (*p)
	{
		int			len = pg_utf_mblen(p);
		pg_wchar	cp;

		if (len > 1 && pg_utf8_islegal(p, len))
		{
			cp = utf8_to_unicode(p);
			if ((cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x1F000 && cp <= 0x1FAFF))
				return true;
		}
		/* never step over the terminating zero */
		for (int i = 0; i < len && *p; i++)
			p++;
	}
	return false;
}

/*
 * Was a part of the (possibly qualified) name that starts at "location" in
 * the source text longer than NAMEDATALEN - 1 bytes?  The parser silently
 * truncates such identifiers (with a NOTICE that never reaches the log, so
 * emit_log_hook does not see it), and only the source text still has the
 * full name.  The text is only inspected, never stored.
 */
static bool
pga_name_truncated(const char *src, int location)
{
	const char *p;

	if (src == NULL || location < 0 || (size_t) location >= strlen(src))
		return false;

	p = src + location;
	for (;;)
	{
		int			len = 0;

		if (*p == '"')
		{
			/* quoted identifier, "" stands for one double quote */
			for (p++; *p; p++)
			{
				if (*p == '"')
				{
					if (p[1] != '"')
						break;
					p++;
				}
				len++;
			}
			if (*p != '"')
				return false;	/* unterminated: not our business */
			p++;
		}
		else
		{
			while (*p && (isalnum((unsigned char) *p) || *p == '_' || *p == '$' ||
						  IS_HIGHBIT_SET(*p)))
			{
				p++;
				len++;
			}
		}

		if (len >= NAMEDATALEN)
			return true;

		while (isspace((unsigned char) *p))
			p++;
		if (*p != '.')
			return false;
		p++;
		while (isspace((unsigned char) *p))
			p++;
	}
}

/* Count a table name with an emoji, or one that got truncated. */
static void
pga_check_table_name(RangeVar *rv, const char *queryString,
					 PgaUtilityEvents *ev)
{
	if (rv == NULL)
		return;
	if (pga_has_emoji(rv->relname))
		pga_add_event(ev, PGA_C_EMOJI_NAME, 1);
	if (pga_name_truncated(queryString, rv->location))
		pga_add_event(ev, PGA_C_LONG_NAME, 1);
}

/* ALTER SYSTEM SET pg_achievements.enabled = off (or false, 0, ...) */
static bool
pga_is_disable_attempt(AlterSystemStmt *stmt)
{
	VariableSetStmt *set = stmt->setstmt;
	A_Const    *con;
	bool		value;

	if (set == NULL || set->kind != VAR_SET_VALUE || set->name == NULL ||
		pg_strcasecmp(set->name, "pg_achievements.enabled") != 0 ||
		list_length(set->args) != 1 || !IsA(linitial(set->args), A_Const))
		return false;

	con = (A_Const *) linitial(set->args);
	if (con->isnull)
		return false;
	if (IsA(&con->val, String))
		return parse_bool(strVal(&con->val), &value) && !value;
	if (IsA(&con->val, Integer))
		return intVal(&con->val) == 0;
	return false;
}

/*
 * Decide, before execution, which counters a utility command bumps.  It
 * must run before the command: IF EXISTS checks need the catalog state
 * before the DROP, and the parse tree may be gone afterwards.
 *
 * The parse tree has not been validated yet, so nothing here may throw on
 * bad input (see pga_option_enabled): an error would disable tracking for
 * the session, although only the command of the user is wrong.
 */
static void
pga_classify_utility(Node *parsetree, const char *queryString,
					 PgaUtilityEvents *ev)
{
	switch (nodeTag(parsetree))
	{
		case T_ExplainStmt:
			pga_add_event(ev, PGA_C_EXPLAIN_ANY, 1);
			if (pga_option_enabled(((ExplainStmt *) parsetree)->options, "analyze"))
				pga_add_event(ev, PGA_C_EXPLAIN_ANALYZE, 1);
			break;

		case T_CreateStmt:
			{
				CreateStmt *stmt = (CreateStmt *) parsetree;

				/* CREATE TABLE IF NOT EXISTS of an existing table: nothing new */
				if (stmt->if_not_exists && pga_relation_exists(stmt->relation))
					break;
				pga_add_event(ev, PGA_C_CREATE_TABLE, 1);
				if (stmt->partspec != NULL || stmt->partbound != NULL)
					pga_add_event(ev, PGA_C_PARTITION_CREATE, 1);
				pga_check_table_name(stmt->relation, queryString, ev);
			}
			break;

		case T_CreateTableAsStmt:
			{
				CreateTableAsStmt *stmt = (CreateTableAsStmt *) parsetree;

				if (stmt->objtype == OBJECT_TABLE &&
					!(stmt->if_not_exists && pga_relation_exists(stmt->into->rel)))
				{
					pga_add_event(ev, PGA_C_CREATE_TABLE, 1);
					pga_check_table_name(stmt->into->rel, queryString, ev);
				}
			}
			break;

		case T_IndexStmt:
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				/* CREATE INDEX IF NOT EXISTS of an existing index */
				if (stmt->if_not_exists && pga_index_exists(stmt))
					break;
				pga_add_event(ev, PGA_C_CREATE_INDEX, 1);
				if (stmt->concurrent)
					pga_add_event(ev, PGA_C_CREATE_INDEX_CONCURRENTLY, 1);
			}
			break;

		case T_CreateFunctionStmt:
			pga_add_event(ev, PGA_C_CREATE_FUNCTION, 1);
			break;

		case T_CreateTrigStmt:
			pga_add_event(ev, PGA_C_CREATE_TRIGGER, 1);
			break;

		case T_CreateExtensionStmt:
			{
				CreateExtensionStmt *stmt = (CreateExtensionStmt *) parsetree;

				if (!OidIsValid(get_extension_oid(stmt->extname, true)))
					pga_add_event(ev, PGA_C_CREATE_EXTENSION, 1);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (stmt->removeType == OBJECT_TABLE)
				{
					ListCell   *lc;
					int			n = 0;

					/* with IF EXISTS, count only the tables that exist */
					foreach(lc, stmt->objects)
					{
						RangeVar   *rv = pga_rangevar_from_names((List *) lfirst(lc));

						if (pga_relation_exists(rv))
							n++;
					}
					pga_add_event(ev, PGA_C_DROP_TABLE, n);
				}
			}
			break;

		case T_DropdbStmt:
			if (OidIsValid(get_database_oid(((DropdbStmt *) parsetree)->dbname, true)))
				pga_add_event(ev, PGA_C_DROP_DATABASE, 1);
			break;

		case T_TruncateStmt:
			pga_add_event(ev, PGA_C_TRUNCATE, 1);
			break;

		case T_VacuumStmt:
			{
				VacuumStmt *stmt = (VacuumStmt *) parsetree;

				if (stmt->is_vacuumcmd && pga_option_enabled(stmt->options, "full"))
					pga_add_event(ev, PGA_C_VACUUM_FULL, 1);
			}
			break;

		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				/* a ROLLBACK outside of a transaction block only warns */
				if (stmt->kind == TRANS_STMT_ROLLBACK && IsTransactionBlock())
					pga_add_event(ev, PGA_C_ROLLBACK, 1);
				else if (stmt->kind == TRANS_STMT_ROLLBACK_TO)
					pga_add_event(ev, PGA_C_ROLLBACK_TO_SAVEPOINT, 1);
			}
			break;

		case T_CommentStmt:
			pga_add_event(ev, PGA_C_COMMENT_ON, 1);
			break;

		case T_RefreshMatViewStmt:
			if (((RefreshMatViewStmt *) parsetree)->concurrent)
				pga_add_event(ev, PGA_C_REFRESH_MATVIEW_CONCURRENTLY, 1);
			break;

		case T_CreatePolicyStmt:
			pga_add_event(ev, PGA_C_CREATE_POLICY, 1);
			break;

		case T_ListenStmt:
		case T_NotifyStmt:
			pga_add_event(ev, PGA_C_LISTEN_NOTIFY, 1);
			break;

		case T_LockStmt:
			if (((LockStmt *) parsetree)->mode == AccessExclusiveLock)
				pga_add_event(ev, PGA_C_LOCK_EXCLUSIVE, 1);
			break;

		case T_AlterSystemStmt:
			if (pga_is_disable_attempt((AlterSystemStmt *) parsetree))
				pga_add_event(ev, PGA_C_DISABLE_ATTEMPT, 1);
			break;

		default:
			break;
	}

	if (pga_command_log_level(parsetree) == LOGSTMT_DDL && pga_is_friday_evening())
		pga_add_event(ev, PGA_C_DDL_FRIDAY_EVENING, 1);
}

static void
pga_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	PgaUtilityEvents ev;
	volatile bool track = false;	/* set inside PG_TRY, read after it */
	int			level = nesting_level;
	int			saved_execute_level = execute_level;
	bool		is_execute = IsA(pstmt->utilityStmt, ExecuteStmt);
	bool		is_prepare = IsA(pstmt->utilityStmt, PrepareStmt);
	bool		top = pga_level_top();

	ev.n = 0;
	if (pga_tracking_active() && pga_level_tracked())
	{
		PGA_GUARD_BEGIN;
		{
			pga_classify_utility(pstmt->utilityStmt, queryString, &ev);
			track = true;
		}
		PGA_GUARD_END;
	}

	nesting_level++;

	/*
	 * PREPARE and EXECUTE are how a statement is sent, so the statement
	 * counts as if it had been sent at their level, as with the protocol
	 * messages Parse and Execute, which do not nest: PREPARE analyzes it
	 * (attempt counters, and its features for later executions), EXECUTE
	 * runs it.
	 */
	if ((is_execute || is_prepare) && top)
		execute_level = nesting_level;

	PG_TRY();
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		nesting_level--;
		execute_level = saved_execute_level;
	}
	PG_END_TRY();

	if (track && pga_tracking_active())
	{
		PGA_GUARD_BEGIN;
		{
			PgaPending *p = &pending[Min(level, PGA_MAX_NESTING - 1)];

			/* SET ROLE and friends switch the role events are counted for */
			(void) pga_sync_role();

			for (int i = 0; i < ev.n; i++)
				pga_count(ev.counters[i], ev.amounts[i]);
			if (is_execute)
				p->valid = false;	/* the statement it ran has counted */
			else
				pga_apply_pending(p);

			if (level == 0)
				pga_maybe_flush_by_time(GetCurrentTimestamp());
		}
		PGA_GUARD_END;
	}
}

/*
 * emit_log_hook: count syntax errors, deadlocks, serialization failures,
 * missing tables and divisions by zero of this backend.
 */
static void
pga_emit_log(ErrorData *edata)
{
	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);

	/* no allocation, no locks, no ereport here */
	if (edata->elevel == ERROR && pga_tracking_active())
		pga_count_error(edata->sqlerrcode);
}

/* ClientAuthentication_hook: note a successful login. */
static void
pga_client_authentication(Port *port, int status)
{
	if (prev_ClientAuthentication)
		prev_ClientAuthentication(port, status);

	/*
	 * The client is not ready for notices yet, and per-role settings such
	 * as opt_out are applied later: just remember to count the connection
	 * when the startup transaction commits.
	 */
	if (status == STATUS_OK)
		startup_pending = true;
}

/*
 * Transaction callback: count the connection at the end of the startup
 * transaction, check for rollbacks of old transactions, and flush local
 * deltas at every commit and abort.
 */
static void
pga_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (startup_pending)
			{
				startup_pending = false;
				if (pga_tracking_active())
				{
					PGA_GUARD_BEGIN;
					{
						if (pga_sync_role())
							pga_count(PGA_C_CONNECT, 1);
					}
					PGA_GUARD_END;
				}
			}
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PREPARE:
			memset(pending, 0, sizeof(pending));
			if (pga_shared != NULL && !pga_session_disabled)
			{
				PGA_GUARD_BEGIN;
				{
					pga_flush_local(pga_tracking_active());
				}
				PGA_GUARD_END;
			}
			break;

		case XACT_EVENT_ABORT:
			memset(pending, 0, sizeof(pending));
			if (pga_shared != NULL && !pga_session_disabled)
			{
				PGA_GUARD_BEGIN;
				{
					if (pga_tracking_active())
					{
						int64		age;

						age = GetCurrentTimestamp() -
							GetCurrentTransactionStartTimestamp() +
							pga_duration_shift_us();
						if (age >= USECS_PER_HOUR)
							pga_count(PGA_C_ROLLBACK_LONG_XACT, 1);
					}
					pga_flush_local(pga_tracking_active());
				}
				PGA_GUARD_END;
			}
			break;

		default:
			break;
	}
}

/*
 * Chain all hooks; previous hooks are always called, so other extensions
 * (pg_stat_statements, auto_explain) keep working in any preload order.
 */
void
pga_install_hooks(void)
{
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pga_post_parse_analyze;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pga_ProcessUtility;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pga_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pga_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pga_ExecutorEnd;
	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = pga_emit_log;
	prev_ClientAuthentication = ClientAuthentication_hook;
	ClientAuthentication_hook = pga_client_authentication;

	RegisterXactCallback(pga_xact_callback, NULL);
}
