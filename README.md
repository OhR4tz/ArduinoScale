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
3. Keep the platform empty while the scale starts. It tares itself about one second after power-up. Wait for **`# Tare complete`**. If anything was on the platform, empty it and send `tare`.
4. Optional: send `tare` again at any time to re-zero the empty platform.
5. Place your **100 g weight** on the platform and let it settle.
6. Send `calibrate`. Keep the weight still until **`# Calibration saved`** appears.
7. Remove the weight and start weighing. The scale sends **one reading per item**, in grams, once the item's weight is stable:

```text
# Item detected; waiting for stable weight...
weight_g,100.03
# Platform empty; ready for next item.
```

If startup already says **`# Saved calibration loaded`**, use the everyday steps below instead of calibrating again. Tare and calibration each usually take about two seconds. Two decimal places describe the display format, not guaranteed accuracy.

## 3. Everyday use

1. Power the scale with the platform **empty**. No laptop is needed.
2. The scale loads its saved calibration and tares itself. Over USB you will see **`# Saved calibration loaded`**, then **`# Tare complete`**.
3. If something was on the platform at power-up, empty it and send `tare` over USB or Bluetooth.
4. Put an object on the scale. After about 0.8 seconds of steady readings, the scale sends a single `weight_g` line.
5. Remove the object. When the platform reads empty, the scale is ready for the next item.

Readings are event based: the scale sends nothing while the platform is empty, and one reading per item after it settles. Adding more weight to an item that has already been weighed does not send a new reading. Empty the platform first. Items under **5 g** are not reported. These thresholds are constants at the top of the sketch:

| Constant | Default | Meaning |
| --- | --- | --- |
| `LOAD_THRESHOLD_G` | 5 g | Minimum weight that counts as an item. |
| `EMPTY_THRESHOLD_G` | 2 g | The platform counts as empty below this. |
| `STABLE_TOLERANCE_G` / `STABLE_TOLERANCE_FRACTION` | 1 g / 0.5% | Maximum spread, whichever is larger, for readings to count as stable. |
| `STABLE_READINGS` | 4 | Consecutive stable readings, about 0.2 s each, before sending. |
| `EMPTY_READINGS` | 2 | Consecutive empty readings before the scale re-arms. |

If the zero drifts so the empty platform reads 2 g or more, the scale will not re-arm. Send `tare` again.

You do **not** need the 100 g weight each time. The calibration factor is saved in EEPROM. The empty-platform zero is set by the automatic tare at each startup. Opening Serial Monitor may reset the Uno. Recalibrate if readings drift or the scale's mounting changes.

| Command | What it does |
| --- | --- |
| `tare` or `t` | Zero the empty platform. |
| `calibrate` or `c` | Calibrate with 100 g after taring, and save the result. |
| `help` or `h` | Show instructions. |

Send commands in lowercase, ending with a newline, through **USB Serial Monitor** or a **Bluetooth** connection. Replies and readings go to both. A Bluetooth command can occasionally arrive garbled, because HX711 reads briefly block the Bluetooth receiver. The scale then replies `# ERROR: unknown command`; send the command again.

## 4. Connect over Bluetooth

These steps are for an **HC-05 Bluetooth Classic module**. The receiving app must support **Classic SPP/RFCOMM**. A BLE-only app cannot connect to an HC-05. An HM-10 is a different, BLE module and needs a different app connection method.

### Pair and connect

1. Power the Arduino and module. USB can stay connected, and Arduino Serial Monitor can remain open while Bluetooth sends readings.
2. Calibrate over USB once if necessary. After that, the scale tares itself at power-up, and `tare` can also be sent over Bluetooth.
3. On the receiving device, turn on Bluetooth and scan for nearby devices. The module is commonly named **HC-05**, though its configured name can differ.
4. Pair with the module. If asked for a PIN, use its configured PIN; common factory values are **1234** or **0000**, depending on the module/firmware.
5. Open an app that supports **Bluetooth Classic serial**, select the paired module, and connect. Pairing alone does not open the data connection. On Android, allow the app's requested **Nearby devices** permissions.
6. The app should receive one `weight_g,...` line each time an object is placed and settles. Disconnect another Bluetooth client first if the module is already connected elsewhere.

An Android phone with a Classic serial app is a useful first connection test. The **Samsung Watch8 Classic app must implement the Classic serial connection**; direct watch connectivity still needs testing with the actual app and module. Viewing USB output on a Mac requires only Arduino IDE.

The module's serial connection to the Uno must be configured for **9600 baud, 8N1, in normal data mode**. This is the module's UART setting, not a baud setting in the watch app. The sketch does not configure the module with AT commands.

### For the app developer

Connect as an RFCOMM client to the HC-05's standard SPP UUID:

```text
00001101-0000-1000-8000-00805F9B34FB
```

- Receive ASCII records separated by **CRLF** (`\r\n`).
- Parse `weight_g,123.45` as a decimal weight in grams. Each line is a single event: one stable weight for one item placed on an empty platform. No lines are sent while the platform is empty or while an item is settling.
- `# Item detected; waiting for stable weight...` means a reading is coming. `# Platform empty; ready for next item.` means the previous item was removed.
- Lines beginning with `#` contain status or errors. They can appear between readings.
- Send `tare\n` to re-zero the empty platform. `# Tare complete` confirms it. If you get `# ERROR: unknown command`, the bytes were garbled, so resend.
- Buffer bytes until a newline. A Bluetooth read may contain part of a line or several lines.
- Silence is normal between items. Do not treat it as a stale connection. After an error or disconnection, show the reading as unavailable. Clear it when the platform-empty status arrives.

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
| Paired, but no Bluetooth readings | Connect within the Classic serial app, grant permissions, confirm module UART is 9600, complete the one-time USB calibration, and make sure the platform was empty at power-up. |

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
