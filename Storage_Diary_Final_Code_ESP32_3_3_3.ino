#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// =====================================================
// 1. WIFI CONFIGURATION
// =====================================================

const char* AP_SSID = "ESP32-Diary";
const char* AP_PASSWORD = "12345678";

WebServer server(80);

// =====================================================
// 2. TFT: VSPI
// =====================================================

#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_CS    21
#define TFT_DC    22
#define TFT_RST   -1

// =====================================================
// 3. TFT SPI BUS
// =====================================================

SPIClass tftSPI(VSPI);

Adafruit_ST7735 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// =====================================================
// 4. BUTTONS
// LEFT = GPIO 25, MIDDLE/SELECT = GPIO 26, RIGHT = GPIO 4
// Active HIGH: each button connects its GPIO to 3.3 V.
// =====================================================

#define BTN_LEFT    25
#define BTN_SELECT  26
#define BTN_RIGHT   4

const int buttonPins[3] = {
  BTN_LEFT, BTN_SELECT, BTN_RIGHT
};

// =====================================================
// 5. STORAGE CONFIGURATION
// =====================================================

const char* DIARY_DIR = "/diary";

const int MAX_NOTES = 10;              // Keep only the newest 10 notes in RAM to fit ESP32 DRAM
const int TOTAL_NOTE_SLOTS = 900;       // Persistent note IDs: 0 through 899
const int WEB_PAGE_SIZE = 50;            // Web list ranges: 0-49, 50-99, ...
const int MAX_TITLE = 48;
const int MAX_BODY = 800;
const int CHARS_PER_PAGE = 100;

struct Note {
  char filename[20];
  char title[MAX_TITLE + 1];
  char body[MAX_BODY + 1];
};

Note notes[MAX_NOTES];

int noteCount = 0;
int currentNote = 0;

int bodyPage = 0;
int bodyPageCount = 1;

bool storageReady = false;

char deletedTitle[MAX_TITLE + 1] = "";

// =====================================================
// 6. FSM
// =====================================================

enum State {
  BROWSE,
  VIEW_NOTE,
  DELETE_CONFIRM,
  DELETED
};

enum Event {
  EVENT_NONE,
  EVENT_LEFT,
  EVENT_SELECT,
  EVENT_RIGHT
};

State currentState = BROWSE;
unsigned long stateStartTime = 0;

void enterState(State nextState);
void drawScreen();
void loadNotes();
bool saveNote(const char* title, const char* body);
bool deleteCurrentNote();
void updateBodyPages();
bool readNoteFile(const char* path, Note& note);
void handleEvent(Event event);
void moveNote(int direction);

// =====================================================
// 7. BUTTON DEBOUNCING
// Active HIGH, INPUT_PULLDOWN
// =====================================================

bool lastRawState[3] = {LOW, LOW, LOW};
bool stableState[3] = {LOW, LOW, LOW};
unsigned long lastDebounceTime[3] = {0, 0, 0};
const unsigned long DEBOUNCE_MS = 30;

Event readButtonEvent() {
  for (int i = 0; i < 3; i++) {
    bool raw = digitalRead(buttonPins[i]);

    if (raw != lastRawState[i]) {
      lastRawState[i] = raw;
      lastDebounceTime[i] = millis();
    }

    if (millis() - lastDebounceTime[i] >= DEBOUNCE_MS) {
      if (stableState[i] != lastRawState[i]) {
        stableState[i] = lastRawState[i];

        if (stableState[i] == HIGH) {
          if (i == 0) return EVENT_LEFT;
          if (i == 1) return EVENT_SELECT;
          if (i == 2) return EVENT_RIGHT;
        }
      }
    }
  }

  return EVENT_NONE;
}

// =====================================================
// 8. SPI CHIP-SELECT HELPERS
// =====================================================

void prepareTFT() {
  // The TFT is the only SPI peripheral in this build.
}

void prepareSD() {
  // Kept as a no-op so the original storage routines remain easy to follow.
  // LittleFS uses internal flash and does not share the TFT SPI bus.
}

// =====================================================
// 9. PAGINATION
// =====================================================

void updateBodyPages() {
  bodyPageCount = 1;

  if (noteCount > 0) {
    int length = strlen(notes[currentNote].body);
    bodyPageCount = max(1, (length + CHARS_PER_PAGE - 1) / CHARS_PER_PAGE);
  }

  if (bodyPage < 0) bodyPage = 0;
  if (bodyPage >= bodyPageCount) bodyPage = bodyPageCount - 1;
}

// =====================================================
// 10. READ ONE NOTE
// File format: first line = title, remaining content = body
// =====================================================

bool readNoteFile(const char* path, Note& note) {
  if (!storageReady) return false;

  prepareSD();
  File file = LittleFS.open(path, FILE_READ);

  if (!file) {
    Serial.print("Cannot open: ");
    Serial.println(path);
    return false;
  }

  String title = file.readStringUntil('\n');
  String body = file.readString();
  file.close();

  title.trim();

  while (body.length() > 0 &&
         (body[body.length() - 1] == '\n' ||
          body[body.length() - 1] == '\r')) {
    body.remove(body.length() - 1);
  }

  snprintf(note.filename, sizeof(note.filename), "%s", path);
  snprintf(note.title, sizeof(note.title), "%s", title.c_str());
  snprintf(note.body, sizeof(note.body), "%s", body.c_str());

  return true;
}

// =====================================================
// 11. LOAD NOTES FROM LITTLEFS
// =====================================================

void loadNotes() {
  if (!storageReady) return;

  // Keep only the newest MAX_NOTES entries in RAM. All 900 note slots
  // remain stored in LittleFS and are browsed directly by the web page.
  static Note newest[MAX_NOTES];
  int found = 0;

  for (int n = TOTAL_NOTE_SLOTS - 1; n >= 0 && found < MAX_NOTES; n--) {
    char path[20];
    snprintf(path, sizeof(path), "/diary/%03d.txt", n);
    if (!LittleFS.exists(path)) continue;
    if (readNoteFile(path, newest[found])) found++;
  }

  noteCount = found;
  // Reverse newest-first temporary list to oldest-first RAM list.
  for (int i = 0; i < found; i++) {
    notes[i] = newest[found - 1 - i];
  }

  if (currentNote >= noteCount) currentNote = noteCount - 1;
  if (currentNote < 0) currentNote = 0;
  bodyPage = 0;
  updateBodyPages();

  Serial.print("Newest notes loaded into RAM: ");
  Serial.println(noteCount);
}

// =====================================================
// 12. SAVE NOTE TO LITTLEFS
// =====================================================

bool saveNote(const char* title, const char* body) {
  if (!storageReady || title == nullptr || body == nullptr) return false;

  size_t titleLen = strlen(title);
  size_t bodyLen = strlen(body);

  if (titleLen == 0 || titleLen > MAX_TITLE || bodyLen > MAX_BODY) {
    Serial.println("Save rejected: invalid length or note limit.");
    return false;
  }

  char path[20];
  bool found = false;
  int highestUsed = -1;

  // Prefer the next number so the newest note normally has the highest ID.
  for (int n = 0; n < TOTAL_NOTE_SLOTS; n++) {
    snprintf(path, sizeof(path), "/diary/%03d.txt", n);
    if (LittleFS.exists(path)) highestUsed = n;
  }

  if (highestUsed + 1 < TOTAL_NOTE_SLOTS) {
    snprintf(path, sizeof(path), "/diary/%03d.txt", highestUsed + 1);
    found = !LittleFS.exists(path);
  }

  // If the highest slot is occupied, reuse a gap if one exists.
  if (!found) {
    for (int n = 0; n < TOTAL_NOTE_SLOTS; n++) {
      snprintf(path, sizeof(path), "/diary/%03d.txt", n);
      if (!LittleFS.exists(path)) { found = true; break; }
    }
  }

  if (!found) {
    Serial.println("All 900 note slots are occupied.");
    return false;
  }

  File file = LittleFS.open(path, FILE_WRITE);

  if (!file) {
    Serial.println("Failed to create note file.");
    return false;
  }

  file.println(title);
  file.print(body);
  file.flush();
  file.close();

  // Reload the saved entries, then select the exact file just written.
  // This ensures the TFT displays the newest note even if an older
  // filename was reused after a previous note was deleted.
  loadNotes();
  for (int i = 0; i < noteCount; i++) {
    if (strcmp(notes[i].filename, path) == 0) {
      currentNote = i;
      break;
    }
  }
  bodyPage = 0;
  updateBodyPages();

  Serial.print("Saved note: ");
  Serial.println(path);
  Serial.println("Latest note selected for TFT display.");

  return true;
}

// =====================================================
// 13. VERIFIED PERMANENT DELETION
// =====================================================

bool deleteCurrentNote() {
  if (!storageReady || noteCount == 0) {
    Serial.println("Delete rejected: no internal flash or no notes.");
    return false;
  }

  char path[20];
  snprintf(path, sizeof(path), "%s", notes[currentNote].filename);
  snprintf(deletedTitle, sizeof(deletedTitle), "%s", notes[currentNote].title);

  Serial.print("Attempting to delete: ");
  Serial.println(path);

  prepareSD();

  if (!LittleFS.exists(path)) {
    Serial.println("Delete failed: file does not exist.");
    loadNotes();
    return false;
  }

  bool removed = LittleFS.remove(path);

  prepareSD();

  if (!removed || LittleFS.exists(path)) {
    Serial.println("DELETE FAILED: file still exists.");
    return false;
  }

  Serial.println("DELETE VERIFIED: file removed.");
  loadNotes();
  return true;
}

// =====================================================
// 14. TFT DISPLAY HELPERS
// =====================================================

void drawHeader(const char* title) {
  prepareTFT();
  tft.fillRect(0, 0, 160, 22, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setCursor(5, 7);
  tft.print(title);
}

void drawFooter(const char* text) {
  prepareTFT();
  tft.fillRect(0, 109, 160, 19, ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setCursor(4, 115);
  tft.print(text);
}

// =====================================================
// 15. RENDER TFT SCREEN
// =====================================================

void drawScreen() {
  prepareTFT();
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextWrap(true);

  switch (currentState) {
    case BROWSE:
      drawHeader("STORAGE DIARY");

      if (noteCount == 0) {
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(8, 42);
        tft.print("No saved notes");

        tft.setTextColor(storageReady ? ST77XX_GREEN : ST77XX_RED);
        tft.setCursor(8, 62);
        tft.print(storageReady ? "FLASH ready" : "FLASH offline");

        drawFooter("Waiting for notes");
        break;
      }

      tft.setTextColor(ST77XX_CYAN);
      tft.setCursor(8, 31);
      tft.print("Entry ");
      tft.print(currentNote + 1);
      tft.print("/");
      tft.print(noteCount);

      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(8, 47);
      tft.print(notes[currentNote].title);

      tft.setTextColor(ST77XX_GREEN);
      tft.setCursor(8, 65);
      tft.print(notes[currentNote].body);

      drawFooter("L/R: Move SEL: Open");
      break;

    case VIEW_NOTE: {
      drawHeader("VIEW ENTRY");

      if (noteCount == 0) {
        drawFooter("No notes");
        break;
      }

      updateBodyPages();

      tft.setTextColor(ST77XX_CYAN);
      tft.setTextWrap(false);
      tft.setCursor(5, 29);
      tft.print(notes[currentNote].title);

      int start = bodyPage * CHARS_PER_PAGE;
      String pageText = String(notes[currentNote].body)
                          .substring(start, start + CHARS_PER_PAGE);

      tft.setTextColor(ST77XX_WHITE);
      tft.setTextWrap(true);
      tft.setCursor(5, 44);
      tft.print(pageText);

      tft.setTextWrap(false);
      tft.setTextColor(ST77XX_CYAN);
      tft.setCursor(5, 99);
      tft.print("Page ");
      tft.print(bodyPage + 1);
      tft.print("/");
      tft.print(bodyPageCount);

      drawFooter("L/R: Page SEL: Delete");
      break;
    }

    case DELETE_CONFIRM:
      drawHeader("CONFIRM DELETE");

      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(8, 35);
      tft.print("Delete this entry?");

      tft.setTextColor(ST77XX_RED);
      tft.setCursor(8, 54);
      if (noteCount > 0) tft.print(notes[currentNote].title);

      tft.setTextColor(ST77XX_YELLOW);
      tft.setCursor(8, 76);
      tft.print("SEL = Confirm");

      drawFooter("L/R: Cancel");
      break;

    case DELETED:
      drawHeader("DELETE RESULT");

      tft.setTextColor(ST77XX_GREEN);
      tft.setCursor(8, 40);
      tft.print("Deleted:");

      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(8, 59);
      tft.print(deletedTitle);
      break;
  }
}

// =====================================================
// 16. FSM STATE TRANSITIONS
// =====================================================

void enterState(State nextState) {
  currentState = nextState;
  stateStartTime = millis();

  Serial.print("FSM state: ");

  switch (currentState) {
    case BROWSE:         Serial.println("BROWSE"); break;
    case VIEW_NOTE:      Serial.println("VIEW_NOTE"); break;
    case DELETE_CONFIRM: Serial.println("DELETE_CONFIRM"); break;
    case DELETED:        Serial.println("DELETED"); break;
  }

  drawScreen();
}

void moveNote(int direction) {
  if (noteCount == 0) return;

  currentNote += direction;

  if (currentNote < 0) currentNote = noteCount - 1;
  if (currentNote >= noteCount) currentNote = 0;

  bodyPage = 0;
  drawScreen();
}

// =====================================================
// 17. FSM EVENT HANDLER
// =====================================================

void handleEvent(Event event) {
  if (event == EVENT_NONE) return;

  switch (currentState) {
    case BROWSE:
      if (event == EVENT_LEFT) {
        moveNote(-1);
      } else if (event == EVENT_RIGHT) {
        moveNote(1);
      } else if (event == EVENT_SELECT && noteCount > 0) {
        bodyPage = 0;
        updateBodyPages();
        enterState(VIEW_NOTE);
      }
      break;

    case VIEW_NOTE:
      if (event == EVENT_SELECT) {
        enterState(DELETE_CONFIRM);
      } else if (event == EVENT_LEFT) {
        updateBodyPages();

        if (bodyPage > 0) {
          bodyPage--;
          drawScreen();
        } else {
          enterState(BROWSE);
        }
      } else if (event == EVENT_RIGHT) {
        updateBodyPages();

        if (bodyPage < bodyPageCount - 1) {
          bodyPage++;
          drawScreen();
        } else {
          enterState(BROWSE);
        }
      }
      break;

    case DELETE_CONFIRM:
      if (event == EVENT_SELECT) {
        if (deleteCurrentNote()) {
          enterState(DELETED);
        } else {
          Serial.println("Entry was not deleted.");
        }
      } else if (event == EVENT_LEFT || event == EVENT_RIGHT) {
        enterState(VIEW_NOTE);
      }
      break;

    case DELETED:
      break;
  }
}

// =====================================================
// 18. HTML ESCAPING
// =====================================================

String htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {
    switch (input[i]) {
      case '&':  output += "&amp;";  break;
      case '<':  output += "&lt;";   break;
      case '>':  output += "&gt;";   break;
      case '"':  output += "&quot;"; break;
      case '\'': output += "&#39;";  break;
      default:   output += input[i]; break;
    }
  }

  return output;
}

// =====================================================
// 19. WEB PAGE
// =====================================================

void handleHome() {
  int startIndex = 0;
  if (server.hasArg("start")) startIndex = server.arg("start").toInt();
  if (startIndex < 0) startIndex = 0;
  startIndex = (startIndex / WEB_PAGE_SIZE) * WEB_PAGE_SIZE;
  if (startIndex >= TOTAL_NOTE_SLOTS) startIndex = TOTAL_NOTE_SLOTS - WEB_PAGE_SIZE;

  String page;
  page.reserve(12000);
  page += F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Diary</title><style>"
    "body{font-family:Arial;max-width:760px;margin:20px auto;padding:0 14px;"
    "background:#101820;color:#eee}h1{color:#78dce8}"
    "input,textarea,button{box-sizing:border-box;width:100%;padding:12px;"
    "margin:7px 0;border-radius:8px;font:inherit}"
    "input,textarea{background:#202b36;color:white;border:1px solid #65717c}"
    "textarea{min-height:220px;resize:vertical;line-height:1.45}"
    "button,.nav a{background:#167d9a;color:white;border:0;font-weight:bold;"
    "padding:12px;border-radius:8px;text-decoration:none;display:inline-block}"
    ".note{padding:12px;margin:10px 0;background:#202b36;border-radius:8px;"
    "overflow-wrap:anywhere;white-space:pre-wrap}.muted{color:#aab8c2;font-size:14px}"
    ".nav{display:flex;gap:8px;justify-content:space-between;margin:12px 0}"
    ".nav a{flex:1;text-align:center}.nav .off{opacity:.35;pointer-events:none}"
    "</style></head><body><h1>Storage Diary</h1>"
    "<p class='muted'>Up to 900 note slots (0-899), saved in ESP32 internal flash.</p>"
  );

  if (server.hasArg("status")) {
    page += server.arg("status") == "saved"
      ? F("<p>Note saved successfully. The newest note is selected on the TFT.</p>")
      : F("<p>Save failed. Check the title/body limits and flash storage status.</p>");
  }

  page += F("<h2>Write a new entry</h2><form method='POST' action='/save'>"
            "<label for='title'>Title (up to 48 characters)</label>"
            "<input id='title' name='title' maxlength='48' required>"
            "<label for='body'>Note (up to 800 characters)</label>"
            "<textarea id='body' name='body' maxlength='800' required></textarea>"
            "<button type='submit'>Save note</button></form>");

  page += F("<h2>Browse saved notes</h2><p class='muted'>Showing note numbers ");
  page += String(startIndex);
  page += F("-");
  page += String(min(startIndex + WEB_PAGE_SIZE - 1, TOTAL_NOTE_SLOTS - 1));
  page += F("</p><div class='nav'>");
  if (startIndex > 0) {
    page += F("<a href='/?start="); page += String(startIndex - WEB_PAGE_SIZE);
    page += F("'>Previous 50</a>");
  } else page += F("<a class='off'>Previous 50</a>");
  if (startIndex + WEB_PAGE_SIZE < TOTAL_NOTE_SLOTS) {
    page += F("<a href='/?start="); page += String(startIndex + WEB_PAGE_SIZE);
    page += F("'>Next 50</a>");
  } else page += F("<a class='off'>Next 50</a>");
  page += F("</div>");

  if (!storageReady) {
    page += F("<p>Internal flash is unavailable. Notes cannot be saved until LittleFS mounts.</p>");
  } else {
    int displayed = 0;
    for (int n = startIndex; n < startIndex + WEB_PAGE_SIZE && n < TOTAL_NOTE_SLOTS; n++) {
      char path[20];
      snprintf(path, sizeof(path), "/diary/%03d.txt", n);
      if (!LittleFS.exists(path)) continue;
      Note item;
      if (!readNoteFile(path, item)) continue;
      page += F("<div class='note'><strong>Note #");
      page += String(n);
      page += F(": "); page += htmlEscape(String(item.title));
      page += F("</strong><p>"); page += htmlEscape(String(item.body));
      page += F("</p></div>");
      displayed++;
    }
    if (displayed == 0) page += F("<p>No notes in this range yet.</p>");
  }
  page += F("<p class='muted'>The TFT keeps the newest 10 notes available for button-based browsing."
            " The web page lists all 900 slots in groups of 50.</p></body></html>");
  server.send(200, "text/html; charset=utf-8", page);
}

// =====================================================
// 20. WEB SAVE HANDLER
// =====================================================

void handleSave() {
  if (!server.hasArg("title") || !server.hasArg("body")) {
    server.send(400, "text/plain", "Missing title or body.");
    return;
  }

  String title = server.arg("title");
  String body = server.arg("body");
  title.trim();

  if (title.length() == 0 ||
      title.length() > MAX_TITLE ||
      body.length() > MAX_BODY) {
    server.send(400, "text/plain", "Invalid note length.");
    return;
  }

  bool saved = saveNote(title.c_str(), body.c_str());

  if (saved) {
    // Immediately show the note that was just submitted on the TFT.
    enterState(VIEW_NOTE);

    server.sendHeader("Location", "/?status=saved");
    server.send(303, "text/plain", "Saved");
  } else {
    server.sendHeader("Location", "/?status=error");
    server.send(303, "text/plain", "Save failed");
  }
}

// =====================================================
// 21. WIFI ACCESS POINT + WEB SERVER
// =====================================================

void setupWebServer() {
  WiFi.mode(WIFI_AP);

  bool started = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (started) {
    Serial.println("Wi-Fi AP started");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("IP address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Wi-Fi AP failed");
  }

  server.on("/", HTTP_GET, handleHome);
  server.on("/save", HTTP_POST, handleSave);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("Web server started");
}

// =====================================================
// 22. SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Storage Diary booting (LittleFS edition)...");

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);

  tftSPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  Serial.println("Initializing TFT...");
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  Serial.println("TFT OK");

  // Do NOT auto-format on failure. Formatting would erase existing notes.
  Serial.println("Mounting LittleFS internal flash...");
  storageReady = LittleFS.begin(false);

  if (storageReady) {
    Serial.println("LittleFS mounted successfully.");
    if (!LittleFS.exists(DIARY_DIR) && !LittleFS.mkdir(DIARY_DIR)) {
      Serial.println("Failed to create /diary directory.");
      storageReady = false;
    }
    if (storageReady) {
      size_t total = LittleFS.totalBytes();
      size_t used = LittleFS.usedBytes();
      Serial.print("LittleFS total: "); Serial.print(total / 1024); Serial.println(" KB");
      Serial.print("LittleFS used: "); Serial.print(used / 1024); Serial.println(" KB");
    }
  } else {
    Serial.println("LittleFS mount FAILED. No automatic format was performed.");
    Serial.println("Check the ESP32 partition scheme / LittleFS partition configuration.");
  }

  for (int i = 0; i < 3; i++) {
    pinMode(buttonPins[i], INPUT_PULLDOWN);
  }

  if (storageReady) {
    loadNotes();
  } else {
    noteCount = 0;
    Serial.println("Starting UI without persistent storage.");
  }

  enterState(BROWSE);
  setupWebServer();
  Serial.println("System ready.");
}

// =====================================================
// 23. MAIN LOOP
// =====================================================

void loop() {
  Event event = readButtonEvent();

  if (event != EVENT_NONE) {
    Serial.print("Button event: ");
    Serial.println((int)event);
    handleEvent(event);
  }

  if (currentState == DELETED &&
      millis() - stateStartTime >= 1200) {
    enterState(BROWSE);
  }

  server.handleClient();
  delay(1);
}
