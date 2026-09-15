const express = require('express');
const mqtt = require('mqtt');
const sqlite3 = require('sqlite3').verbose();
const cors = require('cors');

const app = express();
app.use(express.json());
app.use(cors());

// ============================================================================
// 1. DATABASE INITIALIZATION (SQLite3)
// ============================================================================
const db = new sqlite3.Database('./aquarium_database.db', (err) => {
    if (err) console.error('Database connection error:', err.message);
    else console.log('✅ Connected to SQLite Database: aquarium_database.db');
});

// Create Telemetry table if it doesn't exist
db.run(`CREATE TABLE IF NOT EXISTS telemetry (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    temperature REAL,
    chiller_status INTEGER,
    pump_status INTEGER,
    feed_count INTEGER
)`);

// ============================================================================
// 2. MQTT BROKER CONNECTION & AUTOMATIC DATA LOGGING
// ============================================================================
const MQTT_BROKER = "mqtt://broker.hivemq.com:1883";
const TOPIC_TELEMETRY = "beca/ai/telemetry";
const TOPIC_COMMAND = "beca/ai/command";

const mqttClient = mqtt.connect(MQTT_BROKER);

mqttClient.on('connect', () => {
    console.log('✅ Backend Server connected to MQTT Broker!');
    mqttClient.subscribe(TOPIC_TELEMETRY);
});

mqttClient.on('message', (topic, message) => {
    if (topic === TOPIC_TELEMETRY) {
        try {
            const data = JSON.parse(message.toString());
            console.log('📥 Insert record into Database:', data);

            const stmt = db.prepare(`INSERT INTO telemetry (temperature, chiller_status, pump_status, feed_count) VALUES (?, ?, ?, ?)`);
            stmt.run(data.temp, data.chiller ? 1 : 0, data.pump ? 1 : 0, data.feed_count);
            stmt.finalize();
        } catch (e) {
            console.error('⚠️ Error parsing MQTT JSON:', e.message);
        }
    }
});

// ============================================================================
// 3. REST API ENDPOINTS FOR WEBSITE DASHBOARD
// ============================================================================

// GET /api/history - Retrieve historical telemetry data from Database
app.get('/api/history', (req, res) => {
    db.all(`SELECT * FROM telemetry ORDER BY id DESC LIMIT 50`, [], (err, rows) => {
        if (err) return res.status(500).json({ error: err.message });
        res.json(rows.reverse()); // Reverse array so newest data is on the right
    });
});

// POST /api/command - Send control commands to ESP32-S3 via MQTT
app.post('/api/command', (req, res) => {
    const { device, action } = req.body;
    const payload = JSON.stringify({ device, action });
    mqttClient.publish(TOPIC_COMMAND, payload);
    res.json({ success: true, command: payload });
});

const PORT = 3000;
app.listen(PORT, () => {
    console.log(`🚀 Backend Server running at: http://localhost:${PORT}`);
});