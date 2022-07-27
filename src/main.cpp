#include <Arduino.h>
#include <AsyncUDP.h>
#include <WiFi.h>
#include <esp32-ps2dev.h>  // Emulate a PS/2 device
#include <esp_wifi.h>

#include "secrets.h"  // Contains the WiFi credentials

class Ps2ipPacket {
 public:
  // /**
  //  * @brief Construct a new Ps2ipPacket object.
  //  *
  //  * @param type packet type. 'M' for mouse, 'K' for keyboard.
  //  * @param len length of data. 1-16.
  //  * @param data data[0] to data[len - 1] will be copied to this object.
  //  */
  // Ps2ipPacket(char type, uint8_t len, uint8_t *data) {
  //   this->type = type;
  //   this->len = len;
  //   for (int i = 0; i < len; i++) {
  //     this->data[i] = data[i];
  //   }
  // }
  char type;
  uint8_t len;
  uint8_t data[16];
  boolean is_valid_mouse_packet() {
    // validate packet type
    if (type != 'M') return false;
    // validate packet length
    if (len != 4) return false;
    // check if 1st byte's 4th bit is 1
    if ((data[0] & 0x08) != 0x08) return false;
    // check if 4th byte is between -8 and 7
    int8_t x = data[3];
    if (x < -8 || x > 7) return false;

    return true;
  }
};

const uint8_t LED_BUILTIN = 2;

const uint32_t MAIN_LOOP_DELAY = 1000;
// Some hosts do not send enable data reporting command.
// Enable these flags to allow ESP32 to send data report unconditionally.
const boolean FORCE_DATA_REPORTING_KEYBOARD = true;
const boolean FORCE_DATA_REPORTING_MOUSE = false;

const IPAddress IP(172, 21, 186, 100);
const IPAddress GATEWAY(172, 21, 186, 2);
const IPAddress SUBNET_MASK(255, 255, 255, 0);
const IPAddress DNS(172, 21, 186, 1);

AsyncUDP Udp;
const uint16_t UDP_PORT = 3252;

void mouse_handle_host_msg(void *args);
void keyboard_handle_host_msg(void *args);
void mouse_write(void *args);
void keyboard_write(void *args);

esp32_ps2dev::PS2Mouse mouse(17, 16);
esp32_ps2dev::PS2Keyboard keyboard(19, 18);

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);

  WiFi.config(IP, GATEWAY, SUBNET_MASK, DNS);
  delay(10);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  esp_wifi_set_ps(WIFI_PS_NONE);  // Disable power saving to reduce latency

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  Udp.listen(UDP_PORT);
  Serial.println("Started UDP server on port " + String(UDP_PORT));
  Udp.onPacket([&](AsyncUDPPacket packet) {
    Ps2ipPacket ps2ip_packet;

    // read type field
    int ret = packet.read();
    if (ret == -1) {
      Serial.println("packet.read() failed.");
      return;
    }
    ps2ip_packet.type = ret;
    if (ps2ip_packet.type != 'M' && ps2ip_packet.type != 'K') {
      Serial.println("Invalid type field");
      return;
    }

    // read length field
    ret = packet.read();
    if (ret == -1) {
      Serial.println("packet.read() failed.");
      return;
    }
    ps2ip_packet.len = ret;
    if (ps2ip_packet.len > sizeof(ps2ip_packet.data)) {
      Serial.println("Invalid length field");
      return;
    }

#ifdef _ESP32_PS2IP_DEBUG_
    Serial.printf("ps2ip_packet.type: %c\n", ps2ip_packet.type);
    Serial.printf("ps2ip_packet.len: %d\n", ps2ip_packet.len);
#endif

    ret = packet.read(ps2ip_packet.data, ps2ip_packet.len);
    if (ret < ps2ip_packet.len) {
      Serial.println("packet.read() unexpectedly reached end of packet.");
      return;
    }

    esp32_ps2dev::PS2Packet ps2_packet;
    if (ps2ip_packet.type == 'K') {
#ifdef _ESP32_PS2IP_DEBUG_
      Serial.println("Recieved keyboard packet.");
#endif
      if (!keyboard.data_reporting_enabled() && !FORCE_DATA_REPORTING_KEYBOARD) return;
      ps2_packet.len = ps2ip_packet.len;
      memcpy(ps2_packet.data, ps2ip_packet.data, ps2_packet.len);
      keyboard.send_packet(&ps2_packet);
    } else if (ps2ip_packet.type == 'M') {
#ifdef _ESP32_PS2IP_DEBUG_
      Serial.println("Recieved mouse packet.");
#endif
      if (!mouse.data_reporting_enabled() && !FORCE_DATA_REPORTING_MOUSE) return;
      ps2_packet.len = 3 + mouse.has_wheel(); // truncate to 3 bytes if ESP32 is working as a mouse without a wheel
      memcpy(ps2_packet.data, ps2ip_packet.data, ps2_packet.len);
      if (mouse.has_4th_and_5th_buttons()) ps2_packet.data[3] &= 0x0F;
      mouse.send_packet(&ps2_packet);
    }
  });

  Serial.println("Setup complete.");
}

void loop() { delay(MAIN_LOOP_DELAY); }
