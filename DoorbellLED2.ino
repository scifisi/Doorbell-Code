#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

// --------------------------
// Hardware configuration
// --------------------------
constexpr uint8_t LED_PIN = 14;
constexpr uint16_t PHYSICAL_NUM_LEDS = 25;
constexpr uint16_t NUM_LEDS = 24; // visible pixels only (physical LEDs 2..25)
constexpr uint16_t PIXEL_OFFSET = 1; // skip hidden physical LED #1

constexpr uint8_t BUTTON_PIN = 18;         // momentary doorbell button
constexpr bool BUTTON_ACTIVE_LOW = true;   // true: button to GND with INPUT_PULLUP
constexpr uint8_t RADAR_PRESENCE_PIN = 19; // LD2410C OUT pin
constexpr bool RADAR_ACTIVE_HIGH = true;
constexpr bool RADAR_USE_PULLDOWN = true;

// Doorbell relay/chime output
constexpr int8_t DOORBELL_OUT_PIN = 23;
constexpr bool DOORBELL_RELAY_ACTIVE_HIGH = true;

// Wi-Fi credentials
const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// Location (UK)
constexpr float LATITUDE = 53.82f;
constexpr float LONGITUDE = -3.05f;

// Defaults for tunable settings
constexpr uint8_t DEFAULT_PRESENCE_BRIGHTNESS = 120;
constexpr uint8_t DEFAULT_RAINBOW_SPEED = 2;
constexpr uint32_t DEFAULT_PRESENCE_TIMEOUT_MS = 15000;
constexpr uint32_t DEFAULT_DOORBELL_RELAY_PULSE_MS = 1000;

// Timing
constexpr uint32_t IDLE_ANIM_STEP_MS = 25;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t BUTTON_FLASH_MS = 700;
constexpr uint32_t SUN_SCHEDULE_REFRESH_MS = 600000; // 10 min

// Runtime-tunable settings (via web UI + persisted in NVS)
enum NightMode : uint8_t { NIGHT_AUTO = 0, NIGHT_FORCE_DAY = 1, NIGHT_FORCE_NIGHT = 2 };
enum PresencePaletteMode : uint8_t { PAL_RAINBOW = 0, PAL_OCEAN = 1, PAL_LAVA = 2, PAL_FOREST = 3, PAL_HEAT = 4, PAL_PARTY = 5 };

uint8_t presenceBrightness = DEFAULT_PRESENCE_BRIGHTNESS;
uint8_t rainbowSpeed = DEFAULT_RAINBOW_SPEED;
uint32_t presenceTimeoutMs = DEFAULT_PRESENCE_TIMEOUT_MS;
uint32_t doorbellRelayPulseMs = DEFAULT_DOORBELL_RELAY_PULSE_MS;
NightMode nightMode = NIGHT_AUTO;
PresencePaletteMode presencePaletteMode = PAL_RAINBOW;
bool allowDaytimePresenceAnimation = false;

CRGB leds[PHYSICAL_NUM_LEDS];
CRGB glowPixels[NUM_LEDS];
WebServer server(80);
Preferences prefs;

bool isNight = false;
bool timeValid = false;
bool presenceActive = false;
uint32_t lastPresenceMs = 0;
uint32_t lastIdleFrameMs = 0;
uint32_t lastScheduleCalcMs = 0;

// Doorbell state
bool lastButtonRead = HIGH;
bool debouncedButton = HIGH;
uint32_t lastDebounceMs = 0;
bool doorbellPulseActive = false;
uint32_t doorbellPulseStartMs = 0;
uint32_t buttonFlashUntilMs = 0;

// Animation state
uint8_t hueOffset = 0;

// Schedule state
int sunriseMinutesLocal = 7 * 60;
int sunsetMinutesLocal = 18 * 60;
int currentMinutesLocal = -1;

int clampMinutes(int m) {
  if (m < 0) return 0;
  if (m > 1439) return 1439;
  return m;
}

int dayOfYear(const tm& t) {
  return t.tm_yday + 1;
}

int calculateSunEventMinutesLocal(int doy, float latitude, float longitude, int tzHours, bool sunrise) {
  const float gamma = 2.0f * PI / 365.0f * (doy - 1);
  const float eqtime = 229.18f * (0.000075f + 0.001868f * cosf(gamma) - 0.032077f * sinf(gamma) -
                                  0.014615f * cosf(2 * gamma) - 0.040849f * sinf(2 * gamma));
  const float decl = 0.006918f - 0.399912f * cosf(gamma) + 0.070257f * sinf(gamma) -
                     0.006758f * cosf(2 * gamma) + 0.000907f * sinf(2 * gamma) -
                     0.002697f * cosf(3 * gamma) + 0.00148f * sinf(3 * gamma);

  const float latRad = latitude * PI / 180.0f;
  const float zenith = 90.833f * PI / 180.0f;

  float cosH = (cosf(zenith) - sinf(latRad) * sinf(decl)) / (cosf(latRad) * cosf(decl));
  if (cosH > 1.0f) return sunrise ? 9 * 60 : 15 * 60;   // polar night fallback
  if (cosH < -1.0f) return sunrise ? 0 : 23 * 60 + 59;  // midnight sun fallback

  const float hourAngle = acosf(cosH);
  const float hourAngleDeg = hourAngle * 180.0f / PI;

  const float solarNoon = 720.0f - 4.0f * longitude - eqtime + (tzHours * 60.0f);
  float minutes = sunrise ? (solarNoon - hourAngleDeg * 4.0f) : (solarNoon + hourAngleDeg * 4.0f);

  while (minutes < 0) minutes += 1440.0f;
  while (minutes >= 1440.0f) minutes -= 1440.0f;
  return clampMinutes((int)roundf(minutes));
}

void recalcSunSchedule() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    timeValid = false;
    return;
  }

  timeValid = true;
  tm localNow;
  localtime_r(&now, &localNow);

  const int doy = dayOfYear(localNow);
  const int tzHours = localNow.tm_isdst > 0 ? 1 : 0; // UK GMT/BST

  sunriseMinutesLocal = calculateSunEventMinutesLocal(doy, LATITUDE, LONGITUDE, tzHours, true);
  sunsetMinutesLocal = calculateSunEventMinutesLocal(doy, LATITUDE, LONGITUDE, tzHours, false);
  currentMinutesLocal = localNow.tm_hour * 60 + localNow.tm_min;

  Serial.printf("Schedule sunrise=%02d:%02d sunset=%02d:%02d now=%02d:%02d\n",
                sunriseMinutesLocal / 60, sunriseMinutesLocal % 60,
                sunsetMinutesLocal / 60, sunsetMinutesLocal % 60,
                localNow.tm_hour, localNow.tm_min);
}

String hhmm(int mins) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", mins / 60, mins % 60);
  return String(b);
}


String toHex2(uint8_t v) {
  char b[3];
  snprintf(b, sizeof(b), "%02X", v);
  return String(b);
}

String glowPixelsToHexCsv() {
  String out;
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    if (i > 0) out += ",";
    out += toHex2(glowPixels[i].r) + toHex2(glowPixels[i].g) + toHex2(glowPixels[i].b);
  }
  return out;
}

bool parseHexColor(const String& hex, CRGB& c) {
  if (hex.length() != 6) return false;
  char* endptr = nullptr;
  long v = strtol(hex.c_str(), &endptr, 16);
  if (endptr == nullptr || *endptr != '\0' || v < 0 || v > 0xFFFFFF) return false;
  c.r = (v >> 16) & 0xFF;
  c.g = (v >> 8) & 0xFF;
  c.b = v & 0xFF;
  return true;
}

void setDefaultGlowPixels() {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    glowPixels[i] = CRGB(255, 130, 50);
  }
}

void applyGlowPixelsFromCsv(String csv) {
  csv.trim();
  if (csv.length() == 0) return;

  int start = 0;
  uint16_t idx = 0;
  while (start <= csv.length() && idx < NUM_LEDS) {
    int comma = csv.indexOf(',', start);
    String token = (comma == -1) ? csv.substring(start) : csv.substring(start, comma);
    token.trim();
    CRGB c;
    if (parseHexColor(token, c)) {
      glowPixels[idx] = c;
    }
    idx++;
    if (comma == -1) break;
    start = comma + 1;
  }
}


String glowPickerHex() {
  return String("#") + toHex2(glowPixels[0].r) + toHex2(glowPixels[0].g) + toHex2(glowPixels[0].b);
}

String paletteName(PresencePaletteMode m) {
  switch (m) {
    case PAL_OCEAN: return "Ocean";
    case PAL_LAVA: return "Lava";
    case PAL_FOREST: return "Forest";
    case PAL_HEAT: return "Heat";
    case PAL_PARTY: return "Party";
    case PAL_RAINBOW:
    default: return "Rainbow";
  }
}


String htmlPage() {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("not connected");
  String html = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>Doorbell Tuning</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; max-width: 720px; }
    label { display: block; margin-top: 12px; font-weight: bold; }
    input { width: 100%; padding: 8px; margin-top: 4px; }
    button { margin-top: 16px; padding: 10px 14px; }
    .hint { color: #555; font-size: 0.9rem; }
  </style>
</head>
<body>
  <h1>ESP32 Doorbell Tuning</h1>
  <p><b>IP:</b> )HTML";
  html += ip;
  html += R"HTML(</p>
  <p><b>Sunrise:</b> )HTML";
  html += hhmm(sunriseMinutesLocal);
  html += R"HTML( &nbsp; <b>Sunset:</b> )HTML";
  html += hhmm(sunsetMinutesLocal);
  html += R"HTML(</p>
  <p class="hint">Adjust values and click Save. Settings are stored in ESP32 flash.</p>
  <form method="POST" action="/save">
    <label>Soft glow per-pixel editor (idle mode)</label>
    <input type="color" id="picker" value=")HTML";
  html += glowPickerHex();
  html += R"HTML(" style="height:42px; padding:4px;" />
    <div id="pixelGrid" style="display:grid; grid-template-columns: repeat(6, 32px); grid-template-rows: repeat(11, 32px); gap:4px; margin-top:8px;"></div>
    <input type="hidden" id="pixelData" name="pixelData" value=")HTML";
  html += glowPixelsToHexCsv();
  html += R"HTML(" />

    <label>Presence rainbow brightness (1-255)</label>
    <input type="number" name="presenceBrightness" min="1" max="255" value=")HTML";
  html += String(presenceBrightness);
  html += R"HTML(" />

    <label>Rainbow animation speed (1-20)</label>
    <input type="number" name="rainbowSpeed" min="1" max="20" value=")HTML";
  html += String(rainbowSpeed);
  html += R"HTML(" />

    <label>Presence Palette</label>
    <select name="presencePalette" style="width:100%; padding:8px; margin-top:4px;">
      <option value="0" )HTML";
  html += String(presencePaletteMode == PAL_RAINBOW ? "selected" : "");
  html += R"HTML(>Rainbow</option>
      <option value="1" )HTML";
  html += String(presencePaletteMode == PAL_OCEAN ? "selected" : "");
  html += R"HTML(>Ocean</option>
      <option value="2" )HTML";
  html += String(presencePaletteMode == PAL_LAVA ? "selected" : "");
  html += R"HTML(>Lava</option>
      <option value="3" )HTML";
  html += String(presencePaletteMode == PAL_FOREST ? "selected" : "");
  html += R"HTML(>Forest</option>
      <option value="4" )HTML";
  html += String(presencePaletteMode == PAL_HEAT ? "selected" : "");
  html += R"HTML(>Heat</option>
      <option value="5" )HTML";
  html += String(presencePaletteMode == PAL_PARTY ? "selected" : "");
  html += R"HTML(>Party</option>
    </select>

    <label>Night mode selector</label>
    <select name="nightMode" style="width:100%; padding:8px; margin-top:4px;">
      <option value="0" )HTML";
  html += String(nightMode == NIGHT_AUTO ? "selected" : "");
  html += R"HTML(>AUTO (sunrise/sunset)</option>
      <option value="1" )HTML";
  html += String(nightMode == NIGHT_FORCE_DAY ? "selected" : "");
  html += R"HTML(>FORCE DAY</option>
      <option value="2" )HTML";
  html += String(nightMode == NIGHT_FORCE_NIGHT ? "selected" : "");
  html += R"HTML(>FORCE NIGHT</option>
    </select>

    <label>Presence timeout (ms)</label>
    <input type="number" name="presenceTimeoutMs" min="1000" max="120000" value=")HTML";
  html += String(presenceTimeoutMs);
  html += R"HTML(" />

    <label>Relay pulse time on doorbell press (ms)</label>
    <input type="number" name="doorbellRelayPulseMs" min="50" max="10000" value=")HTML";
  html += String(doorbellRelayPulseMs);
  html += R"HTML(" />

    <label style="margin-top:14px; font-weight:normal;">
      <input type="checkbox" name="allowDayPresence" )HTML";
  html += String(allowDaytimePresenceAnimation ? "checked" : "");
  html += R"HTML( />
      Allow presence animation during daytime
    </label>

    <button type="submit">Save</button>
  </form>

  <script>
    (function() {
      const grid = document.getElementById('pixelGrid');
      const picker = document.getElementById('picker');
      const hidden = document.getElementById('pixelData');
      let colors = (hidden.value || '').split(',').filter(Boolean);
      if (colors.length !== 24) {
        colors = Array(24).fill('FF8232');
      }

      const positions = [
        [1,5], [1,4], [1,3], [1,2],
        [2,1], [3,1], [4,1], [5,1],
        [7,1], [8,1], [9,1], [10,1],
        [11,2], [11,3], [11,4], [11,5],
        [10,6], [9,6], [8,6], [7,6],
        [6,5], [6,4], [6,3], [6,2]
      ];

      function render() {
        grid.innerHTML = '';
        colors.forEach((hex, idx) => {
          const b = document.createElement('button');
          b.type = 'button';
          b.title = 'Pixel ' + (idx + 1);
          b.textContent = String(idx + 1);
          b.style.width = '32px';
          b.style.height = '32px';
          b.style.border = '1px solid #555';
          b.style.borderRadius = '4px';
          b.style.background = '#' + hex;
          b.style.fontSize = '10px';
          b.style.color = '#111';
          b.style.fontWeight = 'bold';
          b.style.gridRow = String(positions[idx][0]);
          b.style.gridColumn = String(positions[idx][1]);
          b.addEventListener('click', () => {
            const selected = picker.value.replace('#', '').toUpperCase();
            colors[idx] = selected;
            hidden.value = colors.join(',');
            render();
          });
          grid.appendChild(b);
        });
        hidden.value = colors.join(',');
      }
      render();
    })();
  </script>

</body>
</html>
)HTML";
  return html;
}

void saveSettingsToPrefs() {
  prefs.putBytes("glowPx", glowPixels, sizeof(glowPixels));
  prefs.putUChar("pBri", presenceBrightness);
  prefs.putUChar("rSpd", rainbowSpeed);
  prefs.putULong("pTimeout", presenceTimeoutMs);
  prefs.putULong("rPulse", doorbellRelayPulseMs);
  prefs.putUChar("nMode", (uint8_t)nightMode);
  prefs.putUChar("pPal", (uint8_t)presencePaletteMode);
  prefs.putBool("aDayP", allowDaytimePresenceAnimation);
}

void loadSettingsFromPrefs() {
  setDefaultGlowPixels();
  size_t loaded = prefs.getBytes("glowPx", glowPixels, sizeof(glowPixels));
  if (loaded != sizeof(glowPixels)) {
    setDefaultGlowPixels();
  }
  presenceBrightness = prefs.getUChar("pBri", DEFAULT_PRESENCE_BRIGHTNESS);
  rainbowSpeed = prefs.getUChar("rSpd", DEFAULT_RAINBOW_SPEED);
  presenceTimeoutMs = prefs.getULong("pTimeout", DEFAULT_PRESENCE_TIMEOUT_MS);
  doorbellRelayPulseMs = prefs.getULong("rPulse", DEFAULT_DOORBELL_RELAY_PULSE_MS);
  nightMode = (NightMode)prefs.getUChar("nMode", (uint8_t)NIGHT_AUTO);
  if (nightMode > NIGHT_FORCE_NIGHT) nightMode = NIGHT_AUTO;
  presencePaletteMode = (PresencePaletteMode)prefs.getUChar("pPal", (uint8_t)PAL_RAINBOW);
  if (presencePaletteMode > PAL_PARTY) presencePaletteMode = PAL_RAINBOW;
  allowDaytimePresenceAnimation = prefs.getBool("aDayP", false);
}

void handleRoot() { server.send(200, "text/html", htmlPage()); }
String jsonBool(bool v) { return v ? "true" : "false"; }

void handleStatus() {
  bool radarRaw = digitalRead(RADAR_PRESENCE_PIN) == HIGH;
  String payload = "{";
  payload += "\"isNight\":" + jsonBool(isNight);
  payload += ",\"nightMode\":" + String((int)nightMode);
  payload += ",\"presenceActive\":" + jsonBool(presenceActive);
  payload += ",\"allowDayPresence\":" + jsonBool(allowDaytimePresenceAnimation);
  payload += ",\"pixelData\":\"" + glowPixelsToHexCsv() + "\"";
  payload += ",\"presenceBrightness\":" + String(presenceBrightness);
  payload += ",\"presencePalette\":\"" + paletteName(presencePaletteMode) + "\"";
  payload += ",\"rainbowSpeed\":" + String(rainbowSpeed);
  payload += ",\"radarRawHigh\":" + jsonBool(radarRaw);
  payload += ",\"timeValid\":" + jsonBool(timeValid);
  payload += ",\"sunrise\":\"" + hhmm(sunriseMinutesLocal) + "\"";
  payload += ",\"sunset\":\"" + hhmm(sunsetMinutesLocal) + "\"";
  if (currentMinutesLocal >= 0) {
    payload += ",\"now\":\"" + hhmm(currentMinutesLocal) + "\"";
  } else {
    payload += ",\"now\":null";
  }
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleSave() {
  if (server.hasArg("pixelData")) {
    applyGlowPixelsFromCsv(server.arg("pixelData"));
  }
  if (server.hasArg("presenceBrightness")) {
    presenceBrightness = constrain(server.arg("presenceBrightness").toInt(), 1, 255);
  }
  if (server.hasArg("rainbowSpeed")) {
    rainbowSpeed = constrain(server.arg("rainbowSpeed").toInt(), 1, 20);
  }
  if (server.hasArg("presencePalette")) {
    int pm = constrain(server.arg("presencePalette").toInt(), 0, 5);
    presencePaletteMode = (PresencePaletteMode)pm;
  }
  allowDaytimePresenceAnimation = server.hasArg("allowDayPresence");
  if (server.hasArg("nightMode")) {
    int nm = constrain(server.arg("nightMode").toInt(), 0, 2);
    nightMode = (NightMode)nm;
  }
  if (server.hasArg("presenceTimeoutMs")) {
    presenceTimeoutMs = constrain(server.arg("presenceTimeoutMs").toInt(), 1000, 120000);
  }
  if (server.hasArg("doorbellRelayPulseMs")) {
    doorbellRelayPulseMs = constrain(server.arg("doorbellRelayPulseMs").toInt(), 50, 10000);
  }

  saveSettingsToPrefs();
  recalcSunSchedule();
  Serial.println("Settings updated via web UI");
  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Web server started");
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi not connected (web UI unavailable)");
  }
}

void configureTimeSync() {
  // UK timezone with BST rules.
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org", "time.cloudflare.com");
}

void triggerDoorbell() {
  Serial.println("Doorbell pressed");
  if (DOORBELL_OUT_PIN >= 0) {
    digitalWrite(DOORBELL_OUT_PIN, DOORBELL_RELAY_ACTIVE_HIGH ? HIGH : LOW);
    doorbellPulseActive = true;
    doorbellPulseStartMs = millis();
  }
  buttonFlashUntilMs = millis() + BUTTON_FLASH_MS;
}

void serviceDoorbellPulse() {
  if (!doorbellPulseActive) return;
  if (millis() - doorbellPulseStartMs >= doorbellRelayPulseMs) {
    digitalWrite(DOORBELL_OUT_PIN, DOORBELL_RELAY_ACTIVE_HIGH ? LOW : HIGH);
    doorbellPulseActive = false;
  }
}

void updateNightMode() {
  if (nightMode == NIGHT_FORCE_NIGHT) {
    isNight = true;
    return;
  }
  if (nightMode == NIGHT_FORCE_DAY) {
    isNight = false;
    return;
  }

  if (millis() - lastScheduleCalcMs >= SUN_SCHEDULE_REFRESH_MS || lastScheduleCalcMs == 0) {
    lastScheduleCalcMs = millis();
    recalcSunSchedule();
  }

  if (!timeValid) {
    // Safe fallback while waiting for NTP.
    isNight = true;
    return;
  }

  time_t now = time(nullptr);
  tm localNow;
  localtime_r(&now, &localNow);
  currentMinutesLocal = localNow.tm_hour * 60 + localNow.tm_min;

  isNight = (currentMinutesLocal >= sunsetMinutesLocal) || (currentMinutesLocal < sunriseMinutesLocal);
}

void updatePresenceState() {
  bool radarRawHigh = digitalRead(RADAR_PRESENCE_PIN) == HIGH;
  bool radarPresent = RADAR_ACTIVE_HIGH ? radarRawHigh : !radarRawHigh;

  if (radarPresent) {
    presenceActive = true;
    lastPresenceMs = millis();
  } else if (presenceActive && (millis() - lastPresenceMs > presenceTimeoutMs)) {
    presenceActive = false;
  }
}

void handleDoorbellButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonRead) {
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > BUTTON_DEBOUNCE_MS) {
    if (reading != debouncedButton) {
      debouncedButton = reading;
      bool pressed = BUTTON_ACTIVE_LOW ? (debouncedButton == LOW) : (debouncedButton == HIGH);
      if (pressed) {
        triggerDoorbell();
      }
    }
  }

  lastButtonRead = reading;
}

void renderIdleNightGlow() {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    leds[i + PIXEL_OFFSET] = glowPixels[i];
  }
  leds[0] = CRGB::Black;
  FastLED.setBrightness(255);
  FastLED.show();
}

void renderPresenceAnimation() {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    uint8_t wave = 255;
    uint8_t idx = hueOffset - (i * 7);
    switch (presencePaletteMode) {
      case PAL_OCEAN:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(OceanColors_p, idx, wave, LINEARBLEND);
        break;
      case PAL_LAVA:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(LavaColors_p, idx, wave, LINEARBLEND);
        break;
      case PAL_FOREST:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(ForestColors_p, idx, wave, LINEARBLEND);
        break;
      case PAL_HEAT:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(HeatColors_p, idx, wave, LINEARBLEND);
        break;
      case PAL_PARTY:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(PartyColors_p, idx, wave, LINEARBLEND);
        break;
      case PAL_RAINBOW:
      default:
        leds[i + PIXEL_OFFSET] = ColorFromPalette(RainbowColors_p, idx, wave, LINEARBLEND);
        break;
    }
  }
  leds[0] = CRGB::Black;
  hueOffset += rainbowSpeed;
  FastLED.setBrightness(presenceBrightness);
  FastLED.show();
}

void runStartupLedSelfTest() {
  FastLED.setBrightness(120);
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i + PIXEL_OFFSET] = CRGB::Red;
  leds[0] = CRGB::Black; FastLED.show(); delay(250);
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i + PIXEL_OFFSET] = CRGB::Green;
  leds[0] = CRGB::Black; FastLED.show(); delay(250);
  for (uint16_t i = 0; i < NUM_LEDS; i++) leds[i + PIXEL_OFFSET] = CRGB::Blue;
  leds[0] = CRGB::Black; FastLED.show(); delay(250);
  FastLED.clear(true);
}

void renderButtonFlashIfActive() {
  if (millis() >= buttonFlashUntilMs) return;
  FastLED.setBrightness(180);
  fill_solid(leds, PHYSICAL_NUM_LEDS, CRGB::White);
  leds[0] = CRGB::Black;
  FastLED.show();
}

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RADAR_PRESENCE_PIN, RADAR_USE_PULLDOWN ? INPUT_PULLDOWN : INPUT);

  if (DOORBELL_OUT_PIN >= 0) {
    pinMode(DOORBELL_OUT_PIN, OUTPUT);
    digitalWrite(DOORBELL_OUT_PIN, DOORBELL_RELAY_ACTIVE_HIGH ? LOW : HIGH);
  }

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, PHYSICAL_NUM_LEDS);
  FastLED.clear(true);
  runStartupLedSelfTest();

  prefs.begin("doorbell", false);
  loadSettingsFromPrefs();

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    configureTimeSync();
    recalcSunSchedule();
    setupWebServer();
  }

  Serial.println("Doorbell controller started");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }

  serviceDoorbellPulse();
  handleDoorbellButton();
  updateNightMode();

  renderButtonFlashIfActive();
  if (millis() < buttonFlashUntilMs) {
    delay(10);
    return;
  }

  bool animationAllowed = isNight || allowDaytimePresenceAnimation;

  if (!animationAllowed) {
    FastLED.clear(true);
    delay(10);
    return;
  }

  updatePresenceState();

  if (presenceActive) {
    if (millis() - lastIdleFrameMs >= IDLE_ANIM_STEP_MS) {
      lastIdleFrameMs = millis();
      renderPresenceAnimation();
    }
  } else if (isNight) {
    renderIdleNightGlow();
    delay(50);
  } else {
    // Daytime + day-presence mode: no idle glow, keep LEDs off until presence.
    FastLED.clear(true);
    delay(20);
  }
}
