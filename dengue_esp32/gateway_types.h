#ifndef GATEWAY_TYPES_H
#define GATEWAY_TYPES_H
#include <stdint.h>
/* Keep types in a header so Arduino's generated function prototypes see them. */
struct PlantStatus {
  uint32_t timeLeft, updated;
  uint16_t level, light;
  uint8_t state, humidity, mode;
  bool soilDry, watering, sensorsOk, valid;
};
struct PlantEvent { uint32_t id, generation; uint8_t type; };
struct PlantCommand { char verb[16]; uint32_t queued; };
struct PlantNotice { char text[180]; };
struct PendingCommand {
  bool active;
  uint32_t id, sent;
  uint8_t attempts;
  char verb[16];
};
#endif
