#include "zone/client.h"
#include "zone/zone.h"
#include "zone/private_instance.h"
#include "common/strings.h"
#include "common/repositories/instance_list_repository.h"

// akk-stack: Private Zone Instancing -- dev/test command (GM-only).
// M0 aid to verify the routing resolver end-to-end WITHOUT the client mod: create a private
// instance for yourself, then zone out and back (or #zone) into the base zone -- you should land
// in the instance. The real feature drives all of this from the /instance window via OP_InstanceAction.
// Usage: #privateinstance [create [zoneid] [version] | status | destroy]
void command_privateinstance(Client *c, const Seperator *sep)
{
	if (!strcasecmp(sep->arg[1], "create")) {
		const uint32 zone_id = sep->IsNumber(2) ? Strings::ToUnsignedInt(sep->arg[2]) : zone->GetZoneID();

		const uint16 existing = PrivateInstance::GetActive(c->CharacterID());
		if (existing) {
			c->Message(Chat::Yellow, "[PrivateInstance] Replacing your existing private instance %u.", existing);
			PrivateInstance::Destroy(existing);
		}

		const uint16 id = PrivateInstance::Create(c, zone_id);
		if (id) {
			c->Message(
				Chat::White,
				"[PrivateInstance] Created instance %u of zone %u. Zone out and back in (or #zone) to route into it.",
				id, zone_id
			);
		} else {
			c->Message(Chat::Red, "[PrivateInstance] Create failed (see zone log).");
		}
		return;
	}

	if (!strcasecmp(sep->arg[1], "status")) {
		const uint16 id = PrivateInstance::GetActive(c->CharacterID());
		if (!id) {
			c->Message(Chat::White, "[PrivateInstance] You have no private instance.");
			return;
		}

		auto i = InstanceListRepository::FindOne(database, id);
		c->Message(
			Chat::White,
			"[PrivateInstance] Active instance %u -> zone %u (v%u), notes '%s'. You are in zone %u instance %u.",
			id, i.zone, i.version, i.notes.c_str(), zone->GetZoneID(), zone->GetInstanceID()
		);
		return;
	}

	if (!strcasecmp(sep->arg[1], "destroy")) {
		const uint16 id = PrivateInstance::GetActive(c->CharacterID());
		if (!id) {
			c->Message(Chat::White, "[PrivateInstance] You have no private instance to destroy.");
			return;
		}

		PrivateInstance::Destroy(id);
		c->Message(Chat::White, "[PrivateInstance] Destroyed instance %u.", id);
		return;
	}

	if (!strcasecmp(sep->arg[1], "leave")) {
		// Disband: drop your association. You stay in the instance until you zone out; it tears down
		// (corpses -> public zone) once it is empty of players. Same path the window's Leave button uses.
		const uint16 id = PrivateInstance::Disassociate(c);
		if (id) {
			c->Message(Chat::White, "[PrivateInstance] Left instance %u; it tears down once empty of players.", id);
		} else {
			c->Message(Chat::White, "[PrivateInstance] You have no private instance to leave.");
		}
		return;
	}

	if (!strcasecmp(sep->arg[1], "clearlocks")) {
		// Clears the caller's own per-zone replay lockouts (pi_lockouts bucket) -- DeleteBucket removes
		// both the DB row and the in-memory cache, so it takes effect immediately (no relog).
		c->DeleteBucket("pi_lockouts");
		c->Message(Chat::White, "[PrivateInstance] Cleared your instance lockouts.");
		return;
	}

	c->Message(Chat::White, "Usage: #privateinstance [create [zoneid] [version] | status | leave | destroy | clearlocks]");
}
