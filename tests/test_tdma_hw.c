/* SPDX-License-Identifier: MIT */
#include "../shared/tdma_hw.c"
static int sdk_error;
static bool omit_hook;
int esb_start_tx(void)
{
	if (sdk_error) return sdk_error;
	if (!omit_hook) assert(tdma_hw_defer_start(0x40014018));
	return 0;
}

int main(void)
{
	assert(tdma_hw_start_tx_at(1000) == -ENODEV);
	tdma_hw_cancel(); /* harmless before init */
	fail_allocation = 1;
	assert(tdma_hw_init() == -ENOMEM && allocated == 0);
	fail_allocation = -1;
	assert(tdma_hw_init() == 0);
	unsigned calls = allocation_calls;
	assert(tdma_hw_init() == 0 && calls == allocation_calls);
	assert(!tdma_hw_defer_start(123)); /* legacy/RX/OTA retain SDK start */
	mock_timer.CC[2] = 1000;
	assert(tdma_hw_now() == 1000);
	mock_timer.CC[0] = 960;
	assert(tdma_hw_rx_time() == 960);

	assert(tdma_hw_start_tx_at(1500) == 0);
	assert(mock_timer.CC[1] == 1500 && armed && scheduled == 1);
	assert(enabled & BIT(start_channel));
	assert(task_address[start_channel] == 0x40014018);
	assert(fork_address[start_channel] == (uint32_t)(uintptr_t)&NRF_PPI->TASKS_CHG[start_group].DIS);
	assert(tdma_hw_start_tx_at(1600) == -EBUSY);
	tdma_hw_cancel(); /* OTA/mode takeover before firing must remove trigger */
	assert(!armed && !(enabled & BIT(start_channel)) && cancelled == 1);
	assert(!tdma_hw_defer_start(123));

	assert(tdma_hw_start_tx_at(1079) == -ETIME);
	assert(!armed && rejected && too_late == 1 && !(enabled & BIT(start_channel)));
	assert(tdma_hw_start_tx_at(900) == -ETIME);
	assert(tdma_hw_start_tx_at(1001001) == -ETIME);
	assert(tdma_hw_start_tx_at(1080) == 0); /* minimum lead boundary */
	mock_timer.EVENTS_COMPARE[1] = 1;
	tdma_hw_cancel();
	assert(fired == 1 && cancelled == 1);
	tdma_hw_cancel();
	assert(fired == 1); /* no duplicate completion accounting */
	sdk_error = -EBUSY;
	assert(tdma_hw_start_tx_at(1500) == -EBUSY && !requested && !armed);
	sdk_error = 0; omit_hook = true;
	assert(tdma_hw_start_tx_at(1500) == -ETIME && !requested && !armed);
	omit_hook = false;

	/* Captures and scheduled compares straddle the 32-bit 1 MHz wrap. */
	mock_timer.CC[2] = UINT32_MAX - 100;
	int64_t before = tdma_hw_now();
	assert(tdma_hw_start_tx_at(before + 500) == 0);
	assert(mock_timer.CC[1] == 399);
	mock_timer.CC[0] = UINT32_MAX - 20;
	mock_timer.CC[2] = 40;
	assert(tdma_hw_now() == before + 141);
	assert(tdma_hw_rx_time() == before + 80);
	tdma_hw_cancel();
	puts("Hardware scheduler: allocation recovery, cancellation, late rejection, SDK failures and timer wrap passed");
	return 0;
}
