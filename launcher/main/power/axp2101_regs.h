/* Private register encodings for the PMU. */
#pragma once
#define AXP2101_I2C_HZ               400000
#define AXP2101_TIMEOUT_MS           100
#define AXP2101_REG_STATUS           0x00
#define AXP2101_REG_CHARGE           0x01
#define AXP2101_REG_GAUGE_ENABLE     0x18
#define AXP2101_GAUGE_ENABLE         0x08U
#define AXP2101_REG_BATTERY_LEVELS   0x1a
#define AXP2101_WARNING_SHIFT        4
#define AXP2101_WARNING_MASK         0x0fU
#define AXP2101_WARNING_BASE_PERCENT 5U
#define AXP2101_SHUTDOWN_SHIFT       0
#define AXP2101_SHUTDOWN_MASK        0x0fU
#define AXP2101_REG_ADC_ENABLE       0x30
#define AXP2101_ADC_VBAT_ENABLE      0x01U
#define AXP2101_REG_VBAT_HIGH        0x34
#define AXP2101_REG_VBAT_LOW         0x35
#define AXP2101_REG_PERCENT          0xa4
#define AXP2101_USB_GOOD             0x20U
#define AXP2101_BATTERY_PRESENT      0x08U
#define AXP2101_FLOW_SHIFT           5
#define AXP2101_FLOW_MASK            3U
#define AXP2101_CHARGE_MASK          7U
#define AXP2101_VBAT_HIGH_MASK       0x3fU
#define AXP2101_VBAT_HIGH_SHIFT      8
#define AXP2101_PERCENT_MAX          100U
/* INTEN2/INTSTS2 hold power-key events: bit 0 rising, 1 falling, 2 long, 3 short.
 * Only short/long are enabled (no down level; see input/buttons/buttons.h).
 * A status bit clears by writing a ONE back. */
#define AXP2101_REG_INTEN2           0x41
#define AXP2101_REG_INTSTS2          0x49
#define AXP2101_PKEY_SHORT           0x08U
#define AXP2101_PKEY_LONG            0x04U
/* Long press fires at IRQLEVEL (0x27[5:4]), independently of the PMU power-off
 * OFFLEVEL (0x27[3:2]) gated by 0x22[1]; one hold can fire both. */
#define AXP2101_REG_LEVELS           0x27
#define AXP2101_REG_PWROFF           0x22
#define AXP2101_LEVEL_MASK           3U
#define AXP2101_IRQ_LEVEL_SHIFT      4
#define AXP2101_OFF_LEVEL_SHIFT      2
#define AXP2101_PWROFF_ENABLE        0x02U
#define AXP2101_PWROFF_RESTART       0x01U
