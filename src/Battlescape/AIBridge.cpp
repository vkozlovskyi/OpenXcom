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
#include "../Engine/Logger.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace OpenXcom
{

/**
 * Creates the AIBridge. Does not start listening yet.
 */
AIBridge::AIBridge() : _listenFd(-1), _clientFd(-1), _port(0), _enabled(false), _lastTurnSent(-1)
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

	if (_clientFd < 0)
	{
		// No client — check for incoming connections
		FD_SET(_listenFd, &readfds);
		maxfd = _listenFd;
	}
	else
	{
		// Have client — check for incoming data
		FD_SET(_clientFd, &readfds);
		maxfd = _clientFd;
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

	if (_clientFd < 0 && FD_ISSET(_listenFd, &readfds))
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

	setNonBlocking(fd);
	_clientFd = fd;
	_recvBuf.clear();
	_sendBuf.clear();

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
 * Phase 1: echoes back the parsed message for testing.
 * @param line A single line of JSON text.
 */
void AIBridge::processMessage(const std::string &line)
{
	Log(LOG_DEBUG) << "AIBridge recv: " << line;

	try
	{
		nlohmann::json msg = nlohmann::json::parse(line);

		// Phase 1: echo back for testing
		nlohmann::json echo;
		echo["type"] = "echo";
		echo["data"] = msg;
		sendMessage(echo);
	}
	catch (const nlohmann::json::parse_error &e)
	{
		Log(LOG_WARNING) << "AIBridge: JSON parse error: " << e.what();
		nlohmann::json err;
		err["type"] = "error";
		err["message"] = "invalid JSON";
		sendMessage(err);
	}
}

/**
 * Queues a JSON message for sending to the client.
 * @param msg JSON object to send.
 */
void AIBridge::sendMessage(const nlohmann::json &msg)
{
	if (_clientFd < 0) return;
	_sendBuf += msg.dump() + "\n";
	Log(LOG_DEBUG) << "AIBridge send: " << msg.dump();
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
 * Idempotent — will not send duplicate notifications for the same turn.
 * @param turn Turn number.
 */
void AIBridge::notifyTurnStart(int turn)
{
	if (_lastTurnSent == turn) return;
	_lastTurnSent = turn;

	nlohmann::json msg;
	msg["type"] = "turn_start";
	msg["turn"] = turn;
	sendMessage(msg);
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

}
