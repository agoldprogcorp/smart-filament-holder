/**
 * LVGL v9 Configuration for ESP32-C6 + GC9A01
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_CONF_SKIP 1

/* Color settings */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1  // GC9A01 требует swap байтов

/* Memory settings */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48 * 1024U)

/* Display settings */
#define LV_DPI_DEF 130

/* Compiler settings */
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_FAST_MEM_INIT

/* HAL settings */
#define LV_TICK_CUSTOM 0

/* Feature usage */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

/* Font usage */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 1

/* Widgets */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_ARC 1

/* Logging */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#endif /*LV_CONF_H*/
