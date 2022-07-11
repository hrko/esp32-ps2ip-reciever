#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ps2dev.h>  // Emulate a PS/2 device

#include "secrets.h"  // Contains the WiFi credentials

const IPAddress IP(172, 21, 186, 100);
const IPAddress GATEWAY(172, 21, 186, 2);
const IPAddress SUBNET_MASK(255, 255, 255, 0);
const IPAddress DNS(172, 21, 186, 1);

WiFiUDP Udp;
const uint16_t UDP_PORT = 3252;

PS2dev keyboard(19, 18);  // clock, data
PS2dev mouse(17, 16);     // clock, data

const uint8_t LED_BUILTIN = 2;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

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

  Udp.begin(UDP_PORT);

  keyboard.keyboard_init();
}

void loop() {
  unsigned char leds;
  if (keyboard.keyboard_handle(&leds)) {
    Serial.print('LEDS');
    Serial.println(leds, HEX);
    digitalWrite(LED_BUILTIN, leds);
  }

  int size = Udp.parsePacket();
  if (size) {
    Serial.println();
    Serial.print("size:");
    Serial.println(size);

    for (size_t i = 0; i < size; i++) {
      uint8_t c = Udp.read();
      if (c == 'K') {
        Serial.println("Recieved keyboard input.");
        uint8_t c = Udp.read();
        i++;
        uint8_t length = c;
        for (size_t j = 0; j < length; j++) {
          uint8_t c = Udp.read();
          i++;
          Serial.print("Sending scan code: ");
          Serial.println(c, HEX);
          keyboard.write(c);
        }
      }
      if (c == 'M') {
        Serial.println("Recieved mouse input.");
        uint8_t c = Udp.read();
        i++;
        uint8_t length = c;
        for (size_t j = 0; j < length; j++) {
          uint8_t c = Udp.read();
          i++;
          // just ignore for now
        }
      }
      if (c == 'W') {
        Serial.println("Recieved wait.");
        uint8_t c = Udp.read();
        i++;
        uint8_t length = c;
        for (size_t j = 0; j < length; j++) {
          uint8_t c = Udp.read();
          i++;
          // just ignore for now
        }
      }
    }
  }
}