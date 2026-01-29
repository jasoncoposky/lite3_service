#pragma once

constexpr char dashboard_html[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Lite3 Service Dashboard</title>
    <style>
        :root {
            --bg-dark: #0f172a;
            --card-bg: #1e293b;
            --text-primary: #f8fafc;
            --text-secondary: #94a3b8;
            --accent-blue: #38bdf8;
            --accent-green: #4ade80;
            --accent-red: #f87171;
            --border: #334155;
        }
        body {
            font-family: 'Segoe UI', system-ui, sans-serif;
            background-color: var(--bg-dark);
            color: var(--text-primary);
            margin: 0;
            padding: 20px;
        }
        .container {
            max_width: 1200px;
            margin: 0 auto;
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 30px;
            padding-bottom: 20px;
            border-bottom: 1px solid var(--border);
        }
        .header h1 { margin: 0; font-weight: 300; }
        .status-dot {
            height: 12px;
            width: 12px;
            background-color: var(--accent-green);
            border-radius: 50%;
            display: inline-block;
            margin-right: 8px;
            box-shadow: 0 0 10px var(--accent-green);
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background-color: var(--card-bg);
            padding: 20px;
            border-radius: 12px;
            border: 1px solid var(--border);
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
        }
        .card h3 {
            margin: 0 0 10px 0;
            color: var(--text-secondary);
            font-size: 0.9rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }
        .metric-value {
            font-size: 2.5rem;
            font-weight: 600;
            color: var(--text-primary);
        }
        .metric-unit {
            font-size: 1rem;
            color: var(--text-secondary);
            margin-left: 5px;
        }
        .charts-container {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }
        canvas {
            width: 100%;
            height: 300px;
            background: var(--card-bg);
            border-radius: 12px;
            border: 1px solid var(--border);
            padding: 10px;
        }
        @media (max-width: 768px) {
            .charts-container { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Lite3 Service Monitor</h1>
            <div><span class="status-dot"></span>Online</div>
        </div>

        <!-- KPI Cards -->
        <div class="grid">
            <div class="card">
                <h3>Active Connections</h3>
                <div class="metric-value" id="conn-val">0</div>
            </div>
            <div class="card">
                <h3>Throughput (In)</h3>
                <div>
                    <span class="metric-value" id="rx-val">0</span>
                    <span class="metric-unit">B/s</span>
                </div>
            </div>
            <div class="card">
                <h3>Throughput (Out)</h3>
                <div>
                    <span class="metric-value" id="tx-val">0</span>
                    <span class="metric-unit">B/s</span>
                </div>
            </div>
            <div class="card">
                <h3>Thread Count</h3>
                <div class="metric-value" style="color: var(--accent-blue)" id="thread-val">0</div>
            </div>
            <div class="card">
                <h3>Errors (Rate)</h3>
                <div class="metric-value" style="color: var(--accent-red)" id="err-val">0</div>
            </div>
            <div class="card">
                <h3>Write Latency</h3>
                <div>
                    <span class="metric-value" style="color: var(--accent-green)" id="latency-val">0</span>
                    <span class="metric-unit">ms</span>
                </div>
            </div>
        </div>

        <!-- Replication Cards -->
        <h2 style="font-weight: 300; margin-bottom: 20px; border-bottom: 1px solid var(--border); padding-bottom: 10px;">Replication</h2>
        <div class="grid">
            <div class="card">
                <h3>Keys Repaired</h3>
                <div class="metric-value" style="color: var(--accent-green)" id="rep-keys-val">0</div>
            </div>
            <div class="card">
                <h3>Sync Events</h3>
                <div class="metric-value" style="color: var(--accent-blue)" id="sync-ops-val">0</div>
            </div>
            <div class="card">
                <h3>Mesh Traffic (In)</h3>
                <div>
                    <span class="metric-value" id="mesh-rx-val">0</span>
                    <span class="metric-unit">B/s</span>
                </div>
            </div>
            <div class="card">
                <h3>Mesh Traffic (Out)</h3>
                <div>
                    <span class="metric-value" id="mesh-tx-val">0</span>
                    <span class="metric-unit">B/s</span>
                </div>
            </div>
        </div>

        <!-- Charts -->
        <div class="charts-container">
            <div class="card">
                <h3>Network Traffic (B/s)</h3>
                <canvas id="trafficChart"></canvas>
            </div>
            <div class="card">
                <h3>Avg Write Latency (ms)</h3>
                <canvas id="latencyChart"></canvas>
            </div>
        </div>
    </div>

    <script>
        const SAMPLE_SIZE = 60;
        const POLL_INTERVAL = 1000;

        // Simple Chart Implementation (Zero Dependency)
        class LineChart {
            constructor(canvasId, color) {
                this.canvas = document.getElementById(canvasId);
                this.ctx = this.canvas.getContext('2d');
                this.data = new Array(SAMPLE_SIZE).fill(0);
                this.color = color;

                // Handle DPI
                const dpr = window.devicePixelRatio || 1;
                const rect = this.canvas.getBoundingClientRect();
                this.canvas.width = rect.width * dpr;
                this.canvas.height = rect.height * dpr;
                this.ctx.scale(dpr, dpr);
                this.width = rect.width;
                this.height = rect.height;
            }

            push(value) {
                this.data.shift();
                this.data.push(value);
                this.draw();
            }

            draw() {
                const ctx = this.ctx;
                const w = this.width;
                const h = this.height;
                const pad = 20;

                ctx.clearRect(0, 0, w, h);

                // Auto-scale
                const max = Math.max(...this.data, 0.001); // Min scale 1us for visibility

                // Draw Grid
                ctx.strokeStyle = '#334155';
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.moveTo(pad, h - pad);
                ctx.lineTo(w, h - pad); // X-axis
                ctx.stroke();

                // Draw Line
                ctx.strokeStyle = this.color;
                ctx.lineWidth = 2;
                ctx.lineJoin = 'round';
                ctx.beginPath();

                const step = (w - pad) / (SAMPLE_SIZE - 1);

                this.data.forEach((val, i) => {
                    const x = pad + (i * step);
                    const y = (h - pad) - ((val / max) * (h - 2 * pad));
                    if (i === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                });

                ctx.stroke();

                // Fill area
                ctx.fillStyle = this.color + '20'; // 20% opacity
                ctx.lineTo(w, h - pad);
                ctx.lineTo(pad, h - pad);
                ctx.fill();

                // Draw Max Label
                ctx.fillStyle = '#94a3b8';
                ctx.font = '10px monospace';
                ctx.fillText(max.toFixed(4), 0, 10);
            }
        }

        const trafficChart = new LineChart('trafficChart', '#38bdf8');
        const latencyChart = new LineChart('latencyChart', '#4ade80');

        let lastRx = 0;
        let lastTx = 0;
        let lastErr = 0;
        
        let lastMeshRx = 0;
        let lastMeshTx = 0;
        
        let firstRun = true;

        async function fetchMetrics() {
            try {
                const res = await fetch('/metrics');
                const data = await res.json();

                // Current totals
                const rx = data.throughput.bytes_received_total || 0;
                const tx = data.throughput.bytes_sent_total || 0;
                const err = (data.throughput.http_errors_4xx || 0) + (data.throughput.http_errors_5xx || 0);

                // Replication totals
                let meshRx = 0;
                let meshTx = 0;
                let syncOps = 0;
                let keysRepaired = 0;

                if (data.replication) {
                    keysRepaired = data.replication.keys_repaired || 0;
                    
                    if (data.replication.mesh_traffic) {
                        for (const lane in data.replication.mesh_traffic) {
                            meshRx += data.replication.mesh_traffic[lane].recv || 0;
                            meshTx += data.replication.mesh_traffic[lane].sent || 0;
                        }
                    }
                    
                    if (data.replication.sync_ops) {
                        for (const op in data.replication.sync_ops) {
                            syncOps += data.replication.sync_ops[op] || 0;
                        }
                    }
                }

                // Calculate deltas (rates per second)
                const rxRate = firstRun ? 0 : rx - lastRx;
                const txRate = firstRun ? 0 : tx - lastTx;
                const errRate = firstRun ? 0 : err - lastErr;
                
                const meshRxRate = firstRun ? 0 : meshRx - lastMeshRx;
                const meshTxRate = firstRun ? 0 : meshTx - lastMeshTx;

                lastRx = rx;
                lastTx = tx;
                lastErr = err;
                
                lastMeshRx = meshRx;
                lastMeshTx = meshTx;

                firstRun = false;

                // Update DOM - System
                document.getElementById('conn-val').innerText = data.system.active_connections || 0;
                document.getElementById('thread-val').innerText = data.system.thread_count || 0;
                document.getElementById('rx-val').innerText = rxRate.toLocaleString();
                document.getElementById('tx-val').innerText = txRate.toLocaleString();
                document.getElementById('err-val').innerText = errRate;

                // Update DOM - Replication
                document.getElementById('rep-keys-val').innerText = keysRepaired.toLocaleString();
                document.getElementById('sync-ops-val').innerText = syncOps.toLocaleString();
                document.getElementById('mesh-rx-val').innerText = meshRxRate.toLocaleString();
                document.getElementById('mesh-tx-val').innerText = meshTxRate.toLocaleString();

                // Update DOM - Latency
                const currentSetLat = (data.operations && data.operations.set) ? data.operations.set.avg_latency_s * 1000 : 0;
                document.getElementById('latency-val').innerText = currentSetLat.toFixed(4);

                // Update Charts
                trafficChart.push(rxRate + txRate);

                // Latency (Operations set)
                const setLat = (data.operations && data.operations.set) ? data.operations.set.avg_latency_s * 1000 : 0;
                latencyChart.push(setLat);

            } catch (e) {
                console.error("Fetch failed", e);
            }
        }

        setInterval(fetchMetrics, POLL_INTERVAL);
        fetchMetrics();
    </script>
</body>
</html>)HTML";
