/**
 * @file missiles.cpp
 *
 * Implementation of missile functionality.
 */
#include "missiles.h"

#include <climits>

#include "control.h"
#include "cursor.h"
#include "dead.h"
#ifdef _DEBUG
#include "debug.h"
#endif
#include "engine/cel_header.hpp"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "init.h"
#include "inv.h"
#include "lighting.h"
#include "monster.h"
#include "spells.h"
#include "trigs.h"

namespace devilution {

int ActiveMissiles[MAXMISSILES];
int AvailableMissiles[MAXMISSILES];
MissileStruct Missiles[MAXMISSILES];
int ActiveMissileCount;
bool MissilePreFlag;

namespace {

ChainStruct chain[MAXMISSILES];
int numchains;

const int CrawlNum[19] = { 0, 3, 12, 45, 94, 159, 240, 337, 450, 579, 724, 885, 1062, 1255, 1464, 1689, 1930, 2187, 2460 };

int AddClassHealingBonus(int hp, HeroClass heroClass)
{
	switch (heroClass) {
	case HeroClass::Warrior:
	case HeroClass::Monk:
	case HeroClass::Barbarian:
		return hp * 2;
	case HeroClass::Rogue:
	case HeroClass::Bard:
		return hp + hp / 2;
	default:
		return hp;
	}
}

int ScaleSpellEffect(int base, int spellLevel)
{
	for (int i = 0; i < spellLevel; i++) {
		base += base / 8;
	}

	return base;
}

int GenerateRndSum(int range, int iterations)
{
	int value = 0;
	for (int i = 0; i < iterations; i++) {
		value += GenerateRnd(range);
	}

	return value;
}

bool CheckBlock(Point from, Point to)
{
	while (from != to) {
		from += GetDirection(from, to);
		if (nSolidTable[dPiece[from.x][from.y]])
			return true;
	}

	return false;
}

inline bool InDungeonBounds(Point position)
{
	return position.x > 0 && position.x < MAXDUNX && position.y > 0 && position.y < MAXDUNY;
}

MonsterStruct *FindClosest(Point source, int rad)
{
	if (rad > 19)
		rad = 19;

	for (int i = 1; i < rad; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = source.x + CrawlTable[ck - 1];
			int ty = source.y + CrawlTable[ck];
			if (!InDungeonBounds({ tx, ty }))
				continue;

			int mid = dMonster[tx][ty];
			if (mid > 0 && !CheckBlock(source, { tx, ty }))
				return &Monsters[mid - 1];
		}
	}
	return nullptr;
}

constexpr Direction16 Direction16Flip(Direction16 x, Direction16 pivot)
{
	unsigned ret = (2 * pivot + 16 - x) % 16;

	return static_cast<Direction16>(ret);
}

void UpdateMissileVelocity(MissileStruct &missile, Point destination, int v)
{
	missile.position.velocity = { 0, 0 };

	if (missile.position.tile == destination)
		return;

	double dxp = (destination.x + missile.position.tile.y - missile.position.tile.x - destination.y) * (1 << 21);
	double dyp = (destination.y + destination.x - missile.position.tile.x - missile.position.tile.y) * (1 << 21);
	double dr = sqrt(dxp * dxp + dyp * dyp);
	missile.position.velocity.deltaX = static_cast<int>((dxp * (v << 16)) / dr);
	missile.position.velocity.deltaY = static_cast<int>((dyp * (v << 15)) / dr);
}

/**
 * @brief Add the missile to the lookup tables
 * @param i Missiles index
 */
void PutMissile(MissileStruct &missile)
{
	Point position = missile.position.tile;

	if (!InDungeonBounds(position))
		missile._miDelFlag = true;

	if (missile._miDelFlag) {
		return;
	}

	dFlags[position.x][position.y] |= BFLAG_MISSILE;

	if (missile._miPreFlag)
		MissilePreFlag = true;
}

void UpdateMissilePos(MissileStruct &missile)
{
	int mx = missile.position.traveled.deltaX >> 16;
	int my = missile.position.traveled.deltaY >> 16;
	int dx = mx + 2 * my;
	int dy = 2 * my - mx;
	int lx = dx / 8;
	dx = dx / 64;
	int ly = dy / 8;
	dy = dy / 64;
	missile.position.tile = missile.position.start + Displacement { dx, dy };
	missile.position.offset.deltaX = mx + (dy * 32) - (dx * 32);
	missile.position.offset.deltaY = my - (dx * 16) - (dy * 16);
	ChangeLightOffset(missile._mlid, { lx - (dx * 8), ly - (dy * 8) });
}

void MoveMissilePos(MissileStruct &missile)
{
	int dx;
	int dy;

	switch (missile._mimfnum) {
	case DIR_NW:
	case DIR_N:
	case DIR_NE:
		dx = 0;
		dy = 0;
		break;
	case DIR_E:
		dx = 1;
		dy = 0;
		break;
	case DIR_W:
		dx = 0;
		dy = 1;
		break;
	case DIR_S:
	case DIR_SW:
	case DIR_SE:
		dx = 1;
		dy = 1;
		break;
	}
	int x = missile.position.tile.x + dx;
	int y = missile.position.tile.y + dy;
	if (IsTileAvailable(Monsters[missile._misource], { x, y })) {
		missile.position.tile.x += dx;
		missile.position.tile.y += dy;
		missile.position.offset.deltaX += (dy * 32) - (dx * 32);
		missile.position.offset.deltaY -= (dy * 16) + (dx * 16);
	}
}

bool MonsterMHit(int pnum, int m, int mindam, int maxdam, int dist, missile_id t, bool shift)
{
	auto &monster = Monsters[m];

	bool resist = false;
	if (monster.mtalkmsg != TEXT_NONE
	    || monster._mhitpoints >> 6 <= 0
	    || (t == MIS_HBOLT && monster.MType->mtype != MT_DIABLO && monster.MData->mMonstClass != MC_UNDEAD)) {
		return false;
	}
	if (monster.MType->mtype == MT_ILLWEAV && monster._mgoal == MGOAL_RETREAT)
		return false;
	if (monster._mmode == MM_CHARGE)
		return false;

	uint8_t mor = monster.mMagicRes;
	missile_resistance mir = MissileData[t].mResist;

	if (((mor & IMMUNE_MAGIC) != 0 && mir == MISR_MAGIC)
	    || ((mor & IMMUNE_FIRE) != 0 && mir == MISR_FIRE)
	    || ((mor & IMMUNE_LIGHTNING) != 0 && mir == MISR_LIGHTNING)
	    || ((mor & IMMUNE_ACID) != 0 && mir == MISR_ACID))
		return false;

	if (((mor & RESIST_MAGIC) != 0 && mir == MISR_MAGIC)
	    || ((mor & RESIST_FIRE) != 0 && mir == MISR_FIRE)
	    || ((mor & RESIST_LIGHTNING) != 0 && mir == MISR_LIGHTNING))
		resist = true;

	if (gbIsHellfire && t == MIS_HBOLT && (monster.MType->mtype == MT_DIABLO || monster.MType->mtype == MT_BONEDEMN))
		resist = true;

	int hit = GenerateRnd(100);
	int hper = 0;
	if (pnum != -1) {
		const auto &player = Players[pnum];
		if (MissileData[t].mType == 0) {
			hper = player.GetRangedToHit();
			hper -= monster.mArmorClass;
			hper -= (dist * dist) / 2;
			hper += player._pIEnAc;
		} else {
			hper = player.GetMagicToHit() - (monster.mLevel * 2) - dist;
		}
	} else {
		hper = GenerateRnd(75) - monster.mLevel * 2;
	}

	hper = clamp(hper, 5, 95);

	if (monster._mmode == MM_STONE)
		hit = 0;

	bool ret = false;
	if (CheckMonsterHit(monster, &ret))
		return ret;

	if (hit >= hper) {
#ifdef _DEBUG
		if (!DebugGodMode)
#endif
			return false;
	}

	int dam;
	if (t == MIS_BONESPIRIT) {
		dam = monster._mhitpoints / 3 >> 6;
	} else {
		dam = mindam + GenerateRnd(maxdam - mindam + 1);
	}

	const auto &player = Players[pnum];

	if (MissileData[t].mType == 0) {
		dam = player._pIBonusDamMod + dam * player._pIBonusDam / 100 + dam;
		if (player._pClass == HeroClass::Rogue)
			dam += player._pDamageMod;
		else
			dam += player._pDamageMod / 2;
	}

	if (!shift)
		dam <<= 6;
	if (resist)
		dam >>= 2;

	if (pnum == MyPlayerId)
		monster._mhitpoints -= dam;

	if ((gbIsHellfire && (player._pIFlags & ISPL_NOHEALMON) != 0) || (!gbIsHellfire && (player._pIFlags & ISPL_FIRE_ARROWS) != 0))
		monster._mFlags |= MFLAG_NOHEAL;

	if (monster._mhitpoints >> 6 <= 0) {
		if (monster._mmode == MM_STONE) {
			M_StartKill(m, pnum);
			monster.Petrify();
		} else {
			M_StartKill(m, pnum);
		}
	} else {
		if (resist) {
			PlayEffect(monster, 1);
		} else if (monster._mmode == MM_STONE) {
			if (m > MAX_PLRS - 1)
				M_StartHit(m, pnum, dam);
			monster.Petrify();
		} else {
			if (MissileData[t].mType == 0 && (player._pIFlags & ISPL_KNOCKBACK) != 0) {
				M_GetKnockback(m);
			}
			if (m > MAX_PLRS - 1)
				M_StartHit(m, pnum, dam);
		}
	}

	if (monster._msquelch == 0) {
		monster._msquelch = UINT8_MAX;
		monster.position.last = player.position.tile;
	}

	return true;
}

bool Plr2PlrMHit(int pnum, int p, int mindam, int maxdam, int dist, missile_id mtype, bool shift, bool *blocked)
{
	if (sgGameInitInfo.bFriendlyFire == 0 && gbFriendlyMode)
		return false;

	*blocked = false;

	auto &player = Players[pnum];
	auto &target = Players[p];

	if (target._pInvincible) {
		return false;
	}

	if (mtype == MIS_HBOLT) {
		return false;
	}

	if ((target._pSpellFlags & 1) != 0 && MissileData[mtype].mType == 0) {
		return false;
	}

	int8_t resper;
	switch (MissileData[mtype].mResist) {
	case MISR_FIRE:
		resper = target._pFireResist;
		break;
	case MISR_LIGHTNING:
		resper = target._pLghtResist;
		break;
	case MISR_MAGIC:
	case MISR_ACID:
		resper = target._pMagResist;
		break;
	default:
		resper = 0;
		break;
	}

	int hper = GenerateRnd(100);

	int hit;
	if (MissileData[mtype].mType == 0) {
		hit = player.GetRangedToHit()
		    - (dist * dist / 2)
		    - target.GetArmor();
	} else {
		hit = player.GetMagicToHit()
		    - (target._pLevel * 2)
		    - dist;
	}

	hit = clamp(hit, 5, 95);

	if (hper >= hit) {
		return false;
	}

	int blkper = 100;
	if (!shift && (target._pmode == PM_STAND || target._pmode == PM_ATTACK) && target._pBlockFlag) {
		blkper = GenerateRnd(100);
	}

	int blk = target.GetBlockChance() - (player._pLevel * 2);
	blk = clamp(blk, 0, 100);

	int dam;
	if (mtype == MIS_BONESPIRIT) {
		dam = target._pHitPoints / 3;
	} else {
		dam = mindam + GenerateRnd(maxdam - mindam + 1);
		if (MissileData[mtype].mType == 0)
			dam += player._pIBonusDamMod + player._pDamageMod + dam * player._pIBonusDam / 100;
		if (!shift)
			dam <<= 6;
	}
	if (MissileData[mtype].mType != 0)
		dam /= 2;
	if (resper > 0) {
		dam -= (dam * resper) / 100;
		if (pnum == MyPlayerId)
			NetSendCmdDamage(true, p, dam);
		player.Say(HeroSpeech::ArghClang);
		return true;
	}

	if (blkper < blk) {
		StartPlrBlock(p, GetDirection(target.position.tile, player.position.tile));
		*blocked = true;
	} else {
		if (pnum == MyPlayerId)
			NetSendCmdDamage(true, p, dam);
		StartPlrHit(p, dam, false);
	}

	return true;
}

void CheckMissileCol(MissileStruct &missile, int mindam, int maxdam, bool shift, Point position, bool nodel)
{
	bool blocked;

	int mx = position.x;
	int my = position.y;

	if (mx >= MAXDUNX || mx < 0)
		return;
	if (my >= MAXDUNY || my < 0)
		return;
	if (missile._micaster != TARGET_BOTH && missile._misource != -1) {
		if (missile._micaster == TARGET_MONSTERS) {
			int mid = dMonster[mx][my];
			if (mid > 0) {
				mid -= 1;
				if (MonsterMHit(
				        missile._misource,
				        mid,
				        mindam,
				        maxdam,
				        missile._midist,
				        missile._mitype,
				        shift)) {
					if (!nodel)
						missile._mirange = 0;
					missile._miHitFlag = true;
				}
			} else if (mid < 0) {
				mid = -(mid + 1);
				if (Monsters[mid]._mmode == MM_STONE
				    && MonsterMHit(
				        missile._misource,
				        mid,
				        mindam,
				        maxdam,
				        missile._midist,
				        missile._mitype,
				        shift)) {
					if (!nodel)
						missile._mirange = 0;
					missile._miHitFlag = true;
				}
			}
			if (dPlayer[mx][my] > 0
			    && dPlayer[mx][my] - 1 != missile._misource
			    && Plr2PlrMHit(
			        missile._misource,
			        dPlayer[mx][my] - 1,
			        mindam,
			        maxdam,
			        missile._midist,
			        missile._mitype,
			        shift,
			        &blocked)) {
				if (gbIsHellfire && blocked) {
					int dir = missile._mimfnum + (GenerateRnd(2) != 0 ? 1 : -1);
					int mAnimFAmt = MissileSpriteData[missile._miAnimType].animFAmt;
					if (dir < 0)
						dir = mAnimFAmt - 1;
					else if (dir > mAnimFAmt)
						dir = 0;

					SetMissDir(missile, dir);
				} else if (!nodel) {
					missile._mirange = 0;
				}
				missile._miHitFlag = true;
			}
		} else {
			auto &monster = Monsters[missile._misource];
			if ((monster._mFlags & MFLAG_TARGETS_MONSTER) != 0
			    && dMonster[mx][my] > 0
			    && (Monsters[dMonster[mx][my] - 1]._mFlags & MFLAG_GOLEM) != 0
			    && MonsterTrapHit(dMonster[mx][my] - 1, mindam, maxdam, missile._midist, missile._mitype, shift)) {
				if (!nodel)
					missile._mirange = 0;
				missile._miHitFlag = true;
			}
			if (dPlayer[mx][my] > 0
			    && PlayerMHit(
			        dPlayer[mx][my] - 1,
			        &monster,
			        missile._midist,
			        mindam,
			        maxdam,
			        missile._mitype,
			        shift,
			        0,
			        &blocked)) {
				if (gbIsHellfire && blocked) {
					int dir = missile._mimfnum + (GenerateRnd(2) != 0 ? 1 : -1);
					int mAnimFAmt = MissileSpriteData[missile._miAnimType].animFAmt;
					if (dir < 0)
						dir = mAnimFAmt - 1;
					else if (dir > mAnimFAmt)
						dir = 0;

					SetMissDir(missile, dir);
				} else if (!nodel) {
					missile._mirange = 0;
				}
				missile._miHitFlag = true;
			}
		}
	} else {
		int mid = dMonster[mx][my];
		if (mid > 0) {
			mid -= 1;
			if (missile._micaster == TARGET_BOTH) {
				if (MonsterMHit(
				        missile._misource,
				        mid,
				        mindam,
				        maxdam,
				        missile._midist,
				        missile._mitype,
				        shift)) {
					if (!nodel)
						missile._mirange = 0;
					missile._miHitFlag = true;
				}
			} else if (MonsterTrapHit(mid, mindam, maxdam, missile._midist, missile._mitype, shift)) {
				if (!nodel)
					missile._mirange = 0;
				missile._miHitFlag = true;
			}
		}
		if (dPlayer[mx][my] > 0) {
			if (PlayerMHit(
			        dPlayer[mx][my] - 1,
			        nullptr,
			        missile._midist,
			        mindam,
			        maxdam,
			        missile._mitype,
			        shift,
			        (missile._miAnimType == MFILE_FIREWAL || missile._miAnimType == MFILE_LGHNING) ? 1 : 0,
			        &blocked)) {
				if (gbIsHellfire && blocked) {
					int dir = missile._mimfnum + (GenerateRnd(2) != 0 ? 1 : -1);
					int mAnimFAmt = MissileSpriteData[missile._miAnimType].animFAmt;
					if (dir < 0)
						dir = mAnimFAmt - 1;
					else if (dir > mAnimFAmt)
						dir = 0;

					SetMissDir(missile, dir);
				} else if (!nodel) {
					missile._mirange = 0;
				}
				missile._miHitFlag = true;
			}
		}
	}
	if (dObject[mx][my] != 0) {
		int oi = dObject[mx][my] > 0 ? dObject[mx][my] - 1 : -(dObject[mx][my] + 1);
		if (!Objects[oi]._oMissFlag) {
			if (Objects[oi]._oBreak == 1)
				BreakObject(-1, oi);
			if (!nodel)
				missile._mirange = 0;
			missile._miHitFlag = false;
		}
	}
	if (nMissileTable[dPiece[mx][my]]) {
		if (!nodel)
			missile._mirange = 0;
		missile._miHitFlag = false;
	}
	if (missile._mirange == 0 && MissileData[missile._mitype].miSFX != -1)
		PlaySfxLoc(MissileData[missile._mitype].miSFX, missile.position.tile);
}

void SetMissAnim(MissileStruct &missile, int animtype)
{
	int dir = missile._mimfnum;

	if (animtype > MFILE_NONE) {
		animtype = MFILE_NONE;
	}

	missile._miAnimType = animtype;
	missile._miAnimFlags = MissileSpriteData[animtype].flags;
	missile._miAnimData = MissileSpriteData[animtype].animData[dir].get();
	missile._miAnimDelay = MissileSpriteData[animtype].animDelay[dir];
	missile._miAnimLen = MissileSpriteData[animtype].animLen[dir];
	missile._miAnimWidth = MissileSpriteData[animtype].animWidth;
	missile._miAnimWidth2 = MissileSpriteData[animtype].animWidth2;
	missile._miAnimCnt = 0;
	missile._miAnimFrame = 1;
}

bool MissilesFoundTarget(MissileStruct &missile, Point *position, int rad)
{
	rad = std::min(rad, 19);
	for (int i = 0; i < rad; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = position->x + CrawlTable[ck - 1];
			int ty = position->y + CrawlTable[ck];
			if (!InDungeonBounds({ tx, ty }))
				continue;

			int dp = dPiece[tx][ty];
			if (nSolidTable[dp] || dObject[tx][ty] != 0 || (dFlags[i][j] & BFLAG_MISSILE) != 0)
				continue;

			missile.position.tile = { tx, ty };
			*position = { tx, ty };
			return true;
		}
	}
	return false;
}

bool CheckIfTrig(Point position)
{
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i].position.WalkingDistance(position) < 2)
			return true;
	}
	return false;
}

bool GuardianTryFireAt(MissileStruct &missile, Point target)
{
	Point position = missile.position.tile;

	if (!LineClearMissile(position, target))
		return false;
	int mi = dMonster[target.x][target.y] - 1;
	if (mi < MAX_PLRS)
		return false;
	if (Monsters[mi]._mhitpoints >> 6 <= 0)
		return false;

	Direction dir = GetDirection(position, target);
	missile._miVar3 = AvailableMissiles[0];
	AddMissile(position, target, dir, MIS_FIREBOLT, TARGET_MONSTERS, missile._misource, missile._midam, GetSpellLevel(missile._misource, SPL_FIREBOLT));
	SetMissDir(missile, 2);
	missile._miVar2 = 3;

	return true;
}

void FireballUpdate(int i, Displacement offset, bool alwaysDelete)
{
	auto &missile = Missiles[i];
	missile._mirange--;

	int id = missile._misource;
	Point p = (missile._micaster == TARGET_MONSTERS) ? Players[id].position.tile : Monsters[id].position.tile;

	if (missile._miAnimType == MFILE_BIGEXP) {
		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			AddUnLight(missile._mlid);
		}
	} else {
		int dam = missile._midam;
		missile.position.traveled += offset;
		UpdateMissilePos(missile);
		if (missile.position.tile != missile.position.start)
			CheckMissileCol(missile, dam, dam, false, missile.position.tile, false);
		if (missile._mirange == 0) {
			Point m = missile.position.tile;
			ChangeLight(missile._mlid, missile.position.tile, missile._miAnimFrame);

			constexpr Displacement Pattern[] = { { 0, 0 }, { 0, 1 }, { 0, -1 }, { 1, 0 }, { 1, -1 }, { 1, 1 }, { -1, 0 }, { -1, 1 }, { -1, -1 } };
			for (auto shift : Pattern) {
				if (!CheckBlock(p, m + shift))
					CheckMissileCol(missile, dam, dam, false, m + shift, true);
			}

			if (!TransList[dTransVal[m.x][m.y]]
			    || (missile.position.velocity.deltaX < 0 && ((TransList[dTransVal[m.x][m.y + 1]] && nSolidTable[dPiece[m.x][m.y + 1]]) || (TransList[dTransVal[m.x][m.y - 1]] && nSolidTable[dPiece[m.x][m.y - 1]])))) {
				missile.position.tile.x++;
				missile.position.tile.y++;
				missile.position.offset.deltaY -= 32;
			}
			if (missile.position.velocity.deltaY > 0
			    && ((TransList[dTransVal[m.x + 1][m.y]] && nSolidTable[dPiece[m.x + 1][m.y]])
			        || (TransList[dTransVal[m.x - 1][m.y]] && nSolidTable[dPiece[m.x - 1][m.y]]))) {
				missile.position.offset.deltaY -= 32;
			}
			if (missile.position.velocity.deltaX > 0
			    && ((TransList[dTransVal[m.x][m.y + 1]] && nSolidTable[dPiece[m.x][m.y + 1]])
			        || (TransList[dTransVal[m.x][m.y - 1]] && nSolidTable[dPiece[m.x][m.y - 1]]))) {
				missile.position.offset.deltaX -= 32;
			}
			missile._mimfnum = 0;
			SetMissAnim(missile, MFILE_BIGEXP);
			missile._mirange = missile._miAnimLen - 1;
			missile.position.velocity = {};
		} else if (missile.position.tile.x != missile._miVar1 || missile.position.tile.y != missile._miVar2) {
			missile._miVar1 = missile.position.tile.x;
			missile._miVar2 = missile.position.tile.y;
			ChangeLight(missile._mlid, missile.position.tile, 8);
		}
		if (alwaysDelete)
			missile._miDelFlag = true;
	}

	PutMissile(missile);
}

bool GrowWall(int playerId, Point position, Point target, missile_id type, int spellLevel, int damage)
{
	int dp = dPiece[position.x][position.y];
	assert(dp <= MAXTILES && dp >= 0);

	if (nMissileTable[dp] || !InDungeonBounds(target)) {
		return false;
	}

	AddMissile(position, position, Players[playerId]._pdir, type, TARGET_BOTH, playerId, damage, spellLevel);
	return true;
}

bool CanAddEffect(const PlayerStruct &player, missile_id type)
{
	if (currlevel != player.plrlevel)
		return false;

	for (int i = 0; i < ActiveMissileCount; i++) {
		int mi = ActiveMissiles[i];
		auto &missile = Missiles[mi];
		if (missile._mitype == type && &Players[missile._misource] == &player)
			return false;
	}

	return true;
}

} // namespace

void GetDamageAmt(int i, int *mind, int *maxd)
{
	assert(MyPlayerId >= 0 && MyPlayerId < MAX_PLRS);
	assert(i >= 0 && i < 64);

	auto &myPlayer = Players[MyPlayerId];

	int sl = myPlayer._pSplLvl[i] + myPlayer._pISplLvlAdd;

	switch (i) {
	case SPL_FIREBOLT:
		*mind = (myPlayer._pMagic / 8) + sl + 1;
		*maxd = *mind + 9;
		break;
	case SPL_HEAL:
	case SPL_HEALOTHER:
		/// BUGFIX: healing calculation is unused
		*mind = AddClassHealingBonus(myPlayer._pLevel + sl + 1, myPlayer._pClass) - 1;
		*maxd = AddClassHealingBonus((4 * myPlayer._pLevel) + (6 * sl) + 10, myPlayer._pClass) - 1;
		break;
	case SPL_LIGHTNING:
	case SPL_RUNELIGHT:
		*mind = 2;
		*maxd = 2 + myPlayer._pLevel;
		break;
	case SPL_FLASH:
		*mind = ScaleSpellEffect(myPlayer._pLevel, sl);
		*mind += *mind / 2;
		*maxd = *mind * 2;
		break;
	case SPL_IDENTIFY:
	case SPL_TOWN:
	case SPL_STONE:
	case SPL_INFRA:
	case SPL_RNDTELEPORT:
	case SPL_MANASHIELD:
	case SPL_DOOMSERP:
	case SPL_BLODRIT:
	case SPL_INVISIBIL:
	case SPL_BLODBOIL:
	case SPL_TELEPORT:
	case SPL_ETHEREALIZE:
	case SPL_REPAIR:
	case SPL_RECHARGE:
	case SPL_DISARM:
	case SPL_RESURRECT:
	case SPL_TELEKINESIS:
	case SPL_BONESPIRIT:
	case SPL_WARP:
	case SPL_REFLECT:
	case SPL_BERSERK:
	case SPL_SEARCH:
	case SPL_RUNESTONE:
		*mind = -1;
		*maxd = -1;
		break;
	case SPL_FIREWALL:
	case SPL_LIGHTWALL:
	case SPL_FIRERING:
		*mind = 2 * myPlayer._pLevel + 4;
		*maxd = *mind + 36;
		break;
	case SPL_FIREBALL:
	case SPL_RUNEFIRE: {
		int base = (2 * myPlayer._pLevel) + 4;
		*mind = ScaleSpellEffect(base, sl);
		*maxd = ScaleSpellEffect(base + 36, sl);
	} break;
	case SPL_GUARDIAN: {
		int base = (myPlayer._pLevel / 2) + 1;
		*mind = ScaleSpellEffect(base, sl);
		*maxd = ScaleSpellEffect(base + 9, sl);
	} break;
	case SPL_CHAIN:
		*mind = 4;
		*maxd = 4 + (2 * myPlayer._pLevel);
		break;
	case SPL_WAVE:
		*mind = 6 * (myPlayer._pLevel + 1);
		*maxd = *mind + 54;
		break;
	case SPL_NOVA:
	case SPL_IMMOLAT:
	case SPL_RUNEIMMOLAT:
	case SPL_RUNENOVA:
		*mind = ScaleSpellEffect((myPlayer._pLevel + 5) / 2, sl) * 5;
		*maxd = ScaleSpellEffect((myPlayer._pLevel + 30) / 2, sl) * 5;
		break;
	case SPL_FLAME:
		*mind = 3;
		*maxd = myPlayer._pLevel + 4;
		*maxd += *maxd / 2;
		break;
	case SPL_GOLEM:
		*mind = 11;
		*maxd = 17;
		break;
	case SPL_APOCA:
		*mind = myPlayer._pLevel;
		*maxd = *mind * 6;
		break;
	case SPL_ELEMENT:
		*mind = ScaleSpellEffect(2 * myPlayer._pLevel + 4, sl);
		/// BUGFIX: add here '*mind /= 2;'
		*maxd = ScaleSpellEffect(2 * myPlayer._pLevel + 40, sl);
		/// BUGFIX: add here '*maxd /= 2;'
		break;
	case SPL_CBOLT:
		*mind = 1;
		*maxd = *mind + (myPlayer._pMagic / 4);
		break;
	case SPL_HBOLT:
		*mind = myPlayer._pLevel + 9;
		*maxd = *mind + 9;
		break;
	case SPL_FLARE:
		*mind = (myPlayer._pMagic / 2) + 3 * sl - (myPlayer._pMagic / 8);
		*maxd = *mind;
		break;
	}
}

int GetSpellLevel(int playerId, spell_id sn)
{
	auto &player = Players[playerId];

	if (playerId != MyPlayerId)
		return 1; // BUGFIX spell level will be wrong in multiplayer

	return std::max(player._pISplLvlAdd + player._pSplLvl[sn], 0);
}

/**
 * @brief Returns the direction a vector from p1(x1, y1) to p2(x2, y2) is pointing to.
 *
 *      W  sW  SW   Sw  S
 *              ^
 *     nW       |       Se
 *              |
 *     NW ------+-----> SE
 *              |
 *     Nw       |       sE
 *              |
 *      N  Ne  NE   nE  E
 *
 * @param x1 the x coordinate of p1
 * @param y1 the y coordinate of p1
 * @param x2 the x coordinate of p2
 * @param y2 the y coordinate of p2
 * @return the direction of the p1->p2 vector
 */
Direction16 GetDirection16(Point p1, Point p2)
{
	Displacement offset = p2 - p1;
	Displacement absolute = abs(offset);

	bool flipY = offset.deltaX != absolute.deltaX;
	bool flipX = offset.deltaY != absolute.deltaY;

	bool flipMedian = false;
	if (absolute.deltaX > absolute.deltaY) {
		std::swap(absolute.deltaX, absolute.deltaY);
		flipMedian = true;
	}

	Direction16 ret = DIR16_S;
	if (3 * absolute.deltaX <= (absolute.deltaY * 2)) { // mx/my <= 2/3, approximation of tan(33.75)
		if (5 * absolute.deltaX < absolute.deltaY)      // mx/my < 0.2, approximation of tan(11.25)
			ret = DIR16_SW;
		else
			ret = DIR16_Sw;
	}

	Direction16 medianPivot = DIR16_S;
	if (flipY) {
		ret = Direction16Flip(ret, DIR16_SW);
		medianPivot = Direction16Flip(medianPivot, DIR16_SW);
	}
	if (flipX) {
		ret = Direction16Flip(ret, DIR16_SE);
		medianPivot = Direction16Flip(medianPivot, DIR16_SE);
	}
	if (flipMedian)
		ret = Direction16Flip(ret, medianPivot);
	return ret;
}

void DeleteMissile(int mi, int i)
{
	auto &missile = Missiles[mi];
	if (missile._mitype == MIS_MANASHIELD) {
		int src = missile._misource;
		if (src == MyPlayerId)
			NetSendCmd(true, CMD_REMSHIELD);
		Players[src].pManaShield = false;
	}

	AvailableMissiles[MAXMISSILES - ActiveMissileCount] = mi;
	ActiveMissileCount--;
	if (ActiveMissileCount > 0 && i != ActiveMissileCount)
		ActiveMissiles[i] = ActiveMissiles[ActiveMissileCount];
}

bool MonsterTrapHit(int m, int mindam, int maxdam, int dist, missile_id t, bool shift)
{
	auto &monster = Monsters[m];

	bool resist = false;
	if (monster.mtalkmsg != TEXT_NONE) {
		return false;
	}
	if (monster._mhitpoints >> 6 <= 0) {
		return false;
	}
	if (monster.MType->mtype == MT_ILLWEAV && monster._mgoal == MGOAL_RETREAT)
		return false;
	if (monster._mmode == MM_CHARGE)
		return false;

	missile_resistance mir = MissileData[t].mResist;
	int mor = monster.mMagicRes;
	if (((mor & IMMUNE_MAGIC) != 0 && mir == MISR_MAGIC)
	    || ((mor & IMMUNE_FIRE) != 0 && mir == MISR_FIRE)
	    || ((mor & IMMUNE_LIGHTNING) != 0 && mir == MISR_LIGHTNING)) {
		return false;
	}

	if (((mor & RESIST_MAGIC) != 0 && mir == MISR_MAGIC)
	    || ((mor & RESIST_FIRE) != 0 && mir == MISR_FIRE)
	    || ((mor & RESIST_LIGHTNING) != 0 && mir == MISR_LIGHTNING)) {
		resist = true;
	}

	int hit = GenerateRnd(100);
	int hper = 90 - (BYTE)monster.mArmorClass - dist;
	hper = clamp(hper, 5, 95);
	bool ret;
	if (CheckMonsterHit(monster, &ret)) {
		return ret;
	}
	if (hit >= hper && monster._mmode != MM_STONE) {
#ifdef _DEBUG
		if (!DebugGodMode)
#endif
			return false;
	}

	int dam = mindam + GenerateRnd(maxdam - mindam + 1);
	if (!shift)
		dam <<= 6;
	if (resist)
		monster._mhitpoints -= dam / 4;
	else
		monster._mhitpoints -= dam;
#ifdef _DEBUG
	if (DebugGodMode)
		monster._mhitpoints = 0;
#endif
	if (monster._mhitpoints >> 6 <= 0) {
		if (monster._mmode == MM_STONE) {
			M_StartKill(m, -1);
			monster.Petrify();
		} else {
			M_StartKill(m, -1);
		}
	} else {
		if (resist) {
			PlayEffect(monster, 1);
		} else if (monster._mmode == MM_STONE) {
			if (m > MAX_PLRS - 1)
				M_StartHit(m, -1, dam);
			monster.Petrify();
		} else {
			if (m > MAX_PLRS - 1)
				M_StartHit(m, -1, dam);
		}
	}
	return true;
}

bool PlayerMHit(int pnum, MonsterStruct *monster, int dist, int mind, int maxd, missile_id mtype, bool shift, int earflag, bool *blocked)
{
	*blocked = false;

	auto &player = Players[pnum];

	if (player._pHitPoints >> 6 <= 0) {
		return false;
	}

	if (player._pInvincible) {
		return false;
	}

	if ((player._pSpellFlags & 1) != 0 && MissileData[mtype].mType == 0) {
		return false;
	}

	int hit = GenerateRnd(100);
#ifdef _DEBUG
	if (DebugGodMode)
		hit = 1000;
#endif
	int hper = 40;
	if (MissileData[mtype].mType == 0) {
		int tac = player.GetArmor();
		if (monster != nullptr) {
			hper = monster->mHit
			    + ((monster->mLevel - player._pLevel) * 2)
			    + 30
			    - (dist * 2) - tac;
		} else {
			hper = 100 - (tac / 2) - (dist * 2);
		}
	} else if (monster != nullptr) {
		hper += (monster->mLevel * 2) - (player._pLevel * 2) - (dist * 2);
	}

	int minhit = 10;
	if (currlevel == 14)
		minhit = 20;
	if (currlevel == 15)
		minhit = 25;
	if (currlevel == 16)
		minhit = 30;
	hper = std::max(hper, minhit);

	int blk = 100;
	if ((player._pmode == PM_STAND || player._pmode == PM_ATTACK) && player._pBlockFlag) {
		blk = GenerateRnd(100);
	}

	if (shift)
		blk = 100;
	if (mtype == MIS_ACIDPUD)
		blk = 100;

	int blkper = player.GetBlockChance(false);
	if (monster != nullptr)
		blkper -= (monster->mLevel - player._pLevel) * 2;
	blkper = clamp(blkper, 0, 100);

	int8_t resper;
	switch (MissileData[mtype].mResist) {
	case MISR_FIRE:
		resper = player._pFireResist;
		break;
	case MISR_LIGHTNING:
		resper = player._pLghtResist;
		break;
	case MISR_MAGIC:
	case MISR_ACID:
		resper = player._pMagResist;
		break;
	default:
		resper = 0;
		break;
	}

	if (hit >= hper) {
		return false;
	}

	int dam;
	if (mtype == MIS_BONESPIRIT) {
		dam = player._pHitPoints / 3;
	} else {
		if (!shift) {
			dam = (mind << 6) + GenerateRnd((maxd - mind + 1) << 6);
			if (monster == nullptr)
				if ((player._pIFlags & ISPL_ABSHALFTRAP) != 0)
					dam /= 2;
			dam += player._pIGetHit * 64;
		} else {
			dam = mind + GenerateRnd(maxd - mind + 1);
			if (monster == nullptr)
				if ((player._pIFlags & ISPL_ABSHALFTRAP) != 0)
					dam /= 2;
			dam += player._pIGetHit;
		}

		dam = std::max(dam, 64);
	}

	if ((resper <= 0 || gbIsHellfire) && blk < blkper) {
		Direction dir = player._pdir;
		if (monster != nullptr) {
			dir = GetDirection(player.position.tile, monster->position.tile);
		}
		*blocked = true;
		StartPlrBlock(pnum, dir);
		return true;
	}

	if (resper > 0) {
		dam -= dam * resper / 100;
		if (pnum == MyPlayerId) {
			ApplyPlrDamage(pnum, 0, 0, dam, earflag);
		}

		if (player._pHitPoints >> 6 > 0) {
			player.Say(HeroSpeech::ArghClang);
		}
		return true;
	}

	if (pnum == MyPlayerId) {
		ApplyPlrDamage(pnum, 0, 0, dam, earflag);
	}

	if (player._pHitPoints >> 6 > 0) {
		StartPlrHit(pnum, dam, false);
	}

	return true;
}

void SetMissDir(MissileStruct &missile, int dir)
{
	missile._mimfnum = dir;
	SetMissAnim(missile, missile._miAnimType);
}

void InitMissileGFX()
{
	for (int mi = 0; MissileSpriteData[mi].animFAmt != 0; mi++) {
		if (!gbIsHellfire && mi > MFILE_SCBSEXPD)
			break;
		if (MissileSpriteData[mi].flags == MissileDataFlags::MonsterOwned)
			continue;
		MissileSpriteData[mi].LoadGFX();
	}
}

void FreeMissiles()
{
	for (int mi = 0; MissileSpriteData[mi].animFAmt != 0; mi++) {
		if (MissileSpriteData[mi].flags == MissileDataFlags::MonsterOwned)
			continue;

		MissileSpriteData[mi].FreeGFX();
	}
}

void FreeMissiles2()
{
	for (int mi = 0; MissileSpriteData[mi].animFAmt != 0; mi++) {
		if (MissileSpriteData[mi].flags != MissileDataFlags::MonsterOwned)
			continue;

		MissileSpriteData[mi].FreeGFX();
	}
}

void InitMissiles()
{
	auto &myPlayer = Players[MyPlayerId];

	AutoMapShowItems = false;
	myPlayer._pSpellFlags &= ~0x1;
	if (myPlayer._pInfraFlag) {
		for (int i = 0; i < ActiveMissileCount; ++i) {
			int mi = ActiveMissiles[i];
			auto &missile = Missiles[mi];
			if (missile._mitype == MIS_INFRA) {
				int src = missile._misource;
				if (src == MyPlayerId)
					CalcPlrItemVals(MyPlayerId, true);
			}
		}
	}

	if ((myPlayer._pSpellFlags & 2) == 2 || (myPlayer._pSpellFlags & 4) == 4) {
		myPlayer._pSpellFlags &= ~0x2;
		myPlayer._pSpellFlags &= ~0x4;
		for (int i = 0; i < ActiveMissileCount; ++i) {
			int mi = ActiveMissiles[i];
			auto &missile = Missiles[mi];
			if (missile._mitype == MIS_BLODBOIL) {
				if (missile._misource == MyPlayerId) {
					int missingHP = myPlayer._pMaxHP - myPlayer._pHitPoints;
					CalcPlrItemVals(MyPlayerId, true);
					ApplyPlrDamage(MyPlayerId, 0, 1, missingHP + missile._miVar2);
				}
			}
		}
	}

	ActiveMissileCount = 0;
	for (int i = 0; i < MAXMISSILES; i++) {
		AvailableMissiles[i] = i;
		ActiveMissiles[i] = 0;
	}
	numchains = 0;
	for (auto &link : chain) {
		link.idx = -1;
		link._mitype = MIS_ARROW;
		link._mirange = 0;
	}
	for (int j = 0; j < MAXDUNY; j++) {
		for (int i = 0; i < MAXDUNX; i++) { // NOLINT(modernize-loop-convert)
			dFlags[i][j] &= ~BFLAG_MISSILE;
		}
	}
}

void AddHiveExplosion(MissileStruct &missile, Point /*dst*/, int midir)
{
	for (int x : { 80, 81 }) {
		for (int y : { 62, 63 }) {
			AddMissile({ x, y }, { 80, 62 }, midir, MIS_HIVEEXP, missile._micaster, missile._misource, missile._midam, 0);
		}
	}
	missile._miDelFlag = true;
}

void AddRune(MissileStruct &missile, Point dst, spell_id spellID, missile_id missileID)
{
	if (LineClearMissile(missile.position.start, dst)) {
		if (missile._misource >= 0)
			UseMana(missile._misource, spellID);
		if (MissilesFoundTarget(missile, &dst, 10)) {
			missile._miVar1 = missileID;
			missile._mlid = AddLight(dst, 8);
		} else {
			missile._miDelFlag = true;
		}
	} else {
		missile._miDelFlag = true;
	}
}

void AddFireRune(MissileStruct &missile, Point dst, int /*midir*/)
{
	AddRune(missile, dst, SPL_RUNEFIRE, MIS_HIVEEXP);
}

void AddLightningRune(MissileStruct &missile, Point dst, int /*midir*/)
{
	AddRune(missile, dst, SPL_RUNELIGHT, MIS_LIGHTBALL);
}

void AddGreatLightningRune(MissileStruct &missile, Point dst, int /*midir*/)
{
	AddRune(missile, dst, SPL_RUNENOVA, MIS_NOVA);
}

void AddImmolationRune(MissileStruct &missile, Point dst, int /*midir*/)
{
	if (LineClearMissile(missile.position.start, dst)) {
		if (missile._misource >= 0)
			UseMana(missile._misource, SPL_RUNEIMMOLAT);
		if (MissilesFoundTarget(missile, &dst, 10)) {
			missile._miVar1 = MIS_IMMOLATION;
			missile._mlid = AddLight(dst, 8);
		} else {
			missile._miDelFlag = true;
		}
	} else {
		missile._miDelFlag = true;
	}
}

void AddStoneRune(MissileStruct &missile, Point dst, int /*midir*/)
{
	if (LineClearMissile(missile.position.start, dst)) {
		if (missile._misource >= 0)
			UseMana(missile._misource, SPL_RUNESTONE);
		if (MissilesFoundTarget(missile, &dst, 10)) {
			missile._miVar1 = MIS_STONE;
			missile._mlid = AddLight(dst, 8);
		} else {
			missile._miDelFlag = true;
		}
	} else {
		missile._miDelFlag = true;
	}
}

void AddReflection(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;

	if (missile._misource < 0)
		return;

	auto &player = Players[missile._misource];

	int add = (missile._mispllvl != 0 ? missile._mispllvl : 2) * player._pLevel;
	if (player.wReflections + add >= std::numeric_limits<uint16_t>::max())
		add = 0;
	player.wReflections += add;
	if (missile._misource == MyPlayerId)
		NetSendCmdParam1(true, CMD_SETREFLECT, player.wReflections);

	UseMana(missile._misource, SPL_REFLECT);
}

void AddBerserk(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miDelFlag = true;

	if (missile._misource < 0)
		return;

	for (int i = 0; i < 6; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = dst.x + CrawlTable[ck - 1];
			int ty = dst.y + CrawlTable[ck];
			if (!InDungeonBounds({ tx, ty }))
				continue;

			int dm = dMonster[tx][ty];
			dm = dm > 0 ? dm - 1 : -(dm + 1);
			if (dm < MAX_PLRS)
				continue;
			auto &monster = Monsters[dm];

			if (monster._uniqtype != 0 || monster._mAi == AI_DIABLO)
				continue;
			if (monster._mmode == MM_FADEIN || monster._mmode == MM_FADEOUT)
				continue;
			if ((monster.mMagicRes & IMMUNE_MAGIC) != 0)
				continue;
			if ((monster.mMagicRes & RESIST_MAGIC) != 0 && ((monster.mMagicRes & RESIST_MAGIC) != 1 || GenerateRnd(2) != 0))
				continue;
			if (monster._mmode == MM_CHARGE)
				continue;

			i = 6;
			auto slvl = GetSpellLevel(missile._misource, SPL_BERSERK);
			monster._mFlags |= MFLAG_BERSERK | MFLAG_GOLEM;
			monster.mMinDamage = (GenerateRnd(10) + 120) * monster.mMinDamage / 100 + slvl;
			monster.mMaxDamage = (GenerateRnd(10) + 120) * monster.mMaxDamage / 100 + slvl;
			monster.mMinDamage2 = (GenerateRnd(10) + 120) * monster.mMinDamage2 / 100 + slvl;
			monster.mMaxDamage2 = (GenerateRnd(10) + 120) * monster.mMaxDamage2 / 100 + slvl;
			int r = (currlevel < 17 || currlevel > 20) ? 3 : 9;
			monster.mlid = AddLight(monster.position.tile, r);
			UseMana(missile._misource, SPL_BERSERK);
			break;
		}
	}
}

void AddHorkSpawn(MissileStruct &missile, Point dst, int midir)
{
	UpdateMissileVelocity(missile, dst, 8);
	missile._mirange = 9;
	missile._miVar1 = midir;
	PutMissile(missile);
}

void AddJester(MissileStruct &missile, Point dst, int midir)
{
	missile_id spell = MIS_FIREBOLT;
	switch (GenerateRnd(10)) {
	case 0:
	case 1:
		spell = MIS_FIREBOLT;
		break;
	case 2:
		spell = MIS_FIREBALL;
		break;
	case 3:
		spell = MIS_FIREWALLC;
		break;
	case 4:
		spell = MIS_GUARDIAN;
		break;
	case 5:
		spell = MIS_CHAIN;
		break;
	case 6:
		spell = MIS_TOWN;
		UseMana(missile._misource, SPL_TOWN);
		break;
	case 7:
		spell = MIS_TELEPORT;
		break;
	case 8:
		spell = MIS_APOCA;
		break;
	case 9:
		spell = MIS_STONE;
		break;
	}
	AddMissile(missile.position.start, dst, midir, spell, missile._micaster, missile._misource, 0, missile._mispllvl);
	missile._miDelFlag = true;
}

void AddStealPotions(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	for (int i = 0; i < 3; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = missile.position.start.x + CrawlTable[ck - 1];
			int ty = missile.position.start.y + CrawlTable[ck];
			if (!InDungeonBounds({ tx, ty }))
				continue;
			int8_t pnum = dPlayer[tx][ty];
			if (pnum == 0)
				continue;
			auto &player = Players[abs(pnum) - 1];

			bool hasPlayedSFX = false;
			for (int si = 0; si < MAXBELTITEMS; si++) {
				int ii = -1;
				if (player.SpdList[si]._itype == ITYPE_MISC) {
					if (GenerateRnd(2) == 0)
						continue;
					switch (player.SpdList[si]._iMiscId) {
					case IMISC_FULLHEAL:
						ii = ItemMiscIdIdx(IMISC_HEAL);
						break;
					case IMISC_HEAL:
					case IMISC_MANA:
						player.RemoveSpdBarItem(si);
						break;
					case IMISC_FULLMANA:
						ii = ItemMiscIdIdx(IMISC_MANA);
						break;
					case IMISC_REJUV:
						if (GenerateRnd(2) != 0) {
							ii = ItemMiscIdIdx(IMISC_MANA);
						} else {
							ii = ItemMiscIdIdx(IMISC_HEAL);
						}
						break;
					case IMISC_FULLREJUV:
						switch (GenerateRnd(3)) {
						case 0:
							ii = ItemMiscIdIdx(IMISC_FULLMANA);
							break;
						case 1:
							ii = ItemMiscIdIdx(IMISC_FULLHEAL);
							break;
						default:
							ii = ItemMiscIdIdx(IMISC_REJUV);
							break;
						}
						break;
					default:
						continue;
					}
				}
				if (ii != -1) {
					SetPlrHandItem(&player.HoldItem, ii);
					GetPlrHandSeed(&player.HoldItem);
					player.HoldItem._iStatFlag = true;
					player.SpdList[si] = player.HoldItem;
				}
				if (!hasPlayedSFX) {
					PlaySfxLoc(IS_POPPOP2, { tx, ty });
					hasPlayedSFX = true;
				}
			}
			force_redraw = 255;
		}
	}
	missile._miDelFlag = true;
}

void AddManaTrap(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	for (int i = 0; i < 3; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = missile.position.start.x + CrawlTable[ck - 1];
			int ty = missile.position.start.y + CrawlTable[ck];
			if (0 < tx && tx < MAXDUNX && 0 < ty && ty < MAXDUNY) {
				int8_t pid = dPlayer[tx][ty];
				if (pid != 0) {
					auto &player = Players[abs(pid) - 1];

					player._pMana = 0;
					player._pManaBase = player._pMana + player._pMaxManaBase - player._pMaxMana;
					CalcPlrInv(pid, false);
					drawmanaflag = true;
					PlaySfxLoc(TSFX_COW7, { tx, ty });
				}
			}
		}
	}
	missile._miDelFlag = true;
}

void AddSpecArrow(MissileStruct &missile, Point dst, int /*midir*/)
{
	int av = 0;

	if (missile._micaster == TARGET_MONSTERS) {
		auto &player = Players[missile._misource];

		if (player._pClass == HeroClass::Rogue)
			av += (player._pLevel - 1) / 4;
		else if (player._pClass == HeroClass::Warrior || player._pClass == HeroClass::Bard)
			av += (player._pLevel - 1) / 8;

		if ((player._pIFlags & ISPL_QUICKATTACK) != 0)
			av++;
		if ((player._pIFlags & ISPL_FASTATTACK) != 0)
			av += 2;
		if ((player._pIFlags & ISPL_FASTERATTACK) != 0)
			av += 4;
		if ((player._pIFlags & ISPL_FASTESTATTACK) != 0)
			av += 8;
	}

	missile._mirange = 1;
	missile._miVar1 = dst.x;
	missile._miVar2 = dst.y;
	missile._miVar3 = av;
}

void AddWarp(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	int minDistanceSq = std::numeric_limits<int>::max();
	Point src = missile.position.start;
	Point tile = src;
	if (missile._misource >= 0) {
		tile = Players[missile._misource].position.tile;
	}

	for (int i = 0; i < numtrigs && i < MAXTRIGGERS; i++) {
		TriggerStruct *trg = &trigs[i];
		if (trg->_tmsg == WM_DIABTWARPUP || trg->_tmsg == WM_DIABPREVLVL || trg->_tmsg == WM_DIABNEXTLVL || trg->_tmsg == WM_DIABRTNLVL) {
			Point candidate = trg->position;
			if ((leveltype == DTYPE_CATHEDRAL || leveltype == DTYPE_CATACOMBS) && (trg->_tmsg == WM_DIABNEXTLVL || trg->_tmsg == WM_DIABPREVLVL || trg->_tmsg == WM_DIABRTNLVL)) {
				candidate += Displacement { 0, 1 };
			} else {
				candidate += Displacement { 1, 0 };
			}
			Displacement off = src - candidate;
			int distanceSq = off.deltaY * off.deltaY + off.deltaX * off.deltaX;
			if (distanceSq < minDistanceSq) {
				minDistanceSq = distanceSq;
				tile = candidate;
			}
		}
	}
	missile._mirange = 2;
	missile.position.tile = tile;
	if (missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_WARP);
}

void AddLightningWall(MissileStruct &missile, Point dst, int /*midir*/)
{
	UpdateMissileVelocity(missile, dst, 16);
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mirange = 255 * (missile._mispllvl + 1);
	if (missile._misource < 0) {
		missile._miVar1 = missile.position.start.x;
		missile._miVar2 = missile.position.start.y;
	} else {
		missile._miVar1 = Players[missile._misource].position.tile.x;
		missile._miVar2 = Players[missile._misource].position.tile.y;
	}
}

void AddRuneExplosion(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	if (IsAnyOf(missile._micaster, TARGET_MONSTERS, TARGET_BOTH)) {
		int dmg = 2 * (Players[missile._misource]._pLevel + GenerateRndSum(10, 2)) + 4;
		dmg = ScaleSpellEffect(dmg, missile._mispllvl);

		missile._midam = dmg;

		constexpr Displacement Offsets[] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 0, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
		for (Displacement offset : Offsets)
			CheckMissileCol(missile, dmg, dmg, false, missile.position.tile + offset, true);
	}
	missile._mlid = AddLight(missile.position.start, 8);
	SetMissDir(missile, 0);
	missile._mirange = missile._miAnimLen - 1;
}

void AddFireNova(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int sp = 16;
	if (missile._micaster == TARGET_MONSTERS) {
		sp += std::min(missile._mispllvl, 34);
	}
	UpdateMissileVelocity(missile, dst, sp);
	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miVar4 = missile.position.start.x;
	missile._miVar5 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
}

void AddLightningArrow(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	UpdateMissileVelocity(missile, dst, 32);
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mirange = 255;
	if (missile._misource < 0) {
		missile._miVar1 = missile.position.start.x;
		missile._miVar2 = missile.position.start.y;
	} else {
		missile._miVar1 = Players[missile._misource].position.tile.x;
		missile._miVar2 = Players[missile._misource].position.tile.y;
	}
	missile._midam <<= 6;
}

void AddMana(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	auto &player = Players[missile._misource];

	int manaAmount = (GenerateRnd(10) + 1) << 6;
	for (int i = 0; i < player._pLevel; i++) {
		manaAmount += (GenerateRnd(4) + 1) << 6;
	}
	for (int i = 0; i < missile._mispllvl; i++) {
		manaAmount += (GenerateRnd(6) + 1) << 6;
	}
	if (player._pClass == HeroClass::Sorcerer)
		manaAmount *= 2;
	if (player._pClass == HeroClass::Rogue || player._pClass == HeroClass::Bard)
		manaAmount += manaAmount / 2;
	player._pMana += manaAmount;
	if (player._pMana > player._pMaxMana)
		player._pMana = player._pMaxMana;
	player._pManaBase += manaAmount;
	if (player._pManaBase > player._pMaxManaBase)
		player._pManaBase = player._pMaxManaBase;
	UseMana(missile._misource, SPL_MANA);
	missile._miDelFlag = true;
	drawmanaflag = true;
}

void AddMagi(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	auto &player = Players[missile._misource];

	player._pMana = player._pMaxMana;
	player._pManaBase = player._pMaxManaBase;
	UseMana(missile._misource, SPL_MAGI);
	missile._miDelFlag = true;
	drawmanaflag = true;
}

void AddRing(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	if (missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_FIRERING);
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miDelFlag = false;
	missile._mirange = 7;
}

void AddSearch(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miVar1 = missile._misource;
	AutoMapShowItems = true;
	int lvl = 2;
	if (missile._misource > -1)
		lvl = Players[missile._misource]._pLevel * 2;
	missile._mirange = lvl + 10 * missile._mispllvl + 245;
	if (missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_SEARCH);

	for (int i = 0; i < ActiveMissileCount; i++) {
		int mx = ActiveMissiles[i];
		if (&Missiles[mx] != &missile) {
			MissileStruct *mis = &Missiles[mx];
			if (mis->_miVar1 == missile._misource && mis->_mitype == MIS_SEARCH) {
				int r1 = missile._mirange;
				int r2 = mis->_mirange;
				if (r2 < INT_MAX - r1)
					mis->_mirange = r1 + r2;
				missile._miDelFlag = true;
				break;
			}
		}
	}
}

void AddCboltArrow(MissileStruct &missile, Point dst, int midir)
{
	missile._mirnd = GenerateRnd(15) + 1;
	if (missile._micaster != TARGET_MONSTERS) {
		missile._midam = 15;
	}

	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mlid = AddLight(missile.position.start, 5);
	UpdateMissileVelocity(missile, dst, 8);
	missile._miVar1 = 5;
	missile._miVar2 = midir;
	missile._mirange = 256;
}

void AddLArrow(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int av = 32;
	if (missile._micaster == TARGET_MONSTERS) {
		auto &player = Players[missile._misource];
		if (player._pClass == HeroClass::Rogue)
			av += (player._pLevel) / 4;
		else if (player._pClass == HeroClass::Warrior || player._pClass == HeroClass::Bard)
			av += (player._pLevel) / 8;

		if (gbIsHellfire) {
			if ((player._pIFlags & ISPL_QUICKATTACK) != 0)
				av++;
			if ((player._pIFlags & ISPL_FASTATTACK) != 0)
				av += 2;
			if ((player._pIFlags & ISPL_FASTERATTACK) != 0)
				av += 4;
			if ((player._pIFlags & ISPL_FASTESTATTACK) != 0)
				av += 8;
		} else {
			if (player._pClass == HeroClass::Rogue || player._pClass == HeroClass::Warrior || player._pClass == HeroClass::Bard)
				av -= 1;
		}
	}
	UpdateMissileVelocity(missile, dst, av);

	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 5);
}

void AddArrow(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int av = 32;
	if (missile._micaster == TARGET_MONSTERS) {
		auto &player = Players[missile._misource];

		if ((player._pIFlags & ISPL_RNDARROWVEL) != 0) {
			av = GenerateRnd(32) + 16;
		}
		if (player._pClass == HeroClass::Rogue)
			av += (player._pLevel - 1) / 4;
		else if (player._pClass == HeroClass::Warrior || player._pClass == HeroClass::Bard)
			av += (player._pLevel - 1) / 8;

		if (gbIsHellfire) {
			if ((player._pIFlags & ISPL_QUICKATTACK) != 0)
				av++;
			if ((player._pIFlags & ISPL_FASTATTACK) != 0)
				av += 2;
			if ((player._pIFlags & ISPL_FASTERATTACK) != 0)
				av += 4;
			if ((player._pIFlags & ISPL_FASTESTATTACK) != 0)
				av += 8;
		}
	}
	UpdateMissileVelocity(missile, dst, av);
	missile._miAnimFrame = GetDirection16(missile.position.start, dst) + 1;
	missile._mirange = 256;
}

void UpdateVileMissPos(MissileStruct &missile, Point dst)
{
	for (int k = 1; k < 50; k++) {
		for (int j = -k; j <= k; j++) {
			int yy = j + dst.y;
			for (int i = -k; i <= k; i++) {
				int xx = i + dst.x;
				if (PosOkPlayer(Players[MyPlayerId], { xx, yy })) {
					missile.position.tile = { xx, yy };
					return;
				}
			}
		}
	}
	missile.position.tile = dst;
}

void AddRndTeleport(MissileStruct &missile, Point dst, int /*midir*/)
{
	int pn;
	int r1;
	int r2;

	int nTries = 0;
	do {
		nTries++;
		if (nTries > 500) {
			r1 = 0;
			r2 = 0;
			break; //BUGFIX: warps player to 0/0 in hellfire, change to return or use 1.09's version of the code
		}
		r1 = GenerateRnd(3) + 4;
		r2 = GenerateRnd(3) + 4;
		if (GenerateRnd(2) == 1)
			r1 = -r1;
		if (GenerateRnd(2) == 1)
			r2 = -r2;

		r1 += missile.position.start.x;
		r2 += missile.position.start.y;
		if (r1 < MAXDUNX && r1 >= 0 && r2 < MAXDUNY && r2 >= 0) { ///BUGFIX: < MAXDUNX / < MAXDUNY (fixed)
			pn = dPiece[r1][r2];
		}
	} while (nSolidTable[pn] || dObject[r1][r2] != 0 || dMonster[r1][r2] != 0);

	missile._mirange = 2;
	if (!setlevel || setlvlnum != SL_VILEBETRAYER) {
		missile.position.tile = { r1, r2 };
		if (missile._micaster == TARGET_MONSTERS)
			UseMana(missile._misource, SPL_RNDTELEPORT);
	} else {
		int oi = dObject[dst.x][dst.y] - 1;
		// BUGFIX: should only run magic circle check if dObject[dx][dy] is non-zero.
		if (Objects[oi]._otype == OBJ_MCIRCLE1 || Objects[oi]._otype == OBJ_MCIRCLE2) {
			missile.position.tile = dst;
			if (!PosOkPlayer(Players[MyPlayerId], dst))
				UpdateVileMissPos(missile, dst);
		}
	}
}

void AddFirebolt(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int sp = 26;
	if (missile._micaster == TARGET_MONSTERS) {
		sp = 16;
		if (missile._misource != -1) {
			sp += std::min(missile._mispllvl * 2, 47);
		}

		int i;
		for (i = 0; i < ActiveMissileCount; i++) {
			int mx = ActiveMissiles[i];
			auto &guardian = Missiles[mx];
			if (guardian._mitype == MIS_GUARDIAN && guardian._misource == missile._misource && guardian._miVar3 >= 0 && guardian._miVar3 < MAXMISSILES && &Missiles[guardian._miVar3] == &missile)
				break;
		}
		if (i == ActiveMissileCount)
			UseMana(missile._misource, SPL_FIREBOLT);
	}
	UpdateMissileVelocity(missile, dst, sp);
	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
}

void AddMagmaball(MissileStruct &missile, Point dst, int /*midir*/)
{
	UpdateMissileVelocity(missile, dst, 16);
	missile.position.traveled.deltaX += 3 * missile.position.velocity.deltaX;
	missile.position.traveled.deltaY += 3 * missile.position.velocity.deltaY;
	UpdateMissilePos(missile);
	if (!gbIsHellfire || (missile.position.velocity.deltaX & 0xFFFF0000) != 0 || (missile.position.velocity.deltaY & 0xFFFF0000) != 0)
		missile._mirange = 256;
	else
		missile._mirange = 1;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
}

void AddTeleport(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miDelFlag = true;
	for (int i = 0; i < 6; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = dst.x + CrawlTable[ck - 1];
			int ty = dst.y + CrawlTable[ck];
			if (0 < tx && tx < MAXDUNX && 0 < ty && ty < MAXDUNY) {
				if (IsTileNotSolid({ tx, ty }) && dMonster[tx][ty] == 0 && dObject[tx][ty] == 0 && dPlayer[tx][ty] == 0) {
					missile.position.tile = { tx, ty };
					missile.position.start = { tx, ty };
					missile._miDelFlag = false;
					i = 6;
					break;
				}
			}
		}
	}

	if (!missile._miDelFlag) {
		UseMana(missile._misource, SPL_TELEPORT);
		missile._mirange = 2;
	}
}

void AddLightball(MissileStruct &missile, Point dst, int /*midir*/)
{
	UpdateMissileVelocity(missile, dst, 16);
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mirange = 255;
	if (missile._misource < 0) {
		missile._miVar1 = missile.position.start.x;
		missile._miVar2 = missile.position.start.y;
	} else {
		missile._miVar1 = Players[missile._misource].position.tile.x;
		missile._miVar2 = Players[missile._misource].position.tile.y;
	}
}

void AddFirewall(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._midam = GenerateRndSum(10, 2) + 2;
	missile._midam += missile._misource >= 0 ? Players[missile._misource]._pLevel : currlevel; // BUGFIX: missing parenthesis around ternary (fixed)
	missile._midam <<= 3;
	UpdateMissileVelocity(missile, dst, 16);
	int i = missile._mispllvl;
	missile._mirange = 10;
	if (i > 0)
		missile._mirange *= i + 1;
	if (missile._micaster == TARGET_PLAYERS || missile._misource < 0)
		missile._mirange += currlevel;
	else
		missile._mirange += (Players[missile._misource]._pISplDur * missile._mirange) / 128;
	missile._mirange *= 16;
	missile._miVar1 = missile._mirange - missile._miAnimLen;
}

void AddFireball(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int sp = 16;
	if (missile._micaster == TARGET_MONSTERS) {
		sp += std::min(missile._mispllvl * 2, 34);

		int dmg = 2 * (Players[missile._misource]._pLevel + GenerateRndSum(10, 2)) + 4;
		missile._midam = ScaleSpellEffect(dmg, missile._mispllvl);

		UseMana(missile._misource, SPL_FIREBALL);
	}
	UpdateMissileVelocity(missile, dst, sp);
	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miVar4 = missile.position.start.x;
	missile._miVar5 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
}

void AddLightctrl(MissileStruct &missile, Point dst, int /*midir*/)
{
	if (missile._midam == 0 && missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_LIGHTNING);
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	UpdateMissileVelocity(missile, dst, 32);
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mirange = 256;
}

void AddLightning(MissileStruct &missile, Point dst, int midir)
{
	missile.position.start = dst;
	if (midir >= 0) {
		missile.position.offset = Missiles[midir].position.offset;
		missile.position.traveled = Missiles[midir].position.traveled;
	}
	missile._miAnimFrame = GenerateRnd(8) + 1;

	if (midir < 0 || missile._micaster == TARGET_PLAYERS || missile._misource == -1) {
		if (midir < 0 || missile._misource == -1)
			missile._mirange = 8;
		else
			missile._mirange = 10;
	} else {
		missile._mirange = (missile._mispllvl / 2) + 6;
	}
	missile._mlid = AddLight(missile.position.tile, 4);
}

void AddMisexp(MissileStruct &missile, Point dst, int /*midir*/)
{
	if (missile._micaster != TARGET_MONSTERS && missile._misource >= 0) {
		switch (Monsters[missile._misource].MType->mtype) {
		case MT_SUCCUBUS:
			SetMissAnim(missile, MFILE_FLAREEXP);
			break;
		case MT_SNOWWICH:
			SetMissAnim(missile, MFILE_SCBSEXPB);
			break;
		case MT_HLSPWN:
			SetMissAnim(missile, MFILE_SCBSEXPD);
			break;
		case MT_SOLBRNR:
			SetMissAnim(missile, MFILE_SCBSEXPC);
			break;
		default:
			break;
		}
	}

	missile.position.tile = Missiles[dst.x].position.tile;
	missile.position.start = Missiles[dst.x].position.start;
	missile.position.offset = Missiles[dst.x].position.offset;
	missile.position.traveled = Missiles[dst.x].position.traveled;
	missile._mirange = missile._miAnimLen;
}

void AddWeapexp(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miVar2 = dst.x;
	if (dst.x == 1)
		SetMissAnim(missile, MFILE_MAGBLOS);
	else
		SetMissAnim(missile, MFILE_MINILTNG);
	missile._mirange = missile._miAnimLen - 1;
}

void AddTown(MissileStruct &missile, Point dst, int /*midir*/)
{
	int tx = dst.x;
	int ty = dst.y;
	if (currlevel != 0) {
		missile._miDelFlag = true;
		for (int i = 0; i < 6; i++) {
			int k = CrawlNum[i];
			int ck = k + 2;
			for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
				tx = dst.x + CrawlTable[ck - 1];
				ty = dst.y + CrawlTable[ck];
				if (InDungeonBounds({ tx, ty })) {
					int dp = dPiece[tx][ty];
					if ((dFlags[i][j] & BFLAG_MISSILE) == 0 && !nSolidTable[dp] && !nMissileTable[dp] && dObject[tx][ty] == 0 && dPlayer[tx][ty] == 0) {
						if (!CheckIfTrig({ tx, ty })) {
							missile.position.tile = { tx, ty };
							missile.position.start = { tx, ty };
							missile._miDelFlag = false;
							i = 6;
							break;
						}
					}
				}
			}
		}
	} else {
		missile.position.tile = { tx, ty };
		missile.position.start = { tx, ty };
	}
	missile._mirange = 100;
	missile._miVar1 = missile._mirange - missile._miAnimLen;
	for (int i = 0; i < ActiveMissileCount; i++) {
		int mx = ActiveMissiles[i];
		if (Missiles[mx]._mitype == MIS_TOWN && (&Missiles[mx] != &missile) && Missiles[mx]._misource == missile._misource)
			Missiles[mx]._mirange = 0;
	}
	PutMissile(missile);
	if (missile._misource == MyPlayerId && !missile._miDelFlag && currlevel != 0) {
		if (!setlevel) {
			NetSendCmdLocParam3(true, CMD_ACTIVATEPORTAL, { tx, ty }, currlevel, leveltype, 0);
		} else {
			NetSendCmdLocParam3(true, CMD_ACTIVATEPORTAL, { tx, ty }, setlvlnum, leveltype, 1);
		}
	}
}

void AddFlash(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	if (missile._misource != -1) {
		if (missile._micaster == TARGET_MONSTERS) {
			int dmg = GenerateRndSum(20, Players[missile._misource]._pLevel + 1) + Players[missile._misource]._pLevel + 1;
			missile._midam = ScaleSpellEffect(dmg, missile._mispllvl);
			missile._midam += missile._midam / 2;
			UseMana(missile._misource, SPL_FLASH);
		} else {
			missile._midam = Monsters[missile._misource].mLevel * 2;
		}
	} else {
		missile._midam = currlevel / 2;
	}
	missile._mirange = 19;
}

void AddFlash2(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	if (missile._micaster == TARGET_MONSTERS) {
		if (missile._misource != -1) {
			int dmg = Players[missile._misource]._pLevel + 1;
			dmg += GenerateRndSum(20, dmg);
			missile._midam = ScaleSpellEffect(dmg, missile._mispllvl);
			missile._midam += missile._midam / 2;
		} else {
			missile._midam = currlevel / 2;
		}
	}
	missile._miPreFlag = true;
	missile._mirange = 19;
}

void AddManashield(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	auto &player = Players[missile._misource];

	if (player.pManaShield && !CanAddEffect(player, MIS_MANASHIELD)) {
		missile._miDelFlag = true;
		return;
	}

	missile._mirange = 48 * player._pLevel;
	if (missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_MANASHIELD);
	if (missile._misource == MyPlayerId)
		NetSendCmd(true, CMD_SETSHIELD);
	player.pManaShield = true;
}

void AddFiremove(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._midam = GenerateRnd(10) + Players[missile._misource]._pLevel + 1;
	UpdateMissileVelocity(missile, dst, 16);
	missile._mirange = 255;
	missile.position.tile.x++;
	missile.position.tile.y++;
	missile.position.offset.deltaY -= 32;
}

void AddGuardian(MissileStruct &missile, Point dst, int /*midir*/)
{
	auto &player = Players[missile._misource];

	int dmg = GenerateRnd(10) + (player._pLevel / 2) + 1;
	missile._midam = ScaleSpellEffect(dmg, missile._mispllvl);

	missile._miDelFlag = true;
	for (int i = 0; i < 6; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = dst.x + CrawlTable[ck - 1];
			int ty = dst.y + CrawlTable[ck];
			k = dPiece[tx][ty];
			if (InDungeonBounds({ tx, ty })) {
				if (LineClearMissile(missile.position.start, { tx, ty })) {
					if (dMonster[tx][ty] == 0 && !nSolidTable[k] && !nMissileTable[k] && dObject[tx][ty] == 0 && (dFlags[i][j] & BFLAG_MISSILE) == 0) {
						missile.position.tile = { tx, ty };
						missile.position.start = { tx, ty };
						missile._miDelFlag = false;
						UseMana(missile._misource, SPL_GUARDIAN);
						i = 6;
						break;
					}
				}
			}
		}
	}

	if (!missile._miDelFlag) {
		missile._mlid = AddLight(missile.position.tile, 1);
		missile._mirange = missile._mispllvl + (player._pLevel / 2);
		missile._mirange += (missile._mirange * player._pISplDur) / 128;

		if (missile._mirange > 30)
			missile._mirange = 30;
		missile._mirange <<= 4;
		if (missile._mirange < 30)
			missile._mirange = 30;

		missile._miVar1 = missile._mirange - missile._miAnimLen;
		missile._miVar3 = 1;
	}
}

void AddChain(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miVar1 = dst.x;
	missile._miVar2 = dst.y;
	missile._mirange = 1;
	UseMana(missile._misource, SPL_CHAIN);
}

namespace {
void InitMissileAnimationFromMonster(MissileStruct &mis, int midir, const MonsterStruct &mon, MonsterGraphic graphic)
{
	const AnimStruct &anim = mon.MType->GetAnimData(graphic);
	mis._mimfnum = midir;
	mis._miAnimFlags = MissileDataFlags::None;
	const auto &celSprite = *anim.CelSpritesForDirections[midir];
	mis._miAnimData = celSprite.Data();
	mis._miAnimDelay = anim.Rate;
	mis._miAnimLen = anim.Frames;
	mis._miAnimWidth = celSprite.Width();
	mis._miAnimWidth2 = CalculateWidth2(celSprite.Width());
	mis._miAnimAdd = 1;
	mis._miVar1 = 0;
	mis._miVar2 = 0;
	mis._miLightFlag = true;
	mis._mirange = 256;
}
} // namespace

void AddRhino(MissileStruct &missile, Point dst, int midir)
{
	auto &monster = Monsters[missile._misource];

	MonsterGraphic graphic = MonsterGraphic::Special;
	if (monster.MType->mtype < MT_HORNED || monster.MType->mtype > MT_OBLORD) {
		if (monster.MType->mtype < MT_NSNAKE || monster.MType->mtype > MT_GSNAKE) {
			graphic = MonsterGraphic::Walk;
		} else {
			graphic = MonsterGraphic::Attack;
		}
	}
	UpdateMissileVelocity(missile, dst, 18);
	InitMissileAnimationFromMonster(missile, midir, monster, graphic);
	if (monster.MType->mtype >= MT_NSNAKE && monster.MType->mtype <= MT_GSNAKE)
		missile._miAnimFrame = 7;
	if (monster._uniqtype != 0) {
		missile._miUniqTrans = monster._uniqtrans + 1;
		missile._mlid = monster.mlid;
	}
	PutMissile(missile);
}

void AddFlare(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	UpdateMissileVelocity(missile, dst, 16);
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
	if (missile._micaster == TARGET_MONSTERS) {
		UseMana(missile._misource, SPL_FLARE);
		ApplyPlrDamage(missile._misource, 5);
	} else if (missile._misource > 0) {
		auto &monster = Monsters[missile._misource];
		if (monster.MType->mtype == MT_SUCCUBUS)
			SetMissAnim(missile, MFILE_FLARE);
		if (monster.MType->mtype == MT_SNOWWICH)
			SetMissAnim(missile, MFILE_SCUBMISB);
		if (monster.MType->mtype == MT_HLSPWN)
			SetMissAnim(missile, MFILE_SCUBMISD);
		if (monster.MType->mtype == MT_SOLBRNR)
			SetMissAnim(missile, MFILE_SCUBMISC);
	}

	if (MissileSpriteData[missile._miAnimType].animFAmt == 16) {
		SetMissDir(missile, GetDirection16(missile.position.start, dst));
	}
}

void AddAcid(MissileStruct &missile, Point dst, int /*midir*/)
{
	UpdateMissileVelocity(missile, dst, 16);
	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	if ((!gbIsHellfire && (missile.position.velocity.deltaX & 0xFFFF0000) != 0) || (missile.position.velocity.deltaY & 0xFFFF0000) != 0)
		missile._mirange = 5 * (Monsters[missile._misource]._mint + 4);
	else
		missile._mirange = 1;
	missile._mlid = NO_LIGHT;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	PutMissile(missile);
}

void AddAcidpud(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miLightFlag = true;
	int monst = missile._misource;
	missile._mirange = GenerateRnd(15) + 40 * (Monsters[monst]._mint + 1);
	missile._miPreFlag = true;
}

void AddStone(MissileStruct &missile, Point dst, int /*midir*/)
{
	int tx;
	int ty;

	bool found = false;
	for (int i = 0; i < 6; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			tx = dst.x + CrawlTable[ck - 1];
			ty = dst.y + CrawlTable[ck];
			if (!InDungeonBounds({ tx, ty }))
				continue;

			int mid = dMonster[tx][ty];
			if (mid == 0)
				continue;
			mid = abs(mid) - 1;
			auto &monster = Monsters[mid];

			if (IsAnyOf(monster.MType->mtype, MT_GOLEM, MT_DIABLO, MT_NAKRUL))
				continue;

			if (IsAnyOf(monster._mmode, MM_FADEIN, MM_FADEOUT, MM_CHARGE))
				continue;

			found = true;
			missile._miVar1 = monster._mmode;
			missile._miVar2 = mid;
			monster.Petrify();
			i = 6;
			break;
		}
	}

	if (!found) {
		missile._miDelFlag = true;
		return;
	}

	missile.position.tile = { tx, ty };
	missile.position.start = missile.position.tile;
	missile._mirange = missile._mispllvl + 6;
	missile._mirange += (missile._mirange * Players[missile._misource]._pISplDur) / 128;

	if (missile._mirange > 15)
		missile._mirange = 15;
	missile._mirange <<= 4;
	UseMana(missile._misource, SPL_STONE);
}

void AddGolem(MissileStruct &missile, Point dst, int /*midir*/)
{
	for (int i = 0; i < ActiveMissileCount; i++) {
		int mx = ActiveMissiles[i];
		if (Missiles[mx]._mitype == MIS_GOLEM) {
			if ((&Missiles[mx] != &missile) && Missiles[mx]._misource == missile._misource) {
				missile._miDelFlag = true;
				return;
			}
		}
	}
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miVar4 = dst.x;
	missile._miVar5 = dst.y;
	if (Monsters[missile._misource].position.tile != GolemHoldingCell && missile._misource == MyPlayerId)
		M_StartKill(missile._misource, missile._misource);
	UseMana(missile._misource, SPL_GOLEM);
}

void AddBoom(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile.position.tile = dst;
	missile.position.start = dst;
	missile._mirange = missile._miAnimLen;
}

void AddHeal(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	auto &player = Players[missile._misource];

	int hp = (GenerateRnd(10) + 1) << 6;
	for (int i = 0; i < player._pLevel; i++) {
		hp += (GenerateRnd(4) + 1) << 6;
	}
	for (int i = 0; i < missile._mispllvl; i++) {
		hp += (GenerateRnd(6) + 1) << 6;
	}

	if (player._pClass == HeroClass::Warrior || player._pClass == HeroClass::Barbarian || player._pClass == HeroClass::Monk) {
		hp *= 2;
	} else if (player._pClass == HeroClass::Rogue || player._pClass == HeroClass::Bard) {
		hp += hp / 2;
	}

	player._pHitPoints = std::min(player._pHitPoints + hp, player._pMaxHP);
	player._pHPBase = std::min(player._pHPBase + hp, player._pMaxHPBase);

	UseMana(missile._misource, SPL_HEAL);
	missile._miDelFlag = true;
	drawhpflag = true;
}

void AddHealOther(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_HEALOTHER);
	if (missile._misource == MyPlayerId) {
		NewCursor(CURSOR_HEALOTHER);
		if (sgbControllerActive)
			TryIconCurs();
	}
}

void AddElement(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}

	int dmg = 2 * (Players[missile._misource]._pLevel + GenerateRndSum(10, 2)) + 4;
	missile._midam = ScaleSpellEffect(dmg, missile._mispllvl) / 2;

	UpdateMissileVelocity(missile, dst, 16);
	SetMissDir(missile, GetDirection(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miVar4 = dst.x;
	missile._miVar5 = dst.y;
	missile._mlid = AddLight(missile.position.start, 8);
	UseMana(missile._misource, SPL_ELEMENT);
}

extern void FocusOnInventory();

void AddIdentify(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_IDENTIFY);
	if (missile._misource == MyPlayerId) {
		if (sbookflag)
			sbookflag = false;
		if (!invflag) {
			invflag = true;
			if (sgbControllerActive)
				FocusOnInventory();
		}
		NewCursor(CURSOR_IDENTIFY);
	}
}

void AddFirewallC(MissileStruct &missile, Point dst, int midir)
{
	missile._miDelFlag = true;
	for (int i = 0; i < 6; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			int tx = dst.x + CrawlTable[ck - 1];
			int ty = dst.y + CrawlTable[ck];
			if (0 < tx && tx < MAXDUNX && 0 < ty && ty < MAXDUNY) {
				k = dPiece[tx][ty];
				if (LineClearMissile(missile.position.start, { tx, ty })) {
					if (missile.position.start != Point { tx, ty } && !nSolidTable[k] && dObject[tx][ty] == 0) {
						missile._miVar1 = tx;
						missile._miVar2 = ty;
						missile._miVar5 = tx;
						missile._miVar6 = ty;
						missile._miDelFlag = false;
						i = 6;
						break;
					}
				}
			}
		}
	}

	if (!missile._miDelFlag) {
		missile._miVar3 = left[left[midir]];
		missile._miVar4 = right[right[midir]];
		missile._mirange = 7;
		UseMana(missile._misource, SPL_FIREWALL);
	}
}

void AddInfra(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._mirange = ScaleSpellEffect(1584, missile._mispllvl);
	missile._mirange += missile._mirange * Players[missile._misource]._pISplDur / 128;

	if (missile._micaster == TARGET_MONSTERS)
		UseMana(missile._misource, SPL_INFRA);
}

void AddWave(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miVar1 = dst.x;
	missile._miVar2 = dst.y;
	missile._mirange = 1;
	missile._miAnimFrame = 4;
	UseMana(missile._misource, SPL_WAVE);
}

void AddNova(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile._miVar1 = dst.x;
	missile._miVar2 = dst.y;

	if (missile._misource != -1) {
		int dmg = GenerateRndSum(6, 5) + Players[missile._misource]._pLevel + 5;
		missile._midam = ScaleSpellEffect(dmg / 2, missile._mispllvl);

		if (missile._micaster == TARGET_MONSTERS)
			UseMana(missile._misource, SPL_NOVA);
	} else {
		missile._midam = (currlevel / 2) + GenerateRndSum(3, 3);
	}

	missile._mirange = 1;
}

void AddBlodboil(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	auto &player = Players[missile._misource];

	if ((player._pSpellFlags & 6) != 0 || player._pHitPoints <= player._pLevel << 6) {
		missile._miDelFlag = true;
		return;
	}

	UseMana(missile._misource, SPL_BLODBOIL);
	int tmp = 3 * player._pLevel;
	tmp <<= 7;
	player._pSpellFlags |= 2;
	missile._miVar2 = tmp;
	int lvl = player._pLevel * 2;
	missile._mirange = lvl + 10 * missile._mispllvl + 245;
	CalcPlrItemVals(missile._misource, true);
	force_redraw = 255;
	player.Say(HeroSpeech::Aaaaargh);
}

void AddRepair(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_REPAIR);
	if (missile._misource == MyPlayerId) {
		if (sbookflag)
			sbookflag = false;
		if (!invflag) {
			invflag = true;
			if (sgbControllerActive)
				FocusOnInventory();
		}
		NewCursor(CURSOR_REPAIR);
	}
}

void AddRecharge(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_RECHARGE);
	if (missile._misource == MyPlayerId) {
		if (sbookflag)
			sbookflag = false;
		if (!invflag) {
			invflag = true;
			if (sgbControllerActive)
				FocusOnInventory();
		}
		NewCursor(CURSOR_RECHARGE);
	}
}

void AddDisarm(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_DISARM);
	if (missile._misource == MyPlayerId) {
		NewCursor(CURSOR_DISARM);
		if (sgbControllerActive) {
			if (pcursobj != -1)
				NetSendCmdLocParam1(true, CMD_DISARMXY, { cursmx, cursmy }, pcursobj);
			else
				NewCursor(CURSOR_HAND);
		}
	}
}

void AddApoca(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miVar1 = 8;
	missile._miVar2 = std::max(missile.position.start.y - 8, 1);
	missile._miVar3 = std::min(missile.position.start.y + 8, MAXDUNY - 1);
	missile._miVar4 = std::max(missile.position.start.x - 8, 1);
	missile._miVar5 = std::min(missile.position.start.x + 8, MAXDUNX - 1);
	missile._miVar6 = missile._miVar4;
	int playerLevel = Players[missile._misource]._pLevel;
	missile._midam = GenerateRndSum(6, playerLevel) + playerLevel;
	missile._mirange = 255;
	UseMana(missile._misource, SPL_APOCA);
}

void AddFlame(MissileStruct &missile, Point dst, int midir)
{
	missile._miVar2 = 5 * missile._midam;
	missile.position.start = dst;
	missile.position.offset = Missiles[midir].position.offset;
	missile.position.traveled = Missiles[midir].position.traveled;
	missile._mirange = missile._miVar2 + 20;
	missile._mlid = AddLight(missile.position.start, 1);
	if (missile._micaster == TARGET_MONSTERS) {
		int i = GenerateRnd(Players[missile._misource]._pLevel) + GenerateRnd(2);
		missile._midam = 8 * i + 16 + ((8 * i + 16) / 2);
	} else {
		auto &monster = Monsters[missile._misource];
		missile._midam = monster.mMinDamage + GenerateRnd(monster.mMaxDamage - monster.mMinDamage + 1);
	}
}

void AddFlamec(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	UpdateMissileVelocity(missile, dst, 32);
	if (missile._micaster == TARGET_MONSTERS) {
		UseMana(missile._misource, SPL_FLAME);
	}
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mirange = 256;
}

void AddCbolt(MissileStruct &missile, Point dst, int midir)
{
	missile._mirnd = GenerateRnd(15) + 1;
	missile._midam = (missile._micaster == TARGET_MONSTERS) ? (GenerateRnd(Players[missile._misource]._pMagic / 4) + 1) : 15;

	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	missile._miAnimFrame = GenerateRnd(8) + 1;
	missile._mlid = AddLight(missile.position.start, 5);

	UpdateMissileVelocity(missile, dst, 8);
	missile._miVar1 = 5;
	missile._miVar2 = midir;
	missile._mirange = 256;
}

void AddHbolt(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	int sp = 16;
	if (missile._misource != -1) {
		sp += std::min(missile._mispllvl * 2, 47);
	}

	UpdateMissileVelocity(missile, dst, sp);
	SetMissDir(missile, GetDirection16(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._mlid = AddLight(missile.position.start, 8);
	missile._midam = GenerateRnd(10) + Players[missile._misource]._pLevel + 9;
	UseMana(missile._misource, SPL_HBOLT);
}

void AddResurrect(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	UseMana(missile._misource, SPL_RESURRECT);
	if (missile._misource == MyPlayerId) {
		NewCursor(CURSOR_RESURRECT);
		if (sgbControllerActive)
			TryIconCurs();
	}
	missile._miDelFlag = true;
}

void AddResurrectBeam(MissileStruct &missile, Point dst, int /*midir*/)
{
	missile.position.tile = dst;
	missile.position.start = dst;
	missile._mirange = MissileSpriteData[MFILE_RESSUR1].animLen[0];
}

void AddTelekinesis(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._miDelFlag = true;
	UseMana(missile._misource, SPL_TELEKINESIS);
	if (missile._misource == MyPlayerId)
		NewCursor(CURSOR_TELEKINESIS);
}

void AddBoneSpirit(MissileStruct &missile, Point dst, int midir)
{
	if (missile.position.start == dst) {
		dst += static_cast<Direction>(midir);
	}
	UpdateMissileVelocity(missile, dst, 16);
	SetMissDir(missile, GetDirection(missile.position.start, dst));
	missile._mirange = 256;
	missile._miVar1 = missile.position.start.x;
	missile._miVar2 = missile.position.start.y;
	missile._miVar4 = dst.x;
	missile._miVar5 = dst.y;
	missile._mlid = AddLight(missile.position.start, 8);
	if (missile._micaster == TARGET_MONSTERS) {
		UseMana(missile._misource, SPL_BONESPIRIT);
		ApplyPlrDamage(missile._misource, 6);
	}
}

void AddRportal(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	missile._mirange = 100;
	missile._miVar1 = 100 - missile._miAnimLen;
	PutMissile(missile);
}

void AddDiabApoca(MissileStruct &missile, Point /*dst*/, int /*midir*/)
{
	int players = gbIsMultiplayer ? MAX_PLRS : 1;
	for (int pnum = 0; pnum < players; pnum++) {
		auto &player = Players[pnum];
		if (!player.plractive)
			continue;
		if (!LineClearMissile(missile.position.start, player.position.future))
			continue;

		AddMissile({ 0, 0 }, player.position.future, 0, MIS_BOOM2, missile._micaster, missile._misource, missile._midam, 0);
	}
	missile._miDelFlag = true;
}

int AddMissile(Point src, Point dst, int midir, missile_id mitype, mienemy_type micaster, int id, int midam, int spllvl)
{
	if (ActiveMissileCount >= MAXMISSILES - 1)
		return -1;

	int mi = AvailableMissiles[0];
	auto &missile = Missiles[mi];

	AvailableMissiles[0] = AvailableMissiles[MAXMISSILES - ActiveMissileCount - 1];
	ActiveMissiles[ActiveMissileCount] = mi;
	ActiveMissileCount++;

	memset(&missile, 0, sizeof(missile));

	missile._mitype = mitype;
	missile._micaster = micaster;
	missile._misource = id;
	missile._midam = midam;
	missile._miAnimType = MissileData[mitype].mFileNum;
	missile._miDrawFlag = MissileData[mitype].mDraw;
	missile._mispllvl = spllvl;
	missile._mimfnum = midir;

	if (missile._miAnimType == MFILE_NONE || MissileSpriteData[missile._miAnimType].animFAmt < 8)
		SetMissDir(missile, 0);
	else
		SetMissDir(missile, midir);

	missile.position.tile = src;
	missile.position.start = src;
	missile._miAnimAdd = 1;
	missile._mlid = NO_LIGHT;

	if (MissileData[mitype].mlSFX != -1) {
		PlaySfxLoc(MissileData[mitype].mlSFX, missile.position.start);
	}

	MissileData[mitype].mAddProc(missile, dst, midir);

	return mi;
}

void MI_Golem(int mi)
{
	auto &missile = Missiles[mi];
	int src = missile._misource;
	if (Monsters[src].position.tile == GolemHoldingCell) {
		for (int i = 0; i < 6; i++) {
			int k = CrawlNum[i];
			int ck = k + 2;
			for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
				const int8_t *ct = &CrawlTable[ck];
				int tx = missile._miVar4 + *(ct - 1);
				int ty = missile._miVar5 + *ct;

				if (!InDungeonBounds({ tx, ty }))
					continue;

				if (!LineClearMissile({ missile._miVar1, missile._miVar2 }, { tx, ty }))
					continue;

				if (dMonster[tx][ty] != 0 || nSolidTable[dPiece[tx][ty]] || dObject[tx][ty] != 0)
					continue;

				SpawnGolum(src, { tx, ty }, mi);
				i = 6;
				break;
			}
		}
	}
	missile._miDelFlag = true;
}

void MI_LArrow(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int p = missile._misource;
	if (missile._miAnimType == MFILE_MINILTNG || missile._miAnimType == MFILE_MAGBLOS) {
		ChangeLight(missile._mlid, missile.position.tile, missile._miAnimFrame + 5);
		missile_resistance rst = MissileData[missile._mitype].mResist;
		if (missile._mitype == MIS_LARROW) {
			int mind;
			int maxd;
			if (p != -1) {
				mind = Players[p]._pILMinDam;
				maxd = Players[p]._pILMaxDam;
			} else {
				mind = GenerateRnd(10) + 1 + currlevel;
				maxd = GenerateRnd(10) + 1 + currlevel * 2;
			}
			MissileData[MIS_LARROW].mResist = MISR_LIGHTNING;
			CheckMissileCol(missile, mind, maxd, false, missile.position.tile, true);
		}
		if (missile._mitype == MIS_FARROW) {
			int mind;
			int maxd;
			if (p != -1) {
				mind = Players[p]._pIFMinDam;
				maxd = Players[p]._pIFMaxDam;
			} else {
				mind = GenerateRnd(10) + 1 + currlevel;
				maxd = GenerateRnd(10) + 1 + currlevel * 2;
			}
			MissileData[MIS_FARROW].mResist = MISR_FIRE;
			CheckMissileCol(missile, mind, maxd, false, missile.position.tile, true);
		}
		MissileData[missile._mitype].mResist = rst;
	} else {
		missile._midist++;
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);

		int mind;
		int maxd;
		if (p != -1) {
			if (missile._micaster == TARGET_MONSTERS) {
				mind = Players[p]._pIMinDam;
				maxd = Players[p]._pIMaxDam;
			} else {
				mind = Monsters[p].mMinDamage;
				maxd = Monsters[p].mMaxDamage;
			}
		} else {
			mind = GenerateRnd(10) + 1 + currlevel;
			maxd = GenerateRnd(10) + 1 + currlevel * 2;
		}

		if (missile.position.tile != missile.position.start) {
			missile_resistance rst = MissileData[missile._mitype].mResist;
			MissileData[missile._mitype].mResist = MISR_NONE;
			CheckMissileCol(missile, mind, maxd, false, missile.position.tile, false);
			MissileData[missile._mitype].mResist = rst;
		}
		if (missile._mirange == 0) {
			missile._mimfnum = 0;
			missile.position.traveled -= missile.position.velocity;
			UpdateMissilePos(missile);
			if (missile._mitype == MIS_LARROW)
				SetMissAnim(missile, MFILE_MINILTNG);
			else
				SetMissAnim(missile, MFILE_MAGBLOS);
			missile._mirange = missile._miAnimLen - 1;
			missile.position.StopMissile();
		} else {
			if (missile.position.tile.x != missile._miVar1 || missile.position.tile.y != missile._miVar2) {
				missile._miVar1 = missile.position.tile.x;
				missile._miVar2 = missile.position.tile.y;
				ChangeLight(missile._mlid, { missile._miVar1, missile._miVar2 }, 5);
			}
		}
	}
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

void MI_Arrow(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	missile._midist++;
	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);
	int p = missile._misource;

	int mind;
	int maxd;
	if (p != -1) {
		if (missile._micaster == TARGET_MONSTERS) {
			mind = Players[p]._pIMinDam;
			maxd = Players[p]._pIMaxDam;
		} else {
			mind = Monsters[p].mMinDamage;
			maxd = Monsters[p].mMaxDamage;
		}
	} else {
		mind = currlevel;
		maxd = 2 * currlevel;
	}
	if (missile.position.tile != missile.position.start)
		CheckMissileCol(missile, mind, maxd, false, missile.position.tile, false);
	if (missile._mirange == 0)
		missile._miDelFlag = true;
	PutMissile(missile);
}

void MI_Firebolt(int i)
{
	auto &missile = Missiles[i];
	int d;

	missile._mirange--;
	if (missile._mitype != MIS_BONESPIRIT || missile._mimfnum != 8) {
		int omx = missile.position.traveled.deltaX;
		int omy = missile.position.traveled.deltaY;
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
		int p = missile._misource;
		if (p != -1) {
			if (missile._micaster == TARGET_MONSTERS) {
				auto &player = Players[p];
				switch (missile._mitype) {
				case MIS_FIREBOLT:
					d = GenerateRnd(10) + (player._pMagic / 8) + missile._mispllvl + 1;
					break;
				case MIS_FLARE:
					d = 3 * missile._mispllvl - (player._pMagic / 8) + (player._pMagic / 2);
					break;
				case MIS_BONESPIRIT:
					d = 0;
					break;
				default:
					break;
				}
			} else {
				auto &monster = Monsters[p];
				d = monster.mMinDamage + GenerateRnd(monster.mMaxDamage - monster.mMinDamage + 1);
			}
		} else {
			d = currlevel + GenerateRnd(2 * currlevel);
		}
		if (missile.position.tile != missile.position.start) {
			CheckMissileCol(missile, d, d, false, missile.position.tile, false);
		}
		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			missile.position.traveled = { omx, omy };
			UpdateMissilePos(missile);
			missile.position.StopMissile();
			switch (missile._mitype) {
			case MIS_FIREBOLT:
			case MIS_MAGMABALL:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_MISEXP, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_FLARE:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_MISEXP2, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_ACID:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_MISEXP3, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_BONESPIRIT:
				SetMissDir(missile, 8);
				missile._mirange = 7;
				missile._miDelFlag = false;
				PutMissile(missile);
				return;
			case MIS_LICH:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_EXORA1, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_PSYCHORB:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_EXBL2, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_NECROMORB:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_EXRED3, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_ARCHLICH:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_EXYEL2, missile._micaster, missile._misource, 0, 0);
				break;
			case MIS_BONEDEMON:
				AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_EXBL3, missile._micaster, missile._misource, 0, 0);
				break;
			default:
				break;
			}
			if (missile._mlid != NO_LIGHT)
				AddUnLight(missile._mlid);
			PutMissile(missile);
		} else {
			if (missile.position.tile.x != missile._miVar1 || missile.position.tile.y != missile._miVar2) {
				missile._miVar1 = missile.position.tile.x;
				missile._miVar2 = missile.position.tile.y;
				if (missile._mlid != NO_LIGHT)
					ChangeLight(missile._mlid, { missile._miVar1, missile._miVar2 }, 8);
			}
			PutMissile(missile);
		}
	} else if (missile._mirange == 0) {
		if (missile._mlid != NO_LIGHT)
			AddUnLight(missile._mlid);
		missile._miDelFlag = true;
		PlaySfxLoc(LS_BSIMPCT, missile.position.tile);
		PutMissile(missile);
	} else
		PutMissile(missile);
}

void MI_Lightball(int i)
{
	auto &missile = Missiles[i];
	int tx = missile._miVar1;
	int ty = missile._miVar2;
	missile._mirange--;
	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);
	int j = missile._mirange;
	CheckMissileCol(missile, missile._midam, missile._midam, false, missile.position.tile, false);
	if (missile._miHitFlag)
		missile._mirange = j;
	int8_t obj = dObject[tx][ty];
	if (obj != 0 && tx == missile.position.tile.x && ty == missile.position.tile.y) {
		int oi = (obj > 0) ? (obj - 1) : -(obj + 1);
		if (Objects[oi]._otype == OBJ_SHRINEL || Objects[oi]._otype == OBJ_SHRINER)
			missile._mirange = j;
	}
	if (missile._mirange == 0)
		missile._miDelFlag = true;
	PutMissile(missile);
}

void MI_Acidpud(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int range = missile._mirange;
	CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile, false);
	missile._mirange = range;
	if (range == 0) {
		if (missile._mimfnum != 0) {
			missile._miDelFlag = true;
		} else {
			SetMissDir(missile, 1);
			missile._mirange = missile._miAnimLen;
		}
	}
	PutMissile(missile);
}

void MI_Firewall(int i)
{
	auto &missile = Missiles[i];
	constexpr int ExpLight[14] = { 2, 3, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12, 12 };

	missile._mirange--;
	if (missile._mirange == missile._miVar1) {
		SetMissDir(missile, 1);
		missile._miAnimFrame = GenerateRnd(11) + 1;
	}
	if (missile._mirange == missile._miAnimLen - 1) {
		SetMissDir(missile, 0);
		missile._miAnimFrame = 13;
		missile._miAnimAdd = -1;
	}
	CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile, true);
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	if (missile._mimfnum != 0 && missile._mirange != 0 && missile._miAnimAdd != -1 && missile._miVar2 < 12) {
		if (missile._miVar2 == 0)
			missile._mlid = AddLight(missile.position.tile, ExpLight[0]);
		ChangeLight(missile._mlid, missile.position.tile, ExpLight[missile._miVar2]);
		missile._miVar2++;
	}
	PutMissile(missile);
}

void MI_Fireball(int i)
{
	auto &missile = Missiles[i];
	FireballUpdate(i, missile.position.velocity, false);
}

void MI_HorkSpawn(int mi)
{
	auto &missile = Missiles[mi];
	missile._mirange--;
	CheckMissileCol(missile, 0, 0, false, missile.position.tile, false);
	if (missile._mirange <= 0) {
		missile._miDelFlag = true;
		for (int i = 0; i < 2; i++) {
			int k = CrawlNum[i];
			int ck = k + 2;
			for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
				int tx = missile.position.tile.x + CrawlTable[ck - 1];
				int ty = missile.position.tile.y + CrawlTable[ck];
				if (!InDungeonBounds({ tx, ty }))
					continue;

				int dp = dPiece[tx][ty];
				if (nSolidTable[dp] || dMonster[tx][ty] != 0 || dPlayer[tx][ty] != 0 || dObject[tx][ty] != 0)
					continue;

				auto md = static_cast<Direction>(missile._miVar1);
				int monsterId = AddMonster({ tx, ty }, md, 1, true);
				if (monsterId != -1)
					M_StartStand(Monsters[monsterId], md);

				i = 6;
				break;
			}
		}
	} else {
		missile._midist++;
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
	}
	PutMissile(missile);
}

void MI_Rune(int i)
{
	auto &missile = Missiles[i];
	int mx = missile.position.tile.x;
	int my = missile.position.tile.y;
	int mid = dMonster[mx][my];
	int8_t pid = dPlayer[mx][my];
	if (mid != 0 || pid != 0) {
		Direction dir;
		if (mid != 0) {
			mid = (mid > 0) ? (mid - 1) : -(mid + 1);
			dir = GetDirection(missile.position.tile, Monsters[mid].position.tile);
		} else {
			pid = (pid > 0) ? (pid - 1) : -(pid + 1);
			dir = GetDirection(missile.position.tile, Players[pid].position.tile);
		}
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
		AddMissile({ mx, my }, { mx, my }, dir, static_cast<missile_id>(missile._miVar1), TARGET_BOTH, missile._misource, missile._midam, missile._mispllvl);
	}
	PutMissile(missile);
}

void MI_LightningWall(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int range = missile._mirange;
	CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile, false);
	if (missile._miHitFlag)
		missile._mirange = range;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
	PutMissile(missile);
}

void MI_HiveExplode(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange <= 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

void MI_LightningArrow(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);

	int mx = missile.position.tile.x;
	int my = missile.position.tile.y;
	assert(mx >= 0 && mx < MAXDUNX);
	assert(my >= 0 && my < MAXDUNY);
	int pn = dPiece[mx][my];
	assert(pn >= 0 && pn <= MAXTILES);

	if (missile._misource == -1) {
		if ((mx != missile.position.start.x || my != missile.position.start.y) && nMissileTable[pn]) {
			missile._mirange = 0;
		}
	} else if (nMissileTable[pn]) {
		missile._mirange = 0;
	}

	if (!nMissileTable[pn]) {
		if ((mx != missile._miVar1 || my != missile._miVar2) && mx > 0 && my > 0 && mx < MAXDUNX && my < MAXDUNY) {
			if (missile._misource != -1) {
				if (missile._micaster == TARGET_PLAYERS
				    && IsAnyOf(Monsters[missile._misource].MType->mtype, MT_STORM, MT_RSTORM, MT_STORML, MT_MAEL)) {
					AddMissile(
					    missile.position.tile,
					    missile.position.start,
					    i,
					    MIS_LIGHTNING2,
					    missile._micaster,
					    missile._misource,
					    missile._midam,
					    missile._mispllvl);
				} else {
					AddMissile(
					    missile.position.tile,
					    missile.position.start,
					    i,
					    MIS_LIGHTNING,
					    missile._micaster,
					    missile._misource,
					    missile._midam,
					    missile._mispllvl);
				}
			} else {
				AddMissile(
				    missile.position.tile,
				    missile.position.start,
				    i,
				    MIS_LIGHTNING,
				    missile._micaster,
				    missile._misource,
				    missile._midam,
				    missile._mispllvl);
			}
			missile._miVar1 = missile.position.tile.x;
			missile._miVar2 = missile.position.tile.y;
		}
	}

	if (missile._mirange == 0 || mx <= 0 || my <= 0 || mx >= MAXDUNX || my > MAXDUNY) { // BUGFIX my >= MAXDUNY
		missile._miDelFlag = true;
	}
}

void MI_FireRing(int i)
{
	auto &missile = Missiles[i];
	missile._miDelFlag = true;
	int8_t src = missile._misource;
	uint8_t lvl = missile._micaster == TARGET_MONSTERS ? Players[src]._pLevel : currlevel;
	int dmg = 16 * (GenerateRndSum(10, 2) + lvl + 2) / 2;

	int k = CrawlNum[3];
	int ck = k + 2;
	for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
		int tx = missile._miVar1 + CrawlTable[ck - 1];
		int ty = missile._miVar2 + CrawlTable[ck];
		if (!InDungeonBounds({ tx, ty }))
			continue;
		int dp = dPiece[tx][ty];
		if (nSolidTable[dp])
			continue;
		if (dObject[tx][ty] != 0)
			continue;
		if (!LineClearMissile(missile.position.tile, { tx, ty }))
			continue;
		if (nMissileTable[dp] || missile.limitReached) {
			missile.limitReached = true;
			continue;
		}

		AddMissile({ tx, ty }, { tx, ty }, 0, MIS_FIREWALL, TARGET_BOTH, src, dmg, missile._mispllvl);
	}
}

void MI_Search(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange != 0)
		return;

	missile._miDelFlag = true;
	PlaySfxLoc(IS_CAST7, Players[missile._miVar1].position.tile);
	AutoMapShowItems = false;
}

void MI_LightningWallC(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		return;
	}

	int id = missile._misource;
	int lvl = (id > -1) ? Players[id]._pLevel : 0;
	int dmg = 16 * (GenerateRndSum(10, 2) + lvl + 2);

	{
		Point position = { missile._miVar1, missile._miVar2 };
		Point target = position + static_cast<Direction>(missile._miVar3);

		if (!missile.limitReached && GrowWall(id, position, target, MIS_LIGHTWALL, missile._mispllvl, dmg)) {
			missile._miVar1 = target.x;
			missile._miVar2 = target.y;
		} else {
			missile.limitReached = true;
		}
	}

	{
		Point position = { missile._miVar5, missile._miVar6 };
		Point target = position + static_cast<Direction>(missile._miVar4);

		if (missile._miVar7 == 0 && GrowWall(id, position, target, MIS_LIGHTWALL, missile._mispllvl, dmg)) {
			missile._miVar5 = target.x;
			missile._miVar6 = target.y;
		} else {
			missile._miVar7 = 1;
		}
	}
}

void MI_FireNova(int i)
{
	auto &missile = Missiles[i];
	int sx1 = 0;
	int sy1 = 0;
	int id = missile._misource;
	int dam = missile._midam;
	Point src = missile.position.tile;
	Direction dir = DIR_S;
	mienemy_type en = TARGET_PLAYERS;
	if (id != -1) {
		dir = Players[id]._pdir;
		en = TARGET_MONSTERS;
	}
	for (const auto &k : VisionCrawlTable) {
		if (sx1 != k[6] || sy1 != k[7]) {
			Displacement offsets[] = { { k[6], k[7] }, { -k[6], -k[7] }, { -k[6], +k[7] }, { +k[6], -k[7] } };
			for (Displacement offset : offsets)
				AddMissile(src, src + offset, dir, MIS_FIRENOVA, en, id, dam, missile._mispllvl);
			sx1 = k[6];
			sy1 = k[7];
		}
	}
	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
}

void MI_SpecArrow(int i)
{
	auto &missile = Missiles[i];
	int id = missile._misource;
	int dam = missile._midam;
	Point src = missile.position.tile;
	Point dst = { missile._miVar1, missile._miVar2 };
	int spllvl = missile._miVar3;
	missile_id mitype = MIS_ARROW;
	Direction dir = DIR_S;
	mienemy_type micaster = TARGET_PLAYERS;
	if (id != -1) {
		auto &player = Players[id];
		dir = player._pdir;
		micaster = TARGET_MONSTERS;

		switch (player._pILMinDam) {
		case 0:
			mitype = MIS_FIRENOVA;
			break;
		case 1:
			mitype = MIS_LIGHTARROW;
			break;
		case 2:
			mitype = MIS_CBOLTARROW;
			break;
		case 3:
			mitype = MIS_HBOLTARROW;
			break;
		}
	}
	AddMissile(src, dst, dir, mitype, micaster, id, dam, spllvl);
	if (mitype == MIS_CBOLTARROW) {
		AddMissile(src, dst, dir, mitype, micaster, id, dam, spllvl);
		AddMissile(src, dst, dir, mitype, micaster, id, dam, spllvl);
	}
	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
}

void MI_Lightctrl(int i)
{
	assert(i >= 0 && i < MAXMISSILES);
	auto &missile = Missiles[i];
	missile._mirange--;

	int dam;
	int id = missile._misource;
	if (id != -1) {
		if (missile._micaster == TARGET_MONSTERS) {
			dam = (GenerateRnd(2) + GenerateRnd(Players[id]._pLevel) + 2) << 6;
		} else {
			auto &monster = Monsters[id];
			dam = 2 * (monster.mMinDamage + GenerateRnd(monster.mMaxDamage - monster.mMinDamage + 1));
		}
	} else {
		dam = GenerateRnd(currlevel) + 2 * currlevel;
	}

	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);

	int mx = missile.position.tile.x;
	int my = missile.position.tile.y;
	assert(mx >= 0 && mx < MAXDUNX);
	assert(my >= 0 && my < MAXDUNY);
	int pn = dPiece[mx][my];
	assert(pn >= 0 && pn <= MAXTILES);

	if (id != -1 || Point { mx, my } != missile.position.start) {
		if (nMissileTable[pn]) {
			missile._mirange = 0;
		}
	}
	if (!nMissileTable[pn]
	    && Point { mx, my } != Point { missile._miVar1, missile._miVar2 }
	    && InDungeonBounds({ mx, my })) {
		if (id != -1) {
			if (missile._micaster == TARGET_PLAYERS
			    && IsAnyOf(Monsters[id].MType->mtype, MT_STORM, MT_RSTORM, MT_STORML, MT_MAEL)) {
				AddMissile(
				    missile.position.tile,
				    missile.position.start,
				    i,
				    MIS_LIGHTNING2,
				    missile._micaster,
				    id,
				    dam,
				    missile._mispllvl);
			} else {
				AddMissile(
				    missile.position.tile,
				    missile.position.start,
				    i,
				    MIS_LIGHTNING,
				    missile._micaster,
				    id,
				    dam,
				    missile._mispllvl);
			}
		} else {
			AddMissile(
			    missile.position.tile,
			    missile.position.start,
			    i,
			    MIS_LIGHTNING,
			    missile._micaster,
			    id,
			    dam,
			    missile._mispllvl);
		}
		missile._miVar1 = missile.position.tile.x;
		missile._miVar2 = missile.position.tile.y;
	}
	assert(mx != 0 && my != 0);
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
	}
}

void MI_Lightning(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int j = missile._mirange;
	if (missile.position.tile != missile.position.start)
		CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile, false);
	if (missile._miHitFlag)
		missile._mirange = j;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

void MI_Town(int i)
{
	auto &missile = Missiles[i];
	int expLight[17] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15 };

	if (missile._mirange > 1)
		missile._mirange--;
	if (missile._mirange == missile._miVar1)
		SetMissDir(missile, 1);
	if (currlevel != 0 && missile._mimfnum != 1 && missile._mirange != 0) {
		if (missile._miVar2 == 0)
			missile._mlid = AddLight(missile.position.tile, 1);
		ChangeLight(missile._mlid, missile.position.tile, expLight[missile._miVar2]);
		missile._miVar2++;
	}

	for (int p = 0; p < MAX_PLRS; p++) {
		auto &player = Players[p];
		if (player.plractive && currlevel == player.plrlevel && !player._pLvlChanging && player._pmode == PM_STAND && player.position.tile == missile.position.tile) {
			ClrPlrPath(player);
			if (p == MyPlayerId) {
				NetSendCmdParam1(true, CMD_WARP, missile._misource);
				player._pmode = PM_NEWLVL;
			}
		}
	}

	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

void MI_Flash(int i)
{
	auto &missile = Missiles[i];
	if (missile._micaster == TARGET_MONSTERS) {
		if (missile._misource != -1)
			Players[missile._misource]._pInvincible = true;
	}
	missile._mirange--;

	constexpr Displacement Offsets[] = { { -1, 0 }, { 0, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
	for (Displacement offset : Offsets)
		CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile + offset, true);

	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		if (missile._micaster == TARGET_MONSTERS) {
			if (missile._misource != -1)
				Players[missile._misource]._pInvincible = false;
		}
	}
	PutMissile(missile);
}

void MI_Flash2(int i)
{
	auto &missile = Missiles[i];
	if (missile._micaster == TARGET_MONSTERS) {
		if (missile._misource != -1)
			Players[missile._misource]._pInvincible = true;
	}
	missile._mirange--;

	constexpr Displacement Offsets[] = { { -1, -1 }, { 0, -1 }, { 1, -1 } };
	for (Displacement offset : Offsets)
		CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile + offset, true);

	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		if (missile._micaster == TARGET_MONSTERS) {
			if (missile._misource != -1)
				Players[missile._misource]._pInvincible = false;
		}
	}
	PutMissile(missile);
}

void MI_Manashield(int i)
{
	auto &missile = Missiles[i];
	int id = missile._misource;
	if (id != MyPlayerId) {
		if (currlevel != Players[id].plrlevel)
			missile._miDelFlag = true;
	} else {
		if (Players[id]._pMana <= 0 || !Players[id].plractive)
			missile._mirange = 0;

		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			NetSendCmd(true, CMD_ENDSHIELD);
		}
	}
	PutMissile(missile);
}

void MI_Firemove(int i)
{
	auto &missile = Missiles[i];
	constexpr int ExpLight[14] = { 2, 3, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12, 12 };

	missile.position.tile.x--;
	missile.position.tile.y--;
	missile.position.offset.deltaY += 32;
	missile._miVar1++;
	if (missile._miVar1 == missile._miAnimLen) {
		SetMissDir(missile, 1);
		missile._miAnimFrame = GenerateRnd(11) + 1;
	}
	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);
	int j = missile._mirange;
	CheckMissileCol(missile, missile._midam, missile._midam, false, missile.position.tile, false);
	if (missile._miHitFlag)
		missile._mirange = j;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	if (missile._mimfnum != 0 || missile._mirange == 0) {
		if (missile.position.tile.x != missile._miVar3 || missile.position.tile.y != missile._miVar4) {
			missile._miVar3 = missile.position.tile.x;
			missile._miVar4 = missile.position.tile.y;
			ChangeLight(missile._mlid, { missile._miVar3, missile._miVar4 }, 8);
		}
	} else {
		if (missile._miVar2 == 0)
			missile._mlid = AddLight(missile.position.tile, ExpLight[0]);
		ChangeLight(missile._mlid, missile.position.tile, ExpLight[missile._miVar2]);
		missile._miVar2++;
	}
	missile.position.tile.x++;
	missile.position.tile.y++;
	missile.position.offset.deltaY -= 32;
	PutMissile(missile);
}

void MI_Guardian(int i)
{
	assert(i >= 0 && i < MAXMISSILES);
	auto &missile = Missiles[i];

	missile._mirange--;

	if (missile._miVar2 > 0) {
		missile._miVar2--;
	}
	if (missile._mirange == missile._miVar1 || (missile._mimfnum == MFILE_GUARD && missile._miVar2 == 0)) {
		SetMissDir(missile, 1);
	}

	Point position = missile.position.tile;

	if ((missile._mirange % 16) == 0) {
		Displacement previous = { 0, 0 };

		bool found = false;
		for (int j = 0; j < 23 && !found; j++) {
			for (int k = 10; k >= 0 && !found; k -= 2) {
				const Displacement offset { VisionCrawlTable[j][k], VisionCrawlTable[j][k + 1] };
				if (offset == Displacement { 0, 0 }) {
					break;
				}
				if (previous == offset) {
					continue;
				}
				found = GuardianTryFireAt(missile, { position.x + offset.deltaX, position.y + offset.deltaY })
				    || GuardianTryFireAt(missile, { position.x - offset.deltaX, position.y - offset.deltaY })
				    || GuardianTryFireAt(missile, { position.x + offset.deltaX, position.y - offset.deltaY })
				    || GuardianTryFireAt(missile, { position.x - offset.deltaX, position.y + offset.deltaY });
				if (!found) {
					previous = offset;
				}
			}
		}
	}

	if (missile._mirange == 14) {
		SetMissDir(missile, 0);
		missile._miAnimFrame = 15;
		missile._miAnimAdd = -1;
	}

	missile._miVar3 += missile._miAnimAdd;

	if (missile._miVar3 > 15) {
		missile._miVar3 = 15;
	} else if (missile._miVar3 > 0) {
		ChangeLight(missile._mlid, position, missile._miVar3);
	}

	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}

	PutMissile(missile);
}

void MI_Chain(int mi)
{
	auto &missile = Missiles[mi];
	int id = missile._misource;
	Point position = missile.position.tile;
	Direction dir = GetDirection(position, { missile._miVar1, missile._miVar2 });
	AddMissile(position, { missile._miVar1, missile._miVar2 }, dir, MIS_LIGHTCTRL, TARGET_MONSTERS, id, 1, missile._mispllvl);
	int rad = missile._mispllvl + 3;
	if (rad > 19)
		rad = 19;
	for (int i = 1; i < rad; i++) {
		int k = CrawlNum[i];
		int ck = k + 2;
		for (auto j = static_cast<uint8_t>(CrawlTable[k]); j > 0; j--, ck += 2) {
			Point target = position + Displacement { CrawlTable[ck - 1], CrawlTable[ck] };
			if (InDungeonBounds(target) && dMonster[target.x][target.y] > 0) {
				dir = GetDirection(position, target);
				AddMissile(position, target, dir, MIS_LIGHTCTRL, TARGET_MONSTERS, id, 1, missile._mispllvl);
			}
		}
	}
	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
}

void MI_Weapexp(int i)
{
	auto &missile = Missiles[i];
	constexpr int ExpLight[10] = { 9, 10, 11, 12, 11, 10, 8, 6, 4, 2 };

	missile._mirange--;
	int id = missile._misource;
	int mind;
	int maxd;
	if (missile._miVar2 == 1) {
		mind = Players[id]._pIFMinDam;
		maxd = Players[id]._pIFMaxDam;
		MissileData[missile._mitype].mResist = MISR_FIRE;
	} else {
		mind = Players[id]._pILMinDam;
		maxd = Players[id]._pILMaxDam;
		MissileData[missile._mitype].mResist = MISR_LIGHTNING;
	}
	CheckMissileCol(missile, mind, maxd, false, missile.position.tile, false);
	if (missile._miVar1 == 0) {
		missile._mlid = AddLight(missile.position.tile, 9);
	} else {
		if (missile._mirange != 0)
			ChangeLight(missile._mlid, missile.position.tile, ExpLight[missile._miVar1]);
	}
	missile._miVar1++;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	} else {
		PutMissile(missile);
	}
}

void MI_Misexp(int i)
{
	auto &missile = Missiles[i];
	constexpr int ExpLight[] = { 9, 10, 11, 12, 11, 10, 8, 6, 4, 2, 1, 0, 0, 0, 0 };

	missile._mirange--;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	} else {
		if (missile._miVar1 == 0)
			missile._mlid = AddLight(missile.position.tile, 9);
		else
			ChangeLight(missile._mlid, missile.position.tile, ExpLight[missile._miVar1]);
		missile._miVar1++;
		PutMissile(missile);
	}
}

void MI_Acidsplat(int i)
{
	auto &missile = Missiles[i];
	if (missile._mirange == missile._miAnimLen) {
		missile.position.tile.x++;
		missile.position.tile.y++;
		missile.position.offset.deltaY -= 32;
	}
	missile._mirange--;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		int monst = missile._misource;
		int dam = (Monsters[monst].MData->mLevel >= 2 ? 2 : 1);
		AddMissile(missile.position.tile, { i, 0 }, missile._mimfnum, MIS_ACIDPUD, TARGET_PLAYERS, monst, dam, missile._mispllvl);
	} else {
		PutMissile(missile);
	}
}

void MI_Teleport(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange <= 0) {
		missile._miDelFlag = true;
		return;
	}

	int id = missile._misource;
	auto &player = Players[id];

	dPlayer[player.position.tile.x][player.position.tile.y] = 0;
	PlrClrTrans(player.position.tile);
	player.position.tile = { missile.position.tile.x, missile.position.tile.y };
	player.position.future = player.position.tile;
	player.position.old = player.position.tile;
	PlrDoTrans(player.position.tile);
	missile._miVar1 = 1;
	dPlayer[player.position.tile.x][player.position.tile.y] = id + 1;
	if (leveltype != DTYPE_TOWN) {
		ChangeLightXY(player._plid, player.position.tile);
		ChangeVisionXY(player._pvid, player.position.tile);
	}
	if (id == MyPlayerId) {
		ViewX = player.position.tile.x - ScrollInfo.tile.x;
		ViewY = player.position.tile.y - ScrollInfo.tile.y;
	}
}

void MI_Stone(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	auto &monster = Monsters[missile._miVar2];
	if (monster._mhitpoints == 0 && missile._miAnimType != MFILE_SHATTER1) {
		missile._mimfnum = 0;
		missile._miDrawFlag = true;
		SetMissAnim(missile, MFILE_SHATTER1);
		missile._mirange = 11;
	}
	if (monster._mmode != MM_STONE) {
		missile._miDelFlag = true;
		return;
	}

	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		if (monster._mhitpoints > 0) {
			monster._mmode = (MON_MODE)missile._miVar1;
			monster.AnimInfo.IsPetrified = false;
		} else {
			AddDead(monster.position.tile, stonendx, monster._mdir);
		}
	}
	if (missile._miAnimType == MFILE_SHATTER1)
		PutMissile(missile);
}

void MI_Boom(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._miVar1 == 0)
		CheckMissileCol(missile, missile._midam, missile._midam, false, missile.position.tile, true);
	if (missile._miHitFlag)
		missile._miVar1 = 1;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
	PutMissile(missile);
}

void MI_Rhino(int i)
{
	auto &missile = Missiles[i];
	int monst = missile._misource;
	auto &monster = Monsters[monst];
	if (monster._mmode != MM_CHARGE) {
		missile._miDelFlag = true;
		return;
	}
	UpdateMissilePos(missile);
	Point prevPos = missile.position.tile;
	Point newPosSnake;
	dMonster[prevPos.x][prevPos.y] = 0;
	if (monster._mAi == AI_SNAKE) {
		missile.position.traveled += missile.position.velocity * 2;
		UpdateMissilePos(missile);
		newPosSnake = missile.position.tile;
		missile.position.traveled -= missile.position.velocity;
	} else {
		missile.position.traveled += missile.position.velocity;
	}
	UpdateMissilePos(missile);
	Point newPos = missile.position.tile;
	if (!IsTileAvailable(monster, newPos) || (monster._mAi == AI_SNAKE && !IsTileAvailable(monster, newPosSnake))) {
		MissToMonst(i, prevPos);
		missile._miDelFlag = true;
		return;
	}
	monster.position.future = newPos;
	monster.position.old = newPos;
	monster.position.tile = newPos;
	dMonster[newPos.x][newPos.y] = -(monst + 1);
	if (monster._uniqtype != 0)
		ChangeLightXY(missile._mlid, newPos);
	MoveMissilePos(missile);
	PutMissile(missile);
}

void MI_FirewallC(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		return;
	}

	int id = missile._misource;

	{
		Point position = { missile._miVar1, missile._miVar2 };
		Point target = position + static_cast<Direction>(missile._miVar3);

		if (!missile.limitReached && GrowWall(id, position, target, MIS_FIREWALL, missile._mispllvl, 0)) {
			missile._miVar1 = target.x;
			missile._miVar2 = target.y;
		} else {
			missile.limitReached = true;
		}
	}

	{
		Point position = { missile._miVar5, missile._miVar6 };
		Point target = position + static_cast<Direction>(missile._miVar4);

		if (missile._miVar7 == 0 && GrowWall(id, position, target, MIS_FIREWALL, missile._mispllvl, 0)) {
			missile._miVar5 = target.x;
			missile._miVar6 = target.y;
		} else {
			missile._miVar7 = 1;
		}
	}
}

void MI_Infra(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	Players[missile._misource]._pInfraFlag = true;
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		CalcPlrItemVals(missile._misource, true);
	}
}

void MI_Apoca(int i)
{
	auto &missile = Missiles[i];
	int id = missile._misource;
	bool exit = false;
	int j;
	int k;
	for (j = missile._miVar2; j < missile._miVar3 && !exit; j++) {
		for (k = missile._miVar4; k < missile._miVar5 && !exit; k++) {
			if (dMonster[k][j] < MAX_PLRS)
				continue;
			if (nSolidTable[dPiece[k][j]])
				continue;
			if (gbIsHellfire && !LineClearMissile(missile.position.tile, { k, j }))
				continue;
			AddMissile({ k, j }, { k, j }, Players[id]._pdir, MIS_BOOM, TARGET_MONSTERS, id, missile._midam, 0);
			exit = true;
		}
		if (!exit) {
			missile._miVar4 = missile._miVar6;
		}
	}

	if (exit) {
		missile._miVar2 = j - 1;
		missile._miVar4 = k;
	} else {
		missile._miDelFlag = true;
	}
}

void MI_Wave(int i)
{
	bool f1 = false;
	bool f2 = false;
	assert(i >= 0 && i < MAXMISSILES);

	auto &missile = Missiles[i];
	int id = missile._misource;
	Point src = missile.position.tile;
	Direction sd = GetDirection(src, { missile._miVar1, missile._miVar2 });
	Direction dira = left[left[sd]];
	Direction dirb = right[right[sd]];
	Point na = src + sd;
	int pn = dPiece[na.x][na.y];
	assert(pn >= 0 && pn <= MAXTILES);
	if (!nMissileTable[pn]) {
		Direction pdir = Players[id]._pdir;
		AddMissile(na, na + sd, pdir, MIS_FIREMOVE, TARGET_MONSTERS, id, 0, missile._mispllvl);
		na += dira;
		Point nb = src + sd + dirb;
		for (int j = 0; j < (missile._mispllvl / 2) + 2; j++) {
			pn = dPiece[na.x][na.y]; // BUGFIX: dPiece is accessed before check against dungeon size and 0
			assert(pn >= 0 && pn <= MAXTILES);
			if (nMissileTable[pn] || f1 || !InDungeonBounds(na)) {
				f1 = true;
			} else {
				AddMissile(na, na + sd, pdir, MIS_FIREMOVE, TARGET_MONSTERS, id, 0, missile._mispllvl);
				na += dira;
			}
			pn = dPiece[nb.x][nb.y]; // BUGFIX: dPiece is accessed before check against dungeon size and 0
			assert(pn >= 0 && pn <= MAXTILES);
			if (nMissileTable[pn] || f2 || !InDungeonBounds(nb)) {
				f2 = true;
			} else {
				AddMissile(nb, nb + sd, pdir, MIS_FIREMOVE, TARGET_MONSTERS, id, 0, missile._mispllvl);
				nb += dirb;
			}
		}
	}

	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
}

void MI_Nova(int i)
{
	auto &missile = Missiles[i];
	int sx1 = 0;
	int sy1 = 0;
	int id = missile._misource;
	int dam = missile._midam;
	Point src = missile.position.tile;
	Direction dir = DIR_S;
	mienemy_type en = TARGET_PLAYERS;
	if (id != -1) {
		dir = Players[id]._pdir;
		en = TARGET_MONSTERS;
	}
	for (const auto &k : VisionCrawlTable) {
		if (sx1 != k[6] || sy1 != k[7]) {
			AddMissile(src, src + Displacement { k[6], k[7] }, dir, MIS_LIGHTBALL, en, id, dam, missile._mispllvl);
			AddMissile(src, src + Displacement { -k[6], -k[7] }, dir, MIS_LIGHTBALL, en, id, dam, missile._mispllvl);
			AddMissile(src, src + Displacement { -k[6], k[7] }, dir, MIS_LIGHTBALL, en, id, dam, missile._mispllvl);
			AddMissile(src, src + Displacement { k[6], -k[7] }, dir, MIS_LIGHTBALL, en, id, dam, missile._mispllvl);
			sx1 = k[6];
			sy1 = k[7];
		}
	}
	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
}

void MI_Blodboil(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;

	if (missile._mirange != 0) {
		return;
	}

	int id = missile._misource;
	auto &player = Players[id];

	int hpdif = player._pMaxHP - player._pHitPoints;

	if ((player._pSpellFlags & 2) != 0) {
		player._pSpellFlags &= ~0x2;
		player._pSpellFlags |= 4;
		int lvl = player._pLevel * 2;
		missile._mirange = lvl + 10 * missile._mispllvl + 245;
	} else {
		player._pSpellFlags &= ~0x4;
		missile._miDelFlag = true;
		hpdif += missile._miVar2;
	}

	CalcPlrItemVals(id, true);
	ApplyPlrDamage(id, 0, 1, hpdif);
	force_redraw = 255;
	player.Say(HeroSpeech::HeavyBreathing);
}

void MI_Flame(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	missile._miVar2--;
	int k = missile._mirange;
	CheckMissileCol(missile, missile._midam, missile._midam, true, missile.position.tile, false);
	if (missile._mirange == 0 && missile._miHitFlag)
		missile._mirange = k;
	if (missile._miVar2 == 0)
		missile._miAnimFrame = 20;
	if (missile._miVar2 <= 0) {
		k = missile._miAnimFrame;
		if (k > 11)
			k = 24 - k;
		ChangeLight(missile._mlid, missile.position.tile, k);
	}
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	if (missile._miVar2 <= 0)
		PutMissile(missile);
}

void MI_Flamec(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int src = missile._misource;
	missile.position.traveled += missile.position.velocity;
	UpdateMissilePos(missile);
	if (missile.position.tile.x != missile._miVar1 || missile.position.tile.y != missile._miVar2) {
		int id = dPiece[missile.position.tile.x][missile.position.tile.y];
		if (!nMissileTable[id]) {
			AddMissile(
			    missile.position.tile,
			    missile.position.start,
			    i,
			    MIS_FLAME,
			    missile._micaster,
			    src,
			    missile._miVar3,
			    missile._mispllvl);
		} else {
			missile._mirange = 0;
		}
		missile._miVar1 = missile.position.tile.x;
		missile._miVar2 = missile.position.tile.y;
		missile._miVar3++;
	}
	if (missile._mirange == 0 || missile._miVar3 == 3)
		missile._miDelFlag = true;
}

void MI_Cbolt(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._miAnimType != MFILE_LGHNING) {
		if (missile._miVar3 == 0) {
			constexpr int BPath[16] = { -1, 0, 1, -1, 0, 1, -1, -1, 0, 0, 1, 1, 0, 1, -1, 0 };

			auto md = static_cast<Direction>(missile._miVar2);
			switch (BPath[missile._mirnd]) {
			case -1:
				md = left[md];
				break;
			case 1:
				md = right[md];
				break;
			}

			missile._mirnd = (missile._mirnd + 1) & 0xF;
			UpdateMissileVelocity(missile, missile.position.tile + md, 8);
			missile._miVar3 = 16;
		} else {
			missile._miVar3--;
		}
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
		CheckMissileCol(missile, missile._midam, missile._midam, false, missile.position.tile, false);
		if (missile._miHitFlag) {
			missile._miVar1 = 8;
			missile._mimfnum = 0;
			missile.position.offset = { 0, 0 };
			missile.position.velocity = {};
			SetMissAnim(missile, MFILE_LGHNING);
			missile._mirange = missile._miAnimLen;
			UpdateMissilePos(missile);
		}
		ChangeLight(missile._mlid, missile.position.tile, missile._miVar1);
	}
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

void MI_Hbolt(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._miAnimType != MFILE_HOLYEXPL) {
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
		int dam = missile._midam;
		if (missile.position.tile != missile.position.start) {
			CheckMissileCol(missile, dam, dam, false, missile.position.tile, false);
		}
		if (missile._mirange == 0) {
			missile.position.traveled -= missile.position.velocity;
			UpdateMissilePos(missile);
			missile._mimfnum = 0;
			SetMissAnim(missile, MFILE_HOLYEXPL);
			missile._mirange = missile._miAnimLen - 1;
			missile.position.StopMissile();
		} else {
			if (missile.position.tile != Point { missile._miVar1, missile._miVar2 }) {
				missile._miVar1 = missile.position.tile.x;
				missile._miVar2 = missile.position.tile.y;
				ChangeLight(missile._mlid, { missile._miVar1, missile._miVar2 }, 8);
			}
		}
	} else {
		ChangeLight(missile._mlid, missile.position.tile, missile._miAnimFrame + 7);
		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			AddUnLight(missile._mlid);
		}
	}
	PutMissile(missile);
}

void MI_Element(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int dam = missile._midam;
	int id = missile._misource;
	if (missile._miAnimType == MFILE_BIGEXP) {
		Point c = missile.position.tile;
		Point p = Players[id].position.tile;
		ChangeLight(missile._mlid, missile.position.tile, missile._miAnimFrame);
		if (!CheckBlock(p, c))
			CheckMissileCol(missile, dam, dam, true, c, true);

		constexpr Displacement Offsets[] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { 1, -1 }, { 1, 1 }, { -1, 0 }, { -1, 1 }, { -1, -1 } };
		for (Displacement offset : Offsets) {
			if (!CheckBlock(p, c + offset))
				CheckMissileCol(missile, dam, dam, true, c + offset, true);
		}

		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			AddUnLight(missile._mlid);
		}
	} else {
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
		Point c = missile.position.tile;
		CheckMissileCol(missile, dam, dam, false, c, false);
		if (missile._miVar3 == 0 && c == Point { missile._miVar4, missile._miVar5 })
			missile._miVar3 = 1;
		if (missile._miVar3 == 1) {
			missile._miVar3 = 2;
			missile._mirange = 255;
			auto *monster = FindClosest(c, 19);
			if (monster != nullptr) {
				Direction sd = GetDirection(c, monster->position.tile);
				SetMissDir(missile, sd);
				UpdateMissileVelocity(missile, monster->position.tile, 16);
			} else {
				Direction sd = Players[id]._pdir;
				SetMissDir(missile, sd);
				UpdateMissileVelocity(missile, c + sd, 16);
			}
		}
		if (c != Point { missile._miVar1, missile._miVar2 }) {
			missile._miVar1 = c.x;
			missile._miVar2 = c.y;
			ChangeLight(missile._mlid, c, 8);
		}
		if (missile._mirange == 0) {
			missile._mimfnum = 0;
			SetMissAnim(missile, MFILE_BIGEXP);
			missile._mirange = missile._miAnimLen - 1;
			missile.position.StopMissile();
		}
	}
	PutMissile(missile);
}

void MI_Bonespirit(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	int dam = missile._midam;
	int id = missile._misource;
	if (missile._mimfnum == 8) {
		ChangeLight(missile._mlid, missile.position.tile, missile._miAnimFrame);
		if (missile._mirange == 0) {
			missile._miDelFlag = true;
			AddUnLight(missile._mlid);
		}
		PutMissile(missile);
	} else {
		missile.position.traveled += missile.position.velocity;
		UpdateMissilePos(missile);
		Point c = missile.position.tile;
		CheckMissileCol(missile, dam, dam, false, c, false);
		if (missile._miVar3 == 0 && c == Point { missile._miVar4, missile._miVar5 })
			missile._miVar3 = 1;
		if (missile._miVar3 == 1) {
			missile._miVar3 = 2;
			missile._mirange = 255;
			auto *monster = FindClosest(c, 19);
			if (monster != nullptr) {
				missile._midam = monster->_mhitpoints >> 7;
				SetMissDir(missile, GetDirection(c, monster->position.tile));
				UpdateMissileVelocity(missile, monster->position.tile, 16);
			} else {
				Direction sd = Players[id]._pdir;
				SetMissDir(missile, sd);
				UpdateMissileVelocity(missile, c + sd, 16);
			}
		}
		if (c != Point { missile._miVar1, missile._miVar2 }) {
			missile._miVar1 = c.x;
			missile._miVar2 = c.y;
			ChangeLight(missile._mlid, c, 8);
		}
		if (missile._mirange == 0) {
			SetMissDir(missile, 8);
			missile.position.velocity = {};
			missile._mirange = 7;
		}
		PutMissile(missile);
	}
}

void MI_ResurrectBeam(int i)
{
	auto &missile = Missiles[i];
	missile._mirange--;
	if (missile._mirange == 0)
		missile._miDelFlag = true;
	PutMissile(missile);
}

void MI_Rportal(int i)
{
	auto &missile = Missiles[i];
	int expLight[17] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15 };

	if (missile._mirange > 1)
		missile._mirange--;
	if (missile._mirange == missile._miVar1)
		SetMissDir(missile, 1);

	if (currlevel != 0 && missile._mimfnum != 1 && missile._mirange != 0) {
		if (missile._miVar2 == 0)
			missile._mlid = AddLight(missile.position.tile, 1);
		ChangeLight(missile._mlid, missile.position.tile, expLight[missile._miVar2]);
		missile._miVar2++;
	}
	if (missile._mirange == 0) {
		missile._miDelFlag = true;
		AddUnLight(missile._mlid);
	}
	PutMissile(missile);
}

static void DeleteMissiles()
{
	for (int i = 0; i < ActiveMissileCount;) {
		if (Missiles[ActiveMissiles[i]]._miDelFlag) {
			DeleteMissile(ActiveMissiles[i], i);
		} else {
			i++;
		}
	}
}

void ProcessMissiles()
{
	for (int i = 0; i < ActiveMissileCount; i++) {
		auto &missile = Missiles[ActiveMissiles[i]];
		const auto &position = missile.position.tile;
		dFlags[position.x][position.y] &= ~BFLAG_MISSILE;
		if (!InDungeonBounds(position))
			missile._miDelFlag = true;
	}

	DeleteMissiles();

	MissilePreFlag = false;

	for (int i = 0; i < ActiveMissileCount; i++) {
		auto &missile = Missiles[ActiveMissiles[i]];
		if (MissileData[missile._mitype].mProc != nullptr)
			MissileData[missile._mitype].mProc(ActiveMissiles[i]);
		if (missile._miAnimFlags == MissileDataFlags::NotAnimated)
			continue;

		missile._miAnimCnt++;
		if (missile._miAnimCnt < missile._miAnimDelay)
			continue;

		missile._miAnimCnt = 0;
		missile._miAnimFrame += missile._miAnimAdd;
		if (missile._miAnimFrame > missile._miAnimLen)
			missile._miAnimFrame = 1;
		else if (missile._miAnimFrame < 1)
			missile._miAnimFrame = missile._miAnimLen;
	}

	DeleteMissiles();
}

void missiles_process_charge()
{
	for (int i = 0; i < ActiveMissileCount; i++) {
		int mi = ActiveMissiles[i];
		auto &missile = Missiles[mi];

		missile._miAnimData = MissileSpriteData[missile._miAnimType].animData[missile._mimfnum].get();
		if (missile._mitype != MIS_RHINO)
			continue;

		CMonster *mon = Monsters[missile._misource].MType;

		MonsterGraphic graphic;
		if (mon->mtype >= MT_HORNED && mon->mtype <= MT_OBLORD) {
			graphic = MonsterGraphic::Special;
		} else if (mon->mtype >= MT_NSNAKE && mon->mtype <= MT_GSNAKE) {
			graphic = MonsterGraphic::Attack;
		} else {
			graphic = MonsterGraphic::Walk;
		}
		missile._miAnimData = mon->GetAnimData(graphic).CelSpritesForDirections[missile._mimfnum]->Data();
	}
}

void RedoMissileFlags()
{
	for (int i = 0; i < ActiveMissileCount; i++) {
		auto &missile = Missiles[ActiveMissiles[i]];
		PutMissile(missile);
	}
}

void ClearMissileSpot(const MissileStruct &missile)
{
	const Point tile = missile.position.tile;
	dFlags[tile.x][tile.y] &= ~BFLAG_MISSILE;
}

} // namespace devilution
