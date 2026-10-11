"""Host tests of the production RF handshake with a deterministic worker/clock.

Extract the small production functions to avoid mocking the entire Nordic SDK.
This checks the command/worker contract, not RF hardware or Zephyr scheduling.
Run with Python on a host with a C compiler (CC, or cc).
"""
import os
from pathlib import Path
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[1] / "Tracker/src/connection/esb.c"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef int atomic_t;
static int atomic_get(atomic_t *v) { return *v; }
static void atomic_set(atomic_t *v, int x) { *v = x; }
static void atomic_clear(atomic_t *v) { *v = 0; }
static atomic_t rf_muted, rf_stopped;
static bool updating, radio_active = true, clock_active = true;
static int64_t now, worker_at;
static bool worker_enabled;
static int rf_write_mutex;
#define K_FOREVER 0
#define SYS_STATUS_CONNECTION_ERROR 1
static unsigned irq_lock(void) { return 0; }
static void irq_unlock(unsigned key) { (void)key; }
static bool ota_active(void) { return updating; }
static int64_t k_uptime_get(void) { return now; }
static void k_msleep(int ms);
static void rf_stop(void);
static bool send_data = true, esb_state = true, timer_state = true;
static bool esb_paired = true;
static int tx_errors = 50;
static int64_t last_tx_success = 12, last_tx_fail = 34;
#if CONFIG_RADIO_TDMA
static bool tdma_pending = true, tdma_inflight = true;
static struct { bool valid; } tdma_sync = { true };
#endif
static void k_mutex_lock(int *m, int wait) { (void)wait; assert(!*m); *m = 1; }
static void k_mutex_unlock(int *m) { assert(*m); *m = 0; }
static void esb_deinitialize(void) {
    assert(rf_write_mutex); assert(rf_muted); assert(!rf_stopped);
    radio_active = false;
}
static void clocks_stop(void) { clock_active = false; }
static void set_status(int status, bool value) { (void)status; assert(!value); }
'''

TESTS = r'''
static void k_msleep(int ms) {
    now += ms;
    if (worker_enabled && now >= worker_at && rf_muted && !rf_stopped)
        rf_stop();
}
int main(void) {
    assert(esb_rf_enabled()); assert(!esb_rf_stopped());
    updating = true;
    assert(esb_rf_set_enabled(false) == -EBUSY);
    assert(esb_rf_enabled() && radio_active);
    updating = false;
    worker_enabled = true; worker_at = 1200; /* pairing sleep */
    assert(esb_rf_set_enabled(false) == 0);
    assert(now == 1200 && esb_rf_stopped() && !esb_rf_enabled());
    assert(!radio_active && !clock_active && !rf_write_mutex);
    assert(!send_data && !esb_state && !timer_state);
    assert(!esb_paired);
    assert(!tx_errors && !last_tx_success && !last_tx_fail);
#if CONFIG_RADIO_TDMA
    assert(!tdma_pending && !tdma_inflight && !tdma_sync.valid);
#endif
    assert(esb_rf_set_enabled(false) == 0); /* repeated off */
    assert(now == 1200);
    assert(esb_rf_set_enabled(true) == 0);
    assert(esb_rf_enabled() && !esb_rf_stopped());
    assert(esb_rf_set_enabled(true) == 0); /* repeated on */
    worker_enabled = false; radio_active = true;
    assert(esb_rf_set_enabled(false) == -ETIMEDOUT);
    assert(!esb_rf_enabled() && !esb_rf_stopped());
    assert(esb_rf_set_enabled(true) == -ETIMEDOUT);
    assert(!esb_rf_enabled()); /* cannot cancel an unacknowledged stop */
    worker_enabled = true; worker_at = now + 30;
    assert(esb_rf_set_enabled(true) == 0); /* late worker drains first */
    assert(esb_rf_enabled() && !esb_rf_stopped() && !radio_active);
    radio_active = true; worker_at = now + 20;
    assert(esb_rf_set_enabled(false) == 0); /* previous ack cannot satisfy new off */
    assert(!radio_active && esb_rf_stopped());
    puts("RF handshake tests passed");
}
'''


if __name__ == "__main__":
    source = SOURCE.read_text()
    functions = "\n".join(function(source, name) for name in (
        "bool esb_rf_enabled(void)", "bool esb_rf_stopped(void)",
        "int esb_rf_set_enabled(bool enabled)", "static void rf_stop(void)",
    ))
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / "test.c").write_text(PRELUDE + functions + TESTS)
        for tdma in (0, 1):
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", f"-DCONFIG_RADIO_TDMA={tdma}", str(path / "test.c"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)
