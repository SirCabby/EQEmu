/*	EQEmu: EQEmulator

	akk-stack custom -- Advanced Looting System: Need/Greed roll sessions.

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
#pragma once

#include "common/timer.h"
#include "common/types.h"

#include <list>
#include <map>
#include <string>
#include <vector>

class Client;

// akk-stack Advanced Looting: a player's saved permanent action for a specific item, persisted in
// character_loot_never.mode. No row = no preference (the player decides live). These values ARE the
// stored mode column, so do not renumber them.
namespace AdvLootPref {
	enum : uint8 {
		Never = 0, // don't list it / auto-pass -- the player never wants it
		Need  = 1, // auto-roll Need on it every time it drops
		Greed = 2, // auto-roll Greed on it every time it drops
	};
	static const uint8 Remove = 255; // wire-only sentinel: delete the saved preference (reset to none)
}

// akk-stack Advanced Looting: Need/Greed rolling.
//
// EVERY drop that reaches a player's Advanced Loot window is automatically up for a roll -- there is
// no "start a roll" step. Each eligible looter answers Need / Greed / Pass; once everyone has
// answered, the highest Need roll wins, falling back to the highest Greed roll. The item is handed
// straight to the winner (the same out-of-range transfer the Loot button uses), so nobody has to be
// near the corpse. Ties are impossible: each voter draws a d1000 as they vote.
//
// A roll has no deadline of its own -- it lives exactly as long as the corpse does. If the corpse
// rots before it resolves, the item rots with it and the row disappears from every window. That
// decay clock is what the window's per-row timer counts down.
//
// LOCKED IS THE ROLLING STATE. Grouped kills open every drop LOCKED: eligible members roll
// Need/Greed/Pass and it auto-resolves, while direct Loot/Sell and the native corpse are blocked for
// non-controllers. The loot controller (Master Looter, else leader) can UNLOCK a drop to turn it into a
// free-for-all -- anyone eligible then Loots/Sells it directly (no rolling). Solo kills open unlocked.
//
// A drop is identified by (corpse_id, adv_uid) -- LootItem::adv_uid, a stable per-corpse id, because
// equip_slot is -1 for almost every NPC drop. Sessions are per-zone and in memory only.
class AdvLootRollManager {
public:
	enum Vote : uint8 {
		VoteNone  = 0,
		VoteNeed  = 1,
		VoteGreed = 2,
		VotePass  = 3
	};

	AdvLootRollManager() { m_refresh.Start(); }

	// Put a freshly dropped corpse item up for a roll. Called once per item as the loot list is
	// pushed out at death; `eligible` is the set of character ids that will actually SEE the item.
	// `locked` true = rolling mode (grouped kills); false = free-for-all (solo kills Loot/Sell directly).
	void Open(
		uint16                     corpse_id,
		uint32                     adv_uid,
		uint32                     item_id,
		const std::string         &item_name,
		const std::vector<uint32> &eligible,
		bool                       locked = false
	);

	// Record an eligible looter's answer. Ignored from non-participants or a second vote (locked = the
	// rolling state, so a locked row DOES accept votes; unlocked rows simply never receive any).
	void Cast(Client *voter, uint16 corpse_id, uint32 adv_uid, Vote vote);

	// Any eligible looter can flag THIS drop for "single sell": if they win it, it is auto-sold for its
	// merchant price instead of going to their bags. Per-drop and per-player (not the persistent
	// always-sell). Toggled by the player before the roll resolves.
	void MarkSellOnWin(Client *c, uint16 corpse_id, uint32 adv_uid, bool on);

	// Loot-controller lock toggle: suspend (lock) / resume (unlock) the roll on a drop so the
	// controller can hand it out directly. `locked` true = lock. Controller-enforced by the caller.
	void SetLocked(uint16 corpse_id, uint32 adv_uid, bool locked);

	// Drop a session without resolving it -- the item left the corpse another way (the loot
	// controller took it or handed it out directly).
	void Cancel(uint16 corpse_id, uint32 adv_uid);

	// A player just changed their saved preference for item_id -- apply it to any LIVE roll they are in
	// on that item: Never/Greed/Need auto-cast the matching vote right now (mode == AdvLootPref value).
	void ApplyPrefToLive(Client *c, uint32 item_id, uint8 mode);

	// Driven by Zone::Process(): clears out sessions whose corpse has rotted and refreshes everyone's
	// per-row timers / tallies.
	void Process();

	// Whole seconds left before this corpse rots (0 if it is gone or has no decay timer running).
	static uint32 CorpseSecondsLeft(uint16 corpse_id);

	// Lock queries (drive who may Loot/Sell): a locked drop is reserved for the loot controller.
	bool IsLocked(uint16 corpse_id, uint32 adv_uid);
	bool CorpseHasLock(uint16 corpse_id); // any locked drop on this corpse (gates the NATIVE loot window)

private:
	struct Session {
		uint16                  corpse_id  = 0;
		uint32                  adv_uid    = 0;
		uint32                  item_id    = 0;
		bool                    locked     = false; // controller suspended the roll to hand it out
		std::string             item_name;
		std::vector<uint32>     eligible;     // character ids that may vote
		std::map<uint32, uint8> votes;        // character id -> Vote
		std::map<uint32, int>   rolls;        // character id -> d1000 (Need / Greed only)
		std::vector<uint32>     sell_on_win;  // character ids who chose to single-sell this drop if won
	};

	Session *Find(uint16 corpse_id, uint32 adv_uid);
	void     Resolve(Session &s);
	void     Erase(const Session *s);
	void     SendTally(const Session &s); // push tally + lock + corpse-seconds to the row (ALW5)
	void     SendRowRemoved(const Session &s);
	void     Announce(const Session &s, const std::string &message);

	std::list<Session> m_sessions; // list, not vector: Find() hands out pointers that must stay valid
	Timer              m_refresh{1000};
};

extern AdvLootRollManager adv_loot_rolls;
