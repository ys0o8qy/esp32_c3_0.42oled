#pragma once
#include <Arduino.h>
struct NetworkView {
  bool portalActive;
  bool connected;
  bool configured;
  uint32_t uploaded;
  int httpStatus;
  char state[24];
  char apName[16];
  char apPassword[12];
};
bool networkBegin();
void networkRequestPortal();
void networkPublishSample(float temperature, float humidity, bool valid);
NetworkView networkView();
