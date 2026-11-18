# ArduinoScale

A load-cell scale for an Elegoo Uno V3 / Arduino Uno and HX711. View weight in **Arduino IDE on Mac or Windows**, and send the same readings over Bluetooth to an app. Saved calibration survives unplugging the Arduino.

## 1. Upload with Arduino IDE

You need Arduino IDE, a USB data cable, the wired scale, and a known **100 g weight** for the first calibration.

1. Download or clone this repository, then open [`ArduinoScale/ArduinoScale.ino`](ArduinoScale/ArduinoScale.ino).
2. Open **Library Manager** and install **HX711 Arduino Library by Bogdan Necula**, version **0.7.5**. `SoftwareSerial` and `EEPROM` are included with the Uno board package.
3. Select **Tools > Board > Arduino AVR Boards > Arduino Uno**. If that option is missing, install **Arduino AVR Boards** through Boards Manager.
4. Connect the Arduino to your laptop with USB. Under **Tools > Port**, select the port that appears for the board. Mac ports often start with `/dev/cu.`; Windows ports use `COM` numbers.
5. Click **Upload** and wait for it to finish.

## 2. Open Serial Monitor and calibrate once

1. Open **Tools > Serial Monitor**.
2. Set **9600 baud** and **Newline**.
3. Wait for the startup messages. Empty the weighing platform and let it settle.
4. Type `tare` in the message box and send it. Wait for **`# Tare complete`**.
5. Place your **100 g weight** on the platform and let it settle.
6. Send `calibrate`. Keep the weight still until **`# Calibration saved`** appears.
7. Remove the weight and start weighing. Readings are in grams:

```text
weight_g,0.12
weight_g,100.03
```

If startup already says **`# Saved calibration loaded`**, use the everyday steps below instead of calibrating again. Tare and calibration each usually take about two seconds. Two decimal places describe the display format, not guaranteed accuracy.

## 3. Everyday use

1. Connect the Arduino and open **Serial Monitor** at **9600 baud**, with **Newline** selected.
2. Wait for **`# Saved calibration loaded`**.
3. Empty the platform, send `tare`, and wait for **`# Tare complete`**.
4. Put an object on the scale and read its weight.

You do **not** need the 100 g weight each time. The calibration factor is saved in EEPROM, but the empty-platform zero is set with `tare` after each reset. Opening Serial Monitor may reset the Uno. Recalibrate if readings drift or the scale's mounting changes.

| Command | What it does |
| --- | --- |
| `tare` or `t` | Zero the empty platform. |
| `calibrate` or `c` | Calibrate with 100 g after taring, and save the result. |
| `help` or `h` | Show instructions. |

Send commands in lowercase through **USB Serial Monitor**. Bluetooth receives readings and status messages; this sketch does not accept Bluetooth commands.

## 4. Connect over Bluetooth

These steps are for an **HC-05 Bluetooth Classic module**. The receiving app must support **Classic SPP/RFCOMM**. A BLE-only app cannot connect to an HC-05. An HM-10 is a different, BLE module and needs a different app connection method.

### Pair and connect

1. Power the Arduino and module. USB can stay connected, and Arduino Serial Monitor can remain open while Bluetooth sends readings.
2. Use Serial Monitor to calibrate if necessary, then tare the empty platform as described above. Keep USB connected if you need to send another `tare` command.
3. On the receiving device, turn on Bluetooth and scan for nearby devices. The module is commonly named **HC-05**, though its configured name can differ.
4. Pair with the module. If asked for a PIN, use its configured PIN; common factory values are **1234** or **0000**, depending on the module/firmware.
5. Open an app that supports **Bluetooth Classic serial**, select the paired module, and connect. Pairing alone does not open the data connection. On Android, allow the app's requested **Nearby devices** permissions.
6. The app should receive `weight_g,...` lines as you weigh objects. Disconnect another Bluetooth client first if the module is already connected elsewhere.

An Android phone with a Classic serial app is a useful first connection test. The **Samsung Watch8 Classic app must implement the Classic serial connection**; direct watch connectivity still needs testing with the actual app and module. Viewing USB output on a Mac requires only Arduino IDE.

The module's serial connection to the Uno must be configured for **9600 baud, 8N1, in normal data mode**. This is the module's UART setting, not a baud setting in the watch app. The sketch does not configure the module with AT commands.

### For the app developer

Connect as an RFCOMM client to the HC-05's standard SPP UUID:

```text
00001101-0000-1000-8000-00805F9B34FB
```

- Receive ASCII records separated by **CRLF** (`\r\n`).
- Parse `weight_g,123.45` as a signed decimal weight in grams, at most twice per second.
- Lines beginning with `#` contain status or errors. They can appear between readings.
- Buffer bytes until a newline. A Bluetooth read may contain part of a line or several lines.
- Show unavailable/stale data after an error, disconnection, or missing fresh measurements instead of presenting the last reading as current.

See Android's [Classic connection guide](https://developer.android.com/develop/connectivity/bluetooth/connect-bluetooth-devices) and [Bluetooth permissions guide](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions). HC-05 pairing examples are described in this [module connection guide](https://www.martyncurrey.com/arduino-with-hc-05-bluetooth-module-in-slave-mode/).

## Wiring reference

| Signal | Uno pin |
| --- | --- |
| HX711 DT / DOUT | D3 |
| HX711 SCK | D4 |
| Bluetooth TXD to Uno RX | D8 |
| Uno TX to Bluetooth RXD | D9 |

This is the sketch's signal pinout. Follow the wiring and voltage requirements for your particular breakout boards. The HX711 library is [`HX711.h` by bogde](https://github.com/bogde/HX711), rather than the original tutorial's `Q2HX711.h`.

## Troubleshooting

| What you see | What to do |
| --- | --- |
| Port busy / cannot open port | Close other serial terminals. Only one program can hold the USB port at a time. |
| Garbled Serial Monitor text | Set the monitor to **9600 baud**. |
| Commands do nothing | Select **Newline**, use lowercase commands, and wait for startup to finish. |
| `No valid saved calibration` | Follow the one-time tare and 100 g calibration steps. |
| Calibration was not saved | The current session can still weigh. Keep or replace the 100 g weight and retry `calibrate`. |
| HX711 error / no readings | Readings pause while the sensor is unavailable or saturated. Resolve the error, then retry any failed tare/calibration. |
| Bluetooth module is not listed | Use a Classic-capable receiver, check module power/data mode, and disconnect existing clients. A BLE-only scan will not find an HC-05. |
| Paired, but no Bluetooth readings | Connect within the Classic serial app, grant permissions, confirm module UART is 9600, and complete USB calibration/tare. |

An interrupted EEPROM save can require calibration again. Saved data is checked at startup; EEPROM is written only after successful calibration, and unchanged values are not rewritten.

## Optional: Windows PowerShell terminal

Arduino IDE is the main workflow on both Mac and Windows. Windows users can alternatively close Serial Monitor and run this from the project folder, replacing `COMx` with the Arduino's port:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\terminal.ps1 -Port COMx
```

Wait for startup, then send the same commands. Type `quit` to release the port before using Arduino IDE again. The execution-policy setting applies only to that launched PowerShell process.

## Project files and validation

- [`ArduinoScale/ArduinoScale.ino`](ArduinoScale/ArduinoScale.ino): Uno firmware.
- [`terminal.ps1`](terminal.ps1): optional Windows USB terminal.
- `.tools/`: ignored local library/build cache; not required for Arduino IDE setup.

The sketch compiled for Arduino Uno with Arduino AVR Boards 1.8.7 and HX711 0.7.5. USB output in Arduino IDE has been confirmed on the scale. Reset/power-cycle persistence and watch connectivity still need explicit hardware verification.

Based on the [Maker Portal load-cell and HX711 tutorial](https://makersportal.com/blog/2019/5/12/smartphone-arduino-weighing-scale-with-load-cell-and-hx711).
