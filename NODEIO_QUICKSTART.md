# NodeIO Quick Start Guide

## 5-Minute Setup

### 1. Understanding NodeIO
NodeIO is FloraLink's IoT communication system that connects sensor nodes to the ESP32-C3 hub via WebSocket. Think of it as a "smart home nerve center" that manages multiple sensor devices.

### 2. Start the Hub
```powershell
# Build and flash the ESP32-C3 hub
idf.py build
idf.py -p COM9 flash monitor

# Look for these log messages:
# [nodeio] NodeIO initialized
# [websockserver] WebSocket server started on /ws
```

### 3. Test with Simulated Nodes
```powershell
# Run test client with 2 nodes, 5-second intervals
cd test
python testnode.py ws://192.168.1.100/ws 5 2

# You should see:
# Connected node 0 to ws://192.168.1.100/ws
# Connected node 1 to ws://192.168.1.100/ws
# Node 0: Sending sensor data...
```

### 4. Monitor in Web UI
```
http://192.168.1.100/nodes
```
You'll see your connected nodes with real-time sensor data!

## Basic Node Connection Flow

### 1. Node Connects
```json
{
    "magic": 3203391147,
    "type": "connect",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "controller": 2,
    "sw_version": "1.0.0",
    "sensors": ["temperature", "humidity"],
    "services": ["diagnostics"]
}
```

### 2. Hub Responds
```json
{
    "type": "connect_response",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "status": "accepted"
}
```

### 3. Hub Sends Subscription
```json
{
    "type": "notify_subscription",
    "node_id": 1,
    "payload": {
        "status": "subscribed",
        "interval": 5000,
        "filter": {
            "sensors": ["temperature", "humidity"],
            "services": ["diagnostics"]
        }
    }
}
```

### 4. Node Sends Data
```json
{
    "magic": 3203391147,
    "type": "node_data",
    "node_id": 1,
    "seq_num": 10,
    "timestamp": 1733338444,
    "payload": [
        {
            "type": "sensor",
            "sensor": {
                "temperature": 22.7,
                "humidity": 45.1
            }
        },
        {
            "type": "diagnostics",
            "diagnostics": {
                "uptime_sec": 864,
                "free_heap": 140000,
                "rssi": -56,
                "error_code": 0
            }
        }
    ]
}
```

## Interactive Test Client

### Basic Commands
```powershell
python testnode.py ws://hub-ip/ws 2 3   # 2s interval, 3 nodes

# In the client, type 'cmd' then Enter:
cmd
> add 4-6        # Add nodes 4, 5, 6
> remove 1       # Remove node 1  
> pause 2        # Pause node 2
> resume 2       # Resume node 2
> status 0       # Check node 0 status
> list           # List all nodes
> done           # Exit command mode
```

### Monitor Logs
```
[nodeio] Node 0 connected with capabilities: temp,humidity,diagnostics
[nodeio] Node 0 subscribed with interval 2000ms
[nodeio] Node 0 data: temp=22.5°C, humidity=45%
```

## Creating Your Own Node

### Arduino/ESP8266 Example
```cpp
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

WebSocketsClient webSocket;
uint8_t nodeId = 1;
uint32_t seqNum = 0;

void connectToHub() {
    webSocket.begin("192.168.1.100", 80, "/ws");
    webSocket.onEvent(webSocketEvent);
    webSocket.setExtraHeaders("Sec-WebSocket-Protocol: arduino");
}

void sendConnect() {
    DynamicJsonDocument doc(1024);
    doc["magic"] = 3203391147;
    doc["type"] = "connect";
    doc["node_id"] = nodeId;
    doc["seq_num"] = ++seqNum;
    doc["timestamp"] = WiFi.getTime();
    doc["controller"] = 2;  // CONTROLLER_ARDUINO
    doc["sw_version"] = "1.0.0";
    
    JsonArray sensors = doc.createNestedArray("sensors");
    sensors.add("temperature");
    sensors.add("humidity");
    
    JsonArray services = doc.createNestedArray("services");
    services.add("diagnostics");
    
    String message;
    serializeJson(doc, message);
    webSocket.sendTXT(message);
}

void sendSensorData() {
    DynamicJsonDocument doc(1024);
    doc["magic"] = 3203391147;
    doc["type"] = "node_data";
    doc["node_id"] = nodeId;
    doc["seq_num"] = ++seqNum;
    doc["timestamp"] = WiFi.getTime();
    
    JsonArray payload = doc.createNestedArray("payload");
    
    // Sensor data
    JsonObject sensorItem = payload.createNestedObject();
    sensorItem["type"] = "sensor";
    JsonObject sensor = sensorItem.createNestedObject("sensor");
    sensor["temperature"] = 25.5;
    sensor["humidity"] = 60.0;
    
    // Diagnostics
    JsonObject diagItem = payload.createNestedObject();
    diagItem["type"] = "diagnostics";
    JsonObject diag = diagItem.createNestedObject("diagnostics");
    diag["uptime_sec"] = millis() / 1000;
    diag["free_heap"] = ESP.getFreeHeap();
    diag["rssi"] = WiFi.RSSI();
    diag["error_code"] = 0;
    
    String message;
    serializeJson(doc, message);
    webSocket.sendTXT(message);
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.println("WebSocket Connected");
            sendConnect();
            break;
            
        case WStype_TEXT:
            handleMessage((char*)payload);
            break;
            
        case WStype_DISCONNECTED:
            Serial.println("WebSocket Disconnected");
            break;
    }
}

void handleMessage(const char* message) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, message);
    
    String type = doc["type"];
    if (type == "connect_response") {
        String status = doc["status"];
        Serial.println("Connection " + status);
    }
    else if (type == "notify_subscription") {
        JsonObject payload = doc["payload"];
        String status = payload["status"];
        if (status == "subscribed") {
            int interval = payload["interval"];
            Serial.println("Subscribed with " + String(interval) + "ms interval");
        }
    }
}
```

### Python Node Example
```python
import asyncio
import websockets
import json
import time

class SimpleNode:
    def __init__(self, node_id=1):
        self.node_id = node_id
        self.seq_num = 0
        self.subscribed = False
        self.interval = 5000  # Default 5 seconds
        
    async def connect(self, uri):
        async with websockets.connect(uri) as websocket:
            # Send connect message
            await self.send_connect(websocket)
            
            # Listen for messages and send data
            async for message in websocket:
                await self.handle_message(websocket, message)
    
    async def send_connect(self, websocket):
        message = {
            "magic": 0xBEEFBEEF,
            "type": "connect",
            "node_id": self.node_id,
            "seq_num": self.next_seq(),
            "timestamp": int(time.time()),
            "controller": 2,
            "sw_version": "1.0.0",
            "sensors": ["temperature", "humidity"],
            "services": ["diagnostics"]
        }
        await websocket.send(json.dumps(message))
    
    async def send_data(self, websocket):
        message = {
            "magic": 0xBEEFBEEF,
            "type": "node_data", 
            "node_id": self.node_id,
            "seq_num": self.next_seq(),
            "timestamp": int(time.time()),
            "payload": [
                {
                    "type": "sensor",
                    "sensor": {
                        "temperature": 20.0 + (time.time() % 10),
                        "humidity": 40 + (time.time() % 20)
                    }
                },
                {
                    "type": "diagnostics",
                    "diagnostics": {
                        "uptime_sec": int(time.time()),
                        "free_heap": 50000,
                        "rssi": -60,
                        "error_code": 0
                    }
                }
            ]
        }
        await websocket.send(json.dumps(message))
    
    async def handle_message(self, websocket, message):
        data = json.loads(message)
        msg_type = data.get("type")
        
        if msg_type == "connect_response":
            print(f"Node {self.node_id}: Connection {data['status']}")
            
        elif msg_type == "notify_subscription":
            payload = data.get("payload", {})
            if payload.get("status") == "subscribed":
                self.subscribed = True
                self.interval = payload.get("interval", 5000)
                print(f"Node {self.node_id}: Subscribed with {self.interval}ms interval")
                
                # Start sending data
                asyncio.create_task(self.data_loop(websocket))
    
    async def data_loop(self, websocket):
        while self.subscribed:
            await self.send_data(websocket)
            await asyncio.sleep(self.interval / 1000.0)
    
    def next_seq(self):
        self.seq_num += 1
        return self.seq_num

# Usage
async def main():
    node = SimpleNode(node_id=1)
    await node.connect("ws://192.168.1.100/ws")

asyncio.run(main())
```

## Common Use Cases

### 1. Garden Monitoring
```json
{
    "sensors": ["temperature", "humidity", "moisture", "light"],
    "services": ["diagnostics"]
}
```

### 2. Weather Station
```json
{
    "sensors": ["temperature", "humidity", "distance"],
    "services": ["diagnostics", "ota"]
}
```

### 3. Security System
```json
{
    "sensors": ["distance", "light"],
    "services": ["diagnostics"]
}
```

## Troubleshooting

### Node Won't Connect
```
✅ Check WebSocket URL: ws://hub-ip/ws
✅ Verify magic number: 3203391147 (0xBEEFBEEF)
✅ Ensure node_id in range 0-7
✅ Check WiFi connection
```

### No Data Received
```
✅ Wait for subscription message from hub
✅ Check sensor names match capability list
✅ Verify JSON format is correct
✅ Monitor hub logs for errors
```

### Connection Drops
```
✅ Implement WebSocket ping/pong handling
✅ Check network stability
✅ Monitor memory usage on node
✅ Handle reconnection gracefully
```

## Next Steps

1. 📖 Read the full [NodeIO Documentation](NODEIO_README.md)
2. 🔍 Explore the [API Reference](NODEIO_API_REFERENCE.md)  
3. 🧪 Try the interactive test client
4. 🛠️ Build your first real sensor node
5. 🌐 Integrate with the web dashboard

Happy IoT coding! 🚀