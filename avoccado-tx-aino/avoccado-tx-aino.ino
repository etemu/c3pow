#if 1 // BOF preprocessor bug workaround
#define HW 3 // define hardware revision, 1=switchcube, 2=C3POWproto2, 3=Avoccado-avr-fred... for different pin assignments and peripheral hardware
#include "Arduino.h"
inline unsigned long get_last_time();

void active();
void ledst(int sta);
void colorWipe(uint32_t c, uint8_t wait);
void set_last_read_angle_data(unsigned long time, float x, float y, float z, float x_gyro, float y_gyro, float z_gyro);
void calibrate_sensors();
void getangle();
void send_K(unsigned int to);
boolean send_L(uint16_t to, byte* ledmap);
void processPacket();
#line 2
__asm volatile ("nop"); // BOF preprocessor bug workaround
#endif

//#define USE_LEDS // LED stripe used
//#define USE_TOUCH // capacitive touch sensing

// I2Cdev and MPU6050 must be installed as libraries, or else the .cpp/.h files
// for both classes must be in the include path of the project
#include "I2Cdev.h"
#include "MPU6050.h"
#include <avr/sleep.h>
#include <avr/power.h>

#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE // Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation is used in I2Cdev.h
#include "Wire.h"
#endif
#include <RF24.h>
#include <SPI.h>
#include <stdarg.h>
#include <EEPROM.h>
#ifdef USE_TOUCH
#include <CapacitiveSensor.h>
#endif
#define DEBUG 1 // debug mode with verbose output over serial at 115200 bps
#define USE_EEPROM // read nodeID and network settings from EEPROM at bootup, overwrites nodeID and MAC.
#define LEDPIN 4
#define L 4
#define KEEPALIVE 1 // keep connections alive with regular polling to node 0
//#define USE_LEDS // LED stripe used
//#define USE_TOUCH // capacitive touch sensing
#define TIMEOUT_HIBERNATE 512
#define TIMEOUT_MEMS 5000
#define TIMEOUT_RADIO 8000
#define PIN_VIBR 3 // pin for vibration motor, 9 for switchcube1, 5 for prototype2

#ifdef USE_LEDS
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel leds = Adafruit_NeoPixel(8, LEDPIN, NEO_GRB + NEO_KHZ800); // number of pixels in strip, pin number, pixel type flags
#endif
RF24 radio(9, 10); // CE, CS. CE at pin A0, CSN at pin 10

#ifdef USE_TOUCH
CapacitiveSensor cs = CapacitiveSensor(8, 7);       // capacitive sensing at pins 7 and 8
#endif

const unsigned long interval_filter = 1000;
const unsigned long interval = 50; // main loop interval in [ms]
unsigned long last_time_filtered;
byte sweep = 0;
byte nodeID = 00; // Unique Node Identifier (2...254) - also the last byte of the IPv4 adress, not used if USE_EEPROM is set
uint16_t this_node = 00; // always begin with 0 for octal declaration, not used if USE_EEPROM is set
// Debug variables, TODO: don't initialize if DEBUG is set to 0
unsigned long iterations = 0;
unsigned long errors = 0;
unsigned int loss = 0;
unsigned long p_sent = 0;
unsigned long p_recv = 0;

const short max_active_nodes = 32; // the size of the array with active nodes
uint16_t active_nodes[max_active_nodes];
short num_active_nodes = 0;
short next_ping_node_index = 0;
unsigned long last_time_sent;
unsigned long updates = 0; // has to be changed to unsigned long if the interval is too long
boolean send_T(uint16_t to);
void send_L1(int to, int _b);
void handle_K(char header);
void handle_L(char header);
void handle_T(char header);
void handle_B(char header);
void ledupdate(byte* ledmap);
void p(char *fmt, ... );
void ledst(int sta = 127);
void mpucheck();
bool strobe = 1;

// class default I2C address is 0x68
// specific I2C addresses may be passed as a parameter here
// AD0 low = 0x68 (default)
// AD0 high = 0x69
MPU6050 mpu;
//MPU6050 mpu(0x69); // alternative I2C address, if AD0 is set high

int16_t ax, ay, az; // accel values
int16_t gx, gy, gz; // gyro values

#define OUTPUT_READABLE_mpu // tab-separated list of the accel X/Y/Z and then gyro X/Y/Z values in decimal.
// #define OUTPUT_BINARY_mpu //send all 6 axes of data as 16-bit

unsigned long last_read_time;
float         last_x_angle;  // These are the filtered angles
float         last_y_angle;
float         last_z_angle;
float         last_gyro_x_angle;  // Store the gyro angles to compare drift
float         last_gyro_y_angle;
float         last_gyro_z_angle;

inline unsigned long get_last_time() {
  return last_read_time;
}
inline float get_last_x_angle() {
  return last_x_angle;
}
inline float get_last_y_angle() {
  return last_y_angle;
}
inline float get_last_z_angle() {
  return last_z_angle;
}
inline float get_last_gyro_x_angle() {
  return last_gyro_x_angle;
}
inline float get_last_gyro_y_angle() {
  return last_gyro_y_angle;
}
inline float get_last_gyro_z_angle() {
  return last_gyro_z_angle;
}

//  Use the following global variables and access functions
//  to calibrate the acceleration sensor
float    base_x_accel;
float    base_y_accel;
float    base_z_accel;

float    base_x_gyro;
float    base_y_gyro;
float    base_z_gyro;

// Sleep declarations
typedef enum { wdt_16ms = 0, wdt_32ms, wdt_64ms, wdt_128ms, wdt_250ms, wdt_500ms, wdt_1s, wdt_2s, wdt_4s, wdt_8s } wdt_prescalar_e;
void setup_watchdog(uint8_t prescalar);
void do_sleep(void);
const short sleep_cycles_per_transmission = 10;
volatile short sleep_cycles_remaining = sleep_cycles_per_transmission;
void powerMode(byte _mode);

void vibr(byte _state);
void pulse(unsigned int _length = 50);
bool checkTouch(unsigned int _sensitivity = 96);
byte mode = 0;
unsigned long lastActive = 0;

// The various roles supported by this sketch
typedef enum { sender = 1, receiver } role_e;

// The debug-friendly names of those roles
const char* role_friendly_name[] = { "invalid", "sender", "receiver"};

// The role of the current running sketch
role_e role;

const int role_pin = 1;

// Radio pipe addresses for the 2 nodes to communicate.
const uint64_t pipes[2] = { 0xA40CCAD000LL, 0xA40CCAD001LL };


void setup() {
#if HW == 2 // only for C3POW prototype 2:
  pinMode(A1, OUTPUT); //GND for NRF24
  digitalWrite(A1, LOW);
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH); // Vcc for MPU6050
  pinMode(8, OUTPUT); // Vcc for the NRF24 module
  digitalWrite(8, HIGH); // Vcc for the NRF24 module activated. Shutdown and hard power cut-off with LOW. Need to be initialized again afterwards.
#endif
  pinMode(PIN_VIBR, OUTPUT); // vibration motor
  pinMode(L, OUTPUT); // status LED
  digitalWrite(L, HIGH);
  vibr(0); // pull low and disable motor for now
  vibr(1); // vibrate during bootup
#ifdef USE_LEDS
  leds.begin(); // the 8 LEDs
  leds.show(); // Initialize all pixels to 'off'
  ledst(5); // set initial status to 5, ledst() sets the first
#endif
#ifdef USE_TOUCH
  cs.set_CS_AutocaL_Millis(10000);
#endif
  Serial.begin(115200); // initialize serial communication
  delay(64);
  vibr(0); // stop vibrating
  // initialize devices
  Serial.println(F("avoccado aino 0.201412182308"));
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
  Serial.println(F("I2C setup"));
  Wire.begin(); // join I2C bus (I2Cdev library doesn't do this automatically)
#elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
  Serial.println(F("FastwireSetup"));
  Fastwire::setup(400, true);
#endif
  Serial.println(F("mpu.initialize"));
  mpu.initialize();
  ledst(4);
#ifdef USE_EEPROM
  nodeID = EEPROM.read(0);
  Serial.print(F("EEPROM, "));
#endif
  Serial.println(nodeID);
  Serial.print(F("Network ID (oct): "));
#ifdef USE_EEPROM
  this_node = ((int) EEPROM.read(15) * 256) + ((int) (EEPROM.read(16)));
  Serial.print(F("EEPROM, "));
#endif
  Serial.print(this_node, OCT);
  Serial.print(F(", (dec): "));
  Serial.print(this_node, DEC);
  Serial.println();
  SPI.begin(); // SPI for the NRF24
  radio.begin(); // init of the NRF24
  // The amplifier gain can be set to RF24_PA_MIN=-18dBm, RF24_PA_LOW=-12dBm, RF24_PA_MED=-6dBM, and RF24_PA_MAX=0dBm.
  radio.setPALevel(RF24_PA_MAX); // transmitter gain value (see above)
  radio.enableDynamicPayloads();   // enable dynamic payloads
  radio.setRetries(4, 8);  // optionally, increase the delay between retries & # of retries
  radio.setChannel(23);
  Serial.print(F("\t datarate: "));
  Serial.println(radio.getDataRate());
  radio.setDataRate(RF24_250KBPS);
  radio.setCRCLength(RF24_CRC_16);
  Serial.print(F("PA-level: "));
  Serial.print(radio.getPALevel());
  Serial.print(F("\t CRC: "));
  Serial.print(radio.getCRCLength());
  Serial.print(F("\t datarate 250K: "));
  Serial.println(radio.getDataRate());
  Serial.println();

  if ( role_pin == 1 )
    role = sender;
  else
    role = receiver;

  p("\t role: %s\n\r", role_friendly_name[role]);
  if ( role == sender )
  {
    radio.openWritingPipe(pipes[0]);
    radio.openReadingPipe(1, pipes[1]);
  }
  else
  {
    radio.openWritingPipe(pipes[1]);
    radio.openReadingPipe(1, pipes[0]);
  }

  radio.startListening();
  radio.printDetails();
  Serial.println();
  Serial.print(F("UID: "));
  Serial.print(F("(N/A)\n"));
  p("%010ld: Starting up\n", millis());
  digitalWrite(L, LOW);
#ifdef USE_LEDS
  colorWipe(leds.Color(50, 0, 0), 50); // Red
  colorWipe(leds.Color(0, 50, 0), 50); // Green
  colorWipe(leds.Color(0, 0, 50), 50); // Blue
  colorWipe(leds.Color(0, 0, 0), 0); // clear
  ledst(5);
#endif

  // verify connection
  Serial.println(F("init I2C..."));
  Serial.println(mpu.testConnection() ? "MPU6050 connected." : "MPU6050 error.");
  calibrate_sensors();
  set_last_read_angle_data(millis(), 0, 0, 0, 0, 0, 0);
#ifdef USE_LEDS
  ledst(1);
#endif
  setup_watchdog(wdt_1s); // Set the watchdog timer interval
  radio.powerUp();
}

void powerMode(byte _mode = mode) {
  switch (_mode) // check which power mode should be set active
  {
    case 0:
      break; // no change in power mode
    case 1:
      mpu.setSleepEnabled(0); // activate the MEMS device
      Serial.println(F("powerMode(1), MEMS up"));
      mode = 0;
      break;
    case 2:
      radio.powerUp(); // activate the radio
      Serial.println(F("powerMode(2), radio up"));
      mode = 0;
      break;
    case 5:
      Serial.println(F("powerMode(5), hibernate"));
      radio.powerDown();
      mpu.setSleepEnabled(1);
      mpu.setStandbyXGyroEnabled(1);
      mpu.setStandbyYGyroEnabled(1);
      mpu.setStandbyZGyroEnabled(1);
      Serial.print(F("Sleeping."));
      vibr(1);
      delay(64);
      vibr(0);
      delay(196);
      vibr(1);
      delay(64);
      vibr(0);
      setup_watchdog(wdt_250ms); // set activity check interval
      while ( ((ay < (-10000)) || ay > (10000)) ) { // if the Avoccado device is resting in this position
        do_sleep(); // keep on sleeping
        Serial.print(F("."));
        mpu.setSleepEnabled(0); // power up MEMS, wake up every watchdog timer interval
        //        mpucheck();
        mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz); // get the new acceleration values TODO: implement for ADXL345
        //mpu.setSleepEnabled(1);
      }
      for (int _i = 0; _i < 2; _i++) { // vibrate briefly when waking up from hibernation mode
        vibr(1);
        delay(128);
        vibr(0);
        delay(56);
      }
      Serial.println();
      powerMode(1); // power up MEMS
      mpu.setStandbyXGyroEnabled(0);
      mpu.setStandbyYGyroEnabled(0);
      mpu.setStandbyZGyroEnabled(0);
      powerMode(2); // power up radio
      active();
      break;
    case 6: // shutdown until next hard reset
      Serial.println(F("powerMode(6), shutdown"));
      radio.powerDown();
      mpu.setSleepEnabled(1);
      vibr(4); // descending heartbeat pattern
      delay(512);
      vibr(4);
      delay(700);
      vibr(4);
      delay(900);
      vibr(4);
      delay(1100);
      vibr(5);
      delay(1280);
      vibr(5);
      delay(1650);
      pulse(32);
      setup_watchdog(wdt_8s); // longest watchdog interval
      while (1) do_sleep(); // sleep forever
      break;
    default: // no change in power mode
      return;
  };
}

bool checkTouch(unsigned int _sensitivity)
{
#ifdef USE_TOUCH
  long csVal = cs.capacitiveSensor(24);
  if (csVal > _sensitivity) {
    //send_L1(01,_val);
    //Serial.println(csVal);
    return 1;
  }
#endif
  return 0;
}

void active() {
  lastActive = millis();
}

void loop() {

  /////////////////////////
  //powerMode(6); // WIP - shutdown forever, disable hardware until next hard reset
  /////////////////////////

  if (millis() - lastActive > TIMEOUT_HIBERNATE && ((ay < (-10000)) || ay > (10000))) powerMode(5);
  //  if (millis()-lastActive> TIMEOUT_MEMS) mpu.setSleepEnabled(1);
  //  if (millis()-lastActive> TIMEOUT_RADIO) radio.powerDown();
  updates++;

  /*if ( radio.available() ) // while there are packets in the FIFO buffer
  {
    processPacket();
  }
  */
  unsigned long now = millis();
  unsigned long nowM = micros();
  if ( now - last_time_filtered >= interval_filter ) { 
    getangle();
    last_time_filtered = now;
  }

  if ( now - last_time_sent >= interval )
  {
    //ledst(2);
    /* if (DEBUG) {
      p("%010ld: %ld Hz\n", millis(), updates * 1000 / interval);
    } */
    updates = 0;
    last_time_sent = now;
    digitalWrite(L, HIGH);
    radio.powerUp();
    send_K(1); // send out a new packet to a remote node.
    radio.powerDown();
    digitalWrite(L, LOW);
  }

}


// 0=16ms, 1=32ms,2=64ms,3=125ms,4=250ms,5=500ms
// 6=1 sec,7=2 sec, 8=4 sec, 9= 8sec
void setup_watchdog(uint8_t prescalar)
{
  prescalar = min(9, prescalar);
  uint8_t wdtcsr = prescalar & 7;
  if ( prescalar & 8 )
    wdtcsr |= _BV(WDP3);
  MCUSR &= ~_BV(WDRF);
  WDTCSR = _BV(WDCE) | _BV(WDE);
  WDTCSR = _BV(WDCE) | wdtcsr | _BV(WDIE);
}

ISR(WDT_vect)
{
  --sleep_cycles_remaining;
}

void do_sleep(void)
{
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); // sleep mode is set here
  sleep_enable();
  sleep_mode();                        // System sleeps here
  sleep_disable();                     // System continues execution here when watchdog timed out
}


// Arduino version of the printf()-funcition in C
void p(char *fmt, ... ) {
  char tmp[128]; // resulting string limited to 128 chars
  va_list args;
  va_start (args, fmt );
  vsnprintf(tmp, 128, fmt, args);
  va_end (args);
  Serial.print(tmp);
}
