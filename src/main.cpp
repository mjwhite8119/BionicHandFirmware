#include <Arduino.h>

#include <PololuRPiSlave.h>
#include <Wire.h>
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
// #define ENABLE_INDEX A4 // PWM
// #define PHASE_INDEX A5 // Direction
#define ENABLE_INDEX 5 // PWM
#define PHASE_INDEX 4 // Direction


// Servo pwms[5];
// Servo indexPWM;

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
int s0 = 4;
int s1 = 5;
int s2 = 6;
int s3 = 7;

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
  // indexPWM.attach(A5);
  // Setup motor pins
  pinMode(PHASE_INDEX,OUTPUT);
  pinMode(ENABLE_INDEX,OUTPUT);
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
    // Serial.print("Potentiometer ");
    // Serial.print(i);
    // Serial.print(" is at ");
    // Serial.print(percentage);
    // Serial.println("%");

    // TODO change to array
    if (i == 0) {
      rPiLink.buffer.leftEncoder = percentage;
    } else {
      rPiLink.buffer.rightEncoder = percentage;
    }
    
    // delay(1000);
  }
}

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

  // RPi wants the status to be 1 otherwise it will report a brownout.
  rPiLink.buffer.status = 1;

  // Flip the right side motor to better match normal FRC setups
  //   motors.flipRightMotor(true);

  Serial.begin(9600);
  Serial.println("Normal mode init");
}

void startMotors(int speed) {
  // FORWARD
  digitalWrite(PHASE_INDEX,HIGH); // direction
  analogWrite(ENABLE_INDEX,speed); // PWM
  // io.digitalWrite(SX1509_AIN2, LOW); // Enable
  // io.analogWrite(SX1509_AIN2, speed); // Enable
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

  // Update the built-ins
  rPiLink.buffer.builtinDioValues[0] = buttonA.isPressed();

  // Check if button A is pressed
  if (rPiLink.buffer.builtinDioValues[0]) {
    Serial.println("ButtonA is pressed...");
    rPiLink.buffer.leftMotor = 200;
    rPiLink.buffer.rightMotor = 0;
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

  // Motors
  motors.setSpeeds(rPiLink.buffer.leftMotor, rPiLink.buffer.rightMotor);
  startMotors(rPiLink.buffer.leftMotor); 
  
  // Encoders
  readEncoders();

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
  // rPiLink.buffer.leftEncoder = 0;
  // rPiLink.buffer.rightEncoder = 0;

  rPiLink.buffer.batteryMillivolts = battMV;
}

void setup() {
  rPiLink.init(20);

  // Set up the buzzer in playcheck mode
  buzzer.playMode(PLAY_CHECK);

  normalModeInit();
  
  configureMotors();

  // Setup analog Multiplexer to read the encoders.
  setupAnalogMUX();
}

void loop() {
  // Get the latest data including recent i2c master writes
  rPiLink.updateBuffer();

  // Constantly write the firmware ident
  rPiLink.buffer.firmwareIdent = FIRMWARE_IDENT;

  normalModeLoop();

  rPiLink.finalizeWrites();
}
