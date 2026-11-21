# StackTerminal

StackTerminal is a UART terminal for the M5Stack Atom VoiceS3R (C126-ECHO) and Atomic Display Base (A115). It runs a dark LVGL interface at 720p over HDMI and accepts a USB HID Boot keyboard using the Polish programmer layout.

Two dots at the right end of the top bar show UART activity. Green indicates receive (RX), and blue indicates transmit (TX).

## Hardware connections

```mermaid
flowchart LR
    supply["5 V USB-C supply"] -->|USB-C| power["M5Stack Grove to USB-C (A140)"]
    power -->|Grove branch, 5 V and GND only| hub["M5Stack Grove Hub (U006)"]
    hub ---|Grove branch, UART and GND| dut["Device under test (DUT)"]
    subgraph terminal["StackTerminal assembly"]
        atom["Atom VoiceS3R (C126-ECHO)"]
        atom ---|Docked via pin headers| base["Atomic Display Base (A115)"]
    end

    hub ---|Main port to Atom Grove, UART and power| atom
    atom ---|Atom USB-C port| adapter["USB-C to USB-A adapter"]
    adapter ---|USB-A| keyboard["USB HID keyboard"]
    base -->|HDMI| monitor["720p HDMI monitor"]
```

Connect the A140 to one branch port on the Grove Hub, the DUT to another branch, and the Atom's Grove port to the hub's main port. The hub wires its ports in parallel. For the 5 V supply shown, connect only 5 V and GND between the A140 and the hub. The A140 also routes USB D+ and D− directly to the two Grove signal pins, which the DUT connection uses for UART. [A140 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/537/SCH_Grove2USB-C_V1.0.pdf) · [Grove Hub documentation](https://docs.m5stack.com/en/unit/Unit_Hub)

Seat the Atom in the Atomic Display Base and connect the monitor to the base's HDMI port. Plug a data-capable USB-C to USB-A adapter into the Atom's own USB-C port, then plug the keyboard into the adapter. [Atomic Display Base documentation](https://docs.m5stack.com/en/atom/Atomic_Display_Base)

### Grove UART pinout

| Grove wire | Atom pin | DUT connection |
| --- | --- | --- |
| Black | GND | Common ground |
| White | GPIO1 / RX | DUT TX |
| Yellow | GPIO2 / TX | DUT RX |
| Red | 5 V supply | Leave disconnected at the DUT end if the DUT has its own power supply |

UART uses 3.3 V logic. Use the appropriate converter for 5 V UART, RS-232, or RS-485. The red Grove wire carries 5 V from the power supply. [Atom VoiceS3R pinout](https://docs.m5stack.com/en/core/Atom_VoiceS3R#pinmap)

## Getting started

1. Disconnect the Grove power feed. Connect the Atom directly to your computer over USB-C and put it into download mode using the reset button. [M5Stack flashing instructions](https://docs.m5stack.com/en/arduino/atom_echos3r/program)
2. Run `pio run -t upload` from the project directory.
3. Wire the setup as shown above. Type a character and check that it appears in the input field at the bottom of the screen.
4. Press Ctrl+Tab and set the terminal's UART parameters to match the DUT. The first boot uses 115200 baud, 8 data bits, no parity, and 1 stop bit (8N1).

## Keyboard controls

| Key | Action |
| --- | --- |
| Enter | Send the command. Append CR and LF in ASCII mode |
| Shift+Enter | Insert CR and LF at the cursor in ASCII mode |
| Ctrl+Enter | Send the command without appending a line ending |
| Up / Down | Recall sent commands for the input format. Moving past the newest command restores the draft |
| Left / Right, Home / End | Move the cursor or jump to the start or end of the line |
| Insert | Toggle insert and overwrite modes |
| Page Up / Page Down | Scroll received lines by one page. The last page follows new data |
| Ctrl+Up / Ctrl+Down | Change the font size |
| Ctrl+Right / Ctrl+Left | Cycle the transmit mode forward or backward through ASCII, HEX, DEC LE, and DEC BE |
| Left Alt+Right / Left Alt+Left | Cycle the receive display forward or backward through the same modes |
| Ctrl+Tab | Open UART and color settings |
| Right Alt + letter | Type a Polish character |
| Ctrl+A through Ctrl+Z | Insert an ASCII control code from SOH through SUB in ASCII mode |

In Settings, use Up and Down to select a field. Use Left and Right to change the value, or type a baud rate or a six-digit RGB color. Enter saves the settings and current font size to nonvolatile storage (NVS). Esc or Ctrl+Tab closes Settings without saving.

The transmit mode appears at the right end of the input row. The receive mode appears beside the activity dots in the top bar. Both start in ASCII and can be set independently.

In HEX transmit mode, two hex digits encode one raw byte. Spaces are skipped. `FF` sends one byte, and `01 02 FF` sends three bytes. In DEC LE or DEC BE, type unsigned 32-bit values from 0 to 4294967295 separated by spaces. `16909060` sends `04 03 02 01` in DEC LE and `01 02 03 04` in DEC BE. Numeric modes send only the encoded bytes. ASCII, HEX, and decimal each keep a draft and up to 100 sent commands in RAM.

The ASCII receive display splits incoming UTF-8 text on CR, LF, or CR+LF. A CR+LF pair ends one history line and appears as two outlined labels. Other control characters, such as SOH, TAB, and ESC, also have outlined labels. The terminal keeps 512 text lines in RAM. Long lines wrap on screen. The ASCII receive limit is 2,048 characters per line.

HEX displays the original bytes in arrival order, including line endings. DEC LE and DEC BE group every four consecutive bytes as an unsigned 32-bit integer. DEC LE matches the ESP32-S3's native byte order. Pending bytes appear as bracketed hex until a complete word arrives. Numeric modes retain up to 512 rows of 16 bytes each. Diagnostic messages occupy their own rows and start a new group. [ESP32-S3 byte order](https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32s3/api-reference/peripherals/spi_master.html)

## Quick hardware test

1. Send `41 01 0D 0A 42 0A 43 0D` to the Atom over UART. The screen should show three lines, with separate `SOH`, `CR`, and `LF` labels after `A` on the first line.
2. Type `A`, press Shift+Enter, type `B`, and press Ctrl+Enter. The DUT should receive `41 0D 0A 42`.
3. Open Settings with Ctrl+Tab. Change a color and the baud rate, press Enter to save, and reboot the Atom. Verify that both values persist.
4. After a restart, send `01 02 03 04 05` as the first UART bytes. Press Left Alt+Right three times. The history should show `01 02 03 04 05`, then `67305985 [05]`, then `16909060 [05]`. The RX label should follow each switch while the input row still shows `TX ASCII`.
5. Press Ctrl+Right twice to select `TX DEC LE`. Type `16909060 X` and press Enter. The editor should reject `X`, and the DUT should receive exactly `04 03 02 01`. The RX label should still show `DEC BE (u32)`.

## USB keyboard diagnostics

1. Flash the diagnostic build with `pio run -e stackterminal-usb-diag -t upload`.
2. Start the Atom with HDMI connected. Unplug and reconnect the keyboard, then press a key. If testing through an optional USB hub, keep the hub connected and replug the keyboard at the hub.
3. Read the USB messages in the terminal history. `USB root` shows whether the Atom's USB controller detects an attached device, such as the keyboard or an optional hub. `logical power` reports the controller state. Later messages show the device count, HID keyboard detection, disconnects, and the length of the first input report.

`pio run -t upload` flashes the standard build. [ESP-IDF USB Host documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/usb_host.html)

The bitmap fonts were generated from [JetBrains Mono 2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304) using `lv_font_conv 1.5.3`. See the [font license](src/fonts/OFL.txt).
