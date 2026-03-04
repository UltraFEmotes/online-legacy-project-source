const net = require('net');
const http = require('http');

const PORT = process.env.PORT || 25565;
const DASHBOARD_PORT = process.env.DASHBOARD_PORT || 8080;

// store by sessionId (username) -> { controlSocket, hostName, maxPlayers, activeClients: Map(clientId -> socket) }
const hosts = new Map();

// store waiting joiners by sessionId -> Map(joinerId -> { socket, username })
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
                maxPlayers: 8,
                activeClients: new Map() // JoinerId -> socket
            });
            waitingJoiners.set(sessionId, new Map());

            socket.on('close', () => {
                console.log(`[-] HOST disconnected: "${hostName}" (session: ${sessionId})`);

                // Disconnect all connected clients
                const hostInfo = hosts.get(sessionId);
                if (hostInfo) {
                    for (const [clientId, clientSocket] of hostInfo.activeClients) {
                        clientSocket.destroy();
                    }
                }

                hosts.delete(sessionId);
                waitingJoiners.delete(sessionId);
            });
            socket.on('error', (err) => {
                console.log(`[!] HOST error: "${hostName}" (session: ${sessionId})`, err.message);

                const hostInfo = hosts.get(sessionId);
                if (hostInfo) {
                    for (const [clientId, clientSocket] of hostInfo.activeClients) {
                        clientSocket.destroy();
                    }
                }

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
                    playerCount: hostInfo.activeClients.size + 1, // +1 for the host
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

            const joinerUsername = parts.length >= 3 ? parts[2] : "Unknown";
            const joinerId = joinerUsername + "_" + Math.random().toString(36).substring(2, 6);

            console.log(`[*] JOIN incoming for session: ${sessionId} (Joiner ID: ${joinerId})`);

            waitingJoiners.get(sessionId).set(joinerId, { socket: socket, username: joinerUsername });

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

            const joinerData = sessionWaiters.get(joinerId);
            if (!joinerData) {
                console.log(`[!] ACCEPT failed, joiner disconnected: ${joinerId}`);
                socket.end();
                return;
            }

            const joinerSocket = joinerData.socket;

            // Remove from waiting queue
            sessionWaiters.delete(joinerId);

            // Add to active clients map
            const hostInfo = hosts.get(sessionId);
            if (hostInfo) {
                // Keep track of this socket so we can kick it from the dashboard
                hostInfo.activeClients.set(joinerId, joinerSocket);
            } else {
                joinerSocket.destroy();
                socket.destroy();
                return;
            }

            console.log(`[<->] Bridging connection for ${joinerId}!`);

            // If there was any trailing data in the ACCEPT packet, forward it
            if (firstLineEnd !== -1 && data.length > firstLineEnd + 1) {
                const remaining = data.slice(firstLineEnd + 1);
                joinerSocket.write(remaining);
            }

            // Bridge
            socket.pipe(joinerSocket);
            joinerSocket.pipe(socket);

            const cleanup = () => {
                if (!joinerSocket.destroyed) joinerSocket.destroy();
                if (!socket.destroyed) socket.destroy();
                if (hostInfo) hostInfo.activeClients.delete(joinerId);
            };

            socket.on('error', cleanup);
            joinerSocket.on('error', cleanup);
            socket.on('close', cleanup);
            joinerSocket.on('close', cleanup);
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

// ============================================
// HTTP Web Dashboard
// ============================================

const dashboardHtml = `
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Minecraft Relay Dashboard</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; }
        .container { max-width: 1000px; margin: 0 auto; }
        h1 { border-bottom: 2px solid #334155; padding-bottom: 10px; color: #38bdf8; }
        .server-card { background: #1e293b; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1); border-left: 4px solid #10b981; }
        .server-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }
        .server-title { font-size: 1.25rem; font-weight: bold; }
        .badge { background: #334155; padding: 4px 8px; border-radius: 4px; font-size: 0.875rem; }
        .btn { background: #ef4444; color: white; border: none; padding: 6px 12px; border-radius: 4px; cursor: pointer; font-size: 0.875rem; transition: background 0.2s; }
        .btn:hover { background: #dc2626; }
        .btn-stop { background: #f59e0b; }
        .btn-stop:hover { background: #d97706; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { text-align: left; padding: 10px; border-bottom: 1px solid #334155; }
        th { color: #94a3b8; font-weight: 500; }
        tr:last-child td { border-bottom: none; }
        .empty-state { color: #94a3b8; font-style: italic; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎮 Relay Server Dashboard</h1>
        <div id="servers">Loading...</div>
    </div>

    <script>
        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                renderServers(data);
            } catch (err) {
                document.getElementById('servers').innerHTML = '<p class="empty-state">Error loading server data.</p>';
            }
        }

        async function kickPlayer(sessionId, clientId) {
            if (!confirm('Kick player ' + clientId + '?')) return;
            await fetch('/api/kick', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sessionId, clientId })
            });
            fetchStatus();
        }

        async function stopServer(sessionId) {
            if (!confirm('Stop server ' + sessionId + ' and disconnect everyone?')) return;
            await fetch('/api/stop', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sessionId })
            });
            fetchStatus();
        }

        function renderServers(servers) {
            const container = document.getElementById('servers');
            if (servers.length === 0) {
                container.innerHTML = '<p class="empty-state">No servers currently active.</p>';
                return;
            }

            let html = '';
            for (const srv of servers) {
                html += \`
                <div class="server-card">
                    <div class="server-header">
                        <div>
                            <span class="server-title">\${srv.hostName}</span>
                            <span class="badge" style="margin-left: 10px;">Session: \${srv.sessionId}</span>
                        </div>
                        <div>
                            <span class="badge" style="margin-right: 10px;">Players: \${srv.clients.length + 1} / \${srv.maxPlayers}</span>
                            <button class="btn btn-stop" onclick="stopServer('\${srv.sessionId}')">Stop Server</button>
                        </div>
                    </div>
                    
                    <table>
                        <thead>
                            <tr>
                                <th>Player Type</th>
                                <th>Name / Connection ID</th>
                                <th>Action</th>
                            </tr>
                        </thead>
                        <tbody>
                            <tr>
                                <td><span style="color: #10b981; font-weight: bold;">Host</span></td>
                                <td>\${srv.sessionId}</td>
                                <td>-</td>
                            </tr>
                \`;

                for (const client of srv.clients) {
                    html += \`
                            <tr>
                                <td>Remote Player</td>
                                <td>\${client}</td>
                                <td><button class="btn" onclick="kickPlayer('\${srv.sessionId}', '\${client}')">Kick</button></td>
                            </tr>
                    \`;
                }

                html += \`
                        </tbody>
                    </table>
                </div>
                \`;
            }
            container.innerHTML = html;
        }

        fetchStatus();
        setInterval(fetchStatus, 3000); // Auto-refresh every 3s
    </script>
</body>
</html>
`;

const dashboard = http.createServer((req, res) => {
    // Basic Auth Check
    const auth = req.headers['authorization'];
    if (!auth) {
        res.setHeader('WWW-Authenticate', 'Basic realm="Admin Dashboard"');
        res.writeHead(401);
        res.end('Authentication required.');
        return;
    }

    const b64auth = (auth || '').split(' ')[1] || '';
    const [login, password] = Buffer.from(b64auth, 'base64').toString().split(':');

    if (login !== 'admin' || password !== 'admin') {
        res.setHeader('WWW-Authenticate', 'Basic realm="Admin Dashboard"');
        res.writeHead(401);
        res.end('Access denied.');
        return;
    }

    if (req.url === '/' && req.method === 'GET') {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(dashboardHtml);
    }
    else if (req.url === '/api/status' && req.method === 'GET') {
        const status = [];
        for (const [sessionId, hostInfo] of hosts) {
            status.push({
                sessionId: sessionId,
                hostName: hostInfo.hostName,
                maxPlayers: hostInfo.maxPlayers,
                clients: Array.from(hostInfo.activeClients.keys())
            });
        }
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(status));
    }
    else if (req.url === '/api/kick' && req.method === 'POST') {
        let body = '';
        req.on('data', chunk => body += chunk.toString());
        req.on('end', () => {
            try {
                const data = JSON.parse(body);
                const hostInfo = hosts.get(data.sessionId);
                if (hostInfo && hostInfo.activeClients.has(data.clientId)) {
                    hostInfo.activeClients.get(data.clientId).destroy();
                    hostInfo.activeClients.delete(data.clientId);
                    res.writeHead(200);
                    res.end('{"status": "kicked"}');
                } else {
                    res.writeHead(404);
                    res.end('{"error": "not found"}');
                }
            } catch (e) {
                res.writeHead(400); res.end();
            }
        });
    }
    else if (req.url === '/api/stop' && req.method === 'POST') {
        let body = '';
        req.on('data', chunk => body += chunk.toString());
        req.on('end', () => {
            try {
                const data = JSON.parse(body);
                const hostInfo = hosts.get(data.sessionId);
                if (hostInfo) {
                    hostInfo.controlSocket.destroy(); // Destroying the host control socket auto-disconnects all its clients via the close event handler
                    res.writeHead(200);
                    res.end('{"status": "stopped"}');
                } else {
                    res.writeHead(404);
                    res.end('{"error": "not found"}');
                }
            } catch (e) {
                res.writeHead(400); res.end();
            }
        });
    }
    else {
        res.writeHead(404);
        res.end('Not found');
    }
});

dashboard.listen(DASHBOARD_PORT, '0.0.0.0', () => {
    console.log(`[+] Dashboard Server listening on http://0.0.0.0:${DASHBOARD_PORT}`);
    console.log(`[i] Default Dashboard Login: admin / admin`);
});
