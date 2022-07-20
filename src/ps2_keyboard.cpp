#include <ps2dev.h>

class PS2keyboard : public PS2dev {
 public:
  PS2keyboard(int clk, int data) : PS2dev(clk, data) {}
  void ack() {
    // while (write(0xFA)) delay(1);
    write(0xFA);
    Serial.println("Sent keyboard ACK");
  }
  uint8_t _last_packet_len = 0;
  uint8_t _last_packet_data[16];
  /**
   * @brief send multi-byte data to the host 
   * 
   * @param len amount of data in byte
   * @param data data to send to the host
   * @return -1 on fail, 0 on success
   */
  int write_multi(uint8_t len, uint8_t *data) {
    boolean failed = false;
    for (size_t i = 0; i < len; i++) {
      int ret = write(data[i]);
      if (ret != 0) {
        failed = true;
        return -1;
      }
    }
    for (size_t i = 0; i < len; i++) {
      _last_packet_data[i] = data[i];
    }
    return 0;
  }
  int keyboard_reply(uint8_t cmd, unsigned char *leds) {
    unsigned char val;
    unsigned char enabled;
    switch (cmd) {
      case 0xFF:  // reset
        ack();
        // the while loop lets us wait for the host to be ready
        while (write(0xAA) != 0) delay(1);
        break;
      case 0xFE:  // resend
        ack();
        break;
      case 0xF6:  // set defaults
        // enter stream mode
        ack();
        break;
      case 0xF5:  // disable data reporting
        // FM
        enabled = 0;
        ack();
        break;
      case 0xF4:  // enable data reporting
        // FM
        enabled = 1;
        ack();
        break;
      case 0xF3:  // set typematic rate
        ack();
        if (!read(&val)) ack();  // do nothing with the rate
        break;
      case 0xF2:  // get device id
        ack();
        write(0xAB);
        write(0x83);
        break;
      case 0xF0:  // set scan code set
        ack();
        if (!read(&val)) ack();  // do nothing with the rate
        break;
      case 0xEE:  // echo
        // ack();
        write(0xEE);
        break;
      case 0xED:  // set/reset LEDs
        ack();
        if (!read(&val)) ack();  // do nothing with the rate
#ifdef _PS2DBG
        _PS2DBG.print("LEDs: ");
        _PS2DBG.println(*leds, HEX);
        // digitalWrite(LED_BUILTIN, *leds);
#endif
        return 1;
        break;
    }
    return 0;
  }
  int keyboard_handle(unsigned char *leds) {
    unsigned char c;  // char stores data recieved from computer for KBD
    if (available()) {
      if (!read(&c)) return keyboard_reply(c, leds);
    }
    return 0;
  }
};
