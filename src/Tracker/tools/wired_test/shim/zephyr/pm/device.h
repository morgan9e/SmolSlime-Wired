#pragma once
enum pm_device_action { PM_DEVICE_ACTION_SUSPEND, PM_DEVICE_ACTION_RESUME };
static inline int pm_device_action_run(const struct device *d, enum pm_device_action a) { (void)d; (void)a; return 0; }
