#include "stdafx.h"

#ifdef _WINDOWS64

#include "WinsockNetLayer.h"
#include "..\..\Common\Network\PlatformNetworkManagerStub.h"
#include "..\..\..\Minecraft.World\Socket.h"
#include "json.hpp"

HttpResponse WinsockNetLayer::DoWinHttpRequest(const wchar_t* host, const std::wstring& path, const wchar_t* method, const std::string& requestData, const std::vector<std::wstring>& headers)
{
	HttpResponse response;
	response.status = 0;

	HINTERNET hSession = WinHttpOpen(L"Minecraft Client", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return response;

	HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) { WinHttpCloseHandle(hSession); return response; }

	HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return response; }

	for (const auto& header : headers) {
		WinHttpAddRequestHeaders(hRequest, header.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	}

	BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)requestData.c_str(), (DWORD)requestData.size(), (DWORD)requestData.size(), 0);
	if (bResults) bResults = WinHttpReceiveResponse(hRequest, NULL);

	if (bResults) {
		DWORD dwStatusCode = 0;
		DWORD dwSize = sizeof(dwStatusCode);
		WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
		response.status = dwStatusCode;

		DWORD dwDownloaded = 0;
		do {
			dwSize = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
			if (dwSize == 0) break;
			char* pszOutBuffer = new char[dwSize + 1];
			if (!pszOutBuffer) break;
			ZeroMemory(pszOutBuffer, dwSize + 1);
			if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
				response.body.append(pszOutBuffer, dwDownloaded);
			}
			delete[] pszOutBuffer;
		} while (dwSize > 0);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return response;
}

SOCKET WinsockNetLayer::s_listenSocket = INVALID_SOCKET;
SOCKET WinsockNetLayer::s_hostConnectionSocket = INVALID_SOCKET;
HANDLE WinsockNetLayer::s_acceptThread = NULL;
HANDLE WinsockNetLayer::s_clientRecvThread = NULL;

std::string WinsockNetLayer::s_accessToken = "";
std::string WinsockNetLayer::s_userId = "";

std::string WinsockNetLayer::s_customHostIp = "";
int WinsockNetLayer::s_customHostPort = 0;

bool WinsockNetLayer::s_isHost = false;
bool WinsockNetLayer::s_connected = false;
bool WinsockNetLayer::s_active = false;
bool WinsockNetLayer::s_initialized = false;

BYTE WinsockNetLayer::s_localSmallId = 0;
BYTE WinsockNetLayer::s_hostSmallId = 0;
BYTE WinsockNetLayer::s_nextSmallId = 1;

CRITICAL_SECTION WinsockNetLayer::s_sendLock;
CRITICAL_SECTION WinsockNetLayer::s_connectionsLock;

std::vector<Win64RemoteConnection> WinsockNetLayer::s_connections;

SOCKET WinsockNetLayer::s_advertiseSock = INVALID_SOCKET;
HANDLE WinsockNetLayer::s_advertiseThread = NULL;
volatile bool WinsockNetLayer::s_advertising = false;
Win64LANBroadcast WinsockNetLayer::s_advertiseData = {};
CRITICAL_SECTION WinsockNetLayer::s_advertiseLock;
int WinsockNetLayer::s_hostGamePort = WIN64_NET_DEFAULT_PORT;

SOCKET WinsockNetLayer::s_discoverySock = INVALID_SOCKET;
HANDLE WinsockNetLayer::s_discoveryThread = NULL;
volatile bool WinsockNetLayer::s_discovering = false;
CRITICAL_SECTION WinsockNetLayer::s_discoveryLock;
std::vector<Win64LANSession> WinsockNetLayer::s_discoveredSessions;

CRITICAL_SECTION WinsockNetLayer::s_disconnectLock;
std::vector<BYTE> WinsockNetLayer::s_disconnectedSmallIds;

CRITICAL_SECTION WinsockNetLayer::s_freeSmallIdLock;
std::vector<BYTE> WinsockNetLayer::s_freeSmallIds;

bool g_Win64MultiplayerHost = false;
bool g_Win64MultiplayerJoin = false;
int g_Win64MultiplayerPort = WIN64_NET_DEFAULT_PORT;
char g_Win64MultiplayerIP[256] = "127.0.0.1";

bool WinsockNetLayer::Initialize()
{
	if (s_initialized) return true;

	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0)
	{
		app.DebugPrintf("WSAStartup failed: %d\n", result);
		return false;
	}

	InitializeCriticalSection(&s_sendLock);
	InitializeCriticalSection(&s_connectionsLock);
	InitializeCriticalSection(&s_advertiseLock);
	InitializeCriticalSection(&s_discoveryLock);
	InitializeCriticalSection(&s_disconnectLock);
	InitializeCriticalSection(&s_freeSmallIdLock);

	s_initialized = true;

	StartDiscovery();

	return true;
}

void WinsockNetLayer::SetSupabaseCredentials(const std::string& token, const std::string& userId)
{
	s_accessToken = token;
	s_userId = userId;
}

void WinsockNetLayer::SetCustomHostAddress(const std::string& ip, int port)
{
	s_customHostIp = ip;
	s_customHostPort = port;
}

void WinsockNetLayer::Shutdown()
{
	StopAdvertising();
	StopDiscovery();

	s_active = false;
	s_connected = false;

	if (s_listenSocket != INVALID_SOCKET)
	{
		closesocket(s_listenSocket);
		s_listenSocket = INVALID_SOCKET;
	}

	if (s_hostConnectionSocket != INVALID_SOCKET)
	{
		closesocket(s_hostConnectionSocket);
		s_hostConnectionSocket = INVALID_SOCKET;
	}

	EnterCriticalSection(&s_connectionsLock);
	for (size_t i = 0; i < s_connections.size(); i++)
	{
		s_connections[i].active = false;
		if (s_connections[i].tcpSocket != INVALID_SOCKET)
		{
			closesocket(s_connections[i].tcpSocket);
		}
	}
	s_connections.clear();
	LeaveCriticalSection(&s_connectionsLock);

	if (s_acceptThread != NULL)
	{
		WaitForSingleObject(s_acceptThread, 2000);
		CloseHandle(s_acceptThread);
		s_acceptThread = NULL;
	}

	if (s_clientRecvThread != NULL)
	{
		WaitForSingleObject(s_clientRecvThread, 2000);
		CloseHandle(s_clientRecvThread);
		s_clientRecvThread = NULL;
	}

	if (s_initialized)
	{
		DeleteCriticalSection(&s_sendLock);
		DeleteCriticalSection(&s_connectionsLock);
		DeleteCriticalSection(&s_advertiseLock);
		DeleteCriticalSection(&s_discoveryLock);
		DeleteCriticalSection(&s_disconnectLock);
		s_disconnectedSmallIds.clear();
		DeleteCriticalSection(&s_freeSmallIdLock);
		s_freeSmallIds.clear();
		WSACleanup();
		s_initialized = false;
	}
}

bool WinsockNetLayer::HostGame(int port)
{
	if (!s_initialized && !Initialize()) return false;

	s_isHost = true;
	s_localSmallId = 0;
	s_hostSmallId = 0;
	s_nextSmallId = 1;
	s_hostGamePort = port;

	EnterCriticalSection(&s_freeSmallIdLock);
	s_freeSmallIds.clear();
	LeaveCriticalSection(&s_freeSmallIdLock);

	struct addrinfo hints = {};
	struct addrinfo *result = NULL;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	std::string relayIp = s_customHostIp.empty() ? "127.0.0.1" : s_customHostIp;
	int relayPort = s_customHostPort == 0 ? 25565 : s_customHostPort;

	char portStr[16];
	sprintf_s(portStr, "%d", relayPort);

	int iResult = getaddrinfo(relayIp.c_str(), portStr, &hints, &result);
	if (iResult != 0)
	{
		app.DebugPrintf("getaddrinfo failed for Relay Server %s:%d - %d\n", relayIp.c_str(), relayPort, iResult);
		return false;
	}

	s_listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (s_listenSocket == INVALID_SOCKET)
	{
		app.DebugPrintf("socket() failed: %d\n", WSAGetLastError());
		freeaddrinfo(result);
		return false;
	}

	int noDelay = 1;
	setsockopt(s_listenSocket, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay));

	iResult = connect(s_listenSocket, result->ai_addr, (int)result->ai_addrlen);
	freeaddrinfo(result);
	if (iResult == SOCKET_ERROR)
	{
		app.DebugPrintf("connect() to Relay Server %s:%d failed: %d\n", relayIp.c_str(), relayPort, WSAGetLastError());
		closesocket(s_listenSocket);
		s_listenSocket = INVALID_SOCKET;
		return false;
	}

	extern char g_Win64Username[17];
	std::string req = "HOST " + std::string(g_Win64Username) + "\n";
	send(s_listenSocket, req.c_str(), (int)req.length(), 0);

	s_active = true;
	s_connected = true;

	s_acceptThread = CreateThread(NULL, 0, AcceptThreadProc, NULL, 0, NULL);

	app.DebugPrintf("Win64 LAN: Hosting via Cloud Relay at %s:%d (Session: %s)\n", relayIp.c_str(), relayPort, g_Win64Username);
	return true;
}

bool WinsockNetLayer::JoinGame(const char *ip, int port)
{
	// NOTE: 'ip' parameter acts as the Session ID (Owner ID) in the Relay Architecture!
	if (!s_initialized && !Initialize()) return false;

	s_isHost = false;
	s_hostSmallId = 0;

	struct addrinfo hints = {};
	struct addrinfo *result = NULL;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	std::string relayIp = s_customHostIp.empty() ? "127.0.0.1" : s_customHostIp;
	int relayPort = s_customHostPort == 0 ? 25565 : s_customHostPort;

	char portStr[16];
	sprintf_s(portStr, "%d", relayPort);

	int iResult = getaddrinfo(relayIp.c_str(), portStr, &hints, &result);
	if (iResult != 0)
	{
		app.DebugPrintf("getaddrinfo failed for Relay Server %s:%d - %d\n", relayIp.c_str(), relayPort, iResult);
		return false;
	}

	s_hostConnectionSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (s_hostConnectionSocket == INVALID_SOCKET)
	{
		app.DebugPrintf("socket() failed: %d\n", WSAGetLastError());
		freeaddrinfo(result);
		return false;
	}

	int noDelay = 1;
	setsockopt(s_hostConnectionSocket, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay));

	iResult = connect(s_hostConnectionSocket, result->ai_addr, (int)result->ai_addrlen);
	freeaddrinfo(result);
	if (iResult == SOCKET_ERROR)
	{
		app.DebugPrintf("connect() to Relay Server %s:%d failed: %d\n", relayIp.c_str(), relayPort, WSAGetLastError());
		closesocket(s_hostConnectionSocket);
		s_hostConnectionSocket = INVALID_SOCKET;
		return false;
	}

	std::string hostname(ip);
	std::string req = "JOIN " + hostname + "\n";
	send(s_hostConnectionSocket, req.c_str(), (int)req.length(), 0);

	BYTE assignBuf[1];
	int bytesRecv = recv(s_hostConnectionSocket, (char *)assignBuf, 1, 0);
	if (bytesRecv != 1)
	{
		app.DebugPrintf("Failed to receive small ID assignment from host\n");
		closesocket(s_hostConnectionSocket);
		s_hostConnectionSocket = INVALID_SOCKET;
		return false;
	}
	s_localSmallId = assignBuf[0];

	app.DebugPrintf("Win64 LAN: Connected via Relay to Session %s, assigned smallId=%d\n", ip, s_localSmallId);

	s_active = true;
	s_connected = true;

	s_clientRecvThread = CreateThread(NULL, 0, ClientRecvThreadProc, NULL, 0, NULL);

	return true;
}

bool WinsockNetLayer::SendOnSocket(SOCKET sock, const void *data, int dataSize)
{
	if (sock == INVALID_SOCKET || dataSize <= 0) return false;

	EnterCriticalSection(&s_sendLock);

	BYTE header[4];
	header[0] = (BYTE)((dataSize >> 24) & 0xFF);
	header[1] = (BYTE)((dataSize >> 16) & 0xFF);
	header[2] = (BYTE)((dataSize >> 8) & 0xFF);
	header[3] = (BYTE)(dataSize & 0xFF);

	int totalSent = 0;
	int toSend = 4;
	while (totalSent < toSend)
	{
		int sent = send(sock, (const char *)header + totalSent, toSend - totalSent, 0);
		if (sent == SOCKET_ERROR || sent == 0)
		{
			LeaveCriticalSection(&s_sendLock);
			return false;
		}
		totalSent += sent;
	}

	totalSent = 0;
	while (totalSent < dataSize)
	{
		int sent = send(sock, (const char *)data + totalSent, dataSize - totalSent, 0);
		if (sent == SOCKET_ERROR || sent == 0)
		{
			LeaveCriticalSection(&s_sendLock);
			return false;
		}
		totalSent += sent;
	}

	LeaveCriticalSection(&s_sendLock);
	return true;
}

bool WinsockNetLayer::SendToSmallId(BYTE targetSmallId, const void *data, int dataSize)
{
	if (!s_active) return false;

	if (s_isHost)
	{
		SOCKET sock = GetSocketForSmallId(targetSmallId);
		if (sock == INVALID_SOCKET) return false;
		return SendOnSocket(sock, data, dataSize);
	}
	else
	{
		return SendOnSocket(s_hostConnectionSocket, data, dataSize);
	}
}

SOCKET WinsockNetLayer::GetSocketForSmallId(BYTE smallId)
{
	EnterCriticalSection(&s_connectionsLock);
	for (size_t i = 0; i < s_connections.size(); i++)
	{
		if (s_connections[i].smallId == smallId && s_connections[i].active)
		{
			SOCKET sock = s_connections[i].tcpSocket;
			LeaveCriticalSection(&s_connectionsLock);
			return sock;
		}
	}
	LeaveCriticalSection(&s_connectionsLock);
	return INVALID_SOCKET;
}

static bool RecvExact(SOCKET sock, BYTE *buf, int len)
{
	int totalRecv = 0;
	while (totalRecv < len)
	{
		int r = recv(sock, (char *)buf + totalRecv, len - totalRecv, 0);
		if (r <= 0) return false;
		totalRecv += r;
	}
	return true;
}

void WinsockNetLayer::HandleDataReceived(BYTE fromSmallId, BYTE toSmallId, unsigned char *data, unsigned int dataSize)
{
	INetworkPlayer *pPlayerFrom = g_NetworkManager.GetPlayerBySmallId(fromSmallId);
	INetworkPlayer *pPlayerTo = g_NetworkManager.GetPlayerBySmallId(toSmallId);

	if (pPlayerFrom == NULL || pPlayerTo == NULL) return;

	if (s_isHost)
	{
		::Socket *pSocket = pPlayerFrom->GetSocket();
		if (pSocket != NULL)
			pSocket->pushDataToQueue(data, dataSize, false);
	}
	else
	{
		::Socket *pSocket = pPlayerTo->GetSocket();
		if (pSocket != NULL)
			pSocket->pushDataToQueue(data, dataSize, true);
	}
}

DWORD WINAPI WinsockNetLayer::AcceptThreadProc(LPVOID param)
{
	char buf[256];
	while (s_active)
	{
		int ret = recv(s_listenSocket, buf, sizeof(buf)-1, 0);
		if (ret <= 0)
		{
			if (s_active)
				app.DebugPrintf("Relay Server control socket disconnected.\n");
			break;
		}
		buf[ret] = 0;
		
		std::string str(buf);
		size_t pos = 0;
		while ((pos = str.find('\n')) != std::string::npos)
		{
			std::string line = str.substr(0, pos);
			str.erase(0, pos + 1);

			if (line.find("CLIENT ") == 0)
			{
				std::string clientId = line.substr(7);
				
				struct addrinfo hints = {};
				struct addrinfo *result = NULL;

				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_protocol = IPPROTO_TCP;

				std::string relayIp = s_customHostIp.empty() ? "127.0.0.1" : s_customHostIp;
				int relayPort = s_customHostPort == 0 ? 25565 : s_customHostPort;
				char portStr[16];
				sprintf_s(portStr, "%d", relayPort);

				int iResult = getaddrinfo(relayIp.c_str(), portStr, &hints, &result);
				if (iResult != 0) continue;

				SOCKET clientSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
				if (clientSocket == INVALID_SOCKET) { freeaddrinfo(result); continue; }

				int noDelay = 1;
				setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay));

				iResult = connect(clientSocket, result->ai_addr, (int)result->ai_addrlen);
				freeaddrinfo(result);
				if (iResult == SOCKET_ERROR)
				{
					closesocket(clientSocket);
					continue;
				}
				
				extern char g_Win64Username[17];
				std::string acc = "ACCEPT " + std::string(g_Win64Username) + " " + clientId + "\n";
				send(clientSocket, acc.c_str(), (int)acc.length(), 0);

				extern QNET_STATE _iQNetStubState;
				if (_iQNetStubState != QNET_STATE_GAME_PLAY && _iQNetStubState != QNET_STATE_SESSION_HOSTING && _iQNetStubState != QNET_STATE_SESSION_STARTING)
				{
					app.DebugPrintf("Win64 LAN: Rejecting connection, game not ready\n");
					closesocket(clientSocket);
					continue;
				}

				BYTE assignedSmallId;
				EnterCriticalSection(&s_freeSmallIdLock);
				if (!s_freeSmallIds.empty())
				{
					assignedSmallId = s_freeSmallIds.back();
					s_freeSmallIds.pop_back();
				}
				else if (s_nextSmallId < MINECRAFT_NET_MAX_PLAYERS)
				{
					assignedSmallId = s_nextSmallId++;
				}
				else
				{
					LeaveCriticalSection(&s_freeSmallIdLock);
					app.DebugPrintf("Win64 LAN: Server full, rejecting connection\n");
					closesocket(clientSocket);
					continue;
				}
				LeaveCriticalSection(&s_freeSmallIdLock);

				BYTE assignBuf[1] = { assignedSmallId };
				int sent = send(clientSocket, (const char *)assignBuf, 1, 0);
				if (sent != 1)
				{
					closesocket(clientSocket);
					continue;
				}

				Win64RemoteConnection conn;
				conn.tcpSocket = clientSocket;
				conn.smallId = assignedSmallId;
				conn.active = true;
				conn.recvThread = NULL;

				EnterCriticalSection(&s_connectionsLock);
				s_connections.push_back(conn);
				int connIdx = (int)s_connections.size() - 1;
				LeaveCriticalSection(&s_connectionsLock);

				app.DebugPrintf("Win64 LAN: Client connected via Relay, assigned smallId=%d\n", assignedSmallId);

				IQNetPlayer *qnetPlayer = &IQNet::m_player[assignedSmallId];

				extern void Win64_SetupRemoteQNetPlayer(IQNetPlayer *player, BYTE smallId, bool isHost, bool isLocal);
				Win64_SetupRemoteQNetPlayer(qnetPlayer, assignedSmallId, false, false);

				extern CPlatformNetworkManagerStub *g_pPlatformNetworkManager;
				g_pPlatformNetworkManager->NotifyPlayerJoined(qnetPlayer);

				DWORD *threadParam = new DWORD;
				*threadParam = connIdx;
				HANDLE hThread = CreateThread(NULL, 0, RecvThreadProc, threadParam, 0, NULL);

				EnterCriticalSection(&s_connectionsLock);
				if (connIdx < (int)s_connections.size())
					s_connections[connIdx].recvThread = hThread;
				LeaveCriticalSection(&s_connectionsLock);
			}
		}
	}
	return 0;
}

DWORD WINAPI WinsockNetLayer::RecvThreadProc(LPVOID param)
{
	DWORD connIdx = *(DWORD *)param;
	delete (DWORD *)param;

	EnterCriticalSection(&s_connectionsLock);
	if (connIdx >= (DWORD)s_connections.size())
	{
		LeaveCriticalSection(&s_connectionsLock);
		return 0;
	}
	SOCKET sock = s_connections[connIdx].tcpSocket;
	BYTE clientSmallId = s_connections[connIdx].smallId;
	LeaveCriticalSection(&s_connectionsLock);

	BYTE *recvBuf = new BYTE[WIN64_NET_RECV_BUFFER_SIZE];

	while (s_active)
	{
		BYTE header[4];
		if (!RecvExact(sock, header, 4))
		{
			app.DebugPrintf("Win64 LAN: Client smallId=%d disconnected (header)\n", clientSmallId);
			break;
		}

		int packetSize = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];

		if (packetSize <= 0 || packetSize > WIN64_NET_RECV_BUFFER_SIZE)
		{
			app.DebugPrintf("Win64 LAN: Invalid packet size %d from client smallId=%d\n", packetSize, clientSmallId);
			break;
		}

		if (!RecvExact(sock, recvBuf, packetSize))
		{
			app.DebugPrintf("Win64 LAN: Client smallId=%d disconnected (body)\n", clientSmallId);
			break;
		}

		HandleDataReceived(clientSmallId, s_hostSmallId, recvBuf, packetSize);
	}

	delete[] recvBuf;

	EnterCriticalSection(&s_connectionsLock);
	for (size_t i = 0; i < s_connections.size(); i++)
	{
		if (s_connections[i].smallId == clientSmallId)
		{
			s_connections[i].active = false;
			closesocket(s_connections[i].tcpSocket);
			s_connections[i].tcpSocket = INVALID_SOCKET;
			break;
		}
	}
	LeaveCriticalSection(&s_connectionsLock);

	EnterCriticalSection(&s_disconnectLock);
	s_disconnectedSmallIds.push_back(clientSmallId);
	LeaveCriticalSection(&s_disconnectLock);

	return 0;
}

bool WinsockNetLayer::PopDisconnectedSmallId(BYTE *outSmallId)
{
	bool found = false;
	EnterCriticalSection(&s_disconnectLock);
	if (!s_disconnectedSmallIds.empty())
	{
		*outSmallId = s_disconnectedSmallIds.back();
		s_disconnectedSmallIds.pop_back();
		found = true;
	}
	LeaveCriticalSection(&s_disconnectLock);
	return found;
}

void WinsockNetLayer::PushFreeSmallId(BYTE smallId)
{
	EnterCriticalSection(&s_freeSmallIdLock);
	s_freeSmallIds.push_back(smallId);
	LeaveCriticalSection(&s_freeSmallIdLock);
}

DWORD WINAPI WinsockNetLayer::ClientRecvThreadProc(LPVOID param)
{
	BYTE *recvBuf = new BYTE[WIN64_NET_RECV_BUFFER_SIZE];

	while (s_active && s_hostConnectionSocket != INVALID_SOCKET)
	{
		BYTE header[4];
		if (!RecvExact(s_hostConnectionSocket, header, 4))
		{
			app.DebugPrintf("Win64 LAN: Disconnected from host (header)\n");
			break;
		}

		int packetSize = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];

		if (packetSize <= 0 || packetSize > WIN64_NET_RECV_BUFFER_SIZE)
		{
			app.DebugPrintf("Win64 LAN: Invalid packet size %d from host\n", packetSize);
			break;
		}

		if (!RecvExact(s_hostConnectionSocket, recvBuf, packetSize))
		{
			app.DebugPrintf("Win64 LAN: Disconnected from host (body)\n");
			break;
		}

		HandleDataReceived(s_hostSmallId, s_localSmallId, recvBuf, packetSize);
	}

	delete[] recvBuf;

	return 0;
}

bool WinsockNetLayer::StartAdvertising(int gamePort, const wchar_t *hostName, unsigned int gameSettings, unsigned int texPackId, unsigned char subTexId, unsigned short netVer)
{
	extern bool g_IsOfflineMode;
	if (g_IsOfflineMode) return true; // Don't advertise if offline

	if (s_advertising) return true;
	if (!s_initialized) return false;

	EnterCriticalSection(&s_advertiseLock);
	memset(&s_advertiseData, 0, sizeof(s_advertiseData));
	s_advertiseData.magic = WIN64_LAN_BROADCAST_MAGIC;
	s_advertiseData.netVersion = netVer;
	s_advertiseData.gamePort = (WORD)gamePort;
	wcsncpy_s(s_advertiseData.hostName, 32, hostName, _TRUNCATE);
	s_advertiseData.playerCount = 1;
	s_advertiseData.maxPlayers = MINECRAFT_NET_MAX_PLAYERS;
	s_advertiseData.gameHostSettings = gameSettings;
	s_advertiseData.texturePackParentId = texPackId;
	s_advertiseData.subTexturePackId = subTexId;
	s_advertiseData.isJoinable = 0;
	s_hostGamePort = gamePort;
	LeaveCriticalSection(&s_advertiseLock);

	s_advertising = true;
	s_advertiseThread = CreateThread(NULL, 0, AdvertiseThreadProc, NULL, 0, NULL);

	app.DebugPrintf("Win64 LAN: Started advertising on Supabase\n");
	return true;
}

void WinsockNetLayer::StopAdvertising()
{
	if (!s_advertising) return;
	s_advertising = false;

	if (s_advertiseThread != NULL)
	{
		WaitForSingleObject(s_advertiseThread, 2000);
		CloseHandle(s_advertiseThread);
		s_advertiseThread = NULL;
	}

	// Send synchronous STOP to Supabase
	if (!s_accessToken.empty() && !s_userId.empty())
	{
		std::vector<std::wstring> headers;
		headers.push_back(L"apikey: eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVwbXdnb2t3Zmt4aHF1aXJzeW1xIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzI1MDU1NTMsImV4cCI6MjA4ODA4MTU1M30.xTBRQy0wCEFzlnLObQINtUay3SGJwTCrZ9mOKC24cy4");
		std::wstring wtoken(s_accessToken.begin(), s_accessToken.end());
		headers.push_back(L"Authorization: Bearer " + wtoken);
		headers.push_back(L"Content-Type: application/json");

		nlohmann::json body = {
			{"is_online", false}
		};

		std::string query_str = "/rest/v1/servers?owner_id=eq." + s_userId;
		std::wstring query(query_str.begin(), query_str.end());
		DoWinHttpRequest(L"epmwgokwfkxhquirsymq.supabase.co", query, L"PATCH", body.dump(), headers);
	}
}

void WinsockNetLayer::UpdateAdvertisePlayerCount(BYTE count)
{
	EnterCriticalSection(&s_advertiseLock);
	s_advertiseData.playerCount = count;
	LeaveCriticalSection(&s_advertiseLock);

	// Player count is tracked by the relay server automatically
}

void WinsockNetLayer::UpdateAdvertiseJoinable(bool joinable)
{
	EnterCriticalSection(&s_advertiseLock);
	s_advertiseData.isJoinable = joinable ? 1 : 0;
	LeaveCriticalSection(&s_advertiseLock);
}

DWORD WINAPI WinsockNetLayer::AdvertiseThreadProc(LPVOID param)
{
	// No-op: The relay server automatically tracks hosts via the control socket.
	// When the HOST control socket closes, the relay removes the session.
	while (s_advertising)
	{
		Sleep(5000);
	}
	return 0;
}

bool WinsockNetLayer::StartDiscovery()
{
	if (s_discovering) return true;
	if (!s_initialized) return false;

	s_discovering = true;
	s_discoveryThread = CreateThread(NULL, 0, DiscoveryThreadProc, NULL, 0, NULL);

	app.DebugPrintf("Win64 LAN: Started discovering servers via Relay\n");
	return true;
}

void WinsockNetLayer::StopDiscovery()
{
	if (!s_discovering) return;
	s_discovering = false;

	if (s_discoveryThread != NULL)
	{
		WaitForSingleObject(s_discoveryThread, 2000);
		CloseHandle(s_discoveryThread);
		s_discoveryThread = NULL;
	}

	EnterCriticalSection(&s_discoveryLock);
	s_discoveredSessions.clear();
	LeaveCriticalSection(&s_discoveryLock);
}

std::vector<Win64LANSession> WinsockNetLayer::GetDiscoveredSessions()
{
	std::vector<Win64LANSession> result;
	EnterCriticalSection(&s_discoveryLock);
	result = s_discoveredSessions;
	LeaveCriticalSection(&s_discoveryLock);
	return result;
}

DWORD WINAPI WinsockNetLayer::DiscoveryThreadProc(LPVOID param)
{
	while (s_discovering)
	{
		// Connect to relay and send LIST command
		struct addrinfo hints = {};
		struct addrinfo *result = NULL;

		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		std::string relayIp = s_customHostIp.empty() ? "127.0.0.1" : s_customHostIp;
		int relayPort = s_customHostPort == 0 ? 25565 : s_customHostPort;
		char portStr[16];
		sprintf_s(portStr, "%d", relayPort);

		int iResult = getaddrinfo(relayIp.c_str(), portStr, &hints, &result);
		if (iResult != 0)
		{
			Sleep(5000);
			continue;
		}

		SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
		if (sock == INVALID_SOCKET) { freeaddrinfo(result); Sleep(5000); continue; }

		iResult = connect(sock, result->ai_addr, (int)result->ai_addrlen);
		freeaddrinfo(result);
		if (iResult == SOCKET_ERROR)
		{
			closesocket(sock);
			Sleep(5000);
			continue;
		}

		// Send LIST command
		const char* listCmd = "LIST\n";
		send(sock, listCmd, (int)strlen(listCmd), 0);

		// Receive JSON response
		char recvBuf[4096];
		std::string jsonStr;
		while (true)
		{
			int ret = recv(sock, recvBuf, sizeof(recvBuf) - 1, 0);
			if (ret <= 0) break;
			recvBuf[ret] = 0;
			jsonStr += recvBuf;
			if (jsonStr.find('\n') != std::string::npos) break;
		}
		closesocket(sock);

		// Trim trailing newline
		while (!jsonStr.empty() && (jsonStr.back() == '\n' || jsonStr.back() == '\r'))
			jsonStr.pop_back();

		if (!jsonStr.empty())
		{
			try
			{
				auto json_array = nlohmann::json::parse(jsonStr);
				DWORD now = GetTickCount();

				EnterCriticalSection(&s_discoveryLock);
				s_discoveredSessions.clear();

				for (auto& el : json_array)
				{
					std::string sessionId = el["sessionId"];
					std::string hostName = el["hostName"];
					int currPlayers = el["playerCount"];
					int maxPlayers = el["maxPlayers"];

					Win64LANSession session = {};
					strncpy_s(session.hostIP, sizeof(session.hostIP), sessionId.c_str(), _TRUNCATE);
					session.hostPort = relayPort;

					MultiByteToWideChar(CP_UTF8, 0, hostName.c_str(), -1, session.hostName, 32);

					session.playerCount = currPlayers;
					session.maxPlayers = maxPlayers;
					session.isJoinable = true;
					session.lastSeenTick = now;
					session.netVersion = MINECRAFT_NET_VERSION;
					session.gameHostSettings = 0;
					session.texturePackParentId = 0;
					session.subTexturePackId = 0;

					s_discoveredSessions.push_back(session);

					app.DebugPrintf("Win64 ONLINE: Discovered game \"%s\" (session: %s, %d/%d players)\n",
						hostName.c_str(), sessionId.c_str(), currPlayers, maxPlayers);
				}

				LeaveCriticalSection(&s_discoveryLock);
			}
			catch (const std::exception& e)
			{
				app.DebugPrintf("Win64 ONLINE: Failed to parse discovery JSON: %s\n", e.what());
			}
		}

		Sleep(5000); // 5 sec interval
	}

	return 0;
}

#endif
