/**
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2022, CESAR. All rights reserved.
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


#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <knot/knot_protocol.h>
#include <ell/ell.h>
#include <knot/knot_types.h>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/log_stdout.h>

#include "conf-device.h"
#include "iface-opc-ua.h"

#define RECONNECT_TIMEOUT_IN_SECONDS 5

#define STRING_EMPTY_LEN 0

#define NODEIDTYPE_NUMERIC		"NUMERIC"
#define NODEIDTYPE_STRING		"STRING"
#define NODEIDTYPE_GUID			"GUID"
#define NODEIDTYPE_BYTESTRING		"BYTESTRING"

#define ENCPT_URL_BASIC128RSA15		"http://opcfoundation.org/UA/SecurityPolicy#Basic128Rsa15"
#define ENCPT_URL_BASIC256		"http://opcfoundation.org/UA/SecurityPolicy#Basic256"
#define ENCPT_URL_NONE			"http://opcfoundation.org/UA/SecurityPolicy#None"
#define ENCPT_URL_BASIC256SHA256	"http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"

static struct l_timeout *connect_to;
static struct l_io *opc_ua_io;
static iface_opc_ua_connected_cb_t conn_cb;
static iface_opc_ua_disconnected_cb_t disconn_cb;
struct knot_thing thing_opc_ua;
static UA_Client *opc_ua_client;

union opc_ua_types {
	float val_float;
	uint8_t val_bool;
	int8_t val_i8;
	uint8_t val_u8;
	int16_t val_i16;
	uint16_t val_u16;
	int32_t val_i32;
	uint32_t val_u32;
	int64_t val_i64;
	uint64_t val_u64;
};

static UA_ByteString load_cert_file(const char *const path)
{
	UA_ByteString fileContents = UA_STRING_NULL;
	size_t read;

	FILE *fp = fopen(path, "rb");

	if (!fp)
		return fileContents;

	fseek(fp, 0, SEEK_END);
	fileContents.length = (size_t)ftell(fp);
	fileContents.data =
		(UA_Byte *)UA_malloc(fileContents.length * sizeof(UA_Byte));
	if (fileContents.data) {
		fseek(fp, 0, SEEK_SET);
		read = fread(fileContents.data, sizeof(UA_Byte),
			     fileContents.length, fp);
	if (read != fileContents.length)
		UA_ByteString_clear(&fileContents);
	} else {
		fileContents.length = STRING_EMPTY_LEN;
	}
	fclose(fp);

	return fileContents;
}

static void get_node_id(UA_NodeId *parent, int namespace_index,
			      char *identifier_type,
			      char *identifier)
{
	UA_NodeId_init(parent);

	if (!strcmp(identifier_type, NODEIDTYPE_STRING)) {
		*parent = UA_NODEID_STRING(namespace_index, identifier);
	} else if (!strcmp(identifier_type, NODEIDTYPE_NUMERIC)) {
		char *ptr;
		uint32_t num;

		num = (uint32_t)strtol(identifier, &ptr, 10);
		*parent = UA_NODEID_NUMERIC(namespace_index, num);
	} else if (!strcmp(identifier_type, NODEIDTYPE_GUID)) {
		UA_Guid *guid_aux = UA_Guid_new();
		UA_String string_aux = UA_STRING(identifier);

		UA_Guid_parse(guid_aux, string_aux);
		*parent = UA_NODEID_GUID(namespace_index, *guid_aux);
	} else if (!strcmp(identifier_type, NODEIDTYPE_BYTESTRING)) {
		*parent = UA_NODEID_BYTESTRING(namespace_index, identifier);
	} else {
		UA_NodeId_isNull(parent);
	}
}

static UA_StatusCode setup_read_data(UA_Variant *value,
				     struct knot_data_item *data_item)
{
	UA_StatusCode rc = UA_STATUSCODE_BAD;
	UA_NodeId *parent = UA_NodeId_new();

	UA_Variant_init(value);
	UA_NodeId_init(parent);

	get_node_id(parent, data_item->namespace,
			 data_item->identifier_type,
			 data_item->identifier);
	rc = UA_Client_readValueAttribute(opc_ua_client,
						  *parent, value);

	free(parent);
	return rc;
}

static UA_StatusCode get_type_int(union opc_ua_types *tmp,
				  void * data,
				  int value_type_size)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	if (value_type_size == 8)
		tmp->val_i8 = *(UA_Byte *)data;
	else if (value_type_size == 16)
		tmp->val_i16 = *(UA_Int16 *)data;
	else if (value_type_size == 32)
		tmp->val_i32 = *(UA_Int32 *)data;
	else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode get_type_uint(union opc_ua_types *tmp,
				   void * data,
				   int value_type_size)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	if (value_type_size == 8)
		tmp->val_u8 = *(UA_SByte *)data;
	if (value_type_size == 16)
		tmp->val_u16 = *(UA_UInt16 *)data;
	else if (value_type_size == 32)
		tmp->val_u32 = *(UA_UInt32 *)data;
	else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode get_type_float(union opc_ua_types *tmp,
				    void * data,
				    int value_type_size)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	if (value_type_size == 1)
		tmp->val_bool = *(UA_Boolean *)data;
	else if (value_type_size == 8)
		tmp->val_bool = *(UA_SByte *)data;
	else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode get_read_value(UA_Variant *value,
				    uint8_t value_type,
				    int value_type_size,
				    union opc_ua_types *tmp)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	switch (value_type) {
	case KNOT_VALUE_TYPE_BOOL:
		rc = get_type_float(tmp, value->data, value_type_size);
		break;
	case KNOT_VALUE_TYPE_FLOAT:
		tmp->val_float = *(UA_Float *)value->data;
		break;
	case KNOT_VALUE_TYPE_INT:
		rc = get_type_int(tmp, value->data, value_type_size);
		break;
	case KNOT_VALUE_TYPE_UINT:
		rc = get_type_uint(tmp, value->data, value_type_size);
		break;
	case KNOT_VALUE_TYPE_INT64:
		tmp->val_i64 = *(UA_Int64 *)value->data;
		break;
	case KNOT_VALUE_TYPE_UINT64:
		tmp->val_u64 = *(UA_UInt64 *)value->data;
		break;
	default:
		rc = UA_STATUSCODE_BAD;
	}

	return rc;
}

int iface_opc_ua_config(struct knot_data_item *data_item,
			       struct knot_thing *thing)
{
	return 0;
}

int iface_opc_ua_read_data(struct knot_data_item *data_item)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;
	int rental = UA_STATUSCODE_GOOD;
	union opc_ua_types tmp;
	UA_Variant *value = UA_Variant_new();

	rc = setup_read_data(value, data_item);
	memset(&tmp, 0, sizeof(tmp));

	if (rc == UA_STATUSCODE_GOOD) {
		rc = get_read_value(value, data_item->schema.value_type,
				    data_item->value_type_size, &tmp);
	}

	if (rc & UA_STATUSCODE_BAD) {
		l_error("Failed to read from OPC UA: %#010x",
			rc);
		l_io_destroy(opc_ua_io);
		opc_ua_io = NULL;
		rental = -EINTR;
	} else if (rc & UA_STATUSCODE_UNCERTAIN) {
		l_error("Failed to read from OPC UA for Data_%d: %#010x",
			data_item->sensor_id, rc);
		rental = -ENXIO;
	} else {
		memcpy(&data_item->current_val, &tmp, sizeof(tmp));
	}

	UA_Variant_delete(value);
	return rental;
}

static void on_disconnected(struct l_io *io, void *user_data)
{
	if (disconn_cb)
		disconn_cb(user_data);

	if (connect_to)
		l_timeout_modify(connect_to, RECONNECT_TIMEOUT_IN_SECONDS);
}

static void attempt_connect(struct l_timeout *to, void *user_data)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;
	bool is_authenticated_connection = false;

	l_debug("Trying to connect to OPC UA Server");

	/* Check and destroy if an IO is already allocated */
	if (opc_ua_io) {
		l_io_destroy(opc_ua_io);
		opc_ua_io = NULL;
	}

	is_authenticated_connection =
		((strlen(thing_opc_ua.driver_login) != STRING_EMPTY_LEN) &&
		strlen(thing_opc_ua.driver_password) != STRING_EMPTY_LEN)
		? true : false;

	if (is_authenticated_connection) {
		rc = UA_Client_connectUsername(opc_ua_client,
					       thing_opc_ua.geral_url,
					       thing_opc_ua.driver_login,
					       thing_opc_ua.driver_password);
	} else {
		rc = UA_Client_connect(opc_ua_client,
				       thing_opc_ua.geral_url);
	}

	if (rc != UA_STATUSCODE_GOOD) {
		l_error("Error connecting to OPC UA Server: %#010x",
			rc);
		goto retry;
	}

	opc_ua_io = l_io_new(rc);
	if (!opc_ua_io) {
		l_error("Error setting up io state");
		goto connection_close;
	}

	if (!l_io_set_disconnect_handler(opc_ua_io, on_disconnected, user_data,
					NULL)) {
		l_error("Couldn't set OPC UA disconnect handler");
		goto io_destroy;
	}

	if (conn_cb)
		conn_cb(user_data);

	return;

io_destroy:
	l_io_destroy(opc_ua_io);
	opc_ua_io = NULL;
connection_close:
	UA_Client_disconnect(opc_ua_client);
retry:
	l_timeout_modify(to, RECONNECT_TIMEOUT_IN_SECONDS);
}

static UA_StatusCode set_security_mode_config(UA_ClientConfig *config)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;
	if (thing_opc_ua.driver_security_mode <= UA_MESSAGESECURITYMODE_NONE &&
	    thing_opc_ua.driver_security_mode >=
	    UA_MESSAGESECURITYMODE_SIGNANDENCRYPT)
		config->securityMode = thing_opc_ua.driver_security_mode;
	else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode set_security_policy_config(UA_ClientConfig *config)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	if (strlen(thing_opc_ua.driver_security_policy) != STRING_EMPTY_LEN) {
		char policy_axi[1028] = {};
		sprintf(policy_axi,
			"http://opcfoundation.org/UA/SecurityPolicy#%s",
			thing_opc_ua.driver_security_policy);
		config->securityPolicyUri =
			UA_STRING_ALLOC(policy_axi);
	} else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode set_security_uri_config(UA_ClientConfig *config)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	if (strlen(thing_opc_ua.driver_security_policy)
	    != STRING_EMPTY_LEN) {
		char uri_axi[256] = {};
		sprintf(uri_axi, "urn:%s:KNoTVirtualThing",
			thing_opc_ua.name);
		config-> clientDescription.applicationUri =
			UA_STRING_ALLOC(uri_axi);
	} else
		rc = UA_STATUSCODE_BAD;

	return rc;
}

static UA_StatusCode set_client_config(UA_Client *client)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;
	UA_ClientConfig *config_client = UA_Client_getConfig(client);

	if (thing_opc_ua.driver_security == true) {
		UA_ByteString certificate =
			load_cert_file(thing_opc_ua.driver_path_certificate);
		UA_ByteString privateKey  =
			load_cert_file(thing_opc_ua.driver_path_private_key);
		rc = UA_ClientConfig_setDefaultEncryption(config_client,
						     certificate, privateKey,
						     NULL, 0, NULL, 0);
		UA_ByteString_clear(&certificate);
		UA_ByteString_clear(&privateKey);
	} else {
		rc = UA_ClientConfig_setDefault(config_client);
	}

	rc = set_security_mode_config(config_client);
	rc = set_security_policy_config(config_client);
	rc = set_security_uri_config(config_client);

	return rc;
}

int iface_opc_ua_start(struct knot_thing thing,
		       iface_opc_ua_connected_cb_t connected_cb,
		       iface_opc_ua_disconnected_cb_t disconnected_cb,
		       void *user_data)
{
	UA_StatusCode rc = UA_STATUSCODE_GOOD;

	thing_opc_ua = thing;

	opc_ua_client = UA_Client_new();
	rc = set_client_config(opc_ua_client);
	if (rc != UA_STATUSCODE_GOOD)
		return -(int)rc;

	conn_cb = connected_cb;
	disconn_cb = disconnected_cb;

	connect_to = l_timeout_create_ms(1, attempt_connect,
					 NULL, NULL);

	return 0;
}

void iface_opc_ua_stop(void)
{
	l_timeout_remove(connect_to);
	connect_to = NULL;

	l_io_destroy(opc_ua_io);
	opc_ua_io = NULL;

	UA_Client_disconnect(opc_ua_client);
	UA_Client_delete(opc_ua_client);
}
