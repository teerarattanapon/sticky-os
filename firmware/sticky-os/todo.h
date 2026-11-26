// Todo: a plain to-do list of its own, kept in the board's flash. Separate
// from Notes: no AI, no notes, its own file (/todo.txt).
#pragma once
#include <Arduino.h>
#include <vector>

struct TodoItem {
  bool done;
  String text;
  uint32_t added;  // unix seconds; 0 when the clock wasn't set
};

void todoBegin();  // loads the saved list; LittleFS is already mounted (notesBegin)
std::vector<TodoItem> &todoItems();
// Open items first (newest first), then done ones: the order on screen.
std::vector<int> todoOrder();
int todoOpenCount();
bool todoAdd(const String &text);  // false: empty or the list is full
void todoToggle(int index);
void todoDelete(int index);
int todoClearDone();  // removes every done item; how many went
void todoDemo(bool on);  // screenshots: made-up items, never saved
