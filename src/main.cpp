#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ps2dev.h>  // Emulate a PS/2 device

#include "ps2_keyboard.cpp"  // Contains the PS/2 keyboard code
#include "ps2_mouse.cpp"     // Contains the PS/2 mouse code
#include "secrets.h"         // Contains the WiFi credentials

const uint8_t LED_BUILTIN = 2;

TaskHandle_t thp[3];
QueueHandle_t xQueue_mouse_code;
QueueHandle_t xQueue_keyboard_code;
const size_t MOUSE_QUEUE_LEN = 30;
const size_t KEYBOARD_QUEUE_LEN = 30;
const size_t MOUSE_QUEUE_ITEM_SIZE = 5;
const size_t KEYBOARD_QUEUE_ITEM_SIZE = 9;
const BaseType_t CORE_ID_MOUSE_LOOP = 1;
const BaseType_t CORE_ID_KEYBOARD_LOOP = 0;
const BaseType_t CORE_ID_MOUSE_WRITE = 1;
const BaseType_t CORE_ID_KEYBOARD_WRITE = 0;
const UBaseType_t PRIORITY_MOUSE_LOOP = 3;
const UBaseType_t PRIORITY_KEYBOARD_LOOP = 2;
const UBaseType_t PRIORITY_MOUSE_WRITE = 5;
const UBaseType_t PRIORITY_KEYBOARD_WRITE = 4;
const uint32_t MOUSE_LOOP_DELAY = 8;
const uint32_t KEYBOARD_LOOP_DELAY = 8;
const uint32_t MAIN_LOOP_DELAY = 10;

const IPAddress IP(172, 21, 186, 100);
const IPAddress GATEWAY(172, 21, 186, 2);
const IPAddress SUBNET_MASK(255, 255, 255, 0);
const IPAddress DNS(172, 21, 186, 1);

WiFiUDP Udp;
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

  Udp.begin(UDP_PORT);
  Serial.println("Started UDP server on port " + String(UDP_PORT));

  xQueue_mouse_code = xQueueCreate(MOUSE_QUEUE_LEN, MOUSE_QUEUE_ITEM_SIZE);
  xQueue_keyboard_code = xQueueCreate(KEYBOARD_QUEUE_LEN, KEYBOARD_QUEUE_ITEM_SIZE);

  // function, task_name, stack_size, NULL, task_handle, priority, core_id
  xTaskCreateUniversal(mouse_loop, "mouse_loop", 4096, NULL, PRIORITY_MOUSE_LOOP, &thp[0], CORE_ID_MOUSE_LOOP);
  Serial.println("Created [mouse_loop] task");
  xTaskCreateUniversal(keyboard_loop, "keyboard_loop", 4096, NULL, PRIORITY_KEYBOARD_LOOP, &thp[1], CORE_ID_KEYBOARD_LOOP);
  Serial.println("Created [keyboard_loop] task");

  Serial.println("Setup complete.");
}

typedef struct {
  PS2mouse *mouse;
  uint8_t len;
  uint8_t data[16];
} mouse_write_args_t;

void mouse_write(void *args) {
  mouse_write_args_t *args_p = (mouse_write_args_t *)args;
  PS2mouse *mouse = args_p->mouse;
  uint8_t len = args_p->len;
  uint8_t *data = args_p->data;

  if ((len == 3 + mouse->has_wheel) && mouse->data_report_enabled) {
    int ret = mouse->write_multi(len, data);
    if (ret != 0) {
      Serial.println("Error: mouse_write: Data report interrupted");
    }
  } else {
    Serial.println("Error: mouse_write: Legnth of data report is invalid");
  }

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

    uint8_t msg[MOUSE_QUEUE_ITEM_SIZE];
    if (xQueueReceive(xQueue_mouse_code, msg, 0) == pdTRUE) {
      mouse_write_args_t *args_p = new mouse_write_args_t;
      args_p->mouse = &mouse;
      args_p->len = 3 + mouse.has_wheel;
      for (int i = 0; i < args_p->len; i++) {
        args_p->data[i] = msg[i + 1];
      }
      TaskHandle_t thp;
      xTaskCreateUniversal(mouse_write, "mouse_write", 4096, args_p, PRIORITY_MOUSE_WRITE, &thp, CORE_ID_MOUSE_WRITE);
    }
    delay(MOUSE_LOOP_DELAY);
  }
}

typedef struct {
  PS2keyboard *keyboard;
  uint8_t len;
  uint8_t data[16];
} keyboard_write_args_t;

void keyboard_write(void *args) {
  keyboard_write_args_t *args_p = (keyboard_write_args_t *)args;
  PS2keyboard *keyboard = args_p->keyboard;
  uint8_t len = args_p->len;
  uint8_t *data = args_p->data;

  keyboard->write_multi(len, data);

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

    uint8_t msg[KEYBOARD_QUEUE_ITEM_SIZE];
    if (xQueueReceive(xQueue_keyboard_code, msg, 0) == pdTRUE) {
      keyboard_write_args_t *args_p = new keyboard_write_args_t;
      args_p->keyboard = &keyboard;
      args_p->len = msg[0];
      for (int i = 0; i < args_p->len; i++) {
        args_p->data[i] = msg[i + 1];
      }
      TaskHandle_t thp;
      xTaskCreateUniversal(keyboard_write, "keyboard_write", 4096, args_p, PRIORITY_KEYBOARD_WRITE, &thp, CORE_ID_KEYBOARD_WRITE);
    }
    delay(KEYBOARD_LOOP_DELAY);
  }
}

void loop() {
  // note: recieved packets are sometimes combined into one packet.
  int size = Udp.parsePacket();
  if (size) {
#ifdef _DBG_ESP32_PS2_EMU_
    Serial.println();
    Serial.print("size:");
    Serial.println(size);
#endif

    for (size_t i = 0; i < size; i++) {
      // read type field
      int ret = Udp.read();
      if (ret == -1) {
        Serial.println("Udp.read() failed while reading type field");
        break;
      }
      i++;
      uint8_t type = ret;

      // read length field
      ret = Udp.read();
      if (ret == -1) {
        Serial.println("Udp.read() failed while reading length field");
        break;
      }
      i++;
      uint8_t length = ret;

#ifdef _DBG_ESP32_PS2_EMU_
      Serial.printf("type: %c\n", type);
      Serial.printf("length: %u\n", length);
#endif

      size_t queue_item_size = std::max(MOUSE_QUEUE_ITEM_SIZE, KEYBOARD_QUEUE_ITEM_SIZE);
      uint8_t queue_item[queue_item_size];
      queue_item[0] = length;
      if (length > queue_item_size - 1) {
        Serial.println("invalid length field in received packet");
        break;
      }
      for (size_t j = 0; j < length; j++) {
        ret = Udp.read();
        if (ret == -1) {
          Serial.println("Udp.read() failed while reading data");
          break;
        }
        i++;
        queue_item[j + 1] = ret;
      }

      if (type == 'K') {
#ifdef _DBG_ESP32_PS2_EMU_
        Serial.println("Recieved keyboard packet.");
#endif
        xQueueSend(xQueue_keyboard_code, queue_item, 0);
      } else if (type == 'M') {
#ifdef _DBG_ESP32_PS2_EMU_
        Serial.println("Recieved mouse packet.");
#endif
        xQueueSend(xQueue_mouse_code, queue_item, 0);
      }
    }
  }
  delay(MAIN_LOOP_DELAY);
}
