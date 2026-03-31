// M5StickC Plus 1.1 single-file lightweight firmware prototype
// Target: Arduino framework / ESP32
// Build hint: arduino-cli compile --fqbn esp32:esp32:m5stick-c-plus .
// This file intentionally keeps all logic in one file as requested.

#include <M5StickCPlus.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_spi_flash.h>
#include <vector>
#include <functional>
#include <math.h>

static constexpr uint16_t COLOR_BG = 0x0000;
static constexpr uint16_t COLOR_PANEL = 0x1082;
static constexpr uint16_t COLOR_ACCENT = 0x07FF;
static constexpr uint16_t COLOR_TEXT = 0xFFFF;
static constexpr uint16_t COLOR_SUBT = 0x7BEF;
static constexpr uint16_t COLOR_WARN = 0xFD20;

enum ViewId { VIEW_BOOT = 0, VIEW_HOME, VIEW_INFO, VIEW_APPS, VIEW_ENGINE, VIEW_SETTINGS };

struct SystemInfo {
  String chip;
  String sdk;
  uint32_t cpuMhz;
  uint32_t heap;
  uint32_t flashSize;
  uint8_t battery;
  String ip;
};

struct AppEntry {
  String name;
  String description;
  std::function<void()> run;
};

struct EngineVar { String key; int32_t value; };

class MiniEngine {
 public:
  MiniEngine() : pc_(0), halted_(false), lastResult_(0) {}

  void reset() {
    program_.clear();
    vars_.clear();
    pc_ = 0;
    halted_ = false;
    lastResult_ = 0;
  }

  bool load(const String &script) {
    reset();
    int start = 0;
    while (start < script.length()) {
      int end = script.indexOf("\n", start);
      if (end < 0) end = script.length();
      String line = script.substring(start, end);
      line.trim();
      if (line.length() && !line.startsWith("#")) program_.push_back(line);
      start = end + 1;
    }
    return program_.size() > 0;
  }

  bool step() {
    if (halted_ || pc_ >= program_.size()) { halted_ = true; return false; }
    String op = program_[pc_++];
    executeLine(op);
    return !halted_;
  }

  void run(uint16_t maxSteps = 256) {
    uint16_t i = 0;
    while (!halted_ && i < maxSteps) { step(); ++i; }
  }

  int32_t lastResult() const { return lastResult_; }

  String dumpVars() const {
    String out;
    for (size_t i = 0; i < vars_.size(); ++i) {
      out += vars_[i].key + "=" + String(vars_[i].value);
      if (i + 1 < vars_.size()) out += " ";
    }
    return out;
  }

 private:
  std::vector<String> program_;
  std::vector<EngineVar> vars_;
  size_t pc_;
  bool halted_;
  int32_t lastResult_;

  int findVar(const String &k) const {
    for (size_t i = 0; i < vars_.size(); ++i) if (vars_[i].key == k) return (int)i;
    return -1;
  }

  int32_t getValue(const String &token) {
    if (!token.length()) return 0;
    bool numeric = true;
    for (size_t i = 0; i < token.length(); ++i) {
      char c = token[i];
      if ((c < 48 || c > 57) && !(i == 0 && (c == 45 || c == 43))) { numeric = false; break; }
    }
    if (numeric) return token.toInt();
    int idx = findVar(token);
    return idx >= 0 ? vars_[idx].value : 0;
  }

  void setVar(const String &k, int32_t v) {
    int idx = findVar(k);
    if (idx >= 0) vars_[idx].value = v;
    else vars_.push_back({k, v});
  }

  void executeLine(const String &line) {
    int s1 = line.indexOf(" ");
    String cmd = s1 < 0 ? line : line.substring(0, s1);
    String rest = s1 < 0 ? "" : line.substring(s1 + 1);
    cmd.toUpperCase();
    if (cmd == "HALT") { halted_ = true; return; }
    if (cmd == "SET") {
      int sp = rest.indexOf(" ");
      if (sp > 0) { String k = rest.substring(0, sp); String v = rest.substring(sp + 1); setVar(k, getValue(v)); }
      return;
    }
    if (cmd == "ADD" || cmd == "SUB" || cmd == "MUL" || cmd == "DIV") {
      int sp1 = rest.indexOf(" ");
      int sp2 = rest.indexOf(" ", sp1 + 1);
      if (sp1 > 0 && sp2 > sp1) {
        String lhs = rest.substring(0, sp1);
        String a = rest.substring(sp1 + 1, sp2);
        String b = rest.substring(sp2 + 1);
        int32_t av = getValue(a), bv = getValue(b), r = 0;
        if (cmd == "ADD") r = av + bv;
        if (cmd == "SUB") r = av - bv;
        if (cmd == "MUL") r = av * bv;
        if (cmd == "DIV") r = bv == 0 ? 0 : av / bv;
        setVar(lhs, r); lastResult_ = r;
      }
      return;
    }
    if (cmd == "SLEEP") { int ms = getValue(rest); delay(ms); return; }
    if (cmd == "BEEP") { M5.Beep.tone(1800, 70); return; }
    if (cmd == "PRINT") { Serial.println(rest); return; }
    if (cmd == "IFGT") {
      int sp1 = rest.indexOf(" ");
      int sp2 = rest.indexOf(" ", sp1 + 1);
      if (sp1 > 0 && sp2 > sp1) {
        String a = rest.substring(0, sp1);
        String b = rest.substring(sp1 + 1, sp2);
        int32_t jump = getValue(rest.substring(sp2 + 1));
        if (getValue(a) > getValue(b)) {
          if (jump >= 0 && (size_t)jump < program_.size()) pc_ = jump;
        }
      }
      return;
    }
  }
};

class FirmwareUI {
 public:
  void begin() {
    view_ = VIEW_BOOT;
    selected_ = 0;
    scroll_ = 0;
    bootStart_ = millis();
    prepareApps();
    renderBoot();
  }

  void update() {
    M5.update();
    if (view_ == VIEW_BOOT && millis() - bootStart_ > 1800) { view_ = VIEW_HOME; drawCurrent(); }
    handleButtons();
  }

 private:
  ViewId view_ = VIEW_BOOT;
  int selected_ = 0;
  int scroll_ = 0;
  uint32_t bootStart_ = 0;
  SystemInfo info_;
  std::vector<AppEntry> apps_;
  MiniEngine engine_;

  void prepareApps() {
    apps_.clear();
    apps_.push_back({"Neofetch", "Show device summary", [this]() { renderInfoPanel(); }});
    apps_.push_back({"Scanner", "Lightweight placeholder scanner", [this]() { renderScanner(); }});
    apps_.push_back({"Packets", "Simple packet monitor UI", [this]() { renderPackets(); }});
    apps_.push_back({"Engine", "Run tiny script engine", [this]() { runDemoScript(); }});
    apps_.push_back({"About", "Build and capability notes", [this]() { renderAbout(); }});
  }

  void readSystemInfo() {
    info_.chip = String(ESP.getChipModel());
    info_.sdk = String(ESP.getSdkVersion());
    info_.cpuMhz = ESP.getCpuFreqMHz();
    info_.heap = ESP.getFreeHeap();
    info_.flashSize = ESP.getFlashChipSize();
    info_.battery = M5.Axp.GetBatteryLevel();
    info_.ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("offline");
  }

  void drawHeader(const String &title) {
    M5.Lcd.fillScreen(COLOR_BG);
    M5.Lcd.fillRect(0, 0, 240, 20, COLOR_PANEL);
    M5.Lcd.setTextColor(COLOR_ACCENT, COLOR_PANEL);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(6, 6);
    M5.Lcd.print(title);
    M5.Lcd.setCursor(178, 6);
    M5.Lcd.setTextColor(COLOR_SUBT, COLOR_PANEL);
    M5.Lcd.print(String(M5.Axp.GetBatteryLevel()) + "%");
  }

  void renderBoot() {
    M5.Lcd.fillScreen(COLOR_BG);
    M5.Lcd.setTextColor(COLOR_ACCENT, COLOR_BG);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(18, 32);
    M5.Lcd.print("M5 Core LiteOS");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(COLOR_SUBT, COLOR_BG);
    M5.Lcd.setCursor(22, 62);
    M5.Lcd.print("inspired minimal security UI");
    for (int i = 0; i < 200; i += 12) {
      M5.Lcd.drawRect(20 + i / 2, 90, 10, 8, COLOR_PANEL);
      delay(18);
    }
  }

  void drawCurrent() {
    if (view_ == VIEW_HOME) renderHome();
    if (view_ == VIEW_INFO) renderInfoPanel();
    if (view_ == VIEW_APPS) renderApps();
    if (view_ == VIEW_ENGINE) renderEngineView();
    if (view_ == VIEW_SETTINGS) renderSettings();
  }

  void renderHome() {
    drawHeader("HOME");
    const char* menu[] = {"System", "Apps", "Engine", "Settings"};
    int total = 4;
    for (int i = 0; i < total; ++i) {
      int y = 28 + i * 24;
      uint16_t bg = (i == selected_) ? COLOR_PANEL : COLOR_BG;
      uint16_t fg = (i == selected_) ? COLOR_ACCENT : COLOR_TEXT;
      M5.Lcd.fillRect(10, y, 220, 20, bg);
      M5.Lcd.setTextColor(fg, bg);
      M5.Lcd.setCursor(18, y + 6);
      M5.Lcd.print(menu[i]);
    }
    M5.Lcd.setTextColor(COLOR_SUBT, COLOR_BG);
    M5.Lcd.setCursor(8, 126);
    M5.Lcd.print("A:down B:select A+B:back");
  }

  void renderInfoPanel() {
    readSystemInfo();
    drawHeader("NEOFETCH");
    M5.Lcd.setTextColor(COLOR_ACCENT, COLOR_BG);
    const char* art[] = {
      "    __  __  ____      ",
      "   /  \\/  \\/ ___|     ",
      "  / /\\  /\\\\__ \\     ",
      " / /  \\/  \\___) |    ",
      " \\/        \\____/     "
    };
    for (int i = 0; i < 5; ++i) { M5.Lcd.setCursor(6, 28 + i * 10); M5.Lcd.print(art[i]); }
    int x = 124;
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Lcd.setCursor(x, 26); M5.Lcd.print("m5stickc+");
    M5.Lcd.setCursor(x, 38); M5.Lcd.print("chip: " + info_.chip);
    M5.Lcd.setCursor(x, 50); M5.Lcd.print("cpu : " + String(info_.cpuMhz) + "MHz");
    M5.Lcd.setCursor(x, 62); M5.Lcd.print("heap: " + String(info_.heap / 1024) + "KB");
    M5.Lcd.setCursor(x, 74); M5.Lcd.print("flash:" + String(info_.flashSize / (1024 * 1024)) + "MB");
    M5.Lcd.setCursor(x, 86); M5.Lcd.print("bat : " + String(info_.battery) + "%");
    M5.Lcd.setCursor(x, 98); M5.Lcd.print("ip  : " + info_.ip);
    M5.Lcd.setCursor(10, 124);
    M5.Lcd.setTextColor(COLOR_SUBT, COLOR_BG);
    M5.Lcd.print("ASCII neofetch / Button B to return");
  }

  void renderApps() {
    drawHeader("APPS");
    int viewCount = 4;
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + viewCount) scroll_ = selected_ - viewCount + 1;
    for (int i = 0; i < viewCount; ++i) {
      int idx = scroll_ + i;
      if (idx >= (int)apps_.size()) break;
      int y = 26 + i * 24;
      bool sel = idx == selected_;
      M5.Lcd.fillRect(6, y, 228, 21, sel ? COLOR_PANEL : COLOR_BG);
      M5.Lcd.setTextColor(sel ? COLOR_ACCENT : COLOR_TEXT, sel ? COLOR_PANEL : COLOR_BG);
      M5.Lcd.setCursor(10, y + 4);
      M5.Lcd.print(apps_[idx].name);
      M5.Lcd.setTextColor(COLOR_SUBT, sel ? COLOR_PANEL : COLOR_BG);
      M5.Lcd.setCursor(80, y + 4);
      M5.Lcd.print(apps_[idx].description);
    }
  }

  void renderEngineView() {
    drawHeader("ENGINE");
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Lcd.setCursor(8, 28);
    M5.Lcd.print("Tiny script VM (C++ core)");
    M5.Lcd.setCursor(8, 42);
    M5.Lcd.print("Ops: SET ADD SUB MUL DIV");
    M5.Lcd.setCursor(8, 56);
    M5.Lcd.print("Flow: IFGT HALT SLEEP BEEP");
    M5.Lcd.setCursor(8, 70);
    M5.Lcd.print("Press B to run demo script");
    M5.Lcd.setCursor(8, 90);
    M5.Lcd.setTextColor(COLOR_ACCENT, COLOR_BG);
    M5.Lcd.print("vars: " + engine_.dumpVars());
    M5.Lcd.setCursor(8, 110);
    M5.Lcd.setTextColor(COLOR_SUBT, COLOR_BG);
    M5.Lcd.print("Result: " + String(engine_.lastResult()));
  }

  void renderSettings() {
    drawHeader("SETTINGS");
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Lcd.setCursor(8, 30); M5.Lcd.print("- UI theme: dark cyan");
    M5.Lcd.setCursor(8, 44); M5.Lcd.print("- Build target: <=1MB advised");
    M5.Lcd.setCursor(8, 58); M5.Lcd.print("- Disable unused libs in Arduino");
    M5.Lcd.setCursor(8, 72); M5.Lcd.print("- Use release + LTO");
    M5.Lcd.setCursor(8, 86); M5.Lcd.print("- Core app set is minimal");
  }

  void renderScanner() {
    drawHeader("SCANNER");
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    for (int i = 0; i < 8; ++i) {
      M5.Lcd.fillRect(12, 28 + i * 12, 180, 8, COLOR_PANEL);
      M5.Lcd.fillRect(12, 28 + i * 12, (i * 23) % 180, 8, COLOR_ACCENT);
      M5.Lcd.setCursor(198, 28 + i * 12);
      M5.Lcd.setTextColor(COLOR_SUBT, COLOR_BG);
      M5.Lcd.print(String((i * 13) % 100) + "%");
    }
    M5.Lcd.setCursor(8, 126); M5.Lcd.print("Demo only: lightweight placeholder");
  }

  void renderPackets() {
    drawHeader("PACKETS");
    for (int i = 0; i < 9; ++i) {
      int y = 24 + i * 11;
      M5.Lcd.setTextColor((i % 2) ? COLOR_TEXT : COLOR_SUBT, COLOR_BG);
      M5.Lcd.setCursor(8, y);
      M5.Lcd.print(String(i) + "  RSSI=" + String(-34 - i * 3) + "  ch=" + String((i % 11) + 1));
    }
    M5.Lcd.setTextColor(COLOR_WARN, COLOR_BG);
    M5.Lcd.setCursor(8, 126);
    M5.Lcd.print("monitor-only stub");
  }

  void renderAbout() {
    drawHeader("ABOUT");
    M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);
    M5.Lcd.setCursor(8, 26); M5.Lcd.print("Single-file firmware demo");
    M5.Lcd.setCursor(8, 38); M5.Lcd.print("Designed for M5StickC Plus 1.1");
    M5.Lcd.setCursor(8, 50); M5.Lcd.print("UI + Neofetch + VM engine");
    M5.Lcd.setCursor(8, 62); M5.Lcd.print("Not full Bruce parity");
    M5.Lcd.setCursor(8, 74); M5.Lcd.print("Expandable app table included");
  }

  void runDemoScript() {
    String script =
      "SET a 6\n"
      "SET b 7\n"
      "MUL ans a b\n"
      "PRINT engine_answer\n"
      "BEEP\n"
      "HALT\n";
    engine_.load(script);
    engine_.run();
    renderEngineView();
  }

  void enterFromHome() {
    if (selected_ == 0) { view_ = VIEW_INFO; }
    if (selected_ == 1) { view_ = VIEW_APPS; selected_ = 0; scroll_ = 0; }
    if (selected_ == 2) { view_ = VIEW_ENGINE; }
    if (selected_ == 3) { view_ = VIEW_SETTINGS; }
    drawCurrent();
  }

  void handleButtons() {
    if (M5.BtnA.wasPressed() && M5.BtnB.wasPressed()) {
      view_ = VIEW_HOME; selected_ = 0; scroll_ = 0; drawCurrent(); return;
    }

    if (view_ == VIEW_HOME) {
      if (M5.BtnA.wasPressed()) { selected_ = (selected_ + 1) % 4; renderHome(); }
      if (M5.BtnB.wasPressed()) enterFromHome();
      return;
    }

    if (view_ == VIEW_INFO || view_ == VIEW_SETTINGS) {
      if (M5.BtnB.wasPressed()) { view_ = VIEW_HOME; selected_ = 0; drawCurrent(); }
      return;
    }

    if (view_ == VIEW_ENGINE) {
      if (M5.BtnB.wasPressed()) { runDemoScript(); }
      if (M5.BtnA.wasPressed()) { view_ = VIEW_HOME; drawCurrent(); }
      return;
    }

    if (view_ == VIEW_APPS) {
      if (M5.BtnA.wasPressed()) { selected_ = (selected_ + 1) % apps_.size(); renderApps(); }
      if (M5.BtnB.wasPressed()) { apps_[selected_].run(); }
      return;
    }
  }
};

FirmwareUI g_ui;

struct Capability { const char* id; const char* note; int risk; };
static Capability g_caps[] = {
  {"cap_001", "modular slot 1", 1},
  {"cap_002", "modular slot 2", 2},
  {"cap_003", "modular slot 3", 3},
  {"cap_004", "modular slot 4", 4},
  {"cap_005", "modular slot 5", 0},
  {"cap_006", "modular slot 6", 1},
  {"cap_007", "modular slot 7", 2},
  {"cap_008", "modular slot 8", 3},
  {"cap_009", "modular slot 9", 4},
  {"cap_010", "modular slot 10", 0},
  {"cap_011", "modular slot 11", 1},
  {"cap_012", "modular slot 12", 2},
  {"cap_013", "modular slot 13", 3},
  {"cap_014", "modular slot 14", 4},
  {"cap_015", "modular slot 15", 0},
  {"cap_016", "modular slot 16", 1},
  {"cap_017", "modular slot 17", 2},
  {"cap_018", "modular slot 18", 3},
  {"cap_019", "modular slot 19", 4},
  {"cap_020", "modular slot 20", 0},
  {"cap_021", "modular slot 21", 1},
  {"cap_022", "modular slot 22", 2},
  {"cap_023", "modular slot 23", 3},
  {"cap_024", "modular slot 24", 4},
  {"cap_025", "modular slot 25", 0},
  {"cap_026", "modular slot 26", 1},
  {"cap_027", "modular slot 27", 2},
  {"cap_028", "modular slot 28", 3},
  {"cap_029", "modular slot 29", 4},
  {"cap_030", "modular slot 30", 0},
  {"cap_031", "modular slot 31", 1},
  {"cap_032", "modular slot 32", 2},
  {"cap_033", "modular slot 33", 3},
  {"cap_034", "modular slot 34", 4},
  {"cap_035", "modular slot 35", 0},
  {"cap_036", "modular slot 36", 1},
  {"cap_037", "modular slot 37", 2},
  {"cap_038", "modular slot 38", 3},
  {"cap_039", "modular slot 39", 4},
  {"cap_040", "modular slot 40", 0},
  {"cap_041", "modular slot 41", 1},
  {"cap_042", "modular slot 42", 2},
  {"cap_043", "modular slot 43", 3},
  {"cap_044", "modular slot 44", 4},
  {"cap_045", "modular slot 45", 0},
  {"cap_046", "modular slot 46", 1},
  {"cap_047", "modular slot 47", 2},
  {"cap_048", "modular slot 48", 3},
  {"cap_049", "modular slot 49", 4},
  {"cap_050", "modular slot 50", 0},
  {"cap_051", "modular slot 51", 1},
  {"cap_052", "modular slot 52", 2},
  {"cap_053", "modular slot 53", 3},
  {"cap_054", "modular slot 54", 4},
  {"cap_055", "modular slot 55", 0},
  {"cap_056", "modular slot 56", 1},
  {"cap_057", "modular slot 57", 2},
  {"cap_058", "modular slot 58", 3},
  {"cap_059", "modular slot 59", 4},
  {"cap_060", "modular slot 60", 0},
  {"cap_061", "modular slot 61", 1},
  {"cap_062", "modular slot 62", 2},
  {"cap_063", "modular slot 63", 3},
  {"cap_064", "modular slot 64", 4},
  {"cap_065", "modular slot 65", 0},
  {"cap_066", "modular slot 66", 1},
  {"cap_067", "modular slot 67", 2},
  {"cap_068", "modular slot 68", 3},
  {"cap_069", "modular slot 69", 4},
  {"cap_070", "modular slot 70", 0},
  {"cap_071", "modular slot 71", 1},
  {"cap_072", "modular slot 72", 2},
  {"cap_073", "modular slot 73", 3},
  {"cap_074", "modular slot 74", 4},
  {"cap_075", "modular slot 75", 0},
  {"cap_076", "modular slot 76", 1},
  {"cap_077", "modular slot 77", 2},
  {"cap_078", "modular slot 78", 3},
  {"cap_079", "modular slot 79", 4},
  {"cap_080", "modular slot 80", 0},
  {"cap_081", "modular slot 81", 1},
  {"cap_082", "modular slot 82", 2},
  {"cap_083", "modular slot 83", 3},
  {"cap_084", "modular slot 84", 4},
  {"cap_085", "modular slot 85", 0},
  {"cap_086", "modular slot 86", 1},
  {"cap_087", "modular slot 87", 2},
  {"cap_088", "modular slot 88", 3},
  {"cap_089", "modular slot 89", 4},
  {"cap_090", "modular slot 90", 0},
  {"cap_091", "modular slot 91", 1},
  {"cap_092", "modular slot 92", 2},
  {"cap_093", "modular slot 93", 3},
  {"cap_094", "modular slot 94", 4},
  {"cap_095", "modular slot 95", 0},
  {"cap_096", "modular slot 96", 1},
  {"cap_097", "modular slot 97", 2},
  {"cap_098", "modular slot 98", 3},
  {"cap_099", "modular slot 99", 4},
  {"cap_100", "modular slot 100", 0},
  {"cap_101", "modular slot 101", 1},
  {"cap_102", "modular slot 102", 2},
  {"cap_103", "modular slot 103", 3},
  {"cap_104", "modular slot 104", 4},
  {"cap_105", "modular slot 105", 0},
  {"cap_106", "modular slot 106", 1},
  {"cap_107", "modular slot 107", 2},
  {"cap_108", "modular slot 108", 3},
  {"cap_109", "modular slot 109", 4},
  {"cap_110", "modular slot 110", 0},
  {"cap_111", "modular slot 111", 1},
  {"cap_112", "modular slot 112", 2},
  {"cap_113", "modular slot 113", 3},
  {"cap_114", "modular slot 114", 4},
  {"cap_115", "modular slot 115", 0},
  {"cap_116", "modular slot 116", 1},
  {"cap_117", "modular slot 117", 2},
  {"cap_118", "modular slot 118", 3},
  {"cap_119", "modular slot 119", 4},
  {"cap_120", "modular slot 120", 0},
  {"cap_121", "modular slot 121", 1},
  {"cap_122", "modular slot 122", 2},
  {"cap_123", "modular slot 123", 3},
  {"cap_124", "modular slot 124", 4},
  {"cap_125", "modular slot 125", 0},
  {"cap_126", "modular slot 126", 1},
  {"cap_127", "modular slot 127", 2},
  {"cap_128", "modular slot 128", 3},
  {"cap_129", "modular slot 129", 4},
  {"cap_130", "modular slot 130", 0},
  {"cap_131", "modular slot 131", 1},
  {"cap_132", "modular slot 132", 2},
  {"cap_133", "modular slot 133", 3},
  {"cap_134", "modular slot 134", 4},
  {"cap_135", "modular slot 135", 0},
  {"cap_136", "modular slot 136", 1},
  {"cap_137", "modular slot 137", 2},
  {"cap_138", "modular slot 138", 3},
  {"cap_139", "modular slot 139", 4},
  {"cap_140", "modular slot 140", 0},
  {"cap_141", "modular slot 141", 1},
  {"cap_142", "modular slot 142", 2},
  {"cap_143", "modular slot 143", 3},
  {"cap_144", "modular slot 144", 4},
  {"cap_145", "modular slot 145", 0},
  {"cap_146", "modular slot 146", 1},
  {"cap_147", "modular slot 147", 2},
  {"cap_148", "modular slot 148", 3},
  {"cap_149", "modular slot 149", 4},
  {"cap_150", "modular slot 150", 0},
  {"cap_151", "modular slot 151", 1},
  {"cap_152", "modular slot 152", 2},
  {"cap_153", "modular slot 153", 3},
  {"cap_154", "modular slot 154", 4},
  {"cap_155", "modular slot 155", 0},
  {"cap_156", "modular slot 156", 1},
  {"cap_157", "modular slot 157", 2},
  {"cap_158", "modular slot 158", 3},
  {"cap_159", "modular slot 159", 4},
  {"cap_160", "modular slot 160", 0},
  {"cap_161", "modular slot 161", 1},
  {"cap_162", "modular slot 162", 2},
  {"cap_163", "modular slot 163", 3},
  {"cap_164", "modular slot 164", 4},
  {"cap_165", "modular slot 165", 0},
  {"cap_166", "modular slot 166", 1},
  {"cap_167", "modular slot 167", 2},
  {"cap_168", "modular slot 168", 3},
  {"cap_169", "modular slot 169", 4},
  {"cap_170", "modular slot 170", 0},
  {"cap_171", "modular slot 171", 1},
  {"cap_172", "modular slot 172", 2},
  {"cap_173", "modular slot 173", 3},
  {"cap_174", "modular slot 174", 4},
  {"cap_175", "modular slot 175", 0},
  {"cap_176", "modular slot 176", 1},
  {"cap_177", "modular slot 177", 2},
  {"cap_178", "modular slot 178", 3},
  {"cap_179", "modular slot 179", 4},
  {"cap_180", "modular slot 180", 0},
  {"cap_181", "modular slot 181", 1},
  {"cap_182", "modular slot 182", 2},
  {"cap_183", "modular slot 183", 3},
  {"cap_184", "modular slot 184", 4},
  {"cap_185", "modular slot 185", 0},
  {"cap_186", "modular slot 186", 1},
  {"cap_187", "modular slot 187", 2},
  {"cap_188", "modular slot 188", 3},
  {"cap_189", "modular slot 189", 4},
  {"cap_190", "modular slot 190", 0},
  {"cap_191", "modular slot 191", 1},
  {"cap_192", "modular slot 192", 2},
  {"cap_193", "modular slot 193", 3},
  {"cap_194", "modular slot 194", 4},
  {"cap_195", "modular slot 195", 0},
  {"cap_196", "modular slot 196", 1},
  {"cap_197", "modular slot 197", 2},
  {"cap_198", "modular slot 198", 3},
  {"cap_199", "modular slot 199", 4},
  {"cap_200", "modular slot 200", 0},
  {"cap_201", "modular slot 201", 1},
  {"cap_202", "modular slot 202", 2},
  {"cap_203", "modular slot 203", 3},
  {"cap_204", "modular slot 204", 4},
  {"cap_205", "modular slot 205", 0},
  {"cap_206", "modular slot 206", 1},
  {"cap_207", "modular slot 207", 2},
  {"cap_208", "modular slot 208", 3},
  {"cap_209", "modular slot 209", 4},
  {"cap_210", "modular slot 210", 0},
  {"cap_211", "modular slot 211", 1},
  {"cap_212", "modular slot 212", 2},
  {"cap_213", "modular slot 213", 3},
  {"cap_214", "modular slot 214", 4},
  {"cap_215", "modular slot 215", 0},
  {"cap_216", "modular slot 216", 1},
  {"cap_217", "modular slot 217", 2},
  {"cap_218", "modular slot 218", 3},
  {"cap_219", "modular slot 219", 4},
  {"cap_220", "modular slot 220", 0},
  {"cap_221", "modular slot 221", 1},
  {"cap_222", "modular slot 222", 2},
  {"cap_223", "modular slot 223", 3},
  {"cap_224", "modular slot 224", 4},
  {"cap_225", "modular slot 225", 0},
  {"cap_226", "modular slot 226", 1},
  {"cap_227", "modular slot 227", 2},
  {"cap_228", "modular slot 228", 3},
  {"cap_229", "modular slot 229", 4},
  {"cap_230", "modular slot 230", 0},
  {"cap_231", "modular slot 231", 1},
  {"cap_232", "modular slot 232", 2},
  {"cap_233", "modular slot 233", 3},
  {"cap_234", "modular slot 234", 4},
  {"cap_235", "modular slot 235", 0},
  {"cap_236", "modular slot 236", 1},
  {"cap_237", "modular slot 237", 2},
  {"cap_238", "modular slot 238", 3},
  {"cap_239", "modular slot 239", 4},
  {"cap_240", "modular slot 240", 0},
  {"cap_241", "modular slot 241", 1},
  {"cap_242", "modular slot 242", 2},
  {"cap_243", "modular slot 243", 3},
  {"cap_244", "modular slot 244", 4},
  {"cap_245", "modular slot 245", 0},
  {"cap_246", "modular slot 246", 1},
  {"cap_247", "modular slot 247", 2},
  {"cap_248", "modular slot 248", 3},
  {"cap_249", "modular slot 249", 4},
  {"cap_250", "modular slot 250", 0},
  {"cap_251", "modular slot 251", 1},
  {"cap_252", "modular slot 252", 2},
  {"cap_253", "modular slot 253", 3},
  {"cap_254", "modular slot 254", 4},
  {"cap_255", "modular slot 255", 0},
  {"cap_256", "modular slot 256", 1},
  {"cap_257", "modular slot 257", 2},
  {"cap_258", "modular slot 258", 3},
  {"cap_259", "modular slot 259", 4},
  {"cap_260", "modular slot 260", 0},
  {"cap_261", "modular slot 261", 1},
  {"cap_262", "modular slot 262", 2},
  {"cap_263", "modular slot 263", 3},
  {"cap_264", "modular slot 264", 4},
  {"cap_265", "modular slot 265", 0},
  {"cap_266", "modular slot 266", 1},
  {"cap_267", "modular slot 267", 2},
  {"cap_268", "modular slot 268", 3},
  {"cap_269", "modular slot 269", 4},
  {"cap_270", "modular slot 270", 0},
  {"cap_271", "modular slot 271", 1},
  {"cap_272", "modular slot 272", 2},
  {"cap_273", "modular slot 273", 3},
  {"cap_274", "modular slot 274", 4},
  {"cap_275", "modular slot 275", 0},
  {"cap_276", "modular slot 276", 1},
  {"cap_277", "modular slot 277", 2},
  {"cap_278", "modular slot 278", 3},
  {"cap_279", "modular slot 279", 4},
  {"cap_280", "modular slot 280", 0},
  {"cap_281", "modular slot 281", 1},
  {"cap_282", "modular slot 282", 2},
  {"cap_283", "modular slot 283", 3},
  {"cap_284", "modular slot 284", 4},
  {"cap_285", "modular slot 285", 0},
  {"cap_286", "modular slot 286", 1},
  {"cap_287", "modular slot 287", 2},
  {"cap_288", "modular slot 288", 3},
  {"cap_289", "modular slot 289", 4},
  {"cap_290", "modular slot 290", 0},
  {"cap_291", "modular slot 291", 1},
  {"cap_292", "modular slot 292", 2},
  {"cap_293", "modular slot 293", 3},
  {"cap_294", "modular slot 294", 4},
  {"cap_295", "modular slot 295", 0},
  {"cap_296", "modular slot 296", 1},
  {"cap_297", "modular slot 297", 2},
  {"cap_298", "modular slot 298", 3},
  {"cap_299", "modular slot 299", 4},
  {"cap_300", "modular slot 300", 0},
};

int32_t runCapability(int id, int32_t input) {
  switch (id) {
    case 1: { int32_t v = input + 1; v ^= (1 * 17); v += 1; return v; }
    case 2: { int32_t v = input + 2; v ^= (2 * 17); v += 2; return v; }
    case 3: { int32_t v = input + 3; v ^= (3 * 17); v += 3; return v; }
    case 4: { int32_t v = input + 4; v ^= (4 * 17); v += 4; return v; }
    case 5: { int32_t v = input + 5; v ^= (5 * 17); v += 5; return v; }
    case 6: { int32_t v = input + 6; v ^= (6 * 17); v += 6; return v; }
    case 7: { int32_t v = input + 7; v ^= (7 * 17); v += 0; return v; }
    case 8: { int32_t v = input + 8; v ^= (8 * 17); v += 1; return v; }
    case 9: { int32_t v = input + 9; v ^= (9 * 17); v += 2; return v; }
    case 10: { int32_t v = input + 10; v ^= (10 * 17); v += 3; return v; }
    case 11: { int32_t v = input + 11; v ^= (11 * 17); v += 4; return v; }
    case 12: { int32_t v = input + 12; v ^= (12 * 17); v += 5; return v; }
    case 13: { int32_t v = input + 13; v ^= (13 * 17); v += 6; return v; }
    case 14: { int32_t v = input + 14; v ^= (14 * 17); v += 0; return v; }
    case 15: { int32_t v = input + 15; v ^= (15 * 17); v += 1; return v; }
    case 16: { int32_t v = input + 16; v ^= (16 * 17); v += 2; return v; }
    case 17: { int32_t v = input + 17; v ^= (17 * 17); v += 3; return v; }
    case 18: { int32_t v = input + 18; v ^= (18 * 17); v += 4; return v; }
    case 19: { int32_t v = input + 19; v ^= (19 * 17); v += 5; return v; }
    case 20: { int32_t v = input + 20; v ^= (20 * 17); v += 6; return v; }
    case 21: { int32_t v = input + 21; v ^= (21 * 17); v += 0; return v; }
    case 22: { int32_t v = input + 22; v ^= (22 * 17); v += 1; return v; }
    case 23: { int32_t v = input + 23; v ^= (23 * 17); v += 2; return v; }
    case 24: { int32_t v = input + 24; v ^= (24 * 17); v += 3; return v; }
    case 25: { int32_t v = input + 25; v ^= (25 * 17); v += 4; return v; }
    case 26: { int32_t v = input + 26; v ^= (26 * 17); v += 5; return v; }
    case 27: { int32_t v = input + 27; v ^= (27 * 17); v += 6; return v; }
    case 28: { int32_t v = input + 28; v ^= (28 * 17); v += 0; return v; }
    case 29: { int32_t v = input + 29; v ^= (29 * 17); v += 1; return v; }
    case 30: { int32_t v = input + 30; v ^= (30 * 17); v += 2; return v; }
    case 31: { int32_t v = input + 31; v ^= (31 * 17); v += 3; return v; }
    case 32: { int32_t v = input + 32; v ^= (32 * 17); v += 4; return v; }
    case 33: { int32_t v = input + 33; v ^= (33 * 17); v += 5; return v; }
    case 34: { int32_t v = input + 34; v ^= (34 * 17); v += 6; return v; }
    case 35: { int32_t v = input + 35; v ^= (35 * 17); v += 0; return v; }
    case 36: { int32_t v = input + 36; v ^= (36 * 17); v += 1; return v; }
    case 37: { int32_t v = input + 37; v ^= (37 * 17); v += 2; return v; }
    case 38: { int32_t v = input + 38; v ^= (38 * 17); v += 3; return v; }
    case 39: { int32_t v = input + 39; v ^= (39 * 17); v += 4; return v; }
    case 40: { int32_t v = input + 40; v ^= (40 * 17); v += 5; return v; }
    case 41: { int32_t v = input + 41; v ^= (41 * 17); v += 6; return v; }
    case 42: { int32_t v = input + 42; v ^= (42 * 17); v += 0; return v; }
    case 43: { int32_t v = input + 43; v ^= (43 * 17); v += 1; return v; }
    case 44: { int32_t v = input + 44; v ^= (44 * 17); v += 2; return v; }
    case 45: { int32_t v = input + 45; v ^= (45 * 17); v += 3; return v; }
    case 46: { int32_t v = input + 46; v ^= (46 * 17); v += 4; return v; }
    case 47: { int32_t v = input + 47; v ^= (47 * 17); v += 5; return v; }
    case 48: { int32_t v = input + 48; v ^= (48 * 17); v += 6; return v; }
    case 49: { int32_t v = input + 49; v ^= (49 * 17); v += 0; return v; }
    case 50: { int32_t v = input + 50; v ^= (50 * 17); v += 1; return v; }
    case 51: { int32_t v = input + 51; v ^= (51 * 17); v += 2; return v; }
    case 52: { int32_t v = input + 52; v ^= (52 * 17); v += 3; return v; }
    case 53: { int32_t v = input + 53; v ^= (53 * 17); v += 4; return v; }
    case 54: { int32_t v = input + 54; v ^= (54 * 17); v += 5; return v; }
    case 55: { int32_t v = input + 55; v ^= (55 * 17); v += 6; return v; }
    case 56: { int32_t v = input + 56; v ^= (56 * 17); v += 0; return v; }
    case 57: { int32_t v = input + 57; v ^= (57 * 17); v += 1; return v; }
    case 58: { int32_t v = input + 58; v ^= (58 * 17); v += 2; return v; }
    case 59: { int32_t v = input + 59; v ^= (59 * 17); v += 3; return v; }
    case 60: { int32_t v = input + 60; v ^= (60 * 17); v += 4; return v; }
    case 61: { int32_t v = input + 61; v ^= (61 * 17); v += 5; return v; }
    case 62: { int32_t v = input + 62; v ^= (62 * 17); v += 6; return v; }
    case 63: { int32_t v = input + 63; v ^= (63 * 17); v += 0; return v; }
    case 64: { int32_t v = input + 64; v ^= (64 * 17); v += 1; return v; }
    case 65: { int32_t v = input + 65; v ^= (65 * 17); v += 2; return v; }
    case 66: { int32_t v = input + 66; v ^= (66 * 17); v += 3; return v; }
    case 67: { int32_t v = input + 67; v ^= (67 * 17); v += 4; return v; }
    case 68: { int32_t v = input + 68; v ^= (68 * 17); v += 5; return v; }
    case 69: { int32_t v = input + 69; v ^= (69 * 17); v += 6; return v; }
    case 70: { int32_t v = input + 70; v ^= (70 * 17); v += 0; return v; }
    case 71: { int32_t v = input + 71; v ^= (71 * 17); v += 1; return v; }
    case 72: { int32_t v = input + 72; v ^= (72 * 17); v += 2; return v; }
    case 73: { int32_t v = input + 73; v ^= (73 * 17); v += 3; return v; }
    case 74: { int32_t v = input + 74; v ^= (74 * 17); v += 4; return v; }
    case 75: { int32_t v = input + 75; v ^= (75 * 17); v += 5; return v; }
    case 76: { int32_t v = input + 76; v ^= (76 * 17); v += 6; return v; }
    case 77: { int32_t v = input + 77; v ^= (77 * 17); v += 0; return v; }
    case 78: { int32_t v = input + 78; v ^= (78 * 17); v += 1; return v; }
    case 79: { int32_t v = input + 79; v ^= (79 * 17); v += 2; return v; }
    case 80: { int32_t v = input + 80; v ^= (80 * 17); v += 3; return v; }
    case 81: { int32_t v = input + 81; v ^= (81 * 17); v += 4; return v; }
    case 82: { int32_t v = input + 82; v ^= (82 * 17); v += 5; return v; }
    case 83: { int32_t v = input + 83; v ^= (83 * 17); v += 6; return v; }
    case 84: { int32_t v = input + 84; v ^= (84 * 17); v += 0; return v; }
    case 85: { int32_t v = input + 85; v ^= (85 * 17); v += 1; return v; }
    case 86: { int32_t v = input + 86; v ^= (86 * 17); v += 2; return v; }
    case 87: { int32_t v = input + 87; v ^= (87 * 17); v += 3; return v; }
    case 88: { int32_t v = input + 88; v ^= (88 * 17); v += 4; return v; }
    case 89: { int32_t v = input + 89; v ^= (89 * 17); v += 5; return v; }
    case 90: { int32_t v = input + 90; v ^= (90 * 17); v += 6; return v; }
    case 91: { int32_t v = input + 91; v ^= (91 * 17); v += 0; return v; }
    case 92: { int32_t v = input + 92; v ^= (92 * 17); v += 1; return v; }
    case 93: { int32_t v = input + 93; v ^= (93 * 17); v += 2; return v; }
    case 94: { int32_t v = input + 94; v ^= (94 * 17); v += 3; return v; }
    case 95: { int32_t v = input + 95; v ^= (95 * 17); v += 4; return v; }
    case 96: { int32_t v = input + 96; v ^= (96 * 17); v += 5; return v; }
    case 97: { int32_t v = input + 97; v ^= (97 * 17); v += 6; return v; }
    case 98: { int32_t v = input + 98; v ^= (98 * 17); v += 0; return v; }
    case 99: { int32_t v = input + 99; v ^= (99 * 17); v += 1; return v; }
    case 100: { int32_t v = input + 100; v ^= (100 * 17); v += 2; return v; }
    case 101: { int32_t v = input + 101; v ^= (101 * 17); v += 3; return v; }
    case 102: { int32_t v = input + 102; v ^= (102 * 17); v += 4; return v; }
    case 103: { int32_t v = input + 103; v ^= (103 * 17); v += 5; return v; }
    case 104: { int32_t v = input + 104; v ^= (104 * 17); v += 6; return v; }
    case 105: { int32_t v = input + 105; v ^= (105 * 17); v += 0; return v; }
    case 106: { int32_t v = input + 106; v ^= (106 * 17); v += 1; return v; }
    case 107: { int32_t v = input + 107; v ^= (107 * 17); v += 2; return v; }
    case 108: { int32_t v = input + 108; v ^= (108 * 17); v += 3; return v; }
    case 109: { int32_t v = input + 109; v ^= (109 * 17); v += 4; return v; }
    case 110: { int32_t v = input + 110; v ^= (110 * 17); v += 5; return v; }
    case 111: { int32_t v = input + 111; v ^= (111 * 17); v += 6; return v; }
    case 112: { int32_t v = input + 112; v ^= (112 * 17); v += 0; return v; }
    case 113: { int32_t v = input + 113; v ^= (113 * 17); v += 1; return v; }
    case 114: { int32_t v = input + 114; v ^= (114 * 17); v += 2; return v; }
    case 115: { int32_t v = input + 115; v ^= (115 * 17); v += 3; return v; }
    case 116: { int32_t v = input + 116; v ^= (116 * 17); v += 4; return v; }
    case 117: { int32_t v = input + 117; v ^= (117 * 17); v += 5; return v; }
    case 118: { int32_t v = input + 118; v ^= (118 * 17); v += 6; return v; }
    case 119: { int32_t v = input + 119; v ^= (119 * 17); v += 0; return v; }
    case 120: { int32_t v = input + 120; v ^= (120 * 17); v += 1; return v; }
    case 121: { int32_t v = input + 121; v ^= (121 * 17); v += 2; return v; }
    case 122: { int32_t v = input + 122; v ^= (122 * 17); v += 3; return v; }
    case 123: { int32_t v = input + 123; v ^= (123 * 17); v += 4; return v; }
    case 124: { int32_t v = input + 124; v ^= (124 * 17); v += 5; return v; }
    case 125: { int32_t v = input + 125; v ^= (125 * 17); v += 6; return v; }
    case 126: { int32_t v = input + 126; v ^= (126 * 17); v += 0; return v; }
    case 127: { int32_t v = input + 127; v ^= (127 * 17); v += 1; return v; }
    case 128: { int32_t v = input + 128; v ^= (128 * 17); v += 2; return v; }
    case 129: { int32_t v = input + 129; v ^= (129 * 17); v += 3; return v; }
    case 130: { int32_t v = input + 130; v ^= (130 * 17); v += 4; return v; }
    case 131: { int32_t v = input + 131; v ^= (131 * 17); v += 5; return v; }
    case 132: { int32_t v = input + 132; v ^= (132 * 17); v += 6; return v; }
    case 133: { int32_t v = input + 133; v ^= (133 * 17); v += 0; return v; }
    case 134: { int32_t v = input + 134; v ^= (134 * 17); v += 1; return v; }
    case 135: { int32_t v = input + 135; v ^= (135 * 17); v += 2; return v; }
    case 136: { int32_t v = input + 136; v ^= (136 * 17); v += 3; return v; }
    case 137: { int32_t v = input + 137; v ^= (137 * 17); v += 4; return v; }
    case 138: { int32_t v = input + 138; v ^= (138 * 17); v += 5; return v; }
    case 139: { int32_t v = input + 139; v ^= (139 * 17); v += 6; return v; }
    case 140: { int32_t v = input + 140; v ^= (140 * 17); v += 0; return v; }
    case 141: { int32_t v = input + 141; v ^= (141 * 17); v += 1; return v; }
    case 142: { int32_t v = input + 142; v ^= (142 * 17); v += 2; return v; }
    case 143: { int32_t v = input + 143; v ^= (143 * 17); v += 3; return v; }
    case 144: { int32_t v = input + 144; v ^= (144 * 17); v += 4; return v; }
    case 145: { int32_t v = input + 145; v ^= (145 * 17); v += 5; return v; }
    case 146: { int32_t v = input + 146; v ^= (146 * 17); v += 6; return v; }
    case 147: { int32_t v = input + 147; v ^= (147 * 17); v += 0; return v; }
    case 148: { int32_t v = input + 148; v ^= (148 * 17); v += 1; return v; }
    case 149: { int32_t v = input + 149; v ^= (149 * 17); v += 2; return v; }
    case 150: { int32_t v = input + 150; v ^= (150 * 17); v += 3; return v; }
    case 151: { int32_t v = input + 151; v ^= (151 * 17); v += 4; return v; }
    case 152: { int32_t v = input + 152; v ^= (152 * 17); v += 5; return v; }
    case 153: { int32_t v = input + 153; v ^= (153 * 17); v += 6; return v; }
    case 154: { int32_t v = input + 154; v ^= (154 * 17); v += 0; return v; }
    case 155: { int32_t v = input + 155; v ^= (155 * 17); v += 1; return v; }
    case 156: { int32_t v = input + 156; v ^= (156 * 17); v += 2; return v; }
    case 157: { int32_t v = input + 157; v ^= (157 * 17); v += 3; return v; }
    case 158: { int32_t v = input + 158; v ^= (158 * 17); v += 4; return v; }
    case 159: { int32_t v = input + 159; v ^= (159 * 17); v += 5; return v; }
    case 160: { int32_t v = input + 160; v ^= (160 * 17); v += 6; return v; }
    case 161: { int32_t v = input + 161; v ^= (161 * 17); v += 0; return v; }
    case 162: { int32_t v = input + 162; v ^= (162 * 17); v += 1; return v; }
    case 163: { int32_t v = input + 163; v ^= (163 * 17); v += 2; return v; }
    case 164: { int32_t v = input + 164; v ^= (164 * 17); v += 3; return v; }
    case 165: { int32_t v = input + 165; v ^= (165 * 17); v += 4; return v; }
    case 166: { int32_t v = input + 166; v ^= (166 * 17); v += 5; return v; }
    case 167: { int32_t v = input + 167; v ^= (167 * 17); v += 6; return v; }
    case 168: { int32_t v = input + 168; v ^= (168 * 17); v += 0; return v; }
    case 169: { int32_t v = input + 169; v ^= (169 * 17); v += 1; return v; }
    case 170: { int32_t v = input + 170; v ^= (170 * 17); v += 2; return v; }
    case 171: { int32_t v = input + 171; v ^= (171 * 17); v += 3; return v; }
    case 172: { int32_t v = input + 172; v ^= (172 * 17); v += 4; return v; }
    case 173: { int32_t v = input + 173; v ^= (173 * 17); v += 5; return v; }
    case 174: { int32_t v = input + 174; v ^= (174 * 17); v += 6; return v; }
    case 175: { int32_t v = input + 175; v ^= (175 * 17); v += 0; return v; }
    case 176: { int32_t v = input + 176; v ^= (176 * 17); v += 1; return v; }
    case 177: { int32_t v = input + 177; v ^= (177 * 17); v += 2; return v; }
    case 178: { int32_t v = input + 178; v ^= (178 * 17); v += 3; return v; }
    case 179: { int32_t v = input + 179; v ^= (179 * 17); v += 4; return v; }
    case 180: { int32_t v = input + 180; v ^= (180 * 17); v += 5; return v; }
    case 181: { int32_t v = input + 181; v ^= (181 * 17); v += 6; return v; }
    case 182: { int32_t v = input + 182; v ^= (182 * 17); v += 0; return v; }
    case 183: { int32_t v = input + 183; v ^= (183 * 17); v += 1; return v; }
    case 184: { int32_t v = input + 184; v ^= (184 * 17); v += 2; return v; }
    case 185: { int32_t v = input + 185; v ^= (185 * 17); v += 3; return v; }
    case 186: { int32_t v = input + 186; v ^= (186 * 17); v += 4; return v; }
    case 187: { int32_t v = input + 187; v ^= (187 * 17); v += 5; return v; }
    case 188: { int32_t v = input + 188; v ^= (188 * 17); v += 6; return v; }
    case 189: { int32_t v = input + 189; v ^= (189 * 17); v += 0; return v; }
    case 190: { int32_t v = input + 190; v ^= (190 * 17); v += 1; return v; }
    case 191: { int32_t v = input + 191; v ^= (191 * 17); v += 2; return v; }
    case 192: { int32_t v = input + 192; v ^= (192 * 17); v += 3; return v; }
    case 193: { int32_t v = input + 193; v ^= (193 * 17); v += 4; return v; }
    case 194: { int32_t v = input + 194; v ^= (194 * 17); v += 5; return v; }
    case 195: { int32_t v = input + 195; v ^= (195 * 17); v += 6; return v; }
    case 196: { int32_t v = input + 196; v ^= (196 * 17); v += 0; return v; }
    case 197: { int32_t v = input + 197; v ^= (197 * 17); v += 1; return v; }
    case 198: { int32_t v = input + 198; v ^= (198 * 17); v += 2; return v; }
    case 199: { int32_t v = input + 199; v ^= (199 * 17); v += 3; return v; }
    case 200: { int32_t v = input + 200; v ^= (200 * 17); v += 4; return v; }
    case 201: { int32_t v = input + 201; v ^= (201 * 17); v += 5; return v; }
    case 202: { int32_t v = input + 202; v ^= (202 * 17); v += 6; return v; }
    case 203: { int32_t v = input + 203; v ^= (203 * 17); v += 0; return v; }
    case 204: { int32_t v = input + 204; v ^= (204 * 17); v += 1; return v; }
    case 205: { int32_t v = input + 205; v ^= (205 * 17); v += 2; return v; }
    case 206: { int32_t v = input + 206; v ^= (206 * 17); v += 3; return v; }
    case 207: { int32_t v = input + 207; v ^= (207 * 17); v += 4; return v; }
    case 208: { int32_t v = input + 208; v ^= (208 * 17); v += 5; return v; }
    case 209: { int32_t v = input + 209; v ^= (209 * 17); v += 6; return v; }
    case 210: { int32_t v = input + 210; v ^= (210 * 17); v += 0; return v; }
    case 211: { int32_t v = input + 211; v ^= (211 * 17); v += 1; return v; }
    case 212: { int32_t v = input + 212; v ^= (212 * 17); v += 2; return v; }
    case 213: { int32_t v = input + 213; v ^= (213 * 17); v += 3; return v; }
    case 214: { int32_t v = input + 214; v ^= (214 * 17); v += 4; return v; }
    case 215: { int32_t v = input + 215; v ^= (215 * 17); v += 5; return v; }
    case 216: { int32_t v = input + 216; v ^= (216 * 17); v += 6; return v; }
    case 217: { int32_t v = input + 217; v ^= (217 * 17); v += 0; return v; }
    case 218: { int32_t v = input + 218; v ^= (218 * 17); v += 1; return v; }
    case 219: { int32_t v = input + 219; v ^= (219 * 17); v += 2; return v; }
    case 220: { int32_t v = input + 220; v ^= (220 * 17); v += 3; return v; }
    case 221: { int32_t v = input + 221; v ^= (221 * 17); v += 4; return v; }
    case 222: { int32_t v = input + 222; v ^= (222 * 17); v += 5; return v; }
    case 223: { int32_t v = input + 223; v ^= (223 * 17); v += 6; return v; }
    case 224: { int32_t v = input + 224; v ^= (224 * 17); v += 0; return v; }
    case 225: { int32_t v = input + 225; v ^= (225 * 17); v += 1; return v; }
    case 226: { int32_t v = input + 226; v ^= (226 * 17); v += 2; return v; }
    case 227: { int32_t v = input + 227; v ^= (227 * 17); v += 3; return v; }
    case 228: { int32_t v = input + 228; v ^= (228 * 17); v += 4; return v; }
    case 229: { int32_t v = input + 229; v ^= (229 * 17); v += 5; return v; }
    case 230: { int32_t v = input + 230; v ^= (230 * 17); v += 6; return v; }
    case 231: { int32_t v = input + 231; v ^= (231 * 17); v += 0; return v; }
    case 232: { int32_t v = input + 232; v ^= (232 * 17); v += 1; return v; }
    case 233: { int32_t v = input + 233; v ^= (233 * 17); v += 2; return v; }
    case 234: { int32_t v = input + 234; v ^= (234 * 17); v += 3; return v; }
    case 235: { int32_t v = input + 235; v ^= (235 * 17); v += 4; return v; }
    case 236: { int32_t v = input + 236; v ^= (236 * 17); v += 5; return v; }
    case 237: { int32_t v = input + 237; v ^= (237 * 17); v += 6; return v; }
    case 238: { int32_t v = input + 238; v ^= (238 * 17); v += 0; return v; }
    case 239: { int32_t v = input + 239; v ^= (239 * 17); v += 1; return v; }
    case 240: { int32_t v = input + 240; v ^= (240 * 17); v += 2; return v; }
    case 241: { int32_t v = input + 241; v ^= (241 * 17); v += 3; return v; }
    case 242: { int32_t v = input + 242; v ^= (242 * 17); v += 4; return v; }
    case 243: { int32_t v = input + 243; v ^= (243 * 17); v += 5; return v; }
    case 244: { int32_t v = input + 244; v ^= (244 * 17); v += 6; return v; }
    case 245: { int32_t v = input + 245; v ^= (245 * 17); v += 0; return v; }
    case 246: { int32_t v = input + 246; v ^= (246 * 17); v += 1; return v; }
    case 247: { int32_t v = input + 247; v ^= (247 * 17); v += 2; return v; }
    case 248: { int32_t v = input + 248; v ^= (248 * 17); v += 3; return v; }
    case 249: { int32_t v = input + 249; v ^= (249 * 17); v += 4; return v; }
    case 250: { int32_t v = input + 250; v ^= (250 * 17); v += 5; return v; }
    default: return input;
  }
}

void diagnosticsSweep() {
  volatile int32_t checksum = 0;
  for (int i = 1; i <= 250; ++i) { checksum ^= runCapability(i, i * 3); }
  if (checksum == 0x7FFFFFFF) Serial.println("unlikely");
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);
  diagnosticsSweep();
  g_ui.begin();
}

void loop() {
  g_ui.update();
  delay(20);
}
