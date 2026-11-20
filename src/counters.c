/*-------------------------------------------------------------------------
 *
 * counters.c
 *		Backend-local counter deltas, threshold checks and awards.
 *
 * Hooks only touch the local arrays below.  Shared memory is touched when
 * the deltas are flushed (transaction end, backend exit, timer), and when a
 * local check says that a threshold may have been reached.
 *
 * Effective value of a counter = value in shared memory at the last refresh
 * plus the local delta (for "max" counters: the larger of both).  Flushed
 * deltas are added to the cached values right away; the cache is re-read
 * from shared memory (to see what other sessions of the role did) at most
 * once per flush_interval_ms, or when a check is about to award something.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/acl.h"
#include "utils/timestamp.h"

bool		pga_session_disabled = false;

/* role the local deltas belong to */
static bool cur_role_valid = false;
static Oid	cur_role_oid = InvalidOid;
static NameData cur_role;
static PgaRoleEntry *cur_entry = NULL;

/* unflushed local changes */
static int64 delta[PGA_NUM_COUNTERS];
static int64 localmax[PGA_NUM_COUNTERS];
static bool dirty = false;
static TimestampTz last_flush = 0;
static TimestampTz last_refresh = 0;	/* last full re-read of shared values */
static bool exit_callback_registered = false;

/* view of shared state as of the last refresh */
static bool cache_ready = false;	/* catalog loaded by the worker */
static int64 cached[PGA_NUM_COUNTERS];
static int64 next_threshold[PGA_NUM_COUNTERS];
static uint32 local_catalog_version = 0;
static bool local_catalog_loaded = false;
static PgaCatalogSlot local_catalog[PGA_MAX_CATALOG];
static int	local_catalog_size = 0;

static void pga_refresh_cache(void);
static void pga_check_counter(PgaCounter c);
static void pga_check_meta(PgaRoleEntry *entry, bool notify);

/*
 * Should events of this backend be counted at all?  Cheap: GUCs and a
 * process type check only, called at the top of every hook.
 */
bool
pga_tracking_active(void)
{
	return pga_enabled &&
		!pga_opt_out &&
		!pga_session_disabled &&
		pga_shared != NULL &&
		MyBackendType == B_BACKEND &&
		!IsBinaryUpgrade;
}

/*
 * Called from PG_CATCH blocks of the hooks: report once and stop tracking in
 * this session.  Only the SQLSTATE is reported, never query text or object
 * names.
 */
void
pga_disable_session(ErrorData *edata)
{
	pga_session_disabled = true;
	ereport(WARNING,
			(errmsg("pg_achievements: internal error, tracking is disabled for this session"),
			 errdetail("SQLSTATE %s", unpack_sql_state(edata->sqlerrcode)),
			 errhidestmt(true),
			 errhidecontext(true)));
}

/*
 * before_shmem_exit callback: hand the last unflushed deltas to shared
 * memory so the worker writes them.
 */
static void
pga_on_exit(int code, Datum arg)
{
	/* never throw from here */
	if (dirty && cur_entry != NULL && pga_shared != NULL)
		pga_flush_local(false);
}

/*
 * Shared entry of the current role, created on first use (the only place a
 * backend takes the lock exclusively).
 */
static PgaRoleEntry *
pga_current_entry(void)
{
	if (cur_entry == NULL && cur_role_valid)
		cur_entry = pga_find_role(NameStr(cur_role), true);
	return cur_entry;
}

/* Switch the local state to another role, flushing the old deltas. */
void
pga_set_role_name(const char *rolname)
{
	if (cur_role_valid && strcmp(NameStr(cur_role), rolname) == 0)
		return;

	if (cur_role_valid && dirty)
		pga_flush_local(false);

	memset(&cur_role, 0, sizeof(cur_role));
	strlcpy(NameStr(cur_role), rolname, NAMEDATALEN);
	cur_role_valid = true;
	cur_entry = NULL;
	memset(delta, 0, sizeof(delta));
	memset(localmax, 0, sizeof(localmax));
	dirty = false;
	pga_refresh_cache();

	if (!exit_callback_registered)
	{
		before_shmem_exit(pga_on_exit, (Datum) 0);
		exit_callback_registered = true;
	}
}

/*
 * Make sure the local state belongs to the current user (GetUserId(), which
 * follows SET ROLE).  Needs catalog access, so it is only called where a
 * transaction is in progress.  Returns false if the role is unknown.
 */
bool
pga_sync_role(void)
{
	Oid			uid;
	int			secctx;
	char	   *name;

	GetUserIdAndSecContext(&uid, &secctx);

	if (!OidIsValid(uid))
		return false;
	if (cur_role_valid && uid == cur_role_oid)
		return true;
	if (!IsTransactionState())
		return cur_role_valid;

	name = GetUserNameFromId(uid, true);
	if (name == NULL)
		return false;

	pga_set_role_name(name);
	cur_role_oid = uid;
	pfree(name);
	return true;
}

/*
 * Copy the catalog snapshot from shared memory if the worker changed it.
 */
static void
pga_load_local_catalog(void)
{
	uint32		version = pg_atomic_read_u32(&pga_shared->catalog_version);

	if (local_catalog_loaded && version == local_catalog_version)
		return;

	LWLockAcquire(pga_shared->lock, LW_SHARED);
	local_catalog_size = pga_shared->catalog_size;
	memcpy(local_catalog, pga_shared->catalog,
		   sizeof(PgaCatalogSlot) * local_catalog_size);
	local_catalog_version = pg_atomic_read_u32(&pga_shared->catalog_version);
	LWLockRelease(pga_shared->lock);

	local_catalog_loaded = true;
}

/* Slot of the backend-local catalog copy, or NULL past its end. */
const PgaCatalogSlot *
pga_local_slot(int slot)
{
	if (slot < 0 || slot >= local_catalog_size)
		return NULL;
	return &local_catalog[slot];
}

/*
 * Slot of an achievement code in the local catalog copy, or -1 while the
 * worker has not loaded the catalog yet.
 */
int
pga_local_slot_by_code(const char *code)
{
	if (pga_shared == NULL || pg_atomic_read_u32(&pga_shared->ready) == 0)
		return -1;
	pga_load_local_catalog();
	for (int i = 0; i < local_catalog_size; i++)
	{
		if (local_catalog[i].used && strcmp(local_catalog[i].code, code) == 0)
			return i;
	}
	return -1;
}

/*
 * Effective value of a counter: shared value at the last refresh plus the
 * local delta (sum) or the larger of both (max).
 */
static inline int64
pga_effective(PgaCounter c)
{
	if (pga_counter_info[c].kind == PGA_SUM)
		return cached[c] + delta[c];
	return Max(cached[c], localmax[c]);
}

/* Lowest threshold among active achievements of counter c not yet awarded. */
static void
pga_compute_thresholds(PgaRoleEntry *entry)
{
	for (int c = 0; c < PGA_NUM_COUNTERS; c++)
		next_threshold[c] = PG_INT64_MAX;

	if (entry == NULL)
		return;

	for (int s = 0; s < local_catalog_size; s++)
	{
		PgaCatalogSlot *slot = &local_catalog[s];

		if (!slot->used || !slot->active || slot->counter < 0)
			continue;
		if (pga_bit_test(entry->awarded, s))
			continue;
		if (slot->threshold < next_threshold[slot->counter])
			next_threshold[slot->counter] = slot->threshold;
	}
}

/* Re-read shared values of the current role. */
static void
pga_refresh_cache(void)
{
	PgaRoleEntry *entry;

	memset(cached, 0, sizeof(cached));
	cache_ready = false;
	for (int c = 0; c < PGA_NUM_COUNTERS; c++)
		next_threshold[c] = PG_INT64_MAX;

	if (pga_shared == NULL || !cur_role_valid)
		return;
	if (pg_atomic_read_u32(&pga_shared->ready) == 0)
		return;

	pga_load_local_catalog();
	entry = pga_current_entry();
	if (entry == NULL)
		return;

	for (int c = 0; c < PGA_NUM_COUNTERS; c++)
		cached[c] = (int64) pg_atomic_read_u64(&entry->values[c]);
	pga_compute_thresholds(entry);
	cache_ready = true;
}

/*
 * Give an achievement to a role.  The bit is claimed atomically, so of all
 * sessions of the role that race for the same achievement exactly one wins
 * and reports it.  Returns 1 if this call awarded it.
 */
int
pga_award_slot(PgaRoleEntry *entry, const char *rolname, int slot, bool notify)
{
	if (!pga_bit_set(entry->awarded, slot))
		return 0;

	(void) pga_enqueue_award(rolname, slot, GetCurrentTimestamp());
	if (notify && pga_notify)
		pga_send_notice(entry, slot);
	return 1;
}

/* Award every meta achievement whose condition now holds. */
static void
pga_check_meta(PgaRoleEntry *entry, bool notify)
{
	bool		changed = true;

	while (changed)
	{
		changed = false;

		for (int m = 0; m < local_catalog_size; m++)
		{
			PgaCatalogSlot *meta = &local_catalog[m];
			bool		complete = true;
			bool		any = false;

			if (!meta->used || !meta->active || meta->counter >= 0)
				continue;
			if (pga_bit_test(entry->awarded, m))
				continue;

			for (int s = 0; s < local_catalog_size && complete; s++)
			{
				PgaCatalogSlot *slot = &local_catalog[s];

				if (s == m || !slot->used || !slot->active || !slot->builtin)
					continue;
				if (meta->counter == PGA_META_KIND_TIER &&
					(slot->counter < 0 || slot->tier != meta->meta_tier))
					continue;

				any = true;
				if (!pga_bit_test(entry->awarded, s))
					complete = false;
			}

			if (complete && any &&
				pga_award_slot(entry, NameStr(entry->rolname), m, notify))
				changed = true;
		}
	}
}

/* Slow path: a threshold of counter c may have been reached. */
static void
pga_check_counter(PgaCounter c)
{
	PgaRoleEntry *entry = pga_current_entry();
	int64		value;
	int			awarded = 0;

	if (entry == NULL)
		return;

	/* use the freshest shared value */
	cached[c] = (int64) pg_atomic_read_u64(&entry->values[c]);
	value = pga_effective(c);

	for (int s = 0; s < local_catalog_size; s++)
	{
		PgaCatalogSlot *slot = &local_catalog[s];

		if (!slot->used || !slot->active || slot->counter != c)
			continue;
		if (value < slot->threshold)
			continue;
		awarded += pga_award_slot(entry, NameStr(cur_role), s, true);
	}

	if (awarded > 0)
		pga_check_meta(entry, true);
	pga_compute_thresholds(entry);
}

/* Check every counter, e.g. after the worker has loaded the catalog. */
void
pga_check_all(void)
{
	PgaRoleEntry *entry;

	if (!cache_ready)
		return;
	entry = pga_current_entry();
	if (entry == NULL)
		return;

	for (int c = 0; c < PGA_NUM_COUNTERS; c++)
	{
		if (pga_effective(c) >= next_threshold[c])
			pga_check_counter(c);
	}
	pga_check_meta(entry, true);
}

/* Add n to a "sum" counter of the current role. */
void
pga_count(PgaCounter c, int64 n)
{
	if (!cur_role_valid || n <= 0)
		return;

	delta[c] += n;
	dirty = true;

	if (cache_ready && pga_effective(c) >= next_threshold[c])
		pga_check_counter(c);
}

/* Record an observation of a "max" counter. */
void
pga_count_max(PgaCounter c, int64 v)
{
	if (!cur_role_valid || v <= localmax[c])
		return;

	localmax[c] = v;
	dirty = true;

	if (cache_ready && pga_effective(c) >= next_threshold[c])
		pga_check_counter(c);
}

/*
 * Called from emit_log_hook: only bump the delta, never check thresholds or
 * touch shared memory while an error is being reported.  The check happens
 * when the transaction is aborted.
 */
void
pga_count_error(int sqlerrcode)
{
	PgaCounter	c;
	Oid			uid;
	int			secctx;

	switch (sqlerrcode)
	{
		case ERRCODE_SYNTAX_ERROR:
			c = PGA_C_ERR_SYNTAX;
			break;
		case ERRCODE_T_R_DEADLOCK_DETECTED:
			c = PGA_C_DEADLOCK;
			break;
		case ERRCODE_T_R_SERIALIZATION_FAILURE:
			c = PGA_C_SERIALIZATION_FAILURE;
			break;
		default:
			return;
	}

	GetUserIdAndSecContext(&uid, &secctx);
	if (!cur_role_valid || !OidIsValid(uid) || uid != cur_role_oid)
		return;

	delta[c]++;
	dirty = true;
}

/*
 * Move the local deltas to shared memory.  With check = true, re-read the
 * shared state and award whatever has been reached meanwhile (also by other
 * sessions of the same role).
 */
void
pga_flush_local(bool check)
{
	PgaRoleEntry *entry;
	TimestampTz now;

	if (pga_shared == NULL || !cur_role_valid)
		return;

	if (dirty)
	{
		entry = pga_current_entry();
		if (entry != NULL)
		{
			for (int c = 0; c < PGA_NUM_COUNTERS; c++)
			{
				if (pga_counter_info[c].kind == PGA_SUM)
				{
					if (delta[c] != 0)
						pg_atomic_fetch_add_u64(&entry->values[c], delta[c]);
					/* our own contribution is known exactly, keep it cached */
					cached[c] += delta[c];
				}
				else if (localmax[c] > 0)
				{
					uint64		old = pg_atomic_read_u64(&entry->values[c]);

					cached[c] = Max(cached[c], localmax[c]);

					while ((int64) old < localmax[c] &&
						   !pg_atomic_compare_exchange_u64(&entry->values[c], &old,
														   (uint64) localmax[c]))
						;
				}
			}
		}
		memset(delta, 0, sizeof(delta));
		memset(localmax, 0, sizeof(localmax));
		dirty = false;
	}

	now = GetCurrentTimestamp();
	last_flush = now;

	/*
	 * Re-reading every counter of the role at each commit is what costs:
	 * with many sessions of one role those cache lines are written by all
	 * of them.  Our own deltas are already in the cache, so only the
	 * progress made by other sessions needs a refresh, and once per flush
	 * interval is enough for that.
	 */
	if (!check)
		return;

	if (!cache_ready ||
		TimestampDifferenceExceeds(last_refresh, now, pga_flush_interval_ms))
	{
		pga_refresh_cache();
		last_refresh = now;
	}

	/*
	 * Compare the cached values with the thresholds.  This is purely local
	 * unless something is due, and it is what reports the counters bumped
	 * by emit_log_hook, which never checks thresholds itself.
	 */
	pga_check_all();
}

/* Make the next flush re-read the shared state (e.g. after a sync). */
void
pga_invalidate_cache(void)
{
	last_refresh = 0;
}

/* Flush at least every flush_interval_ms inside long transactions. */
void
pga_maybe_flush_by_time(TimestampTz now)
{
	if (dirty &&
		TimestampDifferenceExceeds(last_flush, now, pga_flush_interval_ms))
		pga_flush_local(true);
}

/* Current value of a counter of the current role, local delta included. */
int64
pga_counter_value(PgaCounter c)
{
	PgaRoleEntry *entry;
	int64		shared_value = 0;

	if (!pga_sync_role())
		return 0;
	entry = pga_current_entry();
	if (entry != NULL)
		shared_value = (int64) pg_atomic_read_u64(&entry->values[c]);

	if (pga_counter_info[c].kind == PGA_SUM)
		return shared_value + delta[c];
	return Max(shared_value, localmax[c]);
}

/* Drop the local deltas of the current role (after a reset). */
void
pga_forget_local(void)
{
	memset(delta, 0, sizeof(delta));
	memset(localmax, 0, sizeof(localmax));
	dirty = false;
	pga_refresh_cache();
}

const char *
pga_current_role_name(void)
{
	return cur_role_valid ? NameStr(cur_role) : NULL;
}

/* Award the achievement in a slot to a role by hand (achievements.grant). */
bool
pga_grant(const char *rolname, int slot)
{
	PgaRoleEntry *entry = pga_find_role(rolname, true);
	bool		ok;

	if (entry == NULL)
		return false;
	ok = pga_award_slot(entry, rolname, slot, false) > 0;
	if (ok)
		pga_check_meta(entry, false);
	if (cur_role_valid && strcmp(rolname, NameStr(cur_role)) == 0)
		pga_refresh_cache();
	return ok;
}
