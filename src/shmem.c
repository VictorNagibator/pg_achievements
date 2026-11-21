/*-------------------------------------------------------------------------
 *
 * shmem.c
 *		Shared memory structures of pg_achievements.
 *
 * Layout:
 *	- PgaShared: worker state, catalog snapshot, award queue;
 *	- a shared hash table of PgaRoleEntry keyed by role name.
 *
 * Counter values and award bits are atomics, so backends add their deltas
 * and claim awards without exclusive locks.  The LWLock protects the hash
 * table structure (shared for lookups, exclusive only to insert a new role)
 * and the catalog snapshot (exclusive only when the worker reloads it).
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"

PgaShared  *pga_shared = NULL;
HTAB	   *pga_roles = NULL;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Shared memory needed: the fixed struct plus the role hash table. */
static Size
pga_shmem_size(void)
{
	Size		size;

	size = MAXALIGN(sizeof(PgaShared));
	size = add_size(size, hash_estimate_size(pga_max_roles, sizeof(PgaRoleEntry)));
	return size;
}

/* shmem_request_hook: reserve memory and one named LWLock. */
static void
pga_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pga_shmem_size());
	RequestNamedLWLockTranche(PGA_EXTNAME, 1);
}

/* shmem_startup_hook: create or attach the shared structures. */
static void
pga_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	pga_shared = NULL;
	pga_roles = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pga_shared = ShmemInitStruct("pg_achievements", sizeof(PgaShared), &found);
	if (!found)
	{
		memset(pga_shared, 0, sizeof(PgaShared));
		pga_shared->lock = &(GetNamedLWLockTranche(PGA_EXTNAME))->lock;
		SpinLockInit(&pga_shared->queue_mutex);
		pg_atomic_init_u32(&pga_shared->ready, 0);
		pg_atomic_init_u32(&pga_shared->catalog_version, 0);
		pg_atomic_init_u64(&pga_shared->flush_requests, 0);
		pg_atomic_init_u64(&pga_shared->flush_done, 0);
		pg_atomic_init_u32(&pga_shared->hash_full_warned, 0);
	}

	memset(&info, 0, sizeof(info));
	info.keysize = NAMEDATALEN;
	info.entrysize = sizeof(PgaRoleEntry);
	/*
	 * PostgreSQL 19 dropped the separate initial size: a shared hash is
	 * always allocated at its full size up front, which is what we asked
	 * for anyway.
	 */
	pga_roles = ShmemInitHash("pg_achievements roles",
#if PG_VERSION_NUM >= 190000
							  pga_max_roles,
#else
							  pga_max_roles, pga_max_roles,
#endif
							  &info,
							  HASH_ELEM | HASH_STRINGS);

	LWLockRelease(AddinShmemInitLock);
}

/* Install the shared memory hooks; called from _PG_init. */
void
pga_shmem_install_hooks(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pga_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pga_shmem_startup;
}

/* Initialize a freshly inserted role entry (atomics need explicit init). */
static void
pga_init_entry(PgaRoleEntry *entry)
{
	for (int i = 0; i < PGA_NUM_COUNTERS; i++)
	{
		pg_atomic_init_u64(&entry->values[i], 0);
		entry->persisted[i] = 0;
	}
	for (int i = 0; i < PGA_AWARD_WORDS; i++)
	{
		pg_atomic_init_u64(&entry->awarded[i], 0);
		entry->persisted_awarded[i] = 0;
	}
	pg_atomic_init_u32(&entry->reset_requested, 0);
}

/*
 * Look up the shared entry of a role.  Entries are never removed while the
 * server runs, so the returned pointer stays valid and may be cached.
 */
PgaRoleEntry *
pga_find_role(const char *rolname, bool create)
{
	NameData	key;
	PgaRoleEntry *entry;
	bool		found;

	if (pga_roles == NULL)
		return NULL;

	memset(&key, 0, sizeof(key));
	strlcpy(NameStr(key), rolname, NAMEDATALEN);

	LWLockAcquire(pga_shared->lock, LW_SHARED);
	entry = (PgaRoleEntry *) hash_search(pga_roles, &key, HASH_FIND, NULL);
	LWLockRelease(pga_shared->lock);

	if (entry != NULL || !create)
		return entry;

	LWLockAcquire(pga_shared->lock, LW_EXCLUSIVE);
	entry = (PgaRoleEntry *) hash_search(pga_roles, &key, HASH_ENTER_NULL, &found);
	if (entry != NULL && !found)
		pga_init_entry(entry);
	LWLockRelease(pga_shared->lock);

	if (entry == NULL &&
		pg_atomic_exchange_u32(&pga_shared->hash_full_warned, 1) == 0)
		ereport(WARNING,
				(errmsg("pg_achievements: too many roles, new roles are not tracked"),
				 errhint("Increase pg_achievements.max_roles.")));

	return entry;
}

/*
 * Zero the progress of a role.  The worker notices reset_requested and
 * deletes the rows of the role from the tables.
 */
void
pga_reset_role_entry(PgaRoleEntry *entry)
{
	for (int i = 0; i < PGA_NUM_COUNTERS; i++)
		pg_atomic_write_u64(&entry->values[i], 0);
	for (int i = 0; i < PGA_AWARD_WORDS; i++)
		pg_atomic_write_u64(&entry->awarded[i], 0);
	pga_purge_queue_role(NameStr(entry->rolname));
	pg_atomic_write_u32(&entry->reset_requested, 1);
}

/* Worker only: forget all progress (the extension was dropped). */
void
pga_reset_all_entries(void)
{
	HASH_SEQ_STATUS status;
	PgaRoleEntry *entry;

	LWLockAcquire(pga_shared->lock, LW_SHARED);
	hash_seq_init(&status, pga_roles);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		for (int i = 0; i < PGA_NUM_COUNTERS; i++)
		{
			pg_atomic_write_u64(&entry->values[i], 0);
			entry->persisted[i] = 0;
		}
		for (int i = 0; i < PGA_AWARD_WORDS; i++)
		{
			pg_atomic_write_u64(&entry->awarded[i], 0);
			entry->persisted_awarded[i] = 0;
		}
		pg_atomic_write_u32(&entry->reset_requested, 0);
	}
	LWLockRelease(pga_shared->lock);

	SpinLockAcquire(&pga_shared->queue_mutex);
	pga_shared->queue_len = 0;
	pga_shared->queue_head = 0;
	SpinLockRelease(&pga_shared->queue_mutex);
}

/*
 * Put a fresh award into the queue.  When the queue is full the award is
 * still recorded in the role bitmap, and the worker writes it with the time
 * of the flush instead of the exact time.
 */
bool
pga_enqueue_award(const char *rolname, int slot, TimestampTz ts)
{
	bool		ok = false;

	SpinLockAcquire(&pga_shared->queue_mutex);
	if (pga_shared->queue_len < PGA_AWARD_QUEUE_SIZE)
	{
		int			pos = (pga_shared->queue_head + pga_shared->queue_len) % PGA_AWARD_QUEUE_SIZE;
		PgaQueuedAward *qa = &pga_shared->queue[pos];

		memset(&qa->rolname, 0, sizeof(NameData));
		strlcpy(NameStr(qa->rolname), rolname, NAMEDATALEN);
		qa->slot = slot;
		qa->awarded_at = ts;
		pga_shared->queue_len++;
		ok = true;
	}
	SpinLockRelease(&pga_shared->queue_mutex);

	return ok;
}

/* Drop the queued awards of a role (used by reset). */
void
pga_purge_queue_role(const char *rolname)
{
	PgaQueuedAward tmp[PGA_AWARD_QUEUE_SIZE];
	int			n = 0;

	SpinLockAcquire(&pga_shared->queue_mutex);
	for (int i = 0; i < pga_shared->queue_len; i++)
	{
		PgaQueuedAward *qa = &pga_shared->queue[(pga_shared->queue_head + i) % PGA_AWARD_QUEUE_SIZE];

		if (strcmp(NameStr(qa->rolname), rolname) != 0)
			tmp[n++] = *qa;
	}
	memcpy(pga_shared->queue, tmp, n * sizeof(PgaQueuedAward));
	pga_shared->queue_head = 0;
	pga_shared->queue_len = n;
	SpinLockRelease(&pga_shared->queue_mutex);
}

/* Worker only: take up to max queued awards. */
int
pga_drain_queue(PgaQueuedAward *out, int max)
{
	int			n;

	SpinLockAcquire(&pga_shared->queue_mutex);
	n = Min(max, pga_shared->queue_len);
	for (int i = 0; i < n; i++)
		out[i] = pga_shared->queue[(pga_shared->queue_head + i) % PGA_AWARD_QUEUE_SIZE];
	pga_shared->queue_head = (pga_shared->queue_head + n) % PGA_AWARD_QUEUE_SIZE;
	pga_shared->queue_len -= n;
	SpinLockRelease(&pga_shared->queue_mutex);

	return n;
}
