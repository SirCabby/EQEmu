/*	EQEmu: EQEmulator

	Copyright (C) 2001-2026 EQEmu Development Team

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
#include "zone/client.h"

// Server-side spellbook volumes: switch the native 720-slot book to a different slice of the
// character's stored spells. Phase-1 manual trigger; a client-side page-turn auto-swap can
// drive this same call later.
void command_book(Client *c, const Seperator *sep)
{
	const int cur = c->GetSpellbookVolume();
	int       target;

	if (!strcasecmp(sep->arg[1], "next")) {
		target = cur + 1;
	} else if (!strcasecmp(sep->arg[1], "prev")) {
		target = cur - 1;
	} else if (sep->IsNumber(1)) {
		target = Strings::ToInt(sep->arg[1]);
	} else {
		c->Message(
			Chat::White,
			fmt::format(
				"Spellbook volume [{}] of [{}]. Usage: #book [0-{}|next|prev].",
				cur, Client::MAX_SPELLBOOK_VOLUMES, Client::MAX_SPELLBOOK_VOLUMES - 1
			).c_str()
		);
		return;
	}

	// next/prev past an end is a silent no-op so the client's page-turn auto-swap stays put
	// (no swap => no "volume loaded" confirmation => the client leaves the page where it was).
	if (target == cur) {
		return;
	}
	if (target < 0 || target >= Client::MAX_SPELLBOOK_VOLUMES) {
		if (sep->IsNumber(1)) {
			c->Message(Chat::Red, "Invalid spellbook volume %d (valid 0-%d).", target, Client::MAX_SPELLBOOK_VOLUMES - 1);
		}
		return;
	}

	c->SwapSpellbookVolume(target);
}
