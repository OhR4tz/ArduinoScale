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
const byte WEIGHT_SAMPLES = 2;
const unsigned long SENSOR_TIMEOUT_MS = 2000;
const unsigned long CAPTURE_TIMEOUT_MS = 10000;
// Let the HX711 settle after power-up before the automatic startup tare.
const unsigned long STARTUP_TARE_DELAY_MS = 1000;

// Event-based weighing: one weight_g line per item placed on an empty platform.
// Each averaged reading takes about 0.2 s (2 samples at the HX711's 10 SPS), so the
// stable window below spans about 0.8 s. Short readings let a wobble cost only 0.2 s.
const float EMPTY_THRESHOLD_G = 2.0f;          // |weight| below this counts as empty.
const float LOAD_THRESHOLD_G = 5.0f;           // Weight at or above this counts as an item.
const float STABLE_TOLERANCE_G = 1.0f;         // Allowed spread across the stable window...
const float STABLE_TOLERANCE_FRACTION = 0.005f; // ...or 0.5% of the weight, whichever is larger.
const byte STABLE_READINGS = 4;                // Consecutive agreeing readings before sending.
const byte EMPTY_READINGS = 2;                 // Consecutive empty readings before re-arming.

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

// NOT_READY: needs calibration and tare. EMPTY: armed, waiting for an item.
// SETTLING: item detected, waiting for a stable weight. WEIGHED: sent; waiting for removal.
enum PlatformState { NOT_READY, EMPTY, SETTLING, WEIGHED };
PlatformState platformState = NOT_READY;
float stableSum = 0.0f;
float stableMin = 0.0f;
float stableMax = 0.0f;
byte stableCount = 0;
byte emptyCount = 0;
bool startupTarePending = true;

// USB and Bluetooth each get their own buffer so bytes from the two never mix.
struct CommandBuffer {
  char text[20];
  byte length;
  bool overflow;
};
CommandBuffer usbCommand = {};
CommandBuffer bluetoothCommand = {};

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
  status(F("# Commands over USB or Bluetooth (send newline): tare | calibrate | help"));
  status(F("# The scale tares itself at startup: power on with the platform empty."));
  status(F("# One weight_g reading is sent per item once its weight is stable."));
  status(F("# First use or recalibration: tare, add 100 g, then send calibrate."));
  status(F("# Successful calibration is saved in EEPROM. Bluetooth UART: 9600 baud."));
}

void startStableWindow(float grams) {
  stableSum = grams;
  stableMin = grams;
  stableMax = grams;
  stableCount = 1;
}

void cancelSettling() {
  if (platformState == SETTLING) {
    platformState = EMPTY; // An item still on the platform is re-detected afterwards.
  }
}

void processWeight(float grams) {
  switch (platformState) {
    case EMPTY:
      if (grams >= LOAD_THRESHOLD_G) {
        platformState = SETTLING;
        startStableWindow(grams);
        status(F("# Item detected; waiting for stable weight..."));
      }
      break;

    case SETTLING: {
      if (grams < LOAD_THRESHOLD_G) {
        platformState = EMPTY; // Removed (or too light) before it settled.
        break;
      }
      const float low = min(stableMin, grams);
      const float high = max(stableMax, grams);
      const float mean = (stableSum + grams) / (stableCount + 1);
      const float tolerance = max(STABLE_TOLERANCE_G, STABLE_TOLERANCE_FRACTION * fabsf(mean));
      if (high - low > tolerance) {
        startStableWindow(grams); // Still moving: restart from the latest reading.
        break;
      }
      stableSum += grams;
      stableMin = low;
      stableMax = high;
      ++stableCount;
      if (stableCount >= STABLE_READINGS) {
        const float stableGrams = stableSum / stableCount;
        Serial.print(F("weight_g,"));
        Serial.println(stableGrams, 2);
        bluetooth.print(F("weight_g,"));
        bluetooth.println(stableGrams, 2);
        platformState = WEIGHED;
        emptyCount = 0;
      }
      break;
    }

    case WEIGHED:
      if (fabsf(grams) < EMPTY_THRESHOLD_G) {
        if (++emptyCount >= EMPTY_READINGS) {
          platformState = EMPTY;
          status(F("# Platform empty; ready for next item."));
        }
      } else {
        emptyCount = 0;
      }
      break;

    case NOT_READY:
      break;
  }
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

void handleCommand(CommandBuffer &buffer) {
  buffer.text[buffer.length] = '\0';
  const char *command = buffer.text;
  if (buffer.overflow) {
    status(F("# ERROR: command too long. Send help."));
  } else if (buffer.length == 0) {
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

// Bluetooth note: HX711 reads briefly block interrupts, which can corrupt a SoftwareSerial
// byte that arrives at that moment. A garbled command gets an ERROR reply; just resend it.
void readCommands(Stream &port, CommandBuffer &buffer) {
  while (port.available() > 0) {
    const char incoming = (char)port.read();
    if (incoming == '\r' || incoming == '\n') {
      handleCommand(buffer);
      buffer.length = 0;
      buffer.overflow = false;
    } else if (!buffer.overflow) {
      if (buffer.length < sizeof(buffer.text) - 1) {
        buffer.text[buffer.length++] = incoming;
      } else {
        buffer.overflow = true;
      }
    }
  }
}

void finishCapture(long average) {
  if (captureMode == TARE) {
    zeroOffset = average;
    hasTare = true;
    platformState = EMPTY;
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
      platformState = WEIGHED; // Don't report the 100 g weight; wait for its removal.
      emptyCount = 0;
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
    status(F("# Saved calibration loaded. Taring the empty platform at startup."));
  } else {
    status(F("# No valid saved calibration. After the startup tare, add 100 g and send calibrate."));
  }
  showHelp();
}

void loop() {
  readCommands(Serial, usbCommand);
  readCommands(bluetooth, bluetoothCommand);
  unsigned long now = millis();
  if (captureMode != IDLE && now - captureStarted >= CAPTURE_TIMEOUT_MS) {
    captureMode = IDLE;
    resetSamples();
    status(F("# ERROR: capture timed out; previous tare/calibration retained. Retry command."));
  }
  // Startup tare, so the scale works with no laptop attached. A manual tare cancels it.
  // Runs after the timeout check: beginCapture() stamps a start time later than `now`.
  if (startupTarePending && (hasTare || captureMode != IDLE)) {
    startupTarePending = false;
  } else if (startupTarePending && now >= STARTUP_TARE_DELAY_MS) {
    startupTarePending = false;
    status(F("# Startup tare: keep the platform empty."));
    beginCapture(TARE);
  }

  // Never call read_average()/tare() here: they wait indefinitely on a missing sensor.
  // read() is only entered once DOUT indicates a sample is ready. The library
  // protects HX711 clock pulses from SoftwareSerial interrupts on the Uno.
  if (!scale.is_ready()) {
    if (!sensorTimedOut && now - lastReading >= SENSOR_TIMEOUT_MS) {
      sensorTimedOut = true;
      resetSamples();
      cancelSettling();
      status(F("# ERROR: HX711 not ready. Waiting for sensor; commands still work."));
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
    cancelSettling();
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
  if (calibrated && hasTare) {
    processWeight((float)(average - zeroOffset) / countsPerGram);
  }
}
