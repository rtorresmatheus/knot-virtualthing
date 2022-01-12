/**
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2021, CESAR. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#define DRIVER_MAX_PROTOCOL_TYPE_LEN	25
#define DRIVER_MAX_NAME_TYPE_LEN	25
#define DRIVER_MAX_URL_LEN		50

#define DRIVER_MAX_TYPE_TAG_LEN			30
#define DRIVER_MAX_TYPE_PATH_LEN		5
#define DRIVER_MAX_TYPE_STRING_CONNECT_LEN	512

#define DRIVER_MIN_ID		0
#define DRIVER_MAX_ID		255

#define ENDIANNESS_TYPE_BIG_ENDIAN		0x01
#define ENDIANNESS_TYPE_MID_BIG_ENDIAN		0x02
#define ENDIANNESS_TYPE_LITTLE_ENDIAN		0x03
#define ENDIANNESS_TYPE_MID_LITTLE_ENDIAN	0x04


enum CONN_TYPE {
	DRIVER = 0x0F,
	CLOUD = 0xF0
};

struct device_settings {
	char *credentials_path;
	char *device_path;
	char *cloud_path;
};

struct knot_data_item {
	int sensor_id;
	knot_schema schema;
	knot_event event;
	knot_value_type current_val;
	knot_value_type sent_val;
	int reg_addr;
	int value_type_size;
	int32_t tag;
	char tag_name[DRIVER_MAX_TYPE_TAG_LEN];
	char path[DRIVER_MAX_TYPE_PATH_LEN];
	int element_size;
	char string_tag_path[DRIVER_MAX_TYPE_STRING_CONNECT_LEN];
};

struct knot_thing {
	char token[KNOT_PROTOCOL_TOKEN_LEN + 1];
	char id[KNOT_PROTOCOL_UUID_LEN + 1];
	char name[KNOT_PROTOCOL_DEVICE_NAME_LEN];
	char *user_token;

	char protocol_type[DRIVER_MAX_PROTOCOL_TYPE_LEN];
	int endianness_type;
	char name_type[DRIVER_MAX_NAME_TYPE_LEN];
	int driver_id;
	char *rabbitmq_url;
	struct device_settings conf_files;
	char geral_url[DRIVER_MAX_URL_LEN];
	struct l_hashmap *data_items;

	struct l_timeout *msg_to;
};