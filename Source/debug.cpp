/**
 * @file debug.cpp
 *
 * Implementation of debug functions.
 */

#ifdef _DEBUG

#include "debug.h"

#include "automap.h"
#include "control.h"
#include "cursor.h"
#include "engine/load_cel.hpp"
#include "engine/point.hpp"
#include "inv.h"
#include "lighting.h"
#include "setmaps.h"
#include "spells.h"
#include "towners.h"
#include "utils/language.h"
#include "quests.h"

namespace devilution {

std::optional<CelSprite> pSquareCel;
bool DebugGodMode = false;
bool DebugVision = false;

namespace {

int DebugPlayerId;
int DebugQuestId;
int DebugMonsterId;

void SetSpellLevelCheat(spell_id spl, int spllvl)
{
	auto &myPlayer = Players[MyPlayerId];

	myPlayer._pMemSpells |= GetSpellBitmask(spl);
	myPlayer._pSplLvl[spl] = spllvl;
}

void PrintDebugMonster(int m)
{
	char dstr[128];

	auto &monster = Monsters[m];

	sprintf(dstr, "Monster %i = %s", m, _(monster.mName));
	NetSendCmdString(1 << MyPlayerId, dstr);
	sprintf(dstr, "X = %i, Y = %i", monster.position.tile.x, monster.position.tile.y);
	NetSendCmdString(1 << MyPlayerId, dstr);
	sprintf(dstr, "Enemy = %i, HP = %i", monster._menemy, monster._mhitpoints);
	NetSendCmdString(1 << MyPlayerId, dstr);
	sprintf(dstr, "Mode = %i, Var1 = %i", monster._mmode, monster._mVar1);
	NetSendCmdString(1 << MyPlayerId, dstr);

	bool bActive = false;

	for (int i = 0; i < ActiveMonsterCount; i++) {
		if (ActiveMonsters[i] == m)
			bActive = true;
	}

	sprintf(dstr, "Active List = %i, Squelch = %i", bActive ? 1 : 0, monster._msquelch);
	NetSendCmdString(1 << MyPlayerId, dstr);
}

struct DebugCmdItem {
	const std::string_view text;
	const std::string_view description;
	const std::string_view requiredParameter;
	std::string (*actionProc)(const std::string_view);
};

extern std::vector<DebugCmdItem> DebugCmdList;

std::string DebugCmdHelp(const std::string_view parameter)
{
	if (parameter.empty()) {
		std::string ret = "Available Debug Commands: ";
		int lenCurrentLine = ret.length();
		bool first = true;
		for (const auto &dbgCmd : DebugCmdList) {
			if ((dbgCmd.text.length() + lenCurrentLine + 3) > MAX_SEND_STR_LEN) {
				ret.append("\n");
				lenCurrentLine = dbgCmd.text.length();
			} else {
				if (first)
					first = false;
				else
					ret.append(" - ");
				lenCurrentLine += (dbgCmd.text.length() + 3);
			}
			ret.append(dbgCmd.text);
		}
		return ret;
	} else {
		auto debugCmdIterator = std::find_if(DebugCmdList.begin(), DebugCmdList.end(), [&](const DebugCmdItem &elem) { return elem.text == parameter; });
		if (debugCmdIterator == DebugCmdList.end())
			return fmt::format("Debug command {} wasn't found", parameter);
		auto &dbgCmdItem = *debugCmdIterator;
		if (dbgCmdItem.requiredParameter.empty())
			return fmt::format("Description: {}\nParameters: No additional parameter needed.", dbgCmdItem.description);
		return fmt::format("Description: {}\nParameters: {}", dbgCmdItem.description, dbgCmdItem.requiredParameter);
	}
}

std::string DebugCmdGiveGoldCheat(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];

	for (int8_t &itemId : myPlayer.InvGrid) {
		if (itemId != 0)
			continue;

		int ni = myPlayer._pNumInv++;
		SetPlrHandItem(&myPlayer.InvList[ni], IDI_GOLD);
		GetPlrHandSeed(&myPlayer.InvList[ni]);
		myPlayer.InvList[ni]._ivalue = GOLD_MAX_LIMIT;
		myPlayer.InvList[ni]._iCurs = ICURS_GOLD_LARGE;
		myPlayer._pGold += GOLD_MAX_LIMIT;
		itemId = myPlayer._pNumInv;
	}
	CalcPlrInv(MyPlayerId, true);

	return "You are now rich! If only this was as easy in real life...";
}

std::string DebugCmdTakeGoldCheat(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];

	for (auto itemId : myPlayer.InvGrid) {
		itemId -= 1;

		if (itemId < 0)
			continue;
		if (myPlayer.InvList[itemId]._itype != ITYPE_GOLD)
			continue;

		myPlayer.RemoveInvItem(itemId);
	}

	myPlayer._pGold = 0;

	return "You are poor...";
}

std::string DebugCmdWarpToLevel(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];
	auto level = atoi(parameter.data());
	if (level < 0 || level > (gbIsHellfire ? 24 : 16))
		return fmt::format("Level {} is not known. Do you want to write a mod?", level);
	if (!setlevel && myPlayer.plrlevel == level)
		return fmt::format("I did nothing but fulfilled your wish. You are already at level {}.", level);

	setlevel = false;
	StartNewLvl(MyPlayerId, (level != 21) ? interface_mode::WM_DIABNEXTLVL : interface_mode::WM_DIABTWARPUP, level);
	return fmt::format("Welcome to level {}.", level);
}

std::string DebugCmdLoadMap(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];
	auto level = atoi(parameter.data());
	if (level < 1)
		return fmt::format("Map id must be 1 or higher", level);
	if (setlevel && myPlayer.plrlevel == level)
		return fmt::format("I did nothing but fulfilled your wish. You are already at level {}.", level);

	for (auto &quest : Quests) {
		if (level != quest._qslvl)
			continue;

		setlevel = false;
		setlvltype = quest._qlvltype;
		StartNewLvl(MyPlayerId, WM_DIABSETLVL, level);
		return fmt::format("Welcome to {}.", QuestLevelNames[level]);
	}

	return fmt::format("Level {} is not known. Do you want to write a mod?", level);
}

std::unordered_map<std::string_view, _talker_id> TownerShortNameToTownerId = {
	{ "griswold", _talker_id::TOWN_SMITH },
	{ "pepin", _talker_id::TOWN_HEALER },
	{ "ogden", _talker_id::TOWN_TAVERN },
	{ "cain", _talker_id::TOWN_STORY },
	{ "farnham", _talker_id::TOWN_DRUNK },
	{ "adria", _talker_id::TOWN_WITCH },
	{ "gillian", _talker_id::TOWN_BMAID },
	{ "wirt", _talker_id ::TOWN_PEGBOY },
	{ "lester", _talker_id ::TOWN_FARMER },
	{ "girl", _talker_id ::TOWN_GIRL },
	{ "nut", _talker_id::TOWN_COWFARM },
};

std::string DebugCmdVisitTowner(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];

	if (setlevel || myPlayer.plrlevel != 0)
		return "What kind of friends do you have in dungeons?";

	if (parameter.empty()) {
		std::string ret;
		ret = "Who? ";
		for (auto &entry : TownerShortNameToTownerId) {
			ret.append(" ");
			ret.append(entry.first);
		}
		return ret;
	}

	auto it = TownerShortNameToTownerId.find(parameter);
	if (it == TownerShortNameToTownerId.end())
		return fmt::format("{} is unknown. Perhaps he is a ninja?", parameter);

	for (auto &towner : Towners) {
		if (towner._ttype != it->second)
			continue;

		CastSpell(
		    MyPlayerId,
		    SPL_TELEPORT,
		    myPlayer.position.tile.x,
		    myPlayer.position.tile.y,
		    towner.position.x,
		    towner.position.y,
		    1);

		return fmt::format("Say hello to {} from me.", parameter);
	}

	return fmt::format("Couldn't find {}.", parameter);
}

std::string DebugCmdResetLevel(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];
	auto level = atoi(parameter.data());
	if (level < 0 || level > (gbIsHellfire ? 24 : 16))
		return fmt::format("Level {} is not known. Do you want to write an extension mod?", level);
	myPlayer._pLvlVisited[level] = false;
	if (myPlayer.plrlevel == level)
		return fmt::format("Level {} can't be cleaned, cause you still occupy it!", level);
	return fmt::format("Level {} was restored and looks fabulous.", level);
}

std::string DebugCmdGodMode(const std::string_view parameter)
{
	DebugGodMode = !DebugGodMode;
	if (DebugGodMode)
		return "A god descended.";
	return "You are mortal, beware of the darkness.";
}

std::string DebugCmdLighting(const std::string_view parameter)
{
	ToggleLighting();

	return "All raindrops are the same.";
}

std::string DebugCmdMap(const std::string_view parameter)
{
	std::fill(&AutomapView[0][0], &AutomapView[DMAXX - 1][DMAXX - 1], true);

	return "The way is made clear when viewed from above";
}

std::string DebugCmdVision(const std::string_view parameter)
{
	DebugVision = !DebugVision;
	if (DebugVision)
		return "You see as I do.";

	return "My path is set.";
}

std::string DebugCmdQuest(const std::string_view parameter)
{
	if (parameter.empty())
		return "You must provide an id";

	if (parameter.compare("all") == 0) {
		for (auto &quest : Quests) {
			if (IsNoneOf(quest._qactive, QUEST_NOTAVAIL, QUEST_INIT))
				continue;

			quest._qactive = QUEST_ACTIVE;
			quest._qlog = true;
		}

		return "Happy questing";
	}

	int questId = atoi(parameter.data());

	if (questId >= MAXQUESTS)
		return fmt::format("Quest {} is not known. Do you want to write a mod?", questId);
	auto &quest = Quests[questId];

	if (IsNoneOf(quest._qactive, QUEST_NOTAVAIL, QUEST_INIT))
		return fmt::format("{} was already given.", QuestData[questId]._qlstr);

	quest._qactive = QUEST_ACTIVE;
	quest._qlog = true;

	return fmt::format("{} enabled.", QuestData[questId]._qlstr);
}

std::string DebugCmdLevelUp(const std::string_view parameter)
{
	int levels = std::max(1, atoi(parameter.data()));
	for (int i = 0; i < levels; i++)
		NetSendCmd(true, CMD_CHEAT_EXPERIENCE);
	return "New experience leads to new insights.";
}

std::string DebugCmdSetSpellsLevel(const std::string_view parameter)
{
	int level = std::max(0, atoi(parameter.data()));
	for (int i = SPL_FIREBOLT; i < MAX_SPELLS; i++) {
		if (GetSpellBookLevel((spell_id)i) != -1) {
			SetSpellLevelCheat((spell_id)i, level);
		}
	}
	if (level == 0)
		Players[MyPlayerId]._pMemSpells = 0;

	return "Knowledge is power.";
}

std::string DebugCmdRefillHealthMana(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];
	myPlayer._pMana = myPlayer._pMaxMana;
	myPlayer._pManaBase = myPlayer._pMaxManaBase;
	myPlayer._pHitPoints = myPlayer._pMaxHP;
	myPlayer._pHPBase = myPlayer._pMaxHPBase;
	drawhpflag = true;
	drawmanaflag = true;

	return "Ready for more.";
}

std::string DebugCmdGenerateUniqueItem(const std::string_view parameter)
{
	return DebugSpawnItem(parameter.data(), true);
}

std::string DebugCmdGenerateItem(const std::string_view parameter)
{
	return DebugSpawnItem(parameter.data(), false);
}

std::string DebugCmdExit(const std::string_view parameter)
{
	gbRunGame = false;
	gbRunGameResult = false;
	return "See you again my Lord.";
}

std::string DebugCmdArrow(const std::string_view parameter)
{
	auto &myPlayer = Players[MyPlayerId];

	myPlayer._pIFlags &= ~ISPL_FIRE_ARROWS;
	myPlayer._pIFlags &= ~ISPL_LIGHT_ARROWS;

	if (parameter == "normal") {
		// we removed the parameter at the top
	} else if (parameter == "fire") {
		myPlayer._pIFlags |= ISPL_FIRE_ARROWS;
	} else if (parameter == "lightning") {
		myPlayer._pIFlags |= ISPL_LIGHT_ARROWS;
	} else if (parameter == "explosion") {
		myPlayer._pIFlags |= (ISPL_FIRE_ARROWS | ISPL_LIGHT_ARROWS);
	} else {
		return "Unknown is sometimes similar to nothing (unkown effect).";
	}

	return "I can shoot any arrow.";
}

std::string DebugCmdTalkToTowner(const std::string_view parameter)
{
	if (DebugTalkToTowner(parameter.data())) {
		return "Hello from the other side.";
	}
	return "NPC not found.";
}

std::vector<DebugCmdItem> DebugCmdList = {
	{ "help", "Prints help overview or help for a specific command.", "({command})", &DebugCmdHelp },
	{ "give gold", "Fills the inventory with gold.", "", &DebugCmdGiveGoldCheat },
	{ "give xp", "Levels the player up (min 1 level or {levels}).", "({levels})", &DebugCmdLevelUp },
	{ "set spells", "Set spell level to {level} for all spells.", "{level}", &DebugCmdSetSpellsLevel },
	{ "take gold", "Removes all gold from inventory.", "", &DebugCmdTakeGoldCheat },
	{ "give quest", "Enable a given quest.", "({id})", &DebugCmdQuest },
	{ "give map", "Reveal the map.", "", &DebugCmdMap },
	{ "changelevel", "Moves to specifided {level} (use 0 for town).", "{level}", &DebugCmdWarpToLevel },
	{ "map", "Load a quest level {level}.", "{level}", &DebugCmdLoadMap },
	{ "visit", "Visit a towner.", "{towner}", &DebugCmdVisitTowner },
	{ "restart", "Resets specified {level}.", "{level}", &DebugCmdResetLevel },
	{ "god", "Togggles godmode.", "", &DebugCmdGodMode },
	{ "r_drawvision", "Togggles vision debug rendering.", "", &DebugCmdVision },
	{ "r_fullbright", "Toggles whether light shading is in effect.", "", &DebugCmdLighting },
	{ "refill", "Refills health and mana.", "", &DebugCmdRefillHealthMana },
	{ "dropunique", "Attempts to generate unique item {name}.", "{name}", &DebugCmdGenerateUniqueItem },
	{ "dropitem", "Attempts to generate item {name}.", "{name}", &DebugCmdGenerateItem },
	{ "talkto", "Interacts with a NPC whose name contains {name}.", "{name}", &DebugCmdTalkToTowner },
	{ "exit", "Exits the game.", "", &DebugCmdExit },
	{ "arrow", "Changes arrow effect (normal, fire, lightning, explosion).", "{effect}", &DebugCmdArrow },
};

} // namespace

void LoadDebugGFX()
{
	pSquareCel = LoadCel("Data\\Square.CEL", 64);
}

void FreeDebugGFX()
{
	pSquareCel = std::nullopt;
}

void PrintDebugPlayer(bool bNextPlayer)
{
	char dstr[128];

	if (bNextPlayer)
		DebugPlayerId = ((BYTE)DebugPlayerId + 1) & 3;

	auto &player = Players[DebugPlayerId];

	sprintf(dstr, "Plr %i : Active = %i", DebugPlayerId, player.plractive ? 1 : 0);
	NetSendCmdString(1 << MyPlayerId, dstr);

	if (player.plractive) {
		sprintf(dstr, "  Plr %i is %s", DebugPlayerId, player._pName);
		NetSendCmdString(1 << MyPlayerId, dstr);
		sprintf(dstr, "  Lvl = %i : Change = %i", player.plrlevel, player._pLvlChanging ? 1 : 0);
		NetSendCmdString(1 << MyPlayerId, dstr);
		const Point target = player.GetTargetPosition();
		sprintf(dstr, "  x = %i, y = %i : tx = %i, ty = %i", player.position.tile.x, player.position.tile.y, target.x, target.y);
		NetSendCmdString(1 << MyPlayerId, dstr);
		sprintf(dstr, "  mode = %i : daction = %i : walk[0] = %i", player._pmode, player.destAction, player.walkpath[0]);
		NetSendCmdString(1 << MyPlayerId, dstr);
		sprintf(dstr, "  inv = %i : hp = %i", player._pInvincible ? 1 : 0, player._pHitPoints);
		NetSendCmdString(1 << MyPlayerId, dstr);
	}
}

void PrintDebugQuest()
{
	char dstr[128];

	auto &quest = Quests[DebugQuestId];
	sprintf(dstr, "Quest %i :  Active = %i, Var1 = %i", DebugQuestId, quest._qactive, quest._qvar1);
	NetSendCmdString(1 << MyPlayerId, dstr);

	DebugQuestId++;
	if (DebugQuestId == MAXQUESTS)
		DebugQuestId = 0;
}

void GetDebugMonster()
{
	int mi1 = pcursmonst;
	if (mi1 == -1) {
		int mi2 = dMonster[cursmx][cursmy];
		if (mi2 != 0) {
			mi1 = mi2 - 1;
			if (mi2 <= 0)
				mi1 = -(mi2 + 1);
		} else {
			mi1 = DebugMonsterId;
		}
	}
	PrintDebugMonster(mi1);
}

void NextDebugMonster()
{
	char dstr[128];

	DebugMonsterId++;
	if (DebugMonsterId == MAXMONSTERS)
		DebugMonsterId = 0;

	sprintf(dstr, "Current debug monster = %i", DebugMonsterId);
	NetSendCmdString(1 << MyPlayerId, dstr);
}

bool CheckDebugTextCommand(const std::string_view text)
{
	auto debugCmdIterator = std::find_if(DebugCmdList.begin(), DebugCmdList.end(), [&](const DebugCmdItem &elem) { return text.find(elem.text) == 0 && (text.length() == elem.text.length() || text[elem.text.length()] == ' '); });
	if (debugCmdIterator == DebugCmdList.end())
		return false;

	auto &dbgCmd = *debugCmdIterator;
	std::string_view parameter = "";
	if (text.length() > (dbgCmd.text.length() + 1))
		parameter = text.substr(dbgCmd.text.length() + 1);
	const auto result = dbgCmd.actionProc(parameter);

	const std::string delim = "\n";
	auto start = 0U;
	auto end = result.find(delim);
	while (end != std::string::npos) {
		const auto line = result.substr(start, end - start);
		NetSendCmdString(1 << MyPlayerId, line.c_str());
		start = end + delim.length();
		end = result.find(delim, start);
	}
	if (start != result.length())
		NetSendCmdString(1 << MyPlayerId, result.substr(start).c_str());

	return true;
}

} // namespace devilution

#endif
