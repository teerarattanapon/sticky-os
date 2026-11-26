#!/bin/bash
# The Todo app's storage (firmware/sticky-os/todo.cpp), on a computer.
set -e
cd "$(dirname "$0")"
mkdir -p build/fs
CXX=${CXX:-c++}
"$CXX" -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -Ishim -I../../sticky-os \
  -DTODO_TEST -o build/todo_test todo_test.cpp ../../sticky-os/todo.cpp
TODO_FS_DIR=build/fs ./build/todo_test
