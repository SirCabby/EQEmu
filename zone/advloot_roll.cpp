/*	EQEmu: EQEmulator

	akk-stack custom -- Advanced Looting System: Need/Greed roll sessions. See advloot_roll.h.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include "zone/advloot_roll.h"

#include "common/eq_constants.h"
#include "common/say_link.h"
#include "common/strings.h"
#include "zone/client.h"
#include "zone/corpse.h"
#include "zone/entity.h"
#include "zone/groups.h"
#include "zone/zone.h"
#include "zone/zonedb.h"

#include <algorithm>

AdvLootRollManager adv_loot_rolls;

// A clickable item link for the roll chat lines (falls back to the plain name if the item is missing).
// Sent via Client::Message, so each recipient's client translator renders the link for their version.
static std::string adv_item_link(uint32 item_id, const std::string &fallback)
{
	const EQ::ItemData *item = database.GetItem(item_id);
	if (!item) {
		return fallback;
	}

	EQ::SayLinkEngine linker;
	linker.SetLinkType(EQ::saylink::SayLinkItemData);
	linker.SetItemData(item);
	return linker.GenerateLink();
}

// Does this player's saved preference auto-sell this item on loot? An auto-sold win is paid out as
// coin and never enters inventory, so -- exactly like AdvLootItem -- it is exempt from the lore check.
static bool adv_always_sells(uint32 character_id, uint32 item_id)
{
	auto rs = database.QueryDatabase(fmt::format(
		"SELECT `sell` FROM `character_loot_never` WHERE `character_id` = {} AND `item_id` = {}",
		character_id, item_id
	));

	return rs.Success() && rs.RowCount() && atoi(rs.begin()[0]) != 0;
}

uint32 AdvLootRollManager::CorpseSecondsLeft(uint16 corpse_id)
{
	Corpse *corpse = entity_list.GetCorpseByID(corpse_id);
	if (!corpse) {
		return 0;
	}

	const uint32 ms = corpse->GetDecayTime(); // 0xFFFFFFFF == no decay timer running
	if (ms == 0xFFFFFFFF) {
		return 0;
	}

	return ms / 1000;
}

AdvLootRollManager::Session *AdvLootRollManager::Find(uint16 corpse_id, uint32 adv_uid)
{
	for (auto &s : m_sessions) {
		if (s.corpse_id == corpse_id && s.adv_uid == adv_uid) {
			return &s;
		}
	}

	return nullptr;
}

void AdvLootRollManager::Erase(const Session *s)
{
	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
		if (&(*it) == s) {
			m_sessions.erase(it);
			return;
		}
	}
}

// Chat line to everyone the roll concerns (offline / zoned-out members are simply skipped).
void AdvLootRollManager::Announce(const Session &s, const std::string &message, uint16 chat_type)
{
	for (const uint32 character_id : s.eligible) {
		Client *c = entity_list.GetClientByCharID(character_id);
		if (c) {
			c->Message(chat_type, message.c_str());
		}
	}
}

// "ALW5|<corpse_id>,<adv_uid>,<need>,<greed>,<secs>,<locked>,<voted>" -- the row's live tally, the
// whole seconds left before the corpse rots (the window's per-row countdown), whether the controller
// has locked it out of rolling, and whether THIS recipient has already answered. The voted flag is the
// only per-player field, and it is what lets a resynced window (zone-in) know which rows still want an
// answer -- the client's own click-time flag does not survive the rebuild.
void AdvLootRollManager::SendTallyTo(const Session &s, Client *c)
{
	if (!c) {
		return;
	}

	uint32 need  = 0;
	uint32 greed = 0;
	for (const auto &v : s.votes) {
		if (v.second == VoteNeed) {
			++need;
		}
		else if (v.second == VoteGreed) {
			++greed;
		}
	}

	c->SendMarqueeMessage(
		0,
		fmt::format(
			"ALW5|{},{},{},{},{},{},{}",
			s.corpse_id,
			s.adv_uid,
			need,
			greed,
			CorpseSecondsLeft(s.corpse_id),
			s.locked ? 1 : 0,
			s.votes.count(c->CharacterID()) ? 1 : 0
		),
		1000
	);
}

void AdvLootRollManager::SendTally(const Session &s)
{
	for (const uint32 character_id : s.eligible) {
		SendTallyTo(s, entity_list.GetClientByCharID(character_id));
	}
}

// "ALW2|<corpse_id>,<adv_uid>" -- the item left the corpse, so drop the row from every window.
void AdvLootRollManager::SendRowRemoved(const Session &s)
{
	const std::string msg = fmt::format("ALW2|{},{}", s.corpse_id, s.adv_uid);
	for (const uint32 character_id : s.eligible) {
		Client *c = entity_list.GetClientByCharID(character_id);
		if (c) {
			c->SendMarqueeMessage(0, msg, 1000);
		}
	}
}

void AdvLootRollManager::Open(
	uint16                     corpse_id,
	uint32                     adv_uid,
	uint32                     item_id,
	const std::string         &item_name,
	const std::vector<uint32> &eligible,
	bool                       locked
)
{
	if (eligible.empty() || Find(corpse_id, adv_uid)) {
		return;
	}

	Session s;
	s.corpse_id = corpse_id;
	s.adv_uid   = adv_uid;
	s.item_id   = item_id;
	s.item_name = item_name;
	s.eligible  = eligible;
	s.locked    = locked;
	m_sessions.push_back(s);

	SendTally(m_sessions.back()); // show the countdown + (empty) tally on the row immediately
}

void AdvLootRollManager::MarkSellOnWin(Client *c, uint16 corpse_id, uint32 adv_uid, bool on)
{
	if (!c) {
		return;
	}

	Session *s = Find(corpse_id, adv_uid);
	if (!s) {
		return;
	}

	const uint32 character_id = c->CharacterID();
	if (std::find(s->eligible.begin(), s->eligible.end(), character_id) == s->eligible.end()) {
		return; // only an eligible looter may flag the drop
	}

	auto it = std::find(s->sell_on_win.begin(), s->sell_on_win.end(), character_id);
	if (on && it == s->sell_on_win.end()) {
		s->sell_on_win.push_back(character_id);
	}
	else if (!on && it != s->sell_on_win.end()) {
		s->sell_on_win.erase(it);
	}
}

void AdvLootRollManager::Cast(Client *voter, uint16 corpse_id, uint32 adv_uid, Vote vote)
{
	if (!voter || vote == VoteNone) {
		return;
	}

	Session *s = Find(corpse_id, adv_uid);
	if (!s) {
		return;
	}

	// LOCKED is the rolling state (grouped default): eligible looters vote Need/Greed/Pass and the item
	// auto-resolves. (Unlocked = free-for-all, where the window shows Loot/Sell instead of vote buttons,
	// so no votes arrive here anyway.) Do NOT reject a vote on a locked row.
	const uint32 character_id = voter->CharacterID();
	if (std::find(s->eligible.begin(), s->eligible.end(), character_id) == s->eligible.end()) {
		return; // not part of this roll
	}

	if (s->votes.count(character_id)) {
		voter->Message(Chat::White, "[AdvLoot] You have already rolled on %s.", adv_item_link(s->item_id, s->item_name).c_str());
		return;
	}

	s->votes[character_id] = vote;

	if (vote == VotePass) {
		Announce(*s, fmt::format("[AdvLoot] {} passes on {}.", voter->GetCleanName(), adv_item_link(s->item_id, s->item_name)));
	}
	else {
		const int roll         = zone->random.Int(1, 1000);
		s->rolls[character_id] = roll;
		Announce(
			*s,
			fmt::format("[AdvLoot] {} rolls {} ({}) on {}.",
						voter->GetCleanName(), roll, vote == VoteNeed ? "Need" : "Greed", adv_item_link(s->item_id, s->item_name))
		);
	}

	// Resolve as soon as everyone eligible has answered; otherwise the roll simply keeps standing
	// until the corpse rots. (No fixed deadline -- the corpse's decay clock is the only timer.)
	if (s->votes.size() >= s->eligible.size()) {
		Resolve(*s);
		Erase(s);
	}
	else {
		SendTally(*s); // reflect the new Need/Greed count on everyone's row right away
	}
}

void AdvLootRollManager::SetLocked(uint16 corpse_id, uint32 adv_uid, bool locked)
{
	Session *s = Find(corpse_id, adv_uid);
	if (!s || s->locked == locked) {
		return;
	}

	s->locked = locked;
	Announce(
		*s,
		fmt::format("[AdvLoot] {} was {} by the master looter.",
					adv_item_link(s->item_id, s->item_name), locked ? "locked" : "unlocked")
	);
	SendTally(*s); // repaint the row into / out of its locked (Loot/Give) controls at once
}

void AdvLootRollManager::Cancel(uint16 corpse_id, uint32 adv_uid)
{
	Erase(Find(corpse_id, adv_uid));
}

bool AdvLootRollManager::IsLocked(uint16 corpse_id, uint32 adv_uid)
{
	Session *s = Find(corpse_id, adv_uid);
	return s && s->locked;
}

bool AdvLootRollManager::CorpseHasLock(uint16 corpse_id)
{
	for (const auto &s : m_sessions) {
		if (s.corpse_id == corpse_id && s.locked) {
			return true;
		}
	}
	return false;
}

void AdvLootRollManager::ApplyPrefToLive(Client *c, uint32 item_id, uint8 mode)
{
	if (!c) {
		return;
	}

	// Never auto-passes, Need/Greed auto-roll -- i.e. the saved preference votes for the player on any
	// item of this type they are currently being asked to roll on. Cast() ignores rows they have already
	// answered and rows they are not part of, so a stray match is harmless. (Locked IS the rolling state,
	// so a locked row does accept this auto-vote -- that is how a grouped always-need fires.)
	const AdvLootRollManager::Vote vote =
		(mode == AdvLootPref::Need)  ? VoteNeed :
		(mode == AdvLootPref::Greed) ? VoteGreed : VotePass;

	// snapshot the matching (corpse,uid) first -- Cast() can resolve+erase the session mid-iteration
	std::vector<std::pair<uint16, uint32>> hits;
	for (const auto &s : m_sessions) {
		if (s.item_id == item_id) {
			hits.emplace_back(s.corpse_id, s.adv_uid);
		}
	}
	for (const auto &h : hits) {
		Cast(c, h.first, h.second, vote);
	}
}

void AdvLootRollManager::Resolve(Session &s)
{
	Corpse *corpse = entity_list.GetCorpseByID(s.corpse_id);

	// Rank the rollers: every Need (highest first), then every Greed (highest first). A Need beats
	// every Greed outright, so the two tiers are simply walked in order. Pass never wins.
	struct Candidate {
		uint32 character_id;
		int    roll;
		uint8  vote;
	};

	std::vector<Candidate> ranked;
	for (const uint8 wanted : {VoteNeed, VoteGreed}) {
		std::vector<Candidate> tier;
		for (const auto &v : s.votes) {
			if (v.second != wanted) {
				continue;
			}

			const auto r = s.rolls.find(v.first);
			if (r == s.rolls.end()) {
				continue;
			}

			tier.push_back({v.first, r->second, wanted});
		}

		std::sort(tier.begin(), tier.end(), [](const Candidate &a, const Candidate &b) { return a.roll > b.roll; });
		ranked.insert(ranked.end(), tier.begin(), tier.end());
	}

	// Walk that ranking and award the drop to the highest roller who can actually hold it. A LORE item
	// the roller already owns would just bounce off their inventory and the whole roll would be wasted,
	// so it falls through to the next-highest roller who does NOT have one. (A win that gets converted
	// to coin -- single-sell or the persistent always-sell -- never enters inventory, so it is
	// lore-exempt, the same carve-out AdvLootItem makes.)
	uint32 winner_id  = 0;
	int    best       = 0;
	uint8  best_vote  = VoteNone;
	bool   sell       = false;
	bool   lore_skips = false;

	for (const auto &cand : ranked) {
		Client *c = entity_list.GetClientByCharID(cand.character_id);

		const bool cand_sell =
			std::find(s.sell_on_win.begin(), s.sell_on_win.end(), cand.character_id) != s.sell_on_win.end();

		if (c && corpse && !cand_sell && corpse->AdvLoreBlocked(c, s.adv_uid) &&
			!adv_always_sells(cand.character_id, s.item_id)) {
			Announce(
				s,
				fmt::format("[AdvLoot] {} rolled {} but already has a lore {} -- passing to the next roller.",
							c->GetCleanName(), cand.roll, adv_item_link(s.item_id, s.item_name))
			);
			lore_skips = true;
			continue;
		}

		winner_id = cand.character_id;
		best      = cand.roll;
		best_vote = cand.vote;
		sell      = cand_sell;
		break;
	}

	if (!winner_id) {
		Announce(
			s,
			lore_skips
				? fmt::format("[AdvLoot] Everyone who rolled on {} already has one -- it stays on the corpse.",
							  adv_item_link(s.item_id, s.item_name))
				: fmt::format("[AdvLoot] Everyone passed on {} -- it stays on the corpse.",
							  adv_item_link(s.item_id, s.item_name))
		);
		SendRowRemoved(s); // nobody can take it; clear the decided row from every window
		return;
	}

	Client *winner = entity_list.GetClientByCharID(winner_id);

	// If the winner flagged this drop for single-sell, hand them coin instead of the item (AdvSellItem);
	// otherwise loot it normally (AdvLootItem already honors their persistent "always sell").
	const bool ok = winner && corpse &&
	                (sell ? corpse->AdvSellItem(winner, s.adv_uid) : corpse->AdvLootItem(winner, s.adv_uid));

	if (!ok) {
		Announce(s, fmt::format("[AdvLoot] {} could not be awarded -- it stays on the corpse.", adv_item_link(s.item_id, s.item_name)));
		SendRowRemoved(s);
		return;
	}

	// The one line players actually watch for, so it gets its own recolorable channel (the rest of the
	// roll commentary stays plain white). See Chat::AdvLootWin in common/eq_constants.h.
	Announce(
		s,
		fmt::format("[AdvLoot] {} wins {} with a {} roll of {}.",
					winner->GetCleanName(), adv_item_link(s.item_id, s.item_name), best_vote == VoteNeed ? "Need" : "Greed", best),
		Chat::AdvLootWin
	);
	SendRowRemoved(s); // the item is off the corpse now -- clear the row everywhere
}

void AdvLootRollManager::Process()
{
	// Drop any session whose corpse has rotted: the item is gone, so clear its row everywhere.
	for (auto it = m_sessions.begin(); it != m_sessions.end();) {
		if (!entity_list.GetCorpseByID(it->corpse_id)) {
			SendRowRemoved(*it);
			it = m_sessions.erase(it);
			continue;
		}

		++it;
	}

	if (!m_refresh.Check()) {
		return;
	}

	// Keep every window's per-row countdown (and Need/Greed tally) moving as the corpse decays. Also
	// AUTO-UNLOCK any still-locked (rolling) drop once its corpse is in its last minute before rotting:
	// a stalled roll (an eligible member who never answered) would otherwise let the loot rot locked, so
	// we flip it to a free-for-all in time for anyone to grab it. Announce once per corpse -- all of a
	// corpse's drops share its decay clock, so they cross the 60s threshold on the same tick.
	std::vector<uint16> unlock_warned;
	for (auto &s : m_sessions) {
		if (s.locked) {
			const uint32 secs = CorpseSecondsLeft(s.corpse_id);
			if (secs > 0 && secs <= 60) {
				s.locked = false;
				if (std::find(unlock_warned.begin(), unlock_warned.end(), s.corpse_id) == unlock_warned.end()) {
					Announce(s, "[AdvLoot] A corpse is about to rot -- its loot is now free-for-all, grab it!");
					unlock_warned.push_back(s.corpse_id);
				}
			}
		}
		SendTally(s);
	}
}

// Rebuild one client's window from live zone state (see the header). The record format is byte-for-byte
// the death-time push in attack.cpp -- the client parses ALW1 one way -- so the two must stay in step.
void AdvLootRollManager::SendListTo(Client *c)
{
	if (!c) {
		return;
	}

	// Always first: drop whatever the window is still holding. Rows survive zoning client-side (the .asi
	// lives for the whole client process), so without this a player carries the last zone's list around,
	// and its corpse entity ids may now name entirely different corpses here.
	c->SendMarqueeMessage(0, "ALW0", 1000);

	const uint32 character_id = c->CharacterID();
	Group       *group        = c->GetGroup();

	// The Looter dropdown is window state too, and the group may have changed while they were away.
	// Unconditional: solo, the client needs the explicit "ALWg|0" to stop believing it is still grouped.
	if (group) {
		group->SendAdvLootLooterList(c);
	}
	else {
		c->SendMarqueeMessage(0, "ALWg|0", 1000);
	}

	// Which of this zone's open drops is this player part of? Sessions exist for exactly the rows that
	// belong on a window, and each carries the audience it was opened against.
	std::vector<Session *> mine;
	for (auto &s : m_sessions) {
		if (std::find(s.eligible.begin(), s.eligible.end(), character_id) == s.eligible.end()) {
			continue;
		}
		if (!entity_list.GetCorpseByID(s.corpse_id)) {
			continue; // rotting corpse not swept yet; Process() will erase the session on its next tick
		}
		mine.push_back(&s);
	}

	if (mine.empty()) {
		return; // nothing here for them -- the clear above is the whole message (an empty ALW1 would
		        // pop the window open on every zone-in)
	}

	// Their saved per-item actions, same as the death push: mode 0 = never (don't list it at all).
	std::map<uint32, uint8> pref;
	std::map<uint32, uint8> sell;
	{
		auto rows = database.QueryDatabase(fmt::format(
			"SELECT `item_id`, `mode`, `sell` FROM `character_loot_never` WHERE `character_id` = {}",
			character_id
		));
		for (auto row : rows) {
			const uint32 item_id = (uint32) atoi(row[0]);
			pref[item_id] = (uint8) atoi(row[1]);
			sell[item_id] = (uint8) atoi(row[2]);
		}
	}

	const int hdr = ((group ? group->IsLootController(c) : true) ? 1 : 0) | (group ? 2 : 0);

	std::string            msg = "ALW1|" + std::to_string(hdr);
	std::vector<Session *> sent;
	for (auto *s : mine) {
		const auto  pit  = pref.find(s->item_id);
		const bool  has  = (pit != pref.end());
		const uint8 mode = has ? pit->second : 0;
		if (has && mode == 0) {
			continue; // never: they asked not to see it
		}

		const EQ::ItemData *item = database.GetItem(s->item_id);
		if (!item) {
			continue;
		}

		Corpse *corpse = entity_list.GetCorpseByID(s->corpse_id);

		msg += fmt::format(
			"|{},{},{},{},{},{},{},{},{}",
			s->corpse_id,
			s->adv_uid,
			s->item_id,
			(unsigned) (has ? mode : 255),
			(unsigned) ((has && sell.count(s->item_id)) ? sell[s->item_id] : 0),
			(unsigned) item->Icon,
			(unsigned) item->Price,
			corpse ? corpse->GetCleanName() : "a corpse",
			item->Name
		);

		sent.push_back(s);
		if (sent.size() >= 20) {
			break;
		}
	}

	if (sent.empty()) {
		return;
	}

	c->SendMarqueeMessage(0, msg, 1000);

	// Countdown, lock state, and (per-player) whether they have already answered each roll.
	for (auto *s : sent) {
		SendTallyTo(*s, c);
	}
}
