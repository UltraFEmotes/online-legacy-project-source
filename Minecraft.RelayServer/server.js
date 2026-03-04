const net = require('net');

const PORT = process.env.PORT || 25565;

// store by sessionId (username) -> { controlSocket, hostName, playerCount, maxPlayers }
const hosts = new Map();

// store waiting joiners by sessionId -> Map(joinerId -> joinerSocket)
const waitingJoiners = new Map();

const server = net.createServer((socket) => {
    socket.on('error', (err) => {
        // Suppress unhandled ECONNRESET for sockets before they get registered
    });

    socket.once('data', (data) => {
        const firstLineEnd = data.indexOf(10); // Find \n byte
        const firstLineBuf = firstLineEnd !== -1 ? data.slice(0, firstLineEnd) : data;
        const firstLine = firstLineBuf.toString('utf8').trim();
        const parts = firstLine.split(' ');

        if (parts[0] === 'HOST' && parts.length >= 2) {
            const sessionId = parts[1]; // This is the username
            const hostName = parts.length >= 3 ? parts.slice(2).join(' ') : sessionId;

            console.log(`[+] HOST registered: "${hostName}" (session: ${sessionId})`);

            hosts.set(sessionId, {
                controlSocket: socket,
                hostName: hostName,
                playerCount: 1,
                maxPlayers: 8
            });
            waitingJoiners.set(sessionId, new Map());

            socket.on('close', () => {
                console.log(`[-] HOST disconnected: "${hostName}" (session: ${sessionId})`);
                hosts.delete(sessionId);
                waitingJoiners.delete(sessionId);
            });
            socket.on('error', (err) => {
                console.log(`[!] HOST error: "${hostName}" (session: ${sessionId})`, err.message);
                hosts.delete(sessionId);
                waitingJoiners.delete(sessionId);
            });
        }
        else if (parts[0] === 'LIST') {
            // Return JSON array of all active hosted sessions
            const sessions = [];
            for (const [sessionId, hostInfo] of hosts) {
                sessions.push({
                    sessionId: sessionId,
                    hostName: hostInfo.hostName,
                    playerCount: hostInfo.playerCount,
                    maxPlayers: hostInfo.maxPlayers
                });
            }
            const json = JSON.stringify(sessions);
            socket.end(json + '\n');
            console.log(`[?] LIST request served (${sessions.length} sessions)`);
        }
        else if (parts[0] === 'JOIN' && parts.length >= 2) {
            const sessionId = parts[1];
            if (!hosts.has(sessionId)) {
                console.log(`[!] JOIN failed, no such session: ${sessionId}`);
                socket.end('ERROR NO_SESSION\n');
                return;
            }

            const joinerId = Math.random().toString(36).substring(2, 10);
            console.log(`[*] JOIN incoming for session: ${sessionId} (Joiner ID: ${joinerId})`);

            waitingJoiners.get(sessionId).set(joinerId, socket);

            // Tell the host a new client wants to join
            const hostInfo = hosts.get(sessionId);
            hostInfo.controlSocket.write(`CLIENT ${joinerId}\n`);

            socket.on('close', () => {
                const sessionWaiters = waitingJoiners.get(sessionId);
                if (sessionWaiters) sessionWaiters.delete(joinerId);
            });
            socket.on('error', () => {
                const sessionWaiters = waitingJoiners.get(sessionId);
                if (sessionWaiters) sessionWaiters.delete(joinerId);
            });
        }
        else if (parts[0] === 'ACCEPT' && parts.length >= 3) {
            const sessionId = parts[1];
            const joinerId = parts[2];
            console.log(`[>] ACCEPT from Host: ${sessionId} (Joiner ID: ${joinerId})`);

            const sessionWaiters = waitingJoiners.get(sessionId);
            if (!sessionWaiters) {
                console.log(`[!] ACCEPT failed, session not found: ${sessionId}`);
                socket.end();
                return;
            }

            const joinerSocket = sessionWaiters.get(joinerId);
            if (!joinerSocket) {
                console.log(`[!] ACCEPT failed, joiner disconnected: ${joinerId}`);
                socket.end();
                return;
            }

            // Remove from waiting queue
            sessionWaiters.delete(joinerId);

            // Update player count
            const hostInfo = hosts.get(sessionId);
            if (hostInfo) hostInfo.playerCount++;

            console.log(`[<->] Bridging connection for ${joinerId}!`);

            // If there was any trailing data in the ACCEPT packet, forward it
            if (firstLineEnd !== -1 && data.length > firstLineEnd + 1) {
                const remaining = data.slice(firstLineEnd + 1);
                joinerSocket.write(remaining);
            }

            // Bridge
            socket.pipe(joinerSocket);
            joinerSocket.pipe(socket);

            socket.on('error', () => joinerSocket.destroy());
            joinerSocket.on('error', () => socket.destroy());
            socket.on('close', () => {
                joinerSocket.end();
                if (hostInfo) hostInfo.playerCount = Math.max(1, hostInfo.playerCount - 1);
            });
            joinerSocket.on('close', () => {
                socket.end();
                if (hostInfo) hostInfo.playerCount = Math.max(1, hostInfo.playerCount - 1);
            });
        }
        else {
            console.log(`[!] Unknown command: ${parts[0]}`);
            socket.end('ERROR INVALID_COMMAND\n');
        }
    });
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`[+] Relay Server listening on 0.0.0.0:${PORT}`);
    console.log(`[i] Commands: HOST <username>, LIST, JOIN <username>, ACCEPT <session> <joiner>`);
});
