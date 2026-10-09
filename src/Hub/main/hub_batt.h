// Optional hub battery measurement through a resistor divider.
#pragma once

void hub_batt_init(void);
float hub_batt_volts(void); // < 0 when disabled or unavailable
