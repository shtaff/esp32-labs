// =============================================================================
// gps_module.h - NEO-6M on UART2, RX builds only.
//
// Two jobs:
//   1. supply position, which the log needs and which the distance-to-site
//      calculation needs;
//   2. supply UTC, which is what keeps the receiver in step with the
//      transmitter's slot schedule without any WiFi on the receiver at all.
//
// Note this file is excluded from the TX build by build_src_filter, so the
// TinyGPSPlus dependency never enters the transmitter binary.
// =============================================================================
#pragma once

#include <stdint.h>

struct GpsSnapshot {
  bool     locationValid;
  double   lat;
  double   lon;
  double   altitudeM;
  double   speedKmh;
  double   courseDeg;
  double   hdop;
  uint32_t satellites;
  uint32_t locationAgeMs;
};

void gpsBegin();

// Feed the parser and, when a fresh time fix appears, re-anchor the shared
// clock. Call this often from loop(); it is non-blocking.
void gpsPoll();

GpsSnapshot gpsSnapshot();

// Great-circle distance in metres between two WGS84 points.
double gpsDistanceMeters(double lat1, double lon1, double lat2, double lon2);

// Initial great-circle bearing from point 1 to point 2, degrees clockwise from
// true north (0 = north, 90 = east). Logged alongside the distance so a run can
// be sliced by direction - which is what turns a scatter of ranges into
// something you can read as an antenna pattern or a terrain shadow.
double gpsBearingDeg(double lat1, double lon1, double lat2, double lon2);
