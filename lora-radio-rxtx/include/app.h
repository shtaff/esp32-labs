// =============================================================================
// app.h - the two-function interface that main.cpp calls.
//
// Exactly one of tx_app.cpp / rx_app.cpp is compiled into any given binary;
// build_src_filter in platformio.ini excludes the other one outright, so the
// unused mode contributes nothing at all - not even dead code the linker has to
// strip.
// =============================================================================
#pragma once

void appSetup();
void appLoop();
