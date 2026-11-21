/*-------------------------------------------------------------------------
 *
 * notice.c
 *		Client notification about a freshly unlocked achievement.
 *
 *	NOTICE:  🥇 [GOLD] Achievement unlocked: Forgot WHERE (+50)
 *	DETAIL:  UPDATE without a WHERE clause. Total score: 135.
 *	Unlocked by 2 of 15 players (13%).
 *
 * The "collect everything" achievement (meta:all, the platinum one) gets a
 * bigger celebration, see pga_send_platinum_notice().
 *
 *-------------------------------------------------------------------------
 */
#include "pg_achievements.h"

#include "lib/stringinfo.h"
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

/*
 * Number of active builtin achievements the role holds: the platinum
 * achievement is about the builtin catalog, custom ones do not count.
 */
static int
pga_count_awarded_builtin(PgaRoleEntry *entry)
{
	int			n = 0;

	for (int s = 0; s < PGA_MAX_CATALOG; s++)
	{
		const PgaCatalogSlot *slot = pga_local_slot(s);

		if (slot == NULL)
			break;
		if (slot->used && slot->active && slot->builtin &&
			pga_bit_test(entry->awarded, s))
			n++;
	}
	return n;
}

/* Emoji are used only if asked for and both sides speak UTF8. */
static bool
pga_use_emoji(void)
{
	return pga_notice_style == PGA_NOTICE_EMOJI &&
		GetDatabaseEncoding() == PG_UTF8 &&
		pg_get_client_encoding() == PG_UTF8;
}

/* Description with a full stop at the end, for the DETAIL line. */
static const char *
pga_sentence_end(const char *desc)
{
	size_t		len = strlen(desc);

	return (len > 0 && strchr(".!?", desc[len - 1]) != NULL) ? "" : ".";
}

/*
 * The elephant on the platinum notice: Slonik, the PostgreSQL mascot, seen
 * from the front.  Plain ASCII, so it looks the same in both notice styles.
 */
static const char *const slonik_art =
"\n"
"            _.-\"\"\"\"-._\n"
"          .'          '.\n"
"     .--./  (o)    (o)  \\.--.\n"
"    (    |              |    )\n"
"     '--'\\      __      /'--'\n"
"          '.   |  |   .'\n"
"            '-.|  |.-'\n"
"               |  |\n"
"               |  |\n"
"               '--'\n";

/*
 * Notice of the platinum "collect everything" achievement: the usual first
 * line, then the elephant and a congratulation in DETAIL and a hint.
 */
static void
pga_send_platinum_notice(PgaRoleEntry *entry, const PgaCatalogSlot *slot,
						 bool emoji)
{
	StringInfoData detail;

	initStringInfo(&detail);
	appendStringInfo(&detail, "%s%s Total score: " INT64_FORMAT ".\n",
					 slot->description, pga_sentence_end(slot->description),
					 pga_total_score(entry));
	appendStringInfoString(&detail, slonik_art);
	appendStringInfo(&detail,
					 "\n%sC O N G R A T U L A T I O N S%s\n"
					 "You have collected all %d achievements.\n"
					 "Slonik bows to you: there is nothing left to unlock.",
					 emoji ? "\xF0\x9F\x8E\x89 " : "",	/* U+1F389 */
					 emoji ? " \xF0\x9F\x8E\x89" : "",
					 pga_count_awarded_builtin(entry));

	ereport(NOTICE,
			(errmsg("%s%s[%s] Achievement unlocked: %s (+%d)",
					emoji ? tier_medal[PGA_TIER_PLATINUM] : "",
					emoji ? " " : "",
					tier_upper[PGA_TIER_PLATINUM], slot->title, slot->points),
			 errdetail("%s", detail.data),
			 errhint("Go outside, touch grass, run VACUUM."),
			 errhidestmt(true),
			 errhidecontext(true)));
	pfree(detail.data);
}

/*
 * Rarity line of the DETAIL, like the "% of players" of game consoles, or
 * an empty string while the role plays alone: "1 of 1" says nothing.  The
 * role has just got the achievement, so it is among the holders.
 */
static char *
pga_rarity_line(int slot)
{
	int			holders;
	int			players;

	pga_slot_rarity(slot, &holders, &players);
	if (players < 2 || holders < 1)
		return pstrdup("");
	if (holders == 1)
		return psprintf("\nYou are the first of %d players to unlock it.",
						players);
	return psprintf("\nUnlocked by %d of %d players (%d%%).",
					holders, players,
					(int) ((holders * 100.0) / players + 0.5));
}

/* Send the "Achievement unlocked" notice for a slot to the client. */
void
pga_send_notice(PgaRoleEntry *entry, int s)
{
	const PgaCatalogSlot *slot = pga_local_slot(s);
	const char *medal = "";
	const char *sep = "";
	const char *desc;
	char	   *rarity;
	int			tier;
	bool		emoji = pga_use_emoji();

	if (slot == NULL)
		return;

	if (slot->counter == PGA_META_KIND_ALL && slot->tier == PGA_TIER_PLATINUM)
	{
		pga_send_platinum_notice(entry, slot, emoji);
		return;
	}

	tier = (slot->tier >= 0 && slot->tier < PGA_NUM_TIERS) ? slot->tier : 0;

	if (emoji)
	{
		medal = tier_medal[tier];
		sep = " ";
	}

	desc = slot->description;
	rarity = pga_rarity_line(s);

	/*
	 * Neither the statement nor the context goes to the log with this
	 * message: query texts must never be logged by the extension.
	 */
	ereport(NOTICE,
			(errmsg("%s%s[%s] Achievement unlocked: %s (+%d)",
					medal, sep, tier_upper[tier], slot->title, slot->points),
			 errdetail("%s%s Total score: " INT64_FORMAT ".%s",
					   desc, pga_sentence_end(desc),
					   pga_total_score(entry), rarity),
			 errhidestmt(true),
			 errhidecontext(true)));
	pfree(rarity);
}
