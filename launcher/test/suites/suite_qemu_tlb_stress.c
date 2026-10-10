/* Throwaway (t8jy proof branch, never merged): core 0 maps and unmaps
 * flash pages in a loop, each unmap flushing QEMU's TLBs, while core 1
 * reads peripheral registers. Under QEMU without the espressif/qemu#174
 * fix core 1 is expected to take LoadStorePIFAddrError. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/systimer_reg.h"
#include "unity.h"

#define STRESS_MS  30000
#define PAGE_BYTES 0x10000
#define PAGES      64

/* Plain words in internal RAM: QEMU aborts on an atomic to PSRAM. */
static DRAM_ATTR volatile bool stop;
static DRAM_ATTR volatile unsigned reads;

static void
reader(void* arg) {
    (void)arg;
    uint32_t sink = 0;
    while (!stop) {
        for (int i = 0; i < 256; i++) {
            sink += REG_READ(SYSTIMER_CONF_REG);
            sink += REG_READ(GPIO_IN_REG);
        }
        reads += 512;
    }
    (void)sink;
    stop = false;
    vTaskDelete(NULL);
}

static void
test_core_1_reads_registers_while_core_0_remaps_flash(void) {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "assets");
    TEST_ASSERT_NOT_NULL(part);
    stop = false;
    reads = 0;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(reader, "tlbstress", 2048, NULL, 1, NULL, 1));
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(STRESS_MS);
    unsigned maps = 0;
    volatile uint8_t sink = 0;
    while (xTaskGetTickCount() < end) {
        const void* mapped = NULL;
        esp_partition_mmap_handle_t handle;
        const size_t offset = (size_t)(maps % PAGES) * PAGE_BYTES;
        TEST_ASSERT_EQUAL(ESP_OK,
                          esp_partition_mmap(part, offset, PAGE_BYTES, ESP_PARTITION_MMAP_DATA, &mapped, &handle));
        sink += ((const uint8_t*)mapped)[maps & 0xff];
        esp_partition_munmap(handle);
        maps++;
    }
    stop = true;
    while (stop) {
        vTaskDelay(1);
    }
    ESP_LOGI("tlbstress", "maps=%u core1_reads=%u", maps, reads);
    (void)sink;
}

#endif

void
run_qemu_tlb_stress_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_core_1_reads_registers_while_core_0_remaps_flash);
#endif
}

SUITE_REGISTER_ON_REQUEST(run_qemu_tlb_stress_suite);
