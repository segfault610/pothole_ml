/*
 * board_pins.h — single source of truth for the wiring.
 *
 * Board: classic ESP32 NodeMCU / DevKit V1 (ESP32-WROOM-32).
 * See docs/01-hardware.md for the wiring diagram.
 *
 *   ADXL345 (GY-291)          ESP32
 *   ----------------          -----------
 *   VCC                       3V3
 *   GND                       GND
 *   SDA                       GPIO21
 *   SCL                       GPIO22
 *   CS                        3V3   (I2C mode)
 *   SDO                       GND   (address 0x53)
 *
 *   NEO-7M (GY-NEO7M)         ESP32
 *   -----------------         -----------
 *   VCC                       3V3 (or VIN/5V if the module has a regulator)
 *   GND                       GND
 *   TX                        GPIO16 (UART2 RX)
 *   RX                        GPIO17 (UART2 TX)
 *
 *   Status LED                GPIO2 (on-board blue LED)
 *   Mark button               GPIO0 (on-board BOOT button, collector only)
 */

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define BOARD_I2C_PORT      0
#define BOARD_I2C_SDA       21
#define BOARD_I2C_SCL       22

#define BOARD_GPS_UART      2
#define BOARD_GPS_RX        16      /* ESP32 RX  <- GPS TX */
#define BOARD_GPS_TX        17      /* ESP32 TX  -> GPS RX */
#define BOARD_GPS_BAUD      9600    /* NEO-7M factory default */

#define BOARD_LED           2
#define BOARD_MARK_BUTTON   0       /* BOOT button, read after boot only */

#endif
