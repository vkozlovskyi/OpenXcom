/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "AIBridge.h"
#include "BattlescapeGame.h"
#include "../Engine/Logger.h"
#include "../Engine/Language.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/Tile.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/MapData.h"
#include "../Mod/Unit.h"
#include "../Mod/MapDataSet.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <map>
#include <algorithm>
#include <set>

namespace OpenXcom
{

/**
 * Creates the AIBridge with a reference to the battle state.
 * @param save Pointer to the saved battle game.
 */
AIBridge::AIBridge(SavedBattleGame *save) : _listenFd(-1), _clientFd(-1), _port(0), _enabled(false), _lastTurnSent(-1), _currentTurn(0), _lang(0), _save(save), _hasPendingCommand(false), _actionExecuting(false), _executingUnitId(-1), _discoveredCountBefore(0), _mapDirty(false), _wasUsed(false)
{
}

/**
 * Cleans up sockets.
 */
AIBridge::~AIBridge()
{
	stop();
}

/**
 * Sets a file descriptor to non-blocking mode.
 * @param fd File descriptor.
 * @return True on success.
 */
bool AIBridge::setNonBlocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return false;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/**
 * Starts listening on the given TCP port (localhost only).
 * @param port TCP port number.
 * @return True on success.
 */
bool AIBridge::start(int port)
{
	_port = port;

	_listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_listenFd < 0)
	{
		Log(LOG_ERROR) << "AIBridge: socket() failed: " << strerror(errno);
		return false;
	}

	int opt = 1;
	setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	if (bind(_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		Log(LOG_ERROR) << "AIBridge: bind() failed on port " << port << ": " << strerror(errno);
		close(_listenFd);
		_listenFd = -1;
		return false;
	}

	if (listen(_listenFd, 1) < 0)
	{
		Log(LOG_ERROR) << "AIBridge: listen() failed: " << strerror(errno);
		close(_listenFd);
		_listenFd = -1;
		return false;
	}

	if (!setNonBlocking(_listenFd))
	{
		Log(LOG_ERROR) << "AIBridge: failed to set non-blocking mode";
		close(_listenFd);
		_listenFd = -1;
		return false;
	}

	_enabled = true;
	Log(LOG_INFO) << "AIBridge: listening on 127.0.0.1:" << port;
	return true;
}

/**
 * Stops the server and closes all connections.
 */
void AIBridge::stop()
{
	closeClient();
	if (_listenFd >= 0)
	{
		close(_listenFd);
		_listenFd = -1;
	}
	_enabled = false;
}

/**
 * Called every frame from BattlescapeGame::think().
 * Uses select() with zero timeout for non-blocking IO.
 */
void AIBridge::poll()
{
	if (!_enabled) return;

	fd_set readfds, writefds;
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);

	int maxfd = -1;

	// Always listen for new connections (allows replacing old client)
	FD_SET(_listenFd, &readfds);
	maxfd = _listenFd;

	if (_clientFd >= 0)
	{
		// Have client — check for incoming data
		FD_SET(_clientFd, &readfds);
		if (_clientFd > maxfd) maxfd = _clientFd;
		if (!_sendBuf.empty())
		{
			FD_SET(_clientFd, &writefds);
		}
	}

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	int ret = select(maxfd + 1, &readfds, &writefds, NULL, &tv);
	if (ret <= 0) return;

	if (FD_ISSET(_listenFd, &readfds))
	{
		tryAccept();
	}
	if (_clientFd >= 0)
	{
		if (FD_ISSET(_clientFd, &readfds))
			tryRead();
		if (_clientFd >= 0 && FD_ISSET(_clientFd, &writefds))
			tryWrite();
	}
}

/**
 * Tries to accept a pending client connection.
 * Only one client is supported at a time.
 */
void AIBridge::tryAccept()
{
	struct sockaddr_in clientAddr;
	socklen_t addrLen = sizeof(clientAddr);
	int fd = accept(_listenFd, (struct sockaddr*)&clientAddr, &addrLen);
	if (fd < 0) return;

	// Drop existing client if any (allows reconnection)
	if (_clientFd >= 0)
	{
		Log(LOG_INFO) << "AIBridge: dropping old client for new connection";
		closeClient();
	}

	setNonBlocking(fd);
	_clientFd = fd;
	_recvBuf.clear();
	_sendBuf.clear();
	_lastTurnSent = -1; // reset so turn_start is re-sent to the new client
	_hasPendingCommand = false;
	_actionExecuting = false;

	Log(LOG_INFO) << "AIBridge: client connected";

	nlohmann::json hello;
	hello["type"] = "connected";
	hello["version"] = "1.0";
	sendMessage(hello);
}

/**
 * Reads available data from the client socket (non-blocking).
 * Splits input by newlines and processes complete JSON-lines messages.
 */
void AIBridge::tryRead()
{
	char buf[4096];
	ssize_t n = recv(_clientFd, buf, sizeof(buf), 0);
	if (n <= 0)
	{
		if (n == 0)
		{
			Log(LOG_INFO) << "AIBridge: client disconnected";
			closeClient();
		}
		else if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			Log(LOG_WARNING) << "AIBridge: recv error: " << strerror(errno);
			closeClient();
		}
		return;
	}

	_recvBuf.append(buf, n);

	size_t pos;
	while ((pos = _recvBuf.find('\n')) != std::string::npos)
	{
		std::string line = _recvBuf.substr(0, pos);
		_recvBuf.erase(0, pos + 1);
		if (!line.empty())
		{
			processMessage(line);
		}
	}
}

/**
 * Writes pending data to the client socket (non-blocking).
 */
void AIBridge::tryWrite()
{
	if (_sendBuf.empty()) return;

	ssize_t n = send(_clientFd, _sendBuf.c_str(), _sendBuf.size(), 0);
	if (n < 0)
	{
		if (errno != EAGAIN && errno != EWOULDBLOCK)
		{
			Log(LOG_WARNING) << "AIBridge: send error: " << strerror(errno);
			closeClient();
		}
		return;
	}
	_sendBuf.erase(0, n);
}

/**
 * Processes a complete JSON-lines message received from the client.
 * Parses JSON commands into AICommand struct for BattlescapeGame to dispatch.
 * @param line A single line of JSON text.
 */
void AIBridge::processMessage(const std::string &line)
{
	Log(LOG_DEBUG) << "AIBridge recv: " << line;

	nlohmann::json msg;
	try
	{
		msg = nlohmann::json::parse(line);
	}
	catch (const nlohmann::json::parse_error &e)
	{
		Log(LOG_WARNING) << "AIBridge: JSON parse error: " << e.what();
		nlohmann::json errMsg;
		errMsg["type"] = "error";
		errMsg["message"] = "invalid JSON";
		sendMessage(errMsg);
		return;
	}

	// Validate required "action" field
	if (!msg.contains("action") || !msg["action"].is_string())
	{
		nlohmann::json errMsg;
		errMsg["type"] = "error";
		errMsg["message"] = "missing 'action' field";
		sendMessage(errMsg);
		return;
	}

	// Handle get_state synchronously (read-only, works even while action is executing)
	if (msg["action"].get<std::string>() == "get_state")
	{
		if (!_lang)
		{
			sendError("get_state", -1, "no_state_yet");
			return;
		}
		nlohmann::json state = serializeGameState(_currentTurn, _lang);
		state["type"] = "game_state";
		bool includeMap = msg.value("include_map", false);
		if (!includeMap)
		{
			state.erase("ascii_map");
			state.erase("map_legend");
		}
		state["events"] = flushEvents();
		sendMessage(state);
		return;
	}

	// Reject if we already have a pending command or action is executing
	if (_hasPendingCommand || _actionExecuting)
	{
		sendError(msg["action"].get<std::string>(),
			msg.value("unit_id", -1), "busy");
		return;
	}

	// Parse command
	AICommand cmd;
	cmd.action = msg["action"].get<std::string>();
	cmd.unitId = msg.value("unit_id", -1);
	cmd.hand = msg.value("hand", "right");
	cmd.shotType = msg.value("shot_type", "snap");
	cmd.value = msg.value("fuse", msg.value("direction", 0));
	cmd.exact = msg.value("exact", false);
	cmd.radius = msg.value("radius", 0);

	// Parse 'from' position [x, y, z] for get_fire_options
	if (msg.contains("from") && msg["from"].is_array() && msg["from"].size() == 3)
	{
		cmd.from = Position(
			msg["from"][0].get<int>(),
			msg["from"][1].get<int>(),
			msg["from"][2].get<int>()
		);
		cmd.hasFrom = true;
	}

	// Parse target position [x, y, z]
	if (msg.contains("target") && msg["target"].is_array() && msg["target"].size() == 3)
	{
		cmd.target = Position(
			msg["target"][0].get<int>(),
			msg["target"][1].get<int>(),
			msg["target"][2].get<int>()
		);
	}

	// Validate action type
	if (cmd.action != "select" && cmd.action != "walk" && cmd.action != "shoot" &&
		cmd.action != "kneel" && cmd.action != "throw" && cmd.action != "prime" &&
		cmd.action != "end_turn" && cmd.action != "get_reachable" &&
		cmd.action != "get_path_cost" &&
		cmd.action != "get_fire_options" &&
		cmd.action != "get_blast_check" &&
		cmd.action != "launch" &&
		cmd.action != "turn")
	{
		sendError(cmd.action, cmd.unitId, "unknown_action");
		return;
	}

	// Validate unit_id is present for actions that need it
	if (cmd.action != "end_turn" && cmd.action != "get_blast_check" && cmd.unitId < 0)
	{
		sendError(cmd.action, cmd.unitId, "missing_unit_id");
		return;
	}

	// Validate target is present for actions that need it
	if ((cmd.action == "walk" || cmd.action == "shoot" || cmd.action == "throw" ||
		cmd.action == "get_path_cost" || cmd.action == "get_blast_check" ||
		cmd.action == "launch") && !msg.contains("target"))
	{
		sendError(cmd.action, cmd.unitId, "missing_target");
		return;
	}

	_pendingCommand = cmd;
	_hasPendingCommand = true;
	_wasUsed = true;
	Log(LOG_DEBUG) << "AIBridge: queued command: " << cmd.action << " unit=" << cmd.unitId;
}

/**
 * Queues a JSON message for sending to the client.
 * @param msg JSON object to send.
 */
void AIBridge::sendMessage(const nlohmann::json &msg)
{
	if (_clientFd < 0) return;
	_sendBuf += msg.dump() + "\n";
}

/**
 * Closes the client connection but keeps listening for new ones.
 */
void AIBridge::closeClient()
{
	if (_clientFd >= 0)
	{
		close(_clientFd);
		_clientFd = -1;
	}
	_recvBuf.clear();
	_sendBuf.clear();
}

/**
 * Notifies that a battle has started.
 */
void AIBridge::notifyBattleStart()
{
	nlohmann::json msg;
	msg["type"] = "battle_start";
	sendMessage(msg);
}

/**
 * Notifies that the player's turn has started.
 * Sends the full game state so the AI client can make decisions.
 * Idempotent — will not send duplicate notifications for the same turn.
 * @param turn Turn number.
 * @param lang Language for translating unit names.
 */
void AIBridge::notifyTurnStart(int turn, Language *lang)
{
	if (_lastTurnSent == turn) return;
	_lastTurnSent = turn;
	_currentTurn = turn;
	_lang = lang;

	nlohmann::json msg = serializeGameState(turn, lang);
	// Don't flush events here — turn_start is a push notification
	// that may not reach anyone. Events stay in queue until an
	// explicit client command (get_state, walk, etc.) flushes them.
	sendMessage(msg);

	Log(LOG_INFO) << "AIBridge: sent turn_start with game state (turn " << turn << ")";
}

/**
 * Notifies that the battle has ended.
 */
void AIBridge::notifyBattleEnd()
{
	nlohmann::json msg;
	msg["type"] = "battle_end";
	sendMessage(msg);
}

/**
 * Returns true if a client is currently connected.
 */
bool AIBridge::isConnected() const
{
	return _clientFd >= 0;
}

/**
 * Returns true if any client command was ever received this battle.
 */
bool AIBridge::wasUsed() const
{
	return _wasUsed;
}

// --- Game State Serialization (Phase 2) ---

/**
 * Serializes the full battle state into a turn_start JSON message.
 * Includes player units, visible enemies, and discovered map tiles.
 * @param turn Current turn number.
 * @param lang Language for unit name translation.
 * @return JSON object ready to send.
 */
nlohmann::json AIBridge::serializeGameState(int turn, Language *lang) const
{
	nlohmann::json msg;
	msg["type"] = "turn_start";
	msg["turn"] = turn;

	// Map metadata
	nlohmann::json map;
	map["size_x"] = _save->getMapSizeX();
	map["size_y"] = _save->getMapSizeY();
	map["size_z"] = _save->getMapSizeZ();
	map["global_shade"] = _save->getGlobalShade();
	msg["map"] = map;

	// Player units and visible enemies
	nlohmann::json units = nlohmann::json::array();
	nlohmann::json visibleEnemies = nlohmann::json::array();
	std::set<int> seenEnemyIds;

	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		BattleUnit *unit = *i;
		if (unit->getFaction() != FACTION_PLAYER) continue;
		if (unit->isOut()) continue;

		units.push_back(serializeUnit(unit, lang));

		// Collect visible enemies (deduplicated)
		for (std::vector<BattleUnit*>::iterator e = unit->getVisibleUnits()->begin(); e != unit->getVisibleUnits()->end(); ++e)
		{
			BattleUnit *enemy = *e;
			if (seenEnemyIds.find(enemy->getId()) == seenEnemyIds.end())
			{
				seenEnemyIds.insert(enemy->getId());
				visibleEnemies.push_back(serializeVisibleEnemy(enemy, lang));
			}
		}
	}
	msg["units"] = units;
	msg["visible_enemies"] = visibleEnemies;

	// Build MapDataSet-to-character mapping for building type display
	int sizeX = _save->getMapSizeX();
	int sizeY = _save->getMapSizeY();
	int sizeZ = _save->getMapSizeZ();
	std::vector<MapDataSet*> *dataSets = _save->getMapDataSets();
	std::set<int> ufoSets, craftSets;
	std::map<int, char> dataSetChars; // mdsID -> floor display char
	nlohmann::json mapLegend;
	char nextChar = 'a';
	for (size_t i = 0; i < dataSets->size(); i++)
	{
		std::string name = dataSets->at(i)->getName();
		// X-COM 1 UFOs: UFO1, U_EXT02, U_WALL02, U_BITS, U_DISEC2, U_OPER2, U_PODS
		// TFTD UFOs: UEXT2, UEXT3, UINT1, UINT2, UINT3
		if (name.compare(0, 3, "UFO") == 0 || name.compare(0, 2, "U_") == 0
			|| name.compare(0, 4, "UEXT") == 0 || name.compare(0, 4, "UINT") == 0)
		{
			ufoSets.insert(i);
			dataSetChars[i] = 'u';
		}
		// X-COM 1 crafts: PLANE, LIGHTNIN, AVENGER
		// TFTD crafts: TRITON, HAMMER, LEVIATH
		else if (name == "PLANE" || name == "LIGHTNIN" || name == "AVENGER"
			|| name == "TRITON" || name == "HAMMER" || name == "LEVIATH")
		{
			craftSets.insert(i);
			dataSetChars[i] = 's';
		}
		else if (name != "BLANKS" && nextChar <= 'z')
		{
			dataSetChars[i] = nextChar;
			mapLegend[std::string(1, nextChar)] = name;
			nextChar++;
			// skip 's' and 'u' — reserved for craft/UFO
			if (nextChar == 's' || nextChar == 'u') nextChar++;
			if (nextChar == 's' || nextChar == 'u') nextChar++;
		}
	}
	mapLegend["u"] = "UFO";
	mapLegend["s"] = "craft";
	mapLegend["/"] = "stairs";
	mapLegend["^"] = "gravlift";

	// ASCII map per z-level (only levels with discovered tiles)
	nlohmann::json asciiMap;
	for (int z = 0; z < sizeZ; z++)
	{
		bool hasDiscovered = false;
		for (int y = 0; y < sizeY && !hasDiscovered; y++)
		{
			for (int x = 0; x < sizeX && !hasDiscovered; x++)
			{
				Tile *tile = _save->getTile(Position(x, y, z));
				if (tile && tile->isDiscovered(2) && !tile->isVoid())
					hasDiscovered = true;
			}
		}
		if (hasDiscovered)
		{
			asciiMap[std::to_string(z)] = serializeAsciiMap(z, dataSetChars);
		}
	}
	msg["ascii_map"] = asciiMap;
	msg["map_legend"] = mapLegend;

	int ux0 = sizeX, uy0 = sizeY, uz0 = sizeZ, ux1 = -1, uy1 = -1, uz1 = -1;
	int cx0 = sizeX, cy0 = sizeY, cz0 = sizeZ, cx1 = -1, cy1 = -1, cz1 = -1;
	for (int z2 = 0; z2 < sizeZ; z2++)
	{
		for (int y2 = 0; y2 < sizeY; y2++)
		{
			for (int x2 = 0; x2 < sizeX; x2++)
			{
				Tile *t = _save->getTile(Position(x2, y2, z2));
				if (!t || !t->isDiscovered(2)) continue;
				for (int part = 0; part < 4; part++)
				{
					int mdID, mdsID;
					t->getMapData(&mdID, &mdsID, (TilePart)part);
					if (mdsID < 0) continue;
					if (ufoSets.count(mdsID))
					{
						if (x2 < ux0) ux0 = x2; if (x2 > ux1) ux1 = x2;
						if (y2 < uy0) uy0 = y2; if (y2 > uy1) uy1 = y2;
						if (z2 < uz0) uz0 = z2; if (z2 > uz1) uz1 = z2;
						break;
					}
					else if (craftSets.count(mdsID))
					{
						if (x2 < cx0) cx0 = x2; if (x2 > cx1) cx1 = x2;
						if (y2 < cy0) cy0 = y2; if (y2 > cy1) cy1 = y2;
						if (z2 < cz0) cz0 = z2; if (z2 > cz1) cz1 = z2;
						break;
					}
				}
			}
		}
	}
	if (ux1 >= 0)
	{
		msg["ufo_bounds"] = {{"x_min", ux0}, {"y_min", uy0}, {"z_min", uz0},
		                     {"x_max", ux1}, {"y_max", uy1}, {"z_max", uz1}};
	}
	if (cx1 >= 0)
	{
		msg["craft_bounds"] = {{"x_min", cx0}, {"y_min", cy0}, {"z_min", cz0},
		                       {"x_max", cx1}, {"y_max", cy1}, {"z_max", cz1}};
	}

	return msg;
}

/**
 * Serializes a player unit with full stats, inventory, and visibility info.
 * @param unit The player's battle unit.
 * @param lang Language for name translation.
 * @return JSON object with unit data.
 */
nlohmann::json AIBridge::serializeUnit(BattleUnit *unit, Language *lang) const
{
	nlohmann::json j;
	j["id"] = unit->getId();
	j["name"] = unit->getName(lang);
	j["type"] = unit->getType();

	Position pos = unit->getPosition();
	j["pos"] = { pos.x, pos.y, pos.z };
	j["direction"] = unit->getDirection();

	// Current and max stats
	UnitStats *base = unit->getBaseStats();
	j["tu"] = unit->getTimeUnits();
	j["tu_max"] = base->tu;
	j["hp"] = unit->getHealth();
	j["hp_max"] = base->health;
	j["energy"] = unit->getEnergy();
	j["energy_max"] = base->stamina;
	j["morale"] = unit->getMorale();
	j["stun"] = unit->getStunlevel();
	j["kneeling"] = unit->isKneeled();
	j["fire"] = unit->getFire();

	// Fatal wounds per body part
	nlohmann::json wounds;
	wounds["head"] = unit->getFatalWound(BODYPART_HEAD);
	wounds["torso"] = unit->getFatalWound(BODYPART_TORSO);
	wounds["right_arm"] = unit->getFatalWound(BODYPART_RIGHTARM);
	wounds["left_arm"] = unit->getFatalWound(BODYPART_LEFTARM);
	wounds["right_leg"] = unit->getFatalWound(BODYPART_RIGHTLEG);
	wounds["left_leg"] = unit->getFatalWound(BODYPART_LEFTLEG);
	j["fatal_wounds"] = wounds;

	// Current armor values per side
	nlohmann::json armor;
	armor["front"] = unit->getArmor(SIDE_FRONT);
	armor["left"] = unit->getArmor(SIDE_LEFT);
	armor["right"] = unit->getArmor(SIDE_RIGHT);
	armor["rear"] = unit->getArmor(SIDE_REAR);
	armor["under"] = unit->getArmor(SIDE_UNDER);
	j["armor_current"] = armor;

	// Base stats for AI reasoning
	nlohmann::json stats;
	stats["tu"] = base->tu;
	stats["stamina"] = base->stamina;
	stats["health"] = base->health;
	stats["bravery"] = base->bravery;
	stats["reactions"] = base->reactions;
	stats["firing"] = base->firing;
	stats["throwing"] = base->throwing;
	stats["strength"] = base->strength;
	stats["melee"] = base->melee;
	j["stats"] = stats;

	// Inventory items
	nlohmann::json inv = nlohmann::json::array();
	for (std::vector<BattleItem*>::iterator it = unit->getInventory()->begin(); it != unit->getInventory()->end(); ++it)
	{
		inv.push_back(serializeItem(*it, unit));
	}
	j["inventory"] = inv;

	// IDs of visible enemies (full data in top-level visible_enemies)
	nlohmann::json visIds = nlohmann::json::array();
	for (std::vector<BattleUnit*>::iterator it = unit->getVisibleUnits()->begin(); it != unit->getVisibleUnits()->end(); ++it)
	{
		visIds.push_back((*it)->getId());
	}
	j["visible_enemies"] = visIds;

	return j;
}

/**
 * Serializes an inventory item with type, ammo, and pre-calculated TU costs.
 * @param item The battle item.
 * @param owner The unit carrying this item (for TU cost calculation).
 * @return JSON object with item data.
 */
nlohmann::json AIBridge::serializeItem(BattleItem *item, BattleUnit *owner) const
{
	nlohmann::json j;
	j["id"] = item->getId();
	j["type"] = item->getRules()->getType();

	if (item->getSlot())
		j["slot"] = item->getSlot()->getId();

	// Battle type as readable string
	BattleType bt = item->getRules()->getBattleType();
	switch (bt)
	{
		case BT_FIREARM: j["battle_type"] = "firearm"; break;
		case BT_AMMO: j["battle_type"] = "ammo"; break;
		case BT_MELEE: j["battle_type"] = "melee"; break;
		case BT_GRENADE: j["battle_type"] = "grenade"; break;
		case BT_PROXIMITYGRENADE: j["battle_type"] = "proximity_grenade"; break;
		case BT_MEDIKIT: j["battle_type"] = "medikit"; break;
		case BT_SCANNER: j["battle_type"] = "scanner"; break;
		case BT_MINDPROBE: j["battle_type"] = "mind_probe"; break;
		case BT_PSIAMP: j["battle_type"] = "psi_amp"; break;
		case BT_FLARE: j["battle_type"] = "flare"; break;
		case BT_CORPSE: j["battle_type"] = "corpse"; break;
		default: j["battle_type"] = "none"; break;
	}

	// Ammo info
	if (item->getAmmoItem())
	{
		j["ammo_type"] = item->getAmmoItem()->getRules()->getType();
		j["ammo_qty"] = item->getAmmoItem()->getAmmoQuantity();
	}
	else if (item->getRules()->getClipSize() > 0)
	{
		// Built-in ammo (laser weapons, etc.)
		j["ammo_qty"] = item->getAmmoQuantity();
	}

	// Grenade fuse timer
	if (item->getFuseTimer() >= 0)
	{
		j["fuse_timer"] = item->getFuseTimer();
	}

	// Pre-calculated TU costs for weapon actions
	RuleItem *rules = item->getRules();
	if (bt == BT_FIREARM || bt == BT_MELEE)
	{
		if (rules->getTUSnap() > 0)
			j["tu_snap"] = owner->getActionTUs(BA_SNAPSHOT, item);
		if (rules->getTUAimed() > 0)
			j["tu_aimed"] = owner->getActionTUs(BA_AIMEDSHOT, item);
		if (rules->getTUAuto() > 0)
			j["tu_auto"] = owner->getActionTUs(BA_AUTOSHOT, item);
		if (bt == BT_MELEE || rules->getTUMelee() > 0)
			j["tu_melee"] = owner->getActionTUs(BA_HIT, item);

		j["power"] = rules->getPower();
		j["max_range"] = rules->getMaxRange();
		j["accuracy_snap"] = rules->getAccuracySnap();
		j["accuracy_aimed"] = rules->getAccuracyAimed();
		j["accuracy_auto"] = rules->getAccuracyAuto();
		j["two_handed"] = rules->isTwoHanded();
	}

	// TU costs for usable items
	if (bt == BT_MEDIKIT || bt == BT_SCANNER || bt == BT_MINDPROBE || bt == BT_PSIAMP)
	{
		j["tu_use"] = owner->getActionTUs(BA_USE, item);
	}

	// Throw cost
	if (bt == BT_GRENADE || bt == BT_FIREARM || bt == BT_FLARE)
	{
		j["tu_throw"] = owner->getActionTUs(BA_THROW, item);
	}

	// Medikit quantities
	if (bt == BT_MEDIKIT)
	{
		j["heal_qty"] = item->getHealQuantity();
		j["painkillers_qty"] = item->getPainKillerQuantity();
		j["stimulant_qty"] = item->getStimulantQuantity();
	}

	return j;
}

/**
 * Serializes a visible enemy unit with limited info (position, type, direction).
 * @param unit The enemy unit.
 * @param lang Language for name translation.
 * @return JSON object with enemy data.
 */
nlohmann::json AIBridge::serializeVisibleEnemy(BattleUnit *unit, Language *lang) const
{
	nlohmann::json j;
	j["id"] = unit->getId();
	j["type"] = unit->getType();
	j["name"] = unit->getName(lang);

	Position pos = unit->getPosition();
	j["pos"] = { pos.x, pos.y, pos.z };
	j["direction"] = unit->getDirection();
	j["kneeling"] = unit->isKneeled();

	// Faction (could be hostile or neutral)
	switch (unit->getFaction())
	{
		case FACTION_HOSTILE: j["faction"] = "hostile"; break;
		case FACTION_NEUTRAL: j["faction"] = "neutral"; break;
		default: j["faction"] = "unknown"; break;
	}

	return j;
}

/**
 * Serializes a z-level as a 2x2-per-tile ASCII map string.
 * Each game tile maps to a 2-char wide by 2-char tall block:
 *   [NW corner][north edge]
 *   [west edge][floor]
 *
 * Characters: a-z building/terrain type (see map_legend), # impassable, space undiscovered/void,
 * | west wall, - north wall, + corner, \ door, ! = UFO hull, : ; fence,
 * / stairs, ^ gravlift, 1-9/A-E player units (by index), X enemies, ~ smoke, * fire
 *
 * @param z The z-level to render.
 * @return ASCII string with newlines separating rows.
 */
std::string AIBridge::serializeAsciiMap(int z, const std::map<int, char> &dataSetChars) const
{
	int sizeX = _save->getMapSizeX();
	int sizeY = _save->getMapSizeY();
	int gridW = sizeX * 2;
	int gridH = sizeY * 2;

	// Build set of visible enemy IDs (only show enemies the player can actually see)
	std::set<int> visibleEnemyIds;
	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		BattleUnit *u = *i;
		if (u->getFaction() != FACTION_PLAYER || u->isOut()) continue;
		for (std::vector<BattleUnit*>::iterator j = u->getVisibleUnits()->begin(); j != u->getVisibleUnits()->end(); ++j)
		{
			visibleEnemyIds.insert((*j)->getId());
		}
	}

	// Build index of player units for numbering (1-9, A-E)
	std::map<int, char> unitChars; // unit ID -> display char
	int unitIdx = 0;
	for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		BattleUnit *u = *i;
		if (u->getFaction() != FACTION_PLAYER || u->isOut()) continue;
		char c;
		if (unitIdx < 9)
			c = '1' + unitIdx;
		else
			c = 'A' + (unitIdx - 9);
		unitChars[u->getId()] = c;
		unitIdx++;
	}

	// Initialize grid with spaces (undiscovered)
	std::vector<char> grid(gridW * gridH, ' ');

	for (int y = 0; y < sizeY; y++)
	{
		for (int x = 0; x < sizeX; x++)
		{
			Tile *tile = _save->getTile(Position(x, y, z));
			if (!tile || !tile->isDiscovered(2) || tile->isVoid()) continue;

			int gx = x * 2;
			int gy = y * 2;

			// NW corner (top-left of 2x2 block) — always '+'
			grid[gy * gridW + gx] = '+';

			// North edge (top-right of 2x2 block)
			MapData *northWall = tile->getMapData(O_NORTHWALL);
			if (northWall)
			{
				if (northWall->isDoor() || northWall->isUFODoor())
					grid[gy * gridW + gx + 1] = '\\';
				else
				{
					int armor = northWall->getArmor();
					if (armor >= 80)
						grid[gy * gridW + gx + 1] = '='; // UFO hull
					else if (armor <= 20)
						grid[gy * gridW + gx + 1] = ';'; // fence/light
					else
						grid[gy * gridW + gx + 1] = '-'; // regular wall
				}
			}
			else
			{
				grid[gy * gridW + gx + 1] = ' ';
			}

			// West edge (bottom-left of 2x2 block)
			MapData *westWall = tile->getMapData(O_WESTWALL);
			if (westWall)
			{
				if (westWall->isDoor() || westWall->isUFODoor())
					grid[(gy + 1) * gridW + gx] = '\\';
				else
				{
					int armor = westWall->getArmor();
					if (armor >= 80)
						grid[(gy + 1) * gridW + gx] = '!'; // UFO hull
					else if (armor <= 20)
						grid[(gy + 1) * gridW + gx] = ':'; // fence/light
					else
						grid[(gy + 1) * gridW + gx] = '|'; // regular wall
				}
			}
			else
			{
				grid[(gy + 1) * gridW + gx] = ' ';
			}

			// Floor (bottom-right of 2x2 block) — the main content cell
			// Determine zone character from MapDataSet (building type)
			char zoneChar = '.';
			int zonePriority = 0; // 0=default, 1=named, 2=craft, 3=UFO
			for (int part = 0; part < 4; part++)
			{
				int mdID, mdsID;
				tile->getMapData(&mdID, &mdsID, (TilePart)part);
				if (mdsID < 0) continue;
				std::map<int, char>::const_iterator it = dataSetChars.find(mdsID);
				if (it != dataSetChars.end())
				{
					int p = (it->second == 'u') ? 3 : (it->second == 's') ? 2 : 1;
					if (p > zonePriority)
					{
						zonePriority = p;
						zoneChar = it->second;
					}
				}
			}
			char floorChar = zoneChar;
			MapData *floor = tile->getMapData(O_FLOOR);
			MapData *object = tile->getMapData(O_OBJECT);

			// Check walkability
			if (!floor && !object)
			{
				Tile *tileBelow = _save->getTile(Position(x, y, z - 1));
				if (tile->hasNoFloor(tileBelow))
					floorChar = ' '; // void/hole
			}
			else if (object && object->getTUCost(MT_WALK) == 255)
			{
				floorChar = '#'; // impassable object
			}

			// Stairs (terrainLevel <= -16 means stair top, auto z-transition)
			if (tile->getTerrainLevel() <= -16)
				floorChar = '/';

			// Gravlift
			if ((floor && floor->isGravLift()) || (object && object->isGravLift()))
				floorChar = '^';

			// Override with environmental effects
			if (tile->getFire() > 0)
				floorChar = '*';
			else if (tile->getSmoke() > 0)
				floorChar = '~';

			// Override with units
			BattleUnit *tileUnit = tile->getUnit();
			if (tileUnit)
			{
				if (tileUnit->getFaction() == FACTION_PLAYER && !tileUnit->isOut())
				{
					std::map<int, char>::iterator it = unitChars.find(tileUnit->getId());
					if (it != unitChars.end())
						floorChar = it->second;
				}
				else if (tileUnit->getFaction() == FACTION_HOSTILE && !tileUnit->isOut()
				&& visibleEnemyIds.count(tileUnit->getId()))
				{
					floorChar = 'X';
				}
			}

			grid[(gy + 1) * gridW + gx + 1] = floorChar;
		}
	}

	// Build string with newlines, trimming trailing spaces per row
	std::string result;
	result.reserve(gridH * (gridW + 1));
	for (int gy = 0; gy < gridH; gy++)
	{
		int end = gridW;
		while (end > 0 && grid[gy * gridW + end - 1] == ' ')
			end--;
		if (end > 0)
		{
			result.append(&grid[gy * gridW], end);
		}
		result += '\n';
	}
	return result;
}

/**
 * Counts total discovered tiles on the map (for detecting new discoveries after walk).
 */
int AIBridge::countDiscoveredTiles() const
{
	int count = 0;
	int sizeX = _save->getMapSizeX();
	int sizeY = _save->getMapSizeY();
	int sizeZ = _save->getMapSizeZ();
	for (int z = 0; z < sizeZ; z++)
		for (int y = 0; y < sizeY; y++)
			for (int x = 0; x < sizeX; x++)
			{
				Tile *tile = _save->getTile(Position(x, y, z));
				if (tile && tile->isDiscovered(2))
					count++;
			}
	return count;
}

/**
 * Attaches ascii_map and map_legend to a JSON message (reuses serializeGameState map logic).
 */
void AIBridge::attachMap(nlohmann::json &msg) const
{
	// Build MapDataSet-to-character mapping (same logic as serializeGameState)
	std::vector<MapDataSet*> *dataSets = _save->getMapDataSets();
	std::map<int, char> dataSetChars;
	nlohmann::json mapLegend;
	char nextChar = 'a';
	for (size_t i = 0; i < dataSets->size(); i++)
	{
		std::string name = dataSets->at(i)->getName();
		if (name.compare(0, 3, "UFO") == 0 || name.compare(0, 2, "U_") == 0
			|| name.compare(0, 4, "UEXT") == 0 || name.compare(0, 4, "UINT") == 0)
		{
			dataSetChars[i] = 'u';
		}
		else if (name == "PLANE" || name == "LIGHTNIN" || name == "AVENGER"
			|| name == "TRITON" || name == "HAMMER" || name == "LEVIATH")
		{
			dataSetChars[i] = 's';
		}
		else if (name != "BLANKS" && nextChar <= 'z')
		{
			dataSetChars[i] = nextChar;
			mapLegend[std::string(1, nextChar)] = name;
			nextChar++;
			if (nextChar == 's' || nextChar == 'u') nextChar++;
			if (nextChar == 's' || nextChar == 'u') nextChar++;
		}
	}
	mapLegend["u"] = "UFO";
	mapLegend["s"] = "craft";
	mapLegend["/"] = "stairs";
	mapLegend["^"] = "gravlift";

	nlohmann::json asciiMap;
	int sizeZ = _save->getMapSizeZ();
	int sizeX = _save->getMapSizeX();
	int sizeY = _save->getMapSizeY();
	for (int z = 0; z < sizeZ; z++)
	{
		bool hasDiscovered = false;
		for (int y = 0; y < sizeY && !hasDiscovered; y++)
			for (int x = 0; x < sizeX && !hasDiscovered; x++)
			{
				Tile *tile = _save->getTile(Position(x, y, z));
				if (tile && tile->isDiscovered(2) && !tile->isVoid())
					hasDiscovered = true;
			}
		if (hasDiscovered)
			asciiMap[std::to_string(z)] = serializeAsciiMap(z, dataSetChars);
	}
	msg["ascii_map"] = asciiMap;
	msg["map_legend"] = mapLegend;
}

// --- Event Queue (Phase 4) ---

/**
 * Pushes an event to the queue. Events accumulate and are sent with the next response.
 * @param event JSON object describing the event.
 */
void AIBridge::pushEvent(const nlohmann::json &event)
{
	_eventQueue.push_back(event);
	Log(LOG_DEBUG) << "AIBridge: event queued: " << event.value("type", "unknown");
}

/**
 * Inserts an event at a specific position in the queue.
 * Used to ensure shot_result appears before unit_wounded/unit_killed events.
 */
void AIBridge::insertEvent(size_t pos, const nlohmann::json &event)
{
	if (pos >= _eventQueue.size())
		_eventQueue.push_back(event);
	else
		_eventQueue.insert(_eventQueue.begin() + pos, event);
	Log(LOG_DEBUG) << "AIBridge: event inserted at " << pos << ": " << event.value("type", "unknown");
}

/**
 * Returns current event queue size.
 */
size_t AIBridge::getEventCount() const
{
	return _eventQueue.size();
}

/**
 * Marks the ASCII map as dirty — next action_complete will attach updated map.
 */
void AIBridge::setMapDirty()
{
	_mapDirty = true;
}

/**
 * Returns all pending events as a JSON array and clears the queue.
 * @return JSON array of events.
 */
nlohmann::json AIBridge::flushEvents()
{
	nlohmann::json events = nlohmann::json::array();
	for (std::vector<nlohmann::json>::iterator it = _eventQueue.begin(); it != _eventQueue.end(); ++it)
	{
		events.push_back(*it);
	}
	_eventQueue.clear();
	return events;
}

// --- Command Handling (Phase 3) ---

/**
 * Returns true if there is a parsed command waiting to be dispatched.
 */
bool AIBridge::hasPendingCommand() const
{
	return _hasPendingCommand;
}

/**
 * Returns and clears the pending command.
 */
AICommand AIBridge::consumeCommand()
{
	_hasPendingCommand = false;
	return _pendingCommand;
}

/**
 * Returns true if a dispatched action is still animating.
 */
bool AIBridge::isActionExecuting() const
{
	return _actionExecuting;
}

/**
 * Marks that an async action has been dispatched (walk, shoot, throw).
 * @param unitId Unit performing the action.
 * @param action Action type string.
 */
void AIBridge::setActionExecuting(int unitId, const std::string &action)
{
	_actionExecuting = true;
	_executingUnitId = unitId;
	_executingAction = action;

	// Snapshot discovered tile count before walk so we can detect new discoveries
	if (action == "walk")
		_discoveredCountBefore = countDiscoveredTiles();
}

/**
 * Sends action_complete to the client and resets executing state.
 * @param unitId Unit that performed the action.
 * @param action Action type string.
 * @param success Whether the action succeeded.
 * @param error Error description if failed.
 */
void AIBridge::notifyActionComplete(int unitId, const std::string &action, bool success, const std::string &error)
{
	_actionExecuting = false;

	nlohmann::json msg;
	msg["type"] = "action_complete";
	msg["action"] = action;
	msg["unit_id"] = unitId;
	msg["success"] = success;
	if (!error.empty())
		msg["error"] = error;

	// Attach accumulated events
	msg["events"] = flushEvents();

	// Enrich with unit state after action
	if (success && unitId >= 0)
	{
		BattleUnit *unit = 0;
		for (std::vector<BattleUnit*>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
		{
			if ((*i)->getId() == unitId) { unit = *i; break; }
		}
		if (unit)
		{
			Position pos = unit->getPosition();
			msg["pos"] = {pos.x, pos.y, pos.z};
			msg["tu"] = unit->getTimeUnits();
			msg["energy"] = unit->getEnergy();
			msg["hp"] = unit->getHealth();
			msg["direction"] = unit->getDirection();
			msg["morale"] = unit->getMorale();

			// Ammo status for hands (useful after shooting)
			if (action == "shoot")
			{
				BattleItem *rh = unit->getItem("STR_RIGHT_HAND");
				BattleItem *lh = unit->getItem("STR_LEFT_HAND");
				if (rh && rh->getAmmoItem())
					msg["ammo_right"] = rh->getAmmoItem()->getAmmoQuantity();
				else if (rh && rh->getRules()->getBattleType() == BT_FIREARM)
					msg["ammo_right"] = 0;
				if (lh && lh->getAmmoItem())
					msg["ammo_left"] = lh->getAmmoItem()->getAmmoQuantity();
				else if (lh && lh->getRules()->getBattleType() == BT_FIREARM)
					msg["ammo_left"] = 0;
			}

			// Visible enemies after this action
			nlohmann::json enemies = nlohmann::json::array();
			for (std::vector<BattleUnit*>::iterator j = _save->getUnits()->begin(); j != _save->getUnits()->end(); ++j)
			{
				BattleUnit *enemy = *j;
				if (enemy->getFaction() != FACTION_HOSTILE || enemy->isOut()) continue;
				if (std::find(unit->getVisibleUnits()->begin(), unit->getVisibleUnits()->end(), enemy) != unit->getVisibleUnits()->end())
				{
					nlohmann::json e;
					e["id"] = enemy->getId();
					Position ep = enemy->getPosition();
					e["pos"] = {ep.x, ep.y, ep.z};
					enemies.push_back(e);
				}
			}
			msg["visible_enemies"] = enemies;
		}
	}

	// Attach full map if it changed (walk discovered new tiles, explosion, fire, door, etc.)
	if (_mapDirty)
	{
		attachMap(msg);
		_mapDirty = false;
	}
	else if (action == "walk" && success)
	{
		int discoveredNow = countDiscoveredTiles();
		if (discoveredNow > _discoveredCountBefore)
		{
			attachMap(msg);
		}
	}

	sendMessage(msg);
}

/**
 * Sends action_complete for the currently executing action using stored unit/action.
 * @param success Whether the action succeeded.
 * @param error Error description if failed.
 */
void AIBridge::notifyCurrentActionComplete(bool success, const std::string &error)
{
	notifyActionComplete(_executingUnitId, _executingAction, success, error);
}

/**
 * Sends an error response to the client.
 * @param action Action that failed.
 * @param unitId Unit involved.
 * @param error Error description.
 */
void AIBridge::sendError(const std::string &action, int unitId, const std::string &error)
{
	nlohmann::json msg;
	msg["type"] = "action_error";
	msg["action"] = action;
	msg["unit_id"] = unitId;
	msg["error"] = error;
	msg["events"] = flushEvents();
	sendMessage(msg);
	Log(LOG_DEBUG) << "AIBridge: error " << error << " for action " << action;
}

}
