// 錠剤カウントはかり（AtomS3 Lite ＋ M5Stack Unit Weight-I2C）
//
// 配線: Unit Weight-I2C の Grove ケーブルを AtomS3 Lite の Grove 端子へ挿すだけ。
//   AtomS3 Lite Grove: 1=GPIO1(SCL) 2=GPIO2(SDA) 3=5V 4=GND
//   Unit Weight-I2C: SDA/SCL は 4.7kΩ で 3.3V にプルアップ済み（公式回路図で確認）
//
// ロードセルは差し替えて使う前提で、校正値を2組持つ。
//   スロット0「小」= 300g ロードセル（錠剤）
//   スロット1「大」= 10kg ロードセル（本）
//
// 重さの計算はユニット側の換算値を使わず、生のADC値（レジスタ0x00）から自前で行う。
// 記事で「生の値がどう揺れるか」を見せるため。

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// ---- ピンと定数 ----
static const uint8_t PIN_SDA = 2;
static const uint8_t PIN_SCL = 1;
static const uint8_t PIN_LED = 35;   // AtomS3 Lite 内蔵のフルカラーLED
static const uint8_t PIN_BTN = 41;   // AtomS3 Lite 本体のボタン（押すとLOW）
static const uint8_t UNIT_ADDR = 0x26;
static const uint8_t REG_RAW_ADC = 0x00;

static const uint32_t SAMPLE_MS = 100;   // ユニットのHX711は毎秒10回なので100msごとに読む
static const int WIN = 16;               // 平均をとる個数（約1.6秒ぶん）
static const int HIST = 600;             // 1秒ごとの重さを10分ぶん持つ（ずれのグラフ用）
static const int MAX_ITEMS = 20;

// ---- 校正スロット ----
struct Slot {
  const char* label;
  float stableBand;   // この標準偏差(g)より小さければ「安定」
  float zeroBand;     // この範囲(g)なら空とみなして自動でゼロを追う
  float minCpg;       // 校正で受け付ける「1gあたりのカウント」の下限（これより小さければ、重さがかかっていない）
  int32_t offset;     // 空のときの生の値
  float countsPerGram;
  bool calibrated;
};
Slot slots[2] = {
  {"小（300g）", 0.02f, 0.03f, 1000.0f, 0, 0, false},
  {"大（10kg）", 1.0f, 2.0f, 50.0f, 0, 0, false},
};
int cur = 0;

// ---- 品目（1個あたりの重さ） ----
struct Item { String name; float unit; };
Item items[MAX_ITEMS];
int itemCount = 0;
int curItem = -1;

// ---- 状態 ----
int32_t win[WIN];
int winFill = 0, winPos = 0;
int32_t lastRaw = 0;
bool sensorOk = false;
uint32_t errCount = 0;
float meanRaw = 0, sdRaw = 0;
float weightG = 0, sdG = 0;
bool stable = false;
bool autoZero = true;
uint32_t zeroSince = 0;

float hist[HIST];
int histPos = 0, histFill = 0;
uint32_t lastHistMs = 0, lastSampleMs = 0;

Preferences prefs;

// ---- 瓶を置きっぱなしにして「減った粒」を見つける ----
// 瓶が持ち上げられる・ふたを開けるなどで値がゆれたら、落ち着いたあとの重さを、ゆれる前の重さと比べる。
// ゆれていないあいだは、前の重さを今の重さに合わせ続けるので、ゆっくりしたずれは数に入らない。
static const float TAKE_MIN_G = 3.0f;      // これより軽いときは「瓶がのっていない」とみなす
static const uint32_t SETTLE_MS = 2000;    // ゆれたあと、この時間落ち着いていたら比べる
static const float TAKE_TOL = 0.35f;       // 粒数の端数がこれより大きければ「あいまい」
float baseW = NAN;          // ゆれる前の、落ち着いていた瓶の重さ
bool disturbed = false;
uint32_t settledSince = 0;
struct TakeEv { time_t t; int n; float x; };
static const int MAX_EV = 30;
TakeEv evs[MAX_EV];
int evCount = 0;
WebServer server(80);
bool apMode = false;

// ---- LED ----
void led(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_LED, r, g, b);
#else
  neopixelWrite(PIN_LED, r, g, b);
#endif
}

// ---- ユニットから生の値を読む ----
bool readRaw(int32_t& out) {
  Wire.beginTransmission(UNIT_ADDR);
  Wire.write(REG_RAW_ADC);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)UNIT_ADDR, 4) != 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= (uint32_t)Wire.read() << (8 * i);
  out = (int32_t)v;
  return true;
}

// 窓の中の値を並べ替え、上下2個ずつ捨てて平均（たまに混ざる外れ値よけ）
void computeStats() {
  int n = winFill;
  if (n == 0) return;
  int32_t tmp[WIN];
  for (int i = 0; i < n; i++) tmp[i] = win[i];
  for (int i = 1; i < n; i++) {  // 挿入ソート（16個なので十分）
    int32_t k = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  int cut = (n >= 8) ? 2 : 0;
  double sum = 0; int m = 0;
  for (int i = cut; i < n - cut; i++) { sum += tmp[i]; m++; }
  meanRaw = sum / m;
  double ss = 0;
  for (int i = cut; i < n - cut; i++) { double d = tmp[i] - meanRaw; ss += d * d; }
  sdRaw = sqrt(ss / m);
}

void updateWeight() {
  Slot& s = slots[cur];
  if (s.calibrated && s.countsPerGram != 0) {
    weightG = (meanRaw - s.offset) / s.countsPerGram;
    sdG = sdRaw / fabs(s.countsPerGram);
  } else {
    weightG = 0;
    sdG = 0;
  }
  stable = (winFill == WIN) && s.calibrated && (sdG < s.stableBand);

  // 自動ゼロ追従: 空のまま3秒安定していたら、ゼロ点をそっと合わせ直す
  if (autoZero && stable && fabs(weightG) < s.zeroBand) {
    if (zeroSince == 0) zeroSince = millis();
    if (millis() - zeroSince > 3000) {
      s.offset = (int32_t)lround(meanRaw);
      zeroSince = millis();
    }
  } else {
    zeroSince = 0;
  }
}

void recordTake(int n, float x, float before, float after) {
  time_t t = time(nullptr);
  if (evCount == MAX_EV) { for (int i = 1; i < MAX_EV; i++) evs[i - 1] = evs[i]; evCount--; }
  evs[evCount++] = {t, n, x};
  bool fresh = !LittleFS.exists("/take.csv");
  File f = LittleFS.open("/take.csv", FILE_APPEND);
  if (f) {
    if (fresh) f.print("日時,品目,減った粒,粒数の計算値,前g,後g\n");
    f.printf("%s,%s,%d,%.2f,%.3f,%.3f\n", nowText().c_str(),
             curItem >= 0 ? items[curItem].name.c_str() : "", n, x, before, after);
    f.close();
  }
  Serial.printf("# take n=%d x=%.2f before=%.3f after=%.3f\n", n, x, before, after);
}

void detectTake() {
  if (curItem < 0 || !slots[cur].calibrated) return;
  float unit = items[curItem].unit;
  if (unit <= 0) return;
  bool onScale = weightG > TAKE_MIN_G;
  if (!stable || !onScale) {
    if (!isnan(baseW)) disturbed = true;
    settledSince = 0;
    return;
  }
  // 落ち着いていて、瓶がのっている
  if (isnan(baseW)) { baseW = weightG; return; }
  if (!disturbed) { baseW = weightG; return; }   // ゆっくりしたずれに合わせる
  if (settledSince == 0) settledSince = millis();
  if (millis() - settledSince < SETTLE_MS) return;
  float x = (baseW - weightG) / unit;
  int n = (int)lround(x);
  if (n != 0) recordTake(fabs(x - n) <= TAKE_TOL ? n : 0, x, baseW, weightG);
  baseW = weightG;
  disturbed = false;
  settledSince = 0;
}

void sample() {
  int32_t raw;
  if (readRaw(raw)) {
    sensorOk = true;
    lastRaw = raw;
    win[winPos] = raw;
    winPos = (winPos + 1) % WIN;
    if (winFill < WIN) winFill++;
    computeStats();
    updateWeight();
    detectTake();
    // シリアルにCSVで流す（記事のグラフ用）: ミリ秒,生の値,重さg
    Serial.printf("%lu,%ld,%.4f\n", (unsigned long)millis(), (long)raw, weightG);
  } else {
    sensorOk = false;
    errCount++;
  }
}

// ---- 保存と読み込み ----
void saveSlots() {
  prefs.begin("scale", false);
  for (int i = 0; i < 2; i++) {
    String p = "s" + String(i);
    prefs.putInt((p + "off").c_str(), slots[i].offset);
    prefs.putFloat((p + "cpg").c_str(), slots[i].countsPerGram);
    prefs.putBool((p + "cal").c_str(), slots[i].calibrated);
  }
  prefs.putInt("slot", cur);
  prefs.putBool("autozero", autoZero);
  prefs.end();
}

void saveItems() {
  String s;
  for (int i = 0; i < itemCount; i++) {
    s += items[i].name; s += '\t'; s += String(items[i].unit, 6); s += '\n';
  }
  prefs.begin("scale", false);
  prefs.putString("items", s);
  prefs.putInt("item", curItem);
  prefs.end();
}

void loadAll() {
  prefs.begin("scale", true);
  for (int i = 0; i < 2; i++) {
    String p = "s" + String(i);
    slots[i].offset = prefs.getInt((p + "off").c_str(), 0);
    slots[i].countsPerGram = prefs.getFloat((p + "cpg").c_str(), 0);
    slots[i].calibrated = prefs.getBool((p + "cal").c_str(), false);
  }
  cur = constrain(prefs.getInt("slot", 0), 0, 1);
  autoZero = prefs.getBool("autozero", true);
  String s = prefs.getString("items", "");
  curItem = prefs.getInt("item", -1);
  prefs.end();

  itemCount = 0;
  int start = 0;
  while (start < (int)s.length() && itemCount < MAX_ITEMS) {
    int nl = s.indexOf('\n', start);
    if (nl < 0) nl = s.length();
    String line = s.substring(start, nl);
    int tab = line.indexOf('\t');
    if (tab > 0) {
      items[itemCount].name = line.substring(0, tab);
      items[itemCount].unit = line.substring(tab + 1).toFloat();
      if (items[itemCount].unit > 0) itemCount++;
    }
    start = nl + 1;
  }
  if (curItem >= itemCount) curItem = -1;
}

// ---- 時刻 ----
String nowText() {
  time_t t = time(nullptr);
  if (t < 1700000000) return String("");  // まだ時刻が合っていない
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}

// ---- Web ----
String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else o += c;
  }
  return o;
}

void sendJsonOk(const String& msg = "") {
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"" + jsonEscape(msg) + "\"}");
}
void sendJsonErr(const String& msg) {
  server.send(400, "application/json", "{\"ok\":false,\"msg\":\"" + jsonEscape(msg) + "\"}");
}

void handleState() {
  Slot& s = slots[cur];
  String j = "{";
  j += "\"sensorOk\":" + String(sensorOk ? "true" : "false");
  j += ",\"errCount\":" + String(errCount);
  j += ",\"raw\":" + String(lastRaw);
  j += ",\"meanRaw\":" + String(meanRaw, 1);
  j += ",\"sdRaw\":" + String(sdRaw, 1);
  j += ",\"slot\":" + String(cur);
  j += ",\"slotLabel\":\"" + jsonEscape(s.label) + "\"";
  j += ",\"calibrated\":" + String(s.calibrated ? "true" : "false");
  j += ",\"countsPerGram\":" + String(s.countsPerGram, 3);
  j += ",\"weight\":" + String(weightG, 4);
  j += ",\"sd\":" + String(sdG, 4);
  j += ",\"stable\":" + String(stable ? "true" : "false");
  j += ",\"autoZero\":" + String(autoZero ? "true" : "false");
  j += ",\"item\":" + String(curItem);
  if (curItem >= 0) {
    float u = items[curItem].unit;
    float x = weightG / u;
    long c = lround(x);
    if (c < 0) c = 0;
    j += ",\"unit\":" + String(u, 5);
    j += ",\"count\":" + String(c);
    j += ",\"frac\":" + String(fabs(x - c), 3);
  }
  // 減った粒の記録
  j += ",\"baseW\":" + (isnan(baseW) ? String("null") : String(baseW, 3));
  {
    time_t t = time(nullptr);
    struct tm today; localtime_r(&t, &today);
    int todayN = 0;
    for (int i = 0; i < evCount; i++) {
      struct tm tv; localtime_r(&evs[i].t, &tv);
      if (tv.tm_year == today.tm_year && tv.tm_yday == today.tm_yday && evs[i].n > 0) todayN += evs[i].n;
    }
    j += ",\"todayTaken\":" + String(todayN);
    j += ",\"events\":[";
    for (int i = evCount - 1, k = 0; i >= 0 && k < 10; i--, k++) {
      if (k) j += ",";
      char buf[24];
      struct tm tv; localtime_r(&evs[i].t, &tv);
      strftime(buf, sizeof(buf), "%m/%d %H:%M", &tv);
      j += "{\"t\":\"" + String(buf) + "\",\"n\":" + String(evs[i].n) + ",\"x\":" + String(evs[i].x, 2) + "}";
    }
    j += "]";
  }
  j += ",\"items\":[";
  for (int i = 0; i < itemCount; i++) {
    if (i) j += ",";
    j += "{\"name\":\"" + jsonEscape(items[i].name) + "\",\"unit\":" + String(items[i].unit, 5) + "}";
  }
  j += "]";
  j += ",\"time\":\"" + nowText() + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handleHistory() {
  String j = "[";
  for (int i = 0; i < histFill; i++) {
    int idx = (histPos - histFill + i + HIST) % HIST;
    if (i) j += ",";
    j += String(hist[idx], 4);
  }
  j += "]";
  server.send(200, "application/json", j);
}

void handleTare() {
  if (winFill < WIN) { sendJsonErr("まだ値がそろっていません。数秒待ってください"); return; }
  slots[cur].offset = (int32_t)lround(meanRaw);
  saveSlots();
  sendJsonOk("ゼロに合わせました");
}

// 校正: 皿を空にしてゼロに合わせたあと、重さの分かっているものをのせて g を送る
// 例: 1円玉10枚 = 10.00g
void handleCal() {
  float g = server.arg("g").toFloat();
  if (g <= 0) { sendJsonErr("のせた重さ(g)を入れてください"); return; }
  if (winFill < WIN) { sendJsonErr("まだ値がそろっていません"); return; }
  float delta = meanRaw - slots[cur].offset;
  if (fabs(delta) < 50) { sendJsonErr("値がほとんど変わっていません。のせたか、配線を確かめてください"); return; }
  // 差が小さすぎるときは受け付けない（重さが台に逃げている、ゼロの位置がのせたあとで覚えられている、など）
  if (fabs(delta) / g < slots[cur].minCpg) {
    sendJsonErr("のせる前と後の差が小さすぎます（1gあたり " + String(fabs(delta) / g, 0) + " カウント）。"
                "皿を空にして「ゼロに合わせる」を押してから、のせ直してください");
    return;
  }
  // 配線のA+とA-が逆でも、ここで向きごと覚えるので動く
  slots[cur].countsPerGram = delta / g;
  slots[cur].calibrated = true;
  saveSlots();
  sendJsonOk("校正しました（1gあたり " + String(fabs(slots[cur].countsPerGram), 1) + " カウント）");
}

// N個のせて1個の重さを覚える
void handleLearn() {
  int n = server.arg("n").toInt();
  String name = server.arg("name");
  name.trim();
  if (n <= 0) { sendJsonErr("個数を入れてください"); return; }
  if (name.length() == 0) { sendJsonErr("名前を入れてください"); return; }
  if (!slots[cur].calibrated) { sendJsonErr("先に校正してください"); return; }
  if (!stable) { sendJsonErr("値が落ち着いてから押してください"); return; }
  if (weightG <= 0) { sendJsonErr("重さが0以下です"); return; }
  float unit = weightG / n;
  int idx = -1;
  for (int i = 0; i < itemCount; i++) if (items[i].name == name) idx = i;
  if (idx < 0) {
    if (itemCount >= MAX_ITEMS) { sendJsonErr("登録できるのは" + String(MAX_ITEMS) + "件までです"); return; }
    idx = itemCount++;
    items[idx].name = name;
  }
  items[idx].unit = unit;
  curItem = idx;
  saveItems();
  sendJsonOk(name + " を1個 " + String(unit, 4) + "g で覚えました");
}

// 今の品目の1個の重さを、数字で直す（瓶から10粒出した差などで測ったとき）
void handleUnit() {
  if (curItem < 0) { sendJsonErr("品目を選んでください"); return; }
  float u = server.arg("g").toFloat();
  if (u <= 0) { sendJsonErr("1個の重さ(g)を入れてください"); return; }
  items[curItem].unit = u;
  saveItems();
  sendJsonOk(items[curItem].name + " を1個 " + String(u, 4) + "g に直しました");
}

void handleTakeCsv() {
  File f = LittleFS.open("/take.csv", FILE_READ);
  if (!f) { server.send(200, "text/csv; charset=utf-8", "日時,品目,減った粒,粒数の計算値,前g,後g\n"); return; }
  server.streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

void handleSelect() {
  int i = server.arg("i").toInt();
  if (i < -1 || i >= itemCount) { sendJsonErr("その品目はありません"); return; }
  curItem = i;
  saveItems();
  sendJsonOk();
}

void handleDelete() {
  int i = server.arg("i").toInt();
  if (i < 0 || i >= itemCount) { sendJsonErr("その品目はありません"); return; }
  for (int k = i; k < itemCount - 1; k++) items[k] = items[k + 1];
  itemCount--;
  if (curItem == i) curItem = -1;
  else if (curItem > i) curItem--;
  saveItems();
  sendJsonOk();
}

void handleSlot() {
  int s = server.arg("s").toInt();
  if (s < 0 || s > 1) { sendJsonErr("スロットは0か1です"); return; }
  cur = s;
  winFill = 0; winPos = 0;  // 平均をやり直す
  saveSlots();
  sendJsonOk(String(slots[cur].label) + " に切り替えました");
}

void handleAutoZero() {
  autoZero = server.arg("on") == "1";
  saveSlots();
  sendJsonOk();
}

// 今の数を記録する（日時,品目,個数,重さ）
void handleLog() {
  if (curItem < 0) { sendJsonErr("品目を選んでください"); return; }
  if (!stable) { sendJsonErr("値が落ち着いてから押してください"); return; }
  long c = lround(weightG / items[curItem].unit);
  bool fresh = !LittleFS.exists("/log.csv");
  File f = LittleFS.open("/log.csv", FILE_APPEND);
  if (f && fresh) f.print("日時,品目,個数,重さg\n");
  if (!f) { sendJsonErr("記録ファイルを開けませんでした"); return; }
  f.printf("%s,%s,%ld,%.3f\n", nowText().c_str(), items[curItem].name.c_str(), c, weightG);
  f.close();
  sendJsonOk(items[curItem].name + " " + String(c) + " 個を記録しました");
}

void handleLogCsv() {
  File f = LittleFS.open("/log.csv", FILE_READ);
  if (!f) { server.send(200, "text/csv; charset=utf-8", "日時,品目,個数,重さg\n"); return; }
  server.streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

extern const char INDEX_HTML[] PROGMEM;
void handleIndex() { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

void setupWeb() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/tare", HTTP_POST, handleTare);
  server.on("/api/cal", HTTP_POST, handleCal);
  server.on("/api/learn", HTTP_POST, handleLearn);
  server.on("/api/select", HTTP_POST, handleSelect);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/api/slot", HTTP_POST, handleSlot);
  server.on("/api/autozero", HTTP_POST, handleAutoZero);
  server.on("/api/log", HTTP_POST, handleLog);
  server.on("/log.csv", HTTP_GET, handleLogCsv);
  server.on("/api/unit", HTTP_POST, handleUnit);
  server.on("/take.csv", HTTP_GET, handleTakeCsv);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("pillscale");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    led(0, 0, 40); delay(250); led(0, 0, 0); delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("# Wi-Fi OK: http://%s/  (http://pillscale.local/)\n", WiFi.localIP().toString().c_str());
    configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("PillScale", SETUP_AP_PASSWORD);
    Serial.printf("# Wi-Fi NG. AP mode: SSID=PillScale  http://%s/\n", WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin("pillscale")) MDNS.addService("http", "tcp", 80);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  led(20, 20, 20);
  delay(300);
  Serial.println("# pill-scale start");
  Serial.println("ms,raw,weight_g");

  if (!LittleFS.begin(true)) Serial.println("# LittleFS NG");
  loadAll();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  setupWifi();
  setupWeb();
}

void loop() {
  server.handleClient();

  uint32_t now = millis();
  if (now - lastSampleMs >= SAMPLE_MS) {
    lastSampleMs = now;
    sample();
    if (!sensorOk) led(40, 0, 0);
    else if (!slots[cur].calibrated) led(40, 20, 0);
    else if (stable) led(0, 30, 0);
    else led(0, 0, 30);
  }

  if (now - lastHistMs >= 1000) {
    lastHistMs = now;
    hist[histPos] = weightG;
    histPos = (histPos + 1) % HIST;
    if (histFill < HIST) histFill++;
  }

  // 本体ボタン: 押したらゼロに合わせる
  static bool prev = true;
  bool b = digitalRead(PIN_BTN);
  if (prev && !b && winFill == WIN) {
    slots[cur].offset = (int32_t)lround(meanRaw);
    saveSlots();
    Serial.println("# tare by button");
  }
  prev = b;
}
