/*-------------------------------------------------------------------------
 *
 * worker.c
 *		Launcher and flush worker: load progress into shared memory and
 *		write counters and awards to the tables of the extension.
 *
 * Two processes are involved.  The launcher is registered at server start
 * and has no database connection; it starts the flush worker once
 * pg_achievements.database exists and restarts it after a failure.
 *
 * Every flush_interval_ms (or on request from achievements.sync()) the
 * worker runs one cycle in its own transaction:
 *	1. checks that the extension exists in pg_achievements.database;
 *	2. on the first cycle loads the catalog, counters and awards, merging
 *	   them with what backends have counted meanwhile;
 *	3. reloads the catalog snapshot if the table changed;
 *	4. unless in recovery, writes resets, queued awards and what the
 *	   counters gained since the last write.
 *
 * Counters are written as increments (sum counters) or with GREATEST (max
 * counters), never as absolute values.  Rows that reach the tables from
 * elsewhere, such as a pg_dump being restored while the server runs, are
 * therefore added to rather than overwritten; the BEFORE INSERT triggers of
 * the tables merge restored rows with the rows the worker has written.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/* set by the SIGTERM handler: finish the current cycle, flush once, exit */
static volatile sig_atomic_t got_sigterm = false;

/* the "catalog is full" warning is printed once per worker lifetime */
static bool catalog_full_warned = false;

/*
 * Roles whose reset was processed in the current cycle.  If the cycle fails,
 * their reset requests are restored so that the rows still get deleted.
 */
static PgaRoleEntry **resets_done = NULL;
static int	nresets_done = 0;

/*
 * What the tables hold for each role ("persisted" values and awards).  A
 * cycle works on copies, and pga_publish_progress() copies them back into
 * shared memory only after the transaction has committed: since counters
 * are written as increments, bookkeeping that ran ahead of a failed
 * transaction would lose events, and bookkeeping that lagged behind a
 * committed one would count them twice.
 */
typedef struct PgaStaged
{
	PgaRoleEntry *entry;
	int64		persisted[PGA_NUM_COUNTERS];
	uint64		persisted_awarded[PGA_AWARD_WORDS];
} PgaStaged;

static PgaStaged *staged = NULL;
static int	nstaged = 0;

/* memory of one cycle; survives the transaction so PG_CATCH can use it */
static MemoryContext cycle_cxt = NULL;

/* SIGTERM of both processes: no die(), the loops exit on their own terms */
static void
pga_worker_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

/*
 * The launcher is a static worker without a database connection.  It waits
 * until pg_achievements.database exists, then starts the flush worker as a
 * dynamic worker and restarts it 10 seconds after it exits.  This way the
 * database may be created after the server has started.
 *
 * Only one flush worker may run: two would both write the same increments.
 * A flush worker can outlive its launcher (pg_terminate_backend() of the
 * launcher), so the restarted launcher does not start another one while
 * pga_shared->worker_pid is set, and the flush worker itself refuses to
 * run next to another one (see pga_claim_worker).
 */
void
pga_register_worker(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = PGA_WORKER_RESTART_S;
	strlcpy(worker.bgw_library_name, PGA_EXTNAME, BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "pga_launcher_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "pg_achievements launcher", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "pg_achievements launcher", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}

static bool
pga_database_exists(void)
{
	bool		exists = false;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;

	/*
	 * Without a database only shared catalogs can be read, and not through
	 * their indexes: scan pg_database like the autovacuum launcher does.
	 */
	StartTransactionCommand();
	(void) GetTransactionSnapshot();
	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);

		if (strcmp(NameStr(db->datname), pga_database) == 0)
		{
			exists = true;
			break;
		}
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	CommitTransactionCommand();
	return exists;
}

/*
 * Start the flush worker.  It is a dynamic worker that never restarts by
 * itself: the launcher is notified when it exits (bgw_notify_pid) and
 * decides when to start it again.
 */
static BackgroundWorkerHandle *
pga_start_flush_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_notify_pid = MyProcPid;
	strlcpy(worker.bgw_library_name, PGA_EXTNAME, BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "pga_worker_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "pg_achievements worker", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "pg_achievements", BGW_MAXLEN);

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
	{
		ereport(WARNING,
				(errmsg("pg_achievements: could not start the worker"),
				 errhint("Consider increasing max_worker_processes.")));
		return NULL;
	}
	return handle;
}

/*
 * Main loop of the launcher.  Once a second (or when a backend wakes it up)
 * it checks the flush worker and starts it if needed, keeping at least
 * PGA_WORKER_RESTART_S seconds between an exit and the next start.
 */
void
pga_launcher_main(Datum main_arg)
{
	BackgroundWorkerHandle *handle = NULL;
	TimestampTz last_exit = 0;
	bool		warned = false;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, pga_worker_sigterm);
	BackgroundWorkerUnblockSignals();

	/* no database: only shared catalogs such as pg_database */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	pga_shared->launcher_latch = MyLatch;

	while (!got_sigterm)
	{
		pid_t		pid;

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (handle != NULL &&
			GetBackgroundWorkerPid(handle, &pid) == BGWH_STOPPED)
		{
			pfree(handle);
			handle = NULL;
			last_exit = GetCurrentTimestamp();
		}

		/*
		 * A flush worker started by an earlier launcher may still run; we
		 * have no handle for it, so poll until it is gone.
		 */
		if (handle == NULL && pga_shared->worker_pid != 0)
			last_exit = GetCurrentTimestamp();
		else if (handle == NULL &&
				 (last_exit == 0 ||
				  TimestampDifferenceExceeds(last_exit, GetCurrentTimestamp(),
											 PGA_WORKER_RESTART_S * 1000)))
		{
			if (pga_database_exists())
			{
				handle = pga_start_flush_worker();
				warned = false;
				if (handle == NULL)
					last_exit = GetCurrentTimestamp();
			}
			else if (!warned)
			{
				ereport(LOG,
						(errmsg("pg_achievements: waiting for database \"%s\" to be created",
								pga_database)));
				warned = true;
			}
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		/* absorbs ProcSignalBarriers, e.g. from DROP DATABASE */
		CHECK_FOR_INTERRUPTS();
	}

	pga_shared->launcher_latch = NULL;

	/*
	 * Exit code 1, not 0: a static worker that exits with 0 is unregistered
	 * for good, and after pg_terminate_backend() of the launcher nothing
	 * would restart the flush worker any more.  At shutdown the postmaster
	 * restarts nothing anyway.
	 */
	proc_exit(1);
}

/*
 * Ask for a cycle right now (achievements.sync).  When the flush worker is
 * not running, wake the launcher instead so that it starts it sooner.
 */
void
pga_wake_worker(void)
{
	Latch	   *latch;

	if (pga_shared == NULL)
		return;
	latch = pga_shared->worker_latch;
	if (latch != NULL)
		SetLatch(latch);
	else if ((latch = pga_shared->launcher_latch) != NULL)
		SetLatch(latch);
}

/* ---------- SPI helpers ---------- */

/* Run a statement and insist on the expected SPI result code. */

static void
pga_exec(const char *sql, int nargs, Oid *types, Datum *values, int expected)
{
	int			ret = SPI_execute_with_args(sql, nargs, types, values, NULL, false, 0);

	if (ret != expected)
		elog(ERROR, "pg_achievements: SPI_execute_with_args failed: %d", ret);
}

/* Column accessors of the current SPI result, by 1-based column number. */
static char *
pga_getstr(HeapTuple tup, TupleDesc desc, int col)
{
	return SPI_getvalue(tup, desc, col);
}

static int64
pga_getint(HeapTuple tup, TupleDesc desc, int col)
{
	bool		isnull;
	Datum		d = SPI_getbinval(tup, desc, col, &isnull);

	if (isnull)
		return 0;
	if (SPI_gettypeid(desc, col) == INT4OID)
		return DatumGetInt32(d);
	return DatumGetInt64(d);
}

static bool
pga_getbool(HeapTuple tup, TupleDesc desc, int col)
{
	bool		isnull;
	Datum		d = SPI_getbinval(tup, desc, col, &isnull);

	return !isnull && DatumGetBool(d);
}

/* ---------- catalog ---------- */

static void
pga_parse_counter_key(const char *key, PgaCatalogSlot *slot)
{
	if (strncmp(key, PGA_META_TIER_PREFIX, strlen(PGA_META_TIER_PREFIX)) == 0)
	{
		slot->counter = PGA_META_KIND_TIER;
		slot->meta_tier = pga_tier_by_name(key + strlen(PGA_META_TIER_PREFIX));
		if (slot->meta_tier < 0)
			slot->active = false;
	}
	else if (strcmp(key, PGA_META_ALL) == 0)
	{
		slot->counter = PGA_META_KIND_ALL;
		slot->meta_tier = -1;
	}
	else
	{
		slot->counter = pga_counter_by_key(key);
		slot->meta_tier = -1;
		/* unknown key, or a feature this server lacks (MERGE on 14) */
		if (!pga_counter_supported(slot->counter))
			slot->active = false;
	}
}

/*
 * Copy a text of the catalog into a fixed-size buffer of a slot.  The
 * buffers fit every row the table accepts, but a cut, if any, must not
 * split a multibyte character: a notice with half a character is refused
 * by the conversion to the client encoding.
 */
static void
pga_copy_text(char *dst, const char *src, int size)
{
	int			len = pg_mbcliplen(src, strlen(src), size - 1);

	memcpy(dst, src, len);
	dst[len] = '\0';
}

/*
 * Read achievements.catalog into a new snapshot.  Existing codes keep their
 * slots; removed codes stay in place as inactive and deleted; new codes get
 * new slots.  Publishes the snapshot only if something changed.
 *
 * A deleted code is taken away from every role on each cycle: the foreign
 * key has deleted its awarded rows, and if the code is defined again it
 * gets its old slot back, which must not look earned.  Doing it on every
 * cycle, not just once, also catches a backend that awarded the code from
 * a catalog copy older than the deletion.
 */
static void
pga_load_catalog(void)
{
	PgaCatalogSlot *snap;
	int			size;
	bool	   *seen;
	bool		changed = false;

	pga_exec("SELECT code, title, description, tier::text, points, counter_key, "
			 "threshold, enabled, builtin, secret FROM achievements.catalog "
			 "ORDER BY code",
			 0, NULL, NULL, SPI_OK_SELECT);

	snap = palloc(sizeof(PgaCatalogSlot) * PGA_MAX_CATALOG);
	seen = palloc0(sizeof(bool) * PGA_MAX_CATALOG);
	size = pga_shared->catalog_size;
	memcpy(snap, pga_shared->catalog, sizeof(PgaCatalogSlot) * size);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		HeapTuple	tup = SPI_tuptable->vals[r];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		char	   *code = pga_getstr(tup, desc, 1);
		PgaCatalogSlot slot;
		int			idx = -1;

		for (int i = 0; i < size; i++)
		{
			if (strcmp(snap[i].code, code) == 0)
			{
				idx = i;
				break;
			}
		}
		if (idx < 0)
		{
			if (size >= PGA_MAX_CATALOG)
			{
				if (!catalog_full_warned)
					ereport(WARNING,
							(errmsg("pg_achievements: more than %d achievements in the catalog, the rest is ignored",
									PGA_MAX_CATALOG)));
				catalog_full_warned = true;
				continue;
			}
			idx = size++;
			memset(&snap[idx], 0, sizeof(PgaCatalogSlot));
		}

		memset(&slot, 0, sizeof(slot));
		slot.used = true;
		slot.active = pga_getbool(tup, desc, 8);
		slot.builtin = pga_getbool(tup, desc, 9);
		slot.secret = pga_getbool(tup, desc, 10);
		slot.tier = pga_tier_by_name(pga_getstr(tup, desc, 4));
		if (slot.tier < 0)
		{
			slot.tier = PGA_TIER_BRONZE;
			slot.active = false;
		}
		slot.points = (int32) pga_getint(tup, desc, 5);
		slot.threshold = pga_getint(tup, desc, 7);
		pga_copy_text(slot.code, code, PGA_CODE_LEN);
		pga_copy_text(slot.title, pga_getstr(tup, desc, 2), PGA_TITLE_LEN);
		pga_copy_text(slot.description, pga_getstr(tup, desc, 3), PGA_DESC_LEN);
		pga_parse_counter_key(pga_getstr(tup, desc, 6), &slot);

		seen[idx] = true;
		if (memcmp(&snap[idx], &slot, sizeof(slot)) != 0)
		{
			snap[idx] = slot;
			changed = true;
		}
	}

	/* codes deleted from the table */
	for (int i = 0; i < size; i++)
	{
		if (!seen[i] && snap[i].used && !snap[i].deleted)
		{
			snap[i].active = false;
			snap[i].deleted = true;
			changed = true;
		}
	}

	if (changed || size != pga_shared->catalog_size)
	{
		LWLockAcquire(pga_shared->lock, LW_EXCLUSIVE);
		memcpy(pga_shared->catalog, snap, sizeof(PgaCatalogSlot) * size);
		pga_shared->catalog_size = size;
		pga_shared->catalog_encoding = GetDatabaseEncoding();
		pg_atomic_fetch_add_u32(&pga_shared->catalog_version, 1);
		LWLockRelease(pga_shared->lock);
	}

	for (int i = 0; i < size; i++)
	{
		if (snap[i].deleted)
			pga_clear_slot_everywhere(i);
	}
}

/* Slot holding a code in the shared catalog, or -1.  Worker only. */
static int
pga_slot_by_code(const char *code)
{
	for (int i = 0; i < pga_shared->catalog_size; i++)
	{
		if (pga_shared->catalog[i].used && strcmp(pga_shared->catalog[i].code, code) == 0)
			return i;
	}
	return -1;
}

/* ---------- loading progress ---------- */

/*
 * Load counters and awards from the tables into shared memory, on the first
 * cycle after start or after CREATE EXTENSION.  Values are merged with what
 * backends counted before the load: sums are added, maxima combined, and
 * the loaded values become the "persisted" baseline, so the next write
 * stores exactly the events counted meanwhile.
 */
static void
pga_load_progress(void)
{
	pga_exec("SELECT role_name::text, counter_key, value FROM achievements.counters",
			 0, NULL, NULL, SPI_OK_SELECT);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		HeapTuple	tup = SPI_tuptable->vals[r];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		char	   *role = pga_getstr(tup, desc, 1);
		int			c = pga_counter_by_key(pga_getstr(tup, desc, 2));
		int64		value = pga_getint(tup, desc, 3);
		PgaRoleEntry *entry;

		if (c < 0 || value <= 0)
			continue;
		entry = pga_find_role(role, true);
		if (entry == NULL)
			continue;

		if (pga_counter_info[c].kind == PGA_SUM)
			pg_atomic_fetch_add_u64(&entry->values[c], (uint64) value);
		else
		{
			uint64		old = pg_atomic_read_u64(&entry->values[c]);

			while ((int64) old < value &&
				   !pg_atomic_compare_exchange_u64(&entry->values[c], &old, (uint64) value))
				;
		}
		entry->persisted[c] = value;
	}

	pga_exec("SELECT role_name::text, code FROM achievements.awarded",
			 0, NULL, NULL, SPI_OK_SELECT);

	for (uint64 r = 0; r < SPI_processed; r++)
	{
		HeapTuple	tup = SPI_tuptable->vals[r];
		TupleDesc	desc = SPI_tuptable->tupdesc;
		char	   *role = pga_getstr(tup, desc, 1);
		int			slot = pga_slot_by_code(pga_getstr(tup, desc, 2));
		PgaRoleEntry *entry;

		if (slot < 0)
			continue;
		entry = pga_find_role(role, true);
		if (entry == NULL)
			continue;
		(void) pga_bit_set(entry->awarded, slot);
		entry->persisted_awarded[slot / 64] |= UINT64CONST(1) << (slot % 64);
	}
}

/* ---------- writing ---------- */

/* Store one award; use_ts = false stores "now" (overflowed queue). */
static void
pga_write_award(PgaRoleEntry *entry, int slot, TimestampTz ts, bool use_ts)
{
	Oid			types[3] = {NAMEOID, TEXTOID, TIMESTAMPTZOID};
	Datum		values[3];

	values[0] = NameGetDatum(&entry->rolname);
	values[1] = CStringGetTextDatum(pga_shared->catalog[slot].code);
	values[2] = TimestampTzGetDatum(use_ts ? ts : GetCurrentTimestamp());

	pga_exec("INSERT INTO achievements.awarded (role_name, code, awarded_at) "
			 "SELECT $1, $2, $3 WHERE EXISTS "
			 "(SELECT 1 FROM achievements.catalog WHERE code = $2) "
			 "ON CONFLICT DO NOTHING",
			 3, types, values, SPI_OK_INSERT);
}

/* Staged bookkeeping of a role in the current cycle, or NULL. */
static PgaStaged *
pga_staged_for(PgaRoleEntry *entry)
{
	for (int i = 0; i < nstaged; i++)
	{
		if (staged[i].entry == entry)
			return &staged[i];
	}
	return NULL;
}

/* Write one counter: an increment for "sum", the larger value for "max". */
static void
pga_write_counter(PgaRoleEntry *entry, int c, int64 amount)
{
	Oid			types[3] = {NAMEOID, TEXTOID, INT8OID};
	Datum		values[3];

	values[0] = NameGetDatum(&entry->rolname);
	values[1] = CStringGetTextDatum(pga_counter_info[c].key);
	values[2] = Int64GetDatum(amount);

	if (pga_counter_info[c].kind == PGA_SUM)
		pga_exec("INSERT INTO achievements.counters AS c "
				 "(role_name, counter_key, value, updated_at) "
				 "VALUES ($1, $2, $3, now()) "
				 "ON CONFLICT (role_name, counter_key) DO UPDATE "
				 "SET value = c.value + EXCLUDED.value, updated_at = EXCLUDED.updated_at",
				 3, types, values, SPI_OK_INSERT);
	else
		pga_exec("INSERT INTO achievements.counters AS c "
				 "(role_name, counter_key, value, updated_at) "
				 "VALUES ($1, $2, $3, now()) "
				 "ON CONFLICT (role_name, counter_key) DO UPDATE "
				 "SET value = GREATEST(c.value, EXCLUDED.value), "
				 "updated_at = EXCLUDED.updated_at",
				 3, types, values, SPI_OK_INSERT);
}

/*
 * Lock the counters and awarded tables against inserts of other sessions
 * until the end of the transaction.  A restore running COPY into the tables
 * then waits for this cycle (or the cycle is skipped while the COPY runs),
 * and the triggers of the tables see the rows of the worker and merge with
 * them instead of failing on the primary key.  Normally the lock is not
 * waited for, because a restore in a single transaction holds its locks
 * until the very end; the last cycle before exit waits, so that a clean
 * shutdown loses nothing (a shutdown terminates the restore anyway).
 */
static bool
pga_lock_tables(bool wait)
{
	Oid			nsp = get_namespace_oid(PGA_SCHEMA, false);
	const char *const tables[] = {"counters", "awarded"};

	for (int i = 0; i < lengthof(tables); i++)
	{
		Oid			relid = get_relname_relid(tables[i], nsp);

		if (!OidIsValid(relid))
			elog(ERROR, "pg_achievements: table %s.%s is missing", PGA_SCHEMA, tables[i]);
		if (wait)
			LockRelationOid(relid, ShareRowExclusiveLock);
		else if (!ConditionalLockRelationOid(relid, ShareRowExclusiveLock))
			return false;
	}
	return true;
}

/*
 * Write everything that changed since the last cycle, in three steps:
 * resets requested by achievements.reset(), queued awards with their exact
 * time, then awards missing from the queue and what the counters gained.
 * The bookkeeping goes to the staged copies, see PgaStaged.  Returns false
 * if the tables are busy and nothing was written.
 */
static bool
pga_write_progress(bool wait)
{
	HASH_SEQ_STATUS status;
	PgaRoleEntry *entry;
	PgaQueuedAward *queue;
	int			nqueue;
	int			maxentries = 64;

	if (!pga_lock_tables(wait))
		return false;

	/*
	 * Take the queued awards first: a role has an entry before it can
	 * queue an award, so every role in the queue is in the snapshot below.
	 */
	queue = palloc(sizeof(PgaQueuedAward) * PGA_AWARD_QUEUE_SIZE);
	nqueue = pga_drain_queue(queue, PGA_AWARD_QUEUE_SIZE);

	/* snapshot the entry list with the bookkeeping; entries are never removed */
	staged = MemoryContextAlloc(cycle_cxt, sizeof(PgaStaged) * maxentries);
	nstaged = 0;
	LWLockAcquire(pga_shared->lock, LW_SHARED);
	hash_seq_init(&status, pga_roles);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (nstaged >= maxentries)
		{
			maxentries *= 2;
			staged = repalloc(staged, sizeof(PgaStaged) * maxentries);
		}
		staged[nstaged].entry = entry;
		memcpy(staged[nstaged].persisted, entry->persisted, sizeof(entry->persisted));
		memcpy(staged[nstaged].persisted_awarded, entry->persisted_awarded,
			   sizeof(entry->persisted_awarded));
		nstaged++;
	}
	LWLockRelease(pga_shared->lock);

	/* 1. resets requested by achievements.reset() / forget_role() */
	resets_done = MemoryContextAlloc(cycle_cxt, sizeof(PgaRoleEntry *) * Max(nstaged, 1));
	for (int i = 0; i < nstaged; i++)
	{
		Oid			types[1] = {NAMEOID};
		Datum		values[1];

		entry = staged[i].entry;
		if (pg_atomic_exchange_u32(&entry->reset_requested, 0) == 0)
			continue;

		values[0] = NameGetDatum(&entry->rolname);
		pga_exec("DELETE FROM achievements.counters WHERE role_name = $1",
				 1, types, values, SPI_OK_DELETE);
		pga_exec("DELETE FROM achievements.awarded WHERE role_name = $1",
				 1, types, values, SPI_OK_DELETE);
		resets_done[nresets_done++] = entry;
		memset(staged[i].persisted, 0, sizeof(staged[i].persisted));
		memset(staged[i].persisted_awarded, 0, sizeof(staged[i].persisted_awarded));
	}

	/* 2. queued awards, with their exact time */
	for (int i = 0; i < nqueue; i++)
	{
		PgaQueuedAward *qa = &queue[i];
		uint64		bit = UINT64CONST(1) << (qa->slot % 64);
		PgaStaged  *st;

		entry = pga_find_role(NameStr(qa->rolname), false);
		if (entry == NULL || qa->slot >= pga_shared->catalog_size)
			continue;
		st = pga_staged_for(entry);
		if (st == NULL)
			continue;			/* cannot happen, see above; step 3 would catch it */
		if (!pga_bit_test(entry->awarded, qa->slot))
			continue;			/* reset meanwhile */
		if (st->persisted_awarded[qa->slot / 64] & bit)
			continue;
		pga_write_award(entry, qa->slot, qa->awarded_at, true);
		st->persisted_awarded[qa->slot / 64] |= bit;
	}

	/* 3. awards missing from the queue (overflow), and counter values */
	for (int i = 0; i < nstaged; i++)
	{
		PgaStaged  *st = &staged[i];

		entry = st->entry;

		for (int s = 0; s < pga_shared->catalog_size; s++)
		{
			uint64		bit = UINT64CONST(1) << (s % 64);

			if (pga_bit_test(entry->awarded, s) &&
				!(st->persisted_awarded[s / 64] & bit))
			{
				pga_write_award(entry, s, 0, false);
				st->persisted_awarded[s / 64] |= bit;
			}
		}

		for (int c = 0; c < PGA_NUM_COUNTERS; c++)
		{
			int64		value = (int64) pg_atomic_read_u64(&entry->values[c]);

			/*
			 * Counters only grow.  A smaller value means that a reset has
			 * zeroed shared memory and its request has not been processed
			 * yet: the next cycle deletes the rows and writes from zero.
			 */
			if (value <= st->persisted[c])
				continue;

			pga_write_counter(entry, c,
							  pga_counter_info[c].kind == PGA_SUM ?
							  value - st->persisted[c] : value);
			st->persisted[c] = value;
		}
	}
	return true;
}

/*
 * After the commit of a cycle: the tables now hold what the staged
 * bookkeeping says, publish it for the next cycle.
 */
static void
pga_publish_progress(void)
{
	for (int i = 0; i < nstaged; i++)
	{
		PgaRoleEntry *entry = staged[i].entry;

		memcpy(entry->persisted, staged[i].persisted, sizeof(entry->persisted));
		memcpy(entry->persisted_awarded, staged[i].persisted_awarded,
			   sizeof(entry->persisted_awarded));
	}
	nstaged = 0;
}

/*
 * One cycle in one transaction.  Errors propagate to pga_worker_main, which
 * aborts the transaction; the staged bookkeeping of the failed cycle is
 * simply dropped, so the next cycle writes the same changes again.
 * Returns false if writing had to be skipped (busy tables), so that
 * achievements.sync() keeps waiting.  "last" is the cycle before exit.
 */
static bool
pga_cycle(bool last)
{
	bool		exists;
	bool		complete = true;
	bool		ready = pg_atomic_read_u32(&pga_shared->ready) != 0;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "pg_achievements: flush");

	pga_exec("SELECT 1 FROM pg_catalog.pg_extension WHERE extname = 'pg_achievements'",
			 0, NULL, NULL, SPI_OK_SELECT);
	exists = SPI_processed > 0;

	if (!exists)
	{
		if (ready)
		{
			/* the extension was dropped: forget everything */
			pg_atomic_write_u32(&pga_shared->ready, 0);
			pga_reset_all_entries();
			LWLockAcquire(pga_shared->lock, LW_EXCLUSIVE);
			pga_shared->catalog_size = 0;
			memset(pga_shared->catalog, 0, sizeof(pga_shared->catalog));
			pg_atomic_fetch_add_u32(&pga_shared->catalog_version, 1);
			LWLockRelease(pga_shared->lock);
		}
	}
	else
	{
		pga_load_catalog();

		if (!ready)
		{
			pga_load_progress();
			pg_atomic_write_u32(&pga_shared->ready, 1);
		}

		if (!RecoveryInProgress())
			complete = pga_write_progress(last);
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pga_publish_progress();
	pgstat_report_stat(false);
	pgstat_report_activity(STATE_IDLE, NULL);
	return complete;
}

/*
 * Release the claim of pga_claim_worker and do not leave a stale latch
 * pointer behind for backends to set.  No lock: only the owner clears the
 * claim, and taking a lock while exiting could wait for ourselves.
 */
static void
pga_worker_exit(int code, Datum arg)
{
	if (pga_shared->worker_pid == MyProcPid)
	{
		pga_shared->worker_latch = NULL;
		pg_write_barrier();
		pga_shared->worker_pid = 0;
	}
}

/*
 * Become the one flush worker, or return false if another one runs.  The
 * claim is released on exit by pga_worker_exit; a crash reinitializes
 * shared memory, so no stale claim survives.
 */
static bool
pga_claim_worker(void)
{
	pid_t		other;

	LWLockAcquire(pga_shared->lock, LW_EXCLUSIVE);
	other = pga_shared->worker_pid;
	if (other == 0)
	{
		pga_shared->worker_pid = MyProcPid;
		pga_shared->worker_latch = MyLatch;
	}
	LWLockRelease(pga_shared->lock);

	if (other != 0)
	{
		ereport(LOG,
				(errmsg("pg_achievements: worker with PID %d is already running",
						(int) other)));
		return false;
	}
	before_shmem_exit(pga_worker_exit, (Datum) 0);
	return true;
}

/*
 * Main loop of the flush worker.  A failed cycle is reported and retried
 * after at least a second; the worker itself keeps running.  After SIGTERM
 * one last cycle runs so that a clean shutdown loses nothing that backends
 * have already flushed to shared memory.
 */
void
pga_worker_main(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, pga_worker_sigterm);
	BackgroundWorkerUnblockSignals();

	/* before connecting, so a refused worker does not touch the database */
	if (!pga_claim_worker())
		proc_exit(0);

	BackgroundWorkerInitializeConnection(pga_database, NULL, 0);

	cycle_cxt = AllocSetContextCreate(TopMemoryContext,
									  "pg_achievements worker cycle",
									  ALLOCSET_DEFAULT_SIZES);

	ereport(LOG,
			(errmsg("pg_achievements worker started, database \"%s\"", pga_database)));

	for (;;)
	{
		uint64		request;
		bool		ok = false;
		bool		complete = false;
		bool		stopping = got_sigterm;
		MemoryContext oldcxt;

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		request = pg_atomic_read_u64(&pga_shared->flush_requests);

		MemoryContextReset(cycle_cxt);
		resets_done = NULL;
		nresets_done = 0;
		staged = NULL;
		nstaged = 0;
		oldcxt = MemoryContextSwitchTo(cycle_cxt);

		PG_TRY();
		{
			complete = pga_cycle(stopping);
			ok = true;
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(cycle_cxt);
			EmitErrorReport();
			FlushErrorState();
			AbortCurrentTransaction();
			for (int i = 0; i < nresets_done; i++)
				pg_atomic_write_u32(&resets_done[i]->reset_requested, 1);
			nstaged = 0;		/* the bookkeeping of this cycle is void */
		}
		PG_END_TRY();

		MemoryContextSwitchTo(oldcxt);

		if (ok && complete)
			pg_atomic_write_u64(&pga_shared->flush_done, request);

		if (stopping)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ok ? pga_flush_interval_ms : Max(pga_flush_interval_ms, 1000),
						 PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		/* absorbs ProcSignalBarriers, e.g. from DROP DATABASE */
		CHECK_FOR_INTERRUPTS();
	}

	proc_exit(1);
}
