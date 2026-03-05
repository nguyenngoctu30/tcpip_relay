#include <SPI.h>
#include <Ethernet_Generic.h>
#include <PubSubClient.h>
#include <Preferences.h>

// ================= MQTT =================
#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT   1883

// ================= W5500 SPI =================
#define W5500_CS  22
#define SPI_MOSI  23
#define SPI_MISO  15
#define W5500_SCK   21

// ================= RELAY OUTPUT =================
#define RELAY_PIN 7

// ================= MAC THIẾT BỊ GỬI (để subscribe data) =================
#define SENDER_MAC "54:32:04:0C:E6:02"

byte mac[] = {0x54, 0x32, 0x04, 0x0C, 0xE6, 0x03};  // MAC thiết bị nhận

IPAddress default_ip(192,168,1,213);      // IP mặc định thiết bị nhận (khác thiết bị gửi)
IPAddress default_dns(8,8,8,8);
IPAddress default_gateway(192,168,1,1);
IPAddress default_subnet(255,255,255,0);

IPAddress current_ip, current_dns, current_gateway, current_subnet;
IPAddress backup_ip, backup_dns, backup_gateway, backup_subnet;

// Topics của THIẾT BỊ NHẬN (self)
String cmd_topic, data_topic, status_topic, config_topic, ip_topic;

// Topic subscribe từ THIẾT BỊ GỬI
String sender_data_topic;

EthernetClient ethClient;
PubSubClient client(ethClient);
Preferences preferences;

// Trạng thái relay output
int relay_state = 0;
int last_relay_state = -1;

unsigned long lastIpPublish = 0;
const unsigned long IP_PUBLISH_INTERVAL = 60000UL;

// =====================================================
// UTILITY
// =====================================================

String getMacString() {
  String s;
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) s += "0";
    s += String(mac[i], HEX);
    if (i < 5) s += ":";
  }
  s.toUpperCase();
  return s;
}

String getCurrentIPString() {
  IPAddress ip = Ethernet.localIP();
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

// =====================================================
// ĐIỀU KHIỂN RELAY OUTPUT
// =====================================================

void setRelay(int state) {
  relay_state = state;
  digitalWrite(RELAY_PIN, relay_state ? HIGH : LOW);

  Serial.print("Relay OUTPUT set: ");
  Serial.println(relay_state);

  // Chỉ publish nếu trạng thái thực sự thay đổi
  if (relay_state != last_relay_state) {
    last_relay_state = relay_state;
    if (client.connected()) {
      client.publish(data_topic.c_str(), String(relay_state).c_str(), true);
      Serial.print("Published relay state to: ");
      Serial.println(data_topic);
    }
  }
}

// =====================================================
// NETWORK SAVE / LOAD
// =====================================================

void saveNetworkConfig(IPAddress ip, IPAddress dns, IPAddress gw, IPAddress sn) {
  preferences.begin("network", false);
  preferences.putBytes("ip",      (uint8_t*)&ip,  sizeof(ip));
  preferences.putBytes("dns",     (uint8_t*)&dns, sizeof(dns));
  preferences.putBytes("gateway", (uint8_t*)&gw,  sizeof(gw));
  preferences.putBytes("subnet",  (uint8_t*)&sn,  sizeof(sn));
  preferences.end();
}

void loadNetworkConfig() {
  preferences.begin("network", true);

  if (preferences.getBytesLength("ip") == 0) {
    current_ip      = default_ip;
    current_dns     = default_dns;
    current_gateway = default_gateway;
    current_subnet  = default_subnet;
    preferences.end();
    saveNetworkConfig(current_ip, current_dns, current_gateway, current_subnet);
  } else {
    preferences.getBytes("ip",      (uint8_t*)&current_ip,      sizeof(current_ip));
    preferences.getBytes("dns",     (uint8_t*)&current_dns,     sizeof(current_dns));
    preferences.getBytes("gateway", (uint8_t*)&current_gateway, sizeof(current_gateway));
    preferences.getBytes("subnet",  (uint8_t*)&current_subnet,  sizeof(current_subnet));
    preferences.end();
  }
}

void applyNetwork(IPAddress ip, IPAddress dns, IPAddress gw, IPAddress sn) {
  Ethernet.begin(mac, ip, dns, gw, sn);
  delay(4000);

  Serial.print("IP hiện tại: ");
  Serial.println(Ethernet.localIP());
  Serial.print("Link: ");
  Serial.println(Ethernet.linkStatus() == LinkON ? "ON" : "OFF");
}

// =====================================================
// MQTT TEST (dùng khi đổi IP)
// =====================================================

bool testMQTT() {
  Serial.println("\n=== TEST MQTT ===");

  client.disconnect();
  delay(1000);

  String testClient = "ESP32-TEST-" + String(random(0xffff), HEX);

  Serial.print("Connecting MQTT... ");

  if (!client.connect(testClient.c_str())) {
    Serial.println("FAIL CONNECT");
    return false;
  }

  Serial.println("OK");

  for (int i = 0; i < 15; i++) {
    client.loop();
    delay(100);
  }

  delay(500);
  client.disconnect();
  return true;
}

// =====================================================
// APPLY NEW IP WITH ROLLBACK
// =====================================================

void applyNewIP(IPAddress ip, IPAddress dns, IPAddress gw, IPAddress sn) {
  Serial.println("Áp dụng IP mới...");

  backup_ip      = current_ip;
  backup_dns     = current_dns;
  backup_gateway = current_gateway;
  backup_subnet  = current_subnet;

  applyNetwork(ip, dns, gw, sn);

  if (Ethernet.linkStatus() != LinkON) {
    Serial.println("Link FAIL → ROLLBACK");
    applyNetwork(backup_ip, backup_dns, backup_gateway, backup_subnet);
    return;
  }

  if (testMQTT()) {
    Serial.println("IP OK → LƯU VĨNH VIỄN");
    current_ip      = ip;
    current_dns     = dns;
    current_gateway = gw;
    current_subnet  = sn;
    saveNetworkConfig(ip, dns, gw, sn);
  } else {
    Serial.println("MQTT FAIL → ROLLBACK");
    applyNetwork(backup_ip, backup_dns, backup_gateway, backup_subnet);
  }
}

// =====================================================
// CALLBACK
// =====================================================

void callback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.print("→ [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(msg);

  // ── Nhận tín hiệu từ thiết bị GỬI → điều khiển relay output ──
  if (String(topic) == sender_data_topic) {
    int state = msg.toInt();
    if (state == 0 || state == 1) {
      Serial.print("Nhận tín hiệu từ sender: ");
      Serial.println(state);
      setRelay(state);
    }
    return;
  }

  // ── Nhận lệnh điều khiển trực tiếp qua cmd_topic (override thủ công) ──
  if (String(topic) == cmd_topic) {
    int cmd = msg.toInt();
    if (cmd == 0 || cmd == 1) {
      Serial.print("Lệnh thủ công CMD: ");
      Serial.println(cmd);
      setRelay(cmd);
    }
    return;
  }

  // ── Nhận cấu hình IP mới ──
  if (String(topic) == config_topic) {
    IPAddress new_ip, new_gw, new_sn, new_dns;

    if (!new_ip.fromString(msg.substring(msg.indexOf("ip=") + 3, msg.indexOf(",gateway=")))) return;
    if (!new_gw.fromString(msg.substring(msg.indexOf("gateway=") + 8, msg.indexOf(",subnet=")))) return;
    if (!new_sn.fromString(msg.substring(msg.indexOf("subnet=") + 7, msg.indexOf(",dns=")))) return;
    if (!new_dns.fromString(msg.substring(msg.indexOf("dns=") + 4))) return;

    applyNewIP(new_ip, new_dns, new_gw, new_sn);
  }
}

// =====================================================
// MQTT RECONNECT
// =====================================================

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32-RX-" + String(random(0xffff), HEX);

    Serial.print("MQTT reconnect... ");
    Serial.println(clientId);

    if (client.connect(clientId.c_str(), status_topic.c_str(), 1, true, "OFFLINE")) {
      Serial.println("MQTT connected");

      // Hoàn tất MQTT CONNACK handshake
      for (int i = 0; i < 15; i++) {
        client.loop();
        delay(100);
      }

      delay(300);

      // Subscribe topics
      client.subscribe(sender_data_topic.c_str());  // Tín hiệu từ thiết bị gửi
      client.subscribe(cmd_topic.c_str());           // Lệnh thủ công
      client.subscribe(config_topic.c_str());        // Cấu hình IP

      delay(300);

      // Publish trạng thái thiết bị nhận
      Serial.print("Publishing ONLINE to "); Serial.println(status_topic);
      client.publish(status_topic.c_str(), "ONLINE", true);
      delay(200);

      Serial.print("Publishing IP to "); Serial.println(ip_topic);
      client.publish(ip_topic.c_str(), getCurrentIPString().c_str(), true);
      delay(200);

      Serial.print("Publishing relay state to "); Serial.println(data_topic);
      client.publish(data_topic.c_str(), String(relay_state).c_str(), true);

      Serial.println("All topics published successfully");
      Serial.print("Subscribed sender data: "); Serial.println(sender_data_topic);
    } else {
      Serial.print("Fail rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  // Khởi tạo chân relay OUTPUT
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Mặc định tắt relay

  SPI.begin(W5500_SCK, SPI_MISO, SPI_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);

  loadNetworkConfig();
  applyNetwork(current_ip, current_dns, current_gateway, current_subnet);

  String macStr = getMacString();

  // Topics của THIẾT BỊ NHẬN (self)
  cmd_topic    = "device/" + macStr + "/cmd";
  data_topic   = "device/" + macStr + "/data";
  status_topic = "device/" + macStr + "/status";
  config_topic = "device/" + macStr + "/config";
  ip_topic     = "device/" + macStr + "/ip";

  // Topic của THIẾT BỊ GỬI (cần subscribe để nhận tín hiệu)
  sender_data_topic = "device/" SENDER_MAC "/data";

  client.setServer(MQTT_SERVER, MQTT_PORT);
  client.setSocketTimeout(10);
  client.setCallback(callback);

  Serial.println("\n=== ESP32 RECEIVER + W5500 READY ===");
  Serial.print("MAC thiết bị nhận : "); Serial.println(macStr);
  Serial.print("IP                : "); Serial.println(getCurrentIPString());
  Serial.print("Subscribe sender  : "); Serial.println(sender_data_topic);
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  if (!client.connected()) reconnect();

  client.loop();

  unsigned long now = millis();

  if (now - lastIpPublish >= IP_PUBLISH_INTERVAL) {
    lastIpPublish = now;
    client.publish(ip_topic.c_str(),   getCurrentIPString().c_str(), true);
    client.publish(data_topic.c_str(), String(relay_state).c_str(),  true);
  }
}
