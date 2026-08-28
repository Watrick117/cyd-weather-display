#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LVGL_CYD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

namespace {
constexpr uint8_t MAX_NET = 5, MAX_LOC = 5;
constexpr uint32_t REFRESH_CHECK_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MS = REFRESH_CHECK_MS;
constexpr uint32_t WEATHER_WATCHDOG_MS = 35UL * 60UL * 1000UL;

struct Network { String ssid, password; };
struct Location { String name; float lat = 0, lon = 0; };
struct Hour { String time; float temp = NAN; int rain = 0, code = -1; };
struct Day { String date; float high = NAN, low = NAN; int rain = 0, code = -1; };

Preferences prefs;
Network nets[MAX_NET]; Location locs[MAX_LOC];
Hour hours[12]; Day days[4];
uint8_t netCount = 0, locCount = 0, activeLoc = 0;
bool flipped = false, weatherValid = false;
float nowTemp = NAN, feels = NAN, wind = NAN;
int humidity = -1, nowCode = -1;
String updated, statusLine = "Starting";
uint32_t lastFetch = 0, nextFetchAt = 0, nextRefreshCheckAt = 0, lastRetry = 0;
int64_t lastWeatherSlot = -1;

lv_obj_t *activeScreen = nullptr, *keyboard = nullptr, *textarea = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *passwordToggleLabel = nullptr;
lv_obj_t *demoRays[8] = {};
lv_point_precise_t demoRayPoints[8][2];
lv_point_precise_t hourGraphPoints[11][2];
lv_timer_t *demoTimer = nullptr;
float demoPhase = 0;
int demoCx = 160, demoCy = 103, demoInner = 26, demoOuter = 36;
bool passwordVisible = false;
enum class Field : uint8_t { WifiPassword, LocationName, Latitude, Longitude };
Field editField = Field::WifiPassword;
String editValue;
int selectedScan = -1, scanCount = 0;
int editingLocSlot = -1;
String scanSsids[MAX_NET]; int scanRssi[MAX_NET];
Location draftLoc;

static const char * const keyboardLowerMap[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const char * const keyboardUpperMap[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
  "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
  "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

#define KBC(value) static_cast<lv_buttonmatrix_ctrl_t>(value)
static const lv_buttonmatrix_ctrl_t keyboardTextCtrl[] = {
  KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
  KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1),
  KBC(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
  KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 1), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 1),
  KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1), KBC(1),
  KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 1), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 1), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 1),
  KBC(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2), KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 2), KBC(6),
  KBC(LV_BUTTONMATRIX_CTRL_CHECKED | 2), KBC(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2)
};
#undef KBC

String pkey(char group, uint8_t i, char field) { return String(group) + i + field; }

String weekdayFromDate(const String &isoDate) {
  int year = 0, month = 0, day = 0;
  if (sscanf(isoDate.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || month < 1 || month > 12) return "---";
  static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  static const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (month < 3) --year;
  int weekday = (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7;
  return names[weekday];
}

void showSetup(); void showWifiList(); void showWifiPassword(); void showLocation();
void showCurrent(); void showHourly(); void showDaily(); void showSettings();
void showEraseConfirmation();
void showIconDemo();
void iconDemoTick(lv_timer_t *); void iconDemoDelete(lv_event_t *);
bool connectWifi(); bool fetchWeather();
bool resolveLocationName(Location &loc);

void loadConfig() {
  prefs.begin("weather-ui", false);
  flipped = prefs.getBool("flip", false);
  netCount = min<uint8_t>(prefs.getUChar("wifi_n", 0), MAX_NET);
  locCount = min<uint8_t>(prefs.getUChar("loc_n", 0), MAX_LOC);
  activeLoc = prefs.getUChar("active_loc", 0);
  for (uint8_t i = 0; i < netCount; ++i) {
    nets[i].ssid = prefs.getString(pkey('w', i, 's').c_str(), "");
    nets[i].password = prefs.getString(pkey('w', i, 'p').c_str(), "");
  }
  for (uint8_t i = 0; i < locCount; ++i) {
    locs[i].name = prefs.getString(pkey('l', i, 'n').c_str(), "Location");
    locs[i].lat = prefs.getFloat(pkey('l', i, 'a').c_str(), 0);
    locs[i].lon = prefs.getFloat(pkey('l', i, 'o').c_str(), 0);
  }
  if (activeLoc >= locCount) activeLoc = 0;
}

void saveNetwork(const String &ssid, const String &password) {
  int slot = -1;
  for (int i = 0; i < netCount; ++i) if (nets[i].ssid == ssid) slot = i;
  if (slot < 0) slot = netCount < MAX_NET ? netCount++ : MAX_NET - 1;
  nets[slot] = {ssid, password};
  prefs.putString(pkey('w', slot, 's').c_str(), ssid);
  prefs.putString(pkey('w', slot, 'p').c_str(), password);
  prefs.putUChar("wifi_n", netCount);
}

void saveLocation(const Location &loc) {
  int slot = editingLocSlot >= 0 && editingLocSlot < locCount ? editingLocSlot : (locCount < MAX_LOC ? locCount++ : activeLoc);
  locs[slot] = loc; activeLoc = slot;
  prefs.putString(pkey('l', slot, 'n').c_str(), loc.name);
  prefs.putFloat(pkey('l', slot, 'a').c_str(), loc.lat);
  prefs.putFloat(pkey('l', slot, 'o').c_str(), loc.lon);
  prefs.putUChar("loc_n", locCount); prefs.putUChar("active_loc", activeLoc);
  editingLocSlot = -1;
}

lv_obj_t *newScreen(const String &title, bool showHeader = true) {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x173f56), 0);
  lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x4f9bad), 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  if (showHeader) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 320, 30); lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x173f56), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bar, 0, 0); lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = lv_label_create(bar); lv_label_set_text(label, title.c_str());
    lv_obj_set_style_text_color(label, lv_color_white(), 0); lv_obj_align(label, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *net = lv_label_create(bar);
    lv_label_set_text(net, WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(net, WiFi.status() == WL_CONNECTED ? lv_color_hex(0x43e078) : lv_color_hex(0xffa733), 0);
    lv_obj_align(net, LV_ALIGN_RIGHT_MID, -2, 0);
  }
  activeScreen = scr; return scr;
}

lv_obj_t *labelAt(lv_obj_t *parent, const String &value, int x, int y, lv_align_t align = LV_ALIGN_TOP_LEFT,
                  lv_color_t color = lv_color_white(), const lv_font_t *font = &lv_font_montserrat_14) {
  lv_obj_t *l = lv_label_create(parent); lv_label_set_text(l, value.c_str());
  lv_obj_set_style_text_color(l, color, 0); lv_obj_set_style_text_font(l, font, 0);
  lv_obj_align(l, align, x, y); return l;
}

String clockTime12(const String &value) {
  if (value.length() < 5 || value[2] != ':') return value;
  int hour = value.substring(0, 2).toInt();
  if (hour < 0 || hour > 23) return value;
  int displayHour = hour % 12;
  if (!displayHour) displayHour = 12;
  return String(displayHour) + value.substring(2, 5) + (hour < 12 ? " AM" : " PM");
}

lv_obj_t *buttonAt(lv_obj_t *parent, const String &caption, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *data = nullptr, uint32_t color = 0x167986) {
  lv_obj_t *b = lv_button_create(parent); lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0); lv_obj_set_style_radius(b, 6, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
  lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, caption.c_str()); lv_obj_center(l); return b;
}

void loadScreen(lv_obj_t *scr) { lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true); }

lv_obj_t *cardAt(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *card = lv_obj_create(parent); lv_obj_set_pos(card, x, y); lv_obj_set_size(card, w, h);
  lv_obj_set_style_radius(card, 9, 0); lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x173f56), 0); lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
  lv_obj_set_style_pad_all(card, 0, 0); lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE); return card;
}

lv_obj_t *weatherShape(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius = LV_RADIUS_CIRCLE) {
  lv_obj_t *shape = lv_obj_create(parent); lv_obj_remove_style_all(shape);
  lv_obj_set_pos(shape, x, y); lv_obj_set_size(shape, w, h);
  lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0); lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(shape, radius, 0); return shape;
}

void weatherArt(lv_obj_t *parent, int code, int x, int y) {
  if (code <= 3) weatherShape(parent, x + 4, y, 24, 24, 0xfbbf24);
  if (code == 0 || code == 1) return;
  uint32_t cloud = code <= 3 ? 0xe8f2f5 : 0xc9d7dd;
  weatherShape(parent, x + 7, y + 17, 47, 21, cloud, 10);
  weatherShape(parent, x + 13, y + 10, 23, 24, cloud);
  weatherShape(parent, x + 29, y + 6, 29, 30, cloud);
  if (code == 45 || code == 48) {
    weatherShape(parent, x + 8, y + 43, 44, 3, 0xdce9ed, 2);
    weatherShape(parent, x + 14, y + 49, 36, 3, 0xdce9ed, 2);
  } else if (code >= 71 && code <= 86) {
    for (int i = 0; i < 3; ++i) weatherShape(parent, x + 14 + i * 15, y + 44, 6, 6, 0xffffff);
  } else if (code >= 51 && code <= 82) {
    for (int i = 0; i < 3; ++i) weatherShape(parent, x + 15 + i * 14, y + 42, 3, 11, 0x55dbe6, 2);
  } else if (code >= 95) {
    weatherShape(parent, x + 27, y + 40, 6, 13, 0xffd43b, 2);
    weatherShape(parent, x + 32, y + 49, 6, 10, 0xffd43b, 2);
  }
}

void weatherArtSmall(lv_obj_t *parent, int code, int x, int y) {
  if (code <= 3) weatherShape(parent, x + 1, y + 1, 11, 11, 0xfbbf24);
  if (code == 0 || code == 1) return;
  uint32_t cloud = code <= 3 ? 0xe8f2f5 : 0xc9d7dd;
  weatherShape(parent, x + 2, y + 10, 22, 9, cloud, 5);
  weatherShape(parent, x + 6, y + 6, 11, 12, cloud);
  weatherShape(parent, x + 13, y + 4, 13, 14, cloud);
  if (code == 45 || code == 48) {
    weatherShape(parent, x + 4, y + 22, 20, 2, 0xdce9ed, 1);
  } else if (code >= 71 && code <= 86) {
    weatherShape(parent, x + 6, y + 22, 4, 4, 0xffffff);
    weatherShape(parent, x + 17, y + 22, 4, 4, 0xffffff);
  } else if (code >= 51 && code <= 82) {
    weatherShape(parent, x + 7, y + 21, 2, 7, 0x55dbe6, 1);
    weatherShape(parent, x + 17, y + 21, 2, 7, 0x55dbe6, 1);
  } else if (code >= 95) {
    weatherShape(parent, x + 12, y + 20, 4, 9, 0xffd43b, 1);
  }
}

void setupButton(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 1) {
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    statusLine = "Scanning Wi-Fi"; showWifiList();
  } else if (which == 2) {
    editingLocSlot = locCount ? activeLoc : -1;
    draftLoc = locCount ? locs[activeLoc] : Location(); showLocation();
  } else if (which == 3) {
    showCurrent(); if (connectWifi()) fetchWeather(); showCurrent();
  }
}

void showSetup() {
  showSettings();
}

void scanWifi() {
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  int found = WiFi.scanNetworks(false, true); scanCount = 0;
  for (int i = 0; i < found && scanCount < MAX_NET; ++i) {
    bool duplicate = false;
    for (int j = 0; j < scanCount; ++j) if (scanSsids[j] == WiFi.SSID(i)) duplicate = true;
    if (!duplicate && WiFi.SSID(i).length()) { scanSsids[scanCount] = WiFi.SSID(i); scanRssi[scanCount++] = WiFi.RSSI(i); }
  }
  WiFi.scanDelete();
}

void wifiChoice(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == -1) showSetup();
  else if (which == -2) showWifiList();
  else { selectedScan = which; editValue = ""; showWifiPassword(); }
}

void showWifiList() {
  lv_obj_t *busy = newScreen("Scanning Wi-Fi...");
  labelAt(busy, "Please wait", 0, 100, LV_ALIGN_TOP_MID, lv_color_hex(0x55dbe6), &lv_font_montserrat_18);
  loadScreen(busy); lv_timer_handler(); scanWifi();
  lv_obj_t *scr = newScreen("Choose Wi-Fi network");
  for (int i = 0; i < scanCount; ++i) {
    String row = scanSsids[i] + "   " + String(scanRssi[i]) + " dBm";
    buttonAt(scr, row, 10, 35 + i * 33, 300, 29, wifiChoice, reinterpret_cast<void *>(i), i % 2 ? 0x153557 : 0x34414b);
  }
  if (!scanCount) labelAt(scr, "No networks found", 0, 93, LV_ALIGN_TOP_MID, lv_color_hex(0xffa733), &lv_font_montserrat_18);
  buttonAt(scr, "BACK", 8, 207, 82, 28, wifiChoice, reinterpret_cast<void *>(-1), 0x34414b);
  buttonAt(scr, "RESCAN", 230, 207, 82, 28, wifiChoice, reinterpret_cast<void *>(-2), 0x34414b);
  loadScreen(scr);
}

void keyboardEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
  if (code == LV_EVENT_CANCEL) {
    if (editField == Field::WifiPassword) showWifiPassword(); else showLocation();
    return;
  }
  editValue = lv_textarea_get_text(textarea);
  if (editField == Field::LocationName) draftLoc.name = editValue;
  else if (editField == Field::Latitude) draftLoc.lat = editValue.toFloat();
  else if (editField == Field::Longitude) draftLoc.lon = editValue.toFloat();
  if (editField == Field::WifiPassword) showWifiPassword(); else showLocation();
}

void passwordVisibilityEvent(lv_event_t *) {
  passwordVisible = !passwordVisible;
  lv_textarea_set_password_mode(textarea, !passwordVisible);
  lv_label_set_text(passwordToggleLabel, passwordVisible ? "HIDE" : "SHOW");
}

void openKeyboard(Field field, const String &initial, const String &title, bool numeric, bool password) {
  editField = field; editValue = initial;
  lv_obj_t *scr = newScreen(title);
  passwordVisible = false;
  textarea = lv_textarea_create(scr); lv_obj_set_pos(textarea, 5, 34); lv_obj_set_size(textarea, password ? 248 : 310, 34);
  lv_textarea_set_one_line(textarea, true); lv_textarea_set_text(textarea, initial.c_str());
  lv_textarea_set_password_mode(textarea, password); lv_textarea_set_max_length(textarea, password ? 63 : 31);
  if (password) {
    lv_obj_t *toggle = lv_button_create(scr); lv_obj_set_pos(toggle, 257, 34); lv_obj_set_size(toggle, 58, 34);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(0x34414b), 0);
    lv_obj_add_event_cb(toggle, passwordVisibilityEvent, LV_EVENT_CLICKED, nullptr);
    passwordToggleLabel = lv_label_create(toggle); lv_label_set_text(passwordToggleLabel, "SHOW"); lv_obj_center(passwordToggleLabel);
  }
  keyboard = lv_keyboard_create(scr); lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(keyboard, 0, 70); lv_obj_set_size(keyboard, 320, 170);
  lv_obj_set_style_pad_all(keyboard, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_row(keyboard, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_column(keyboard, 2, LV_PART_MAIN);
  lv_keyboard_set_textarea(keyboard, textarea);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, keyboardLowerMap, keyboardTextCtrl);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, keyboardUpperMap, keyboardTextCtrl);
  lv_keyboard_set_mode(keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_CANCEL, nullptr);
  loadScreen(scr);
  lv_obj_update_layout(scr);
  Serial.printf("Keyboard viewport=%ldx%ld object=(%ld,%ld %ldx%ld)\n",
                (long)lv_display_get_horizontal_resolution(lv_display_get_default()),
                (long)lv_display_get_vertical_resolution(lv_display_get_default()),
                (long)lv_obj_get_x(keyboard), (long)lv_obj_get_y(keyboard),
                (long)lv_obj_get_width(keyboard), (long)lv_obj_get_height(keyboard));
}

void passwordButton(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 1) openKeyboard(Field::WifiPassword, editValue, "Wi-Fi password", false, true);
  else if (which == 2) {
    saveNetwork(scanSsids[selectedScan], editValue);
    if (connectWifi()) { if (locCount) { showCurrent(); fetchWeather(); showCurrent(); } else showSetup(); }
    else showWifiPassword();
  } else showWifiList();
}

void showWifiPassword() {
  lv_obj_t *scr = newScreen("Wi-Fi password");
  labelAt(scr, scanSsids[selectedScan], 0, 44, LV_ALIGN_TOP_MID, lv_color_hex(0x55dbe6), &lv_font_montserrat_18);
  String state = editValue.length() ? String(editValue.length()) + " characters entered" : "No password entered";
  labelAt(scr, state, 0, 77, LV_ALIGN_TOP_MID, lv_color_hex(0xbac7d4));
  buttonAt(scr, "ENTER / EDIT PASSWORD", 42, 107, 236, 38, passwordButton, reinterpret_cast<void *>(1));
  buttonAt(scr, "SAVE AND CONNECT", 42, 156, 236, 38, passwordButton, reinterpret_cast<void *>(2), 0x176b3a);
  buttonAt(scr, "BACK", 8, 207, 82, 28, passwordButton, reinterpret_cast<void *>(3), 0x34414b);
  if (statusLine.indexOf("failed") >= 0 || statusLine.indexOf("timed out") >= 0 || statusLine.indexOf("not found") >= 0)
    labelAt(scr, statusLine, 0, 198, LV_ALIGN_TOP_MID, lv_color_hex(0xffa733));
  loadScreen(scr);
}

void locationButton(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 1) openKeyboard(Field::LocationName, draftLoc.name, "Location name", false, false);
  else if (which == 2) openKeyboard(Field::Latitude, draftLoc.lat == 0 ? "" : String(draftLoc.lat, 5), "Latitude", true, false);
  else if (which == 3) openKeyboard(Field::Longitude, draftLoc.lon == 0 ? "" : String(draftLoc.lon, 5), "Longitude", true, false);
  else if (which == 4 && draftLoc.name.length()) {
    saveLocation(draftLoc);
    if (netCount) { showCurrent(); if (connectWifi()) fetchWeather(); showCurrent(); } else showSetup();
  } else showSetup();
}

void showLocation() {
  lv_obj_t *scr = newScreen("Manual location");
  labelAt(scr, "Name", 10, 42, LV_ALIGN_TOP_LEFT, lv_color_hex(0xbac7d4));
  buttonAt(scr, draftLoc.name.length() ? draftLoc.name : "tap to enter", 110, 34, 202, 31, locationButton, reinterpret_cast<void *>(1), 0x34414b);
  labelAt(scr, "Latitude", 10, 82, LV_ALIGN_TOP_LEFT, lv_color_hex(0xbac7d4));
  buttonAt(scr, draftLoc.lat == 0 ? "tap to enter" : String(draftLoc.lat, 5), 110, 74, 202, 31, locationButton, reinterpret_cast<void *>(2), 0x34414b);
  labelAt(scr, "Longitude", 10, 122, LV_ALIGN_TOP_LEFT, lv_color_hex(0xbac7d4));
  buttonAt(scr, draftLoc.lon == 0 ? "tap to enter" : String(draftLoc.lon, 5), 110, 114, 202, 31, locationButton, reinterpret_cast<void *>(3), 0x34414b);
  labelAt(scr, "Decimal coordinates: 40.7128 / -74.0060", 0, 155, LV_ALIGN_TOP_MID, lv_color_hex(0x55dbe6));
  buttonAt(scr, "SAVE LOCATION", 84, 179, 152, 34, locationButton, reinterpret_cast<void *>(4), 0x176b3a);
  buttonAt(scr, "BACK", 8, 207, 65, 28, locationButton, reinterpret_cast<void *>(5), 0x34414b);
  loadScreen(scr);
}

const char *condition(int code) {
  if (code == 0) return "Clear"; if (code <= 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog"; if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain"; if (code <= 77) return "Snow";
  if (code <= 82) return "Showers"; if (code <= 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm"; return "Unknown";
}

lv_color_t conditionColor(int code) {
  if (code == 0) return lv_color_hex(0xffd43b); if (code <= 3) return lv_color_hex(0xd6e1e8);
  return code >= 51 ? lv_color_hex(0x55dbe6) : lv_color_hex(0x8996a0);
}

void navEvent(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 0) showCurrent(); else if (which == 1) showHourly(); else if (which == 2) showDaily(); else showSettings();
}

void addNav(lv_obj_t *scr, int selected) {
  const char *names[] = {"NOW", "12 HR", "4 DAY", "SETTINGS"};
  for (int i = 0; i < 4; ++i) buttonAt(scr, names[i], i * 80, 210, 79, 30, navEvent,
    reinterpret_cast<void *>(i), i == selected ? 0x167986 : 0x34414b);
}

void showCurrent() {
  lv_obj_t *scr = newScreen(locCount ? locs[activeLoc].name : "Weather", false);
  if (!weatherValid) {
    statusLabel = labelAt(scr, statusLine, 0, 78, LV_ALIGN_TOP_MID, lv_color_hex(0xffa733), &lv_font_montserrat_18);
    labelAt(scr, "Weather data not available yet", 0, 116, LV_ALIGN_TOP_MID, lv_color_hex(0xbac7d4));
    buttonAt(scr, "Settings", 110, 156, 100, 32, navEvent, reinterpret_cast<void *>(3), 0x315f72);
  } else {
    lv_obj_t *townLabel = labelAt(scr, locs[activeLoc].name, 4, 1, LV_ALIGN_TOP_LEFT,
                                  lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    lv_obj_set_width(townLabel, 74); lv_label_set_long_mode(townLabel, LV_LABEL_LONG_DOT);
    lv_obj_t *updatedLabel = labelAt(scr, "Last Updated " + updated, 80, 1, LV_ALIGN_TOP_LEFT,
                                     lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    lv_obj_set_width(updatedLabel, 110); lv_obj_set_style_text_align(updatedLabel, LV_TEXT_ALIGN_CENTER, 0);
    String coordinates = String(locs[activeLoc].lat, 4) + ", " + String(locs[activeLoc].lon, 4);
    lv_obj_t *gpsLabel = labelAt(scr, coordinates, 192, 1, LV_ALIGN_TOP_LEFT,
                                 lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    lv_obj_set_width(gpsLabel, 101); lv_obj_set_style_text_align(gpsLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *settingsCog = lv_button_create(scr); lv_obj_set_pos(settingsCog, 296, 0); lv_obj_set_size(settingsCog, 24, 14);
    lv_obj_set_style_bg_opa(settingsCog, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(settingsCog, 0, 0);
    lv_obj_set_style_border_width(settingsCog, 0, 0); lv_obj_set_style_pad_all(settingsCog, 0, 0);
    lv_obj_add_event_cb(settingsCog, navEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(3));
    lv_obj_t *cogLabel = lv_label_create(settingsCog); lv_label_set_text(cogLabel, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(cogLabel, lv_color_white(), 0); lv_obj_center(cogLabel);
    lv_obj_t *nowCard = cardAt(scr, 4, 15, 174, 67);
    lv_obj_t *detailCard = cardAt(scr, 182, 15, 134, 67);
    lv_obj_t *hourCard = cardAt(scr, 4, 86, 312, 82);
    lv_obj_t *dayCard = cardAt(scr, 4, 172, 312, 64);

    labelAt(nowCard, String(nowTemp, 0) + "°", 68, 2, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_t *conditionLabel = labelAt(nowCard, condition(nowCode), 69, 36, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_12);
    lv_obj_set_width(conditionLabel, 92); lv_label_set_long_mode(conditionLabel, LV_LABEL_LONG_DOT);
    labelAt(nowCard, "Feels like " + String(feels, 0) + "°", 69, 52, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);

    if (nowCode == 0 || nowCode == 1) {
      lv_obj_add_event_cb(scr, iconDemoDelete, LV_EVENT_DELETE, nullptr);
      for (int i = 0; i < 8; ++i) {
        demoRays[i] = lv_line_create(scr);
        lv_obj_set_style_line_width(demoRays[i], 3, 0);
        lv_obj_set_style_line_rounded(demoRays[i], true, 0);
        lv_obj_set_style_line_color(demoRays[i], lv_color_hex(0xfbbf24), 0);
      }
      lv_obj_t *core = lv_obj_create(scr);
      lv_obj_set_size(core, 27, 27); lv_obj_align(core, LV_ALIGN_TOP_LEFT, 18, 35);
      lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(core, lv_color_hex(0xfbbf24), 0);
      lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(core, lv_color_hex(0xf59e0b), 0);
      lv_obj_set_style_border_width(core, 1, 0); lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);
      demoCx = 31; demoCy = 48; demoInner = 20; demoOuter = 28;
      demoPhase = 0; iconDemoTick(nullptr); demoTimer = lv_timer_create(iconDemoTick, 50, nullptr);
    } else {
      weatherArt(scr, nowCode, 5, 23);
    }

    labelAt(detailCard, "High / Low", 8, 5, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    labelAt(detailCard, String(days[0].high, 0) + "°  " + String(days[0].low, 0) + "°", 73, 3, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_12);
    labelAt(detailCard, "Humidity", 8, 25, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    labelAt(detailCard, String(humidity) + "%", 73, 23, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_12);
    labelAt(detailCard, "Wind", 8, 45, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    labelAt(detailCard, String(wind, 0) + " mph", 48, 43, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_12);

    float minTemp = hours[0].temp, maxTemp = hours[0].temp;
    for (int i = 0; i < 12; ++i) {
      minTemp = min(minTemp, hours[i].temp); maxTemp = max(maxTemp, hours[i].temp);
    }
    labelAt(hourCard, String(minTemp, 0) + "° low", 7, 3, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_10);
    labelAt(hourCard, "Next 12 hours", 0, 3, LV_ALIGN_TOP_MID, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
    labelAt(hourCard, String(maxTemp, 0) + "° high", -7, 3, LV_ALIGN_TOP_RIGHT, lv_color_white(), &lv_font_montserrat_10);
    float span = max(1.0f, maxTemp - minTemp);
    for (int i = 0; i < 12; ++i) {
      int x = 8 + i * 27;
      int y = 52 - (int)(((hours[i].temp - minTemp) / span) * 29.0f);
      int rainHeight = min(13, max(1, hours[i].rain * 13 / 100));
      uint32_t precipitationColor = hours[i].code >= 95 ? 0xffd43b : 0x55dbe6;
      lv_obj_t *rainBar = weatherShape(hourCard, x - 2, 58 - rainHeight, 5, rainHeight, precipitationColor, 2);
      lv_obj_set_style_bg_opa(rainBar, hours[i].rain ? LV_OPA_70 : LV_OPA_20, 0);
      weatherShape(hourCard, x - 2, y - 2, 5, 5, 0xffffff);
      if (i < 11) {
        int nextY = 52 - (int)(((hours[i + 1].temp - minTemp) / span) * 29.0f);
        hourGraphPoints[i][0] = {(lv_value_precise_t)x, (lv_value_precise_t)y};
        hourGraphPoints[i][1] = {(lv_value_precise_t)(x + 27), (lv_value_precise_t)nextY};
        lv_obj_t *segment = lv_line_create(hourCard); lv_line_set_points(segment, hourGraphPoints[i], 2);
        lv_obj_set_style_line_width(segment, 2, 0); lv_obj_set_style_line_color(segment, lv_color_white(), 0);
      }
      if (i == 0 || i == 3 || i == 6 || i == 9 || i == 11) {
        String hour = clockTime12(hours[i].time);
        int labelX = max(1, min(276, x - (int)hour.length() * 3));
        labelAt(hourCard, hour, labelX, 64, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
      }
    }

    for (int i = 0; i < 4; ++i) {
      int x = i * 77 + 3;
      weatherArtSmall(dayCard, days[i].code, x, 20);
      labelAt(dayCard, days[i].date, x + 29, 8, LV_ALIGN_TOP_LEFT, lv_color_hex(0xdce9ed), &lv_font_montserrat_10);
      labelAt(dayCard, String(days[i].high, 0) + "/" + String(days[i].low, 0) + "°", x + 29, 24, LV_ALIGN_TOP_LEFT, lv_color_white(), &lv_font_montserrat_12);
      labelAt(dayCard, String(LV_SYMBOL_TINT) + " " + String(days[i].rain) + "%", x + 29, 39, LV_ALIGN_TOP_LEFT, lv_color_hex(0x55dbe6));
    }
  }
  loadScreen(scr);
}

void showHourly() {
  lv_obj_t *scr = newScreen("Next 12 hours");
  for (int i = 0; i < 12; ++i) {
    int col = i % 2, row = i / 2, x = col * 160, y = 34 + row * 28;
    labelAt(scr, clockTime12(hours[i].time), x + 5, y, LV_ALIGN_TOP_LEFT, lv_color_hex(0x55dbe6));
    labelAt(scr, String(hours[i].temp, 0) + "F", x + 72, y, LV_ALIGN_TOP_LEFT);
    labelAt(scr, String(hours[i].rain) + "%", x + 122, y, LV_ALIGN_TOP_LEFT,
            hours[i].rain >= 40 ? lv_color_hex(0x55dbe6) : lv_color_hex(0xbac7d4));
  }
  loadScreen(scr);
}

void showDaily() {
  lv_obj_t *scr = newScreen("Four-day forecast");
  for (int i = 0; i < 4; ++i) {
    int y = 37 + i * 41;
    labelAt(scr, days[i].date, 8, y, LV_ALIGN_TOP_LEFT);
    labelAt(scr, condition(days[i].code), 82, y, LV_ALIGN_TOP_LEFT, conditionColor(days[i].code));
    labelAt(scr, String(days[i].high, 0) + "/" + String(days[i].low, 0) + "F", 220, y, LV_ALIGN_TOP_LEFT);
    labelAt(scr, String(days[i].rain) + "%", 279, y, LV_ALIGN_TOP_LEFT, lv_color_hex(0x55dbe6));
  }
  loadScreen(scr);
}

void settingsEvent(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 1) showWifiList();
  else if (which == 2) {
    editingLocSlot = locCount ? activeLoc : -1;
    draftLoc = locCount ? locs[activeLoc] : Location(); showLocation();
  }
  else if (which == 3) {
    flipped = !flipped; prefs.putBool("flip", flipped);
    lv_display_set_rotation(lv_display_get_default(), flipped ? USB_RIGHT : USB_LEFT); showSettings();
  } else if (which == 4) { if (WiFi.status() != WL_CONNECTED) connectWifi(); fetchWeather(); showCurrent(); }
  else if (which == 5 && locCount > 1) {
    activeLoc = (activeLoc + 1) % locCount; prefs.putUChar("active_loc", activeLoc); fetchWeather(); showCurrent();
  } else if (which == 6) showEraseConfirmation();
  else if (which == 7) showCurrent();
}

void eraseDataEvent(lv_event_t *e) {
  intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
  if (which == 0) { showSettings(); return; }

  for (uint8_t i = 0; i < MAX_NET; ++i) {
    prefs.remove(pkey('w', i, 's').c_str());
    prefs.remove(pkey('w', i, 'p').c_str());
    nets[i] = Network();
  }
  for (uint8_t i = 0; i < MAX_LOC; ++i) {
    prefs.remove(pkey('l', i, 'n').c_str());
    prefs.remove(pkey('l', i, 'a').c_str());
    prefs.remove(pkey('l', i, 'o').c_str());
    locs[i] = Location();
  }
  prefs.remove("wifi_n"); prefs.remove("loc_n"); prefs.remove("active_loc");
  netCount = 0; locCount = 0; activeLoc = 0; weatherValid = false;
  updated = ""; statusLine = "User data erased";
  WiFi.disconnect(true, true);
  Serial.println("Saved Wi-Fi and GPS data erased");
  showSettings();
}

void showEraseConfirmation() {
  lv_obj_t *scr = newScreen("Erase user data?");
  labelAt(scr, "ERASE WI-FI AND GPS?", 0, 54, LV_ALIGN_TOP_MID, lv_color_hex(0xff8a80), &lv_font_montserrat_18);
  labelAt(scr, "Saved password, town and coordinates", 0, 91, LV_ALIGN_TOP_MID, lv_color_hex(0xdce9ed), &lv_font_montserrat_12);
  labelAt(scr, "will be permanently removed.", 0, 110, LV_ALIGN_TOP_MID, lv_color_hex(0xdce9ed), &lv_font_montserrat_12);
  buttonAt(scr, "CANCEL", 30, 155, 120, 40, eraseDataEvent, reinterpret_cast<void *>(0), 0x34414b);
  buttonAt(scr, "ERASE", 170, 155, 120, 40, eraseDataEvent, reinterpret_cast<void *>(1), 0xa83232);
  loadScreen(scr);
}

void iconDemoTick(lv_timer_t *) {
  constexpr float PI_F = 3.14159265f;
  demoPhase += 0.035f;
  if (demoPhase >= 2 * PI_F) demoPhase -= 2 * PI_F;
  for (int i = 0; i < 8; ++i) {
    float angle = demoPhase + i * PI_F / 4.0f;
    demoRayPoints[i][0] = {(lv_value_precise_t)(demoCx + cosf(angle) * demoInner), (lv_value_precise_t)(demoCy + sinf(angle) * demoInner)};
    demoRayPoints[i][1] = {(lv_value_precise_t)(demoCx + cosf(angle) * demoOuter), (lv_value_precise_t)(demoCy + sinf(angle) * demoOuter)};
    lv_line_set_points_mutable(demoRays[i], demoRayPoints[i], 2);
    lv_obj_invalidate(demoRays[i]);
  }
}

void iconDemoDelete(lv_event_t *) {
  if (demoTimer) { lv_timer_delete(demoTimer); demoTimer = nullptr; }
  for (auto &ray : demoRays) ray = nullptr;
}

void iconDemoBack(lv_event_t *) { showSettings(); }

void showIconDemo() {
  lv_obj_t *scr = newScreen("Meteocons animation test");
  lv_obj_add_event_cb(scr, iconDemoDelete, LV_EVENT_DELETE, nullptr);
  for (int i = 0; i < 8; ++i) {
    demoRays[i] = lv_line_create(scr);
    lv_obj_set_style_line_width(demoRays[i], 4, 0);
    lv_obj_set_style_line_rounded(demoRays[i], true, 0);
    lv_obj_set_style_line_color(demoRays[i], lv_color_hex(0xfbbf24), 0);
  }
  lv_obj_t *core = lv_obj_create(scr);
  lv_obj_set_size(core, 35, 35); lv_obj_align(core, LV_ALIGN_TOP_MID, 0, 85);
  lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(core, lv_color_hex(0xfbbf24), 0);
  lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(core, lv_color_hex(0xf59e0b), 0);
  lv_obj_set_style_border_width(core, 2, 0);
  lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);
  labelAt(scr, "Meteocons clear-day", 0, 145, LV_ALIGN_TOP_MID, lv_color_white(), &lv_font_montserrat_18);
  labelAt(scr, "Native LVGL playback - stored on device", 0, 170, LV_ALIGN_TOP_MID, lv_color_hex(0xbac7d4));
  buttonAt(scr, "BACK", 120, 199, 80, 32, iconDemoBack, nullptr, 0x34414b);
  demoCx = 160; demoCy = 103; demoInner = 26; demoOuter = 36;
  demoPhase = 0; iconDemoTick(nullptr);
  demoTimer = lv_timer_create(iconDemoTick, 50, nullptr);
  loadScreen(scr);
}

void showSettings() {
  lv_obj_t *scr = newScreen("Settings");
  String wifiTitle = netCount ? "WI-FI   " + nets[0].ssid : "SET UP WI-FI";
  buttonAt(scr, wifiTitle, 20, 34, 280, 29, settingsEvent, reinterpret_cast<void *>(1), netCount ? 0x176b3a : 0x167986);
  String locationTitle = locCount ? "LOCATION   " + locs[activeLoc].name : "ENTER LOCATION";
  buttonAt(scr, locationTitle, 20, 67, 280, 29, settingsEvent, reinterpret_cast<void *>(2));
  if (locCount) {
    String coordinates = String(locs[activeLoc].lat, 4) + ", " + String(locs[activeLoc].lon, 4) + "  (saved together)";
    labelAt(scr, coordinates, 0, 97, LV_ALIGN_TOP_MID, lv_color_hex(0xb8dce3), &lv_font_montserrat_10);
  }
  buttonAt(scr, "FLIP SCREEN 180", 20, 111, 280, 29, settingsEvent, reinterpret_cast<void *>(3));
  if (netCount && locCount) {
    buttonAt(scr, "REFRESH", 20, 144, 130, 29, settingsEvent, reinterpret_cast<void *>(4), 0x176b3a);
    if (locCount > 1) buttonAt(scr, "NEXT LOCATION", 170, 144, 130, 29, settingsEvent, reinterpret_cast<void *>(5), 0x176b3a);
  } else {
    labelAt(scr, "Complete Wi-Fi and location above", 0, 150, LV_ALIGN_TOP_MID, lv_color_hex(0xffd43b), &lv_font_montserrat_12);
  }
  buttonAt(scr, "ERASE WI-FI & GPS", 20, 178, 280, 25, settingsEvent, reinterpret_cast<void *>(6), 0xa83232);
  if (weatherValid) buttonAt(scr, "BACK", 120, 208, 80, 28, settingsEvent, reinterpret_cast<void *>(7), 0x34414b);
  loadScreen(scr);
}

bool connectWifi() {
  if (!netCount) return false;
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  for (int i = 0; i < netCount; ++i) {
    statusLine = "Connecting to " + nets[i].ssid; Serial.println(statusLine);
    WiFi.disconnect(true, false); delay(250); WiFi.mode(WIFI_STA);
    WiFi.begin(nets[i].ssid.c_str(), nets[i].password.c_str());
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) { lv_timer_handler(); delay(20); }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      statusLine = "Connected";
      Serial.println("Wi-Fi: " + WiFi.localIP().toString());
      return true;
    }
    wl_status_t result = WiFi.status();
    Serial.printf("Wi-Fi failed: SSID='%s', password length=%u, status=%d\n",
                  nets[i].ssid.c_str(), nets[i].password.length(), (int)result);
    if (result == WL_NO_SSID_AVAIL) statusLine = "Network not found (2.4 GHz required)";
    else if (result == WL_CONNECT_FAILED) statusLine = "Authentication failed - check password";
    else statusLine = "Connection timed out - check password";
    WiFi.disconnect();
  }
  return false;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED || !locCount) return false;
  Location &l = locs[activeLoc]; statusLine = "Updating weather";
  resolveLocationName(l);
  Serial.printf("Weather request: location='%s', lat=%.5f, lon=%.5f, free heap=%u\n",
                l.name.c_str(), l.lat, l.lon, ESP.getFreeHeap());
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(l.lat, 5) + "&longitude=" + String(l.lon, 5) +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
    "&hourly=temperature_2m,precipitation_probability,weather_code"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
    "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=4";
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(15000);
  if (!http.begin(client, url)) { statusLine = "Weather connection failed"; Serial.println(statusLine); return false; }
  int result = http.GET();
  if (result != HTTP_CODE_OK) {
    statusLine = "Weather error " + String(result);
    Serial.println(statusLine + ": " + http.errorToString(result));
    if (result > 0) Serial.println("Weather response: " + http.getString().substring(0, 300));
    http.end(); return false;
  }
  // Buffering the response is more reliable than parsing HTTPClient's TLS stream
  // directly on this ESP32 core; the Open-Meteo payload is small enough for heap.
  String payload = http.getString();
  http.end();
  Serial.printf("Weather response: %u bytes\n", payload.length());
  JsonDocument doc; DeserializationError err = deserializeJson(doc, payload);
  if (err) { statusLine = "Weather data error"; Serial.println(statusLine + ": " + err.c_str()); return false; }
  nowTemp = doc["current"]["temperature_2m"] | NAN; feels = doc["current"]["apparent_temperature"] | NAN;
  wind = doc["current"]["wind_speed_10m"] | NAN; humidity = doc["current"]["relative_humidity_2m"] | -1;
  nowCode = doc["current"]["weather_code"] | -1; updated = String((const char *)(doc["current"]["time"] | ""));
  if (updated.length() >= 16) updated = updated.substring(11, 16);
  JsonArray times = doc["hourly"]["time"]; int start = 0;
  String fullNow = String((const char *)(doc["current"]["time"] | "")); String hourNow = fullNow.substring(0, 13) + ":00";
  for (int i = 0; i < (int)times.size(); ++i) if (String((const char *)times[i]) >= hourNow) { start = i; break; }
  for (int i = 0; i < 12; ++i) {
    int n = start + i; String ts = String((const char *)(doc["hourly"]["time"][n] | ""));
    hours[i].time = ts.length() >= 16 ? ts.substring(11, 16) : "--";
    hours[i].temp = doc["hourly"]["temperature_2m"][n] | NAN;
    hours[i].rain = doc["hourly"]["precipitation_probability"][n] | 0;
    hours[i].code = doc["hourly"]["weather_code"][n] | -1;
  }
  for (int i = 0; i < 4; ++i) {
    String forecastDate = String((const char *)(doc["daily"]["time"][i] | "---"));
    days[i].date = weekdayFromDate(forecastDate);
    days[i].high = doc["daily"]["temperature_2m_max"][i] | NAN; days[i].low = doc["daily"]["temperature_2m_min"][i] | NAN;
    days[i].rain = doc["daily"]["precipitation_probability_max"][i] | 0; days[i].code = doc["daily"]["weather_code"][i] | -1;
  }
  weatherValid = true;
  lastFetch = millis();
  uint32_t minutesUntilRefresh = 30;
  if (updated.length() == 5 && updated[2] == ':') {
    int minute = updated.substring(3, 5).toInt();
    if (minute >= 0 && minute < 60)
      minutesUntilRefresh = minute < 30 ? 30 - minute : 60 - minute;
  }
  nextFetchAt = lastFetch + minutesUntilRefresh * 60UL * 1000UL;
  time_t clockNow = time(nullptr);
  if (clockNow >= 1700000000) {
    lastWeatherSlot = (int64_t)clockNow / 1800;
    struct tm utcNow;
    gmtime_r(&clockNow, &utcNow);
    Serial.printf("NTP time UTC: %04d-%02d-%02d %02d:%02d:%02d; half-hour slot=%lld\n",
                  utcNow.tm_year + 1900, utcNow.tm_mon + 1, utcNow.tm_mday,
                  utcNow.tm_hour, utcNow.tm_min, utcNow.tm_sec,
                  (long long)lastWeatherSlot);
  }
  statusLine = "Updated";
  Serial.printf("Weather updated: %s; next half-hour refresh in %lu minute(s)\n",
                l.name.c_str(), (unsigned long)minutesUntilRefresh);
  return true;
}

bool resolveLocationName(Location &loc) {
  String current = loc.name; current.trim();
  if (current.length() && !current.equalsIgnoreCase("HOME") &&
      !current.equalsIgnoreCase("Location") && !current.equalsIgnoreCase("Locating...")) return true;

  String url = "https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=" + String(loc.lat, 5) +
               "&longitude=" + String(loc.lon, 5) + "&localityLanguage=en";
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setTimeout(12000);
  if (!http.begin(client, url)) { Serial.println("Town lookup connection failed"); return false; }
  int result = http.GET();
  if (result != HTTP_CODE_OK) {
    Serial.printf("Town lookup failed: HTTP %d\n", result); http.end(); return false;
  }
  String payload = http.getString(); http.end();
  JsonDocument doc; DeserializationError err = deserializeJson(doc, payload);
  if (err) { Serial.println("Town lookup data error: " + String(err.c_str())); return false; }

  String town = String((const char *)(doc["city"] | ""));
  if (!town.length()) town = String((const char *)(doc["locality"] | ""));
  if (!town.length()) return false;
  town.trim(); loc.name = town;
  prefs.putString(pkey('l', activeLoc, 'n').c_str(), town);
  Serial.println("Town resolved from coordinates: " + town);
  return true;
}
}  // namespace

void setup() {
  Serial.begin(115200); delay(250); loadConfig();
  LVGL_CYD::begin(flipped ? USB_RIGHT : USB_LEFT);
  Serial.printf("LVGL viewport=%ldx%ld rotation=%d\n",
                (long)lv_display_get_horizontal_resolution(lv_display_get_default()),
                (long)lv_display_get_vertical_resolution(lv_display_get_default()),
                (int)lv_display_get_rotation(lv_display_get_default()));
  Serial.println("CYD Weather with LVGL keyboard");
  if (!netCount || !locCount) showSetup();
  else { showCurrent(); if (connectWifi()) fetchWeather(); showCurrent(); }
}

void loop() {
  lv_timer_handler();
  bool dashboard = activeScreen && (weatherValid || (netCount && locCount));
  uint32_t now = millis();
  bool refreshCheckDue = !nextRefreshCheckAt || (int32_t)(now - nextRefreshCheckAt) >= 0;
  time_t clockNow = time(nullptr);
  bool clockValid = clockNow >= 1700000000;
  int64_t currentWeatherSlot = clockValid ? (int64_t)clockNow / 1800 : -1;
  bool clockSlotDue = refreshCheckDue && clockValid && currentWeatherSlot != lastWeatherSlot;
  bool scheduledDue = refreshCheckDue && !clockValid && nextFetchAt && (int32_t)(now - nextFetchAt) >= 0;
  bool initialRetryDue = refreshCheckDue && !clockValid && !nextFetchAt && now - lastFetch >= WEATHER_RETRY_MS;
  bool watchdogDue = refreshCheckDue && weatherValid && now - lastFetch >= WEATHER_WATCHDOG_MS;
  if (refreshCheckDue) {
    nextRefreshCheckAt = now + REFRESH_CHECK_MS;
    if (clockValid) {
      struct tm utcNow;
      gmtime_r(&clockNow, &utcNow);
      Serial.printf("Refresh check UTC %02d:%02d:%02d; current slot=%lld, last successful slot=%lld\n",
                    utcNow.tm_hour, utcNow.tm_min, utcNow.tm_sec,
                    (long long)currentWeatherSlot, (long long)lastWeatherSlot);
    } else {
      Serial.println("Refresh check: waiting for NTP; using elapsed-time fallback");
    }
  }
  if (dashboard && WiFi.status() == WL_CONNECTED &&
      (clockSlotDue || scheduledDue || initialRetryDue || watchdogDue)) {
    // Set the retry before starting the request so any failure cannot leave an
    // expired half-hour deadline that hammers the weather service every loop.
    nextFetchAt = now + WEATHER_RETRY_MS;
    if (clockSlotDue) Serial.println("New NTP half-hour period detected");
    else Serial.println(watchdogDue ? "Weather refresh watchdog triggered" : "Elapsed-time fallback refresh");
    if (!fetchWeather()) Serial.println("Weather refresh failed; retrying in three minutes");
    showCurrent();
  }
  if (dashboard && WiFi.status() != WL_CONNECTED && now - lastRetry > 30000) {
    lastRetry = now;
    if (connectWifi()) {
      nextFetchAt = millis() + WEATHER_RETRY_MS;
      if (!fetchWeather()) Serial.println("Weather refresh after reconnect failed; retrying in three minutes");
      showCurrent();
    }
  }
  delay(5);
}
