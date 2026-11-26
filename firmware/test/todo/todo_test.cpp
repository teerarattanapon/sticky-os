// The Todo list's storage: add, tick, order, delete, clear, and that it all
// survives a restart (the file is read back by a fresh load).
#include <stdio.h>
#include <string.h>
#include "todo.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

void todoReloadForTest();

int main() {
  remove((std::string(getenv("TODO_FS_DIR")) + "/todo.txt").c_str());
  todoBegin();
  CHECK(todoItems().empty());
  CHECK(!todoAdd("   "));
  CHECK(todoAdd("Buy milk"));
  CHECK(todoAdd("ซื้อไข่\nสองโหล"));  // a line break becomes a space
  CHECK(todoAdd("Call mum"));
  CHECK(todoItems()[1].text == "ซื้อไข่ สองโหล");
  CHECK(todoOpenCount() == 3);
  todoToggle(0);  // Buy milk: done
  std::vector<int> order = todoOrder();
  CHECK(order.size() == 3 && order[0] == 2 && order[1] == 1 && order[2] == 0);  // open, newest first; done last
  CHECK(todoOpenCount() == 2);

  // A long text is cut at a whole letter.
  std::string longThai;
  for (int i = 0; i < 100; i++) longThai += "ก";
  CHECK(todoAdd(longThai.c_str()));
  const String &cut = todoItems().back().text;
  CHECK(cut.length() <= 200 && cut.length() % 3 == 0);

  todoReloadForTest();
  CHECK(todoItems().size() == 4);
  CHECK(todoItems()[0].done && todoItems()[0].text == "Buy milk");
  CHECK(todoItems()[1].text == "ซื้อไข่ สองโหล" && !todoItems()[1].done);

  CHECK(todoClearDone() == 1);
  todoDelete(0);
  todoReloadForTest();
  CHECK(todoItems().size() == 2 && todoItems()[0].text == "Call mum");

  // Demo mode shows made-up items and writes nothing.
  todoDemo(true);
  CHECK(todoItems().size() == 4);
  todoAdd("not saved");
  todoDemo(false);
  todoReloadForTest();
  CHECK(todoItems().size() == 2);

  // Lines from an older or damaged file are skipped, not loaded half.
  FILE *f = fopen((std::string(getenv("TODO_FS_DIR")) + "/todo.txt").c_str(), "a");
  fputs("x\n\n0 no time\n1 @abc\n", f);
  fclose(f);
  todoReloadForTest();
  CHECK(todoItems().size() == 3 && todoItems()[2].text == "no time" && todoItems()[2].added == 0);

  printf(failures ? "todo: %d failed\n" : "todo: ok\n", failures);
  return failures ? 1 : 0;
}
