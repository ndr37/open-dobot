
#define F_CPU 16000000UL
#define BAUD 115200
#include <util/setbaud.h>

#include "dobot.h"

#define SPI_PORT PORTB
#define SPI_DDR  DDRB
#define SPI_MOSI PORTB2
#define SPI_MISO PORTB3
#define SPI_SCK PORTB1
#define SPI_SS PORTB0

#define FPGA_ENABLE_PORT PORTG
#define FPGA_ENABLE_DDR  DDRG
#define FPGA_ENABLE_PIN PORTG1
#define POWERON_PORT PINL
#define POWERON_PIN PORTL5
#define FPGA_COMMAND_PORT PORTL
#define FPGA_COMMAND_DDR DDRL
#define FPGA_COMMAND_PIN PORTL7
// INIT pin is normally low.
#define FPGA_COMMAND_ACCELS_INIT_PIN PORTL0
// SS pins are normally high.
#define FPGA_COMMAND_ACCEL_REAR_SS_PIN PORTL2
#define FPGA_COMMAND_ACCEL_FRONT_SS_PIN PORTL4

#define CMD_QUEUE_SIZE     200

CommandQueue cmdQueue(CMD_QUEUE_SIZE);

#define CMD_READY 0
#define CMD_STEPS 1
#define CMD_EXEC_QUEUE 2
#define CMD_GET_ACCELS 3
#define CMD_SWITCH_TO_ACCEL_REPORT_MODE 4
// DO NOT FORGET TO UPDATE cmdArray SIZE!
funcPtrs cmdArray[5];
// Last index in the commands array.
int cmdLastIndex;

byte cmd[20];
byte crc[2];

// ulong lastTimeExecuted = 0;
byte defer = 1;

unsigned int accelRear;
unsigned int accelFront;
byte accelReportMode = 0;

void setup() {
  cmdArray[CMD_READY] = cmdReady;
  cmdArray[CMD_STEPS] = cmdSteps;
  cmdArray[CMD_EXEC_QUEUE] = cmdExecQueue;
  cmdArray[CMD_GET_ACCELS] = cmdGetAccels;
  cmdArray[CMD_SWITCH_TO_ACCEL_REPORT_MODE] = cmdSwitchToAccelReportMode;
  cmdLastIndex = sizeof(cmdArray) / sizeof(cmdArray[0]) - 1;

  serialInit();

  //---=== Power-on sequence ===---
  // 1. FPGA_ENABLE_PIN = LOW, FPGA_COMMAND_PIN = LOW
  // 2. Check if FPGA_POWERON_PIN == HIGH. If not, then switch to accelerometer reporting mode.
  // 3. Arduino initializes - delay 900ms. Don't need to do anything.
  // 4. FPGA_COMMAND_ACCELS_INIT_PIN = HIGH
  // 5. Delay 35us
  // 6. Set SPI as Master
  // 7. Delay 35us
  // 8. Read accelerometers
  // 9. FPGA_COMMAND_ACCELS_INIT_PIN = LOW
  // 10. Delay 200ms
  // 11. Set SPI as Slave
  // 12. Enable FPGA: FPGA_ENABLE_PIN = HIGH

  //---=== Accelerometer reading sequence ===---
  // 1. Delay 800ns
  // 2. Loop 17 times:
  // 3.1. Delay 770us for accelerometer to prepare data
  // 3.2. FPGA_COMMAND_ACCEL_REAR_SS_PIN = LOW
  // 3.3. Send "RDAX / 00010000 / Read X-channel acceleration through SPI" command = 0x10
  // 3.4. Read two bytes
  // 3.5. FPGA_COMMAND_ACCEL_REAR_SS_PIN = LOW
  // 4. Average the result
  // 5. Repeat 1-3 for another accelerometer - FPGA_COMMAND_ACCEL_FRONT_SS_PIN
  
  // Power-on step 1
  FPGA_ENABLE_DDR = (1<<FPGA_ENABLE_PIN);
  FPGA_COMMAND_DDR = (1<<FPGA_COMMAND_PIN) | (1<<FPGA_COMMAND_ACCELS_INIT_PIN)
                  | (1<<FPGA_COMMAND_ACCEL_REAR_SS_PIN) | (1<<FPGA_COMMAND_ACCEL_FRONT_SS_PIN);
  FPGA_COMMAND_PORT = (1<<FPGA_COMMAND_ACCEL_REAR_SS_PIN) | (1<<FPGA_COMMAND_ACCEL_FRONT_SS_PIN);

  // Power-on step 2
  if (! (POWERON_PORT & (1<<POWERON_PIN))) {
    cmdSwitchToAccelReportMode();
  }

  // Power-on step 3
  // Do nothing.

  // Power-on step 4
  FPGA_COMMAND_PORT |= (1<<FPGA_COMMAND_ACCELS_INIT_PIN);

  // Power-on step 5
  _delay_us(35);

  // Power-on step 6
  // Set MOSI, SCK and SS as output, all others input. SS must be output for SPI remain Master. See docs.
  SPI_DDR = (1<<SPI_MOSI)|(1<<SPI_SCK)|(1<<SPI_SS);
  // Enable SPI, Master, set clock rate fck/16
  SPCR = (1<<SPE)|(1<<MSTR)|(1<<SPR0);

  // Power-on step 7
  _delay_us(35);

  // Power-on step 8
  accelRear = accelRead(FPGA_COMMAND_ACCEL_REAR_SS_PIN);
  accelFront = accelRead(FPGA_COMMAND_ACCEL_FRONT_SS_PIN);

  // Power-on step 9
  FPGA_COMMAND_PORT &= ~(1<<FPGA_COMMAND_ACCELS_INIT_PIN);

  // Power-on step 10
  _delay_ms(200);

  // Power-on step 11
  SPI_DDR = (1<<SPI_MISO);
  // CPOL=0, CPHA=1 - Trailing (Falling) Edge
  SPCR = _BV(SPE) | _BV(CPHA);

  // Power-on step 12
  FPGA_ENABLE_PORT |= (1<<FPGA_ENABLE_PIN);
}

void processCommand() {
  // If something is waiting in the serial port to be read...
  if (UCSR0A&(1<<RXC0)) {
    // Get incoming byte.
    cmd[0] = UDR0;
    if ((accelReportMode && cmd[0] != CMD_GET_ACCELS) || cmd[0] > cmdLastIndex) {
      return;
    }
    cmdArray[cmd[0]]();
  }
}

// CMD: Returns magic number to indicate that the controller is alive.
void cmdReady() {
  serialWrite(1);
  crcCcitt(cmd, 1);
  // Return magic number.
  cmd[0] = 0x40;
  write1(cmd);
}

// CMD: Adds a command to the queue.
void cmdSteps() {
  serialWrite(1);
  if (!read13(&cmd[1])) {
    return;
  }
  if (checkCrc(cmd, 14)) {
    resetCrc();
    if (cmdQueue.appendHead((ulong*) &cmd[1], (ulong*) &cmd[5], (ulong*) &cmd[9], &cmd[13])) {
      cmd[0] = 1;
      write1(cmd);
    } else {
      cmd[0] = 0;
      write1(cmd);
    }
  }
}

// CMD: Returns data read from accelerometers.
void cmdGetAccels() {
  serialWrite(1);
  crcCcitt(cmd, 1);
  write22(cmd, &accelRear, &accelFront);
}

// CMD: Executes deferred commands in the queue.
void cmdExecQueue() {
  defer = 0;
}

// CMD: Switches controller to accelerometers report mode.
void cmdSwitchToAccelReportMode() {
  /* Apparently there is a problem with the way dobot was designed.
   * It is not possible to switch back from SPI Slave to Master.
   * So this code is left in case a proper solution comes up.
   */
  // serialWrite(1);
  // if (checkCrc(cmd, 1)) {
  //   resetCrc();
  //   cmd[0] = 1;
  //   write1(cmd);

    accelReportMode = 1;

    // Disable FPGA.
    FPGA_ENABLE_PORT &= ~(1<<FPGA_ENABLE_PIN);

    // Enable accelerometers reading.
    FPGA_COMMAND_PORT |= (1<<FPGA_COMMAND_ACCELS_INIT_PIN);

    _delay_us(35);

    // Set SPI as Master.
    SPI_DDR = (1<<SPI_MOSI)|(1<<SPI_SCK)|(1<<SPI_SS);
    SPCR = (1<<SPE)|(1<<MSTR)|(1<<SPR0);

    _delay_us(35);

    // Never return from this function. Only update accelerometers
    // and return their values to the driver.
    // Process next waiting commands in between or else they will time out.
    while (1) {
      accelRear = accelRead(FPGA_COMMAND_ACCEL_REAR_SS_PIN);
      processCommand();
      accelFront = accelRead(FPGA_COMMAND_ACCEL_FRONT_SS_PIN);
      processCommand();
    }
  // }
}

void serialInit(void) {
  UBRR0H = UBRRH_VALUE;
  UBRR0L = UBRRL_VALUE;

#if USE_2X
  UCSR0A |= _BV(U2X0);
#else
  UCSR0A &= ~(_BV(U2X0));
#endif

  UCSR0C = _BV(UCSZ01) | _BV(UCSZ00); /* 8-bit data */
  UCSR0B = _BV(RXEN0) | _BV(TXEN0);   /* Enable RX and TX */

//// DEBUG ////
//   UBRR1H = UBRRH_VALUE;
//   UBRR1L = UBRRL_VALUE;

// #if USE_2X
//   UCSR1A |= _BV(U2X1);
// #else
//   UCSR1A &= ~(_BV(U2X1));
// #endif

//   UCSR1C = _BV(UCSZ11) | _BV(UCSZ10); /* 8-bit data */
//   UCSR1B = _BV(RXEN1) | _BV(TXEN1);   /* Enable RX and TX */
}

void serialWrite(byte c) {
  loop_until_bit_is_set(UCSR0A, UDRE0);
  UDR0 = c;
}

void serialWrite(byte data[], byte num) {
  for (byte i = 0; i < num; i++) {
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = data[i];
  }
}

// Returns number of bytes read or 0 if timeout occurred.
// Timeout: 1000 increments ~ 600us, so allow about 9ms interbyte and 18ms overall.
byte serialReadNum(byte data[], byte num) {
  unsigned int interByteTimeout = 0;
  unsigned int transactionTimeout = 0;
  byte cnt = 0;
  byte tmp;
  while (cnt < num) {
    // Wait until data exists.
    while (!(UCSR0A & (1<<RXC0))) {
      interByteTimeout++;
      transactionTimeout++;
      if (interByteTimeout > 15000 || transactionTimeout > 30000) {
        return 0;
      }
    }
    interByteTimeout = 0;
    tmp = UDR0;
    data[cnt++] = tmp;
  }
  return cnt;
}

uint accelRead(unsigned char pin) {
  byte junk;
  uint result = 0, data;
  _delay_us(1);
  // // Clear SPIF bit.
  junk = SPSR;
  junk = SPDR;
  for (byte i = 0; i < 17; i++) {
    _delay_us(770);
    FPGA_COMMAND_PORT &= ~(1<<pin);
    SPDR = 0x10;
    loop_until_bit_is_set(SPSR, SPIF);
    SPDR = 0x00;
    loop_until_bit_is_set(SPSR, SPIF);
    data = SPDR << 8;
    SPDR = 0x00;
    loop_until_bit_is_set(SPSR, SPIF);
    data |= SPDR;
    // There are only 11 bits accelerometer returns,
    // so take advantage by shifting to prevent sum overflow.
    result += (data >> 5);
    FPGA_COMMAND_PORT |= (1<<pin);
  }
  data = result / 17;
  // Average
  return result / 17;
}

byte read2(byte data[]) {
  return serialReadNum(data, 2);
}

byte read13(byte data[]) {
  return serialReadNum(data, 13);
}

void write1(byte data[]) {
  crcCcitt(data, 1, 1);
  data[1] = crc[0];
  data[2] = crc[1];
  serialWrite(data, 3);
}

byte write22(byte data[], uint* val1, uint* val2) {
  data[0] = (byte) (*val1 >> 8) & 0xff;
  data[1] = (byte) *val1 & 0xff;
  data[2] = (byte) (*val2 >> 8) & 0xff;
  data[3] = (byte) *val2 & 0xff;
  write4(data);
}

byte write4(byte data[]) {
  crcCcitt(data, 4, 1);
  data[4] = crc[0];
  data[5] = crc[1];
  serialWrite(data, 6);
}

int dataToInt(byte data[]) {
  int result = ((((uint16_t) data[0]) << 8) & 0xFF00) | (((uint16_t) data[1]) & 0x00FF);
  return result;
}

uint16_t dataToUint(byte data[]) {
  return (uint16_t) dataToInt(data);
}

byte checkCrc(byte data[], int len) {
  if (!read2(&cmd[len])) {
    return 0;
  }
  crcCcitt(cmd, len);
  if (data[len] == crc[0] && data[len + 1] == crc[1]) {
    return 1;
  }
  return 0;
}

byte confirmCrc(byte data[], int len) {
  if (checkCrc(data, len)) {
    serialWrite(1);
    return 1;
  }
  return 0;
}

void resetCrc() {
  crc[0] = 0xff;
  crc[1] = 0xff;
}

void crcCcitt(byte data[], int len) {
  crcCcitt(data, len, 0);
}

void crcCcitt(byte data[], int len, byte keepSeed) {
  uint16_t localCrc;
  if (keepSeed) {
    localCrc = ((((uint16_t) crc[0]) << 8) & 0xFF00) | (((uint16_t) crc[1]) & 0x00FF);
  } else {
    localCrc = 0xffff;
  }
  for (int i = 0; i < len; i++) {
    localCrc = localCrc ^ ( (((uint16_t) data[i]) << 8) & 0xff00);
    for (int n = 0; n < 8; n++) {
      if ((localCrc & 0x8000) == 0x8000) {
        localCrc = ((localCrc << 1) ^ 0x1021);
      } else {
        localCrc = localCrc << 1;
      }
    }
  }
  crc[0] = (byte) (localCrc >> 8);
  crc[1] = (byte) localCrc;
}

inline void writeSpiByte(byte data) {
  byte junk;

  SPDR = data;
  loop_until_bit_is_set(SPSR, SPIF);
  junk = SPDR;
}

void writeSpi(Command* cmd) {
  byte* data = (byte*)cmd;

  FPGA_COMMAND_PORT |= (1<<FPGA_COMMAND_PIN);
  loop_until_bit_is_set(SPSR, SPIF);

  writeSpiByte(sequenceRest[0]);
  for (byte i = 0; i < 13; i++) {
    writeSpiByte(data[i]);
  }
  writeSpiByte(sequenceRest[14]);
  writeSpiByte(sequenceRest[15]);
  writeSpiByte(sequenceRest[16]);
  writeSpiByte(sequenceRest[17]);
  writeSpiByte(sequenceRest[18]);
  FPGA_COMMAND_PORT &= ~(1<<FPGA_COMMAND_PIN);
}

int main() {
  setup();

  byte i = 0;
  byte data;
  while (1) {
    if (SPDR == 0x5a) {
      SPDR = 0x00;
      if (cmdQueue.isEmpty()) {
        writeSpi((Command*) &sequenceRest[1]);
      } else {
        writeSpi(cmdQueue.popTail());
      }
      processCommand();
    }
  }
  return 0;
}
