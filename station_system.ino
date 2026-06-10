// ================================================================
//  Bus Tracker — Button Triggered + LCD + GPRS POST — Arduino MEGA
//
//  GPRS SIM900 → Serial1  TX=pin18, RX=pin19
//  LCD 20x4 I2C→ SDA=pin20, SCL=pin21
//  DFPlayer    → Serial3  TX=pin14, RX=pin15
//  Push Button → pin 7 (other leg to GND)
//
//  Press button → POST bus_id to server
//              → server returns station + ETA
//              → display on LCD + play audio
//
//  WIRING NOTES:
//    - 1K resistor in series on Serial3 TX (pin14) → DFPlayer RX
//    - 100µF capacitor across DFPlayer VCC and GND
//    - Button wired: pin7 → button → GND (uses internal pull-up)
//    - SD card: files in root, copied in order; 0016.mp3 is fallback
//    - FALLBACK_TRACK_INDEX = 16 (0016.mp3 is the 16th file copied)
// ================================================================
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define gprsSerial Serial1
#define dfSerial   Serial3

#define BUTTON_PIN 7

#define FALLBACK_TRACK_INDEX  16

LiquidCrystal_I2C lcd(0x27, 20, 4);

const char SERVER[]   = "ermias21.pythonanywhere.com";
const char ENDPOINT[] = "/api/location/";
const char BUS_ID[]   = "12345";

int  failCount   = 0;
int  lastSid     = -1;
bool requesting  = false;

// ================================================================
//  LCD helper
// ================================================================
void lcdRow(uint8_t row, const char* msg) {
  lcd.setCursor(0, row);
  uint8_t i = 0;
  while (i < 20 && msg[i]) { lcd.write(msg[i]); i++; }
  while (i < 20)            { lcd.write(' ');    i++; }
}

// ================================================================
//  JSON helpers
// ================================================================
int jsonInt(const char* json, const char* key) {
  char tok[32];
  snprintf(tok, sizeof(tok), "\"%s\":", key);
  const char* p = strstr(json, tok);
  if (!p) return -1;
  p += strlen(tok);
  while (*p == ' ') p++;
  return atoi(p);
}

float jsonFloat(const char* json, const char* key) {
  char tok[32];
  snprintf(tok, sizeof(tok), "\"%s\":", key);
  const char* p = strstr(json, tok);
  if (!p) return 0.0f;
  p += strlen(tok);
  while (*p == ' ') p++;
  return atof(p);
}

bool jsonStr(const char* json, const char* key, char* out, uint8_t outLen) {
  char tok[32];
  snprintf(tok, sizeof(tok), "\"%s\":", key);
  const char* p = strstr(json, tok);
  if (!p) { out[0] = '\0'; return false; }
  p += strlen(tok);
  while (*p == ' ') p++;
  if (*p != '"') { out[0] = '\0'; return false; }
  p++;
  uint8_t i = 0;
  while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
  out[i] = '\0';
  return true;
}

// Detects if a JSON key has a null value
bool jsonIsNull(const char* json, const char* key) {
  char tok[32];
  snprintf(tok, sizeof(tok), "\"%s\":", key);
  const char* p = strstr(json, tok);
  if (!p) return true;   // key missing → treat as null
  p += strlen(tok);
  while (*p == ' ') p++;
  return (strncmp(p, "null", 4) == 0);
}

bool extractJson(const char* raw, char* out, uint8_t outLen) {
  const char* start = strchr(raw, '{');
  if (!start) { out[0] = '\0'; return false; }
  const char* end = strrchr(raw, '}');
  if (!end || end <= start) { out[0] = '\0'; return false; }
  uint8_t len = (uint8_t)(end - start + 1);
  if (len >= outLen) len = outLen - 1;
  strncpy(out, start, len);
  out[len] = '\0';
  return true;
}

// ================================================================
//  DFPlayer
// ================================================================
void dfSendCmd(uint8_t cmd, uint8_t param1, uint8_t param2) {
  uint8_t buf[10];
  buf[0] = 0x7E;
  buf[1] = 0xFF;
  buf[2] = 0x06;
  buf[3] = cmd;
  buf[4] = 0x00;
  buf[5] = param1;
  buf[6] = param2;
  int16_t chk = -(int16_t)(buf[1]+buf[2]+buf[3]+buf[4]+buf[5]+buf[6]);
  buf[7] = (chk >> 8) & 0xFF;
  buf[8] = chk & 0xFF;
  buf[9] = 0xEF;
  dfSerial.write(buf, 10);
}

void dfSetVolume(uint8_t vol) { dfSendCmd(0x06, 0x00, vol); delay(200); }

void dfPlayTrack(uint16_t track) {
 dfSendCmd(0x03, (track >> 8) & 0xFF, track & 0xFF);  // 0x03 = play by index from root
}

void dfInit() {
  dfSerial.begin(9600);
  delay(3000);
  dfSendCmd(0x0C, 0x00, 0x00);   // reset
  delay(3000);
  dfSetVolume(25);
  delay(500);
  Serial.println("DFPlayer Ready");
}

// ================================================================
//  Display server response on LCD + play audio
// ================================================================
void displayResponse(const char* json) {
  int   sid    = jsonInt(json,   "sid");
  float dist   = jsonFloat(json, "dist");
  int   etaMin = jsonInt(json,   "eta_min");

  char sname[16]; jsonStr(json, "sname",   sname, sizeof(sname));
  char eta[16];   jsonStr(json, "eta_lbl", eta,   sizeof(eta));

  Serial.print("sid=");      Serial.print(sid);
  Serial.print(" sname=");   Serial.print(sname);
  Serial.print(" dist=");    Serial.print(dist);
  Serial.print(" eta=");     Serial.print(eta);
  Serial.print(" eta_min="); Serial.println(etaMin);

  char row[21];

  // Row 0: station name
  lcdRow(0, "current Station:");
  snprintf(row, sizeof(row), "  %-18s", sname[0] ? sname : "Unknown");
  lcdRow(1, row);

  // Row 2: ETA
  snprintf(row, sizeof(row), "ETA: %-15s", eta[0] ? eta : "--");
  lcdRow(2, row);

  // Row 3: distance
  char distStr[10];
  dtostrf(dist, 0, 1, distStr);
  snprintf(row, sizeof(row), "Dist: %-10s km  ", distStr);
  lcdRow(3, row);

  // ── Play audio by eta_min; fallback to 0016.mp3 if null / 0 / negative ──
  uint16_t track;
  bool etaNull = jsonIsNull(json, "eta_min");

  if (!etaNull && etaMin > 0) {
    track = (uint16_t)etaMin;
    Serial.print("Playing eta_min track "); Serial.println(track);
  } else {
    track = FALLBACK_TRACK_INDEX;   // 0016.mp3
    if (etaNull)          Serial.println("eta_min is null — playing fallback 0016.mp3");
    else if (etaMin == 0) Serial.println("eta_min is 0 — playing fallback 0016.mp3");
    else                  Serial.println("eta_min is negative — playing fallback 0016.mp3");
  }

  dfSetVolume(25);
  delay(200);
  dfPlayTrack(track);
  delay(500);
}

// ================================================================
//  AT command helpers
// ================================================================
static char _atBuf[256];

bool sendAT(const char* cmd, const char* expected = "OK",
            unsigned long timeout = 5000) {
  delay(100);
  Serial.print("[AT] "); Serial.println(cmd);
  gprsSerial.println(cmd);

  _atBuf[0] = '\0';
  uint8_t pos = 0;
  unsigned long t = millis();

  while (millis() - t < timeout) {
    while (gprsSerial.available() && pos < sizeof(_atBuf) - 1) {
      _atBuf[pos++] = (char)gprsSerial.read();
      _atBuf[pos]   = '\0';
    }
    if (strstr(_atBuf, expected)) {
      Serial.print("[OK] "); Serial.println(_atBuf);
      return true;
    }
    if (strstr(_atBuf, "ERROR")) {
      Serial.print("[ERR] "); Serial.println(_atBuf);
      return false;
    }
  }
  Serial.print("[TIMEOUT] "); Serial.println(_atBuf);
  return false;
}

const char* sendATread(const char* cmd, unsigned long timeout = 5000) {
  delay(100);
  gprsSerial.println(cmd);
  _atBuf[0] = '\0';
  uint8_t pos = 0;
  unsigned long t = millis();
  while (millis() - t < timeout) {
    while (gprsSerial.available() && pos < sizeof(_atBuf) - 1) {
      _atBuf[pos++] = (char)gprsSerial.read();
      _atBuf[pos]   = '\0';
    }
    if (strstr(_atBuf, "OK") || strstr(_atBuf, "ERROR")) break;
  }
  Serial.print("[READ] "); Serial.println(_atBuf);
  return _atBuf;
}

void flushGPRS() {
  delay(300);
  while (gprsSerial.available()) gprsSerial.read();
}

// ================================================================
//  GPRS init
// ================================================================
void gprsInit() {
  delay(3000);
  for (int i = 0; i < 5; i++) { gprsSerial.println("AT"); delay(500); flushGPRS(); }
  gprsSerial.println("AT+IPR=9600"); delay(500); flushGPRS();

  sendAT("AT");
  sendAT("ATE0");
  sendAT("AT+CMEE=2");
  sendAT("AT+CPIN?");

  lcdRow(0, "Connecting...       ");
  lcdRow(1, "                    ");

  Serial.println("Waiting for network...");
  for (int i = 0; i < 15; i++) {
    const char* r = sendATread("AT+CREG?");
    if (strstr(r, ",1") || strstr(r, ",5")) {
      Serial.println("Network registered.");
      lcdRow(1, "Network OK          ");
      break;
    }
    delay(2000);
  }

  sendAT("AT+CSQ");
  gprsSerial.println("AT+SAPBR=0,1"); delay(3000); flushGPRS();
  sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"APN\",\"safaricom\"",  "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"USER\",\"\"",           "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"PWD\",\"\"",            "OK", 8000);
  sendAT("AT+SAPBR=1,1", "OK", 20000);
  sendAT("AT+SAPBR=2,1");
  gprsSerial.println("AT+HTTPTERM"); delay(1000); flushGPRS();

  lcdRow(0, "Ready               ");
  lcdRow(1, "Press button for    ");
  lcdRow(2, "station info        ");
  lcdRow(3, "                    ");
  Serial.println("=== GPRS Ready ===");
}

// ================================================================
//  HTTP POST — sends only bus_id, gets station + ETA back
// ================================================================
bool doPost() {
  char body[120];
  snprintf(body, sizeof(body),
           "bus_id=%s&lat=8.886572&lon=38.811935&speed=0.0",
           BUS_ID);

  char url[96];
  snprintf(url, sizeof(url), "http://%s%s", SERVER, ENDPOINT);

  Serial.print("[POST DATA] "); Serial.println(body);

  lcdRow(0, "Fetching info...    ");
  lcdRow(1, "                    ");
  lcdRow(2, "                    ");
  lcdRow(3, "                    ");

  gprsSerial.println("AT+SAPBR=0,1"); delay(2000); flushGPRS();
  sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", "OK", 5000);
  sendAT("AT+SAPBR=3,1,\"APN\",\"safaricom\"",  "OK", 5000);
  sendAT("AT+SAPBR=1,1", "OK", 20000);
  sendAT("AT+SAPBR=2,1");

  gprsSerial.println("AT+HTTPTERM"); delay(1000); flushGPRS();
  if (!sendAT("AT+HTTPINIT", "OK", 8000)) return false;

  sendAT("AT+HTTPPARA=\"CID\",1");
  char urlCmd[128];
  snprintf(urlCmd, sizeof(urlCmd), "AT+HTTPPARA=\"URL\",\"%s\"", url);
  sendAT(urlCmd);
  sendAT("AT+HTTPPARA=\"CONTENT\",\"application/x-www-form-urlencoded\"");

  char dataCmd[48];
  snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%d,5000", (int)strlen(body));
  Serial.print("[AT] "); Serial.println(dataCmd);
  gprsSerial.println(dataCmd);

  _atBuf[0] = '\0';
  uint8_t pos = 0;
  unsigned long t = millis();
  bool gotDL = false;
  while (millis() - t < 8000) {
    while (gprsSerial.available() && pos < sizeof(_atBuf) - 1) {
      _atBuf[pos++] = (char)gprsSerial.read();
      _atBuf[pos]   = '\0';
    }
    if (strstr(_atBuf, "DOWNLOAD")) { gotDL = true; break; }
    if (strstr(_atBuf, "ERROR"))    { break; }
  }
  Serial.print("[DOWNLOAD buf] "); Serial.println(_atBuf);
  if (!gotDL) {
    Serial.println("[ERR] No DOWNLOAD prompt");
    sendAT("AT+HTTPTERM");
    return false;
  }

  gprsSerial.print(body);
  delay(3000);
  flushGPRS();

  Serial.println("[AT] AT+HTTPACTION=1");
  gprsSerial.println("AT+HTTPACTION=1");

  _atBuf[0] = '\0'; pos = 0; t = millis();
  while (millis() - t < 60000) {
    while (gprsSerial.available() && pos < sizeof(_atBuf) - 1) {
      char c = (char)gprsSerial.read();
      _atBuf[pos++] = c;
      _atBuf[pos]   = '\0';
      Serial.write(c);
    }
    const char* urc = strstr(_atBuf, "+HTTPACTION:");
    if (urc) {
      const char* c1 = strchr(urc, ',');
      if (c1 && strchr(c1 + 1, ',')) break;
    }
  }
  Serial.println();
  Serial.print("[HTTP action] "); Serial.println(_atBuf);
  bool success = (strstr(_atBuf, ",200,") != NULL);

  delay(1500); flushGPRS();

  gprsSerial.println("AT+HTTPREAD");
  static char serverResp[256];
  serverResp[0] = '\0'; pos = 0; t = millis();
  while (millis() - t < 10000) {
    while (gprsSerial.available() && pos < sizeof(serverResp) - 1) {
      serverResp[pos++] = (char)gprsSerial.read();
      serverResp[pos]   = '\0';
    }
    if (strstr(serverResp, "\nOK")) break;
  }
  Serial.println("---- Server response ----");
  Serial.println(serverResp);
  Serial.println("-------------------------");

  sendAT("AT+HTTPTERM");

  static char jsonBuf[200];
  bool hasJson = extractJson(serverResp, jsonBuf, sizeof(jsonBuf));

  if (success && hasJson && strstr(jsonBuf, "sid")) {
    displayResponse(jsonBuf);
    return true;
  }

  lcdRow(0, "Request failed      ");
  lcdRow(1, "Check connection    ");
  lcdRow(2, "                    ");
  lcdRow(3, "Press button retry  ");
  Serial.println("[WARN] No valid JSON with sid");
  return false;
}

// ================================================================
//  SIM900 reset on repeated failures
// ================================================================
void moduleReset() {
  Serial.println("=== SIM900 Soft Reset ===");
  lcdRow(0, "Resetting modem...  ");
  lcdRow(1, "                    ");
  lcdRow(2, "                    ");
  lcdRow(3, "                    ");

  flushGPRS();
  gprsSerial.println("AT+CFUN=1,1");
  delay(8000); flushGPRS();

  unsigned long t = millis();
  while (millis() - t < 15000) {
    if (gprsSerial.available()) {
      char line[32]; uint8_t i = 0;
      while (gprsSerial.available() && i < 31) {
        char c = gprsSerial.read();
        if (c == '\n') break;
        line[i++] = c;
      }
      line[i] = '\0';
      Serial.print("[SIM900] "); Serial.println(line);
      if (strstr(line, "RDY")) break;
    }
  }

  delay(2000); flushGPRS();
  sendAT("AT"); sendAT("ATE0");

  for (int i = 0; i < 20; i++) {
    const char* r = sendATread("AT+CREG?");
    if (strstr(r, ",1") || strstr(r, ",5")) { Serial.println("Network registered."); break; }
    delay(2000);
  }

  gprsSerial.println("AT+SAPBR=0,1"); delay(3000); flushGPRS();
  sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"APN\",\"safaricom\"",  "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"USER\",\"\"",           "OK", 8000);
  sendAT("AT+SAPBR=3,1,\"PWD\",\"\"",            "OK", 8000);
  sendAT("AT+SAPBR=1,1", "OK", 20000);
  sendAT("AT+SAPBR=2,1");
  gprsSerial.println("AT+HTTPTERM"); delay(1000); flushGPRS();
  failCount = 0;

  lcdRow(0, "Ready               ");
  lcdRow(1, "Press button for    ");
  lcdRow(2, "station info        ");
  lcdRow(3, "                    ");
  Serial.println("=== Reset Complete ===");
}

// ================================================================
void setup() {
  Serial.begin(9600);
  gprsSerial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcdRow(0, "Station System Started  ");
  lcdRow(1, "                    ");
  lcdRow(2, "                    ");
  lcdRow(3, "                    ");

  Serial.println("=== Bus Tracker (Mega) ===");
  delay(2000);
  dfInit();
  flushGPRS();
  gprsInit();
}

// ================================================================
void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {

      Serial.println("Button pressed — fetching station info...");

      bool ok = doPost();

      if (ok) {
        Serial.println(">>> SUCCESS <<<");
        failCount = 0;
      } else {
        Serial.print(">>> FAILED #"); Serial.println(++failCount);
        if (failCount >= 3) moduleReset();
      }

      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      delay(200);
    }
  }
}