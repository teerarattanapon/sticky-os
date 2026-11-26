#include "todo.h"
#include <LittleFS.h>
#include <time.h>

static const size_t MAX_ITEMS = 60;
static const size_t MAX_TEXT = 200;
static const char *FILE_PATH = "/todo.txt";
static std::vector<TodoItem> items;
static bool demoActive;

std::vector<TodoItem> &todoItems() { return items; }

// One item a line: "0 @1794500000 text" (open, added then), "1 ..." (done).
static void save() {
  if (demoActive) return;
  static const char *TMP = "/todo.tmp";
  File f = LittleFS.open(TMP, "w");
  if (!f) return;
  bool ok = true;
  for (const TodoItem &i : items) ok = ok && f.printf("%c @%lu %s\n", i.done ? '1' : '0', (unsigned long)i.added, i.text.c_str()) > 0;
  f.close();
  if (ok && !LittleFS.rename(TMP, FILE_PATH)) ok = LittleFS.remove(FILE_PATH) && LittleFS.rename(TMP, FILE_PATH);
  if (!ok) {
    LittleFS.remove(TMP);
    Serial.println("todo: couldn't save the list (storage full?): the last saved one stays");
  }
}

void todoBegin() {
  File f = LittleFS.open(FILE_PATH, "r");
  while (f && f.available() && items.size() < MAX_ITEMS) {
    String line = f.readStringUntil('\n');
    if (line.length() < 3 || line[1] != ' ') continue;
    TodoItem it = {line[0] == '1', line.substring(2), 0};
    if (it.text.startsWith("@")) {
      int space = it.text.indexOf(' ');
      it.added = strtoul(it.text.c_str() + 1, nullptr, 10);
      it.text = space > 0 ? it.text.substring(space + 1) : String();
    }
    if (it.text.length()) items.push_back(it);
  }
  if (f) f.close();
  Serial.printf("todo: %d saved items\n", (int)items.size());
}

#ifdef TODO_TEST
void todoReloadForTest() {
  items.clear();
  todoBegin();
}
#endif

std::vector<int> todoOrder() {
  std::vector<int> order;
  for (int done = 0; done < 2; done++)
    for (int i = (int)items.size() - 1; i >= 0; i--)
      if (items[i].done == (bool)done) order.push_back(i);
  return order;
}

int todoOpenCount() {
  int n = 0;
  for (const TodoItem &i : items) n += !i.done;
  return n;
}

bool todoAdd(const String &text) {
  String t = text;
  t.replace('\n', ' ');
  t.replace('\r', ' ');
  t.trim();
  if (!t.length() || items.size() >= MAX_ITEMS) return false;
  if (t.length() > MAX_TEXT) {  // cut at a whole letter
    size_t n = MAX_TEXT;
    while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
    t.remove(n);
  }
  time_t now = time(nullptr);
  items.push_back({false, t, now > 1700000000 ? (uint32_t)now : 0});
  save();
  return true;
}

void todoToggle(int index) {
  if (index < 0 || index >= (int)items.size()) return;
  items[index].done = !items[index].done;
  save();
}

void todoDelete(int index) {
  if (index < 0 || index >= (int)items.size()) return;
  items.erase(items.begin() + index);
  save();
}

int todoClearDone() {
  size_t before = items.size();
  for (size_t i = items.size(); i-- > 0;)
    if (items[i].done) items.erase(items.begin() + i);
  if (items.size() != before) save();
  return before - items.size();
}

void todoDemo(bool on) {
  static std::vector<TodoItem> real;
  if (on == demoActive) return;
  demoActive = on;
  if (!on) {
    items = real;
    return;
  }
  real = items;
  uint32_t t = time(nullptr);
  items = {{true, "Return library books", t - 90000},
           {false, "Call the dentist", t - 7200},
           {false, "ซื้อนมกับไข่", t - 3600},
           {false, "Pack for the weekend", t - 600}};
}
