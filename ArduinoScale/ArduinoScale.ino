#include <HX711.h>  // HX711 Arduino Library by Bogdan Necula (bogde), version 0.7.5
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <math.h>
#include <string.h>

// Tutorial pinout: module TX -> Uno D8; module RX <- Uno D9.
const byte HX711_DATA_PIN = 3;
const byte HX711_CLOCK_PIN = 4;
const unsigned long USB_BAUD = 9600;
const unsigned long BLUETOOTH_BAUD = 9600;
const float CALIBRATION_MASS_G = 100.0f;
const byte CAPTURE_SAMPLES = 20;
const byte WEIGHT_SAMPLES = 5;
const unsigned long SENSOR_TIMEOUT_MS = 2000;
const unsigned long CAPTURE_TIMEOUT_MS = 10000;

HX711 scale;
SoftwareSerial bluetooth(8, 9); // Arduino RX, TX

enum CaptureMode { IDLE, TARE, CALIBRATE };
CaptureMode captureMode = IDLE;
bool hasTare = false;
bool calibrated = false;
bool sensorTimedOut = false;
bool sensorSaturated = false;
long zeroOffset = 0;
float countsPerGram = 1.0f;
long sampleSum = 0;
byte sampleCount = 0;
unsigned long captureStarted = 0;
unsigned long lastReading = 0;
unsigned long lastWeightSent = 0;
char command[20];
byte commandLength = 0;
bool commandOverflow = false;

// EEPROM bytes 0..7: committed marker, format version, 4-byte factor, CRC16.
// Only calibration is persistent: the empty-platform zero must be set each boot.
const byte CALIBRATION_MARKER = 0xA5;
const byte CALIBRATION_VERSION = 1;
static_assert(sizeof(float) == 4, "EEPROM calibration format requires a 4-byte float");
const int EEPROM_VERSION_ADDRESS = 1;
const int EEPROM_FACTOR_ADDRESS = 2;
const int EEPROM_CHECKSUM_ADDRESS = 6;

uint16_t calibrationChecksum(float factor) {
  uint16_t crc = 0xFFFF;
  const byte *bytes = reinterpret_cast<const byte *>(&factor);
  for (byte i = 0; i <= sizeof(factor); ++i) {
    crc ^= (i == 0) ? CALIBRATION_VERSION : bytes[i - 1];
    for (byte bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
  }
  return crc;
}

bool loadCalibration(float &factor) {
  if (EEPROM.read(0) != CALIBRATION_MARKER ||
      EEPROM.read(EEPROM_VERSION_ADDRESS) != CALIBRATION_VERSION) {
    return false;
  }
  float savedFactor;
  uint16_t savedChecksum;
  EEPROM.get(EEPROM_FACTOR_ADDRESS, savedFactor);
  EEPROM.get(EEPROM_CHECKSUM_ADDRESS, savedChecksum);
  // Valid captures have at least 100 counts of change over the 100 g mass.
  // The maximum is bounded by the signed 24-bit ADC's full span / 100 g.
  if (!isfinite(savedFactor) || fabsf(savedFactor) < 1.0f ||
      fabsf(savedFactor) > 167772.16f ||
      savedChecksum != calibrationChecksum(savedFactor)) {
    return false;
  }
  factor = savedFactor;
  return true;
}

bool saveCalibration(float factor) {
  float verifiedFactor;
  // Avoid even rewriting the commit marker when nothing changed.
  if (loadCalibration(verifiedFactor) && verifiedFactor == factor) {
    return true;
  }
  EEPROM.update(0, 0); // Incomplete writes must never look committed.
  EEPROM.update(EEPROM_VERSION_ADDRESS, CALIBRATION_VERSION);
  EEPROM.put(EEPROM_FACTOR_ADDRESS, factor); // put() updates changed bytes only.
  const uint16_t checksum = calibrationChecksum(factor);
  EEPROM.put(EEPROM_CHECKSUM_ADDRESS, checksum);
  EEPROM.update(0, CALIBRATION_MARKER); // Commit only after the record is written.
  return loadCalibration(verifiedFactor) && verifiedFactor == factor;
}

// Status lines start with '#'; machine-readable measurements start with 'weight_g,'.
void status(const __FlashStringHelper *message) {
  Serial.println(message);
  bluetooth.println(message);
}

void resetSamples() {
  sampleSum = 0;
  sampleCount = 0;
}

void showHelp() {
  status(F("# Commands over USB (send newline): tare | calibrate | help"));
  status(F("# Empty platform and send tare after every reset."));
  status(F("# First use or recalibration: tare, add 100 g, then send calibrate."));
  status(F("# Successful calibration is saved in EEPROM. Bluetooth UART: 9600 baud."));
}

void beginCapture(CaptureMode mode) {
  captureMode = mode;
  captureStarted = millis();
  resetSamples();
  if (mode == TARE) {
    status(F("# Taring: keep the empty platform still..."));
  } else {
    status(F("# Calibrating: keep the 100 g weight still..."));
  }
}

void handleCommand() {
  command[commandLength] = '\0';
  if (commandOverflow) {
    status(F("# ERROR: command too long. Send help."));
  } else if (commandLength == 0) {
    return; // Ignore the LF after a CR and empty lines.
  } else if (strcmp(command, "help") == 0 || strcmp(command, "h") == 0) {
    showHelp();
  } else if (captureMode != IDLE) {
    status(F("# Busy capturing; wait for completion or timeout."));
  } else if (strcmp(command, "tare") == 0 || strcmp(command, "t") == 0) {
    beginCapture(TARE);
  } else if (strcmp(command, "calibrate") == 0 || strcmp(command, "c") == 0) {
    if (!hasTare) {
      status(F("# ERROR: empty the platform and send tare first."));
    } else {
      beginCapture(CALIBRATE);
    }
  } else {
    status(F("# ERROR: unknown command. Send help."));
  }
}

void readCommands() {
  while (Serial.available() > 0) {
    const char incoming = (char)Serial.read();
    if (incoming == '\r' || incoming == '\n') {
      handleCommand();
      commandLength = 0;
      commandOverflow = false;
    } else if (!commandOverflow) {
      if (commandLength < sizeof(command) - 1) {
        command[commandLength++] = incoming;
      } else {
        commandOverflow = true;
      }
    }
  }
}

void finishCapture(long average) {
  if (captureMode == TARE) {
    zeroOffset = average;
    hasTare = true;
    if (calibrated) {
      status(F("# Tare complete. Ready to weigh using the current calibration."));
    } else {
      status(F("# Tare complete. Add 100 g and send calibrate."));
    }
  } else {
    const long delta = average - zeroOffset;
    // Accept either load-cell polarity, but reject an effectively empty platform.
    // This checks signal size; the user must supply the actual 100 g mass.
    if (delta > -100 && delta < 100) {
      status(F("# ERROR: calibration change too small; previous calibration retained. Add 100 g and retry."));
    } else {
      countsPerGram = (float)delta / CALIBRATION_MASS_G;
      calibrated = true;
      status(F("# Calibration complete. Remove 100 g; weight_g readings are in grams."));
      if (saveCalibration(countsPerGram)) {
        status(F("# Calibration saved. After reset, only tare the empty platform."));
      } else {
        status(F("# ERROR: calibration not saved; usable this session. Retry calibrate to save."));
      }
    }
  }
  captureMode = IDLE;
  resetSamples();
}

void setup() {
  Serial.begin(USB_BAUD);
  bluetooth.begin(BLUETOOTH_BAUD);
  scale.begin(HX711_DATA_PIN, HX711_CLOCK_PIN);
  lastReading = millis();
  status(F("# ArduinoScale ready. USB: 9600 baud."));
  calibrated = loadCalibration(countsPerGram);
  if (calibrated) {
    status(F("# Saved calibration loaded. Empty platform and send tare to start weighing."));
  } else {
    status(F("# No valid saved calibration. Tare, add 100 g, then send calibrate."));
  }
  showHelp();
}

void loop() {
  readCommands();
  unsigned long now = millis();
  if (captureMode != IDLE && now - captureStarted >= CAPTURE_TIMEOUT_MS) {
    captureMode = IDLE;
    resetSamples();
    status(F("# ERROR: capture timed out; previous tare/calibration retained. Retry command."));
  }

  // Never call read_average()/tare() here: they wait indefinitely on a missing sensor.
  // read() is only entered once DOUT indicates a sample is ready. The library
  // protects HX711 clock pulses from SoftwareSerial interrupts on the Uno.
  if (!scale.is_ready()) {
    if (!sensorTimedOut && now - lastReading >= SENSOR_TIMEOUT_MS) {
      sensorTimedOut = true;
      resetSamples();
      status(F("# ERROR: HX711 not ready. Waiting for sensor; USB commands still work."));
    }
    return;
  }

  const long raw = scale.read();
  now = millis();
  lastReading = now;
  if (sensorTimedOut) {
    sensorTimedOut = false;
    status(F("# HX711 readings resumed."));
  }
  // Signed 24-bit limits indicate saturation, not a usable measurement.
  if (raw == 8388607L || raw == -8388608L) {
    resetSamples();
    if (!sensorSaturated) {
      sensorSaturated = true;
      status(F("# ERROR: HX711 saturated; weight readings paused."));
    }
    if (captureMode != IDLE) {
      captureMode = IDLE;
      status(F("# ERROR: HX711 saturated; capture cancelled."));
    }
    return;
  }
  if (sensorSaturated) {
    sensorSaturated = false;
    status(F("# HX711 saturation cleared; readings resumed."));
  }

  sampleSum += raw; // 20 signed 24-bit samples fit in a signed 32-bit long.
  ++sampleCount;
  if (captureMode != IDLE) {
    if (sampleCount == CAPTURE_SAMPLES) {
      finishCapture(sampleSum / CAPTURE_SAMPLES);
    }
    return;
  }
  if (sampleCount < WEIGHT_SAMPLES) {
    return;
  }
  const long average = sampleSum / WEIGHT_SAMPLES;
  resetSamples();
  if (calibrated && hasTare && now - lastWeightSent >= 500) {
    const float grams = (float)(average - zeroOffset) / countsPerGram;
    Serial.print(F("weight_g,"));
    Serial.println(grams, 2);
    bluetooth.print(F("weight_g,"));
    bluetooth.println(grams, 2);
    lastWeightSent = now;
  }
}
