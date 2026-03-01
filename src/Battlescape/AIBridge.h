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
#include "../lib/nlohmann/json.hpp"

namespace OpenXcom
{

class SavedBattleGame;
class Language;
class BattleUnit;
class BattleItem;
class Tile;

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
	SavedBattleGame *_save; /// battle state for serialization

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
	/// Sends a JSON message to the connected client.
	void sendMessage(const nlohmann::json &msg);
	/// Closes the client connection (keeps listening).
	void closeClient();

	/// Serializes the full game state for turn_start.
	nlohmann::json serializeGameState(int turn, Language *lang) const;
	/// Serializes a player unit with full stats and inventory.
	nlohmann::json serializeUnit(BattleUnit *unit, Language *lang) const;
	/// Serializes an inventory item with TU costs.
	nlohmann::json serializeItem(BattleItem *item, BattleUnit *owner) const;
	/// Serializes a visible enemy unit (limited info).
	nlohmann::json serializeVisibleEnemy(BattleUnit *unit, Language *lang) const;
	/// Serializes a discovered map tile.
	nlohmann::json serializeTile(Tile *tile) const;

public:
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
};

}
