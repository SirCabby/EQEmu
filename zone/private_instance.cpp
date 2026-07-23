/*	EQEmu: EQEmulator

	akk-stack custom feature: Private Zone Instancing -- server engine.
	See private_instance.h and the plan at ~/.claude/plans/curious-finding-sparrow.md.

	M0: opcode plumbing + Create + the Resolve routing hook (ZonePC + Handle_OP_ZoneChange).
	M1: last-3-zones tracking, IsInstanceable blocklist, rested + leadership create gates, per-zone
	    replay lockouts, gated CREATE from the /instance window (+ port-if-in-base-zone), richer state.
*/
#include "private_instance.h"

#include "client.h"
#include "zone.h"
#include "zonedb.h"
#include "entity.h"
#include "corpse.h"
#include "groups.h"
#include "raids.h"
#include "questmgr.h"
#include "common/rulesys.h"
#include "common/servertalk.h"
#include "common/strings.h"
#include "common/zone_store.h"
#include "common/repositories/instance_list_repository.h"

#include <fmt/format.h>
#include <cstring>
#include <ctime>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace PrivateInstance {

const char *const kNotesPrefix = "akk_private:";

// M1: hardcoded tunables (M5 promotes these to rule_values via a dbmate migration).
static const int RECENT_MAX = 2; // last-N instanceable zones offered for Create (matches client REC_ROWS)

// LDoN adventure wings are <theme><a-h> (theme in {guk,mir,mmc,ruj,tak}); they are always-instanced, so
// never privately instanceable. The pattern excludes classic Guk (guktop/gukbottom -> not a single
// trailing a-h). Everything else off-limits is the admin-configurable RuleS(PrivateInstance, Blocklist).
static bool IsLDoNWing(const char *sn)
{
	if (!sn || strlen(sn) != 4 || sn[3] < 'a' || sn[3] > 'h') {
		return false;
	}
	static const char *const themes[] = {"guk", "mir", "mmc", "ruj", "tak"};
	for (auto t : themes) {
		if (strncmp(sn, t, 3) == 0) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- small helpers
// Parse a "a:b,c:d,..." bucket string into (a,b) pairs (used for pi_recent = zone:ver and
// pi_lockouts = zone:expiry_epoch). Skips malformed / zero-key tokens.
static std::vector<std::pair<uint32, uint32>> parse_pairs(const std::string &s)
{
	std::vector<std::pair<uint32, uint32>> v;
	size_t start = 0;
	while (start < s.size()) {
		size_t comma = s.find(',', start);
		std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		start = (comma == std::string::npos) ? s.size() : comma + 1;
		size_t colon = tok.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		uint32 a = Strings::ToUnsignedInt(tok.substr(0, colon));
		uint32 b = Strings::ToUnsignedInt(tok.substr(colon + 1));
		if (a) {
			v.emplace_back(a, b);
		}
	}
	return v;
}

// ---------------------------------------------------------------- M0 core
bool IsPrivateInstance(uint16_t instance_id)
{
	if (!instance_id) {
		return false;
	}

	auto i = InstanceListRepository::FindOne(database, instance_id);
	return i.id != 0 && Strings::BeginsWith(i.notes, kNotesPrefix);
}

int16_t BaseVersion(uint32_t zone_id)
{
	auto z = GetZoneVersionWithFallback(zone_id, 0);
	return z ? static_cast<int16_t>(z->version) : 0;
}

uint16_t Resolve(Client *c, uint32_t zone_id, int16_t version)
{
	if (!c || !zone_id) {
		return 0;
	}

	const auto query = fmt::format(
		"SELECT instance_list.id FROM instance_list, instance_list_player "
		"WHERE instance_list.zone = {} AND instance_list.version = {} "
		"AND instance_list.id = instance_list_player.id "
		"AND instance_list_player.charid = {} "
		"AND instance_list.notes LIKE '{}%' LIMIT 1",
		zone_id,
		version,
		c->CharacterID(),
		kNotesPrefix
	);

	auto results = database.QueryDatabase(query);
	if (!results.Success() || !results.RowCount()) {
		return 0;
	}

	auto row = results.begin();
	return static_cast<uint16_t>(Strings::ToUnsignedInt(row[0]));
}

uint16_t GetActive(uint32_t character_id)
{
	if (!character_id) {
		return 0;
	}

	const auto query = fmt::format(
		"SELECT instance_list.id FROM instance_list, instance_list_player "
		"WHERE instance_list.id = instance_list_player.id "
		"AND instance_list_player.charid = {} "
		"AND instance_list.notes LIKE '{}%' LIMIT 1",
		character_id,
		kNotesPrefix
	);

	auto results = database.QueryDatabase(query);
	if (!results.Success() || !results.RowCount()) {
		return 0;
	}

	auto row = results.begin();
	return static_cast<uint16_t>(Strings::ToUnsignedInt(row[0]));
}

void Destroy(uint16_t instance_id)
{
	if (!instance_id) {
		return;
	}

	// DeleteInstance buries corpses (instance_id -> 0, same/base zone) and cascades
	// membership/spawns/instance-scoped buckets, but intentionally leaves the instance_list row for
	// the native purge. We remove the row now so teardown is immediate.
	database.DeleteInstance(instance_id);
	database.QueryDatabase(fmt::format("DELETE FROM instance_list WHERE id = {}", instance_id));

	LogInfo("[PrivateInstance] Destroyed instance [{}]", instance_id);
}

uint16_t Create(Client *c, uint32_t zone_id)
{
	if (!c || !zone_id) {
		return 0;
	}

	const int16_t version = BaseVersion(zone_id);

	uint16 instance_id = 0;
	if (!database.GetUnusedInstanceID(instance_id) || !instance_id) {
		LogInfo("[PrivateInstance] Create failed: no unused instance id (char [{}])", c->CharacterID());
		return 0;
	}

	// duration 0 here; we immediately flip never_expires=1 below so the native purge never touches it.
	if (!database.CreateInstance(instance_id, zone_id, version, 0)) {
		LogInfo("[PrivateInstance] Create failed: CreateInstance (char [{}] zone [{}])", c->CharacterID(), zone_id);
		return 0;
	}

	// Mark it private + never-expiring (teardown is managed by PrivateInstance, not native expiry).
	database.QueryDatabase(fmt::format(
		"UPDATE instance_list SET never_expires = 1, notes = '{}{}' WHERE id = {}",
		kNotesPrefix,
		c->CharacterID(),
		instance_id
	));

	database.AddClientToInstance(instance_id, c->CharacterID());

	LogInfo(
		"[PrivateInstance] Created instance [{}] zone [{}] version [{}] for char [{}]",
		instance_id, zone_id, version, c->CharacterID()
	);

	return instance_id;
}

// ---------------------------------------------------------------- M1: gates + lockouts + recents
bool IsRestedForInstancing(Client *c)
{
	// IsOutOfCombatRested() (== GetRestTimer()==0) means the out-of-combat rest period has fully elapsed
	// (the "could get OOC fast regen if I sat" state) -- sitting NOT required. While aggro'd the timer
	// returns the pending combat value (nonzero), so true also implies AggroCount==0.
	return c && c->IsOutOfCombatRested();
}

bool IsInstanceable(uint32_t zone_id)
{
	if (!zone_id) {
		return false;
	}
	const char *sn = ZoneName(zone_id);
	if (!sn) {
		return false;
	}
	if (IsLDoNWing(sn)) {
		return false; // always-instanced adventure wings
	}
	for (const auto &b : Strings::Split(RuleS(PrivateInstance, Blocklist), ',')) {
		if (!b.empty() && !strcmp(sn, b.c_str())) {
			return false;
		}
	}
	return true;
}

// Stamp/refresh the per-zone lockout to (now + ZONE_LOCKOUT_SECONDS), pruning expired entries. Static:
// enter-stamping is internal (OnZoneEntry); M2 adds a leave-stamp at Disassociate/zone-out.
static void StampLockout(Client *c, uint32 zone_id)
{
	if (!c || !zone_id) {
		return;
	}
	const uint32 now = static_cast<uint32>(std::time(nullptr));
	const uint32 exp = now + static_cast<uint32>(RuleI(PrivateInstance, ZoneLockoutSeconds));

	std::vector<std::string> out;
	bool replaced = false;
	for (auto &p : parse_pairs(c->GetBucket("pi_lockouts"))) {
		if (p.second <= now) {
			continue; // prune expired
		}
		if (p.first == zone_id) {
			out.push_back(fmt::format("{}:{}", zone_id, exp));
			replaced = true;
		} else {
			out.push_back(fmt::format("{}:{}", p.first, p.second));
		}
	}
	if (!replaced) {
		out.push_back(fmt::format("{}:{}", zone_id, exp));
	}

	c->SetBucket("pi_lockouts", Strings::Implode(",", out));
}

bool IsLockedOut(Client *c, uint32_t zone_id, uint32_t *remaining_seconds)
{
	if (!c || !zone_id) {
		return false;
	}
	const uint32 now = static_cast<uint32>(std::time(nullptr));
	for (auto &p : parse_pairs(c->GetBucket("pi_lockouts"))) {
		if (p.first == zone_id && p.second > now) {
			if (remaining_seconds) {
				*remaining_seconds = p.second - now;
			}
			return true;
		}
	}
	return false;
}

// True if zone_id is in the caller's rolling last-3 list (what Create is allowed to target).
static bool InRecent(Client *c, uint32 zone_id)
{
	for (auto &p : parse_pairs(c->GetBucket("pi_recent"))) {
		if (p.first == zone_id) {
			return true;
		}
	}
	return false;
}

void OnZoneEntry(Client *c)
{
	if (!c || !zone) {
		return;
	}
	const uint32 zid = zone->GetZoneID();
	const uint16 iid = zone->GetInstanceID();

	// 1) Record this zone into the rolling last-3 instanceable-zones list (most-recent first, deduped).
	if (IsInstanceable(zid)) {
		std::vector<std::string> keep;
		keep.push_back(fmt::format("{}:{}", zid, BaseVersion(zid)));
		for (auto &p : parse_pairs(c->GetBucket("pi_recent"))) {
			if (p.first != zid && static_cast<int>(keep.size()) < RECENT_MAX) {
				keep.push_back(fmt::format("{}:{}", p.first, p.second));
			}
		}
		c->SetBucket("pi_recent", Strings::Implode(",", keep));
	}

	// 2) Entering a private instance stamps that zone's replay lockout (M2 adds the leave-stamp).
	if (iid && IsPrivateInstance(iid)) {
		StampLockout(c, zid);
	}
}

// ---------------------------------------------------------------- M2: teardown
uint16_t Disassociate(Client *c)
{
	if (!c) {
		return 0;
	}
	const uint16 id = GetActive(c->CharacterID());
	if (!id) {
		return 0;
	}
	database.RemoveClientFromInstance(id, c->CharacterID());
	LogInfo("[PrivateInstance] char [{}] disassociated from instance [{}]", c->CharacterID(), id);
	return id;
}

bool HasAssociations(uint16_t instance_id)
{
	if (!instance_id) {
		return false;
	}
	auto r = database.QueryDatabase(
		fmt::format("SELECT 1 FROM instance_list_player WHERE id = {} LIMIT 1", instance_id)
	);
	return r.Success() && r.RowCount() > 0;
}

bool ReconcileEmptyInProcess()
{
	if (!zone) {
		return false;
	}
	const uint16 id = zone->GetInstanceID();
	if (!id || !IsPrivateInstance(id)) {
		return false; // not one of ours
	}
	if (HasAssociations(id)) {
		return false; // still owned (e.g. an offline associate) -> keep the record; the zone just idle-shuts
	}

	// No associations and the caller guarantees the zone is empty of PCs. Relocate PC corpses to the
	// base/public zone (visible, same coords -- NOT buried) from THIS process so the corpse destructors
	// won't clobber the instance_id back (see the corpse re-save caveat), THEN destroy the record.
	std::list<Corpse *> corpses;
	entity_list.GetCorpseList(corpses);
	int moved = 0;
	for (auto *corpse : corpses) {
		if (corpse && corpse->IsPlayerCorpse() && corpse->MovePlayerCorpseToNonInstance()) {
			++moved;
		}
	}

	Destroy(id);
	LogInfo(
		"[PrivateInstance] empty+unassociated instance [{}] torn down in-process; moved [{}] corpse(s) to public zone",
		id, moved
	);
	return true;
}

// ---------------------------------------------------------------- M3: sharing + porting
// Deliver an "INSi|<inviter>~<zoneLongName>~<instanceId>" invite carrier to a target (or a whole
// group/raid) wherever they are, via the cross-zone marquee API. The target's instancewnd .asi sniffs
// "INSi" in its DisplayText hook and shows an Accept/Decline prompt. (quest_manager.CrossZoneMarquee is
// context-free: it just builds a ServerOP_CZMarquee packet and broadcasts; each zone delivers locally.)
static void SendInviteCarrier(uint8 update_type, int identifier, const char *client_name, const std::string &message)
{
	quest_manager.CrossZoneMarquee(
		update_type, identifier, /*type*/ 0, /*priority*/ 0, /*fade_in*/ 0, /*fade_out*/ 0,
		/*duration*/ 6000, message.c_str(), client_name ? client_name : ""
	);
}

static std::string InviteMessage(Client *inviter, uint16 instance_id, uint32 zone_id)
{
	return fmt::format("INSi|{}~{}~{}", inviter->GetCleanName(), ZoneLongName(zone_id), instance_id);
}

// Auto-invite every group/raid member except the creator (by name, so out-of-zone members are covered).
static void AutoInviteGroupRaid(Client *creator, uint16 instance_id, uint32 zone_id)
{
	if (!creator) {
		return;
	}
	const std::string msg  = InviteMessage(creator, instance_id, zone_id);
	const char       *self = creator->GetCleanName();

	if (Raid *r = creator->GetRaid()) {
		for (const auto &m : r->members) {
			if (m.member_name[0] && strcmp(m.member_name, self) != 0) {
				SendInviteCarrier(CZUpdateType_ClientName, 0, m.member_name, msg);
			}
		}
	} else if (Group *g = creator->GetGroup()) {
		for (const auto &nm : g->membername) {
			if (nm[0] && strcmp(nm, self) != 0) {
				SendInviteCarrier(CZUpdateType_ClientName, 0, nm, msg);
			}
		}
	}
}

void DoPort(Client *c, uint16_t instance_id)
{
	if (!c || !instance_id || !zone) {
		return;
	}
	auto il = InstanceListRepository::FindOne(database, instance_id);
	if (!il.id) {
		return;
	}
	// Only port a player standing in the base PUBLIC zone of this instance.
	if (zone->GetZoneID() != il.zone || zone->GetInstanceID() != 0) {
		return; // not at the doorstep -> they run there and the routing resolver delivers them
	}
	if (IsRestedForInstancing(c)) {
		c->MovePC(il.zone, instance_id, c->GetX(), c->GetY(), c->GetZ(), c->GetHeading());
	} else {
		c->SetPendingPrivatePort(instance_id);
		c->Message(Chat::White, "[Instance] You'll be moved in as soon as you're rested (out of combat).");
	}
}

void TryDeferredPort(Client *c)
{
	if (!c || !zone) {
		return;
	}
	const uint16 id = c->GetPendingPrivatePort();
	if (!id) {
		return;
	}
	auto il = InstanceListRepository::FindOne(database, id);
	// cancel if the instance died, or we are no longer standing in its base public zone
	if (!il.id || zone->GetZoneID() != il.zone || zone->GetInstanceID() != 0) {
		c->SetPendingPrivatePort(0);
		return;
	}
	if (IsRestedForInstancing(c)) {
		c->SetPendingPrivatePort(0);
		c->MovePC(il.zone, id, c->GetX(), c->GetY(), c->GetZ(), c->GetHeading());
	}
}

// ---------------------------------------------------------------- window protocol (OP_Marquee carrier)
static void Toast(Client *c, const std::string &msg)
{
	if (c) {
		c->SendMarqueeMessage(0, "INSe|" + msg, 4000);
	}
}

// Push the character's instance state to the /instance window over the "INSs" carrier. Layout:
//   INSs|<curInstId>~<curZoneId>~<curZoneLongName>
//       |<z>~<name>~<ver>~<lockRemSecs>;...   (recent, most-recent first)
//       |<z>~<name>~<lockRemSecs>;...         (all active lockouts)
// Field sep '~', record sep ';', section sep '|' -- none occur in zone long names.
static void SendState(Client *c)
{
	if (!c) {
		return;
	}
	const uint32 now = static_cast<uint32>(std::time(nullptr));
	auto locks = parse_pairs(c->GetBucket("pi_lockouts"));
	auto lock_remaining = [&](uint32 z) -> uint32 {
		for (auto &p : locks) {
			if (p.first == z && p.second > now) {
				return p.second - now;
			}
		}
		return 0;
	};

	uint16 cur_id = GetActive(c->CharacterID());
	uint32 cur_zone = 0;
	if (cur_id) {
		auto i = InstanceListRepository::FindOne(database, cur_id);
		cur_zone = i.zone;
	}

	std::string payload = fmt::format(
		"INSs|{}~{}~{}|", cur_id, cur_zone, cur_zone ? ZoneLongName(cur_zone) : ""
	);

	bool first = true;
	for (auto &p : parse_pairs(c->GetBucket("pi_recent"))) {
		if (!first) {
			payload += ";";
		}
		first = false;
		payload += fmt::format(
			"{}~{}~{}~{}", p.first, ZoneLongName(p.first), p.second, lock_remaining(p.first)
		);
	}
	payload += "|";

	first = true;
	for (auto &p : locks) {
		if (p.second <= now) {
			continue;
		}
		if (!first) {
			payload += ";";
		}
		first = false;
		payload += fmt::format("{}~{}~{}", p.first, ZoneLongName(p.first), p.second - now);
	}

	c->SendMarqueeMessage(0, payload, 4000);
}

// Apply the create gates (rested / leadership / instanceable / recent / lockout), create, and
// port-if-in-base-zone. Reachable only from the /instance window (the GM command calls Create directly).
static void CreateFromWindow(Client *c, uint32 zone_id)
{
	if (!RuleB(PrivateInstance, InstancingEnabled)) {
		Toast(c, "Private instancing is disabled on this server.");
		return;
	}
	if (!IsRestedForInstancing(c)) {
		Toast(c, "You must be out of combat and rested to create an instance.");
		return;
	}

	if (Raid *r = c->GetRaid()) {
		if (!r->IsLeader(c)) {
			Toast(c, "Only the raid leader can create an instance.");
			return;
		}
	} else if (Group *g = c->GetGroup()) {
		if (!g->IsLeader(c)) {
			Toast(c, "Only the group leader can create an instance.");
			return;
		}
	}

	if (!IsInstanceable(zone_id)) {
		Toast(c, "That zone cannot be privately instanced.");
		return;
	}
	if (!InRecent(c, zone_id)) {
		Toast(c, "You can only instance one of your recent zones.");
		return;
	}

	uint32 rem = 0;
	if (IsLockedOut(c, zone_id, &rem)) {
		Toast(c, fmt::format("You are locked out of that zone for {}m.", (rem + 59) / 60));
		return;
	}

	// One private instance per player: drop our association to any existing one first. We do NOT
	// destroy it here -- it self-tears-down when it empties (ReconcileEmptyInProcess) or via the world
	// sweep, which keeps it alive for other members once sharing lands in M3.
	if (GetActive(c->CharacterID())) {
		Disassociate(c);
	}

	uint16 id = Create(c, zone_id);
	if (!id) {
		Toast(c, "Failed to create instance.");
		return;
	}

	// Auto-invite the whole group/raid (each member gets an INSi prompt; accept swaps their instance).
	AutoInviteGroupRaid(c, id, zone_id);

	SendState(c); // update the window before any zone change

	// Port-if-in-base-zone (immediate, since create requires rested); else passive -- the routing
	// resolver delivers them on their next entry.
	DoPort(c, id);
}

void HandleAction(Client *c, const uint8_t *body, uint32_t size)
{
	if (!c || !body || size < 1) {
		return;
	}

	const uint8_t action = body[0];

	switch (action) {
		case ACTION_REQUEST_STATE: {
			SendState(c);
			break;
		}
		case ACTION_CREATE: {
			// body: [u8 action][u32 zone_id LE] (version is derived server-side from the zone).
			if (size < 5) {
				Toast(c, "Malformed create request");
				break;
			}
			const uint32_t zone_id = static_cast<uint32_t>(
				body[1] | (body[2] << 8) | (body[3] << 16) | (body[4] << 24)
			);
			CreateFromWindow(c, zone_id);
			break;
		}
		case ACTION_LEAVE: {
			const uint16_t id = Disassociate(c);
			if (id) {
				c->Message(
					Chat::White,
					"[Instance] You left your private instance; it shuts down once it is empty of players."
				);
			} else {
				Toast(c, "You have no private instance to leave.");
			}
			SendState(c);
			break;
		}
		case ACTION_INVITE: {
			// Invite your current target (bodyless). You must have a private instance; the target must
			// be another player (in your zone, since targets are in-zone) -- deliver the INSi carrier to
			// them directly. The invite carries the instance's base zone for display.
			const uint16 id = GetActive(c->CharacterID());
			if (!id) {
				Toast(c, "You have no private instance to invite anyone to.");
				break;
			}
			Mob *t = c->GetTarget();
			if (!t || !t->IsClient()) {
				Toast(c, "Target the player you want to invite.");
				break;
			}
			Client *tc = t->CastToClient();
			if (tc == c) {
				Toast(c, "You can't invite yourself.");
				break;
			}
			auto il = InstanceListRepository::FindOne(database, id);
			tc->SendMarqueeMessage(0, InviteMessage(c, id, il.zone), 6000);
			c->Message(Chat::White, "[Instance] Invited %s to your instance.", tc->GetCleanName());
			break;
		}
		case ACTION_INVITE_RESPONSE: {
			// body: [u8 action][u8 accept][u32 instance_id LE]
			if (size < 6) {
				break;
			}
			const uint8  accept = body[1];
			const uint16 shared = (uint16) (body[2] | (body[3] << 8) | (body[4] << 16) | (body[5] << 24));
			if (!accept) {
				break; // declined -- nothing to do
			}
			if (!shared || !IsPrivateInstance(shared)) {
				Toast(c, "That invite is no longer valid.");
				break;
			}
			if (database.CheckInstanceByCharID(shared, c->CharacterID())) {
				SendState(c); // already in it (self-invite / double-accept) -> no-op
				break;
			}
			auto         il          = InstanceListRepository::FindOne(database, shared);
			const uint32 shared_zone = il.zone;
			uint32       rem         = 0;
			if (IsLockedOut(c, shared_zone, &rem)) {
				Toast(c, fmt::format("You are locked out of that zone for {}m.", (rem + 59) / 60));
				break;
			}
			// swap: drop our current private instance, join the shared one, then port per the rules.
			Disassociate(c);
			database.AddClientToInstance(shared, c->CharacterID());
			c->Message(Chat::White, "[Instance] You joined the shared instance.");
			DoPort(c, shared);
			SendState(c);
			break;
		}
		default: {
			LogInfo("[PrivateInstance] Unhandled action [{}] from [{}]", action, c->GetCleanName());
			break;
		}
	}
}

} // namespace PrivateInstance
