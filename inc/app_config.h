#pragma once

/* Shared compile-time configuration and tuning constants for the light ring.
   CONFIG_* values come from Kconfig (sdkconfig) and are force-included by the
   ESP-IDF build system. */

#define LED_STRIP_RESOLUTION_HZ     10000000U
#define LED_STRIP_MEM_BLOCK_SYMBOLS 128U
#define HTTP_RECV_BUFFER_SIZE       2048
#define BACKUP_MAX_BODY_SIZE        32768
#define AP_MAX_STA_CONNECTIONS      4
#define PALETTE_ENTRY_COUNT         16U
#define BUILTIN_PALETTE_COUNT       7U
#define CUSTOM_PALETTE_SLOT_COUNT   14U
#define CUSTOM_PALETTE_MAX_STOPS    27U
#define PALETTE_NAME_LENGTH         24U
#define CUSTOM_PALETTE_START_ID     (1U + BUILTIN_PALETTE_COUNT)
#define PALETTE_COUNT               (CUSTOM_PALETTE_START_ID + CUSTOM_PALETTE_SLOT_COUNT)
#define PALETTE_NVS_NAMESPACE       "palettes"
#define PALETTE_NVS_KEY             "catalog2"
#define SCENARIO_NVS_NAMESPACE      "scenarios"
#define SCENARIO_NVS_COUNT_KEY      "count"
#define SCENARIO_MAX_COUNT          12U
#define SCENARIO_NAME_LENGTH        32U
#define SCENARIO_PAYLOAD_LENGTH     768U
#define CUSTOM_PALETTE_CIRCULAR_FLAG 0x80U
#define CUSTOM_PALETTE_STOP_COUNT_MASK 0x7FU
#define SWOOSH_PATH_LENGTH          ((CONFIG_LIGHT_RING_LED_COUNT + 1U) / 2U)
#define SHRINK_BAR_COUNT            3U
#define SHRINK_SEGMENT_LENGTH       (CONFIG_LIGHT_RING_LED_COUNT / SHRINK_BAR_COUNT)
#define SHRINK_BAR_LENGTH           (SHRINK_SEGMENT_LENGTH)
#define ROTATION_CORNER_COUNT       3U
#define ROTATION_SEGMENT_LENGTH     (CONFIG_LIGHT_RING_LED_COUNT / ROTATION_CORNER_COUNT)
#define ROTATION_MIN_LENGTH         2U
#define ROTATION_MAX_LENGTH         (ROTATION_SEGMENT_LENGTH)
#define POINT_APEX                  ((CONFIG_LIGHT_RING_LED_COUNT / 2U) - 1U)
#define POINT_ARM_LENGTH            (CONFIG_LIGHT_RING_LED_COUNT - 1U - POINT_APEX)
#define POINT_MIN_REPEAT            1U
#define POINT_MAX_REPEAT            3U
#define EDGE_MAX_LENGTH            (CONFIG_LIGHT_RING_LED_COUNT)
#define EDGE_MIN_REPEAT            1U
#define EDGE_MAX_REPEAT            3U
#define CHASE_MIN_LENGTH          1U
#define CHASE_MAX_LENGTH          (CONFIG_LIGHT_RING_LED_COUNT)
#define DNS_SERVER_PORT             53
#define DNS_PACKET_MAX_SIZE         512
#define STRINGIFY_HELPER(value)     #value
#define STRINGIFY(value)            STRINGIFY_HELPER(value)

