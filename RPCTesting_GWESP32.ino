#ifdef ESP8266
  #include <ESP8266WiFi.h>
#else
  #ifdef ESP32
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
  #endif
#endif

#include <Arduino_MQTT_Client.h>
#include <Server_Side_RPC.h>
#include <ThingsBoard.h>
#include <ArduinoJson.h>

// ==================== CẤU HÌNH CHUNG ====================
#define ENCRYPTED  false   // Nếu không dùng TLS

// WiFi credentials
constexpr char WIFI_SSID[]     = "eoh.io";
constexpr char WIFI_PASSWORD[] = "Eoh@2020";

// ThingsBoard credentials
constexpr char TOKEN[]         = "9SYcguos1RP0ItmwYo1u";
constexpr char THINGSBOARD_SERVER[] = "local-thingsboard.eoh.io";
#if ENCRYPTED
  constexpr uint16_t THINGSBOARD_PORT = 8883U;
#else
  constexpr uint16_t THINGSBOARD_PORT = 1884U;
#endif

constexpr uint16_t MAX_MESSAGE_SIZE = 512U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

// GPIO bạn muốn điều khiển (ví dụ GPIO2)
constexpr uint8_t CONTROL_PIN = 2;

// Tên hai RPC method cho widget:
constexpr char RPC_GET_VALUE[] = "getValue";
constexpr char RPC_SET_VALUE[] = "setValue";

// Tên key telemetry (nếu muốn gửi trạng thái sau khi set)
constexpr char TELEMETRY_KEY_STATE[] = "state";

#if ENCRYPTED
  // Nếu dùng TLS, chèn ROOT CA ở đây
  constexpr char ROOT_CERT[] = R"(-----BEGIN CERTIFICATE----- ... -----END CERTIFICATE-----)";
#endif

// ==================== ĐỐI TƯỢNG MQTT, RPC, TB ====================
#if ENCRYPTED
  WiFiClientSecure espClient;
#else
  WiFiClient       espClient;
#endif

Arduino_MQTT_Client mqttClient(espClient);
constexpr uint8_t MAX_RPC_SUBS = 2, MAX_RPC_RESP = 2;
Server_Side_RPC<MAX_RPC_SUBS, MAX_RPC_RESP> rpc;
const std::array<IAPI_Implementation*, 1U> apis = { &rpc };
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE, Default_Max_Stack_Size, apis);

bool subscribed = false;  // cờ kiểm tra đã subscribe chưa

// ==================== HÀM GỬI TELEMETRY (NẾU CẦN) ====================
void sendStateTelemetry(int value) {
  StaticJsonDocument<64> telem;
  telem[TELEMETRY_KEY_STATE] = value;
  size_t jsonSize = measureJson(telem);
  if (tb.sendTelemetryJson(telem, jsonSize)) {
    Serial.printf("→ Đã gửi telemetry: %s = %d\n", TELEMETRY_KEY_STATE, value);
  } else {
    Serial.println("→ Gửi telemetry thất bại!");
  }
}

// ==================== CALLBACK RPC getValue ====================
void processGetValue(const JsonVariantConst &data, JsonDocument &response) {
  Serial.println("Received RPC method: getValue");
  // Đọc trạng thái chân CONTROL_PIN (0 hoặc 1)
  int current = digitalRead(CONTROL_PIN);
  Serial.printf("  - Trả về giá trị current: %d\n", current);
  // Trả về JSON { "": current } 
  // Thật ra callback chỉ cần gán giá trị dạng số, ThingsBoard sẽ hiểu
  // response sẽ chỉ có 1 field, ví dụ "value": current, nhưng widget chỉ cần số
  response["value"] = current;
  // Nếu widget parse data trực tiếp, “data” trong JS sẽ là số 0/1
}

// ==================== CALLBACK RPC setValue ====================
void processSetValue(const JsonVariantConst &data, JsonDocument &response) {
  Serial.println("Received RPC method: setValue");
  // data ở đây sẽ là số (1) hoặc (0) do widget gửi
  int newState = data.as<int>();  // data là integer
  Serial.printf("  - Yêu cầu setValue: %d\n", newState);
  // Nếu newState = 1 → bật GPIO; if 0 → tắt GPIO
  if (newState == 1) {
    digitalWrite(CONTROL_PIN, HIGH);
    Serial.printf("  -> GPIO%d = HIGH\n", CONTROL_PIN);
     sendStateTelemetry(1);
  } else {
    digitalWrite(CONTROL_PIN, LOW);
    Serial.printf("  -> GPIO%d = LOW\n", CONTROL_PIN);
    sendStateTelemetry(0);
  }
  // Gán response (nếu widget chờ Response) – chúng ta trả về object JSON nhỏ
  response["result"] = String("OK");
}

// ==================== HÀM KẾT NỐI WI-FI ====================
void initWiFi() {
  Serial.printf("Connecting to WiFi: %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected! IP = " + WiFi.localIP().toString());
#if ENCRYPTED
  espClient.setCACert(ROOT_CERT);
#endif
}

bool reconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  initWiFi();
  return (WiFi.status() == WL_CONNECTED);
}

// ==================== SETUP & LOOP CHÍNH ====================
void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  delay(1000);
  Serial.println("=== START SWITCH CONTROL EXAMPLE ===");

  // Thiết lập chân IO
  pinMode(CONTROL_PIN, OUTPUT);
  digitalWrite(CONTROL_PIN, LOW); // Mặc định tắt

  // Kết nối Wi-Fi
  initWiFi();
}


void loop() {
  delay(1000);

  // 1. Nếu mất Wi-Fi, reconnect
  if (!reconnectWiFi()) return;

  // 2. Nếu chưa kết nối TB, connect lại
  if (!tb.connected()) {
    Serial.printf("Connecting to TB: %s (token=%s) ...\n", THINGSBOARD_SERVER, TOKEN);
    if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
      Serial.println("  → Connect TB failed. Retry in 5s");
      delay(5000);
      return;
    }
    Serial.println("  → Connected to ThingsBoard!");
    // Đánh dấu chưa subscribe RPC lần nào kể từ sau khi vừa connect
    subscribed = false;
  }

  // 3. Nếu chưa subscribe RPC (hoặc subscribe lần đầu sau reconnect)
  if (!subscribed) {
    Serial.println("Subscribing RPC methods...");
    const std::array<RPC_Callback, MAX_RPC_SUBS> callbacks = {
      RPC_Callback{ RPC_GET_VALUE, processGetValue },
      RPC_Callback{ RPC_SET_VALUE, processSetValue }
    };
    if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
      Serial.println("  → Subscribe RPC thất bại!");
      // *** Thay đổi: dù thất bại vẫn đánh dấu subscribed = true,
      // để không spam thêm thật nhiều subscription request lên server ***
      subscribed = true;
      // Nếu muốn retry sau 10s, bạn có thể thêm delay như sau:
      // delay(10000);
    } else {
      Serial.println("  → Subscribe RPC xong");
      subscribed = true;
    }
  }

  // 4. Luôn gọi tb.loop() để nhận RPC, giữ kết nối v.v.
  tb.loop();
}
