// Minimal freestanding shared library used to regression-test os64's
// dynamic-linking support (see kernel/src/shared_object.c). Deliberately
// tiny: one exported function, one exported mutable global.
//
// shlib_counter starts at 42 and increments on every call. Two tasks that
// each load this library and call shlib_add exactly once should BOTH see
// shlib_counter start at 42 and end at 43 — proving each task privatized
// its own copy via CoW rather than sharing writes to the one physical page.
// If CoW were broken, the second task to run would see the first task's
// already-incremented value instead.

int shlib_counter = 42;

int shlib_add(int a, int b) {
    shlib_counter++;
    return a + b + shlib_counter;
}
