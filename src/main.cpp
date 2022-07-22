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

TaskHandle_t thp[3];
QueueHandle_t xQueue_mouse_code;
QueueHandle_t xQueue_keyboard_code;
const size_t MOUSE_QUEUE_LEN = 30;
const size_t KEYBOARD_QUEUE_LEN = 30;
// const size_t MOUSE_QUEUE_ITEM_SIZE = 5;
// const size_t KEYBOARD_QUEUE_ITEM_SIZE = 9;
const BaseType_t CORE_ID_MOUSE_LOOP = 1;
const BaseType_t CORE_ID_KEYBOARD_LOOP = 0;
const BaseType_t CORE_ID_MOUSE_WRITE = 1;
const BaseType_t CORE_ID_KEYBOARD_WRITE = 0;
const UBaseType_t PRIORITY_MOUSE_LOOP = 3;
const UBaseType_t PRIORITY_KEYBOARD_LOOP = 2;
const UBaseType_t PRIORITY_MOUSE_WRITE = 5;
const UBaseType_t PRIORITY_KEYBOARD_WRITE = 4;
const uint32_t MOUSE_LOOP_DELAY = 7;
const uint32_t KEYBOARD_LOOP_DELAY = 7;
const uint32_t MAIN_LOOP_DELAY = 1000;
const boolean FORCE_DATA_REPORTING_KEYBOARD = true;
const boolean FORCE_DATA_REPORTING_MOUSE = false;

const IPAddress IP(172, 21, 186, 100);
const IPAddress GATEWAY(172, 21, 186, 2);
const IPAddress SUBNET_MASK(255, 255, 255, 0);
const IPAddress DNS(172, 21, 186, 1);

AsyncUDP Udp;
const uint16_t UDP_PORT = 3252;

void mouse_loop(void *args);
void keyboard_loop(void *args);

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
  xTaskCreateUniversal(mouse_loop, "mouse_loop", 4096, NULL, PRIORITY_MOUSE_LOOP, &thp[0], CORE_ID_MOUSE_LOOP);
  Serial.println("Created [mouse_loop] task");
  xTaskCreateUniversal(keyboard_loop, "keyboard_loop", 4096, NULL, PRIORITY_KEYBOARD_LOOP, &thp[1], CORE_ID_KEYBOARD_LOOP);
  Serial.println("Created [keyboard_loop] task");

  Serial.println("Setup complete.");
}

typedef struct {
  PS2mouse *mouse;
  Ps2ipPacket packet;
} mouse_write_args_t;

void mouse_write(void *args) {
  mouse_write_args_t *args_p = (mouse_write_args_t *)args;
  PS2mouse *mouse = args_p->mouse;
  Ps2ipPacket *pkt = &(args_p->packet);

  uint8_t len = 3 + mouse->has_wheel;
  uint8_t *data = pkt->data;

  if (pkt->is_valid_mouse_packet() && (mouse->data_report_enabled || FORCE_DATA_REPORTING_MOUSE)) {
    int ret = mouse->write_multi(len, data);
    if (ret != 0) {
      Serial.println("Warning: mouse_write: Data report interrupted");
    }
  }
  // else {
  //   Serial.println("Error: mouse_write: Legnth of data report is invalid");
  // }

  delete args_p;
  vTaskDelete(NULL);
}

void mouse_loop(void *args) {  //スレッド ②
  PS2mouse mouse(17, 16);      // clock, data

  // mouse_init
  while (mouse.write(0xAA) != 0) delay(1);
  while (mouse.write(0x00) != 0) delay(1);
  Serial.println("Mouse initialized");

  while (1) {
    mouse.mouse_handle();

    digitalWrite(LED_BUILTIN, mouse.data_report_enabled);

    // uint8_t msg[MOUSE_QUEUE_ITEM_SIZE];
    Ps2ipPacket pkt;
    if (xQueueReceive(xQueue_mouse_code, &pkt, 0) == pdTRUE) {
      mouse_write_args_t *args_p = new mouse_write_args_t;
      args_p->mouse = &mouse;
      args_p->packet = pkt;
      // for (int i = 0; i < args_p->len; i++) {
      //   args_p->data[i] = msg[i + 1];
      // }
      TaskHandle_t thp;
      xTaskCreateUniversal(mouse_write, "mouse_write", 4096, args_p, PRIORITY_MOUSE_WRITE, &thp, CORE_ID_MOUSE_WRITE);
    }
    delay(MOUSE_LOOP_DELAY);
  }
}

typedef struct {
  PS2keyboard *keyboard;
  Ps2ipPacket packet;
} keyboard_write_args_t;

void keyboard_write(void *args) {
  keyboard_write_args_t *args_p = (keyboard_write_args_t *)args;
  PS2keyboard *keyboard = args_p->keyboard;
  Ps2ipPacket *pkt = &(args_p->packet);

  uint8_t len = pkt->len;
  uint8_t *data = pkt->data;

  if (keyboard->data_report_enabled || FORCE_DATA_REPORTING_KEYBOARD) {
    int ret = keyboard->write_multi(len, data);
    if (ret != 0) {
      Serial.println("Warning: keyboard_write: Data report interrupted");
    }
  } else {
    Serial.println("Warning: keyboard_write: Data report is disabled");
  }

  delete args_p;
  vTaskDelete(NULL);
}

void keyboard_loop(void *args) {
  PS2keyboard keyboard(19, 18);  // clock, data

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
    if (xQueueReceive(xQueue_keyboard_code, &pkt, 0) == pdTRUE) {
      keyboard_write_args_t *args_p = new keyboard_write_args_t;
      args_p->keyboard = &keyboard;
      args_p->packet = pkt;
      // for (int i = 0; i < args_p->len; i++) {
      //   args_p->data[i] = msg[i + 1];
      // }
      TaskHandle_t thp;
      xTaskCreateUniversal(keyboard_write, "keyboard_write", 4096, args_p, PRIORITY_KEYBOARD_WRITE, &thp, CORE_ID_KEYBOARD_WRITE);
    }
    delay(KEYBOARD_LOOP_DELAY);
  }
}

void loop() {
  delay(MAIN_LOOP_DELAY);
}
