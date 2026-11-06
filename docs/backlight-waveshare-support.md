**Board:** ESP32-S3-Touch-LCD-4.3 (ESP-IDF v6.0, `driver/i2c_master.h`)

**Problem:** When the backlight is turned on via the CH422G, the CH343P UART USB port disconnects and reconnects (Windows plays the device connect sound). Both USB ports are powered. I don't know whether the ESP32 reboots (I have only checked this by ear so far). The native USB mux line (EXIO5) is never touched after init.

**Code.** The CH422G is driven as two I2C devices, `0x24` (system register) and `0x38` (EXIO0-7 output byte). It is the same sequence as the Waveshare example:

```c
// EXIO bit assignments (from the schematic)
#define CH422G_TP_RST     (1 << 1)   // touch reset, low = in reset
#define CH422G_BACKLIGHT  (1 << 2)   // LCD backlight enable
#define CH422G_LCD_RST    (1 << 3)   // LCD reset, low = in reset
#define CH422G_SD_CS      (1 << 4)   // SD card CS, low = selected
#define CH422G_USB_SEL    (1 << 5)   // native USB / CAN mux, low = USB

#define ADDR_SYS       0x24
#define ADDR_OUT       0x38
#define SYS_IO_OUTPUT  0x01          // EXIO pins are outputs
#define INITIAL_OUT    (CH422G_TP_RST | CH422G_LCD_RST | CH422G_SD_CS)  // backlight off, USB_SEL low

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t val)
{
    return i2c_master_transmit(dev, &val, 1, I2C_MASTER_TIMEOUT_MS);   // 400 kHz bus
}

// init (after the I2C bus is up)
i2c_device_config_t cfg = {};
cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
cfg.scl_speed_hz    = 400000;
cfg.device_address = ADDR_SYS;  i2c_master_bus_add_device(bus, &cfg, &s_sys);
cfg.device_address = ADDR_OUT;  i2c_master_bus_add_device(bus, &cfg, &s_out);
write_reg(s_sys, SYS_IO_OUTPUT);
s_out_val = INITIAL_OUT;
write_reg(s_out, s_out_val);

// set/clear lines, keeping a shadow copy of the output byte
esp_err_t ch422g_set(uint8_t mask, bool on)
{
    uint8_t val = on ? (s_out_val | mask) : (s_out_val & ~mask);
    esp_err_t err = write_reg(s_out, val);
    if (err == ESP_OK) s_out_val = val;
    return err;
}

// backlight on / off
ch422g_set(CH422G_BACKLIGHT, true);    // writes 0x1E to 0x38 (TP_RST|BACKLIGHT|LCD_RST|SD_CS)
ch422g_set(CH422G_BACKLIGHT, false);   // writes 0x1A to 0x38
```

**Questions:**
1. Can switching EXIO2 (backlight) cause the CH343P USB port to drop? For example, a supply dip from the backlight driver, or the backlight enable sharing a net with something on the UART USB path.
2. Is there anything on the board that ties backlight power to the USB-UART bridge (reset or power)?
3. Any recommended decoupling or a hardware revision with a fix?

---

## Waveshare's reply (received 2026-11-06)

They checked the schematic and the CH422G datasheet:

- Address mapping and output values are correct: `0x24` is the 7-bit system command address, `0x01` makes EXIO0-7 outputs, `0x38` is the EXIO output byte; EXIO2 high (`0x1E`) = backlight on, low (`0x1A`) = off.
- **The CH422G does not use standard I2C ACK behaviour.** With the ESP-IDF I2C master driver, set `cfg.flags.disable_ack_check = true` on both CH422G device handles. Otherwise the driver may see a NACK and end the transaction before the output updates. Also log and check the return value of every init write.
- The backlight is on at power-up because DISP has a 10 kΩ pull-up to 3.3 V and the CH422G EXIO pins default to inputs. Once the CH422G is configured, writing EXIO2 low should turn it off.
- EXIO2/DISP is **not** connected to the CH343P USB data or reset path. EXIO5 only controls the native USB/CAN mux, not the CH343P port.
- Likely cause: the backlight driver and the CH343P share the 3.3 V supply, so the backlight's startup current may dip the 3.3 V or 5 V rail and make the CH343P drop and reconnect. Powering both USB ports at once may also upset the board's power-switching circuit.

Suggested tests (none done yet):

1. Add `cfg.flags.disable_ack_check = true` to both CH422G handles (`ch422g.cpp`).
2. Measure DISP/EXIO2: near 0 V when off, 3.3 V when on.
3. Test with only the UART USB port connected (native USB unplugged).
4. Use a stable 5 V supply; if possible scope the 3.3 V and VBUS rails while the backlight turns on.
5. Diagnostic: a 47-220 µF capacitor between 3.3 V and GND near the backlight driver.

They recommend finishing these before concluding whether it's the CH422G transaction or a supply dip. No hardware revision or schematic link between the backlight and the CH343P reset was offered.
