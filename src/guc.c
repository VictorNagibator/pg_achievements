/*-------------------------------------------------------------------------
 *
 * guc.c
 *		Configuration parameters of pg_achievements.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include <limits.h>

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

bool		pga_enabled = true;
char	   *pga_database = NULL;
bool		pga_opt_out = false;
bool		pga_notify = true;
int			pga_notice_style = PGA_NOTICE_EMOJI;
bool		pga_track_nested = false;
int			pga_flush_interval_ms = 1000;
int			pga_big_table_rows = 1000000;
int			pga_max_roles = 1024;
char	   *pga_notify_channel = NULL;

#ifdef PGA_TESTING
char	   *pga_test_now = NULL;
int			pga_test_time_shift = 0;
bool		pga_test_inject_error = false;

static TimestampTz pga_test_now_value = 0;
static bool pga_test_now_set = false;
#endif

static const struct config_enum_entry notice_style_options[] = {
	{"emoji", PGA_NOTICE_EMOJI, false},
	{"ascii", PGA_NOTICE_ASCII, false},
	{NULL, 0, false}
};

#ifdef PGA_TESTING
/* Parse test_now once in the check hook; the value is kept in "extra". */
static bool
check_test_now(char **newval, void **extra, GucSource source)
{
	TimestampTz *ts;

	if (*newval == NULL || (*newval)[0] == '\0')
	{
		*extra = NULL;
		return true;
	}

	/*
	 * Parsing a timestamptz needs catalog access only for named time zones,
	 * which the tests do not use; numeric offsets are handled by datetime.c.
	 */
#if PG_VERSION_NUM >= 160000
	ts = guc_malloc(LOG, sizeof(TimestampTz));
#else
	/* before 16 the GUC machinery frees "extra" with plain free() */
	ts = malloc(sizeof(TimestampTz));
#endif
	if (ts == NULL)
		return false;
	*ts = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
												  CStringGetDatum(*newval),
												  ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(-1)));
	*extra = ts;
	return true;
}

/* Publish the parsed test_now value (or clear it). */
static void
assign_test_now(const char *newval, void *extra)
{
	if (extra == NULL)
		pga_test_now_set = false;
	else
	{
		pga_test_now_set = true;
		pga_test_now_value = *(TimestampTz *) extra;
	}
}
#endif

/*
 * The channel name goes to pg_notify(), which refuses names of NAMEDATALEN
 * bytes or more.  Checking it here turns a typo in postgresql.conf into a
 * clear error at reload instead of a failing write of the worker.
 */
static bool
check_notify_channel(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && strlen(*newval) >= NAMEDATALEN)
	{
		GUC_check_errdetail("The channel name must be shorter than %d bytes.",
							NAMEDATALEN);
		return false;
	}
	return true;
}

/* Register all parameters; called from _PG_init in the postmaster. */
void
pga_define_gucs(void)
{
	DefineCustomBoolVariable("pg_achievements.enabled",
							 "Enables tracking of achievement events.",
							 NULL,
							 &pga_enabled,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("pg_achievements.database",
							   "Database that stores the tables of pg_achievements.",
							   NULL,
							   &pga_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_achievements.opt_out",
							 "Opts the current role or session out of achievement tracking.",
							 "Usually set with ALTER ROLE ... SET pg_achievements.opt_out = on.",
							 &pga_opt_out,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_achievements.notify",
							 "Shows a notice when an achievement is unlocked.",
							 NULL,
							 &pga_notify,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_achievements.notice_style",
							 "Style of achievement notices.",
							 "emoji prints a medal when the client encoding is UTF8.",
							 &pga_notice_style,
							 PGA_NOTICE_EMOJI,
							 notice_style_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_achievements.track_nested",
							 "Tracks statements executed inside functions.",
							 NULL,
							 &pga_track_nested,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_achievements.flush_interval_ms",
							"Interval between flushes of counters to the tables.",
							NULL,
							&pga_flush_interval_ms,
							1000,
							100, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_achievements.big_table_rows",
							"Row estimate starting from which a table counts as big.",
							NULL,
							&pga_big_table_rows,
							1000000,
							1, INT_MAX,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_achievements.max_roles",
							"Maximum number of roles tracked in shared memory.",
							NULL,
							&pga_max_roles,
							1024,
							16, 1000000,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	/*
	 * Read by the trigger on achievements.awarded (see the SQL script), so
	 * every new award is announced with NOTIFY; an empty string turns that
	 * off.  SIGHUP, so that a session cannot hide its awards from listeners.
	 */
	DefineCustomStringVariable("pg_achievements.notify_channel",
							   "NOTIFY channel that announces new achievements.",
							   "An empty string disables the notifications.",
							   &pga_notify_channel,
							   "achievements",
							   PGC_SIGHUP,
							   0,
							   check_notify_channel, NULL, NULL);

#ifdef PGA_TESTING
	DefineCustomStringVariable("pg_achievements.test_now",
							   "Testing only: overrides the current time.",
							   NULL,
							   &pga_test_now,
							   "",
							   PGC_USERSET,
							   GUC_NOT_IN_SAMPLE,
							   check_test_now, assign_test_now, NULL);

	DefineCustomIntVariable("pg_achievements.test_time_shift",
							"Testing only: seconds added to measured durations.",
							NULL,
							&pga_test_time_shift,
							0,
							0, INT_MAX,
							PGC_USERSET,
							GUC_NOT_IN_SAMPLE | GUC_UNIT_S,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_achievements.test_inject_error",
							 "Testing only: raise an error inside the parse analysis hook.",
							 "Checks that internal errors never break user queries.",
							 &pga_test_inject_error,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
#endif

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("pg_achievements");
#else
	EmitWarningsOnPlaceholders("pg_achievements");	/* renamed in 15 */
#endif
}

/* Current wall-clock time, possibly faked in test builds. */
TimestampTz
pga_now(void)
{
#ifdef PGA_TESTING
	if (pga_test_now_set)
		return pga_test_now_value;
#endif
	return GetCurrentTimestamp();
}

/* Extra microseconds added to measured durations (test builds only). */
int64
pga_duration_shift_us(void)
{
#ifdef PGA_TESTING
	return (int64) pga_test_time_shift * USECS_PER_SEC;
#else
	return 0;
#endif
}
