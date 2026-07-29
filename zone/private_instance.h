/*	EQEmu: EQEmulator

	akk-stack custom feature: Private Zone Instancing.

	A thin layer over the native instance_list / instance_list_player primitives that lets a player
	spin up a private copy of a zone, route into it on entry ("next time you enter that zone"),
	share it with others, and tear it down when nobody is associated and it is empty of PCs.

	Private instances are identified by instance_list.notes = "akk_private:<owner_char_id>" and are
	created never_expires=1 -- teardown is managed here (association + emptiness), NOT by the native
	duration-expiry path. See the plan at ~/.claude/plans/curious-finding-sparrow.md.
*/
#ifndef EQEMU_ZONE_PRIVATE_INSTANCE_H
#define EQEMU_ZONE_PRIVATE_INSTANCE_H

#include <cstdint>

class Client;

namespace PrivateInstance {

	// action codes for OP_InstanceAction (client->server). MUST match the instancewnd .asi.
	enum Action : uint8_t {
		ACTION_REQUEST_STATE   = 0,
		ACTION_CREATE          = 1, // body: [u8 action][u32 zone_id][u8 version]
		ACTION_LEAVE           = 2,
		ACTION_INVITE          = 3, // body: [u8 action][char target[] NUL-terminated]
		ACTION_INVITE_RESPONSE = 4, // body: [u8 action][u8 accept]
	};

	// The instance_list.notes prefix that marks a row as one of our private instances.
	extern const char *const kNotesPrefix; // "akk_private:"

	// True if instance_id is a live private instance (notes marked).
	bool IsPrivateInstance(uint16_t instance_id);

	// Resolve the caller's live private instance for (zone_id, version), or 0 if none.
	// This is the routing hook Client::ZonePC uses to send an associated player into their
	// private instance when they enter the base zone (only when no explicit instance was requested).
	uint16_t Resolve(Client *c, uint32_t zone_id, int16_t version);

	// The character's current active private instance id (any zone), or 0 if none. Enforces the
	// one-private-instance-per-player rule at the read layer.
	uint16_t GetActive(uint32_t character_id);

	// Immediately destroy a private instance: bury its corpses into the non-instance zone
	// (instance_id -> 0) and remove the instance_list row. Caller ensures teardown conditions.
	void Destroy(uint16_t instance_id);

	// --- M1: create gates, recent-zone tracking, per-zone lockouts ---

	// True once the player has been out of combat long enough to be out-of-combat-rest eligible.
	// Does NOT require actually sitting: it is GetRestTimer()==0 (the rest period elapsed), which also
	// implies AggroCount==0. The create gate + (M3) the port-timing use this.
	bool IsRestedForInstancing(Client *c);

	// True if zone_id may be privately instanced (not a hub/special/inherently-instanced zone).
	bool IsInstanceable(uint32_t zone_id);

	// Per-zone 1-hour replay lockout. Returns true if the character is currently locked out of zone_id;
	// *remaining_seconds (when non-null) receives the time left.
	bool IsLockedOut(Client *c, uint32_t zone_id, uint32_t *remaining_seconds);

	// Called from Client::CompleteConnect: push this zone onto the rolling last-3 instanceable-zones
	// list (pi_recent bucket) and, if we entered a private instance, stamp its per-zone replay lockout.
	void OnZoneEntry(Client *c);

	// --- M2: teardown ---

	// Remove the caller's association to their own private instance (disband / create-new). Does NOT
	// eject the player and does NOT destroy the instance -- the actor stays until they zone out, and
	// teardown happens when the instance empties (ReconcileEmptyInProcess) or via the world sweep.
	// Returns the disassociated instance id (0 if the caller had none).
	uint16_t Disassociate(Client *c);

	// True if instance_id still has at least one instance_list_player association.
	bool HasAssociations(uint16_t instance_id);

	// Called from the instance's OWN zone process (global `zone`) when it has just gone empty of PCs
	// (numclients==0). If the instance is a private one with NO remaining associations, relocate its
	// PC corpses to the base/public zone (visible, in-process => clobber-safe) and destroy the record;
	// returns true if it tore down. An empty-but-still-associated instance is left intact (its zone
	// process idle-shuts, but the record persists so re-entry re-boots it).
	bool ReconcileEmptyInProcess();

	// --- M3: sharing / porting ---

	// Move the player into instance_id if they are standing in its base PUBLIC zone: immediately when
	// rested, else deferred until they become rested (TryDeferredPort). If they are not in the base
	// zone, does nothing -- they travel there and the routing resolver delivers them on entry.
	void DoPort(Client *c, uint16_t instance_id);

	// Per-tic hook (Client::Process): fires a queued deferred port once the player is rested and still
	// in the target instance's base public zone; cancels it if they left the zone or the instance died.
	void TryDeferredPort(Client *c);

	// The canonical (base/public) version of a zone. Used consistently at Create AND every Resolve
	// call site so a stored private instance is always found on re-entry regardless of which zone
	// the owner was standing in when they created it.
	int16_t BaseVersion(uint32_t zone_id);

	// The version a public context serves: instance 0 -> BaseVersion(zone_id); a registered global
	// static instance (classic clones / versioned revamps) -> that instance's own version. The
	// routing hooks pass the STATIC's version to Resolve so a private base-version instance never
	// hijacks a trip into a DIFFERENT public version (e.g. a hard-mode book target).
	int16_t PublicVersion(uint32_t zone_id, uint16_t instance_id);

	// The public-context instance id of (zone_id, version): the registered global static instance
	// when the zone is served through one (classic clones like nektulos_classic), else 0.
	uint16_t PublicInstanceID(uint32_t zone_id, int16_t version);

	// Create a private instance of zone_id (at its base version) owned by the character and
	// associate them. Returns the new instance id, or 0 on failure. Does NOT move the player.
	// NOTE (M0): gates (rested / leadership / lockout / instanceable) are added in M1.
	uint16_t Create(Client *c, uint32_t zone_id);

	// Dispatch a raw OP_InstanceAction body ([u8 action][args]) from client c.
	void HandleAction(Client *c, const uint8_t *body, uint32_t size);

} // namespace PrivateInstance

#endif // EQEMU_ZONE_PRIVATE_INSTANCE_H
