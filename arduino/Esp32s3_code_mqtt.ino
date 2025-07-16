#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_random.h>

// WiFi credentials - UPDATE THESE
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT Broker configuration - UPDATE THESE
const char* mqtt_server = "broker.emqx.io";  // Free public broker for testing
const int mqtt_port = 1883;
const char* mqtt_username = "";  // Leave empty if no auth required
const char* mqtt_password = "";  // Leave empty if no auth required
const char* mqtt_client_id = "ESP32_BIKE_001";

// MQTT Topics
const char* bike_data_topic = "smartcycle/bike/data";
const char* bike_status_topic = "smartcycle/bike/status";
const char* bike_heartbeat_topic = "smartcycle/bike/heartbeat";

// Bike configuration
const char* bikeId = "BIKE001";
const float baseLat = 19.0760;  // Mumbai
const float baseLng = 72.8777;
const float minSpeed = 12.0;
const float maxSpeed = 28.0;
const float locationOffset = 0.005;

// Timing configuration
const int dataIntervalMS = 5000;     // Send data every 5 seconds
const int heartbeatIntervalMS = 30000; // Send heartbeat every 30 seconds

// Battery and send counter logic
int battery = 70;
int sendCount = 0;
unsigned long lastBatteryUpdate = 0;
unsigned long lastDataSend = 0;
unsigned long lastHeartbeat = 0;

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient client(espClient);

// Function to generate random float within range
float randomFloat(float min, float max) {
  return min + (float)esp_random() / (float)UINT32_MAX * (max - min);
}

// Function to get current timestamp
String getCurrentTimestamp() {
  return String(millis());
}

// Function to generate bike data
void generateBikeData(float& avgSpeed, float& lat, float& lng, int& batteryLevel) {
  avgSpeed = randomFloat(minSpeed, maxSpeed);
  lat = baseLat + (randomFloat(0, 1) - 0.5) * locationOffset;
  lng = baseLng + (randomFloat(0, 1) - 0.5) * locationOffset;
  batteryLevel = battery;
}

// Function to connect to WiFi
void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Callback function for MQTT messages (if subscribing to any topics)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  
  // Handle incoming messages if needed
  if (String(topic) == "smartcycle/bike/command") {
    // Handle commands from server
    if (message == "reset_battery") {
      battery = 70;
      Serial.println("🔋 Battery reset via MQTT command");
    }
  }
}

// Function to connect to MQTT broker
void connectToMQTT() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    if (client.connect(mqtt_client_id, mqtt_username, mqtt_password)) {
      Serial.println("connected");
      
      // Subscribe to command topic
      client.subscribe("smartcycle/bike/command");
      
      // Send initial status
      sendStatusMessage("connected");
      
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" trying again in 5 seconds");
      delay(5000);
    }
  }
}

// Function to send bike data via MQTT
bool sendBikeDataMQTT() {
  if (!client.connected()) {
    Serial.println("MQTT not connected");
    return false;
  }

  // Generate bike data
  float avgSpeed, lat, lng;
  int batteryLevel;
  generateBikeData(avgSpeed, lat, lng, batteryLevel);

  // Create JSON payload
  StaticJsonDocument<400> doc;
  doc["bikeId"] = bikeId;
  doc["timestamp"] = getCurrentTimestamp();
  doc["deviceTime"] = millis();
  
  JsonObject data = doc.createNestedObject("data");
  data["avgSpeed"] = round(avgSpeed * 100) / 100.0;
  
  JsonObject location = data.createNestedObject("location");
  location["lat"] = lat;
  location["lng"] = lng;
  
  data["battery"] = batteryLevel;
  
  // Add device info
  JsonObject device = doc.createNestedObject("device");
  device["rssi"] = WiFi.RSSI();
  device["freeHeap"] = ESP.getFreeHeap();
  device["uptime"] = millis();

  String jsonString;
  serializeJson(doc, jsonString);

  // Publish to MQTT
  if (client.publish(bike_data_topic, jsonString.c_str())) {
    Serial.printf("[✓] %s - MQTT Published | Battery: %d%%\n", bikeId, battery);
    Serial.printf("Speed: %.2f km/h | Lat: %.6f | Lng: %.6f\n", avgSpeed, lat, lng);
    Serial.printf("Topic: %s\n", bike_data_topic);
    return true;
  } else {
    Serial.printf("[x] Error publishing data for %s\n", bikeId);
    return false;
  }
}

// Function to send status message
void sendStatusMessage(const char* status) {
  StaticJsonDocument<200> doc;
  doc["bikeId"] = bikeId;
  doc["status"] = status;
  doc["timestamp"] = getCurrentTimestamp();
  doc["battery"] = battery;
  doc["rssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  client.publish(bike_status_topic, jsonString.c_str());
  Serial.printf("Status sent: %s\n", status);
}

// Function to send heartbeat
void sendHeartbeat() {
  StaticJsonDocument<200> doc;
  doc["bikeId"] = bikeId;
  doc["timestamp"] = getCurrentTimestamp();
  doc["uptime"] = millis();
  doc["battery"] = battery;
  doc["rssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  client.publish(bike_heartbeat_topic, jsonString.c_str());
  Serial.println("💓 Heartbeat sent");
}

// Function to send bike data and update battery
void sendBikeDataOnce() {
  unsigned long currentTime = millis();
  
  Serial.println("\n🔄 Sending bike data via MQTT at " + getCurrentTimestamp());
  
  if (sendBikeDataMQTT()) {
    sendCount++;
    
    // Battery logic: decrease after every 5 sends or 30 seconds
    if (sendCount >= 5 || (currentTime - lastBatteryUpdate) >= 30000) {
      battery--;
      sendCount = 0;
      lastBatteryUpdate = currentTime;
      
      if (battery < 30) {
        battery = 70;
        Serial.println("🔋 Battery reset to 70%");
        sendStatusMessage("battery_reset");
      }
    }
    
    Serial.println("✅ Bike data sent successfully via MQTT\n");
  } else {
    Serial.println("❌ Failed to send bike data via MQTT\n");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("🚴 ESP32 MQTT Bike Simulator started...");
  Serial.println("MQTT Broker: " + String(mqtt_server) + ":" + String(mqtt_port));
  Serial.println("Bike being simulated: " + String(bikeId));
  
  // Connect to WiFi
  connectToWiFi();
  
  // Setup MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  
  // Connect to MQTT broker
  connectToMQTT();
  
  Serial.println("Connected to MQTT broker. Starting bike data transmission...");
  Serial.println("Data Interval: " + String(dataIntervalMS) + "ms");
  Serial.println("Heartbeat Interval: " + String(heartbeatIntervalMS) + "ms");
  
  // Send initial data
  sendBikeDataOnce();
  lastBatteryUpdate = millis();
  lastDataSend = millis();
  lastHeartbeat = millis();
}

void loop() {
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost. Reconnecting...");
    connectToWiFi();
  }
  
  // Check MQTT connection
  if (!client.connected()) {
    Serial.println("MQTT connection lost. Reconnecting...");
    connectToMQTT();
  }
  
  // Maintain MQTT connection
  client.loop();
  
  unsigned long currentTime = millis();
  
  // Send bike data at regular intervals
  if (currentTime - lastDataSend >= dataIntervalMS) {
    sendBikeDataOnce();
    lastDataSend = currentTime;
  }
  
  // Send heartbeat at regular intervals
  if (currentTime - lastHeartbeat >= heartbeatIntervalMS) {
    sendHeartbeat();
    lastHeartbeat = currentTime;
  }
  
  // Small delay to prevent watchdog issues
  delay(100);
}