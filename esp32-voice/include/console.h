// =============================================================================
// console.h - a line-oriented serial console, run from loop().
//
// Everything the two buttons do is also reachable from here, which is what
// makes the firmware bring-up-able on a bare board with nothing soldered to
// it yet. It runs in the Arduino loop task at the lowest priority and never
// touches the radio or the codec directly - it goes through the same entry
// points the buttons use.
//
// Commands:
//   help              this list
//   stat              everything the four display screens show, at once
//   ch [0-7]          show or set the channel
//   enc [on|off]      show or set encryption
//   screen [0-3]      show or set the display screen
//   ptt [ms]          key up for ms milliseconds (default 3000), then release
//   tone [on|off]     force the synthetic test signal even with a mic fitted
//   reboot            restart
// =============================================================================
#pragma once

void consoleBegin();

// Consumes whatever is waiting on the serial port. Called from loop(); returns
// immediately when there is nothing to read.
void consolePoll();
