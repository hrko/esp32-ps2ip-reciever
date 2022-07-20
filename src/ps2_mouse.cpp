#include <Arduino.h>
#include <ps2dev.h>

class PS2mouse : public PS2dev {
 public:
  PS2mouse(int clk, int data) : PS2dev(clk, data) {}
  enum MouseResolutionCode { RES_1 = 0x00, RES_2 = 0x01, RES_4 = 0x02, RES_8 = 0x03 };
  enum MouseScaleBit { ONE_ONE = 0, TWO_ONE = 1 };
  enum MouseMode { REMOTE_MODE = 0, STREAM_MODE = 1, WRAP_MODE = 2 };
  void ack() {
    // while (write(0xFA)) delay(1);
    write(0xFA);
    Serial.println("Sent mouse ACK");
  }
  uint8_t _last_sample_rate[3] = {0, 0, 0};
  boolean has_wheel = false;
  uint8_t sample_rate = 100;
  uint64_t _min_report_interval_us = 1000000 / 100;  // (1 sec / sample rate)
  uint8_t resolution = RES_4;
  boolean scale = ONE_ONE;
  boolean data_report_enabled = false;
  uint8_t _mode = STREAM_MODE;
  uint8_t _last_mode = STREAM_MODE;
  int16_t _count_x = 0;
  uint8_t _count_x_overflow = 0;
  int16_t _count_y = 0;
  uint8_t _count_y_overflow = 0;
  int8_t _count_z = 0;
  uint8_t _button_left = 0;
  uint8_t _button_right = 0;
  uint8_t _button_middle = 0;
  uint64_t _last_report_time = 0;
  boolean _count_or_button_changed_since_last_report = false;
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
  void set_count_and_button(int16_t dx, int16_t dy, int8_t dz, boolean left, boolean right, boolean middle) {
    _count_x += dx;
    if (_count_x > 255 || _count_x < -255) {
      _count_x_overflow = 1;
    }
    _count_y += dy;
    if (_count_y > 255 || _count_y < -255) {
      _count_y_overflow = 1;
    }
    _count_z += dz;
    _button_left = left;
    _button_right = right;
    _button_middle = middle;
    _count_or_button_changed_since_last_report = true;
    return;
  }
  void set_mode(uint8_t mode) {
    _last_mode = _mode;
    _mode = mode;
    return;
  }
  void reset_counter() {
    _count_x = 0;
    _count_y = 0;
    _count_z = 0;
    _count_x_overflow = 0;
    _count_y_overflow = 0;
    _count_or_button_changed_since_last_report = false;
    return;
  }
  // void move(int16_t x, int16_t y, int8_t z) {
  //   _count_x += x;
  //   _count_y += y;
  //   _count_z += z;
  //   if (_mode == STREAM_MODE) {
  //     int64_t min_report_interval_us = 1000000 / sample_rate;
  //     int64_t now = esp_timer_get_time();
  //     if (now - _last_report_time > min_report_interval_us) {
  //       report();
  //     }
  //   }
  // }
  void report() {
    // uint8_t button_left_bit = _button_left;
    // uint8_t button_right_bit = _button_right;
    // uint8_t button_middle_bit = _button_middle;
    // const uint8_t x_sign_bit = ((_count_x & 0x0100) >> 8);
    // const uint8_t y_sign_bit = ((_count_y & 0x0100) >> 8);
    // uint8_t x_overflow_bit = 0;
    // uint8_t y_overflow_bit = 0;
    if (scale == TWO_ONE) {
      int16_t *p[2] = {&_count_x, &_count_y};
      for (size_t i = 0; i < 2; i++) {
        boolean positive = *p[i] >= 0;
        uint16_t abs_value = positive ? *p[i] : -*p[i];
        switch (abs_value) {
          case 1:
            abs_value = 1;
            break;
          case 2:
            abs_value = 1;
            break;
          case 3:
            abs_value = 3;
            break;
          case 4:
            abs_value = 6;
            break;
          case 5:
            abs_value = 9;
            break;
          default:
            abs_value *= 2;
            break;
        }
        if (!positive) *p[i] = -abs_value;
      }
    }
    uint8_t len = 3 + has_wheel;
    uint8_t data[4];
    data[0] = (_button_left) & ((_button_right) << 1) & ((_button_middle) << 2) & (1 << 3) & (((_count_x & 0x0100) >> 8) << 4) &
              (((_count_y & 0x0100) >> 8) << 5) & (_count_x_overflow << 6) & (_count_y_overflow << 7);
    data[1] = _count_x & 0xFF;
    data[2] = _count_y & 0xFF;
    data[3] = _count_z & 0x0F;
    // write((_button_left) & ((_button_right) << 1) & ((_button_middle) << 2) & (1 << 3) &
    //       (((_count_x & 0x0100) >> 8) << 4) & (((_count_y & 0x0100) >> 8) << 5) & (_count_x_overflow << 6) &
    //       (_count_y_overflow << 7));
    // write(_count_x & 0xFF);
    // write(_count_y & 0xFF);
    // if (has_wheel) write(_count_z & 0x0F);
    write_multi(len, data);
    reset_counter();
  }
  void send_status() {
    uint8_t data[3];
    boolean mode = (_mode == REMOTE_MODE);
    data[0] = (_button_right & 1) & ((_button_middle & 1) << 1) & ((_button_left & 1) << 2) & ((0) << 3) & ((scale & 1) << 4) &
              ((data_report_enabled & 1) << 5) & ((mode & 1) << 6) & ((0) << 7);
    data[1] = resolution;
    data[2] = sample_rate;
    for (size_t i = 0; i < 3; i++) {
      write(data[i]);
    }
  }
  int mouse_reply(uint8_t cmd) {
    uint8_t val;
    if (_mode == WRAP_MODE) {
      switch (cmd) {
        case 0xEE:  // set wrap mode
          Serial.println("(WRAP_MODE) Set wrap mode command received");
          ack();
          reset_counter();
          break;
        case 0xEC:  // reset wrap mode
          Serial.println("(WRAP_MODE) Reset wrap mode command received");
          ack();
          reset_counter();
          set_mode(_last_mode);
          break;
        default:
          write(cmd);
      }
      return 0;
    }

    switch (cmd) {
      case 0xFF:  // reset
        Serial.println("mouse_reply(): Reset command received");
        ack();
        // the while loop lets us wait for the host to be ready
        while (write(0xAA) != 0) delay(1);
        while (write(0x00) != 0) delay(1);
        has_wheel = false;
        sample_rate = 100;
        resolution = RES_4;
        scale = ONE_ONE;
        data_report_enabled = false;
        set_mode(STREAM_MODE);
        reset_counter();
        break;
      case 0xFE:  // resend
        Serial.println("mouse_reply(): Resend command received");
        ack();
        break;
      case 0xF6:  // set defaults
        Serial.println("mouse_reply(): Set defaults command received");
        // enter stream mode
        ack();
        sample_rate = 100;
        resolution = RES_4;
        scale = ONE_ONE;
        data_report_enabled = false;
        set_mode(STREAM_MODE);
        reset_counter();
        break;
      case 0xF5:  // disable data reporting
        Serial.println("mouse_reply(): Disable data reporting command received");
        ack();
        data_report_enabled = false;
        reset_counter();
        break;
      case 0xF4:  // enable data reporting
        Serial.println("mouse_reply(): Enable data reporting command received");
        ack();
        data_report_enabled = true;
        reset_counter();
        break;
      case 0xF3:  // set sample rate
        ack();
        if (read(&val) == 0) {
          sample_rate = (val == 0) ? 100 : val;
          _min_report_interval_us = 1000000 / sample_rate;
          _last_sample_rate[0] = _last_sample_rate[1];
          _last_sample_rate[1] = _last_sample_rate[2];
          _last_sample_rate[2] = val;
          Serial.print("Set sample rate command received: ");
          Serial.println(val);
          ack();
          reset_counter();
        }
        break;
      case 0xF2:  // get device id
        Serial.println("mouse_reply(): Get device id command received");
        ack();
        if (_last_sample_rate[0] == 200 && _last_sample_rate[1] == 100 && _last_sample_rate[2] == 80) {
          write(0x03);  // Intellimouse with wheel
          Serial.println("mouse_reply(): Act as Intellimouse with wheel.");
          has_wheel = true;
        } else {
          write(0x00);  // Standard PS/2 mouse
          Serial.println("mouse_reply(): Act as Standard PS/2 mouse.");
          has_wheel = false;
        }
        reset_counter();
        break;
      case 0xF0:  // set remote mode
        Serial.println("mouse_reply(): Set remote mode command received");
        ack();
        reset_counter();
        set_mode(REMOTE_MODE);
        break;
      case 0xEE:  // set wrap mode
        Serial.println("mouse_reply(): Set wrap mode command received");
        ack();
        reset_counter();
        set_mode(WRAP_MODE);
        break;
      case 0xEC:  // reset wrap mode
        Serial.println("mouse_reply(): Reset wrap mode command received");
        ack();
        reset_counter();
        break;
      case 0xEB:  // read data
        ack();
        report();
        reset_counter();
        break;
      case 0xEA:  // set stream mode
        Serial.println("mouse_reply(): Set stream mode command received");
        ack();
        reset_counter();
        break;
      case 0xE9:  // status request
        Serial.println("mouse_reply(): Status request command received");
        ack();
        send_status();
        break;
      case 0xE8:  // set resolution
        ack();
        if (read(&val) == 0) {
          resolution = val;
          Serial.print("mouse_reply(): Set resolution command received: ");
          Serial.println(val, HEX);
          ack();
          reset_counter();
        }
        break;
      case 0xE7:  // set scaling 2:1
        Serial.println("mouse_reply(): Set scaling 2:1 command received");
        ack();
        scale = TWO_ONE;
        break;
      case 0xE6:  // set scaling 1:1
        Serial.println("mouse_reply(): Set scaling 1:1 command received");
        ack();
        scale = ONE_ONE;
        break;
      default:
        Serial.print("mouse_reply(): Unknown command received: ");
        Serial.println(cmd, HEX);
    }
    return 0;
  }
  // void report_stream() {
  //   if (_mode == STREAM_MODE && data_report_enabled) {
  //     uint64_t now = micros();
  //     if (now - _last_report_time > _min_report_interval_us) {
  //       report();
  //       _last_report_time = now;
  //     }
  //   }
  // }
  int mouse_handle() {
    uint8_t c;  // char stores data recieved from computer for KBD
    if (available()) {
      if (!read(&c)) return mouse_reply(c);
      // if (_count_or_button_changed_since_last_report && _mode == STREAM_MODE
      // &&
      //     data_report_enabled) {
      //   uint64_t now = micros();
      //   if (now - _last_report_time > _min_report_interval_us) {
      //     report();
      //     // Serial.println("Report sent");
      //     _last_report_time = now;
      //   }
      // }
    }
    return 0;
  }
};
