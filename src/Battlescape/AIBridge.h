#pragma once
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
#include <string>
#include <vector>
#include <map>
#include "Position.h"
#include "../lib/nlohmann/json.hpp"

namespace OpenXcom
{

class SavedBattleGame;
class Language;
class BattleUnit;
class BattleItem;
class Tile;

/**
 * Parsed command from the AI client.
 */
struct AICommand
{
	std::string action;   /// "walk","shoot","kneel","throw","prime","select","end_turn"
	int unitId;           /// target unit ID (-1 for end_turn)
	Position target;      /// target position for walk/shoot/throw
	std::string shotType; /// "snap","aimed","auto" (for shoot)
	std::string hand;     /// "right","left" — which hand's weapon to use
	int value;            /// fuse timer (for prime)
	Position from;        /// hypothetical position for get_fire_options (-1,-1,-1 = use current)
	bool hasFrom;         /// whether 'from' was specified
	int radius;           /// blast radius for get_blast_check

	AICommand() : unitId(-1), value(0), from(-1,-1,-1), hasFrom(false), radius(0) {}
};

/**
 * Non-blocking TCP socket server for external AI player control.
 * Accepts a single client connection and exchanges JSON-lines messages.
 * Designed to be called from the main SDL thread every frame without blocking.
 */
class AIBridge
{
private:
	int _listenFd;        /// listening socket file descriptor
	int _clientFd;        /// connected client fd (-1 if none)
	int _port;            /// TCP port
	bool _enabled;        /// server is listening
	std::string _recvBuf; /// partial receive buffer
	std::string _sendBuf; /// pending send buffer
	int _lastTurnSent;    /// dedup turn_start notifications
	int _currentTurn;     /// current turn number (for get_state)
	Language *_lang;      /// language pointer (for get_state serialization)
	SavedBattleGame *_save; /// battle state for serialization

	AICommand _pendingCommand;    /// parsed command waiting to be dispatched
	bool _hasPendingCommand;      /// true if _pendingCommand is valid
	bool _actionExecuting;        /// true while a dispatched action is animating
	int _executingUnitId;         /// unit ID of the executing action
	std::string _executingAction; /// action type of the executing action

	std::vector<nlohmann::json> _eventQueue; /// accumulated events for next response
	int _discoveredCountBefore;              /// discovered tile count before walk (for map delta detection)
	bool _mapDirty;                          /// true when map changed (explosion, fire, door, etc.)
	bool _wasUsed;                           /// true after first client command (persists across reconnects)

	/// Sets a file descriptor to non-blocking mode.
	bool setNonBlocking(int fd);
	/// Tries to accept a pending connection.
	void tryAccept();
	/// Reads available data from client (non-blocking).
	void tryRead();
	/// Writes pending data to client (non-blocking).
	void tryWrite();
	/// Processes a complete JSON-lines message from client.
	void processMessage(const std::string &line);
	/// Closes the client connection (keeps listening).
	void closeClient();
	/// Sends an error response to the client.
	void sendError(const std::string &action, int unitId, const std::string &error);

	/// Serializes the full game state for turn_start.
	nlohmann::json serializeGameState(int turn, Language *lang) const;
	/// Serializes a player unit with full stats and inventory.
	nlohmann::json serializeUnit(BattleUnit *unit, Language *lang) const;
	/// Serializes an inventory item with TU costs.
	nlohmann::json serializeItem(BattleItem *item, BattleUnit *owner) const;
	/// Serializes a visible enemy unit (limited info).
	nlohmann::json serializeVisibleEnemy(BattleUnit *unit, Language *lang) const;
	/// Serializes a z-level as a 1x1-per-tile ASCII map with coordinate axes.
	std::string serializeAsciiMap(int z, const std::map<int, char> &dataSetChars) const;
	/// Scans discovered tiles for doors and returns JSON array.
	nlohmann::json serializeDoors() const;
	/// Counts total discovered tiles on the map.
	int countDiscoveredTiles() const;
	/// Attaches ascii_map + map_legend + doors to a JSON message.
	void attachMap(nlohmann::json &msg) const;

public:
	/// Sends a JSON message to the connected client.
	void sendMessage(const nlohmann::json &msg);

	/// Creates the AIBridge with a reference to the battle state.
	AIBridge(SavedBattleGame *save);
	/// Cleans up sockets.
	~AIBridge();

	/// Starts listening on the given port. Returns true on success.
	bool start(int port);
	/// Stops the server and closes all connections.
	void stop();

	/// Called every frame — handles accept, read, write without blocking.
	void poll();

	/// Notifies that a battle has started.
	void notifyBattleStart();
	/// Notifies that the player's turn has started (sends full game state).
	void notifyTurnStart(int turn, Language *lang);
	/// Notifies that the battle has ended.
	void notifyBattleEnd();

	/// Returns true if a client is connected.
	bool isConnected() const;
	/// Returns true if AI bridge was used (any command received).
	bool wasUsed() const;

	/// Returns true if there is a parsed command waiting to be dispatched.
	bool hasPendingCommand() const;
	/// Returns and clears the pending command.
	AICommand consumeCommand();
	/// Returns true if a dispatched action is still animating.
	bool isActionExecuting() const;
	/// Marks that an async action has been dispatched (walk, shoot, throw).
	void setActionExecuting(int unitId, const std::string &action);
	/// Sends action_complete to the client.
	void notifyActionComplete(int unitId, const std::string &action, bool success, const std::string &error = "");
	/// Sends action_complete for the currently executing action.
	void notifyCurrentActionComplete(bool success, const std::string &error = "");

	/// Marks the map as changed — action_complete will attach updated map.
	void setMapDirty();
	/// Pushes an event to the queue (will be sent with the next response).
	void pushEvent(const nlohmann::json &event);
	/// Inserts an event at a specific position in the queue.
	void insertEvent(size_t pos, const nlohmann::json &event);
	/// Returns current event queue size.
	size_t getEventCount() const;
	/// Returns pending events as JSON array and clears the queue.
	nlohmann::json flushEvents();
};

}
