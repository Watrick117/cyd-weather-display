# CYD portable weather display

Target hardware: ESP32-2432S028R (2.8-inch 320x240 ILI9341 display with
XPT2046 resistive touch).

## Confirmed from the factory firmware

- ESP32 revision 3 with 4 MB flash
- ILI9341 display, 320x240, 40 MHz SPI
- Display SPI: MOSI 13, SCLK 14, CS 15
- XPT2046 touch controller
- CH340 USB serial on COM7
- Wi-Fi scanning works

## Implemented

1. Hardware check, touch input, and persistent 180-degree screen flip.
2. Fully on-device setup and settings: Wi-Fi scan/selection, a bottom-aligned
   LVGL touch keyboard with a permanent number row, Show/Hide password control,
   and latitude/longitude entry. No phone setup is required.
3. Open-Meteo current conditions, a 12-hour temperature graph with precipitation
   and storm highlighting, and a four-day forecast with condition icons.
4. Automatic town lookup from saved coordinates, manual refresh, location
   switching, offline reconnect, and a confirmation-protected option to erase
   saved Wi-Fi and GPS data.
5. A compact 320x240 dashboard with animated weather art, weekday forecasts,
   town, coordinates, last-update time, and a settings cog.

On a fresh device, the same Settings screen used by the dashboard cog is shown
first. The weather dashboard opens after both Wi-Fi and a location are saved.

The UI uses the MIT-licensed LVGL_CYD 1.2.2 board library and LVGL's standard
keyboard widget. The library draw buffer is reduced at build time for this
original ESP32 model without PSRAM.

Weather data is provided by [Open-Meteo](https://open-meteo.com/). The animated
weather-art direction was inspired by the MIT-licensed
[Meteocons](https://github.com/basmilius/weather-icons) project; the firmware
draws its compact icons locally with LVGL primitives for the CYD display.
