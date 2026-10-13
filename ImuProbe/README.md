# IMU probe

Tiny test firmware for the ProMicro stacked SPI tracker. It starts in hold mode: IMU powered,
CS high, SPI pins released (for an external I2C check); `p` bit-bangs SPI to read WHO_AM_I,
printing on USB serial (115200, DTR on). It only replaces the app; pairing/calibration (NVS) are untouched.
Send `d`, press reset once, or crash: it returns to the UF2 bootloader; then reinstall the tracker firmware.

Build (WSL): see the `imu-probe` target in the root Makefile.
