#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <painlessMesh.h>
#include <PubSubClient.h> // For MQTT client
#include <ArduinoJson.h>

// --- Configuration Constants ---
// Default AP and MESH settings for configuration mode
const char* AP_SSID = "MeshConfigAP";
const char* AP_PASSWORD = "password";
const char* MESH_SSID = "MyMeshNetwork";
const char* MESH_PASSWORD = "meshpassword";
const int MESH_PORT = 5555;

IPAddress AP_LOCAL_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);


// Reset button pin (example: D0 on NodeMCU/ESP-12E)
// Make sure to connect a pull-up resistor if using a simple button to GND.
// Or use INPUT_PULLUP and connect button to GND.
const int RESET_BUTTON_PIN = D7; // GPIO13 on NodeMCU
const unsigned long RESET_HOLD_TIME_MS = 5000; // 5 seconds

// EEPROM size for configuration storage
#define EEPROM_SIZE sizeof(Config)

// --- Global Objects ---
Scheduler userScheduler;
ESP8266WebServer server(80);
painlessMesh mesh;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// User background periodic tasks
void sendSensedData() ;     // Collect and transmit local sensor data at a configured interval, routing it to either 
                            // the gateway node or directly to the cloud via MQTT, depending on the node's role.
void checkMqttConnection(); // Maintain active MQTT connectivity through periodic status checks (for gateway nodes) 

// Periodically transmits sensed data based on the configured sensing interval (default: 10 seconds)
Task taskSendSensedData( TASK_SECOND * 10 , TASK_FOREVER, &sendSensedData );
// Checks MQTT connectivity every 5 seconds
Task tMqttReconnect(TASK_SECOND * 5, TASK_FOREVER, &checkMqttConnection);

// --- Configuration Structure ---
// This struct defines the data stored in EEPROM
struct Config {
    int configured;           // Flag: 5 if configured, 3 or 0xff (empty EEPROM) if in AP mode
    bool isCoordinator;       // true for gateway/coordinator node, false for router node
    long nodeID;              // Unique user-defined ID for this node
    long coordinatorID;       // For router nodes: The ID of the gateway/coordinator node to send data to. This node acts as the mesh's gateway to the internet and connects to the MQTT broker
                              // For gateway/coordinator nodes: this should be the same as the Node ID.
    char wifiSSID[32];        // For gateway/coordinator nodes: The SSID of the WiFi router that provides internet access.
    char wifiPassword[32];    // For gateway/coordinator nodes: The password of the WiFi router that provides internet access.
    char mqttHost[64];        // For gateway/coordinator nodes: Host url of the MQTT Broker 
    int mqttPort;             // gateway/coordinator nodes: MQTT Broker Port
    char mqttUsername[32];    // gateway/coordinator nodes: MQTT Username
    char mqttPassword[32];    // gateway/coordinator nodes: MQTT Password
    char mqttPublishTopic[64]; // gateway/coordinator nodes: MQTT Publish Topic
    char mqttSubscribeTopic[64]; // gateway/coordinator nodes: MQTT Subscribe Topic
    long dutyCycle;           // The interval, in seconds, at which sensor data is sampled and transmitted
    char meshSSID[32];        // All nodes on the same mesh must use the same mesh SSID
    char meshPassword[32];    // All nodes on the same mesh must use the same mesh password
    int meshPort;             // All nodes on the same mesh must use the same mesh port
    uint32_t channelId;       // For ESP8266: All nodes (gateway/coordinator and router) on the same mesh must use the same channel as the WiFi network the gateway/coordinator node is connected to.
                              // For ESP32: Not used    
};

Config config; // Global configuration variable

// --- State Variables for Normal Operation ---
unsigned long lastSenseTime = 0;
unsigned long lastMQTTReconnectAttempt = 0;
const long MQTT_RECONNECT_INTERVAL = 5000; // 10 seconds

// --- State Variables for Reset Button ---
unsigned long resetButtonPressedTime = 0;
bool resetButtonState = HIGH; // Initial state (active-low)

// --- Function Prototypes ---
void loadConfiguration();
void saveConfiguration();
void resetToFactorySettings();
void startAPMode();
void handleRoot();
void handleSubmit();
void meshInit();
void meshReceivedCallback(uint32_t from, String &msg);
void meshNewConnectionCallback(uint32_t nodeId);
void meshDroppedConnectionCallback(uint32_t nodeId);
void meshNodeTimeAdjustedCallback(int32_t offset);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void reconnectMQTT();
void getChannel();
String getSensorData(); // Placeholder for sensor reading

// stores the channel ID that will be used
uint32_t channelId;

// holds the sensor data in JSON  
StaticJsonDocument<1000> sensor_data;

// --- Setup Function ---
void setup() {
    
    Serial.begin(115200);
    Serial.println();
    Serial.println("Starting Node...");
    
    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    loadConfiguration();        // Load configuration from EEPROM
      
    // Initialize reset button pin
    // Press and hold this button for 10 seconds to reset the node to its default configuration (AP mode)
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up
                                      
    if (config.configured == 5) {        
        Serial.println("Node configured. Starting normal operation.");
        Serial.printf("Node ID: %ld, Coordinator ID: %ld, Duty Cycle: %ld seconds\n",
                      config.nodeID, config.coordinatorID, config.dutyCycle);
        
        if (config.isCoordinator) {
        
            Serial.println("Node is configured as COORDINATOR.");
            
            // Need to connect to WiFi first, and get the channelID. Nodes can be a member of the mesh and at the same time connect to an external WiFi if they use THE SAME channel (ESP8266).
            // Therefore, connect to the gateway first, get the channel ID, then disconnect for the Mesh initialization 

            Serial.printf("Connecting to WiFi: %s\n", config.wifiSSID);

            getChannel();

            // disconnect WiFi and reconnect later using the mesh.stationManual() function with the channelId           
            WiFi.disconnect();
            Serial.printf("Disconnected to WiFi: %s\n", config.wifiSSID);
            
            // Create mesh            
            Serial.println("Creating a mesh..");
            meshInit();
            Serial.println("Mesh created..");
            
            // WiFI and MQTT connect
            mesh.stationManual(config.wifiSSID, config.wifiPassword);
            mesh.setHostname("MeshBridge");
            Serial.println("Re-connecting to gateway..");
            //delay(2000);
            Serial.println("Station IP: ");
            Serial.println(mesh.getStationIP());

            // Bridge node, should (in most cases) be a root node. See [the wiki](https://gitlab.com/painlessMesh/painlessMesh/wikis/Possible-challenges-in-mesh-formation) for some background
            //mesh.setRoot(true);
         
            mqttClient.setClient(espClient);
            mqttClient.setServer(config.mqttHost, config.mqttPort);
            mqttClient.setCallback(mqttCallback);
            // Check connection every 5 seconds
            userScheduler.addTask(tMqttReconnect);
            tMqttReconnect.enable();
              
        } else {
            Serial.println("Node is configured as ROUTER.");

            channelId = config.channelId;
            
            Serial.println("Creating a mesh..");
            meshInit();
            Serial.println("Mesh created..");
        }
            
        Serial.printf("Setting the duty cycle to %ld seconds", config.dutyCycle);
        taskSendSensedData.setInterval(config.dutyCycle * TASK_SECOND);
        userScheduler.addTask( taskSendSensedData );
        taskSendSensedData.enable();
    } else {    
        Serial.println("Node not configured. Starting in AP mode for configuration.");
        startAPMode();         
    }
    
   
}

// --- Loop Function ---
void loop() {
    
    // Check for reset button press (active-low)    
    int currentButtonState = digitalRead(RESET_BUTTON_PIN);
    if (currentButtonState == LOW && resetButtonState == HIGH) { // Button pressed
        resetButtonPressedTime = millis();
        Serial.println("Reset button pressed...");
    } else if (currentButtonState == LOW && (millis() - resetButtonPressedTime >= RESET_HOLD_TIME_MS)) {
        // Button held for 5 seconds
        Serial.println("Reset button held for 5 seconds! Resetting to factory settings...");
        delay(5000);
        resetToFactorySettings();
    } else if (currentButtonState == HIGH && resetButtonState == LOW) { // Button released
        Serial.println("Reset button released.");
        resetButtonPressedTime = 0; // Reset timer
    }
    resetButtonState = currentButtonState; // Update button state for next loop
    
    if (config.configured != 5) {
        // In configuration mode, handle web server requests
        server.handleClient();
    } else {
        // In normal operation mode
        mesh.update(); // Keep the mesh network running
      
        if (config.isCoordinator) {
            mqttClient.loop(); // Keep MQTT connection alive and process incoming messages
        }
         
    }
}


void meshInit() {
    // Initialize PainlessMesh
    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE); // Debugging
    
    Serial.printf("Check Mesh credentials: %s %s %ld %ld \n", config.meshSSID, config.meshPassword, config.meshPort, channelId);
    mesh.init(String(config.meshSSID), String(config.meshPassword), &userScheduler, (uint16_t)config.meshPort, WIFI_AP_STA, (uint8_t)channelId, 0, 10); // last three, channelID, hidden SSID or not, max nodes
    mesh.onReceive(&meshReceivedCallback);
    mesh.onNewConnection(&meshNewConnectionCallback);
    mesh.onDroppedConnection(&meshDroppedConnectionCallback);
    mesh.onNodeTimeAdjusted(&meshNodeTimeAdjustedCallback);
        
}
// --- Configuration Functions ---

void loadConfiguration() {
    EEPROM.get(0, config);
    
    if (config.configured == 5) { // When flashed, all EEPROM entries will be 0xFF or 255
        Serial.println("Configuration loaded from EEPROM.");
        Serial.printf("Is Coordinator: %s\n", config.isCoordinator ? "Yes" : "No");
        Serial.printf("Node ID: %ld\n", config.nodeID);
        Serial.printf("Coordinator ID: %ld\n", config.coordinatorID);
        Serial.printf("Duty Cycle: %ld seconds\n", config.dutyCycle);
        Serial.printf("Channel ID: %ld \n", config.channelId);
        Serial.printf("Mesh SSID: %s \n", config.meshSSID);
        Serial.printf("Mesh password: %s \n", config.meshPassword);
        Serial.printf("Mesh port: %ld \n", config.meshPort);
        Serial.printf("Check whitespaces:%s%s%ld%ld \n", config.meshSSID, config.meshPassword, config.meshPort, config.channelId);
              
        if (config.isCoordinator) {
            Serial.printf("WiFi SSID: %s\n", config.wifiSSID);
            Serial.printf("WiFi SSID: %s\n", config.wifiPassword);
            Serial.printf("Mesh SSID: %s\n", config.meshSSID);
            Serial.printf("MQTT Host: %s, Port: %d\n", config.mqttHost, config.mqttPort);
            Serial.printf("MQTT Pub Topic: %s, Sub Topic: %s\n", config.mqttPublishTopic, config.mqttSubscribeTopic);
        }
    } else {
        Serial.println("No valid configuration found in EEPROM. Using default.");
        // Set default values if not configured
        config.configured = 10; // Ensure this is not equal to 5 (not configured) for AP mode
        config.isCoordinator = false; // Default to router
        config.nodeID = 0;
        config.coordinatorID = 0;
        strcpy(config.wifiSSID, "");
        strcpy(config.wifiPassword, "");
        strcpy(config.meshSSID, "");
        strcpy(config.meshPassword, "");
        config.meshPort = 5555;
        strcpy(config.mqttHost, "");
        config.mqttPort = 1883;
        strcpy(config.mqttUsername, "");
        strcpy(config.mqttPassword, "");
        strcpy(config.mqttPublishTopic, "mesh/data");
        strcpy(config.mqttSubscribeTopic, "mesh/cmd");
        config.dutyCycle = 10; // Default 10 seconds
        
    }
}

void saveConfiguration() {
    EEPROM.put(0, config);
    EEPROM.commit();
    Serial.println("Configuration saved to EEPROM.");
}

void resetToFactorySettings() {
    Serial.println("Resetting to factory settings...");
    // Clear the configured flag and other sensitive info
    //config.configured = false;
    config.configured = 10;
    // Overwrite with default values or clear EEPROM
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0xFF); // Fill with 0xFF to clear
    }
    EEPROM.commit();
    Serial.println("EEPROM cleared. Restarting...");
    ESP.restart(); // Restart the ESP to enter AP mode
}

// --- AP Mode and Web Server Functions ---

void startAPMode() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print("AP Mode Started. SSID: ");
    Serial.print(AP_SSID);
    Serial.print(" IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", HTTP_GET, handleRoot);
    server.on("/submit", HTTP_POST, handleSubmit);
    server.begin();
    Serial.println("HTTP server started.");
}

void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP8266 Mesh Node Configuration</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f2f5; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin: 0; }
        .container { background-color: #ffffff; padding: 2.5rem; border-radius: 0.75rem; box-shadow: 0 10px 15px -3px rgba(0, 0, 0, 0.1), 0 4px 6px -2px rgba(0, 0, 0, 0.05); max-width: 90%; width: 500px; }
        .hstyle { font-size: 1.875rem; font-weight: 700; text-align: center; margin-bottom: 2rem; }
        input[type="text"], input[type="password"], input[type="number"] {width:100%; padding: 0.75rem; border: 1px solid #e2e8f0; border-radius: 0.375rem; margin-bottom: 1rem; box-sizing: border-box; }
        input[type="radio"] { margin-right: 0.5rem; }
        .radio-group label { display: inline-block; margin-right: 1rem; }
        .section-title { font-size: 1.25rem; padding: 0.75em; background-color: #c8cefe; font-weight: 700; margin-top: 1.5rem; margin-bottom: 1rem; color: #2d3748; border-bottom: 2px solid #edf2f7; padding-bottom: 0.5rem; }
        .button { width: 100%; padding: 0.75rem; background-color: #4c51bf; color: white; border: 2px solid #5a67d8; border-radius: 0.375rem; font-size: 1rem; font-weight: 700; cursor: pointer; transition: background-color 0.3s ease; margin-top: 1.5rem; }
        .button:hover { background-color: #6673e5; }
        .hidden { display: none; }
    </style>
</head>
<body>
    <div class="container">
        <h1 class="hstyle">Configure Mesh Node</h1>
        <form action="/submit" method="POST" id="configForm">
            <div class="section-title">Mesh Node Setting</div>
            <div class="radio-group mb-4">
                <input type="radio" name="nodeType" value="router" id="routerRadio" checked> Router
                &nbsp &nbsp &nbsp &nbsp
                <input type="radio" name="nodeType" value="coordinator" id="coordinatorRadio"> Gateway/Bridge
            </div>
            
            <div id="coordinator" class="hidden">        
                <div class="section-title">WiFi Credentials (for Gateway)</div>
                <b>Gateway SSID:</b>
                <input type="text" id="wifiSsid" name="wifiSsid" value="">

                <b>Gateway Password:</b>
                <input type="password" id="wifiPassword" name="wifiPassword" value="">
            </div>
            
            <div class="section-title">Mesh Credentials</div>
            
            <b>Mesh SSID:</b> <input type="text" id="meshSsid" name="meshSsid" value="">

            <b>Mesh Password:</b>  <i>(Must be 8 characters or more)</i>:  
            <input type="password" id="meshPassword" name="meshPassword" value="">
            
            <b>Mesh Port:</b> <input type="number" id="meshPort" name="meshPort" value="5555">

            <b>Channel ID:</b>  
            <input type="number" id="channelId" name="channelId" value="1">
            (<i>Note</i>: Only fill this out if set as a router. It must be the same channel used by the coordinator/gateway.)
            
            <div class="section-title">Node Information</div>
            <b>Node ID (Numeric):</b>
            <input type="number" id="nodeId" name="nodeId" required value="1">
            <div class="section-title">Gateway Node Information</div>
            <b>Gateway Node ID (Numeric):</b>
            <input type="number" id="coordinatorId" name="coordinatorId" required value="0">
                        
            <div id="mqtt" class="hidden">     
                <div class="section-title">MQTT Information</div>
                <b>Host:</b>
                <input type="text" id="mqttHost" name="mqttHost" value="broker.emqx.io">

                <b>Port:</b>
                <input type="number" id="mqttPort" name="mqttPort" value="1883">

                <b>Username (Optional):</b>
                <input type="text" id="mqttUsername" name="mqttUsername" value="">

                <b>Password (Optional):</b>
                <input type="password" id="mqttPassword" name="mqttPassword" value="">

                <b>Publish Topic:</b>
                <input type="text" id="mqttPublishTopic" name="mqttPublishTopic" value="mesh/data">

                <b>Subscribe Topic:</b>
                <input type="text" id="mqttSubscribeTopic" name="mqttSubscribeTopic" value="mesh/cmd">
            </div>

            <div class="section-title">Common Settings</div>
            <b>Sensing Interval (Seconds):</b>
            <input type="number" id="dutyCycle" name="dutyCycle" required value="10">

            <button class="button" type="submit">OK</button>
        </form>
    </div>

    <script>
        const form = document.getElementById('configForm');
        const routerRadio = document.getElementById('routerRadio');
        const coordinatorRadio = document.getElementById('coordinatorRadio');
        const coordinatorSettings = document.getElementById('coordinator');
        const mqttSettings = document.getElementById('mqtt');

        function toggleSettings() {
            if (routerRadio.checked) {
                coordinatorSettings.classList.add('hidden');
                mqttSettings.classList.add('hidden');
            } else {
                coordinatorSettings.classList.remove('hidden');
                mqttSettings.classList.remove('hidden');
            }
        }

        form.addEventListener('submit', function(e) {
            const inputs = form.querySelectorAll('input:not([type="radio"])');
            const meshPass = document.getElementById('meshPassword').value;
            let valid = true;

            inputs.forEach(input => {
                // Check if field is empty and NOT hidden and NOT optional MQTT fields
                if (!input.closest('.hidden') && !input.value.trim() && 
                    input.id !== 'mqttUsername' && input.id !== 'mqttPassword') {
                    valid = false;
                }
            });

            if (!valid) {
                alert("Please fill in all required fields.");
                e.preventDefault();
            } else if (meshPass.length < 8) {
                alert("Mesh Password must be at least 8 characters.");
                e.preventDefault();
            }
        });

        routerRadio.addEventListener('change', toggleSettings);
        coordinatorRadio.addEventListener('change', toggleSettings);

        toggleSettings();
    </script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}

void handleSubmit() {
    Serial.println("Form submitted!");
    if (server.hasArg("nodeType")) {
        String nodeType = server.arg("nodeType");
        if (nodeType == "router") {
            config.isCoordinator = false;
            if (server.hasArg("meshSsid")) strncpy(config.meshSSID, server.arg("meshSsid").c_str(), sizeof(config.meshSSID) - 1);
            if (server.hasArg("meshPassword")) strncpy(config.meshPassword, server.arg("meshPassword").c_str(), sizeof(config.meshPassword) - 1);
            if (server.hasArg("meshPort")) config.meshPort = server.arg("meshPort").toInt();
            if (server.hasArg("nodeId")) config.nodeID = server.arg("nodeId").toInt();
            if (server.hasArg("coordinatorId")) config.coordinatorID = server.arg("coordinatorId").toInt();
            if (server.hasArg("channelId")) config.channelId = server.arg("channelId").toInt();
            Serial.printf("Configuring as Router. Node ID: %ld, Coordinator ID: %ld\n", config.nodeID, config.coordinatorID);
        } else if (nodeType == "coordinator") {
            config.isCoordinator = true;
            if (server.hasArg("wifiSsid")) strncpy(config.wifiSSID, server.arg("wifiSsid").c_str(), sizeof(config.wifiSSID) - 1);
            if (server.hasArg("wifiPassword")) strncpy(config.wifiPassword, server.arg("wifiPassword").c_str(), sizeof(config.wifiPassword) - 1);
            if (server.hasArg("meshSsid")) strncpy(config.meshSSID, server.arg("meshSsid").c_str(), sizeof(config.meshSSID) - 1);
            if (server.hasArg("meshPassword")) strncpy(config.meshPassword, server.arg("meshPassword").c_str(), sizeof(config.meshPassword) - 1);
            if (server.hasArg("meshPort")) config.meshPort = server.arg("meshPort").toInt();
            if (server.hasArg("coordinatorId")) config.coordinatorID = server.arg("coordinatorId").toInt();
            config.nodeID = config.coordinatorID; // Coordinator's node ID is its coordinator ID
            if (server.hasArg("mqttHost")) strncpy(config.mqttHost, server.arg("mqttHost").c_str(), sizeof(config.mqttHost) - 1);
            if (server.hasArg("mqttPort")) config.mqttPort = server.arg("mqttPort").toInt();
            if (server.hasArg("mqttUsername")) strncpy(config.mqttUsername, server.arg("mqttUsername").c_str(), sizeof(config.mqttUsername) - 1);
            if (server.hasArg("mqttPassword")) strncpy(config.mqttPassword, server.arg("mqttPassword").c_str(), sizeof(config.mqttPassword) - 1);
            if (server.hasArg("mqttPublishTopic")) strncpy(config.mqttPublishTopic, server.arg("mqttPublishTopic").c_str(), sizeof(config.mqttPublishTopic) - 1);
            if (server.hasArg("mqttSubscribeTopic")) strncpy(config.mqttSubscribeTopic, server.arg("mqttSubscribeTopic").c_str(), sizeof(config.mqttSubscribeTopic) - 1);
            
            // Get the channel of the desired WiFi gateway/router by scanning all available network and matching them to the desired WiFi network  
            int n = WiFi.scanNetworks();
            Serial.println("Scan done");    
            if (n == 0) {
              Serial.println("No networks found");
            } else {
              Serial.print(n);
              Serial.println(" networks found:");
              for (int i = 0; i < n; ++i) {
                // Print SSID and Channel
                Serial.print(i + 1);
                Serial.print(": ");
                Serial.print(WiFi.SSID(i));      // This prints the SSID 
                Serial.print(", Ch: ");
                Serial.println(WiFi.channel(i)); // This prints the channel
                if (String(WiFi.SSID(i)) == String(config.wifiSSID))
                  channelId = WiFi.channel(i);
                delay(10);  
              }
            }
            config.channelId = channelId;
            Serial.println("");
            Serial.printf("Configuring as Coordinator. Coordinator ID: %ld\n", config.coordinatorID);
            Serial.printf("WiFi: %s, Wifi Channel: %d, MQTT Host: %s:%d\n", config.wifiSSID, channelId, config.mqttHost, config.mqttPort);
        }

        if (server.hasArg("dutyCycle")) config.dutyCycle = server.arg("dutyCycle").toInt();

        config.configured = 5; // Mark as configured
        saveConfiguration();
           
        String response = "<html><body><h1>Configuration saved. <br>Channel used: <b>";
        response += String(config.channelId);
        response += "</b>. <br>Restarting Node.</h1></body></html>";
        
        server.send(200, "text/html", response);
        delay(5000);
        ESP.restart(); // Restart the ESP to apply new configuration
    } else {
        server.send(400, "text/plain", "Bad Request: Missing nodeType.");
    }
}

// --- PainlessMesh Callbacks and Functions ---

void sendSensedData() {  
  
  String data = getSensorData();
  
  if (config.isCoordinator) {
        
         // Get sensor data
        Serial.printf("Coordinator senseed data: %s\n", data.c_str());

        
        if (mqttClient.connected()) {
            mqttClient.publish(config.mqttPublishTopic, data.c_str());
            Serial.printf("Published own data to MQTT topic '%s'\n", config.mqttPublishTopic);
        } else {
            Serial.println("MQTT not connected, cannot publish own data.");
        }               
        

  } else {
      
      Serial.printf("Router %ld sensing data: %s\n", config.nodeID, data.c_str());

      if (mesh.sendBroadcast(data)) {
          Serial.printf("Router %ld broadcasted the payload \n", config.nodeID);
          //Serial.printf("Router %ld sent data to coordinator %ld\n", config.nodeID, config.coordinatorID);
      } else {
          //Serial.printf("Router %ld failed to send data to coordinator %ld (coordinator possibly out of range or not connected)\n", config.nodeID, config.coordinatorID);
          Serial.printf("Router %ld failed to broadcast the payload \n", config.nodeID);
      }
      
  }  
}


void meshReceivedCallback(uint32_t from, String &msg) {
    Serial.printf("Received from %u: %s\n", from, msg.c_str());

    if (config.isCoordinator) {
        // Coordinator receives data from other nodes
        Serial.printf("Coordinator received data from node %u: %s\n", from, msg.c_str());
        if (mqttClient.connected()) {
            
            mqttClient.publish(config.mqttPublishTopic, msg.c_str());
            
            Serial.printf("Published mesh data from %u to MQTT topic '%s'\n", from, config.mqttPublishTopic);
        } else {
            Serial.println("MQTT not connected, cannot publish received mesh data.");
        }
    } else {
        // Router receives data (e.g., commands from coordinator or other routers)
        // For this implementation, routers primarily send data.
        // You can add logic here to process commands if needed.
        Serial.printf("Router received message from %u: %s\n", from, msg.c_str());
    }
}

void meshNewConnectionCallback(uint32_t nodeId) {
    Serial.printf("New connection from %u\n", nodeId);
}

void meshDroppedConnectionCallback(uint32_t nodeId) {
    Serial.printf("Dropped connection from %u\n", nodeId);
}

void meshNodeTimeAdjustedCallback(int32_t offset) {
    Serial.printf("Adjusted time by %d us\n", offset);
}

// --- MQTT Functions ---

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("Message arrived on topic: %s\n", topic);
    Serial.print("Payload: ");
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    Serial.println(message);

    // Example: If coordinator receives a command on subscribe topic,
    // it could broadcast it back to the mesh or perform an action.
    // This is where you'd implement logic for remote control.
    if (config.isCoordinator) {
        if (String(topic) == config.mqttSubscribeTopic) {
            Serial.printf("Coordinator received command via MQTT: %s\n", message.c_str());
            // Example: mesh.sendBroadcast(message); // Broadcast command to all mesh nodes
        }
    }
}

void checkMqttConnection() {
  if (config.isCoordinator) { // Only try MQTT if configured as coordinator
    if (!mqttClient.connected()) {
      Serial.print("Attempting MQTT connection...");
        
      String clientId = "ESP-";
      clientId += String(ESP.getChipId()); // Unique client ID
      clientId += String(random(0xffff), HEX);
      
      Serial.printf("Connecting to MQTT as %s\n", clientId.c_str());
      bool connected;
      if (strlen(config.mqttUsername) > 0) {
          connected = mqttClient.connect(clientId.c_str(), config.mqttUsername, config.mqttPassword);
      } else {
          connected = mqttClient.connect(clientId.c_str());
      }

      if (connected) {
          Serial.println("connected");
          mqttClient.subscribe(config.mqttSubscribeTopic);
          Serial.printf("Subscribed to MQTT topic: %s\n", config.mqttSubscribeTopic);
      } else {
          Serial.print("failed, rc=");
          Serial.print(mqttClient.state());
          Serial.println(" trying again in 5 seconds");
      }
    }
  }
}

void reconnectMQTT() {

    unsigned long currentMillis = millis();
    if (!mqttClient.connected() && (currentMillis - lastMQTTReconnectAttempt > MQTT_RECONNECT_INTERVAL)) {
        Serial.print("Attempting MQTT connection...");
        
        String clientId = "ESP-";
        clientId += String(ESP.getChipId()); // Unique client ID
        clientId += String(random(0xffff), HEX);
        
        Serial.printf("Connecting to MQTT as %s\n", clientId.c_str());
        
        bool connected;
        if (strlen(config.mqttUsername) > 0) {
            connected = mqttClient.connect(clientId.c_str(), config.mqttUsername, config.mqttPassword);
        } else {
            connected = mqttClient.connect(clientId.c_str());
        }

        if (connected) {
            Serial.println("connected");
            mqttClient.subscribe(config.mqttSubscribeTopic);
            Serial.printf("Subscribed to MQTT topic: %s\n", config.mqttSubscribeTopic);
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" trying again in 5 seconds");
        }
        lastMQTTReconnectAttempt = currentMillis;
    }
}

// --- Get the channel by connecting to the WiFi gateway, retrieve chanell, and assign to channelId
void getChannel() {
  // Retrieve channelId
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(config.wifiSSID, config.wifiPassword);
  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      
      int currentButtonState = digitalRead(RESET_BUTTON_PIN);
      if (currentButtonState == LOW && resetButtonState == HIGH) { // Button pressed
          resetButtonPressedTime = millis();
          Serial.println("Reset button pressed...");
      } else if (currentButtonState == LOW && (millis() - resetButtonPressedTime >= RESET_HOLD_TIME_MS)) {
          // Button held for 5 seconds
          Serial.println("Reset button held for 5 seconds! Resetting to factory settings...");
          delay(5000);
          resetToFactorySettings();
      } else if (currentButtonState == HIGH && resetButtonState == LOW) { // Button released
          Serial.println("Reset button released.");
          resetButtonPressedTime = 0; // Reset timer
      }
      resetButtonState = currentButtonState; // Update button state for next loop
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Channel ID: ");
  channelId = WiFi.channel();
  Serial.println(channelId); 
  
}
 
// --- Sensor Data Placeholder ---
// Replace this with your actual sensor reading logic
String getSensorData() {

    char buffer[256];
    
    sensor_data["nodeID"] = config.nodeID;
    //sensor_data["datetime"] = "2025-02-16 12:29:21.830751";
    
    // Example: Simulate a sensor reading (e.g., temperature)
    //float temperature = random(200, 300) / 10.0; // Random temperature between 20.0 and 30.0
    //float humidity = random(0, 1000) / 10.0; // Random temperature between 20.0 and 30.0
    //sensor_data["temperature"] = random(200, 300) / 10.0; // Random temperature between 20.0 and 30.0
    sensor_data["temperature"] = 25.0; 
    sensor_data["humidity"] = 50.0;
    sensor_data["aqi"] = 1;
    sensor_data["co2"] = 500;
    sensor_data["tvoc"] = 200;
    sensor_data["lux"] = 400;    
     
    serializeJson(sensor_data, buffer);
    
    return String(buffer);
}
