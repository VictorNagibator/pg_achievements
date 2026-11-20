/*-------------------------------------------------------------------------
 *
 * notice.c
 *		Client notification about a freshly unlocked achievement.
 *
 *	NOTICE:  🥇 [GOLD] Achievement unlocked: Forgot WHERE (+50)
 *	DETAIL:  UPDATE without a WHERE clause. Total score: 135.
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "mb/pg_wchar.h"

static const char *const tier_upper[PGA_NUM_TIERS] = {
	"BRONZE", "SILVER", "GOLD", "PLATINUM"
};

/* U+1F949, U+1F948, U+1F947, U+1F48E */
static const char *const tier_medal[PGA_NUM_TIERS] = {
	"\xF0\x9F\xA5\x89", "\xF0\x9F\xA5\x88", "\xF0\x9F\xA5\x87", "\xF0\x9F\x92\x8E"
};

/* Sum of points of all achievements the role holds. */
int64
pga_total_score(PgaRoleEntry *entry)
{
	int64		score = 0;

	for (int s = 0; s < PGA_MAX_CATALOG; s++)
	{
		const PgaCatalogSlot *slot = pga_local_slot(s);

		if (slot == NULL)
			break;
		if (slot->used && pga_bit_test(entry->awarded, s))
			score += slot->points;
	}
	return score;
}

void
pga_send_notice(PgaRoleEntry *entry, int s)
{
	const PgaCatalogSlot *slot = pga_local_slot(s);
	const char *medal = "";
	const char *sep = "";
	const char *desc;
	size_t		len;
	int			tier;

	if (slot == NULL)
		return;

	tier = (slot->tier >= 0 && slot->tier < PGA_NUM_TIERS) ? slot->tier : 0;

	if (pga_notice_style == PGA_NOTICE_EMOJI &&
		GetDatabaseEncoding() == PG_UTF8 &&
		pg_get_client_encoding() == PG_UTF8)
	{
		medal = tier_medal[tier];
		sep = " ";
	}

	desc = slot->description;
	len = strlen(desc);

	/*
	 * Neither the statement nor the context goes to the log with this
	 * message: query texts must never be logged by the extension.
	 */
	ereport(NOTICE,
			(errmsg("%s%s[%s] Achievement unlocked: %s (+%d)",
					medal, sep, tier_upper[tier], slot->title, slot->points),
			 errdetail("%s%s Total score: " INT64_FORMAT ".",
					   desc,
					   (len > 0 && strchr(".!?", desc[len - 1]) != NULL) ? "" : ".",
					   pga_total_score(entry)),
			 errhidestmt(true),
			 errhidecontext(true)));
}
