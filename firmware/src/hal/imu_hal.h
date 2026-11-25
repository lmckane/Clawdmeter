#pragma once
#include <stdint.h>

// Optional accelerometer-driven orientation tracker. Returns 0..3 (quarter
// turns CW from default mounting). Boards without an IMU — or boards with
// rotation intentionally disabled, like AMOLED-1.8 fixed at 0° — return 0
// from imu_hal_rotation_quadrant() and no-op on init/tick.

void    imu_hal_init(void);
void    imu_hal_tick(void);
uint8_t imu_hal_rotation_quadrant(void);

// C6 AMOLED-2.16 only: returns one latched, deliberate shake gesture.
// Other boards do not call this until their own gesture support is added.
#ifdef CLAWD_C6_SETTINGS
bool    imu_hal_take_shake(void);
#endif
