/*-------------------------------------------------------------------------
 *
 * hooks.c
 *		Event detection: parse analysis, utility commands, executor, errors,
 *		transactions and authentication.
 *
 * Rules:
 *	- only top-level statements are counted unless track_nested is on;
 *	- update_no_where, delete_no_where, null_eq and select_star are counted
 *	  at parse analysis (the attempt matters); everything else derived from a
 *	  query is kept as "pending" and counted only when the statement finishes
 *	  successfully (ExecutorEnd or the end of ProcessUtility);
 *	- our own code runs inside PG_TRY: an internal failure prints a single
 *	  WARNING, disables tracking for the session and never breaks the user's
 *	  query.  Errors of the hooked functions themselves propagate unchanged.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "tcop/utility.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

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

/* features of an analyzed statement, counted when it succeeds */
#define PGA_MAX_NESTING 32

#define PF_CTE			0x0001
#define PF_RECURSIVE	0x0002
#define PF_WINDOW		0x0004
#define PF_LATERAL		0x0008
#define PF_NIGHT		0x0010
#define PF_SELECT		0x0020

typedef struct PgaPending
{
	bool		valid;
	uint32		flags;
	int			joins;
	int			jsonb_ops;
	int			terminate;
} PgaPending;

static PgaPending pending[PGA_MAX_NESTING];

/* walker state */
typedef struct PgaWalkContext
{
	uint32		flags;
	int			joins;
	int			jsonb_ops;
	int			terminate;
	int			null_eq;
} PgaWalkContext;

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

static inline bool
pga_level_tracked(void)
{
	return nesting_level == 0 || pga_track_nested;
}

static inline PgaPending *
pga_pending_slot(void)
{
	return &pending[Min(nesting_level, PGA_MAX_NESTING - 1)];
}

/*
 * Local wall-clock hour and day of week of a timestamp, in the session time
 * zone.  The result is cached for the current hour to keep the hooks cheap.
 */
static void
pga_local_time(TimestampTz ts, int *hour, int *dow)
{
	static TimestampTz hour_start = 0;
	static TimestampTz hour_end = 0;
	static int	c_hour = -1;
	static int	c_dow = -1;
	static pg_tz *c_tz = NULL;
	struct pg_tm tm;
	fsec_t		fsec;
	int			tz;

	if (ts >= hour_start && ts < hour_end && c_tz == session_timezone)
	{
		*hour = c_hour;
		*dow = c_dow;
		return;
	}

	if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, NULL) != 0)
	{
		*hour = -1;
		*dow = -1;
		return;
	}

	c_hour = tm.tm_hour;
	c_dow = j2day(date2j(tm.tm_year, tm.tm_mon, tm.tm_mday));
	c_tz = session_timezone;
	hour_start = ts - ((int64) tm.tm_min * SECS_PER_MINUTE + tm.tm_sec) * USECS_PER_SEC - fsec;
	hour_end = hour_start + USECS_PER_HOUR;

	*hour = c_hour;
	*dow = c_dow;
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

static bool
pga_is_night(void)
{
	int			hour,
				dow;

	pga_local_time(pga_statement_time(), &hour, &dow);
	return hour >= 3 && hour < 5;
}

static bool
pga_is_friday_evening(void)
{
	int			hour,
				dow;

	pga_local_time(pga_statement_time(), &hour, &dow);
	return dow == 5 && hour >= 18;
}

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

		if (q->cteList != NIL)
			ctx->flags |= PF_CTE;
		if (q->hasRecursive)
			ctx->flags |= PF_RECURSIVE;
		if (q->hasWindowFuncs)
			ctx->flags |= PF_WINDOW;

		foreach(lc, q->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_JOIN)
				ctx->joins++;
			if (rte->lateral)
				ctx->flags |= PF_LATERAL;
		}

		return query_tree_walker(q, pga_walker, context, 0);
	}

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		if (pga_is_jsonb_op(op->opno))
			ctx->jsonb_ops++;
		else if (pga_is_null_equality(op))
			ctx->null_eq++;
	}
	else if (IsA(node, FuncExpr))
	{
		if (((FuncExpr *) node)->funcid == F_PG_TERMINATE_BACKEND)
			ctx->terminate++;
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

static void
pga_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	if (!pga_tracking_active() || !pga_level_tracked())
		return;

	PGA_GUARD_BEGIN;
	{
		PgaPending *p = pga_pending_slot();

		memset(p, 0, sizeof(PgaPending));

		if (pga_sync_role())
		{
			p->valid = true;
			if (pga_is_night())
				p->flags |= PF_NIGHT;

			if (query->commandType != CMD_UTILITY)
			{
				PgaWalkContext ctx;

				memset(&ctx, 0, sizeof(ctx));
				(void) pga_walker((Node *) query, &ctx);

				p->flags |= ctx.flags;
				p->joins = ctx.joins;
				p->jsonb_ops = ctx.jsonb_ops;
				p->terminate = ctx.terminate;
				if (query->commandType == CMD_SELECT)
					p->flags |= PF_SELECT;

				/* counted immediately: the attempt is what matters */
				if (query->commandType == CMD_UPDATE && query->jointree->quals == NULL)
					pga_count(PGA_C_UPDATE_NO_WHERE, 1);
				if (query->commandType == CMD_DELETE && query->jointree->quals == NULL)
					pga_count(PGA_C_DELETE_NO_WHERE, 1);
				if (ctx.null_eq > 0)
					pga_count(PGA_C_NULL_EQ, 1);
				if (query->commandType == CMD_SELECT &&
					pga_has_select_star(pstate, query))
					pga_count(PGA_C_SELECT_STAR, 1);
			}
		}
	}
	PGA_GUARD_END;
}

/* Count the pending features of a statement that has just succeeded. */
static void
pga_apply_pending(PgaPending *p)
{
	if (!p->valid)
		return;

	if (p->flags & PF_SELECT)
		pga_count(PGA_C_STMT_SELECT, 1);
	if (p->flags & PF_CTE)
		pga_count(PGA_C_CTE_ANY, 1);
	if (p->flags & PF_RECURSIVE)
		pga_count(PGA_C_CTE_RECURSIVE, 1);
	if (p->flags & PF_WINDOW)
		pga_count(PGA_C_WINDOW_FUNC, 1);
	if (p->flags & PF_LATERAL)
		pga_count(PGA_C_LATERAL, 1);
	if (p->flags & PF_NIGHT)
		pga_count(PGA_C_NIGHT_QUERY, 1);
	if (p->jsonb_ops > 0)
		pga_count(PGA_C_JSONB_OPS, p->jsonb_ops);
	if (p->joins > 0)
		pga_count_max(PGA_C_JOIN_COUNT_MAX, p->joins);
	if (p->terminate > 0)
		pga_count(PGA_C_TERMINATE_BACKEND, p->terminate);

	p->valid = false;
}

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

			pga_apply_pending(pga_pending_slot());

			if (queryDesc->planstate != NULL)
			{
				(void) pga_seqscan_walker(queryDesc->planstate, &big_seqscan);
				if (big_seqscan)
					pga_count(PGA_C_SEQSCAN_BIG, 1);
			}

			if (nesting_level == 0)
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
	PgaCounter	counters[4];
	int64		amounts[4];
} PgaUtilityEvents;

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

static bool
pga_option_enabled(List *options, const char *name)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, name) == 0)
			return defGetBoolean(opt);
	}
	return false;
}

static void
pga_classify_utility(Node *parsetree, PgaUtilityEvents *ev)
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

				pga_add_event(ev, PGA_C_CREATE_TABLE, 1);
				if (stmt->partspec != NULL || stmt->partbound != NULL)
					pga_add_event(ev, PGA_C_PARTITION_CREATE, 1);
			}
			break;

		case T_CreateTableAsStmt:
			if (((CreateTableAsStmt *) parsetree)->objtype == OBJECT_TABLE)
				pga_add_event(ev, PGA_C_CREATE_TABLE, 1);
			break;

		case T_IndexStmt:
			pga_add_event(ev, PGA_C_CREATE_INDEX, 1);
			if (((IndexStmt *) parsetree)->concurrent)
				pga_add_event(ev, PGA_C_CREATE_INDEX_CONCURRENTLY, 1);
			break;

		case T_CreateFunctionStmt:
			pga_add_event(ev, PGA_C_CREATE_FUNCTION, 1);
			break;

		case T_CreateTrigStmt:
			pga_add_event(ev, PGA_C_CREATE_TRIGGER, 1);
			break;

		case T_CreateExtensionStmt:
			pga_add_event(ev, PGA_C_CREATE_EXTENSION, 1);
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (stmt->removeType == OBJECT_TABLE)
					pga_add_event(ev, PGA_C_DROP_TABLE, list_length(stmt->objects));
			}
			break;

		case T_DropdbStmt:
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

		default:
			break;
	}

	if (GetCommandLogLevel(parsetree) == LOGSTMT_DDL && pga_is_friday_evening())
		pga_add_event(ev, PGA_C_DDL_FRIDAY_EVENING, 1);
}

static void
pga_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	PgaUtilityEvents ev;
	bool		track = false;
	int			level = nesting_level;

	ev.n = 0;
	if (pga_tracking_active() && pga_level_tracked())
	{
		PGA_GUARD_BEGIN;
		{
			pga_classify_utility(pstmt->utilityStmt, &ev);
			track = true;
		}
		PGA_GUARD_END;
	}

	nesting_level++;
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
			pga_apply_pending(p);

			if (level == 0)
				pga_maybe_flush_by_time(GetCurrentTimestamp());
		}
		PGA_GUARD_END;
	}
}

static void
pga_emit_log(ErrorData *edata)
{
	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);

	/* no allocation, no locks, no ereport here */
	if (edata->elevel == ERROR && pga_tracking_active())
		pga_count_error(edata->sqlerrcode);
}

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
