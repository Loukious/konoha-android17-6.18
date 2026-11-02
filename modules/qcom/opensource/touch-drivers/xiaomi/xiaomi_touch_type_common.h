#ifndef __XIAOMI_TOUCH_TYPE_COMMON_H__
#define __XIAOMI_TOUCH_TYPE_COMMON_H__

#include <linux/types.h>
#include <touch/xiaomi_touch.h>

#define FOD_VALUE_LEN 5

enum suspend_state {
	XIAOMI_TOUCH_RESUME = 0,
	XIAOMI_TOUCH_SUSPEND,
	XIAOMI_TOUCH_LP1,
	XIAOMI_TOUCH_LP2,
	XIAOMI_TOUCH_SENSORHUB_ENABLE,
	XIAOMI_TOUCH_SENSORHUB_DISABLE,
	XIAOMI_TOUCH_SENSORHUB_NONUIENABLE,
	XIAOMI_TOUCH_ENABLE_SENSOR = 100,
	XIAOMI_TOUCH_DISABLE_SENSOR = 101,
};

typedef struct hardware_param {
	u16 x_resolution;
	u16 y_resolution;
	u16 rx_num;
	u16 tx_num;
	u8 super_resolution_factor;
	u8 frame_data_page_size;
	u8 frame_data_buf_size;
	u8 raw_data_page_size;
	u8 raw_data_buf_size;
	u8 lockdown_info[8];
	char config_file_name[64];
	char driver_version[64];
	char fw_version[64];
	u8 temp_change_value;
} hardware_param_t;

enum touch_dump_type {
	DUMP_OFF = 0,
	DUMP_ON = 1,
	DUMP_BASE = 100,
};

#endif
