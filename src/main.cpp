#include <Arduino.h>
#include <AsyncUDP.h>
#include <WiFi.h>
#include <esp32-ps2dev.h>  // Emulate a PS/2 device
#include <esp_wifi.h>

#include "ps2_keyboard.cpp"  // Contains the PS/2 keyboard code
#include "ps2_mouse.cpp"     // Contains the PS/2 mouse code
#include "secrets.h"         // Contains the WiFi credentials

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

TaskHandle_t thp[4];
QueueHandle_t xQueue_mouse_code;
QueueHandle_t xQueue_keyboard_code;
const size_t MOUSE_QUEUE_LEN = 30;
const size_t KEYBOARD_QUEUE_LEN = 30;
const BaseType_t CORE_ID_MOUSE_LOOP = 1;
const BaseType_t CORE_ID_KEYBOARD_LOOP = 0;
const BaseType_t CORE_ID_MOUSE_WRITE = 1;
const BaseType_t CORE_ID_KEYBOARD_WRITE = 0;
const UBaseType_t PRIORITY_MOUSE_LOOP = 3;
const UBaseType_t PRIORITY_KEYBOARD_LOOP = 2;
const UBaseType_t PRIORITY_MOUSE_WRITE = 5;
const UBaseType_t PRIORITY_KEYBOARD_WRITE = 4;
const uint32_t MOUSE_LOOP_DELAY = 9;
const uint32_t KEYBOARD_LOOP_DELAY = 9;
const uint32_t MAIN_LOOP_DELAY = 1000;
// Some hosts do not send enable data reporting command.
// Enable these flags to allow ESP32 to send data report unconditionally.
const boolean FORCE_DATA_REPORTING_KEYBOARD = true;
const boolean FORCE_DATA_REPORTING_MOUSE = false;

const IPAddress IP(172, 21, 186, 100);
const IPAddress GATEWAY(172, 21, 186, 2);
const IPAddress SUBNET_MASK(255, 255, 255, 0);
const IPAddress DNS(172, 21, 186, 1);

portMUX_TYPE bus_mutex = portMUX_INITIALIZER_UNLOCKED;
PS2mouse mouse(17, 16);        // clock, data
PS2keyboard keyboard(19, 18);  // clock, data

AsyncUDP Udp;
const uint16_t UDP_PORT = 3252;

void mouse_handle_host_msg(void *args);
void keyboard_handle_host_msg(void *args);
void mouse_write(void *args);
void keyboard_write(void *args);

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
  Udp.onPacket([](AsyncUDPPacket packet) {
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

#ifdef _DBG_ESP32_PS2_EMU_
    Serial.printf("type: %c\n", type);
    Serial.printf("length: %u\n", length);
#endif

    ret = packet.read(ps2ip_packet.data, ps2ip_packet.len);
    if (ret < ps2ip_packet.len) {
      Serial.println("packet.read() unexpectedly reached end of packet.");
      return;
    }

    if (ps2ip_packet.type == 'K') {
#ifdef _DBG_ESP32_PS2_EMU_
      Serial.println("Recieved keyboard packet.");
#endif
      xQueueSend(xQueue_keyboard_code, &ps2ip_packet, 0);
    } else if (ps2ip_packet.type == 'M') {
#ifdef _DBG_ESP32_PS2_EMU_
      Serial.println("Recieved mouse packet.");
#endif
      xQueueSend(xQueue_mouse_code, &ps2ip_packet, 0);
    }
  });

  xQueue_mouse_code = xQueueCreate(MOUSE_QUEUE_LEN, sizeof(Ps2ipPacket));
  xQueue_keyboard_code = xQueueCreate(KEYBOARD_QUEUE_LEN, sizeof(Ps2ipPacket));

  // function, task_name, stack_size, NULL, task_handle, priority, core_id
  xTaskCreateUniversal(mouse_handle_host_msg, "mouse_handle_host_msg", 4096, NULL, PRIORITY_MOUSE_LOOP, &thp[0], CORE_ID_MOUSE_LOOP);
  Serial.println("Created [mouse_handle_host_msg] task");
  xTaskCreateUniversal(keyboard_handle_host_msg, "keyboard_handle_host_msg", 4096, NULL, PRIORITY_KEYBOARD_LOOP, &thp[1],
                       CORE_ID_KEYBOARD_LOOP);
  Serial.println("Created [keyboard_handle_host_msg] task");
  xTaskCreateUniversal(mouse_write, "mouse_write", 4096, NULL, PRIORITY_MOUSE_WRITE, &thp[2], CORE_ID_MOUSE_WRITE);
  Serial.println("Created [mouse_write] task");

  Serial.println("Setup complete.");
}

void mouse_write(void *args) {
  uint64_t last_time = 0;

  while (true) {
    Ps2ipPacket pkt;
    xQueueReceive(xQueue_mouse_code, &pkt, portMAX_DELAY);

    int64_t time_passed_us = micros() - last_time;
    int64_t min_report_interval_us = 1000000 / mouse.sample_rate;
    if (time_passed_us < min_report_interval_us) {
      delayMicroseconds(min_report_interval_us - time_passed_us);
    }
    last_time = micros();

    portENTER_CRITICAL(&bus_mutex);

    pkt.len = 3 + mouse.has_wheel;

    if (pkt.is_valid_mouse_packet() && (mouse.data_report_enabled || FORCE_DATA_REPORTING_MOUSE)) {
      for (size_t i = 0; i < pkt.len; i++) {
        if (mouse.write(pkt.data[i]) != 0) {
          Serial.println("Warning: mouse_write: Data report interrupted");
          break;
        }
      }
    }

    portEXIT_CRITICAL(&bus_mutex);

    delay(1);
  }
}

void mouse_handle_host_msg(void *args) {  //スレッド ②
  // mouse_init
  while (mouse.write(0xAA) != 0) delay(1);
  while (mouse.write(0x00) != 0) delay(1);
  Serial.println("Mouse initialized");

  while (1) {
    portENTER_CRITICAL(&bus_mutex);
    mouse.mouse_handle();
    portEXIT_CRITICAL(&bus_mutex);

    digitalWrite(LED_BUILTIN, mouse.data_report_enabled);

    delay(MOUSE_LOOP_DELAY);
  }
}

void keyboard_write(void *args) {
  portENTER_CRITICAL(&bus_mutex);

  Ps2ipPacket pkt;
  if (xQueuePeek(xQueue_keyboard_code, &pkt, 0) != pdTRUE) {
    goto FINALLY;
  }

  if (keyboard.data_report_enabled || FORCE_DATA_REPORTING_KEYBOARD) {
    int ret = keyboard.write_multi(pkt.len, pkt.data);
    if (ret != 0) {
      Serial.println("Warning: keyboard_write: Data report interrupted");
    } else {
      xQueueReceive(xQueue_keyboard_code, &pkt, 0);
    }
  }

FINALLY:
  portEXIT_CRITICAL(&bus_mutex);
  vTaskDelete(NULL);
}

void keyboard_handle_host_msg(void *args) {
  while (keyboard.write(0xAA) != 0) delay(1);
  Serial.println("Keyboard initialized");

  while (1) {
    uint8_t leds;
    if (keyboard.keyboard_handle(&leds)) {
      Serial.print("LEDS");
      Serial.println(leds, HEX);
      digitalWrite(LED_BUILTIN, leds);
    }

    Ps2ipPacket pkt;
    if (xQueuePeek(xQueue_keyboard_code, &pkt, pdMS_TO_TICKS(KEYBOARD_LOOP_DELAY)) == pdTRUE) {
      TaskHandle_t thp;
      xTaskCreateUniversal(keyboard_write, "keyboard_write", 4096, NULL, PRIORITY_KEYBOARD_WRITE, &thp, CORE_ID_KEYBOARD_WRITE);
    }
  }
}

void loop() { delay(MAIN_LOOP_DELAY); }
