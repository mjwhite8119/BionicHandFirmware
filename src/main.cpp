#include <Arduino.h>

#include <PololuRPiSlave.h>
// #include <Romi32U4.h>
#include <AStar32U4.h>
#include <Servo.h>

#include "shmem_buffer.h"
#include "low_voltage_helper.h"

static constexpr int kModeDigitalOut = 0;
static constexpr int kModeDigitalIn = 1;
static constexpr int kModeAnalogIn = 2;
static constexpr int kModePwm = 3;

/*

  // Built-ins
  bool buttonA;         // DIO 0 (input only)
  bool buttonB, green;  // DIO 1
  bool buttonC, red;    // DIO 2
  bool yellow;          // DIO 3 (output only)
  */

static constexpr int kMaxBuiltInDIO = 8;

/*
Digital pin 12, or PD6, controls the direction of motor 1 
(when LOW, motor current flows from A to B; when HIGH, current flows from B to A).

PE2 (which does not have an Arduino pin number) controls the direction of motor 2.

Digital pin 9, or PB5, controls the speed of motor 1 with PWM generated by the ATmega32U4’s Timer1.

Digital pin 10, or PB6, controls the speed of motor 2 with PWM.
*/

// Set up the servos
#define PWM_INDEX A5

// Servo pwms[5];
Servo indexPWM;

AStar32U4Motors motors;
// TODO need to read potentiometers
// AStar32U4Encoders encoders;
AStar32U4ButtonA buttonA;
AStar32U4ButtonB buttonB;
AStar32U4ButtonC buttonC;
AStar32U4Buzzer buzzer;

PololuRPiSlave<Data, 20> rPiLink;

uint8_t builtinDio0Config = kModeDigitalIn;
uint8_t builtinDio1Config = kModeDigitalOut;
uint8_t builtinDio2Config = kModeDigitalOut;
uint8_t builtinDio3Config = kModeDigitalOut;

uint8_t ioChannelModes[5] = {kModeDigitalOut, kModeDigitalOut, kModeDigitalOut, kModeDigitalOut, kModeDigitalOut};
uint8_t ioDioPins[5] = {11, 4, 20, 21, 22};
uint8_t ioAinPins[5] = {0, A6, A2, A3, A4};

LowVoltageHelper lvHelper;

bool isConfigured = false;

unsigned long lastHeartbeat = 0;
unsigned long lastSwitchTime = 0;

unsigned long lastTimeInterval = 0;
unsigned long TIME_INTERVAL = 1000;

// Setup Encoders

// 74HC4067 multiplexer setup (16 to 1)

//Mux control pins
int s0 = 0;
int s1 = 1;
int s2 = 15;
int s3 = 16;

//Mux in "SIG" pin
int SIG_pin = A8;

void setupAnalogMUX() {
  pinMode(s0, OUTPUT); 
  pinMode(s1, OUTPUT); 
  pinMode(s2, OUTPUT); 
  pinMode(s3, OUTPUT); 

  digitalWrite(s0, LOW);
  digitalWrite(s1, LOW);
  digitalWrite(s2, LOW);
  digitalWrite(s3, LOW);
}

void configureMotors() {
  indexPWM.attach(A5);
}

int readMux(int channel){
  int controlPin[] = {s0, s1, s2, s3};

  int muxChannel[16][4]={
    {0,0,0,0}, //channel 0
    {1,0,0,0}, //channel 1
    {0,1,0,0}, //channel 2
    {1,1,0,0}, //channel 3
    {0,0,1,0}, //channel 4
    {1,0,1,0}, //channel 5
    {0,1,1,0}, //channel 6
    {1,1,1,0}, //channel 7
    {0,0,0,1}, //channel 8
    {1,0,0,1}, //channel 9
    {0,1,0,1}, //channel 10
    {1,1,0,1}, //channel 11
    {0,0,1,1}, //channel 12
    {1,0,1,1}, //channel 13
    {0,1,1,1}, //channel 14
    {1,1,1,1}  //channel 15
  };

  //loop through the 4 sig
  for(int i = 0; i < 4; i ++){
    digitalWrite(controlPin[i], muxChannel[channel][i]);
  }

  //read the value at the SIG pin
  int val = analogRead(SIG_pin);

  //return the value
  return val;
}

void readEncoders()
{
  //Loop through and read all 16 values
  //Reports back Value at channel 6 is: 346
  for(int i = 0; i < 2; i ++){
    int percentage = map(readMux(i), 0, 1023, 0, 100);
    Serial.print("Potentiometer ");
    Serial.print(i);
    Serial.print(" is at ");
    Serial.print(percentage);
    Serial.println("%");
    // delay(1000);
  }
}

void configureBuiltins(uint8_t config) {
  // structure
  // [ConfigFlag] [Unused] [Unused] [Unused] [Unused] [DIO 2 Mode] [DIO 1 Mode] [Unused]
  //       7         6        5         4        3          2            1          0

  // We only care about bits 1 and 2
  builtinDio1Config = (config >> 1) & 0x1;
  builtinDio2Config = (config >> 2) & 0x1;

  // Turn off LEDs if in INPUT mode
  if (builtinDio1Config == kModeDigitalIn) {
    ledGreen(false);
  }
  if (builtinDio2Config == kModeDigitalIn) {
    ledRed(false);
  }

  // Wipe out the register
  rPiLink.buffer.builtinConfig = 0;
}

// void configureIO(uint16_t config) {
//   // 16 bit config register
//   //
//   // MSB
//   // 0 | NEW CONFIG FLAG |
//   //   |-----------------|
//   // 1 |  Pin 0 Mode     |
//   // 2 |  ArdPin 11      |
//   //   |-----------------|
//   // 3 |  Pin 1 Mode     |
//   // 4 |  ArdPin 4       |
//   //   |-----------------|
//   // 5 |  Pin 2 Mode     |
//   // 6 |  ArdPin 20      |
//   //   |-----------------|
//   // 7 |  Pin 3 Mode     |
//   // 8 |  ArdPin 21      |
//   //   |-----------------|
//   // 9 |  Pin 4 Mode     |
//   // 10|  ArdPin 22      |
//   //   |-----------------|
//   // 11|  RESERVED       |
//   // 12|                 |
//   // 13|                 |
//   // 14|                 |
//   // 15|                 |
//   for (uint8_t ioChannel = 0; ioChannel < 5; ioChannel++) {
//     uint8_t offset = 13 - (2 * ioChannel);
//     uint8_t mode = (config >> offset) & 0x3;

//     // Disconnect PWMs
//     if (pwms[ioChannel].attached()) {
//       pwms[ioChannel].detach();
//     }

//     ioChannelModes[ioChannel] = mode;

//     switch(mode) {
//       case kModeDigitalOut:
//         pinMode(ioDioPins[ioChannel], OUTPUT);
//         break;
//       case kModeDigitalIn:
//         pinMode(ioDioPins[ioChannel], INPUT_PULLUP);
//         break;
//       case kModePwm:
//         pwms[ioChannel].attach(ioDioPins[ioChannel]);
//         break;
//       case kModeAnalogIn:
//         if (ioChannel > 0) {
//           // Make sure we set the pin back correctly
//           digitalWrite(ioAinPins[ioChannel], LOW);
//           pinMode(ioAinPins[ioChannel], INPUT);
//         }
//         break;
//     }
//   }

//   // Also set the status flag
//   rPiLink.buffer.status = 1;
//   isConfigured = true;

//   // Reset the config register
//   rPiLink.buffer.ioConfig = 0;
// }

// Initialization routines for normal operation
void normalModeInit() {
  buzzer.play("v10>>g16>>>c16");
  while(buzzer.playCheck()) {
    // no-op to let the init sound finish
  }
  // Default state in wpilib is true. For green
  // and red, they will be specified to output
  // during run-time. Turning off by default
  ledYellow(true);
  ledGreen(false);
  ledRed(false);
  Serial.begin(9600);
  Serial.println("Normal mode init");
}

void normalModeLoop() {
  uint16_t battMV = readBatteryMillivolts();
  lvHelper.update(battMV);

  // Play the LV alert tune if we're in a low voltage state
  // TODO Disable for now until we can figure it out.
  // lvHelper.lowVoltageAlertCheck();

  // Shutdown motors if in low voltage mode
  // if (lvHelper.isLowVoltage()) {
  //   rPiLink.buffer.leftMotor = 0;
  //   rPiLink.buffer.rightMotor = 0;
  // }

  // Check heartbeat and shutdown motors if necessary
  if (millis() - lastHeartbeat > 1000) {
    rPiLink.buffer.leftMotor = 0;
    rPiLink.buffer.rightMotor = 0;
  }

  if (rPiLink.buffer.heartbeat) {
    lastHeartbeat = millis();
    rPiLink.buffer.heartbeat = false;
  }

  uint8_t builtinConfig = rPiLink.buffer.builtinConfig;
  if ((builtinConfig >> 7) & 0x1) {
    configureBuiltins(builtinConfig);
  }

  uint16_t ioConfig = rPiLink.buffer.ioConfig;
  if ((ioConfig >> 15) & 0x1) {
    // Also set the status flag
    rPiLink.buffer.status = 1;
    isConfigured = true;

    // Reset the config register
    rPiLink.buffer.ioConfig = 0;
    // configureIO(ioConfig);
  }

  // Update the built-ins
  rPiLink.buffer.builtinDioValues[0] = buttonA.isPressed();

  // Check if button A is pressed
  if (rPiLink.buffer.builtinDioValues[0]) {
    Serial.println("ButtonA is pressed...");
    rPiLink.buffer.leftMotor = 80;
    rPiLink.buffer.rightMotor = 80;
  }
  ledYellow(rPiLink.buffer.builtinDioValues[3]);

  if (builtinDio1Config == kModeDigitalIn) {
    rPiLink.buffer.builtinDioValues[1] = buttonB.isPressed();
  }
  else {
    ledGreen(rPiLink.buffer.builtinDioValues[1]);
  }

  if (builtinDio2Config == kModeDigitalIn) {
    rPiLink.buffer.builtinDioValues[2] = buttonC.isPressed();
  }
  else {
    ledRed(rPiLink.buffer.builtinDioValues[2]);
  }

  // Loop through all available IO pins
  // for (uint8_t i = 0; i < 5; i++) {
  //   switch (ioChannelModes[i]) {
  //     case kModeDigitalOut: {
  //       digitalWrite(ioDioPins[i], rPiLink.buffer.extIoValues[i] ? HIGH : LOW);
  //       // Serial.print("extIO ");Serial.print(i);Serial.println(" is DigitalOut");
  //     } break;
  //     case kModeDigitalIn: {
  //       int dIn =  digitalRead(ioDioPins[i]);
  //       rPiLink.buffer.extIoValues[i] = digitalRead(ioDioPins[i]);
  //       // if (lastServoAngle[i] != dIn) {
  //       //   Serial.print("extIO ");Serial.print(i);Serial.print(" is DigitalIn ");Serial.println(dIn);
  //       // }      
  //       // lastServoAngle[i] = dIn;
  //     } break;
  //     case kModeAnalogIn: {
  //       if (ioAinPins[i] != 0) {
  //         rPiLink.buffer.extIoValues[i] = analogRead(ioAinPins[i]);
  //       }
  //     } break;
  //     case kModePwm: {
  //       // Only allow writes to PWM if we're not currently locked out due to low voltage
  //       if (pwms[i].attached()) {
  //         if (!lvHelper.isLowVoltage()) {
  //           // Restrict servo movements for the Romi Arm         
  //             pwms[i].write(map(rPiLink.buffer.extIoValues[i], -400, 400, 0, 180));
  //         }
  //         else {
  //           // Attempt to zero out servo-motors in a low voltage mode
  //           pwms[i].write(120);
  //         }
  //       }
  //     } break;
  //   }
  // }

  // Motors
  motors.setSpeeds(rPiLink.buffer.leftMotor, rPiLink.buffer.rightMotor);
  indexPWM.write(0);

  // Encoders
  // TODO read the potentiometers instead of the encoders
  if (rPiLink.buffer.resetLeftEncoder) {
    rPiLink.buffer.resetLeftEncoder = false;
    // encoders.getCountsAndResetLeft();
  }

  if (rPiLink.buffer.resetRightEncoder) {
    rPiLink.buffer.resetRightEncoder = false;
    // encoders.getCountsAndResetRight();
  }

//   rPiLink.buffer.leftEncoder = encoders.getCountsLeft();
//   rPiLink.buffer.rightEncoder = encoders.getCountsRight();
  rPiLink.buffer.leftEncoder = 0;
  rPiLink.buffer.rightEncoder = 0;

  rPiLink.buffer.batteryMillivolts = battMV;
}

void setup() {
  rPiLink.init(20);

  // Set up the buzzer in playcheck mode
  buzzer.playMode(PLAY_CHECK);

  // Flip the right side motor to better match normal FRC setups
  //   motors.flipRightMotor(true);

  normalModeInit();
  // motors.setSpeeds(80, 80);
  // delay(1000);
  // motors.setSpeeds(0, 0);
  // delay(1000);

  // Setup analog Multiplexer to read the encoders.
  // setupAnalogMUX();
}

void loop() {
  // Get the latest data including recent i2c master writes
  rPiLink.updateBuffer();

  // Constantly write the firmware ident
  rPiLink.buffer.firmwareIdent = FIRMWARE_IDENT;

  // This can be removed.  We don't need the status flag and isConfigured
  if (isConfigured) {
    rPiLink.buffer.status = 1;
  }

  normalModeLoop();

  rPiLink.finalizeWrites();
}
